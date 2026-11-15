// =============================================================================
//  netmech - 明文传输实现
// =============================================================================
//  文件    : src/plain_transport.cpp
//  用途    : Transport 的明文 TCP 实现 —— 把接口原样转发到 socket。
//  依赖    : 独立 Asio + 标准库。不含 <iostream>。
//  编码    : UTF-8
//
//  ★ 这一层是"零行为变化"的证明点
//  ---------------------------------
//  转发必须与 0.90 里 Conn 直接调用 socket 的写法**逐字对应**：
//    * some 读   → `socket.async_read_some`
//    * exact 读  → `asio::async_read(socket, ...)`
//    * 批量写    → `asio::async_write(socket, bufs, ...)`（bufs 是 buffer 序列，
//                   绝不能写成 `asio::buffer(bufs.data(), count)`，那会发出
//                   buffer 对象自身的内存）
//    * 关闭      → `close_socket_noexcept<tcp>(sock, graceful)`
//  任何一处偏离都会在回归用例里表现为"读不到 / 写乱码 / 关闭顺序错"。
// =============================================================================
#include "netmech_srcpch.hpp"

#include "transport.hpp"

#include "netmech/common.hpp"

#include <utility>

namespace netmech {

namespace {

/// 明文 TCP 传输：薄转发层。
class PlainTransport final : public Transport {
public:
    explicit PlainTransport(socket_t&& sock) : sock_(std::move(sock)) {}

    [[nodiscard]] executor_type get_executor() noexcept override
    {
        return sock_.get_executor();
    }

    void async_read_some(asio::mutable_buffer buf, IoHandler handler) override
    {
        sock_.async_read_some(buf, std::move(handler));
    }

    void async_read_exact(asio::mutable_buffer buf, IoHandler handler) override
    {
        asio::async_read(sock_, buf, std::move(handler));
    }

    void async_write(const std::array<asio::const_buffer, kMaxIoBatch>& bufs,
                     IoHandler handler) override
    {
        // ★ bufs 必须**整体**作为 buffer sequence 传入；写成
        //   `asio::buffer(bufs.data(), count)` 会把 const_buffer 对象自身的
        //   内存当成数据发出去（0.90 的 conn.cpp 里有这条注释，保留）。
        asio::async_write(sock_, bufs, std::move(handler));
    }

    void close(bool graceful) noexcept override
    {
        close_socket_noexcept<tcp_protocol_t>(sock_, graceful);
    }

    [[nodiscard]] bool is_open() const noexcept override { return sock_.is_open(); }

    [[nodiscard]] endpoint_t local_endpoint(error_code_t& ec) const override
    {
        return sock_.local_endpoint(ec);
    }

    [[nodiscard]] endpoint_t remote_endpoint(error_code_t& ec) const override
    {
        return sock_.remote_endpoint(ec);
    }

    [[nodiscard]] socket_t& lowest_layer() noexcept override { return sock_; }

private:
    socket_t sock_;
};

} // namespace

void Transport::async_handshake(HandshakeHandler handler)
{
    // 明文没有握手。只有 needs_handshake() 为 true 时 Conn 才会调用本函数，
    // 走到这里说明实现漏了 needs_handshake() —— 用明确错误码暴露，
    // 而不是静默"握手成功"。
    //
    // ★ 同步回调是安全的：本函数的调用方（Conn::Impl::establish）本身就在
    //   该连接的 io 线程上执行，因此"同步派发"与"投递后派发"遵循同一线程模型；
    //   而且它保证"受理即恰好一次回调"不会被投递失败破坏。
    if (!handler) {
        return;
    }
    handler(make_boost_error_code(NetErrc::tls_unsupported));
}

TransportPtr make_plain_transport(socket_t&& sock)
{
    return TransportPtr(new PlainTransport(std::move(sock)));
}

} // namespace netmech
