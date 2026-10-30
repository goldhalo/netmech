// ============================================================================
//  test_conn_basic.cpp —— 新 ConnHandler 接口的基础验证
//
//  验证：
//    * on_open / on_read / on_write / on_close 的调用顺序与参数
//    * read(Buf, ReadMode) / write(ConstBuf) 的受理语义
//    * 服务端 echo 往返（server 把收到的字节原样写回，client 收到）
//    * 关闭时在途操作以错误码归还、on_close 恰好一次
//
//  这是按新接口（虚函数回调 + Buf 视图）重写的第一组测试，
//  替代已移入 test/legacy 的旧用例。
// ============================================================================
#include <netmech/net.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "test_framework.hpp"

using namespace netmech;
using namespace netmech_test;

namespace {

using namespace std::chrono_literals;

/// 轮询等待（测试专用；生产代码不应这样等）。
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

/// 服务端：收到什么就原样写回，然后继续读。
class EchoConn : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        ++opened;
        return ep->read({nullptr, m_rx.data(), m_rx.size()});
    }

    void on_read(std::error_code ec, Buf buf, std::size_t n) override
    {
        if (ec) {
            last_read_ec = ec;
            return;
        }
        bytes_in += n;
        ++reads;
        // 校验回传的 buf 就是我们提交的那块内存
        if (buf.data == m_rx.data()) {
            ++buf_matched;
        }
        // 原样回写
        (void)m_ep->write({nullptr, buf.data, n});
        // 续读
        (void)m_ep->read({nullptr, m_rx.data(), m_rx.size()});
    }

    void on_write(std::error_code ec, ConstBuf, std::size_t n) override
    {
        if (!ec) {
            bytes_out += n;
            ++writes;
        }
    }

    void on_close() override { ++closed; }

    std::shared_ptr<Conn>        m_ep;
    std::array<std::uint8_t, 512>    m_rx{};

    std::atomic<int>          opened{0};
    std::atomic<int>          reads{0};
    std::atomic<int>          writes{0};
    std::atomic<int>          closed{0};
    std::atomic<int>          buf_matched{0};
    std::atomic<std::size_t>  bytes_in{0};
    std::atomic<std::size_t>  bytes_out{0};
    std::error_code           last_read_ec;
};

/// 客户端：发一条消息，等回包。
class ClientConn : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        ++opened;
        return ep->read({nullptr, m_rx.data(), m_rx.size()});
    }

    void on_read(std::error_code ec, Buf, std::size_t n) override
    {
        if (ec) {
            last_ec = ec;
            return;
        }
        received.append(reinterpret_cast<const char*>(m_rx.data()), n);
        (void)m_ep->read({nullptr, m_rx.data(), m_rx.size()});
    }

    void on_write(std::error_code, ConstBuf, std::size_t) override {}

    void on_close() override { ++closed; }

    std::shared_ptr<Conn>       m_ep;
    std::array<std::uint8_t, 512>   m_rx{};
    std::string                     received;

    std::atomic<int>  opened{0};
    std::atomic<int>  closed{0};
    std::error_code   last_ec;
};

/// 服务端订阅者
class ServerLinkable : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_accept(Conn& c) override
    {
        (void)c;
        auto h = std::make_shared<EchoConn>();
        last = h;
        return h;
    }

    std::shared_ptr<EchoConn> last;
};

/// 客户端订阅者
class ClientLinkable : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_linkup(Conn& c) override
    {
        (void)c;
        auto h = std::make_shared<ClientConn>();
        last = h;
        return h;
    }

    std::shared_ptr<ClientConn> last;
};

} // namespace

// ---------------------------------------------------------------------------
// 1. 服务端 echo 往返
// ---------------------------------------------------------------------------
NETMECH_TEST(conn_echo_roundtrip)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<ServerLinkable>();
    const Address wild("127.0.0.1", 0);
    REQUIRE(!mech->listen(wild, std::weak_ptr<Linkable>(srv)));

    const auto bound = mech->local_address_of(wild);
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<ClientLinkable>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));

    // 等双方 on_open
    CHECK(wait_until([&] { return srv->last && srv->last->opened.load() == 1; }, 3000ms));
    CHECK(wait_until([&] { return cli->last && cli->last->opened.load() == 1; }, 3000ms));
    REQUIRE(srv->last != nullptr);
    REQUIRE(cli->last != nullptr);

    // 客户端发一条
    const char payload[] = "hello-conn";
    REQUIRE(cli->last->m_ep->write({nullptr, payload, sizeof(payload) - 1}));

    // 服务端收到并回写，客户端收到同样的内容
    CHECK(wait_until([&] { return cli->last->received.size() == sizeof(payload) - 1; }, 3000ms));
    CHECK_EQ(cli->last->received, std::string(payload));

    // 服务端确实读到了，且回传的 buf 就是它提交的那块
    CHECK_EQ(srv->last->bytes_in.load(), sizeof(payload) - 1);
    CHECK_GE(srv->last->buf_matched.load(), 1);
    CHECK_GE(srv->last->writes.load(), 1);

    mech->close_all();
    mech.reset();
}

// ---------------------------------------------------------------------------
// 2. 关闭时 on_close 恰好一次，且未受理的 read/write 不产生回调
// ---------------------------------------------------------------------------
NETMECH_TEST(conn_close_notifies_once)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<ServerLinkable>();
    const Address wild("127.0.0.1", 0);
    REQUIRE(!mech->listen(wild, std::weak_ptr<Linkable>(srv)));
    const auto bound = mech->local_address_of(wild);
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<ClientLinkable>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));
    REQUIRE(wait_until([&] { return srv->last && srv->last->opened.load() == 1; }, 3000ms));
    REQUIRE(wait_until([&] { return cli->last && cli->last->opened.load() == 1; }, 3000ms));

    // 主动关闭客户端连接
    cli->last->m_ep->close();

    CHECK(wait_until([&] { return cli->last->closed.load() == 1; }, 3000ms));

    // 关闭后 is_open 为 false，读写都不受理
    CHECK(!cli->last->m_ep->is_open());
    CHECK(!cli->last->m_ep->write({nullptr, "x", 1}));
    CHECK(!cli->last->m_ep->read({nullptr, nullptr, 0}));

    // 再关一次是幂等的，不会产生第二个 on_close
    cli->last->m_ep->close();
    std::this_thread::sleep_for(50ms);
    CHECK_EQ(cli->last->closed.load(), 1);

    mech->close_all();
    mech.reset();
}

// ---------------------------------------------------------------------------
// 3. 同一时刻只允许一笔读在途
// ---------------------------------------------------------------------------
NETMECH_TEST(conn_single_read_in_flight)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<ServerLinkable>();
    const Address wild("127.0.0.1", 0);
    REQUIRE(!mech->listen(wild, std::weak_ptr<Linkable>(srv)));
    const auto bound = mech->local_address_of(wild);
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<ClientLinkable>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));
    REQUIRE(wait_until([&] { return cli->last && cli->last->opened.load() == 1; }, 3000ms));

    // ClientConn 在 on_open 里已经发起了一笔读 → 再发起必须被拒绝
    std::uint8_t scratch[8] = {};
    CHECK(!cli->last->m_ep->read({nullptr, scratch, sizeof scratch}));

    // 空 buffer 也不受理
    CHECK(!cli->last->m_ep->write({nullptr, nullptr, 0}));

    mech->close_all();
    mech.reset();
}

// ---------------------------------------------------------------------------
// 4. user_context 透传（服务端继承监听器上下文，客户端用自己的）
// ---------------------------------------------------------------------------
NETMECH_TEST(conn_user_context_propagates)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv_ctx = std::make_shared<int>(7);
    auto cli_ctx = std::make_shared<int>(9);

    auto srv = std::make_shared<ServerLinkable>();
    const Address wild("127.0.0.1", 0);
    REQUIRE(!mech->listen(wild, std::weak_ptr<Linkable>(srv), 128, srv_ctx));
    const auto bound = mech->local_address_of(wild);
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<ClientLinkable>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000, Address(), cli_ctx));

    REQUIRE(wait_until([&] { return srv->last && srv->last->opened.load() == 1; }, 3000ms));
    REQUIRE(wait_until([&] { return cli->last && cli->last->opened.load() == 1; }, 3000ms));

    // 服务端连接继承监听器上下文；客户端连接用 connect 传入的那个
    CHECK(srv->last->m_ep->user_context() == srv_ctx);
    CHECK(cli->last->m_ep->user_context() == cli_ctx);

    mech->close_all();
    mech.reset();
}

// ---------------------------------------------------------------------------
// 5. 按连接 id 写 / 关闭
// ---------------------------------------------------------------------------
NETMECH_TEST(mechanism_write_and_close_by_id)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<ServerLinkable>();
    const Address wild("127.0.0.1", 0);
    REQUIRE(!mech->listen(wild, std::weak_ptr<Linkable>(srv)));
    const auto bound = mech->local_address_of(wild);
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<ClientLinkable>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));
    REQUIRE(wait_until([&] { return cli->last && cli->last->opened.load() == 1; }, 3000ms));
    REQUIRE(wait_until([&] { return srv->last && srv->last->opened.load() == 1; }, 3000ms));

    const std::uint64_t cid = cli->last->m_ep->id();

    // 未知 id 一律返回 false
    CHECK(!mech->write(0xFFFFFFF0u, {nullptr, "x", 1}));
    CHECK(!mech->close(0xFFFFFFF0u));

    // 按 id 发数据，服务端应当收到
    const char payload[] = "by-id";
    REQUIRE(mech->write(cid, {nullptr, payload, sizeof(payload) - 1}));
    CHECK(wait_until([&] { return srv->last->bytes_in.load() == sizeof(payload) - 1; }, 3000ms));

    // 按 id 关闭，之后同一 id 不再受理写
    CHECK(mech->close(cid));
    CHECK(wait_until([&] { return cli->last->closed.load() == 1; }, 3000ms));
    CHECK(!mech->write(cid, {nullptr, "x", 1}));

    mech->close_all();
    mech.reset();
}
