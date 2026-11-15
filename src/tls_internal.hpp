// =============================================================================
//  netmech - TLS 内部实现（**内部头**）
// =============================================================================
//  文件    : src/tls_internal.hpp
//  用途    : 定义 TlsContext（SSL_CTX 的持有者 + 校验策略）与
//            make_tls_transport()。OpenSSL/Asio-SSL 只出现在实现文件里。
//  依赖    : 独立 Asio（ssl）+ OpenSSL；`NETMECH_TLS=OFF` 时不编译。
//  编码    : UTF-8
//
//  ★ 为什么 SSL_CTX 要单独一层
//  ---------------------------
//  1. **一个监听器一个 CTX**：证书链、私钥、校验模式、协议版本都是
//     listener/connector 级别的配置，不是每条连接一份（每连接重建 CTX
//     要重新读证书、解析私钥，代价以毫秒计）；
//  2. **生命周期**：`asio::ssl::stream` 必须保证 CTX 比它活得久。
//     这里用 `shared_ptr<TlsContext>`，由传输对象持有 —— 连接关了以后
//     CTX 才可能释放，不存在悬垂；
//  3. **隔离 OpenSSL 头**：只有 tls_transport.cpp / 本头引用 `<openssl/*>`，
//     因此 `NETMECH_TLS=OFF` 的构建连头文件都不需要。
// =============================================================================
#pragma once

#include "netmech_srcpch.hpp"

#include "transport.hpp"

#include "netmech/common.hpp"
#include "netmech/tls.hpp"

#if defined(NETMECH_HAS_TLS)

#    include <asio/ssl.hpp>

#    include <openssl/ssl.h>
#    include <openssl/x509.h>

#endif // NETMECH_HAS_TLS

namespace netmech {

namespace tls_detail {

/// 归一化指纹文本：去掉 `:` / `-` / 空白并转小写。
///
/// @note 与是否编译了 TLS 无关（纯字符串处理），因此实现在 tls_config.cpp，
///       明文构建也能链接到 —— 配置校验必须在**两种构建**下行为一致。
[[nodiscard]] std::string normalize_fingerprint(std::string_view in);

/// 是否全部为十六进制字符（空串返回 false）。
[[nodiscard]] bool is_hex_string(const std::string& s) noexcept;

} // namespace tls_detail

/// 校验配置是否自洽（与是否编译了 TLS 无关）。
///
/// @param[in] cfg    配置。
/// @param[in] server true = 服务端视角，false = 客户端视角。
/// @return 空 = 通过；否则为 `NetErrc::invalid_argument`。
///
/// @note 为什么在"没编译 TLS"的构建里也要校验：调用方写错了配置，
///       应该看到 `invalid_argument`（配置问题）而不是 `tls_unsupported`
///       （构建问题）—— 两者排查方向完全不同。
[[nodiscard]] error_code_t validate_tls_config(const TlsConfig& cfg, bool server);

#if defined(NETMECH_HAS_TLS)

/// 一次 listen()/connect() 共用的 TLS 上下文。
///
/// @par 线程安全
///  - 构建完成后**只读**（`SSL_CTX` 在 OpenSSL 里支持多线程并发握手，
///    前提是不要并发修改配置）；
///  - `verify_peer()` 只读 cfg_，可以在多个 io 线程上并发调用。
class TlsContext {
public:
    /// 按配置构建上下文。
    ///
    /// @param[in]  cfg    配置（已经过 validate_tls_config）。
    /// @param[in]  server true = 服务端。
    /// @param[out] ec     失败原因（文件打不开 / 私钥口令错 / 版本不支持…）。
    /// @return 非空；失败返回空并置 @p ec。
    [[nodiscard]] static std::shared_ptr<TlsContext>
    create(const TlsConfig& cfg, bool server, error_code_t& ec);

    [[nodiscard]] asio::ssl::context& native() noexcept { return ctx_; }
    [[nodiscard]] const TlsConfig&    config() const noexcept { return cfg_; }
    [[nodiscard]] bool                server() const noexcept { return server_; }

    /// 有效的握手超时（毫秒；0 → 默认 10000）。
    [[nodiscard]] std::uint32_t handshake_timeout_ms() const noexcept;

    /// 握手成功之后的校验：指纹 pin，以及（可选）主机名。
    ///
    /// @param[in] ssl 已完成握手的原生句柄。
    /// @return 空 = 通过；否则 `tls_cert_rejected`。
    ///
    /// @note 为什么放在握手之后而不是 verify_callback 里：
    ///       回调式校验拿不到"清晰的失败原因"，而且 OpenSSL 在
    ///       `verify_none` 下根本不会调用回调。握手后统一比对更直接，
    ///       也便于把错误码归到 `tls_cert_rejected`。
    /// @note 安全性：握手与校验之间**不会**向业务交付任何数据
    ///       （on_accept/on_linkup 在校验通过后才触发）。
    [[nodiscard]] error_code_t verify_peer(SSL* ssl) const;

    /// 从已完成的 SSL 连接取对端叶子证书的 SHA-256（小写 hex，无分隔符）。
    [[nodiscard]] static std::string peer_fingerprint(SSL* ssl);

private:
    TlsContext(const TlsConfig& cfg, bool server)
        : cfg_(cfg)
        , server_(server)
        , ctx_(server ? asio::ssl::context::tls_server : asio::ssl::context::tls_client)
    {
    }

    TlsConfig               cfg_;
    bool                    server_;
    asio::ssl::context      ctx_;
};

/// 用一条**已连接**的 TCP socket 构造 TLS 传输（接管所有权，并接管 ctx 引用）。
[[nodiscard]] TransportPtr make_tls_transport(socket_t&& sock,
                                              std::shared_ptr<TlsContext> ctx);

#endif // NETMECH_HAS_TLS

} // namespace netmech
