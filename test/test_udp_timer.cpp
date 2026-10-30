// ============================================================================
//  test_udp_timer.cpp —— UdpEndpoint 的定时能力与端口探测
//
//  定时器不对外暴露类，而是挂在 UdpEndpoint 上（每个端点最多一个），
//  因此这里验证的是"端点级的定时行为"，包括生命周期联动。
// ============================================================================
#include <netmech/net.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "test_framework.hpp"

using namespace netmech;
using namespace netmech_test;

namespace {

using namespace std::chrono_literals;

bool wait_value(const std::atomic<int>& v, int target, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (v.load() >= target) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return v.load() >= target;
}

/// 什么都不做的 handler，只为了让端点能创建。
class NullSink final : public UdpHandler {
public:
    void on_read(UdpEndpoint&, std::error_code, Buf, std::size_t,
                 const Address&) override
    {
    }
};

std::shared_ptr<UdpEndpoint> make_endpoint(std::error_code& ec)
{
    UdpConfig cfg;
    cfg.bind = Address("127.0.0.1", 0);
    return create_udp_endpoint(cfg, std::make_shared<NullSink>(), nullptr, ec);
}

} // namespace

// ---------------------------------------------------------------------------
// 1. set_timer 周期触发；clear_timer 停止
// ---------------------------------------------------------------------------
NETMECH_TEST(udp_timer_fires_and_stops)
{
    std::error_code ec;
    auto ep = make_endpoint(ec);
    REQUIRE(!ec);
    REQUIRE(ep != nullptr);

    std::atomic<int> ticks{0};
    auto owner = std::make_shared<int>(0);

    ep->set_timer(20ms, owner, [&ticks] { ticks.fetch_add(1); });
    CHECK(wait_value(ticks, 3, 2000ms));

    ep->clear_timer();
    const int frozen = ticks.load();
    std::this_thread::sleep_for(120ms);
    CHECK_EQ(ticks.load(), frozen);   // 取消后不再触发

    ep->close();
}

// ---------------------------------------------------------------------------
// 2. 重复 set_timer 是替换，不是叠加
// ---------------------------------------------------------------------------
NETMECH_TEST(udp_timer_replaces_previous)
{
    std::error_code ec;
    auto ep = make_endpoint(ec);
    REQUIRE(!ec);

    std::atomic<int> a{0};
    std::atomic<int> b{0};
    auto owner = std::make_shared<int>(0);

    ep->set_timer(15ms, owner, [&a] { a.fetch_add(1); });
    CHECK(wait_value(a, 2, 2000ms));

    ep->set_timer(15ms, owner, [&b] { b.fetch_add(1); });   // 替换
    const int a_at_switch = a.load();
    CHECK(wait_value(b, 2, 2000ms));

    std::this_thread::sleep_for(80ms);
    // 旧回调不应再有明显增长（允许替换瞬间的一次竞态）
    CHECK(a.load() - a_at_switch <= 1);
    CHECK(b.load() >= 2);

    ep->close();
}

// ---------------------------------------------------------------------------
// 3. 端点关闭后定时器自动停止
// ---------------------------------------------------------------------------
NETMECH_TEST(udp_timer_stops_on_endpoint_close)
{
    std::error_code ec;
    auto ep = make_endpoint(ec);
    REQUIRE(!ec);

    std::atomic<int> ticks{0};
    auto owner = std::make_shared<int>(0);
    ep->set_timer(15ms, owner, [&ticks] { ticks.fetch_add(1); });
    CHECK(wait_value(ticks, 2, 2000ms));

    ep->close();

    // 等一拍，让 close 的收尾跑完
    std::this_thread::sleep_for(80ms);
    const int frozen = ticks.load();
    std::this_thread::sleep_for(150ms);
    CHECK_EQ(ticks.load(), frozen);
}

// ---------------------------------------------------------------------------
// 4. owner 失效 → 自动停表
// ---------------------------------------------------------------------------
NETMECH_TEST(udp_timer_stops_when_owner_gone)
{
    std::error_code ec;
    auto ep = make_endpoint(ec);
    REQUIRE(!ec);

    std::atomic<int> ticks{0};
    {
        auto owner = std::make_shared<int>(42);
        ep->set_timer(15ms, owner, [&ticks] { ticks.fetch_add(1); });
        CHECK(wait_value(ticks, 2, 2000ms));
    }   // owner 在此销毁

    std::this_thread::sleep_for(120ms);
    const int frozen = ticks.load();
    std::this_thread::sleep_for(150ms);
    CHECK_EQ(ticks.load(), frozen);

    ep->close();
}

// ---------------------------------------------------------------------------
// 5. find_free_port_run：起点对齐到偶数，且两个端口连续
// ---------------------------------------------------------------------------
NETMECH_TEST(find_free_port_run_returns_aligned_pair)
{
    auto base = find_free_port_run(30000, 40000, 2, 2);
    REQUIRE(base.has_value());
    CHECK_EQ((*base % 2u), 0u);          // 偶数
    CHECK(base.value() >= 30000);
    CHECK(base.value() < 40000);

    // 探出来的端口确实能被绑定
    UdpConfig cfg;
    cfg.bind = Address("127.0.0.1", *base);
    std::error_code ec;
    auto ep = create_udp_endpoint(cfg, std::make_shared<NullSink>(), nullptr, ec);
    CHECK_MSG(!ec, ("探测出的端口绑不上: " + ec.message()).c_str());
    if (ep) {
        CHECK_EQ(ep->local_address().port(), *base);
        ep->close();
    }
}

// ---------------------------------------------------------------------------
// 6. find_free_port_run：范围不足时返回 nullopt
// ---------------------------------------------------------------------------
NETMECH_TEST(find_free_port_run_rejects_impossible_range)
{
    CHECK(!find_free_port_run(40000, 40001, 4, 1).has_value());   // 只要 4 个，范围只有 2
    CHECK(!find_free_port_run(40000, 40000, 2, 1).has_value());   // 范围只有 1
    CHECK(!find_free_port_run(0, 0, 2, 2).has_value());
    CHECK(!find_free_port_run(41000, 40000, 2, 2).has_value());   // lo > hi
}

// ---------------------------------------------------------------------------
// 7. create_udp_endpoint_on：两个端点与它们的定时回调在**同一线程**
//
//    这是 rtpmech 的前提：RTP / RTCP 两个端点 + RTCP 定时器必须串行，
//    否则 tracker / scheduler 就要加锁。
// ---------------------------------------------------------------------------
NETMECH_TEST(create_udp_endpoint_on_shares_io_thread)
{
    std::error_code ec;
    auto rtp = make_endpoint(ec);
    REQUIRE(!ec);

    UdpConfig rtcp_cfg;
    rtcp_cfg.bind = Address("127.0.0.1", 0);
    auto rtcp = create_udp_endpoint_on(*rtp, rtcp_cfg,
                                       std::make_shared<NullSink>(), nullptr, ec);
    REQUIRE(!ec);
    REQUIRE(rtcp != nullptr);

    std::atomic<std::thread::id> id_rtp{};
    std::atomic<std::thread::id> id_rtcp{};
    std::atomic<bool> got_rtp{false};
    std::atomic<bool> got_rtcp{false};

    auto owner = std::make_shared<int>(0);
    rtp->set_timer(10ms, owner, [&] {
        id_rtp.store(std::this_thread::get_id());
        got_rtp.store(true);
    });
    rtcp->set_timer(10ms, owner, [&] {
        id_rtcp.store(std::this_thread::get_id());
        got_rtcp.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + 3000ms;
    while (std::chrono::steady_clock::now() < deadline
           && !(got_rtp.load() && got_rtcp.load())) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(got_rtp.load());
    REQUIRE(got_rtcp.load());

    // 两者在同一个 io 线程上
    CHECK(id_rtp.load() == id_rtcp.load());
    // 且都不是调用线程（说明确实投递到了 io 线程）
    CHECK(id_rtp.load() != std::this_thread::get_id());

    rtp->close();
    rtcp->close();
}

// ---------------------------------------------------------------------------
// 8. send_to 从非 io 线程调用：dispatch 过去后仍能送达
// ---------------------------------------------------------------------------
NETMECH_TEST(udp_send_from_foreign_thread_delivers)
{
    std::error_code ec;

    // 接收端
    struct Sink : UdpHandler {
        void on_read(UdpEndpoint&, std::error_code, Buf, std::size_t n,
                     const Address&) override
        {
            got.fetch_add(1);
            last_size.store(n);
        }
        std::atomic<int>         got{0};
        std::atomic<std::size_t> last_size{0};
    };
    auto sink = std::make_shared<Sink>();

    UdpConfig rx_cfg;
    rx_cfg.bind = Address("127.0.0.1", 0);
    auto rx = create_udp_endpoint(rx_cfg, sink, nullptr, ec);
    REQUIRE(!ec);

    // 发送端：跟随接收端的 io（也可以不跟随，这里只为确定性）
    UdpConfig tx_cfg;
    tx_cfg.bind = Address("127.0.0.1", 0);
    auto tx = create_udp_endpoint_on(*rx, tx_cfg, std::make_shared<NullSink>(), nullptr, ec);
    REQUIRE(!ec);

    // 主线程（非 io 线程）发送
    const char payload[] = "dispatch-me";
    CHECK(tx->send_to(rx->local_address(), {nullptr, payload, sizeof(payload) - 1}));

    const auto deadline = std::chrono::steady_clock::now() + 3000ms;
    while (std::chrono::steady_clock::now() < deadline && sink->got.load() == 0) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK_EQ(sink->got.load(), 1);
    CHECK_EQ(sink->last_size.load(), sizeof(payload) - 1);
    CHECK_EQ(tx->datagrams_send_failed(), 0u);

    rx->close();
    tx->close();
}
