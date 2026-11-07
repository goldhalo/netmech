// ============================================================================
//  test_runtime.cpp —— 可注入 io 运行时（Runtime）的验证
//
//  覆盖：
//    * Runtime::create(io_threads) 由创建者决定线程数；shutdown 幂等
//    * shutdown 之后新 listen/connect 返回 not_initialized（与默认运行时一致）
//    * Mechanism::create(cfg, rt) 真的用注入的池（功能验证：跨池 echo）
//    * 两个 Runtime 互不影响：关掉一个，另一个照常工作
//    * Mechanism 持有 Runtime 的强引用：宿主先放手也安全
//
//  为什么单独一组：多 .so 进程里每个 .so 各有一份 `IoPool::global()`
//  静态数据（默认运行时 = 每个 .so 一套线程池）。宿主唯一 Runtime 注入是
//  "共用一套线程池"的正解 —— 这组用例把它钉住。
// ============================================================================
#include <netmech/net.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

#include "test_framework.hpp"
#include "test_support.hpp"

using namespace netmech;
using namespace netmech_test;

namespace {

using namespace std::chrono_literals;

Runtime::Options opts_with_threads(std::size_t n)
{
    Runtime::Options o;
    o.io_threads = n;
    return o;
}

/// 服务端：收到什么原样写回。
struct EchoServerHandler final : ConnHandler {
    std::shared_ptr<Conn>      ep;
    std::array<std::uint8_t, 256> rx{};

    bool on_open(std::shared_ptr<Conn> c) override
    {
        ep = c;
        return c->read(Buf{nullptr, rx.data(), rx.size()});
    }
    void on_read(std::error_code ec, Buf b, std::size_t n) override
    {
        if (ec) {
            return;
        }
        (void)ep->write(ConstBuf{nullptr, b.data, n});
        (void)ep->read(Buf{nullptr, rx.data(), rx.size()});
    }
    void on_write(std::error_code, ConstBuf, std::size_t) override {}
    void on_close() override { ep.reset(); }
};

struct EchoServerLink final : Linkable {
    std::shared_ptr<ConnHandler> on_accept(Conn&) override
    {
        return std::make_shared<EchoServerHandler>();
    }
};

/// 客户端：连上就发 "ping"，统计收到的字节数。
struct ClientHandler final : ConnHandler {
    std::shared_ptr<Conn>      ep;
    std::array<std::uint8_t, 256> rx{};
    /// 写缓冲必须是**成员**：Conn::write 不拷贝，buf 要活到 on_write 返回。
    std::array<std::uint8_t, 4> tx{{'p', 'i', 'n', 'g'}};
    std::atomic<int>           received{0};

    bool on_open(std::shared_ptr<Conn> c) override
    {
        ep = c;
        (void)c->read(Buf{nullptr, rx.data(), rx.size()});
        (void)c->write(ConstBuf{nullptr, tx.data(), tx.size()});
        return true;
    }
    void on_read(std::error_code ec, Buf, std::size_t n) override
    {
        if (!ec) {
            received.fetch_add(static_cast<int>(n));
        }
    }
    void on_write(std::error_code, ConstBuf, std::size_t) override {}
    void on_close() override { ep.reset(); }
};

struct ClientLink final : Linkable {
    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        auto handler = std::make_shared<ClientHandler>();
        {
            std::lock_guard<std::mutex> lk(mtx);
            h = handler;
        }
        return handler;
    }
    void on_linkfail(const Address&, std::error_code) override {}

    std::shared_ptr<ClientHandler> handler()
    {
        std::lock_guard<std::mutex> lk(mtx);
        return h;
    }

    std::mutex                     mtx;
    std::shared_ptr<ClientHandler> h;
};

} // namespace

// ---------------------------------------------------------------------------
// 1. 线程数由创建者决定；shutdown 幂等
// ---------------------------------------------------------------------------
NETMECH_TEST(runtime_create_decides_thread_count)
{
    auto rt = Runtime::create(opts_with_threads(2));
    REQUIRE(rt != nullptr);
    CHECK_EQ(rt->io_threads(), std::size_t{2});
    CHECK(rt->running());

    rt->shutdown();
    CHECK(!rt->running());

    rt->shutdown();   // 幂等：重复调用无副作用
    CHECK(!rt->running());
}

// ---------------------------------------------------------------------------
// 2. shutdown 之后：新的 listen/connect 返回 not_initialized（契约与默认运行时一致）
// ---------------------------------------------------------------------------
NETMECH_TEST(runtime_shutdown_blocks_new_operations)
{
    auto rt = Runtime::create(opts_with_threads(2));
    REQUIRE(rt != nullptr);

    auto mech = Mechanism::create(Mechanism::Config{}, rt);
    REQUIRE(mech != nullptr);

    rt->shutdown();

    auto lb = std::make_shared<EchoServerLink>();
    const auto ec = mech->listen(Address::any(0), lb);
    CHECK(ec == NetErrc::not_initialized);

    // 已存在对象仍可安全析构（context 与进程同寿，不会悬垂）
    CHECK(mech->endpoint_count() == 0u);
}

// ---------------------------------------------------------------------------
// 3. 注入的运行时上真的能收发：服务端在自定义 Runtime，客户端在默认 Runtime
// ---------------------------------------------------------------------------
NETMECH_TEST(mechanism_on_injected_runtime_echoes)
{
    auto rt = Runtime::create(opts_with_threads(2));
    REQUIRE(rt != nullptr);

    auto srv = Mechanism::create(Mechanism::Config{}, rt);
    REQUIRE(srv != nullptr);

    auto srv_lb = std::make_shared<EchoServerLink>();
    const Address req{"127.0.0.1", 0};
    const auto ec = srv->listen(req, srv_lb, 8);
    REQUIRE(!ec);

    const auto bound = srv->listening_address(req);
    REQUIRE(bound.has_value());
    CHECK(bound->port() != 0);

    // 客户端用**默认运行时**（另一个池）—— 跨池 TCP 连接应当正常
    auto cli = Mechanism::create();
    REQUIRE(cli != nullptr);
    auto cli_lb = std::make_shared<ClientLink>();
    const auto cec = cli->connect(Address{"127.0.0.1", bound->port()}, cli_lb);
    REQUIRE(!cec);

    REQUIRE(wait_for([&] { return cli_lb->handler() != nullptr; }, 3000ms));
    auto ch = cli_lb->handler();
    REQUIRE(ch != nullptr);
    REQUIRE(wait_for([&] { return ch->received.load() >= 4; }, 3000ms));
    CHECK_EQ(ch->received.load(), 4);

    cli->close_all();
    srv->close_all();
    rt->shutdown();
}

// ---------------------------------------------------------------------------
// 4. 两个 Runtime 互不影响：关掉一个，另一个的机制照常工作
// ---------------------------------------------------------------------------
NETMECH_TEST(two_runtimes_are_isolated)
{
    auto rt1 = Runtime::create(opts_with_threads(1));
    auto rt2 = Runtime::create(opts_with_threads(1));
    REQUIRE(rt1 != nullptr);
    REQUIRE(rt2 != nullptr);

    auto m1 = Mechanism::create(Mechanism::Config{}, rt1);
    auto m2 = Mechanism::create(Mechanism::Config{}, rt2);
    REQUIRE(m1 != nullptr);
    REQUIRE(m2 != nullptr);

    auto lb1 = std::make_shared<EchoServerLink>();
    auto lb2 = std::make_shared<EchoServerLink>();

    const Address req{"127.0.0.1", 0};
    REQUIRE(!m1->listen(req, lb1, 4));
    REQUIRE(!m2->listen(req, lb2, 4));

    rt1->shutdown();   // 只停第一套

    CHECK(!rt1->running());
    CHECK(rt2->running());

    // 第一个机制的新 listen 被拒；第二个照常
    CHECK(m1->listen(Address::any(0), lb1) == NetErrc::not_initialized);
    CHECK(!m2->listen(Address::any(0), lb2));

    m1.reset();
    m2->close_all();
    m2.reset();
    rt2->shutdown();
}

// ---------------------------------------------------------------------------
// 5. Mechanism 持有 Runtime 的强引用：宿主先放手，Mechanism 仍安全
// ---------------------------------------------------------------------------
NETMECH_TEST(mechanism_keeps_runtime_alive)
{
    std::weak_ptr<Runtime> weak;
    std::shared_ptr<Mechanism> mech;

    {
        auto rt = Runtime::create(opts_with_threads(1));
        REQUIRE(rt != nullptr);
        weak = rt;

        mech = Mechanism::create(Mechanism::Config{}, rt);
        REQUIRE(mech != nullptr);

        rt.reset();   // 宿主放手
    }

    // Mechanism 仍持有运行时 → 池还在跑，还能用
    CHECK(!weak.expired());
    if (auto rt = weak.lock()) {
        CHECK(rt->running());
    }

    auto lb = std::make_shared<EchoServerLink>();
    CHECK(!mech->listen(Address{"127.0.0.1", 0}, lb, 4));

    mech.reset();   // 最后一个使用者释放 → 运行时析构（幂等 shutdown）
    CHECK(weak.expired());
}
