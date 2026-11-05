// ============================================================================
//  test_conn_batch.cpp —— 写批量合并（pump_write 的 buffer sequence）
//
//  验证契约：
//    * 队列积压时，多笔写**合并成一次 socket 写**（write_batches < write_requests）
//    * 合并之后，**每笔仍然恰好一次 on_write**，顺序不变（归因不能错）
//    * 合并之后字节流仍然完整、有序（不能少字节、不能重排）
//
//  为什么单独测：这套合并直接把小包吞吐提高一个量级（实测 10 μs/笔 → 0.9 μs/笔），
//  但"一次 async_write 回调一次，却要按字节归因给 N 笔"这段逻辑很细，
//  一旦归因错位就会出现"某笔回调早到/晚到/丢回调"，而应用层只能看到乱序。
// ============================================================================
#include <netmech/net.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>

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

constexpr int         kCount = 100;   ///< 提交笔数，远大于 kMaxWriteBatch(16)
constexpr std::size_t kBlock = 16;    ///< 每笔字节数

/// 服务端：把收到的字节按序累积。
class Collector final : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = std::move(ep);
        return arm();
    }

    void on_read(std::error_code ec, Buf buf, std::size_t n) override
    {
        if (ec) {
            ++errors;
            return;
        }
        m_data.append(static_cast<const char*>(buf.data), n);
        received.fetch_add(n);
        arm();
    }

    void on_write(std::error_code, ConstBuf, std::size_t) override {}
    void on_close() override { ++closed; }

    bool arm()
    {
        return m_ep && m_ep->read({nullptr, m_rx.data(), m_rx.size()});
    }

    std::shared_ptr<Conn>         m_ep;
    std::array<std::uint8_t, 512> m_rx{};
    std::string                   m_data;   ///< 只有 io 线程写，测试线程在完成后读

    std::atomic<std::size_t> received{0};
    std::atomic<int>         errors{0};
    std::atomic<int>         closed{0};
};

class CollectorServer final : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_accept(Conn&) override
    {
        auto h = std::make_shared<Collector>();
        last.set(h);
        return h;
    }
    netmech_test::SyncSlot<Collector> last;   // io 线程写、主线程读
};

/// 客户端：on_open 里一口气提交 kCount 笔（队列必然积压 → 触发合并）。
class Burst final : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = std::move(ep);
        m_blocks.resize(kCount * kBlock);
        for (int i = 0; i < kCount; ++i) {
            std::memset(m_blocks.data() + i * kBlock, i & 0xFF, kBlock);
        }
        for (int i = 0; i < kCount; ++i) {
            ConstBuf b;
            b.data = m_blocks.data() + static_cast<std::size_t>(i) * kBlock;
            b.size = kBlock;
            if (m_ep->write(b)) {
                accepted.fetch_add(1);
            }
        }
        return true;
    }

    void on_read(std::error_code, Buf, std::size_t) override {}
    void on_write(std::error_code ec, ConstBuf, std::size_t) override
    {
        if (ec) {
            ++errors;
        }
        completed.fetch_add(1);
    }
    void on_close() override { m_ep.reset(); }

    std::shared_ptr<Conn>     m_ep;
    std::vector<std::uint8_t> m_blocks;

    std::atomic<int> accepted{0};
    std::atomic<int> completed{0};
    std::atomic<int> errors{0};
};

class BurstClient final : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        auto h = std::make_shared<Burst>();
        last.set(h);
        return h;
    }
    netmech_test::SyncSlot<Burst> last;   // io 线程写、主线程读
};

} // namespace

NETMECH_TEST(write_batches_merge_requests_without_losing_callbacks)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<CollectorServer>();
    REQUIRE(!mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv), 8));
    const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<BurstClient>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));
    REQUIRE(wait_until([&] { return cli->last.ready(); }, 3000ms));

    // 1) 每一笔都必须回调，且只有一次
    REQUIRE(wait_until([&] { return cli->last.get()->completed.load() == kCount; }, 5000ms));
    CHECK_EQ(cli->last.get()->errors.load(), 0);

    // 2) 合并真的发生了：socket 写次数远少于请求笔数
    const auto req = cli->last.get()->m_ep->write_requests();
    const auto bch = cli->last.get()->m_ep->write_batches();
    CHECK_EQ(req, static_cast<std::uint64_t>(kCount));
    CHECK(bch >= 1);
    // 100 笔 = 队首 1 笔 + 若干批 16 笔，理论上 ~7 次；给足余量
    CHECK_MSG(bch <= static_cast<std::uint64_t>(kCount) / 2,
              ("write_batches=" + std::to_string(bch) + " 说明没有合并").c_str());
    std::printf("      受理 %llu 笔 → %llu 次 socket 写（平均每批 %.1f 笔）\n",
                static_cast<unsigned long long>(req), static_cast<unsigned long long>(bch),
                static_cast<double>(req) / static_cast<double>(bch));

    // 3) 服务端收到的字节数正确
    REQUIRE(wait_until([&] {
        return srv->last.ready() && srv->last.get()->received.load() == kCount * kBlock;
    }, 5000ms));
    CHECK_EQ(srv->last.get()->errors.load(), 0);

    // 4) 字节流按序、无缺失（每块内容 = 块序号）
    {
        const std::string& got = srv->last.get()->m_data;
        REQUIRE(got.size() == static_cast<std::size_t>(kCount) * kBlock);
        int   bad   = 0;
        for (int i = 0; i < kCount; ++i) {
            const char want = static_cast<char>(i & 0xFF);
            for (std::size_t k = 0; k < kBlock; ++k) {
                if (got[static_cast<std::size_t>(i) * kBlock + k] != want) {
                    ++bad;
                    break;
                }
            }
        }
        CHECK_EQ(bad, 0);
    }

    mech->close_all();
    mech.reset();
}
