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
#include <thread>

#include "test_framework.hpp"

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
    std::error_code             last_ec;
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
