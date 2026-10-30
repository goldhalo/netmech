// ============================================================================
//  test_conn_write.cpp —— 写顺序与并发写
//
//  验证契约：
//    * 同一线程连续受理的多笔 write，**完成回调顺序 == 受理顺序**（FIFO）
//    * 多线程并发 write 时，每一笔的数据在流上**连续不交错**
//    * 每笔 write 的完成回调精确对应它自己提交的那块缓冲
// ============================================================================
#include <netmech/net.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
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

constexpr std::size_t kBlockSize = 16;

/// 服务端：持续读，把收到的字节全部累积起来。
class Collector final : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        return ep->read({nullptr, m_rx.data(), m_rx.size()});
    }

    void on_read(std::error_code ec, Buf buf, std::size_t n) override
    {
        if (ec || n == 0) {
            if (ec) {
                ++errors;
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_data.append(static_cast<const char*>(buf.data), n);
        }
        received.fetch_add(n);
        (void)m_ep->read({nullptr, m_rx.data(), m_rx.size()});
    }

    void on_write(std::error_code, ConstBuf, std::size_t) override {}
    void on_close() override { ++closed; }

    std::string snapshot() const
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_data;
    }

    std::shared_ptr<Conn>         m_ep;
    std::array<std::uint8_t, 512> m_rx{};
    mutable std::mutex            m_mtx;
    std::string                   m_data;

    std::atomic<std::size_t> received{0};
    std::atomic<int>         errors{0};
    std::atomic<int>         closed{0};
};

class CollectorServer : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_accept(Conn&) override
    {
        auto h = std::make_shared<Collector>();
        last = h;
        return h;
    }
    std::shared_ptr<Collector> last;
};

/// 客户端：on_open 里连续提交若干笔写，记录"提交顺序"与"完成顺序"。
class FifoWriter final : public ConnHandler {
public:
    static constexpr int kBlocks = 6;

    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        for (int i = 0; i < kBlocks; ++i) {
            m_blocks[static_cast<std::size_t>(i)].fill(static_cast<std::uint8_t>('A' + i));
            ConstBuf buf{nullptr, m_blocks[static_cast<std::size_t>(i)].data(), kBlockSize};
            if (ep->write(buf)) {
                submitted.push_back(buf.data);
            } else {
                ++rejected;
            }
        }
        return true;
    }

    void on_read(std::error_code, Buf, std::size_t) override {}

    void on_write(std::error_code ec, ConstBuf buf, std::size_t n) override
    {
        if (ec) {
            ++write_errors;
            return;
        }
        if (n != kBlockSize) {
            ++bad_sizes;
        }
        completed.push_back(buf.data);
    }

    void on_close() override { ++closed; }

    std::shared_ptr<Conn>                       m_ep;
    std::array<std::array<std::uint8_t, kBlockSize>, kBlocks> m_blocks{};

    std::mutex               mtx;
    std::vector<const void*> submitted;
    std::vector<const void*> completed;

    std::atomic<int> rejected{0};
    std::atomic<int> write_errors{0};
    std::atomic<int> bad_sizes{0};
    std::atomic<int> closed{0};
};

class FifoFactory : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        auto h = std::make_shared<FifoWriter>();
        last = h;
        return h;
    }
    std::shared_ptr<FifoWriter> last;
};

} // namespace

// ---------------------------------------------------------------------------
// 1. 同一线程连续写：完成顺序 == 提交顺序，服务端收到的字节顺序一致
// ---------------------------------------------------------------------------
NETMECH_TEST(write_completion_order_matches_submit_order)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<CollectorServer>();
    REQUIRE(!mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv)));
    const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<FifoFactory>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));
    REQUIRE(wait_until([&] { return cli->last != nullptr; }, 3000ms));

    const std::size_t expect = FifoWriter::kBlocks * kBlockSize;
    CHECK(wait_until([&] { return srv->last && srv->last->received.load() >= expect; }, 5000ms));

    std::lock_guard<std::mutex> lk(cli->last->mtx);
    REQUIRE(cli->last->submitted.size() == FifoWriter::kBlocks);
    REQUIRE(cli->last->completed.size() == FifoWriter::kBlocks);

    // 完成顺序与提交顺序逐一对应（buf 指针原样回传）
    CHECK(cli->last->completed == cli->last->submitted);
    CHECK_EQ(cli->last->rejected.load(), 0);
    CHECK_EQ(cli->last->write_errors.load(), 0);
    CHECK_EQ(cli->last->bad_sizes.load(), 0);

    // 服务端收到的字节顺序：A*16, B*16, C*16 ...
    const std::string data = srv->last->snapshot();
    REQUIRE(data.size() >= expect);
    for (int i = 0; i < FifoWriter::kBlocks; ++i) {
        const std::string block = data.substr(static_cast<std::size_t>(i) * kBlockSize, kBlockSize);
        CHECK_EQ(block, std::string(kBlockSize, static_cast<char>('A' + i)));
    }

    mech->close_all();
    mech.reset();
}

// ---------------------------------------------------------------------------
// 2. 多线程并发写：每笔的数据块在流上连续，不与其他线程的块交错
// ---------------------------------------------------------------------------
namespace {

class MultiWriter final : public ConnHandler {
public:
    static constexpr int kPerThread = 20;

    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        return true;
    }

    void on_read(std::error_code, Buf, std::size_t) override {}
    void on_write(std::error_code ec, ConstBuf, std::size_t) override
    {
        if (!ec) {
            completed.fetch_add(1);
        } else {
            failed.fetch_add(1);
        }
    }
    void on_close() override { ++closed; }

    /// 由测试线程调用：写 kPerThread 块，块内字节全为 tag。
    ///
    /// 每块单独 make_shared 并用 Buf::owner 交给库持有 —— 写是异步的，
    /// 栈上缓冲在函数返回后立刻失效，不能用。
    void burst(std::uint8_t tag)
    {
        auto ep = m_ep;
        if (!ep) {
            return;
        }
        for (int i = 0; i < kPerThread; ++i) {
            auto block = std::make_shared<std::array<std::uint8_t, kBlockSize>>();
            block->fill(tag);
            if (ep->write({block, block->data(), block->size()})) {
                accepted.fetch_add(1);
            } else {
                rejected.fetch_add(1);
            }
        }
    }

    std::shared_ptr<Conn>      m_ep;
    std::atomic<int>           accepted{0};
    std::atomic<int>           rejected{0};
    std::atomic<int>           completed{0};
    std::atomic<int>           failed{0};
    std::atomic<int>           closed{0};
};

class MultiFactory : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        auto h = std::make_shared<MultiWriter>();
        last = h;
        return h;
    }
    std::shared_ptr<MultiWriter> last;
};

} // namespace

NETMECH_TEST(concurrent_writes_do_not_interleave)
{
    auto mech = Mechanism::create();
    REQUIRE(mech != nullptr);

    auto srv = std::make_shared<CollectorServer>();
    REQUIRE(!mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv)));
    const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
    REQUIRE(bound.has_value());

    auto cli = std::make_shared<MultiFactory>();
    REQUIRE(!mech->connect(*bound, std::weak_ptr<Linkable>(cli), 3000));
    REQUIRE(wait_until([&] { return cli->last && cli->last->m_ep != nullptr; }, 3000ms));

    // 三个线程同时写，每线程 20 块，块内填充各自的 tag
    std::thread t1([&] { cli->last->burst('X'); });
    std::thread t2([&] { cli->last->burst('Y'); });
    std::thread t3([&] { cli->last->burst('Z'); });
    t1.join();
    t2.join();
    t3.join();

    const std::size_t expect =
        static_cast<std::size_t>(MultiWriter::kPerThread) * 3 * kBlockSize;
    CHECK(wait_until([&] { return srv->last && srv->last->received.load() >= expect; }, 8000ms));

    // 每个 16 字节块内部必须全部是同一种字节 —— 说明没有任何一笔被拆开交错
    const std::string data = srv->last->snapshot();
    REQUIRE(data.size() >= expect);
    std::size_t bad_blocks = 0;
    for (std::size_t off = 0; off + kBlockSize <= data.size(); off += kBlockSize) {
        const char first = data[off];
        for (std::size_t i = 1; i < kBlockSize; ++i) {
            if (data[off + i] != first) {
                ++bad_blocks;
                break;
            }
        }
    }
    CHECK_MSG(bad_blocks == 0, "发现被拆开交错的写块");

    CHECK_EQ(cli->last->rejected.load(), 0);
    CHECK_EQ(cli->last->failed.load(), 0);

    mech->close_all();
    mech.reset();
}
