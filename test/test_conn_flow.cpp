// ============================================================================
//  test_conn_flow.cpp —— 发送反压与保活分级
//
//  验证两条新契约：
//    1. 反压：send 队列配额用尽时 write() 返回 false，**连接保持可用**，
//       且被拒的那笔不会有完成回调（与"受理即恰好一次"一致）。
//    2. 保活分级：接收保活每次检查未通过都回调 on_idle（missed 递增），
//       第 max_miss 次之后才断链，前几次连接仍然可用。
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

// ---------------------------------------------------------------------------
// 反压：on_open 里连续两笔写，配额只够一笔
// ---------------------------------------------------------------------------
class BurstWriter : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        // 只允许 1 个请求在途（bytes 不限制）。set_send_queue_limit 立即生效。
        ep->set_send_queue_limit(0, 1);

        static const std::uint8_t block[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        first_accepted  = ep->write({nullptr, block, sizeof block});
        second_accepted = ep->write({nullptr, block, sizeof block});
        open_after_reject = ep->is_open();
        return true;
    }

    void on_read(std::error_code, Buf, std::size_t) override {}
    void on_write(std::error_code, ConstBuf, std::size_t) override { ++writes; }
    void on_close() override { ++closed; }

    std::shared_ptr<Conn> m_ep;
    bool first_accepted  = false;
    bool second_accepted = false;
    bool open_after_reject = false;
    std::atomic<int> writes{0};
    std::atomic<int> closed{0};
};

class BurstFactory : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        auto h = std::make_shared<BurstWriter>();
        last = h;
        return h;
    }
    std::shared_ptr<BurstWriter> last;
};

// ---------------------------------------------------------------------------
// 保活：不读数据，等分级预警
// ---------------------------------------------------------------------------
class IdleProbe : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        // 故意不发起 read：让接收方向彻底"无活动"。
        ep->enable_recv_keepalive(0ms, 40ms, 3);
        return true;
    }

    void on_read(std::error_code, Buf, std::size_t) override {}
    void on_write(std::error_code, ConstBuf, std::size_t) override {}

    void on_idle(const IdleInfo& info) override
    {
        if (info.kind == IdleKind::recv) {
            missed_seen.push_back(info.missed);
            limits_seen.push_back(info.limit);
            if (!info.has_grace()) {
                last_call_had_grace = false;
            }
        }
    }

    void on_close() override { ++closed; }

    std::shared_ptr<Conn> m_ep;
    std::mutex                          mtx;
    std::vector<std::uint32_t>          missed_seen;
    std::vector<std::uint32_t>          limits_seen;
    bool                                last_call_had_grace = true;
    std::atomic<int>                    closed{0};
};

class IdleFactory : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        auto h = std::make_shared<IdleProbe>();
        last = h;
        return h;
    }
    std::shared_ptr<IdleProbe> last;
};

/// 服务端：接受但不读，只为让客户端的保活探测有对端。
class SilentHandler final : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn>) override { return true; }   // 故意不发起读
    void on_read(std::error_code, Buf, std::size_t) override {}
    void on_write(std::error_code, ConstBuf, std::size_t) override {}
    void on_close() override {}
};

class SilentServer : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_accept(Conn&) override
    {
        accepted.fetch_add(1);
        return std::make_shared<SilentHandler>();
    }

    std::atomic<int> accepted{0};
};

} // namespace

// ---------------------------------------------------------------------------
// 1. 反压：配额用尽 → write 返回 false，连接不断
// ---------------------------------------------------------------------------
NETMECH_TEST(backpressure_rejects_without_closing)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<SilentServer>();
    REQUIRE(!mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv)));
    const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<BurstFactory>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));

    REQUIRE(wait_until([&] { return cli->last != nullptr; }, 3000ms));
    REQUIRE(wait_until([&] { return cli->last->writes.load() >= 1; }, 3000ms));

    // 第一笔受理、第二笔被反压拒绝
    CHECK(cli->last->first_accepted);
    CHECK(!cli->last->second_accepted);

    // 被拒时连接仍然开着（反压不是错误，不断链）
    CHECK(cli->last->open_after_reject);
    CHECK(cli->last->m_ep->is_open());

    // 被拒的那笔没有完成回调：只应看到 1 次 on_write
    std::this_thread::sleep_for(100ms);
    CHECK_EQ(cli->last->writes.load(), 1);
    CHECK_EQ(cli->last->closed.load(), 0);

    mech->close_all();
    mech.reset();
}

// ---------------------------------------------------------------------------
// 2. 保活分级：missed 依次 1..max_miss，最后一次之后才断链
// ---------------------------------------------------------------------------
NETMECH_TEST(keepalive_reports_each_miss)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<SilentServer>();
    REQUIRE(!mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv)));
    const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<IdleFactory>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));
    REQUIRE(wait_until([&] { return cli->last != nullptr; }, 3000ms));

    // 3 拍 × 40ms ≈ 120ms；给足余量
    CHECK(wait_until([&] { return cli->last->closed.load() == 1; }, 5000ms));

    std::lock_guard<std::mutex> lk(cli->last->mtx);
    REQUIRE(cli->last->missed_seen.size() == 3u);
    CHECK_EQ(cli->last->missed_seen[0], 1u);
    CHECK_EQ(cli->last->missed_seen[1], 2u);
    CHECK_EQ(cli->last->missed_seen[2], 3u);
    CHECK_EQ(cli->last->limits_seen[0], 3u);
    // 最后一次没有余地
    CHECK(!cli->last->last_call_had_grace);

    mech->close_all();
    mech.reset();
}

// ---------------------------------------------------------------------------
// 3. 关闭时在途读以错误码归还，on_close 恰好一次
// ---------------------------------------------------------------------------
namespace {

class PendingReader : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        // 发起一笔读并让它一直挂着（对端不发数据）
        posted = ep->read({nullptr, m_rx.data(), m_rx.size()});
        return true;
    }

    void on_read(std::error_code ec, Buf, std::size_t) override
    {
        read_ec = ec;
        read_returned.fetch_add(1);
    }

    void on_write(std::error_code, ConstBuf, std::size_t) override {}
    void on_close() override { closed.fetch_add(1); }

    std::shared_ptr<Conn>         m_ep;
    std::array<std::uint8_t, 256> m_rx{};
    bool                          posted = false;
    std::error_code               read_ec;

    std::atomic<int> read_returned{0};
    std::atomic<int> closed{0};
};

class ReaderFactory : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        auto h = std::make_shared<PendingReader>();
        last = h;
        return h;
    }
    std::shared_ptr<PendingReader> last;
};

} // namespace

NETMECH_TEST(close_returns_inflight_read)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<SilentServer>();
    REQUIRE(!mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv)));
    const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<ReaderFactory>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));
    REQUIRE(wait_until([&] { return cli->last != nullptr; }, 3000ms));

    // 读已受理且尚未完成
    CHECK(cli->last->posted);
    CHECK_EQ(cli->last->read_returned.load(), 0);

    // 主动关闭：在途读必须以非空 ec 归还一次，随后 on_close 一次
    cli->last->m_ep->close();

    CHECK(wait_until([&] { return cli->last->read_returned.load() == 1; }, 3000ms));
    CHECK(wait_until([&] { return cli->last->closed.load() == 1; }, 3000ms));
    CHECK(static_cast<bool>(cli->last->read_ec));          // 归还时带错误码
    CHECK_EQ(cli->last->read_returned.load(), 1);          // 恰好一次
    CHECK_EQ(cli->last->closed.load(), 1);

    mech->close_all();
    mech.reset();
}

// ---------------------------------------------------------------------------
// 4. 反压之后能恢复：配额释放后新的写重新被受理
// ---------------------------------------------------------------------------
namespace {

class BackpressureRecovery : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        ep->set_send_queue_limit(0, 1);   // 只允许 1 个请求在途

        first  = ep->write({nullptr, m_blk.data(), 8});    // 受理
        second = ep->write({nullptr, m_blk.data(), 8});    // 配额用尽 → 拒绝
        return true;                                // 不读，避免干扰
    }

    void on_write(std::error_code ec, ConstBuf, std::size_t) override
    {
        if (ec) {
            ++errors;
            return;
        }
        if (writes.fetch_add(1) == 0) {
            // 第一笔完成 → 配额应当已释放 → 再来一笔必须被受理
            third = m_ep->write({nullptr, m_blk.data(), 8});
        }
    }

    void on_read(std::error_code, Buf, std::size_t) override {}
    void on_close() override { ++closed; }

    std::shared_ptr<Conn> m_ep;
    std::array<std::uint8_t, 8> m_blk{1, 2, 3, 4, 5, 6, 7, 8};

    bool first = false;
    bool second = false;
    bool third = false;
    std::atomic<int> writes{0};
    std::atomic<int> errors{0};
    std::atomic<int> closed{0};
};

class RecoveryFactory : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        auto h = std::make_shared<BackpressureRecovery>();
        last = h;
        return h;
    }
    std::shared_ptr<BackpressureRecovery> last;
};

} // namespace

NETMECH_TEST(backpressure_recovers_after_drain)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<SilentServer>();
    REQUIRE(!mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv)));
    const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<RecoveryFactory>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));
    REQUIRE(wait_until([&] { return cli->last && cli->last->writes.load() >= 2; }, 3000ms));

    CHECK(cli->last->first);    // 第一笔受理
    CHECK(!cli->last->second);  // 配额用尽被拒
    CHECK(cli->last->third);    // 释放后重新受理（恢复）
    CHECK_EQ(cli->last->errors.load(), 0);

    mech->close_all();
    mech.reset();
}

// ---------------------------------------------------------------------------
// 5. 对端 RST 强断（默认 linger_zero=true），本端应收到错误并收敛
// ---------------------------------------------------------------------------
namespace {

/// 服务端：收到一条数据就关闭 —— 默认配置下 linger_zero=true，这就是一次 RST。
class RstTrigger : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        return ep->read({nullptr, m_rx.data(), m_rx.size()});
    }
    void on_read(std::error_code ec, Buf, std::size_t) override
    {
        if (ec) {
            return;
        }
        m_ep->close();   // linger_zero → RST
    }
    void on_write(std::error_code, ConstBuf, std::size_t) override {}
    void on_close() override { m_ep.reset(); }

private:
    std::shared_ptr<Conn>         m_ep;
    std::array<std::uint8_t, 128> m_rx{};
};

class RstServer : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_accept(Conn&) override
    {
        return std::make_shared<RstTrigger>();
    }
};

/// 客户端：发一条，然后静候错误通知。
class RstVictim : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        (void)ep->write({nullptr, "ping", 4});
        return ep->read({nullptr, m_rx.data(), m_rx.size()});
    }
    void on_read(std::error_code ec, Buf, std::size_t) override
    {
        if (ec) {
            read_ec = ec;
            read_failed.fetch_add(1);
        }
    }
    void on_write(std::error_code, ConstBuf, std::size_t) override {}
    void on_close() override { closed.fetch_add(1); }

    std::shared_ptr<Conn>         m_ep;
    std::array<std::uint8_t, 128> m_rx{};
    std::error_code               read_ec;
    std::atomic<int>              read_failed{0};
    std::atomic<int>              closed{0};
};

class RstClient : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        auto h = std::make_shared<RstVictim>();
        last = h;
        return h;
    }
    std::shared_ptr<RstVictim> last;
};

} // namespace

NETMECH_TEST(peer_reset_reports_error_and_closes)
{
    // 默认 linger_zero 已改为 false（优雅关闭）；这里显式开成 RST 来验证强断路径。
    Mechanism::Config cfg;
    cfg.linger_zero = true;
    auto mech = Mechanism::create(cfg);
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<RstServer>();
    REQUIRE(!mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv), 128));
    const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<RstClient>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));
    REQUIRE(wait_until([&] { return cli->last != nullptr; }, 3000ms));

    // 对端 RST 之后，本端的读必须以错误码归还，并且连接关闭恰好一次
    CHECK(wait_until([&] { return cli->last->read_failed.load() == 1; }, 5000ms));
    CHECK(wait_until([&] { return cli->last->closed.load() == 1; }, 5000ms));

    CHECK(static_cast<bool>(cli->last->read_ec));
    CHECK_EQ(cli->last->read_failed.load(), 1);
    CHECK_EQ(cli->last->closed.load(), 1);

    mech->close_all();
    mech.reset();
}
