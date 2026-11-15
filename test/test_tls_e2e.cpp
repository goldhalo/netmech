// ============================================================================
//  test_tls_e2e.cpp —— 进程内 TLS 端到端（自签 + 指纹 pin）
//
//  只在 `NETMECH_TLS=ON` 的构建里真正执行；关掉 TLS 时退化为一条空用例，
//  保证"同一份测试源码在任何构建下都能编过"。
//
//  覆盖：
//    1. 自签证书 + 正确指纹 → 握手成功、on_accept/on_linkup 在**握手之后**
//       才触发、TLS 通道上的回环数据正确；
//    2. 指纹不匹配 → 客户端 on_linkfail = tls_cert_rejected，
//       且**不会**有 on_linkup（业务拿不到半成品连接）；
//    3. 明文客户端打 TLS 端口 → 服务端 on_accept_failed = tls_handshake_timeout，
//       且不会有 on_accept。
//
//  证书是**运行时生成**的（EC P-256 自签），不往仓库里塞 PEM，
//  也不需要装 openssl 命令行。
// ============================================================================
#include "test_support.hpp"

#if defined(NETMECH_HAS_TLS)

#    include <openssl/evp.h>
#    include <openssl/pem.h>
#    include <openssl/x509.h>

#    include <cstdio>
#    include <filesystem>
#    include <mutex>
#    include <string>
#    include <utility>
#    include <vector>

namespace {

/// 判定 error_code 是否属于某个 NetErrc。
bool is_errc(const std::error_code& ec, netmech::NetErrc want)
{
    const auto got = netmech::from_boost_error_code(ec);
    return got.has_value() && *got == want;
}

std::string temp_path(const char* suffix)
{
    static std::atomic<unsigned> seq{0};
    const auto dir = std::filesystem::temp_directory_path();
    const std::string name =
        std::string("netmech_tls_") + std::to_string(seq.fetch_add(1)) + suffix;
    return (dir / name).string();
}

/// 运行时生成的自签证书（EC P-256），析构时删除临时文件。
struct TestCert {
    std::string cert_path;
    std::string key_path;
    std::string fingerprint;   ///< 小写 hex，无分隔符
    bool        ok = false;

    TestCert() = default;
    TestCert(const TestCert&) = delete;
    TestCert& operator=(const TestCert&) = delete;

    /// 移动构造：把临时文件的所有权转移过去（源对象清空，析构时不再删文件）。
    TestCert(TestCert&& other) noexcept
        : cert_path(std::move(other.cert_path))
        , key_path(std::move(other.key_path))
        , fingerprint(std::move(other.fingerprint))
        , ok(other.ok)
    {
        other.cert_path.clear();
        other.key_path.clear();
        other.ok = false;
    }

    ~TestCert()
    {
        if (!cert_path.empty()) {
            std::remove(cert_path.c_str());
        }
        if (!key_path.empty()) {
            std::remove(key_path.c_str());
        }
    }

    /// 把最后一位十六进制字符翻转，得到一个"格式合法但内容不对"的指纹。
    std::string wrong_fingerprint() const
    {
        std::string bad = fingerprint;
        if (!bad.empty()) {
            bad.back() = (bad.back() == 'f') ? '0' : 'f';
        }
        return bad;
    }
};

TestCert make_self_signed()
{
    TestCert out;

    // ---- 1) 生成 EC P-256 私钥 ----
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (pctx == nullptr) {
        return out;
    }
    EVP_PKEY* pkey = nullptr;
    bool ok = (EVP_PKEY_keygen_init(pctx) == 1);
    ok = ok && (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) == 1);
    ok = ok && (EVP_PKEY_keygen(pctx, &pkey) == 1);
    EVP_PKEY_CTX_free(pctx);
    if (!ok || pkey == nullptr) {
        if (pkey != nullptr) {
            EVP_PKEY_free(pkey);
        }
        return out;
    }

    // ---- 2) 自签 X509 ----
    X509* x = X509_new();
    if (x == nullptr) {
        EVP_PKEY_free(pkey);
        return out;
    }

    bool cert_ok = (X509_set_version(x, 2) == 1);
    cert_ok = cert_ok && (ASN1_INTEGER_set(X509_get_serialNumber(x), 1) == 1);
    cert_ok = cert_ok && (X509_gmtime_adj(X509_getm_notBefore(x), -60) != nullptr);
    cert_ok = cert_ok && (X509_gmtime_adj(X509_getm_notAfter(x), 3600) != nullptr);
    cert_ok = cert_ok && (X509_set_pubkey(x, pkey) == 1);

    X509_NAME* name = X509_get_subject_name(x);
    cert_ok = cert_ok && (name != nullptr);
    if (cert_ok) {
        cert_ok = (X509_NAME_add_entry_by_txt(
                       name, "CN", MBSTRING_ASC,
                       reinterpret_cast<const unsigned char*>("netmech-test"),
                       -1, -1, 0) == 1);
        cert_ok = cert_ok && (X509_set_issuer_name(x, name) == 1);
    }
    cert_ok = cert_ok && (X509_sign(x, pkey, EVP_sha256()) > 0);

    // ---- 3) 指纹（SHA-256）----
    if (cert_ok) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int  len = 0;
        cert_ok = (X509_digest(x, EVP_sha256(), digest, &len) == 1);
        if (cert_ok) {
            static const char kHex[] = "0123456789abcdef";
            out.fingerprint.reserve(static_cast<std::size_t>(len) * 2);
            for (unsigned int i = 0; i < len; ++i) {
                out.fingerprint.push_back(kHex[(digest[i] >> 4) & 0x0F]);
                out.fingerprint.push_back(kHex[digest[i] & 0x0F]);
            }
        }
    }

    // ---- 4) 写 PEM ----
    //
    // ★ 必须用 BIO_* 而不是 PEM_write_X509(FILE*, ...)：
    //   Windows 上 OpenSSL 是独立 DLL，FILE* 属于调用方的 CRT。把 FILE* 传给
    //   OpenSSL 会触发 `OPENSSL_Uplink: no OPENSSL_Applink` 直接 abort
    //   （除非应用另外编进 OpenSSL 的 applink.c）。BIO_new_file 走 OpenSSL
    //   自己的 IO，两个平台都干净。
    if (cert_ok) {
        out.cert_path = temp_path(".crt.pem");
        out.key_path  = temp_path(".key.pem");

        BIO* bc = BIO_new_file(out.cert_path.c_str(), "wb");
        if (bc == nullptr) {
            cert_ok = false;
        } else {
            cert_ok = (PEM_write_bio_X509(bc, x) == 1);
            BIO_free(bc);
        }

        BIO* bk = BIO_new_file(out.key_path.c_str(), "wb");
        if (bk == nullptr) {
            cert_ok = false;
        } else {
            cert_ok = cert_ok
                      && (PEM_write_bio_PrivateKey(bk, pkey, nullptr, nullptr, 0, nullptr,
                                                   nullptr) == 1);
            BIO_free(bk);
        }
    }

    X509_free(x);
    EVP_PKEY_free(pkey);
    out.ok = cert_ok;
    return out;
}

/// 记录各类事件的订阅者（自带回环业务）。
struct TlsTestLinkable : public netmech::Linkable {
    std::shared_ptr<netmech::ConnHandler> on_accept(netmech::Conn& c) override
    {
        remember(c);
        accepted.fetch_add(1);
        return std::make_shared<netmech_test::EchoIoHandler>(&rx);
    }

    std::shared_ptr<netmech::ConnHandler> on_linkup(netmech::Conn& c) override
    {
        remember(c);
        linkup.fetch_add(1);
        return std::make_shared<netmech_test::EchoIoHandler>(&rx);
    }

    void on_linkdown(netmech::Conn&) override { linkdown.fetch_add(1); }

    void on_linkfail(const netmech::Address&, std::error_code ec) override
    {
        {
            std::lock_guard<std::mutex> lk(mu);
            last_linkfail = ec;
        }
        linkfail.fetch_add(1);
    }

    void on_accept_failed(const netmech::Address&, std::error_code ec) override
    {
        {
            std::lock_guard<std::mutex> lk(mu);
            last_accept_failed = ec;
        }
        accept_failed.fetch_add(1);
    }

    std::shared_ptr<netmech::Conn> first_endpoint()
    {
        std::lock_guard<std::mutex> lk(mu);
        return eps.empty() ? std::shared_ptr<netmech::Conn>() : eps.front();
    }

    std::error_code linkfail_error()
    {
        std::lock_guard<std::mutex> lk(mu);
        return last_linkfail;
    }

    std::error_code accept_failed_error()
    {
        std::lock_guard<std::mutex> lk(mu);
        return last_accept_failed;
    }

    std::atomic<int>           accepted{0};
    std::atomic<int>           accept_failed{0};
    std::atomic<int>           linkup{0};
    std::atomic<int>           linkdown{0};
    std::atomic<int>           linkfail{0};
    std::atomic<std::uint64_t> rx{0};   ///< 收到的（回环回来的）字节数

private:
    void remember(netmech::Conn& c)
    {
        std::lock_guard<std::mutex> lk(mu);
        eps.push_back(c.shared_from_this());
    }

    std::mutex                                 mu;
    std::vector<std::shared_ptr<netmech::Conn>> eps;
    std::error_code                            last_linkfail;
    std::error_code                            last_accept_failed;
};

} // namespace

// ---------------------------------------------------------------------------
// 1. 自签 + 正确指纹：握手成功、回环数据正确
// ---------------------------------------------------------------------------
NETMECH_TEST(tls_e2e_handshake_and_echo)
{
    TestCert cert = make_self_signed();
    REQUIRE(cert.ok);

    auto server = netmech::Mechanism::create();
    auto client = netmech::Mechanism::create();
    REQUIRE(server != nullptr);
    REQUIRE(client != nullptr);

    auto srv_lb = std::make_shared<TlsTestLinkable>();
    auto cli_lb = std::make_shared<TlsTestLinkable>();

    netmech::TlsConfig srv_tls;
    srv_tls.cert_chain_file = cert.cert_path;
    srv_tls.private_key_file = cert.key_path;

    netmech::ListenOptions lo;
    lo.tls = srv_tls;

    const netmech::Address req{"127.0.0.1", 0};
    EXPECT_OK(server->listen(req, srv_lb, lo));

    const auto bound = server->listening_address(req);
    REQUIRE(bound.has_value());

    netmech::TlsConfig cli_tls;
    cli_tls.pinned_sha256 = cert.fingerprint;
    cli_tls.server_name = "netmech-test";

    netmech::ConnectOptions co;
    co.tls = cli_tls;

    EXPECT_OK(client->connect(*bound, cli_lb, co));

    // on_linkup 必须发生在**握手之后**：能到达这里就说明握手成功了。
    REQUIRE(netmech_test::wait_for(
        [&] { return cli_lb->linkup.load() >= 1 && srv_lb->accepted.load() >= 1; }));

    auto ep = cli_lb->first_endpoint();
    REQUIRE(ep != nullptr);
    CHECK(ep->is_open());

    const char payload[] = "hello-tls";
    const std::size_t payload_len = sizeof(payload) - 1;
    REQUIRE(ep->write({nullptr, payload, payload_len}));

    // 数据经过 TLS 加密通道到达服务端、被原样回写、再解密回到客户端。
    CHECK(netmech_test::wait_for([&] { return cli_lb->rx.load() >= payload_len; }));

    CHECK_EQ(cli_lb->linkfail.load(), 0);
    CHECK_EQ(srv_lb->accept_failed.load(), 0);

    client->close_all();
    server->close_all();
}

// ---------------------------------------------------------------------------
// 2. 指纹不匹配：客户端 on_linkfail = tls_cert_rejected，且没有 on_linkup
// ---------------------------------------------------------------------------
NETMECH_TEST(tls_e2e_wrong_pin_rejected)
{
    TestCert cert = make_self_signed();
    REQUIRE(cert.ok);

    auto server = netmech::Mechanism::create();
    auto client = netmech::Mechanism::create();
    REQUIRE(server != nullptr);
    REQUIRE(client != nullptr);

    auto srv_lb = std::make_shared<TlsTestLinkable>();
    auto cli_lb = std::make_shared<TlsTestLinkable>();

    netmech::TlsConfig srv_tls;
    srv_tls.cert_chain_file = cert.cert_path;
    srv_tls.private_key_file = cert.key_path;

    netmech::ListenOptions lo;
    lo.tls = srv_tls;

    const netmech::Address req{"127.0.0.1", 0};
    EXPECT_OK(server->listen(req, srv_lb, lo));
    const auto bound = server->listening_address(req);
    REQUIRE(bound.has_value());

    netmech::TlsConfig cli_tls;
    cli_tls.pinned_sha256 = cert.wrong_fingerprint();

    netmech::ConnectOptions co;
    co.tls = cli_tls;

    EXPECT_OK(client->connect(*bound, cli_lb, co));

    REQUIRE(netmech_test::wait_for([&] { return cli_lb->linkfail.load() >= 1; }));
    CHECK_MSG(is_errc(cli_lb->linkfail_error(), netmech::NetErrc::tls_cert_rejected),
              "期望 tls_cert_rejected，实际: "
                  + cli_lb->linkfail_error().message());

    // 关键：业务**没有**拿到连接。
    CHECK_EQ(cli_lb->linkup.load(), 0);

    client->close_all();
    server->close_all();
}

// ---------------------------------------------------------------------------
// 3. 明文客户端打 TLS 端口：服务端 on_accept_failed = tls_handshake_timeout
// ---------------------------------------------------------------------------
NETMECH_TEST(tls_e2e_plain_peer_rejected)
{
    TestCert cert = make_self_signed();
    REQUIRE(cert.ok);

    auto server = netmech::Mechanism::create();
    auto client = netmech::Mechanism::create();
    REQUIRE(server != nullptr);
    REQUIRE(client != nullptr);

    auto srv_lb = std::make_shared<TlsTestLinkable>();
    auto cli_lb = std::make_shared<TlsTestLinkable>();

    netmech::TlsConfig srv_tls;
    srv_tls.cert_chain_file = cert.cert_path;
    srv_tls.private_key_file = cert.key_path;
    srv_tls.handshake_timeout_ms = 300;   // 缩短等待：用例要快

    netmech::ListenOptions lo;
    lo.tls = srv_tls;

    const netmech::Address req{"127.0.0.1", 0};
    EXPECT_OK(server->listen(req, srv_lb, lo));
    const auto bound = server->listening_address(req);
    REQUIRE(bound.has_value());

    // 明文客户端（不带 tls）：TCP 能连上，但它不会说 TLS。
    EXPECT_OK(client->connect(*bound, cli_lb));

    REQUIRE(netmech_test::wait_for([&] { return srv_lb->accept_failed.load() >= 1; }));
    CHECK_MSG(is_errc(srv_lb->accept_failed_error(), netmech::NetErrc::tls_handshake_timeout),
              "期望 tls_handshake_timeout，实际: "
                  + srv_lb->accept_failed_error().message());

    // 握手没完成 → 服务端业务从未拿到连接。
    CHECK_EQ(srv_lb->accepted.load(), 0);

    client->close_all();
    server->close_all();
}

#else // NETMECH_HAS_TLS

NETMECH_TEST(tls_e2e_requires_tls_build)
{
    // 本构建没有 TLS：端到端用例不适用（配置校验用例在 test_tls.cpp）。
    CHECK(true);
}

#endif // NETMECH_HAS_TLS
