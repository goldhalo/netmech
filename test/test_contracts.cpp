// ============================================================================
//  test_contracts.cpp —— 头文件承诺的"契约测试"
//
//  这些是**对外承诺**（iohandler.hpp / conn.hpp / runtime.hpp 里写明的行为），
//  单独成文件，防止以后实现改动时静默破约。
//
//  当前覆盖：
//    * on_open 返回 false（拒绝接管）时，on_open 里**已受理**的 read/write
//      仍必须各回调一次（error），但 on_close **不**调用 —— 这就是
//      `retired_handler` 快照存在的理由。
//    * 上面这条在"拒绝接管后立刻关闭连接"的路径上同样成立（不能静默丢通知）。
// ============================================================================
#include <netmech/net.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>

#include "test_framework.hpp"
#include "test_support.hpp"

using namespace netmech;
using namespace netmech_test;

namespace {

using namespace std::chrono_literals;

/// 在 on_open 里先受理读+写、然后拒绝接管。
/// 库必须：把这两笔以错误各回调一次，且**不**调 on_close。
struct RejectOnOpen final : ConnHandler {
    std::shared_ptr<Conn>         ep;
    std::array<std::uint8_t, 64>  rx{};
    std::array<std::uint8_t, 4>   tx{{'a', 'b', 'c', 'd'}};
    std::atomic<int>              reads{0};
    std::atomic<int>              writes{0};
    std::atomic<int>              closes{0};

    bool on_open(std::shared_ptr<Conn> c) override
    {
        ep = c;
        (void)c->read(Buf{nullptr, rx.data(), rx.size()});
        (void)c->write(ConstBuf{nullptr, tx.data(), tx.size()});
        return false;   // 拒绝接管：库应立即关闭连接
    }
    void on_read(std::error_code ec, Buf, std::size_t) override
    {
        if (ec) {
            reads.fetch_add(1);
        }
    }
    void on_write(std::error_code ec, ConstBuf, std::size_t) override
    {
        if (ec) {
            writes.fetch_add(1);
        }
    }
    void on_close() override { closes.fetch_add(1); }
};

struct RejectLink final : Linkable {
    std::shared_ptr<RejectOnOpen> handler;

    std::shared_ptr<ConnHandler> on_accept(Conn&) override
    {
        handler = std::make_shared<RejectOnOpen>();
        return handler;
    }
};

/// 客户端：连上后什么都不做（保持连接直到服务端关闭）。
struct IdleClient final : ConnHandler {
    std::shared_ptr<Conn>        ep;
    std::array<std::uint8_t, 32> rx{};
    bool on_open(std::shared_ptr<Conn> c) override
    {
        ep = c;
        return c->read(Buf{nullptr, rx.data(), rx.size()});
    }
    void on_read(std::error_code, Buf, std::size_t) override {}
    void on_write(std::error_code, ConstBuf, std::size_t) override {}
    void on_close() override { ep.reset(); }
};

struct IdleClientLink final : Linkable {
    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        return std::make_shared<IdleClient>();
    }
    void on_linkfail(const Address&, std::error_code) override {}
};

} // namespace

// ---------------------------------------------------------------------------
// on_open 拒绝接管：已受理的 read/write 各回调一次（error），on_close 不调用
// ---------------------------------------------------------------------------
NETMECH_TEST(contract_reject_on_open_still_completes_accepted_io)
{
    auto srv = Mechanism::create();
    REQUIRE(srv != nullptr);

    auto lb = std::make_shared<RejectLink>();

    // 通过机制自己的 listen 拿到真实端口（端口 0 由系统分配）
    const Address req{"127.0.0.1", 0};
    REQUIRE(!srv->listen(req, lb, 8));
    const auto bound = srv->listening_address(req);
    REQUIRE(bound.has_value());

    auto cli = Mechanism::create();
    REQUIRE(cli != nullptr);
    auto cli_lb = std::make_shared<IdleClientLink>();
    REQUIRE(!cli->connect(Address{"127.0.0.1", bound->port()}, cli_lb));

    // 等服务端 on_open 跑完、库把两笔以错误归还
    REQUIRE(wait_for([&] {
        return lb->handler != nullptr && lb->handler->reads.load() == 1
               && lb->handler->writes.load() == 1;
    }, 3000ms));

    CHECK_EQ(lb->handler->reads.load(), 1);    // 已受理的读：恰好一次（带错误）
    CHECK_EQ(lb->handler->writes.load(), 1);   // 已受理的写：恰好一次（带错误）

    // on_open 返回 false ⇒ 不配平 on_close（文档承诺）
    std::this_thread::sleep_for(150ms);
    CHECK_EQ(lb->handler->closes.load(), 0);

    cli->close_all();
    srv->close_all();
}

// ---------------------------------------------------------------------------
// 对端关闭：在途读以错误归还一次，on_close 恰好一次（不重复通知）
// ---------------------------------------------------------------------------
namespace {

struct InflightHandler final : ConnHandler {
    std::shared_ptr<Conn>        ep;
    std::array<std::uint8_t, 64> rx{};
    std::atomic<int>             reads{0};
    std::atomic<int>             errors{0};
    std::atomic<int>             closes{0};

    bool on_open(std::shared_ptr<Conn> c) override
    {
        ep = c;
        return c->read(Buf{nullptr, rx.data(), rx.size()});
    }
    void on_read(std::error_code ec, Buf, std::size_t) override
    {
        reads.fetch_add(1);
        if (ec) {
            errors.fetch_add(1);
        }
    }
    void on_write(std::error_code, ConstBuf, std::size_t) override {}
    void on_close() override
    {
        closes.fetch_add(1);
        ep.reset();
    }
};

struct InflightLink final : Linkable {
    std::shared_ptr<InflightHandler> h;

    std::shared_ptr<ConnHandler> on_accept(Conn&) override
    {
        h = std::make_shared<InflightHandler>();
        return h;
    }
};

/// 客户端：连上就主动关闭（制造"服务端在途读被打断"）。
struct CloseOnOpen final : ConnHandler {
    bool on_open(std::shared_ptr<Conn> c) override
    {
        c->close();
        return true;
    }
    void on_read(std::error_code, Buf, std::size_t) override {}
    void on_write(std::error_code, ConstBuf, std::size_t) override {}
    void on_close() override {}
};

struct CloseOnOpenLink final : Linkable {
    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        return std::make_shared<CloseOnOpen>();
    }
    void on_linkfail(const Address&, std::error_code) override {}
};

} // namespace

NETMECH_TEST(contract_close_returns_inflight_read_and_notifies_once)
{
    auto srv = Mechanism::create();
    REQUIRE(srv != nullptr);
    auto lb = std::make_shared<InflightLink>();
    const Address req{"127.0.0.1", 0};
    REQUIRE(!srv->listen(req, lb, 8));
    const auto bound = srv->listening_address(req);
    REQUIRE(bound.has_value());

    auto cli = Mechanism::create();
    REQUIRE(cli != nullptr);
    auto cli_lb = std::make_shared<CloseOnOpenLink>();
    REQUIRE(!cli->connect(Address{"127.0.0.1", bound->port()}, cli_lb));

    REQUIRE(wait_for([&] {
        return lb->h != nullptr && lb->h->errors.load() >= 1 && lb->h->closes.load() >= 1;
    }, 3000ms));

    CHECK_EQ(lb->h->errors.load(), 1);   // 在途读：恰好一次（带错误）
    CHECK_EQ(lb->h->closes.load(), 1);   // on_close：恰好一次

    std::this_thread::sleep_for(150ms);
    CHECK_EQ(lb->h->errors.load(), 1);   // 不重复通知
    CHECK_EQ(lb->h->closes.load(), 1);

    cli->close_all();
    srv->close_all();
}
