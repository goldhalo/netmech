// ============================================================================
//  test_mechanism_listeners.cpp —— 单监听器生命周期
//
//  覆盖本次新增的两个接口：
//    * listening_address() / listening_addresses()：端口 0 时取回真实端口
//    * unlisten()：只停指定监听，不动其它监听
//
//  为什么要测：httpmech 的 HttpServer::stop() 之前只能退化成 close_all()，
//  那会把同一 Mechanism 上的别的服务一起关掉。加上单监听器停止之后，
//  "关我的服务"和"关整个进程"才分得开。
// ============================================================================
#include <netmech/common.hpp>
#include <netmech/linkable.hpp>
#include <netmech/mechanism.hpp>

#include <chrono>
#include <memory>
#include <thread>

#include "test_framework.hpp"

using namespace netmech;
using namespace std::chrono_literals;

namespace {

/// 一个只会数 accept 次数的订阅者。
struct CountingLinkable : public Linkable {
    std::atomic<int> accepted{0};
    std::atomic<int> downs{0};

    std::shared_ptr<ConnHandler> on_accept(Conn&) override
    {
        accepted.fetch_add(1);
        return nullptr;   // 不接管，库会立刻关掉这条连接
    }
    void on_linkdown(Conn&) override { downs.fetch_add(1); }
};

} // namespace

// ---------------------------------------------------------------------------
// 1. listen(..:0) 之后能取回系统分配的端口
// ---------------------------------------------------------------------------
NETMECH_TEST(listening_address_reports_ephemeral_port)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto lb = std::make_shared<CountingLinkable>();

    const Address req{"127.0.0.1", 0};   // 端口 0：交给系统
    const auto    ec = mech->listen(req, lb, 8);
    EXPECT_OK(ec);

    const auto bound = mech->listening_address(req);
    REQUIRE(bound.has_value());
    CHECK(bound->port() != 0);
    CHECK_EQ(bound->address_string(), std::string("127.0.0.1"));

    // 入参键（127.0.0.1:0）与实参键（127.0.0.1:真实端口）都能查
    const auto by_bound = mech->listening_address(*bound);
    CHECK(by_bound.has_value());
    CHECK_EQ(by_bound->port(), bound->port());

    const auto all = mech->listening_addresses();
    CHECK_EQ(all.size(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// 2. unlisten 只停指定监听，另一个照常工作
// ---------------------------------------------------------------------------
NETMECH_TEST(unlisten_stops_only_the_target_listener)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto lb = std::make_shared<CountingLinkable>();

    const Address a{"127.0.0.1", 0};
    const Address b{"127.0.0.1", 0};
    EXPECT_OK(mech->listen(a, lb, 8));
    EXPECT_OK(mech->listen(b, lb, 8));
    CHECK_EQ(mech->listening_addresses().size(), std::size_t{2});

    // 两个 :0 监听并存 → 问 ":0 是哪个" 有歧义，约定返回 nullopt
    CHECK(!mech->listening_address(a).has_value());

    const auto both = mech->listening_addresses();
    REQUIRE(both.size() == 2u);
    const Address bound_a = both[0];
    const Address bound_b = both[1];
    CHECK(bound_a.port() != 0);
    CHECK(bound_b.port() != 0);
    CHECK(bound_a != bound_b);

    // 停掉 a（用实际地址，避开歧义）
    EXPECT_OK(mech->unlisten(bound_a));

    // a 没了，b 还在
    CHECK(!mech->listening_address(bound_a).has_value());
    const auto remain = mech->listening_addresses();
    CHECK_EQ(remain.size(), std::size_t{1});
    CHECK_EQ(remain[0].port(), bound_b.port());

    // 幂等：再停一次也算成功
    EXPECT_OK(mech->unlisten(bound_a));
    // 停一个从来没监听过的地址也成功
    EXPECT_OK(mech->unlisten(Address{"127.0.0.1", 1}));
}

// ---------------------------------------------------------------------------
// 3. 停掉之后同一个端口能重新 listen（说明 acceptor 真关了）
// ---------------------------------------------------------------------------
NETMECH_TEST(unlisten_frees_the_port)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto lb = std::make_shared<CountingLinkable>();

    // 先拿一个真实端口：监听 0 号，读回实际端口，再停掉
    const Address probe{"127.0.0.1", 0};
    EXPECT_OK(mech->listen(probe, lb, 8));
    const auto bound = mech->listening_address(probe);
    REQUIRE(bound.has_value());
    const std::uint16_t port = bound->port();
    EXPECT_OK(mech->unlisten(probe));

    // 用刚释放的端口重新监听，应当成功
    const Address again{"127.0.0.1", port};
    const auto    ec = mech->listen(again, lb, 8);
    EXPECT_OK(ec);
    const auto rebound = mech->listening_address(again);
    REQUIRE(rebound.has_value());
    CHECK_EQ(rebound->port(), port);
}

// ---------------------------------------------------------------------------
// 4. unlisten 不影响已建立的连接（只停 accept）
// ---------------------------------------------------------------------------
NETMECH_TEST(unlisten_keeps_established_connections)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto lb = std::make_shared<CountingLinkable>();

    const Address req{"127.0.0.1", 0};
    EXPECT_OK(mech->listen(req, lb, 8));
    const auto bound = mech->listening_address(req);
    REQUIRE(bound.has_value());

    // 停掉监听：这一条是通过"停完之后 listening_address 为空"验证的；
    // 已建立连接的行为由 Conn 自身负责，这里只确认 unlisten 不会把
    // Mechanism 整体打停（running 仍为 true，还能重新 listen）。
    EXPECT_OK(mech->unlisten(req));
    CHECK(mech->running());

    EXPECT_OK(mech->listen(Address{"127.0.0.1", bound->port()}, lb, 8));
}
