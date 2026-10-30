// ============================================================================
//  test_support.hpp —— NetMechanism 测试公共设施
//
//  提供：
//    * 等待条件 / 计数闸门（避免裸 sleep 造成 flaky）
//    * 可复用的 EchoLinkable / EchoIoHandler（回环测试的"对端业务"）
//    * 端口探测（避免固定端口被占用）
//
//  注意：所有 handler 都遵守库的契约——
//    * 同一 Conn 同一时刻只允许一笔 async_read 在途；
//    * async_write 的 buffer 在回调返回前不得复用（每帧独立分配）。
// ============================================================================
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include <netmech/net.hpp>

#include "test_framework.hpp"

namespace netmech_test
{

using namespace std::chrono_literals;
using tcp = asio::ip::tcp;
using std::error_code;

// ---------------------------------------------------------------------------
// 通用等待工具
// ---------------------------------------------------------------------------

/// 轮询等待 predicate 成立；超时返回 false。
inline bool wait_for(const std::function<bool()>& pred,
                     std::chrono::milliseconds timeout = 5000ms,
                     std::chrono::milliseconds step = 2ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(step);
    }
    return pred();
}

inline void sleep_ms(std::uint64_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/// 计数闸门：用于"等待 N 个回调都发生"
class Latch
{
public:
    explicit Latch(std::size_t n = 1) : m_remaining(n) {}

    void count_down()
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_remaining > 0)
                --m_remaining;
        }
        m_cv.notify_all();
    }

    bool wait(std::chrono::milliseconds timeout = 5000ms)
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        return m_cv.wait_for(lk, timeout, [this] { return m_remaining == 0; });
    }

    bool done() const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_remaining == 0;
    }

    Latch& operator=(const Latch&) = delete;

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::size_t m_remaining;
};

//// ---------------------------------------------------------------------------
// 端口工具
// ---------------------------------------------------------------------------

/// 借用系统分配一个本机空闲 TCP 端口（bind 端口 0 后立即释放）。
///
/// @note 从释放到真正 listen 之间存在极小窗口，仅用于本机回环测试。
///
/// ★ 曾尝试"进程级保留端口"（不 close 探测 socket、登记到静态表）来消除
///   这个 TOCTOU 窗口，**该方案不可行，已回退**：
///     * 保留的 acceptor 一直占着端口，而 listen() 带 reuse-address 语义，
///       两者叠加会让后续用例**大面积**失败（实测 40/40 全失败）；
///     * 静态 io_context 跨用例存活，内部状态会干扰后续用例。
///   结论：该窗口属于**测试工具的已知局限**。若需要确定性端口，
///   正确做法是直接在 listen 时用端口 0 + local_address_of 回查，
///   而不是预先探测。
inline std::uint16_t pick_free_port()
{
    asio::io_context ctx;
    tcp::acceptor acc(ctx);
    error_code ec;
    acc.open(tcp::v4(), ec);
    if (ec)
        return 0;
    acc.set_option(asio::socket_base::reuse_address(false), ec);
    acc.bind(tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), ec);
    if (ec)
        return 0;
    const auto ep = acc.local_endpoint(ec);
    acc.close(ec);
    return ec ? 0 : ep.port();
}
inline tcp::endpoint loopback(std::uint16_t port)
{
    return tcp::endpoint(asio::ip::make_address("127.0.0.1"), port);
}

// ---------------------------------------------------------------------------
// Echo 业务：服务端把收到的字节原样回写
// ---------------------------------------------------------------------------

class EchoIoHandler : public netmech::ConnHandler
{
public:
    explicit EchoIoHandler(std::atomic<std::uint64_t>* bytes = nullptr)
        : m_bytes(bytes)
    {
    }

    bool on_open(std::shared_ptr<netmech::Conn> ep) override
    {
        m_ep = ep;
        return start_read();
    }

    void on_read(std::error_code ec, netmech::Buf buf, std::size_t n) override
    {
        if (ec || n == 0)
        {
            if (ec && m_ep)
                m_ep->close();
            return;
        }
        if (m_bytes)
            m_bytes->fetch_add(n);

        // 原样回写：buf 就是本 handler 提交的那块接收缓冲，回调期间有效。
        if (!m_ep->write({nullptr, buf.data, n}))
            m_ep->close();
        // 续读放在 on_write 里（写完再读，保证同一时刻只有一笔读在途）。
    }

    void on_write(std::error_code ec, netmech::ConstBuf, std::size_t n) override
    {
        if (ec)
        {
            if (m_ep)
                m_ep->close();
            return;
        }
        echo_bytes.fetch_add(n);
        (void)start_read();
    }

    void on_close() override
    {
        fini_count.fetch_add(1);
        m_ep.reset();
    }

    void on_idle(const netmech::IdleInfo& info) override
    {
        if (info.kind == netmech::IdleKind::recv)
            recv_ka.fetch_add(1);
        else
            send_ka.fetch_add(1);
    }

    std::shared_ptr<netmech::Conn> endpoint() const { return m_ep; }

    std::atomic<std::uint64_t> fini_count{0};
    std::atomic<std::uint64_t> recv_ka{0};
    std::atomic<std::uint64_t> send_ka{0};
    std::atomic<std::uint64_t> echo_bytes{0};

private:
    bool start_read()
    {
        if (!m_ep || !m_ep->is_open())
            return false;
        return m_ep->read({nullptr, m_rx.data(), m_rx.size()});
    }

    std::shared_ptr<netmech::Conn> m_ep;
    std::array<std::uint8_t, 8192>     m_rx{};
    std::atomic<std::uint64_t>*        m_bytes = nullptr;
};

/// 服务端订阅者：每个 accept 都开一个 EchoIoHandler
class EchoLinkable : public netmech::Linkable
{
public:
    explicit EchoLinkable(std::atomic<std::uint64_t>* bytes = nullptr)
        : m_bytes(bytes)
    {
    }

    std::shared_ptr<netmech::ConnHandler> on_accept(netmech::Conn& c) override
    {
        {
            // 测试要按 id 索引连接：Conn 由库以 shared_ptr 持有，此处可安全取回。
            std::lock_guard<std::mutex> lk(m_mutex);
            m_eps.push_back(c.shared_from_this());
        }
        accepted.fetch_add(1);
        m_latch.count_down();
        return std::make_shared<EchoIoHandler>(m_bytes);
    }

    std::shared_ptr<netmech::ConnHandler> on_linkup(netmech::Conn& c) override
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_eps.push_back(c.shared_from_this());
        }
        linked_up.fetch_add(1);
        m_latch.count_down();
        return std::make_shared<EchoIoHandler>(m_bytes);
    }

    void on_linkdown(netmech::Conn&) override
    {
        linked_down.fetch_add(1);
    }

    void on_linkfail(const netmech::Address&, error_code ec) override
    {
        fail_ec.store(ec.value());
        linked_fail.fetch_add(1);
    }

    /// 等待累计 events 次 accept/linkup
    bool wait_links(std::size_t n, std::chrono::milliseconds timeout = 5000ms)
    {
        return wait_for([&] { return accepted.load() + linked_up.load() >= n; }, timeout);
    }

    std::vector<std::shared_ptr<netmech::Conn>> endpoints_snapshot() const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_eps;
    }

    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> linked_up{0};
    std::atomic<std::uint64_t> linked_down{0};
    std::atomic<std::uint64_t> linked_fail{0};
    std::atomic<int> fail_ec{0};

private:
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<netmech::Conn>> m_eps;
    std::atomic<std::uint64_t>* m_bytes = nullptr;
    Latch m_latch{0};
};

} // namespace netmech_test
