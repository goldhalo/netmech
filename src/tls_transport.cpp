// =============================================================================
//  netmech - TLS 传输实现（OpenSSL）
// =============================================================================
//  文件    : src/tls_transport.cpp
//  用途    : TlsContext（SSL_CTX + 校验策略）与 TlsTransport 的实现。
//  依赖    : 独立 Asio（ssl）+ OpenSSL。`NETMECH_TLS=OFF` 时整个文件为空。
//  编码    : UTF-8
//
//  ★ 与明文实现的差异（同样的 Transport 契约，见 transport.hpp）
//  ---------------------------------------------------------------
//  1. **需要握手**：`needs_handshake() == true`，Conn 会先握手再交棒业务；
//  2. **错误映射**：对端不发 close_notify 直接断开时，asio 报
//     `asio::ssl::error::stream_truncated`。这里把它翻译成本库的
//     `NetErrc::tls_truncated` —— 否则业务看到的是一个普通的 eof，
//     **截断攻击就变成静默的数据缺失**（TLS 头号语义坑）；
//  3. **关闭是尽力而为**：`close(graceful)` 里做一次**非阻塞**的
//     `SSL_shutdown`（只发 close_notify，不等对端回应），然后照常
//     shutdown + close。库的 close() 契约是"同步、noexcept、不阻塞"，
//     不能为了等对端而挂住 io 线程 —— 代价是对端可能看到 truncation。
//  4. **校验在握手之后**：pin / 主机名比对不放进 verify_callback
//     （verify_none 下回调根本不会被调用），而是在握手完成后统一做；
//     校验失败时业务还没拿到 Conn，因此不存在"先交付数据再发现证书不对"。
// =============================================================================
#include "netmech_srcpch.hpp"

#if defined(NETMECH_HAS_TLS)

#    include "tls_internal.hpp"

#    include <openssl/evp.h>
#    include <openssl/x509v3.h>

#    include <algorithm>
#    include <cctype>
#    include <cstring>
#    include <utility>

namespace netmech {

namespace {

/// 把 TLS 特有的错误翻译成本库语义。
error_code_t map_ssl_error(const error_code_t& ec)
{
    if (!ec) {
        return ec;
    }
    if (ec == asio::ssl::error::stream_truncated) {
        // 对端没有发 close_notify 就关掉了 TCP：数据可能被截断。
        return make_boost_error_code(NetErrc::tls_truncated);
    }
    return ec;
}

/// 取对端叶子证书（**调用方负责 X509_free**）。
X509* get_peer_cert(SSL* ssl) noexcept
{
    if (!ssl) {
        return nullptr;
    }
#    if OPENSSL_VERSION_NUMBER >= 0x30000000L
    return SSL_get1_peer_certificate(ssl);
#    else
    // 1.1.1 的 SSL_get_peer_certificate 同样返回一份引用，需要 X509_free。
    return SSL_get_peer_certificate(ssl);
#    endif
}

} // namespace

// =============================================================================
// TlsContext
// =============================================================================

std::shared_ptr<TlsContext> TlsContext::create(const TlsConfig& cfg,
                                               bool server,
                                               error_code_t& ec)
{
    ec.clear();
    std::shared_ptr<TlsContext> self(new TlsContext(cfg, server));
    asio::ssl::context& c = self->ctx_;

    // default_workarounds：屏蔽几个历史遗留的 OpenSSL 兼容性开关
    // （对现代对端没有影响，但能让行为在不同 OpenSSL 版本上更一致）。
    c.set_options(asio::ssl::context::default_workarounds, ec);
    if (ec) {
        return std::shared_ptr<TlsContext>();
    }

    SSL_CTX* raw = c.native_handle();

    // 协议下限：SSLv3 / TLS1.0 / TLS1.1 在本库中**无法启用**。
    int min_version = TLS1_2_VERSION;
    if (cfg.min_version == TlsVersion::tls13) {
#    if defined(TLS1_3_VERSION)
        min_version = TLS1_3_VERSION;
#    else
        // 老 OpenSSL（< 1.1.1）没有 TLS 1.3：明确报"不支持"，
        // 而不是静默降到 1.2 让调用方以为开了 1.3。
        ec = make_boost_error_code(NetErrc::tls_unsupported);
        return std::shared_ptr<TlsContext>();
#    endif
    }
    if (SSL_CTX_set_min_proto_version(raw, min_version) != 1) {
        ec = make_boost_error_code(NetErrc::tls_unsupported);
        return std::shared_ptr<TlsContext>();
    }

    // ---- ALPN ----
    // 线上格式是 `[len][name][len][name]...`（每项 1..255 字节，已在
    // validate_tls_config 里校验）。服务端注册选择回调，客户端设置候选列表。
    if (!cfg.alpn.empty()) {
        auto build_wire = [](const std::vector<std::string>& protos,
                             std::vector<unsigned char>& out) {
            out.clear();
            for (const std::string& p : protos) {
                out.push_back(static_cast<unsigned char>(p.size()));
                out.insert(out.end(), p.begin(), p.end());
            }
        };
        build_wire(cfg.alpn, self->alpn_server_wire_);
        build_wire(cfg.alpn, self->alpn_client_wire_);

        if (server) {
            // 回调的 arg 指向本 TlsContext：它被监听器与所有连接共享，
            // 生命周期覆盖整个 SSL_CTX，因此回调里的指针始终有效。
            SSL_CTX_set_alpn_select_cb(raw, &TlsContext::alpn_select_cb, self.get());
        } else if (self->alpn_client_wire_.size() <=
                   static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
            const int rc = SSL_CTX_set_alpn_protos(
                raw, self->alpn_client_wire_.data(),
                static_cast<unsigned int>(self->alpn_client_wire_.size()));
            if (rc != 0) {
                ec = make_boost_error_code(NetErrc::invalid_argument);
                return std::shared_ptr<TlsContext>();
            }
        }
    }

    // 服务端：设置 session id context，否则会话复用（含 TLS1.3 ticket）不可用。
    if (server) {
        static const unsigned char sid_ctx[] = {'n', 'e', 't', 'm', 'e', 'c', 'h'};
        SSL_CTX_set_session_id_context(raw, sid_ctx,
                                      static_cast<unsigned int>(sizeof(sid_ctx)));
    }

    if (server) {
        if (!cfg.private_key_password.empty()) {
            const std::string password = cfg.private_key_password;
            c.set_password_callback(
                [password](std::size_t /*max_length*/,
                           asio::ssl::context::password_purpose /*purpose*/) {
                    return password;
                },
                ec);
            if (ec) {
                return std::shared_ptr<TlsContext>();
            }
        }

        c.use_certificate_chain_file(cfg.cert_chain_file, ec);
        if (ec) {
            return std::shared_ptr<TlsContext>();
        }
        c.use_private_key_file(cfg.private_key_file, asio::ssl::context::pem, ec);
        if (ec) {
            return std::shared_ptr<TlsContext>();
        }
    }

    if (cfg.verify_certificate) {
        if (!cfg.ca_file.empty()) {
            c.load_verify_file(cfg.ca_file, ec);
            if (ec) {
                return std::shared_ptr<TlsContext>();
            }
        } else {
            // 空 = 用系统默认 CA 路径（Linux 的 /etc/ssl/certs 等）。
            c.set_default_verify_paths(ec);
            if (ec) {
                return std::shared_ptr<TlsContext>();
            }
        }

        int mode = asio::ssl::verify_peer;
        if (server) {
            // 服务端开链校验 = 要求客户端出示证书（mTLS），没证书直接失败。
            mode |= asio::ssl::verify_fail_if_no_peer_cert;
        }
        c.set_verify_mode(mode, ec);
        if (ec) {
            return std::shared_ptr<TlsContext>();
        }
    } else {
        // 自签场景：握手阶段不校验证书链，安全性由握手后的指纹 pin 保证。
        c.set_verify_mode(asio::ssl::verify_none, ec);
        if (ec) {
            return std::shared_ptr<TlsContext>();
        }
    }

    return self;
}

std::uint32_t TlsContext::handshake_timeout_ms() const noexcept
{
    return cfg_.handshake_timeout_ms == 0 ? 10000u : cfg_.handshake_timeout_ms;
}

std::string TlsContext::peer_alpn(SSL* ssl)
{
    if (ssl == nullptr) {
        return std::string();
    }
    const unsigned char* data = nullptr;
    unsigned int         len  = 0;
    ::SSL_get0_alpn_selected(ssl, &data, &len);
    if (data == nullptr || len == 0) {
        return std::string();
    }
    return std::string(reinterpret_cast<const char*>(data), len);
}

int TlsContext::alpn_select_cb(SSL* /*ssl*/, const unsigned char** out,
                               unsigned char* outlen, const unsigned char* in,
                               unsigned int inlen, void* arg)
{
    auto* self = static_cast<TlsContext*>(arg);
    if (self == nullptr || self->alpn_server_wire_.empty()) {
        return SSL_TLSEXT_ERR_NOACK;
    }

    // 按**服务端**顺序挑（SSL_select_next_proto 的语义就是"server 优先"）。
    unsigned char* chosen     = nullptr;
    unsigned char  chosen_len = 0;
    const int      rc         = ::SSL_select_next_proto(
        &chosen, &chosen_len,
        self->alpn_server_wire_.data(),
        static_cast<unsigned int>(self->alpn_server_wire_.size()),
        in, inlen);

    if (rc != OPENSSL_NPN_NEGOTIATED || chosen == nullptr || chosen_len == 0) {
        // 没有共同协议：**不中止握手**，只是协商结果为空。
        // "必须 h2" 是业务策略，由 on_accept/on_linkup 里自己判断并 close()。
        return SSL_TLSEXT_ERR_NOACK;
    }

    // 指针指向 alpn_server_wire_（TlsContext 成员，生命周期覆盖整个 SSL_CTX）。
    *out    = chosen;
    *outlen = chosen_len;
    return SSL_TLSEXT_ERR_OK;
}

std::string TlsContext::peer_fingerprint(SSL* ssl)
{
    X509* cert = get_peer_cert(ssl);
    if (!cert) {
        return std::string();
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;
    const bool ok = (X509_digest(cert, EVP_sha256(), digest, &digest_len) == 1);
    X509_free(cert);
    if (!ok) {
        return std::string();
    }

    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(static_cast<std::size_t>(digest_len) * 2);
    for (unsigned int i = 0; i < digest_len; ++i) {
        out.push_back(kHex[(digest[i] >> 4) & 0x0F]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

error_code_t TlsContext::verify_peer(SSL* ssl) const
{
    if (!ssl) {
        return make_boost_error_code(NetErrc::tls_handshake_failed);
    }

    // 1) 指纹 pin（自签场景的主手段）。
    if (!cfg_.pinned_sha256.empty()) {
        const std::string expected = tls_detail::normalize_fingerprint(cfg_.pinned_sha256);
        const std::string actual   = peer_fingerprint(ssl);
        if (actual.empty() || actual != expected) {
            return make_boost_error_code(NetErrc::tls_cert_rejected);
        }
    }

    // 2) 主机名不在这里查：已经在 TlsTransport 构造时通过
    //    X509_VERIFY_PARAM_set1_host 交给 OpenSSL 的链校验，握手阶段就会失败
    //    （见 TlsTransport 构造函数里的说明 —— X509_check_host 在 4.1 被弃用）。
    //
    //    也就是说：`verify_certificate = true` 时，主机名不匹配的握手**根本
    //    到不了这里**，会走 on_linkfail(tls_cert_rejected)。

    return error_code_t{};
}

// =============================================================================
// TlsTransport
// =============================================================================

namespace {

class TlsTransport final : public Transport {
public:
    TlsTransport(socket_t&& sock, std::shared_ptr<TlsContext> ctx)
        : ctx_(std::move(ctx))
        , ssl_(std::move(sock), ctx_->native())
    {
        // 客户端：SNI（虚拟主机 / 多证书部署需要）。服务端忽略。
        if (!ctx_->server()) {
            const std::string& name = ctx_->config().server_name;
            if (!name.empty()) {
                ::SSL_set_tlsext_host_name(ssl_.native_handle(), name.c_str());

                // 主机名校验交给 OpenSSL 的**链校验**去做（握手期），
                // 而不是握手后自己调 X509_check_host：
                //   * X509_check_host 在 OpenSSL 4.1 被标记 deprecated，
                //     MSVC 的 /WX 会把它变成 error C2220（CI 上真实踩到，
                //     GitHub runner 的 choco 包已经是 4.1）；
                //   * 走 VERIFY_PARAM 后，主机名不匹配会以
                //     「certificate verify failed」在握手阶段失败，
                //     由 map_ssl_error 归一化成 tls_cert_rejected。
                if (ctx_->config().verify_certificate) {
                    X509_VERIFY_PARAM* param = ::SSL_get0_param(ssl_.native_handle());
                    if (param != nullptr) {
                        ::X509_VERIFY_PARAM_set_hostflags(
                            param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
                        ::X509_VERIFY_PARAM_set1_host(param, name.c_str(), name.size());
                    }
                }
            }
        }
    }

    [[nodiscard]] executor_type get_executor() noexcept override
    {
        return ssl_.get_executor();
    }

    [[nodiscard]] bool needs_handshake() const noexcept override { return true; }

    void async_handshake(HandshakeHandler handler) override
    {
        if (!handler) {
            return;
        }
        std::shared_ptr<TlsContext> ctx = ctx_;
        SSL*                        raw = ssl_.native_handle();
        const asio::ssl::stream_base::handshake_type type =
            ctx->server() ? asio::ssl::stream_base::server
                          : asio::ssl::stream_base::client;

        // ★ 强自持：完成处理捕获 ctx（shared_ptr），因此握手期间
        //   SSL_CTX 一定活着；raw 指向的 SSL 对象由本传输持有，
        //   而传输由 Conn::Impl 持有、Impl 又被上层完成处理强引用。
        ssl_.async_handshake(
            type, [this, ctx, raw, handler = std::move(handler)](
                      const error_code_t& ec) mutable {
                if (ec) {
                    handler(map_ssl_error(ec));
                    return;
                }
                handshaken_ = true;
                peer_fingerprint_ = TlsContext::peer_fingerprint(raw);
                // ALPN 结果只在握手结束那一刻有效，这里取一次存下来：
                // 之后 Conn::alpn_protocol() 可以在任意线程读，不必再碰 SSL*。
                alpn_ = TlsContext::peer_alpn(raw);
                // 握手成功之后再做 pin / 主机名校验：失败时业务还没拿到 Conn。
                handler(ctx->verify_peer(raw));
            });
    }

    void async_read_some(asio::mutable_buffer buf, IoHandler handler) override
    {
        ssl_.async_read_some(
            buf, [handler = std::move(handler)](const error_code_t& ec,
                                                std::size_t n) mutable {
                if (handler) {
                    handler(map_ssl_error(ec), n);
                }
            });
    }

    void async_read_exact(asio::mutable_buffer buf, IoHandler handler) override
    {
        asio::async_read(
            ssl_, buf, [handler = std::move(handler)](const error_code_t& ec,
                                                      std::size_t n) mutable {
                if (handler) {
                    handler(map_ssl_error(ec), n);
                }
            });
    }

    void async_write(const std::array<asio::const_buffer, kMaxIoBatch>& bufs,
                     IoHandler handler) override
    {
        // 与明文同一套：bufs 作为**缓冲序列**整体传入（TLS 记录层会自行分片，
        // 完成回调的 n 仍是本次请求的总字节数，因此 Conn 的逐笔归因不受影响）。
        asio::async_write(
            ssl_, bufs, [handler = std::move(handler)](const error_code_t& ec,
                                                       std::size_t n) mutable {
                if (handler) {
                    handler(map_ssl_error(ec), n);
                }
            });
    }

    void close(bool graceful) noexcept override
    {
        try {
            // ★ 用 next_layer()（tcp::socket）而不是 lowest_layer()：
            //   `ssl::stream<S>` 的 lowest_layer_type 是 `basic_socket`，
            //   **没有** shutdown() 成员；next_layer_type 才是 S 本身。
            //   close_socket_noexcept<tcp>() 需要流 socket 的 shutdown 才能
            //   保证"先 FIN 再关 fd"的顺序（A-16）。
            socket_t& sock = ssl_.next_layer();
            if (!sock.is_open()) {
                return;
            }
            if (graceful && handshaken_) {
                // 尽力发送 close_notify：**只发不等待**。
                //
                // 为什么要临时非阻塞：SSL_shutdown 的第二次调用会等待对端
                // 的 close_notify，若对端不发（恶意或异常），阻塞式调用会
                // 永久挂住 io 线程 —— 那正是"一条连接拖死整个池"的经典事故。
                // 这里只做第一次调用（发送 close_notify），随即关闭。
                error_code_t ignored;
                sock.non_blocking(true, ignored);
                ::SSL_shutdown(ssl_.native_handle());
            }
            close_socket_noexcept<tcp_protocol_t>(sock, graceful);
        } catch (...) {
            // close() 契约：绝不抛异常。
        }
    }

    [[nodiscard]] bool is_open() const noexcept override
    {
        return ssl_.next_layer().is_open();
    }

    [[nodiscard]] endpoint_t local_endpoint(error_code_t& ec) const override
    {
        return ssl_.next_layer().local_endpoint(ec);
    }

    [[nodiscard]] endpoint_t remote_endpoint(error_code_t& ec) const override
    {
        return ssl_.next_layer().remote_endpoint(ec);
    }

    [[nodiscard]] socket_t& lowest_layer() noexcept override
    {
        // 返回 S 本身（tcp::socket）：socket 选项（NODELAY/SNDBUF/LINGER）
        // 与明文路径完全一致，握手前后都可设置。
        return ssl_.next_layer();
    }

    [[nodiscard]] bool is_secure() const noexcept override { return true; }

    [[nodiscard]] std::string peer_certificate_sha256() const override
    {
        return peer_fingerprint_;
    }

    [[nodiscard]] std::string negotiated_protocol() const override
    {
        return alpn_;
    }

private:
    // ★ 声明顺序：ctx_ 必须在 ssl_ **之前** —— ssl::stream 析构时需要
    //   SSL_CTX 仍然有效（反向析构 = 先释放 CTX 再析构 stream，UB）。
    std::shared_ptr<TlsContext> ctx_;
    asio::ssl::stream<socket_t> ssl_;
    bool                        handshaken_ = false;
    std::string                 peer_fingerprint_;
    std::string                 alpn_;   ///< 握手协商出的协议（空 = 未协商）
};

} // namespace

TransportPtr make_tls_transport(socket_t&& sock, std::shared_ptr<TlsContext> ctx)
{
    if (!ctx) {
        return TransportPtr();
    }
    return TransportPtr(new TlsTransport(std::move(sock), std::move(ctx)));
}

} // namespace netmech

#endif // NETMECH_HAS_TLS
