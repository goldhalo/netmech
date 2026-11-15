// ============================================================================
//  test_tls.cpp —— TLS 配置校验（v0.91）
//
//  本文件只覆盖**不需要真正握手**的部分，因此：
//    * `NETMECH_TLS=OFF` 的构建照样编、照样跑；
//    * 不需要证书文件、不需要 OpenSSL 运行时。
//
//  覆盖的三类语义：
//    1. 配置错误必须在 listen()/connect() **同步暴露**（invalid_argument），
//       而不是等第一条连接来了才失败 —— 否则现场表现是"服务端静默不接连接"；
//    2. 客户端"既不 pin 也不校验证书链"必须被拒绝（假 TLS）；
//    3. 未编译 TLS 的构建必须返回 tls_unsupported，而不是静默降级成明文。
//
//  端到端握手用例（自签证书 + 指纹 pin）需要证书材料，见
//  docs/TLS.md 的"手工验证"一节。
// ============================================================================
#include <netmech/common.hpp>
#include <netmech/linkable.hpp>
#include <netmech/mechanism.hpp>
#include <netmech/tls.hpp>

#include <memory>
#include <string>
#include <system_error>

#include "test_framework.hpp"

using namespace netmech;

namespace {

/// 判定 error_code 是否属于某个 NetErrc。
bool is_errc(const std::error_code& ec, NetErrc want)
{
    const auto got = from_boost_error_code(ec);
    return got.has_value() && *got == want;
}

/// 服务端"看起来合法"的配置（文件不需要真实存在 —— 校验只做字段检查）。
TlsConfig server_config()
{
    TlsConfig cfg;
    cfg.cert_chain_file = "server.crt";
    cfg.private_key_file = "server.key";
    return cfg;
}

/// 客户端"看起来合法"的配置（自签 + 指纹 pin）。
///
/// ⚠ `[[maybe_unused]]` 不能去掉：它只在 `NETMECH_TLS=OFF` 的构建里被用到
///   （断言 tls_unsupported）。TLS 打开的构建里那段分支被 `#if` 编掉，本函数
///   就成了"定义了但没人用" —— GCC 报 `-Wunused-function`、MSVC /W4 报 C4505，
///   配上 `-DNETMECH_WARNINGS_AS_ERRORS=ON` 直接构建失败（CI 上真实踩过）。
[[maybe_unused]] TlsConfig client_config()
{
    TlsConfig cfg;
    cfg.pinned_sha256 =
        "00112233445566778899aabbccddeeff"
        "00112233445566778899aabbccddeeff";
    return cfg;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. 服务端：缺证书/私钥 → invalid_argument，且不留下半个监听器
// ---------------------------------------------------------------------------
NETMECH_TEST(tls_listen_rejects_missing_cert)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto lb = std::make_shared<Linkable>();

    ListenOptions opts;
    opts.tls = TlsConfig{};   // 空配置：既没证书也没私钥

    const std::error_code ec = mech->listen(Address{"127.0.0.1", 0}, lb, opts);
    CHECK_MSG(is_errc(ec, NetErrc::invalid_argument),
              "期望 invalid_argument，实际: " + ec.message());

    // 关键：失败必须是"什么都没发生"，不能占着监听表里的槽位。
    CHECK(mech->listening_addresses().empty());
    CHECK_EQ(mech->endpoint_count(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// 2. 服务端：指纹格式非法 → invalid_argument（在打开证书文件之前就拒掉）
// ---------------------------------------------------------------------------
NETMECH_TEST(tls_listen_rejects_bad_fingerprint)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto lb = std::make_shared<Linkable>();

    TlsConfig cfg = server_config();
    cfg.pinned_sha256 = "abc";   // 不是 64 个十六进制字符

    ListenOptions opts;
    opts.tls = cfg;

    const std::error_code ec = mech->listen(Address{"127.0.0.1", 0}, lb, opts);
    CHECK_MSG(is_errc(ec, NetErrc::invalid_argument),
              "期望 invalid_argument，实际: " + ec.message());
    CHECK(mech->listening_addresses().empty());
}

// ---------------------------------------------------------------------------
// 3. 客户端：既不 pin 也不校验证书链 → 拒绝（否则就是"假 TLS"）
// ---------------------------------------------------------------------------
NETMECH_TEST(tls_connect_rejects_insecure_config)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto lb = std::make_shared<Linkable>();

    TlsConfig cfg;
    cfg.server_name = "device.local";   // 只给 SNI，不给信任锚

    ConnectOptions opts;
    opts.tls = cfg;

    const std::error_code ec =
        mech->connect(Address{"127.0.0.1", 1}, lb, opts);
    CHECK_MSG(is_errc(ec, NetErrc::invalid_argument),
              "期望 invalid_argument，实际: " + ec.message());
}

// ---------------------------------------------------------------------------
// 4. 客户端：指纹格式非法 → invalid_argument
// ---------------------------------------------------------------------------
NETMECH_TEST(tls_connect_rejects_bad_fingerprint)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto lb = std::make_shared<Linkable>();

    TlsConfig cfg;
    cfg.pinned_sha256 = "0011";   // 长度不足

    ConnectOptions opts;
    opts.tls = cfg;

    const std::error_code ec =
        mech->connect(Address{"127.0.0.1", 1}, lb, opts);
    CHECK_MSG(is_errc(ec, NetErrc::invalid_argument),
              "期望 invalid_argument，实际: " + ec.message());
}

// ---------------------------------------------------------------------------
// 5. 未编译 TLS 的构建：合法配置也必须明确报"不支持"
// ---------------------------------------------------------------------------
NETMECH_TEST(tls_reports_unsupported_when_not_built)
{
#if defined(NETMECH_HAS_TLS)
    // 本构建带 TLS：这一条不适用（用例仍然注册，便于对比数量）。
    CHECK(true);
#else
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto lb = std::make_shared<Linkable>();

    {
        ListenOptions opts;
        opts.tls = server_config();
        const std::error_code ec = mech->listen(Address{"127.0.0.1", 0}, lb, opts);
        CHECK_MSG(is_errc(ec, NetErrc::tls_unsupported),
                  "期望 tls_unsupported，实际: " + ec.message());
    }
    {
        ConnectOptions opts;
        opts.tls = client_config();
        const std::error_code ec = mech->connect(Address{"127.0.0.1", 1}, lb, opts);
        CHECK_MSG(is_errc(ec, NetErrc::tls_unsupported),
                  "期望 tls_unsupported，实际: " + ec.message());
    }

    // 关键：失败的 TLS 受理不能污染监听表。
    CHECK(mech->listening_addresses().empty());
#endif
}

// ---------------------------------------------------------------------------
// 6. TLS 构建下：证书文件打不开时返回底层错误（不是 invalid_argument）
// ---------------------------------------------------------------------------
NETMECH_TEST(tls_listen_reports_missing_cert_file)
{
#if defined(NETMECH_HAS_TLS)
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto lb = std::make_shared<Linkable>();

    TlsConfig cfg;
    cfg.cert_chain_file = "netmech_no_such_cert.pem";
    cfg.private_key_file = "netmech_no_such_key.pem";

    ListenOptions opts;
    opts.tls = cfg;

    const std::error_code ec = mech->listen(Address{"127.0.0.1", 0}, lb, opts);
    CHECK_MSG(static_cast<bool>(ec), "证书文件不存在时必须失败");
    CHECK_MSG(!is_errc(ec, NetErrc::invalid_argument),
              "文件打不开不是参数非法（应原样透传底层错误）");
    CHECK(mech->listening_addresses().empty());
#else
    CHECK(true);
#endif
}

// ---------------------------------------------------------------------------
// 7. TLS 配置失败之后，同一个 Mechanism 的明文 listen 仍然正常
//    （回归：早期版本里失败路径可能把监听表的占位留下来）
// ---------------------------------------------------------------------------
NETMECH_TEST(tls_failure_does_not_break_plain_listen)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto lb = std::make_shared<Linkable>();

    ListenOptions bad;
    bad.tls = TlsConfig{};
    const std::error_code bad_ec = mech->listen(Address{"127.0.0.1", 0}, lb, bad);
    CHECK(static_cast<bool>(bad_ec));

    const Address req{"127.0.0.1", 0};
    const std::error_code ok_ec = mech->listen(req, lb, 8);
    EXPECT_OK(ok_ec);
    CHECK(mech->listening_address(req).has_value());
}
