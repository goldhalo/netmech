// =============================================================================
//  NetMechanism - UdpEndpoint 实现
// =============================================================================
//  文件    : udp_endpoint.cpp
//  用途    : 实现 netmech::UdpEndpoint / create_udp_endpoint。
//  编码    : UTF-8
//
//  设计要点（与 TCP 侧的差异）
//  --------------------------
//  * **推送式**：库自己持续 recvfrom，收到就回调，调用方不调 read()。
//  * **不做会话管理**：源地址只是观测值，不用来识别身份（见 udp.hpp 文件头）。
//  * **错误不导致断链**：除了 socket 不可恢复错误，所有失败都是单次操作失败。
//  * 发送是同步的：send_to 返回 true 只表示已交给内核。
// =============================================================================
#include "netmech_srcpch.hpp"

#include "netmech/udp.hpp"

#include "timer.hpp"

#include "address_internal.hpp"
#include "io_pool.hpp"

#include <array>
#include <chrono>
#include <mutex>
#include <vector>

namespace netmech {

namespace {

using udp = asio::ip::udp;

/// 进程内 UDP 端点编号（与 TCP 的 endpoint_id_t 分开计数，互不干扰）。
std::atomic<std::uint64_t>& udp_id_counter() noexcept
{
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

constexpr std::size_t kMinRecvCapacity = 64;

} // namespace

// =============================================================================
//  Impl
// =============================================================================

struct UdpEndpoint::Impl : public std::enable_shared_from_this<UdpEndpoint::Impl> {
    Impl(asio::io_context& io_ctx, IoPool& pool_ref, UdpConfig conf)
        : io(io_ctx)
        , socket(io)
        , pool(&pool_ref)
        , cfg(std::move(conf))
        , id(udp_id_counter().fetch_add(1, std::memory_order_relaxed) + 1)
    {
    }

    io_context_t&                 io;
    udp::socket                   socket;
    IoPool*                       pool;
    UdpConfig                     cfg;
    std::uint64_t                 id;

    std::shared_ptr<UdpHandler>   handler;
    std::shared_ptr<void>         user_ctx;
    std::weak_ptr<UdpEndpoint>    owner;

    /// 接收缓冲：borrow 模式复用这一块；own 模式每次按实际长度另分配。
    std::vector<std::uint8_t>     recv_buf;

    /// 当前这次 recvfrom 的来源地址（asio 要求调用方持有）。
    udp::endpoint                 sender;

    /// 用户定时回调（每个端点最多一个）。只在其 io 线程内读写。
    std::shared_ptr<Timer>        user_timer;

    /// 定时器代次：set/clear 时递增，用于丢弃旧回调。
    std::uint64_t                 timer_gen = 0;
    std::atomic<bool>             closing{false};
    std::atomic<bool>             closed{false};

    std::mutex                    peer_mutex;
    std::optional<Address>        last_peer;
    std::chrono::steady_clock::time_point last_recv{};

    std::atomic<std::uint64_t>    received{0};
    std::atomic<std::uint64_t>    sent{0};
    std::atomic<std::uint64_t>    send_failed{0};   ///< 已受理但内核拒绝（缓冲满等）
    std::atomic<std::uint64_t>    truncated{0};

    // -------------------------------------------------------------------------
    //  公共门面
    // -------------------------------------------------------------------------

    std::shared_ptr<UdpEndpoint> public_self() const
    {
        return owner.lock();
    }

    /// 真正的发送。**只在其 io 线程上调用**——因此不需要锁。
    void do_send(const udp::endpoint& target, ConstBuf buf) noexcept
    {
        if (closing.load(std::memory_order_acquire)) {
            return;
        }
        error_code_t ec;
        const std::size_t n =
            socket.send_to(asio::buffer(buf.data, buf.size), target, 0, ec);
        if (ec || n != buf.size) {
            send_failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        sent.fetch_add(1, std::memory_order_relaxed);
    }

    /// 交付一条报文给 handler（borrow / own 两种形态）。
    void deliver(const std::uint8_t* data, std::size_t n, const udp::endpoint& from,
                 std::error_code ec)
    {
        auto self = public_self();
        if (!self) {
            return;
        }
        const Address src = detail::from_asio_udp_endpoint(from);

        // 先通知"来源变了"（含首次），再交付数据。
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(peer_mutex);
            if (!last_peer.has_value() || !(*last_peer == src)) {
                last_peer = src;
                changed   = true;
            }
        }
        std::shared_ptr<UdpHandler> h = handler;
        if (!h) {
            return;
        }
        if (changed) {
            try {
                h->on_peer_changed(*self, src);
            } catch (...) {
            }
        }

        if (cfg.deliver == DeliverMode::own) {
            // 每包独立分配：调用方可以持有 owner 跨线程处理。
            auto block = std::make_shared<std::vector<std::uint8_t>>(data, data + n);
            Buf buf{block, block->data(), block->size()};
            try {
                h->on_read(*self, ec, buf, n, src);
            } catch (...) {
            }
        } else {
            Buf buf{nullptr, const_cast<std::uint8_t*>(data), n};
            try {
                h->on_read(*self, ec, buf, n, src);
            } catch (...) {
            }
        }
    }

    // -------------------------------------------------------------------------
    //  接收循环
    // -------------------------------------------------------------------------

    void do_receive()
    {
        if (closing.load(std::memory_order_acquire)) {
            return;
        }
        std::shared_ptr<Impl> self;
        try {
            self = shared_from_this();
        } catch (...) {
            return;
        }
        socket.async_receive_from(
            asio::buffer(recv_buf), sender,
            [self](const error_code_t& ec, std::size_t n) { self->handle_receive(ec, n); });
    }

    void handle_receive(const error_code_t& ec, std::size_t n)
    {
        if (closing.load(std::memory_order_acquire)) {
            return;
        }

        if (ec == asio::error::operation_aborted) {
            return;   // 关闭流程会收尾
        }

        // 除 message_size（截断）与 operation_aborted 外的错误：记录一次，继续收。
        // UDP 没有"断链"概念，单次接收失败不影响端点。
        if (ec && ec != asio::error::message_size) {
            do_receive();
            return;
        }

        // 截断判定：Windows 会报 message_size；但实测某些路径不报，
        // 只能用"读满整个缓冲"兜底（n == capacity 时无法区分是否还有剩余，
        // 宁可报截断也不要把半条报文当完整的交上去）。
        const bool is_truncated = (ec == asio::error::message_size) || (n >= recv_buf.size());

        received.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(peer_mutex);
            last_recv = std::chrono::steady_clock::now();
        }
        if (is_truncated) {
            truncated.fetch_add(1, std::memory_order_relaxed);
        }
        deliver(recv_buf.data(), n, sender,
                is_truncated ? make_boost_error_code(NetErrc::datagram_truncated)
                             : std::error_code{});
        do_receive();
    }

    // -------------------------------------------------------------------------
    //  关闭
    // -------------------------------------------------------------------------

    void close() noexcept
    {
        if (closing.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        error_code_t ignored;
        socket.cancel(ignored);
        socket.close(ignored);

        std::shared_ptr<Impl> self;
        try {
            self = shared_from_this();
        } catch (...) {
            return;
        }
        asio::post(io, [self]() { self->finish_close(); });
    }

    void finish_close() noexcept
    {
        if (closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        // 端点关闭：用户定时器必须停掉，否则回调会在 close 之后继续触发
        ++timer_gen;
        if (user_timer) {
            user_timer->cancel();
            user_timer.reset();
        }
        auto self = public_self();
        std::shared_ptr<UdpHandler> h = std::move(handler);
        handler.reset();
        if (self && h) {
            try {
                h->on_close(*self, std::error_code{});
            } catch (...) {
            }
        }
    }
};

// =============================================================================
//  UdpEndpoint 门面
// =============================================================================

UdpEndpoint::UdpEndpoint(std::shared_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

UdpEndpoint::~UdpEndpoint()
{
    if (m_impl) {
        m_impl->close();
    }
}

bool UdpEndpoint::send_to(const Address& dst, ConstBuf buf)
{
    if (!m_impl || m_impl->closing.load(std::memory_order_acquire)) {
        return false;
    }
    if (buf.data == nullptr || buf.size == 0) {
        return false;
    }
    if (buf.size > kMaxDatagramSize) {
        return false;
    }

    // ★ 不在这里碰 socket。
    //
    //   原先用一把 send_mutex 保护 send_to，理由是"可能多线程同时发"。
    //   但这个端点的所有 IO 本来只在它自己的 io 线程上跑 —— 把发送也
    //   dispatch 过去，锁就没有存在的必要：
    //     · 从收包回调里调 → 已在同一线程，dispatch 同步执行，零开销
    //     · 从业务线程调   → 排队到该线程，依然无锁
    //
    //   代价：返回值只表示"已受理"。真正的失败（发送缓冲满）发生在 io 线程，
    //   计入 stats().datagrams_send_failed()。对实时流这是合理取舍——
    //   宁可丢包降码率，也不阻塞调用线程。
    auto self = m_impl;
    const udp::endpoint target = detail::to_asio_udp_endpoint(dst);
    asio::dispatch(self->io, [self, target, buf]() { self->do_send(target, buf); });
    return true;
}

Address UdpEndpoint::local_address() const noexcept
{
    if (!m_impl) {
        return Address{};
    }
    error_code_t ec;
    const udp::endpoint ep = m_impl->socket.local_endpoint(ec);
    if (ec) {
        return Address{};
    }
    return detail::from_asio_udp_endpoint(ep);
}

bool UdpEndpoint::is_open() const noexcept
{
    return m_impl && !m_impl->closing.load(std::memory_order_acquire);
}

std::uint32_t UdpEndpoint::id() const noexcept
{
    return m_impl ? static_cast<std::uint32_t>(m_impl->id) : 0;
}

std::shared_ptr<void> UdpEndpoint::user_context() const noexcept
{
    return m_impl ? m_impl->user_ctx : nullptr;
}

void UdpEndpoint::close() noexcept
{
    if (m_impl) {
        m_impl->close();
    }
}

std::uint64_t UdpEndpoint::datagrams_received() const noexcept
{
    return m_impl ? m_impl->received.load(std::memory_order_relaxed) : 0;
}

std::uint64_t UdpEndpoint::datagrams_sent() const noexcept
{
    return m_impl ? m_impl->sent.load(std::memory_order_relaxed) : 0;
}

void UdpEndpoint::set_timer(std::chrono::milliseconds interval,
                              std::weak_ptr<void>     owner,
                              std::function<void()>   cb)
{
    if (!m_impl) {
        return;
    }
    auto self = m_impl;
    // 注册/替换都投递到本端点的 io 线程执行 —— 定时器状态只在那里被读写，
    // 因此全程无锁。从网络回调里调用时 dispatch 会同步执行，没有排队开销。
    asio::dispatch(self->io, [self, interval, owner, cb = std::move(cb)]() mutable {
        ++self->timer_gen;
        if (self->user_timer) {
            self->user_timer->cancel();
            self->user_timer.reset();
        }
        if (!cb || interval.count() <= 0) {
            return;   // 取消
        }
        std::error_code ec;
        self->user_timer = Timer::every(self->io, interval, owner, std::move(cb), ec);
    });
}

void UdpEndpoint::clear_timer()
{
    set_timer(std::chrono::milliseconds(0), std::weak_ptr<void>{}, nullptr);
}

std::uint64_t UdpEndpoint::datagrams_send_failed() const noexcept
{
    return m_impl ? m_impl->send_failed.load(std::memory_order_relaxed) : 0;
}

std::uint64_t UdpEndpoint::datagrams_truncated() const noexcept
{
    return m_impl ? m_impl->truncated.load(std::memory_order_relaxed) : 0;
}

std::optional<Address> UdpEndpoint::last_peer() const noexcept
{
    if (!m_impl) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lk(m_impl->peer_mutex);
    return m_impl->last_peer;
}

std::chrono::steady_clock::time_point UdpEndpoint::last_recv_time() const noexcept
{
    if (!m_impl) {
        return {};
    }
    std::lock_guard<std::mutex> lk(m_impl->peer_mutex);
    return m_impl->last_recv;
}

// =============================================================================
//  工厂
// =============================================================================

std::optional<std::uint16_t> find_free_port_run(std::uint16_t lo,
                                               std::uint16_t hi,
                                               std::uint16_t count,
                                               std::uint16_t alignment) noexcept
{
    if (count == 0 || alignment == 0 || lo > hi || count > (hi - lo + 1)) {
        return std::nullopt;
    }

    // 探测不需要跑 io 线程，用一个临时 context 即可（只 bind，不 run）。
    asio::io_context probe_ctx;

    for (std::uint32_t base = lo; base + count - 1u <= hi; ++base) {
        if (alignment > 1 && (base % alignment) != 0) {
            continue;
        }

        std::vector<udp::socket> held;
        held.reserve(count);
        bool all_free = true;

        for (std::uint16_t i = 0; i < count; ++i) {
            error_code_t ec;
            udp::socket sock(probe_ctx);
            sock.open(udp::v4(), ec);
            if (ec) {
                all_free = false;
                break;
            }
            sock.bind(udp::endpoint(asio::ip::address_v4::any(),
                                    static_cast<std::uint16_t>(base + i)), ec);
            if (ec) {
                all_free = false;
                break;
            }
            held.push_back(std::move(sock));   // 持住，避免下一轮前被复用
        }

        if (all_free) {
            // held 在此作用域结束时全部关闭 —— 端口立即可用
            return static_cast<std::uint16_t>(base);
        }
    }
    return std::nullopt;
}

/// 内部工厂：io_context 由调用方指定。
///
/// 两个公开入口的差别只有 io 来源：
///   · create_udp_endpoint      → 从 IoPool 轮转取一个（各端点分散到不同线程）
///   · create_udp_endpoint_on   → 跟随 anchor，与它同线程
std::shared_ptr<UdpEndpoint> create_udp_endpoint_impl(asio::io_context&          io,
                                                      const UdpConfig&           cfg,
                                                      std::shared_ptr<UdpHandler> handler,
                                                      std::shared_ptr<void>      user_ctx,
                                                      std::error_code&           ec);

std::shared_ptr<UdpEndpoint> create_udp_endpoint(const UdpConfig&            cfg,
                                                 std::shared_ptr<UdpHandler> handler,
                                                 std::shared_ptr<void>       user_ctx,
                                                 std::error_code&            ec)
{
    return create_udp_endpoint_impl(IoPool::global().next_io_context(), cfg,
                                std::move(handler), std::move(user_ctx), ec);
}

std::shared_ptr<UdpEndpoint> create_udp_endpoint_on(const UdpEndpoint&          anchor,
                                                    const UdpConfig&            cfg,
                                                    std::shared_ptr<UdpHandler> handler,
                                                    std::shared_ptr<void>       user_ctx,
                                                    std::error_code&            ec)
{
    if (!anchor.m_impl) {
        ec = make_boost_error_code(NetErrc::invalid_argument);
        return nullptr;
    }
    // 跟随 anchor 的 io_context —— 保证两个端点的回调在同一条线程上
    return create_udp_endpoint_impl(anchor.m_impl->io, cfg,
                                std::move(handler), std::move(user_ctx), ec);
}

std::shared_ptr<UdpEndpoint> create_udp_endpoint_impl(asio::io_context&          io,
                                                      const UdpConfig&           cfg,
                                                      std::shared_ptr<UdpHandler> handler,
                                                      std::shared_ptr<void>      user_ctx,
                                                      std::error_code&           ec)
{
    ec.clear();

    if (!handler) {
        ec = make_boost_error_code(NetErrc::invalid_argument);
        return nullptr;
    }
    if (cfg.recv_capacity == 0 || cfg.recv_capacity > kMaxDatagramSize) {
        ec = make_boost_error_code(NetErrc::invalid_argument);
        return nullptr;
    }

    auto impl = std::make_shared<UdpEndpoint::Impl>(io, IoPool::global(), cfg);
    impl->handler  = std::move(handler);
    impl->user_ctx = std::move(user_ctx);
    impl->recv_buf.resize(cfg.recv_capacity);

    // bind
    const udp::endpoint bind_ep = detail::to_asio_udp_endpoint(cfg.bind);
    impl->socket.open(bind_ep.protocol(), ec);
    if (ec) {
        return nullptr;
    }

    if (cfg.reuse_address) {
        impl->socket.set_option(asio::socket_base::reuse_address(true), ec);
        ec.clear();   // 选项失败不影响功能
    }
    if (cfg.allow_broadcast) {
        impl->socket.set_option(asio::socket_base::broadcast(true), ec);
        ec.clear();
    }
    if (cfg.socket_buf_size != 0) {
        impl->socket.set_option(asio::socket_base::receive_buffer_size(
                                    static_cast<int>(cfg.socket_buf_size)), ec);
        impl->socket.set_option(asio::socket_base::send_buffer_size(
                                    static_cast<int>(cfg.socket_buf_size)), ec);
        ec.clear();
    }

    impl->socket.bind(bind_ep, ec);
    if (ec) {
        return nullptr;
    }

    // 组播：加入组（可指定网卡）
    if (cfg.multicast_group.has_value()) {
        const asio::ip::address group = detail::to_asio_address(*cfg.multicast_group);
        if (group.is_v4()) {
            impl->socket.set_option(asio::ip::multicast::join_group(group.to_v4()), ec);
        } else {
            impl->socket.set_option(
                asio::ip::multicast::join_group(group.to_v6(), 0), ec);
        }
        if (ec) {
            return nullptr;
        }
        if (cfg.multicast_hops != 0) {
            impl->socket.set_option(asio::ip::multicast::hops(cfg.multicast_hops), ec);
            ec.clear();
        }
        impl->socket.set_option(
            asio::ip::multicast::enable_loopback(cfg.multicast_loopback), ec);
        ec.clear();
    }

    // 非阻塞：send_to 满时立即返回，绝不卡住调用线程。
    impl->socket.non_blocking(true, ec);
    if (ec) {
        return nullptr;
    }

    auto ep = std::shared_ptr<UdpEndpoint>(new UdpEndpoint(impl));
    impl->owner = ep;

    // 启动接收循环（此时可能立刻收到报文，所以 owner 必须先就绪）
    impl->do_receive();
    return ep;
}

} // namespace netmech
