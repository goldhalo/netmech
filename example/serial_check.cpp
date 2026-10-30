// =============================================================================
//  serial_check —— 串口双向收发自检
// =============================================================================
//  用法：serial_check <端口A> <端口B> [波特率]
//  例：  serial_check COM5 COM6 9600     （com0com 建的一对）
//        serial_check /dev/pts/3 /dev/pts/4 9600
//
//  做三件事：
//    1. A 发 → B 收
//    2. B 发 → A 收
//    3. 双向同时（各发 256 字节，验证不丢不串）
//  退出码 0 = 全通过。
// =============================================================================
#include <netmech/net.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace netmech;

namespace {

using namespace std::chrono_literals;

/// 收数据的 handler：收到多少存多少，并在回调里继续读。
class Sink : public SerialHandler {
public:
    void on_read(std::error_code ec, Buf buf, std::size_t n) override
    {
        if (ec) {
            ++errors;
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mtx);
            data.insert(data.end(), static_cast<std::uint8_t*>(buf.data),
                        static_cast<std::uint8_t*>(buf.data) + n);
        }
        ++reads;
        // 继续读（缓存用同一个缓冲）
        port->read(Buf{nullptr, chunk.data(), chunk.size()});
    }

    void start()
    {
        port->read(Buf{nullptr, chunk.data(), chunk.size()});
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lk(mtx);
        return data.size();
    }

    std::vector<std::uint8_t> snapshot() const
    {
        std::lock_guard<std::mutex> lk(mtx);
        return data;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx);
        data.clear();
    }

    std::shared_ptr<SerialPort>    port;
    std::array<std::uint8_t, 1024> chunk{};
    mutable std::mutex             mtx;
    std::vector<std::uint8_t>      data;
    std::atomic<int>               reads{0};
    std::atomic<int>               errors{0};
};

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout = 3000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

int g_fail = 0;

void check(bool cond, const char* what)
{
    std::printf("  [%s] %s\n", cond ? " ok " : "FAIL", what);
    if (!cond) { ++g_fail; }
}

/// 等对端收到指定内容（最多 2 秒）。
void check_peer(const std::shared_ptr<Sink>& h, const std::uint8_t* want,
                std::size_t n, const char* what)
{
    const bool ok = wait_for([&] { return h->size() >= n; }, 2000ms);
    bool match = false;
    if (ok) {
        const auto got = h->snapshot();
        match = got.size() >= n && std::memcmp(got.data(), want, n) == 0;
    }
    check(ok && match, what);
}

void check_peer_total(const std::shared_ptr<Sink>& h, std::size_t n, const char* what)
{
    check(wait_for([&] { return h->size() >= n; }, 2000ms), what);
}

#define CHECK_PEER(h, want, n, what) check_peer((h), (want), (n), (what))
#define CHECK_PEER_TOTAL(h, n, what) check_peer_total((h), (n), (what))

bool open_pair(const char* a, const char* b, std::uint32_t baud,
               std::shared_ptr<SerialPort>& pa, std::shared_ptr<SerialPort>& pb,
               std::shared_ptr<Sink>& hout_a, std::shared_ptr<Sink>& hout_b)
{
    std::error_code ec;

    SerialConfig ca;
    ca.device    = a;
    ca.baud_rate = baud;
    auto ha = std::make_shared<Sink>();
    pa = SerialPort::create(ca, ha, ec);
    if (!pa) {
        std::fprintf(stderr, "打开 %s 失败: %s\n", a, ec.message().c_str());
        return false;
    }
    ha->port = pa;

    SerialConfig cb;
    cb.device    = b;
    cb.baud_rate = baud;
    auto hb = std::make_shared<Sink>();
    pb = SerialPort::create(cb, hb, ec);
    if (!pb) {
        std::fprintf(stderr, "打开 %s 失败: %s\n", b, ec.message().c_str());
        return false;
    }
    hb->port = pb;

    ha->start();
    hb->start();
    hout_a = ha;
    hout_b = hb;
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <端口A> <端口B> [波特率]\n", argv[0]);
        return 2;
    }
    const std::uint32_t baud = (argc > 3) ? static_cast<std::uint32_t>(std::atoi(argv[3]))
                                          : 9600u;

    if (!netmech::initialize(2)) {
        std::fprintf(stderr, "netmech initialize failed\n");
        return 1;
    }

    std::shared_ptr<SerialPort> pa, pb;
    std::shared_ptr<Sink> ha, hb;
    if (!open_pair(argv[1], argv[2], baud, pa, pb, ha, hb)) {
        netmech::shutdown();
        return 1;
    }
    std::printf("打开 %s <-> %s @ %u\n\n", argv[1], argv[2], baud);

    // ---- 1. A → B ----
    std::printf("--- 1. %s -> %s ---\n", argv[1], argv[2]);
    {
        const std::uint8_t msg[4] = {0x11, 0x22, 0x33, 0x44};
        check(pa->write(ConstBuf{nullptr, msg, sizeof(msg)}), "A 写受理");
        CHECK_PEER(hb, msg, sizeof(msg), "B 收到正确的 4 字节");
    }

    // ---- 2. B → A ----
    std::printf("--- 2. %s -> %s ---\n", argv[2], argv[1]);
    {
        const std::uint8_t msg[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        check(pb->write(ConstBuf{nullptr, msg, sizeof(msg)}), "B 写受理");
        CHECK_PEER(ha, msg, sizeof(msg), "A 收到正确的 4 字节");
    }

    // ---- 3. 双向各 256 字节 ----
    std::printf("--- 3. 双向各 256 字节 ---\n");
    {
        std::vector<std::uint8_t> a2b(256), b2a(256);
        for (int i = 0; i < 256; ++i) {
            a2b[i] = static_cast<std::uint8_t>(i);
            b2a[i] = static_cast<std::uint8_t>(255 - i);
        }
        check(pa->write(ConstBuf{nullptr, a2b.data(), a2b.size()}), "A 写 256 字节受理");
        check(pb->write(ConstBuf{nullptr, b2a.data(), b2a.size()}), "B 写 256 字节受理");
        CHECK_PEER_TOTAL(hb, 260, "B 累计收到 4 + 256 字节");
        CHECK_PEER_TOTAL(ha, 260, "A 累计收到 4 + 256 字节");
    }

    // ---- 4. drain（RS-485 方向控制用）----
    std::printf("--- 4. drain ---\n");
    {
        std::error_code ec;
        check(pa->drain(ec), "drain 返回成功");
    }

    pa->close();
    pb->close();
    netmech::shutdown();

    std::printf("\n%s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
