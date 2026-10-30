// ============================================================================
//  test_conn_lifetime.cpp —— 连接生命周期与通知收敛
//
//  验证三条地基级契约：
//    1. **强自持**：用户丢掉自己的 shared_ptr<Conn> 之后，连接仍活到最后一个
//       IO 完成，on_close / on_linkdown 照常到达（通知不会丢）。
//    2. **handler 释放时机**：库在 on_close 返回之后才释放 handler。
//    3. **收敛**：反复建连/断连之后，Mechanism::endpoint_count() 回落到 0。
// ============================================================================
#include <netmech/net.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "test_framework.hpp"

using namespace netmech;
using namespace netmech_test;

namespace {

using namespace std::chrono_literals;

bool wait_until(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

/// 最小 handler：记录事件，不做 IO。
class ProbeConn final : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        ++opened;
        // ★ 必须保持一笔读在途：没有读操作时，对端发来的 FIN 不会被感知，
        //   连接会静默挂着（TCP 的固有行为，不是库的问题）。
        return ep->read({nullptr, m_rx.data(), m_rx.size()});
    }

    void on_read(std::error_code ec, Buf, std::size_t) override
    {
        if (ec) {
            return;   // 对端关闭 / 本端 close：连接随其后关闭
        }
        (void)m_ep->read({nullptr, m_rx.data(), m_rx.size()});
    }

    void on_write(std::error_code, ConstBuf, std::size_t) override {}

    void on_close() override
    {
        ++closed;
        m_ep.reset();
    }

    std::shared_ptr<Conn>         m_ep;
    std::array<std::uint8_t, 128> m_rx{};
    std::atomic<int>              opened{0};
    std::atomic<int>              closed{0};
};

/// 服务端：保存连接句柄，供测试主动关闭。
class ServerCtl final : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_accept(Conn&) override
    {
        auto h = std::make_shared<ProbeConn>();
        last = h;
        return h;
    }
    std::shared_ptr<ProbeConn> last;
};

/// 客户端：同时记录 handler 的弱引用，用于观察释放时机。
class ClientCtl final : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        auto h = std::make_shared<ProbeConn>();
        last = h;
        weak = h;
        return h;
    }
    void on_linkdown(Conn&) override { linked_down.fetch_add(1); }

    std::shared_ptr<ProbeConn> last;
    std::weak_ptr<ProbeConn>   weak;
    std::atomic<int>           linked_down{0};
};

} // namespace

// ---------------------------------------------------------------------------
// 1. 用户丢掉 Conn 之后，on_close 与 on_linkdown 仍然到达
// ---------------------------------------------------------------------------
NETMECH_TEST(conn_notifies_after_user_releases)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<ServerCtl>();
    REQUIRE(!mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv)));
    const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<ClientCtl>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));
    REQUIRE(wait_until([&] { return cli->last && cli->last->opened.load() == 1; }, 3000ms));
    REQUIRE(wait_until([&] { return srv->last && srv->last->opened.load() == 1; }, 3000ms));

    // ★ 用户彻底放手：把自己持有的那唯一一份 shared_ptr<Conn> 释放掉
    cli->last->m_ep.reset();

    // 对端主动关闭
    srv->last->m_ep->close();

    // 通知仍然要到达（强自持：在途 IO 完成处理捕获了 Conn 的强引用）
    CHECK(wait_until([&] { return cli->last->closed.load() == 1; }, 3000ms));
    CHECK(wait_until([&] { return cli->linked_down.load() == 1; }, 3000ms));

    mech->close_all();
    mech.reset();
}

// ---------------------------------------------------------------------------
// 2. handler 在 on_close 之后才由库释放
// ---------------------------------------------------------------------------
NETMECH_TEST(handler_released_after_close)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<ServerCtl>();
    REQUIRE(!mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv)));
    const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<ClientCtl>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));
    REQUIRE(wait_until([&] { return cli->last && cli->last->opened.load() == 1; }, 3000ms));

    // 库持有 handler 期间，它必须活着（即使测试侧丢掉自己的 shared_ptr）
    std::weak_ptr<ProbeConn> weak = cli->last;
    cli->last.reset();
    CHECK(!weak.expired());   // 库仍强持有

    // 关闭连接 → on_close 之后库释放 handler
    srv->last->m_ep->close();

    CHECK(wait_until([&] { return weak.expired(); }, 3000ms));

    mech->close_all();
    mech.reset();
}

// ---------------------------------------------------------------------------
// 3. 反复建连/断连后连接表回落，不残留
// ---------------------------------------------------------------------------
NETMECH_TEST(repeated_connect_close_converges)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<ServerCtl>();
    REQUIRE(!mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv)));
    const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
    REQUIRE(bound.has_value());

    constexpr int kRounds = 30;
    for (int i = 0; i < kRounds; ++i) {
        auto cli = std::make_shared<ClientCtl>();
        REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));
        REQUIRE(wait_until([&] { return cli->last && cli->last->opened.load() == 1; }, 3000ms));

        // 客户端主动断
        cli->last->m_ep->close();
        REQUIRE(wait_until([&] { return cli->last->closed.load() == 1; }, 3000ms));
    }

    // 全部收敛后，活跃连接数应回落到 0（服务端侧也是）
    CHECK(wait_until([&] { return mech->endpoint_count() == 0; }, 5000ms));

    mech->close_all();
    mech.reset();
}
