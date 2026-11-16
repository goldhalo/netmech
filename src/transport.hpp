// =============================================================================
//  netmech - 传输层抽象（内部头）
// =============================================================================
//  文件    : src/transport.hpp
//  用途    : 定义 Transport 接口 —— 一条**已建立**连接上的字节流。
//            明文 TCP 与 TLS 各是一个实现；Conn 只面向这个接口编程。
//  依赖    : 独立 Asio + 标准库。不含 <iostream>。
//  编码    : UTF-8
//
//  ★ 为什么需要这一层（v0.91 引入）
//  ---------------------------------
//  0.90 之前 `Conn::Impl` 直接持有一个**值成员** `asio::ip::tcp::socket`，
//  读写/关闭全部直达 socket。这意味着：
//    * 无法在 Conn 与 socket 之间插入任何东西（TLS 就是被卡在这里）；
//    * 想支持 TLS 只能模板化 Conn（对外头被迫引 asio）或全局换类型
//      （明文与 TLS 无法共存）。
//  抽出 Transport 之后：
//    * Conn 的状态机（FIFO 写队列 / 反压 / 单笔读 / 强自持 / 幂等关闭 /
//      双向保活）**一行都不用改**，它只认"异步读、异步写、关闭"三个动作；
//    * 明文与 TLS 可以在同一个 Mechanism 上共存（每条连接自己选）；
//    * 将来换 mbedTLS / BearSSL 只需再加一个实现（OpenSSL 只出现在
//      tls_transport.cpp 一个 TU 里）。
//
//  ★ 契约（实现方必须满足，Conn 依赖它们）
//  ---------------------------------------
//  [T1] **线程模型不变**：所有 async_* 的完成处理都在本连接绑定的那一个
//       io_context（= 1 条线程）上执行。实现内部**不得**自己起线程。
//  [T2] **恰好在完成处理里回调一次**：发起失败（例如 socket 已关）允许
//       直接 throw，由 Conn 的 try/catch 兜底；一旦受理，handler 必须且
//       只会被调用一次（非 0 error_code 也算一次）。
//  [T3] **缓冲不拷贝**：读到调用方给的 mutable_buffer，写从调用方给的
//       buffer 序列；实现**不得**假设缓冲在完成回调之后仍然有效。
//  [T4] **close() 幂等、noexcept、任意线程**：可以被调用多次；绝不抛异常。
//       `graceful=false` 表示允许直接 RST（丢未发数据），TLS 下同样意味着
//       不尝试 close_notify。
//  [T5] **执行器一致**：get_executor() 返回的就是底层 socket 的 executor，
//       Conn 用它 post/dispatch，因此"1 context = 1 线程"的串行保证成立。
// =============================================================================
#pragma once

#include "netmech_srcpch.hpp"

#include <array>
#include <functional>
#include <memory>
#include <string>

namespace netmech {

/// 一次批量写最多合并的请求笔数。
///
/// 放在这里（而不是 conn.cpp 的匿名命名空间）是因为它同时是
/// `Transport::async_write` 的缓冲序列长度 —— 接口签名要用到它。
inline constexpr std::size_t kMaxIoBatch = 16;

/// 传输层：一条已建立连接上的异步字节流（明文 TCP 或 TLS over TCP）。
///
/// @par 生命周期
///  - 由库创建（`make_plain_transport` / `make_tls_transport`），
///    `Conn::Impl` 以 `std::unique_ptr` **独占**持有，随 Conn 一起析构；
///  - 析构必须在所属 io_context **存活期间**发生（与 socket 的规则一致，
///    见 Conn::Impl 成员顺序的说明）。
///
/// @note 本类型是**内部实现**，不属于公共 API，调用方不可见。
class Transport {
public:
    /// 底层 socket 的执行器类型。
    ///
    /// Asio 1.30 的 `basic_stream_socket` 默认执行器是 `any_io_executor`，
    /// `asio::ssl::stream` 的 `get_executor()` 也返回同一个类型 —— 因此
    /// Conn 不需要知道"这条连接到底是不是 TLS"。
    using executor_type = asio::any_io_executor;

    /// 读写完成处理：(错误码, 本次交付的字节数)。
    using IoHandler = std::function<void(const error_code_t&, std::size_t)>;

    /// 握手完成处理。
    using HandshakeHandler = std::function<void(const error_code_t&)>;

    virtual ~Transport() = default;

    /// 底层 socket 的执行器（Conn 用它 post / dispatch）。
    [[nodiscard]] virtual executor_type get_executor() noexcept = 0;

    /// 是否需要先做握手（TLS 返回 true；明文返回 false）。
    [[nodiscard]] virtual bool needs_handshake() const noexcept { return false; }

    /// 执行握手。**只有 needs_handshake() 为 true 时才会被调用**。
    ///
    /// @note 明文实现的默认行为是"直接以 operation_not_supported 回调"，
    ///       用来把"误调用"变成可观察的错误，而不是静默成功。
    virtual void async_handshake(HandshakeHandler handler);

    /// `some` 语义读：收到至少 1 字节即完成。
    virtual void async_read_some(asio::mutable_buffer buf, IoHandler handler) = 0;

    /// `exact` 语义读：收满 buf.size() 才完成。
    virtual void async_read_exact(asio::mutable_buffer buf, IoHandler handler) = 0;

    /// 批量写：`bufs` 中未使用的元素为零长度缓冲（由 asio 自动跳过）。
    virtual void async_write(const std::array<asio::const_buffer, kMaxIoBatch>& bufs,
                             IoHandler handler) = 0;

    /// 关闭。幂等、noexcept、任意线程可调用。
    ///
    /// @param[in] graceful true = 走优雅关闭（流协议先 shutdown 再 close；
    ///                     TLS 下尽力发送 close_notify）；
    ///                     false = 允许直接丢弃未发数据（配合 SO_LINGER{1,0}）。
    virtual void close(bool graceful) noexcept = 0;

    /// 连接是否仍然打开（快照，不可用于替代完成回调的错误码）。
    [[nodiscard]] virtual bool is_open() const noexcept = 0;

    [[nodiscard]] virtual endpoint_t local_endpoint(error_code_t& ec) const = 0;
    [[nodiscard]] virtual endpoint_t remote_endpoint(error_code_t& ec) const = 0;

    /// 底层 TCP socket（用于设置 NODELAY / SNDBUF / LINGER 等选项）。
    ///
    /// @note TLS 实现返回 `ssl::stream::lowest_layer()`：socket 选项与
    ///       TCP 语义完全一致，握手前后都可以设置。
    [[nodiscard]] virtual socket_t& lowest_layer() noexcept = 0;

    /// 是否加密传输（诊断用）。
    [[nodiscard]] virtual bool is_secure() const noexcept { return false; }

    /// 对端证书的 SHA-256 指纹（小写十六进制、无分隔符）。
    ///
    /// @return 非 TLS 返回空串；TLS 但未取到证书（例如握手未完成）也返回空串。
    [[nodiscard]] virtual std::string peer_certificate_sha256() const { return std::string(); }

    /// ALPN 协商出的应用层协议（如 `"h2"`、`"http/1.1"`）。
    ///
    /// @return 明文传输、未启用 ALPN、或双方没有共同协议时返回空串。
    /// @note 只在握手成功后才有值；实现里在握手完成时取一次快照，
    ///       之后任意线程读都是只读的，不需要加锁。
    [[nodiscard]] virtual std::string negotiated_protocol() const { return std::string(); }
};

/// 传输对象的唯一所有权句柄。
using TransportPtr = std::unique_ptr<Transport>;

/// 用一条**已连接**的明文 TCP socket 构造传输（接管所有权）。
[[nodiscard]] TransportPtr make_plain_transport(socket_t&& sock);

} // namespace netmech
