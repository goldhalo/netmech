// ============================================================================
//  test_conn_regress.cpp —— 历史缺陷回归（按新接口重写）
//
//  原 test/legacy/test_review_fixes.cpp 与 test_tcp_leak.cpp 里的用例，
//  覆盖的是 B-xx / A-xx 那批曾经踩过的坑。这里保留测试意图、按新接口重写：
//
//    1. close_all 之后能立刻重新 listen 同一端口（TIME_WAIT 不能挡住重启）
//    2. 端口 0 多次 listen 时，local_address_of 返回**最近一次**的绑定
//    3. 连接出错时，每笔已受理的 write **恰好一次**回调（不能多也不能少）
//    4. 反复创建/销毁 Mechanism，handler 必须全部析构（无泄漏）
//
//  注：legacy 里的 test_protocol_abstraction.cpp 测的是内部 protocol_traits，
//  该头已移入 src/，不再由公共测试覆盖，故不重写。
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

/// 最简 handler：只读不写。
class QuietHandler final : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        ++opened;
        return ep->read({nullptr, m_rx.data(), m_rx.size()});
    }
    void on_read(std::error_code ec, Buf, std::size_t) override
    {
        if (ec) {
            return;
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
    std::array<std::uint8_t, 256> m_rx{};
    std::atomic<int>              opened{0};
    std::atomic<int>              closed{0};
};

class QuietServer final : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_accept(Conn&) override
    {
        auto h = std::make_shared<QuietHandler>();
        last = h;
        return h;
    }
    std::shared_ptr<QuietHandler> last;
};

class QuietClient final : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        auto h = std::make_shared<QuietHandler>();
        last = h;
        weak = h;
        return h;
    }
    std::shared_ptr<QuietHandler> last;
    std::weak_ptr<QuietHandler>   weak;
};

} // namespace

// ---------------------------------------------------------------------------
// 1. close_all 之后可以立刻重新 listen 同一端口
//
//    历史问题：监听 socket 关闭后端口若进 TIME_WAIT，重启就 bind 不上。
//    现在靠 SO_REUSEADDR + 监听端的关闭语义解决。
// ---------------------------------------------------------------------------
NETMECH_TEST(regress_relisten_same_port_after_close_all)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<QuietServer>();
    REQUIRE(!mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv)));
    const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
    REQUIRE(bound.has_value());
    REQUIRE(bound->port() != 0);

    mech->close_all();

    // 立刻用同一个端口重新监听（不给 TIME_WAIT 任何缓冲时间）
    const std::error_code ec = mech->listen(*bound, std::weak_ptr<Linkable>(srv));
    CHECK_MSG(!ec, ("重新 listen 同一端口失败: " + ec.message()).c_str());

    const auto again = mech->local_address_of(*bound);
    REQUIRE(again.has_value());
    CHECK_EQ(again->port(), bound->port());

    mech->close_all();
    mech.reset();
}

// ---------------------------------------------------------------------------
// 2. 端口 0 的多次 listen：local_address_of 返回最近一次
//
//    历史问题：端口 0 每次分配新端口，若用入参地址做键会互相覆盖。
// ---------------------------------------------------------------------------
NETMECH_TEST(regress_local_address_of_returns_latest_ephemeral)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<QuietServer>();
    const Address wild("127.0.0.1", 0);

    REQUIRE(!mech->listen(wild, std::weak_ptr<Linkable>(srv)));
    const auto first = mech->local_address_of(wild);
    REQUIRE(first.has_value());

    REQUIRE(!mech->listen(wild, std::weak_ptr<Linkable>(srv)));
    const auto second = mech->local_address_of(wild);
    REQUIRE(second.has_value());

    // 两次应当是不同端口，且查询返回的是**后者**
    CHECK(first->port() != second->port());
    CHECK_EQ(mech->local_address_of(wild)->port(), second->port());

    mech->close_all();
    mech.reset();
}

// ---------------------------------------------------------------------------
// 3. 连接出错时，每笔已受理的 write 恰好回调一次
//
//    历史问题（B-15 / A-03）：队列里的写在断链时被静默丢弃，
//    或者同一笔被回调两次（队长判据失效导致双飞行）。
// ---------------------------------------------------------------------------
namespace {

/// 客户端：一口气提交 N 笔大块写，然后对端强断，统计回调次数。
class BurstThenFail : public ConnHandler {
public:
    static constexpr int kCount = 8;
    static constexpr std::size_t kSize = 16 * 1024;

    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        m_block.resize(kSize, 0x5A);
        for (int i = 0; i < kCount; ++i) {
            if (ep->write({nullptr, m_block.data(), m_block.size()})) {
                accepted.fetch_add(1);
            }
        }
        return true;   // 不读
    }

    void on_read(std::error_code, Buf, std::size_t) override {}
    void on_write(std::error_code ec, ConstBuf, std::size_t) override
    {
        if (ec) {
            failed.fetch_add(1);
        } else {
            succeeded.fetch_add(1);
        }
        callbacks.fetch_add(1);
    }
    void on_close() override { closed.fetch_add(1); }

    std::shared_ptr<Conn>  m_ep;
    std::vector<std::uint8_t> m_block;
    std::atomic<int>       accepted{0};
    std::atomic<int>       callbacks{0};
    std::atomic<int>       succeeded{0};
    std::atomic<int>       failed{0};
    std::atomic<int>       closed{0};
};

class FailClient final : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        auto h = std::make_shared<BurstThenFail>();
        last = h;
        return h;
    }
    std::shared_ptr<BurstThenFail> last;
};

/// 连上就断的 handler（触发对端的写在途失败）。
class SnapConn final : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        ep->close();
        return true;
    }
    void on_read(std::error_code, Buf, std::size_t) override {}
    void on_write(std::error_code, ConstBuf, std::size_t) override {}
    void on_close() override {}
};

/// 服务端：连上就断。
class SnapServer final : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_accept(Conn&) override
    {
        return std::make_shared<SnapConn>();
    }
};

} // namespace

NETMECH_TEST(regress_write_callbacks_exactly_once_on_failure)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<SnapServer>();
    REQUIRE(!mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv), 128));
    const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<FailClient>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));
    REQUIRE(wait_until([&] { return cli->last != nullptr; }, 3000ms));

    // 等连接彻底关闭
    REQUIRE(wait_until([&] { return cli->last->closed.load() == 1; }, 5000ms));
    std::this_thread::sleep_for(100ms);   // 给在途回调一点收尾时间

    const int acc = cli->last->accepted.load();
    REQUIRE(acc > 0);

    // ★ 契约：受理几笔就回调几笔，且每次回调只属于一笔
    CHECK_EQ(cli->last->callbacks.load(), acc);
    // 成功 + 失败 应当等于总回调数（没有第三类）
    CHECK_EQ(cli->last->succeeded.load() + cli->last->failed.load(), acc);
    CHECK_EQ(cli->last->closed.load(), 1);

    mech->close_all();
    mech.reset();
}

// ---------------------------------------------------------------------------
// 4. 反复创建/销毁 Mechanism：handler 必须全部析构（无泄漏）
//
//    历史问题（test_tcp_leak）：每轮结束后仍有 handler 被库强引用住，
//    对象越攒越多。这里用 weak_ptr 直接观察析构。
// ---------------------------------------------------------------------------
NETMECH_TEST(regress_no_handler_leak_across_cycles)
{
    constexpr int kRounds = 30;
    std::vector<std::weak_ptr<QuietHandler>> watched;
    watched.reserve(kRounds);

    for (int i = 0; i < kRounds; ++i) {
        auto mech = Mechanism::create();
        REQUIRE(mech != nullptr);

        auto srv = std::make_shared<QuietServer>();
        REQUIRE(!mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv), 128));
        const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
        REQUIRE(bound.has_value());

        auto cli = std::make_shared<QuietClient>();
        REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));
        REQUIRE(wait_until([&] { return cli->last && cli->last->opened.load() == 1; }, 3000ms));

        cli->last->m_ep->close();
        REQUIRE(wait_until([&] { return cli->last->closed.load() == 1; }, 3000ms));

        watched.push_back(cli->weak);   // 只留弱引用

        mech->close_all();
        mech.reset();
    }

    // 所有轮次的 handler 都应当已经析构
    const bool all_released = wait_until(
        [&] {
            for (const auto& w : watched) {
                if (!w.expired()) {
                    return false;
                }
            }
            return true;
        },
        5000ms);

    CHECK_MSG(all_released, "有 handler 在 Mechanism 销毁后仍未释放");
}
