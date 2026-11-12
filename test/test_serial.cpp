// ============================================================================
//  test_serial.cpp —— 串口测试
//
//  用 POSIX 的 pty 自建一对虚拟串口（不依赖 socat）：
//      master (fd)  ←→  slave (/dev/pts/N)
//  SerialPort 打开 slave，测试从 master 读写。
//
//  只测 Linux（Windows 需要 com0com，留待人工验证）。
// ============================================================================
#include <netmech/net.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <thread>
#include <array>
#include <memory>
#include <vector>
#include <mutex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>

#include "test_framework.hpp"

// 内部头：只为取串口写投递的测试注入点（声明-only，SHARED 下也不引用符号）。
#include "../src/serial_internal.hpp"

#if defined(__linux__)
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#endif

using namespace netmech;
using namespace netmech_test;

namespace {

using namespace std::chrono_literals;

#if defined(__linux__)

/// 一对 pty：master 是 fd，slave 是路径。
struct Pty {
    int         master = -1;
    std::string slave;

    bool open_()
    {
        master = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0) { return false; }
        if (::grantpt(master) != 0) { return false; }
        if (::unlockpt(master) != 0) { return false; }
        const char* name = ::ptsname(master);
        if (name == nullptr) { return false; }
        slave = name;

        // master 设成非阻塞：测试里要轮询读，不能卡死
        const int flags = ::fcntl(master, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(master, F_SETFL, flags | O_NONBLOCK);
        }
        return true;
    }

    ~Pty()
    {
        if (master >= 0) { ::close(master); }
    }
};

#endif   // __linux__

class Sink : public SerialHandler {
public:
    void on_read(std::error_code ec, Buf buf, std::size_t n) override
    {
        last_ec = ec;
        if (!ec && n > 0) {
            // 收下这条，再发起下一笔（读循环不能断）
            if (re_read) {
                read(static_cast<std::uint8_t*>(buf.data), n);
            }
            std::lock_guard<std::mutex> lk(mtx);
            data.insert(data.end(), static_cast<std::uint8_t*>(buf.data),
                        static_cast<std::uint8_t*>(buf.data) + n);
        }
        ++reads;
    }

    void on_close() override { ++closes; }

    void on_write(std::error_code ec, ConstBuf, std::size_t) override
    {
        last_write_ec = ec;
        ++writes;
    }

    // 回调里再读一次（用同一块缓冲）
    void read(std::uint8_t* p, std::size_t n)
    {
        if (port) { port->read(Buf{nullptr, p, n}); }
    }

    std::shared_ptr<SerialPort> port;
    std::mutex                  mtx;
    std::vector<std::uint8_t>   data;
    std::atomic<int>            reads{0};
    std::atomic<int>            closes{0};
    std::atomic<int>            writes{0};
    std::error_code             last_ec;
    std::error_code             last_write_ec;
    bool                        re_read = true;
};

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout = 2000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

// 写投递失败注入用例的状态：钩子是无捕获函数指针，上下文只能放
// 命名空间级静态变量（用例串行运行，无并发）。
std::shared_ptr<SerialPort>                  g_post_fault_port;
std::shared_ptr<std::array<std::uint8_t, 4>> g_post_fault_payload;
std::atomic<bool>                            g_post_fault_b_accepted{false};

} // namespace

// ---------------------------------------------------------------------------
// 1. 打开虚拟串口
// ---------------------------------------------------------------------------
NETMECH_TEST(serial_opens_virtual_port)
{
#if defined(__linux__)
    Pty pty;
    REQUIRE(pty.open_());

    SerialConfig cfg;
    cfg.device    = pty.slave;
    cfg.baud_rate = 9600;
    cfg.data_bits = 8;

    std::error_code ec;
    auto h = std::make_shared<Sink>();
    auto port = SerialPort::create(cfg, h, ec);

    CHECK_MSG(!ec, ("打开串口失败: " + ec.message()).c_str());
    REQUIRE(port != nullptr);
    CHECK(port->is_open());
    CHECK(port->device() == pty.slave);
    CHECK(port->id() != 0);

    port->close();
    CHECK(!port->is_open());
#else
    // Windows 需要 com0com，这里跳过
    CHECK(true);
#endif
}

// ---------------------------------------------------------------------------
// 2. 从 master 写 → SerialPort 读到
// ---------------------------------------------------------------------------
NETMECH_TEST(serial_receives_from_peer)
{
#if defined(__linux__)
    Pty pty;
    REQUIRE(pty.open_());

    SerialConfig cfg;
    cfg.device = pty.slave;

    std::error_code ec;
    auto h = std::make_shared<Sink>();
    auto port = SerialPort::create(cfg, h, ec);
    REQUIRE(port != nullptr);
    h->port = port;

    // 发起读
    std::array<std::uint8_t, 256> buf{};
    REQUIRE(port->read(Buf{nullptr, buf.data(), buf.size()}));

    // 从 master 写 5 字节
    const std::uint8_t payload[5] = {0x11, 0x03, 0x00, 0x00, 0x01};
    const ssize_t written = ::write(pty.master, payload, sizeof(payload));
    CHECK_EQ(written, static_cast<ssize_t>(sizeof(payload)));

    REQUIRE(wait_for([&] { return h->reads.load() > 0; }));
    {
        std::lock_guard<std::mutex> lk(h->mtx);
        CHECK_EQ(h->data.size(), sizeof(payload));
        CHECK(std::memcmp(h->data.data(), payload, sizeof(payload)) == 0);
    }

    port->close();
#else
    CHECK(true);
#endif
}

// ---------------------------------------------------------------------------
// 3. SerialPort 写 → master 收到
// ---------------------------------------------------------------------------
NETMECH_TEST(serial_writes_to_peer)
{
#if defined(__linux__)
    Pty pty;
    REQUIRE(pty.open_());

    SerialConfig cfg;
    cfg.device = pty.slave;

    std::error_code ec;
    auto h = std::make_shared<Sink>();
    auto port = SerialPort::create(cfg, h, ec);
    REQUIRE(port != nullptr);

    const std::uint8_t payload[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    CHECK(port->write(ConstBuf{nullptr, payload, sizeof(payload)}));

    // 从 master 读回来
    std::uint8_t got[8] = {};
    std::size_t  total = 0;
    const auto deadline = std::chrono::steady_clock::now() + 2000ms;
    while (total < sizeof(payload) && std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::read(pty.master, got + total, sizeof(got) - total);
        if (n > 0) { total += static_cast<std::size_t>(n); }
        else { std::this_thread::sleep_for(1ms); }
    }
    CHECK_EQ(total, sizeof(payload));
    CHECK(std::memcmp(got, payload, sizeof(payload)) == 0);

    port->close();
#else
    CHECK(true);
#endif
}

// ---------------------------------------------------------------------------
// 4. 同一时刻只允许一笔读
// ---------------------------------------------------------------------------
NETMECH_TEST(serial_rejects_second_read)
{
#if defined(__linux__)
    Pty pty;
    REQUIRE(pty.open_());

    SerialConfig cfg;
    cfg.device = pty.slave;

    std::error_code ec;
    auto h = std::make_shared<Sink>();
    auto port = SerialPort::create(cfg, h, ec);
    REQUIRE(port != nullptr);

    std::array<std::uint8_t, 64> b1{};
    std::array<std::uint8_t, 64> b2{};
    CHECK(port->read(Buf{nullptr, b1.data(), b1.size()}));
    CHECK(!port->read(Buf{nullptr, b2.data(), b2.size()}));   // 已有在途

    port->close();
#else
    CHECK(true);
#endif
}

// ---------------------------------------------------------------------------
// 5. 关闭后 write/read 返回 false
// ---------------------------------------------------------------------------
NETMECH_TEST(serial_closed_rejects_io)
{
#if defined(__linux__)
    Pty pty;
    REQUIRE(pty.open_());

    SerialConfig cfg;
    cfg.device = pty.slave;

    std::error_code ec;
    auto h = std::make_shared<Sink>();
    auto port = SerialPort::create(cfg, h, ec);
    REQUIRE(port != nullptr);

    port->close();
    port->close();   // 幂等

    const std::uint8_t x = 0;
    CHECK(!port->write(ConstBuf{nullptr, &x, 1}));
    std::uint8_t buf[8] = {};
    CHECK(!port->read(Buf{nullptr, buf, sizeof(buf)}));
#else
    CHECK(true);
#endif
}

// ---------------------------------------------------------------------------
// 6. 参数校验：空设备名被拒
// ---------------------------------------------------------------------------
NETMECH_TEST(serial_rejects_bad_config)
{
    SerialConfig cfg;   // device 为空

    std::error_code ec;
    auto h = std::make_shared<Sink>();
    auto port = SerialPort::create(cfg, h, ec);
    CHECK(port == nullptr);
    CHECK(static_cast<bool>(ec));
}

// ---------------------------------------------------------------------------
// 7. 跨线程写 + 在途读（串口写发起竞争的行为级回归）
//
//    调用线程持续 write()，同时 io 线程不断跑读完成（on_read 里重挂读）。
//    修复前：写发起在调用线程直调 async_write，与 io 线程的读完成处理并发
//    访问同一个 serial_port（asio 明令禁止）。修复后发起统一投递到 io 线程。
//    这里从**行为**上锁住：每笔受理恰好一次 on_write，数据完整到达对端。
// ---------------------------------------------------------------------------
NETMECH_TEST(serial_cross_thread_writes_with_pending_read)
{
#if defined(__linux__)
    Pty pty;
    REQUIRE(pty.open_());

    SerialConfig cfg;
    cfg.device    = pty.slave;
    cfg.baud_rate = 115200;

    std::error_code ec;
    auto h    = std::make_shared<Sink>();
    auto port = SerialPort::create(cfg, h, ec);
    REQUIRE(port != nullptr);
    h->port = port;

    // 读循环保持"一直有在途读"
    std::array<std::uint8_t, 64> rbuf{};
    REQUIRE(port->read(Buf{nullptr, rbuf.data(), rbuf.size()}));

    constexpr int kWriteCount = 200;
    std::atomic<bool> stop{false};

    // 对端持续喂数据：让 io 线程不停处理读完成并重挂读（制造并发）
    std::thread feeder([&] {
        const std::uint8_t p[2] = {0x55, 0xAA};
        while (!stop.load(std::memory_order_relaxed)) {
            const ssize_t n = ::write(pty.master, p, sizeof p);
            (void)n;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    // 调用线程持续写（队列满时重试）。
    //
    // ★ 缓冲区必须活到 on_write：这里用 shared_ptr 当 owner，由库延长生命周期。
    //   如果传栈上数组、线程退出后还有排队中的写，就是 UAF —— ASan 抓的就是这个
    //   （本用例第一版就踩了，正好验证了那条契约）。
    auto payload = std::make_shared<std::array<std::uint8_t, 4>>(
        std::array<std::uint8_t, 4>{{0xDE, 0xAD, 0xBE, 0xEF}});
    std::thread writer([&] {
        int sent = 0;
        while (sent < kWriteCount && !stop.load(std::memory_order_relaxed)) {
            if (port->write(ConstBuf{payload, payload->data(), payload->size()})) {
                ++sent;
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    });

    // 从 master 收 kWriteCount*4 字节
    std::size_t       total = 0;
    const std::size_t want  = static_cast<std::size_t>(kWriteCount) * 4u;
    std::uint8_t      tmp[256] = {};
    const auto deadline = std::chrono::steady_clock::now() + 3000ms;
    while (total < want && std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::read(pty.master, tmp, sizeof tmp);
        if (n > 0) {
            total += static_cast<std::size_t>(n);
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    writer.join();
    stop.store(true, std::memory_order_relaxed);
    feeder.join();

    CHECK_EQ(total, want);
    // 每笔受理恰好一次 on_write（等回调追上）
    REQUIRE(wait_for([&] { return h->writes.load() >= kWriteCount; }, 3000ms));
    CHECK_EQ(h->writes.load(), kWriteCount);

    port->close();
#else
    CHECK(true);
#endif
}

// ---------------------------------------------------------------------------
// 8. drain 带队列场景：先等队列排空，再按累计字节数估算移出时间
//
//    修复前：drain 不看发送队列，只等 ~2 个字符时间（9600bps 下 ~2ms）
//    就返回 true —— 队列里压着 N 笔未发数据也"排空成功"，RS-485 方向
//    控制会截断帧尾。这里压入 8 笔 × 64 字节 = 512 字节（9600bps 下
//    移出约需 533ms），钉住两条契约：
//      ① drain 返回时所有已受理的写都已完成 on_write；
//      ② 等待时长 ≥ 累计字节数按波特率的移出时间。
// ---------------------------------------------------------------------------
NETMECH_TEST(serial_drain_waits_for_queued_writes)
{
#if defined(__linux__)
    Pty pty;
    REQUIRE(pty.open_());

    SerialConfig cfg;
    cfg.device    = pty.slave;
    cfg.baud_rate = 9600;
    cfg.data_bits = 8;

    std::error_code ec;
    auto h    = std::make_shared<Sink>();
    auto port = SerialPort::create(cfg, h, ec);
    REQUIRE(port != nullptr);

    // 一次性压入 8 笔（owner 延长缓冲生命周期到 on_write，同用例 7 的契约）。
    constexpr int kWrites = 8;
    auto payload = std::make_shared<std::array<std::uint8_t, 64>>();
    payload->fill(0x5A);
    for (int i = 0; i < kWrites; ++i) {
        REQUIRE(port->write(ConstBuf{payload, payload->data(), payload->size()}));
    }

    const auto t0 = std::chrono::steady_clock::now();
    std::error_code dec;
    const bool ok = port->drain(dec);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();

    CHECK(ok);
    EXPECT_OK(dec);
    // 契约①：返回即排空 —— 不需要再等回调
    CHECK_EQ(h->writes.load(), kWrites);
    // 契约②：512 字节 × 10 bit / 9600bps ≈ 533ms（sleep_for 保证下界，不 flaky）
    constexpr long long kExpectedMs = 512LL * 10 * 1000 / 9600;
    CHECK_GE(elapsed_ms, kExpectedMs);

    port->close();
#else
    CHECK(true);
#endif
}

// ---------------------------------------------------------------------------
// 9. 空队列 drain：保持旧的快速返回行为（约 2 个字符时间）
// ---------------------------------------------------------------------------
NETMECH_TEST(serial_drain_empty_queue_returns_fast)
{
#if defined(__linux__)
    Pty pty;
    REQUIRE(pty.open_());

    SerialConfig cfg;
    cfg.device    = pty.slave;
    cfg.baud_rate = 9600;

    std::error_code ec;
    auto h    = std::make_shared<Sink>();
    auto port = SerialPort::create(cfg, h, ec);
    REQUIRE(port != nullptr);

    const auto t0 = std::chrono::steady_clock::now();
    std::error_code dec;
    const bool ok = port->drain(dec);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();

    CHECK(ok);
    EXPECT_OK(dec);
    CHECK_LT(elapsed_ms, 500);   // 无积压时不应白等

    port->close();
#else
    CHECK(true);
#endif
}

// ---------------------------------------------------------------------------
// 10. 关闭后 drain 返回 false + link_closed
// ---------------------------------------------------------------------------
NETMECH_TEST(serial_drain_closed_port_fails)
{
#if defined(__linux__)
    Pty pty;
    REQUIRE(pty.open_());

    SerialConfig cfg;
    cfg.device = pty.slave;

    std::error_code ec;
    auto h    = std::make_shared<Sink>();
    auto port = SerialPort::create(cfg, h, ec);
    REQUIRE(port != nullptr);

    port->close();   // open_ 同步翻 false，drain 立即拒绝

    std::error_code dec;
    CHECK(!port->drain(dec));
    CHECK(dec == NetErrc::link_closed);
#else
    CHECK(true);
#endif
}

// ---------------------------------------------------------------------------
// 11. 写投递失败：回滚不得搁浅并发入队的后续写
//
//    修复前：A 入队后解锁，B 看到 writing=true 入队（已受理）；A 的
//    asio::post 抛异常时回滚只 pop 队首并置 writing=false —— B 留在
//    队列里却没有任何 do_write 在途，on_write 永不回调。
//    修复后：撤回 A 后队列非空则保持 writing=true 并重投 do_write。
//    注入钩子在 A 的线程上、解锁后运行，在里面入队 B 即可确定性
//    复现该交错（仅 Linux pty + STATIC 构建）。
// ---------------------------------------------------------------------------
#if defined(__linux__) && !defined(NETMECH_SHARED)
NETMECH_TEST(serial_write_post_failure_does_not_strand_concurrent_writes)
{
    Pty pty;
    REQUIRE(pty.open_());

    SerialConfig cfg;
    cfg.device    = pty.slave;
    cfg.baud_rate = 115200;

    std::error_code ec;
    auto h    = std::make_shared<Sink>();
    auto port = SerialPort::create(cfg, h, ec);
    REQUIRE(port != nullptr);

    auto payload_a = std::make_shared<std::array<std::uint8_t, 4>>(
        std::array<std::uint8_t, 4>{{0xA1, 0xA2, 0xA3, 0xA4}});
    g_post_fault_port    = port;
    g_post_fault_payload = std::make_shared<std::array<std::uint8_t, 4>>(
        std::array<std::uint8_t, 4>{{0xB1, 0xB2, 0xB3, 0xB4}});
    g_post_fault_b_accepted.store(false);

    // 钩子：先入队 B（模拟并发线程，此时 writing=true ⇒ B 直接受理），
    // 再抛异常模拟 A 的投递失败。
    detail::serial_post_fault_hook = []() {
        g_post_fault_b_accepted.store(g_post_fault_port->write(
            ConstBuf{g_post_fault_payload, g_post_fault_payload->data(),
                     g_post_fault_payload->size()}));
        throw std::bad_alloc();
    };
    const bool a_accepted = port->write(
        ConstBuf{payload_a, payload_a->data(), payload_a->size()});
    detail::serial_post_fault_hook = nullptr;

    CHECK(!a_accepted);                      // A 未受理，缓冲立即归还
    CHECK(g_post_fault_b_accepted.load());   // B 已受理

    // B 必须恰好一次 on_write（修复前：被搁浅，永不回调）
    REQUIRE(wait_for([&] { return h->writes.load() >= 1; }, 2000ms));
    CHECK_EQ(h->writes.load(), 1);
    CHECK(!h->last_write_ec);

    // 对端收到的是 B 的数据（A 已撤回，不得出现在线上）
    std::uint8_t got[8] = {};
    std::size_t  total  = 0;
    const auto deadline = std::chrono::steady_clock::now() + 2000ms;
    while (total < 4 && std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::read(pty.master, got + total, sizeof(got) - total);
        if (n > 0) { total += static_cast<std::size_t>(n); }
        else { std::this_thread::sleep_for(1ms); }
    }
    CHECK_EQ(total, std::size_t{4});
    CHECK_EQ(got[0], 0xB1);
    CHECK_EQ(got[3], 0xB4);

    port->close();
    g_post_fault_port.reset();
    g_post_fault_payload.reset();
}
#else
NETMECH_TEST(serial_write_post_failure_does_not_strand_concurrent_writes)
{
    // Windows 需 com0com；SHARED 不导出内部注入点 —— 同其余串口用例跳过。
    CHECK(true);
}
#endif