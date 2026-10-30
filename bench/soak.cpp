// =============================================================================
//  NetMechanism - 长跑稳定性测试（soak）
// =============================================================================
//  文件    : bench/soak.cpp
//  用法    : soak [duration_sec] [conns] [msgs_per_conn]
//            默认  60 秒 / 50 连接 / 每连接 20 条
//
//  它回答的是压测表里测不出来的那个问题：**跑久了会不会漏**。
//
//  每一轮：建 conns 个连接 → 每个发 msgs 条并校验回显 → 全部关闭 → 检查收敛
//  每隔一段时间打印：累计轮次 / 累计消息 / 工作集内存
//
//  判定标准（结束时）：
//    · 每轮结束时 endpoint_count() 必须回落到 0（否则说明连接没摘干净）
//    · 末轮内存相对首轮不应持续单调上涨（允许有波动，但不应线性增长）
// =============================================================================
#include <netmech/net.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <memory>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <cstdio>
#include <unistd.h>
#endif

using namespace netmech;

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

/// 当前进程工作集（Windows）。拿不到就返回 0，不影响测试继续进行。
std::size_t rss_bytes() noexcept
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc)) != 0) {
        return static_cast<std::size_t>(pmc.WorkingSetSize);
    }
#elif defined(__linux__)
    // /proc/self/statm 第二列是常驻页数
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        long total_pages = 0;
        long resident_pages = 0;
        const int got = std::fscanf(f, "%ld %ld", &total_pages, &resident_pages);
        std::fclose(f);
        if (got == 2) {
            return static_cast<std::size_t>(resident_pages)
                 * static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        }
    }
#endif
    return 0;
}

struct Stats {
    std::atomic<long> opened{0};
    std::atomic<long> closed{0};
    std::atomic<long> tx{0};
    std::atomic<long> rx{0};
    std::atomic<long> bad{0};
    std::atomic<long> link_fail{0};
    std::atomic<long> read_err{0};
};

/// 服务端：原样回写。
class EchoHandler final : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        return ep->read({nullptr, m_rx.data(), m_rx.size()});
    }
    void on_read(std::error_code ec, Buf buf, std::size_t n) override
    {
        if (ec) {
            return;
        }
        if (!m_ep->write({nullptr, buf.data, n})) {
            m_ep->close();
        }
    }
    void on_write(std::error_code ec, ConstBuf, std::size_t) override
    {
        if (ec) {
            return;
        }
        (void)m_ep->read({nullptr, m_rx.data(), m_rx.size()});
    }
    void on_close() override { m_ep.reset(); }

private:
    std::shared_ptr<Conn>          m_ep;
    std::array<std::uint8_t, 2048> m_rx{};
};

class EchoServer final : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_accept(Conn&) override
    {
        return std::make_shared<EchoHandler>();
    }
};

/// 客户端：串行发 msgs 条，逐条校验，收完就关。
class SoakConn final : public ConnHandler {
public:
    SoakConn(int msgs, Stats* st) : m_msgs(msgs), m_st(st) {}

    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        m_st->opened.fetch_add(1);
        return send_next();
    }

    void on_read(std::error_code ec, Buf buf, std::size_t n) override
    {
        if (ec) {
            m_st->read_err.fetch_add(1);
            return;
        }
        // 校验内容：首字节是序号
        const auto expect = static_cast<std::uint8_t>(m_seq & 0xFF);
        if (n != kSize || static_cast<const std::uint8_t*>(buf.data)[0] != expect) {
            m_st->bad.fetch_add(1);
        }
        m_st->rx.fetch_add(1);

        ++m_seq;
        (void)send_next();
    }

    void on_write(std::error_code ec, ConstBuf, std::size_t) override
    {
        if (ec) {
            m_st->read_err.fetch_add(1);
        }
    }

    void on_close() override
    {
        m_st->closed.fetch_add(1);
        m_ep.reset();
    }

private:
    static constexpr std::size_t kSize = 64;

    bool send_next()
    {
        if (m_seq >= m_msgs || !m_ep || !m_ep->is_open()) {
            if (m_ep) {
                m_ep->close();
            }
            return true;
        }
        m_buf.fill(0);
        m_buf[0] = static_cast<std::uint8_t>(m_seq & 0xFF);
        if (!m_ep->write({nullptr, m_buf.data(), m_buf.size()})) {
            m_ep->close();
            return true;
        }
        m_st->tx.fetch_add(1);
        return m_ep->read({nullptr, m_rx.data(), m_rx.size()});
    }

    std::shared_ptr<Conn>          m_ep;
    int                            m_msgs;
    int                            m_seq = 0;
    Stats*                         m_st;
    std::array<std::uint8_t, kSize> m_buf{};
    std::array<std::uint8_t, 2048>  m_rx{};
};

class SoakFactory final : public Linkable {
public:
    SoakFactory(int msgs, Stats* st) : m_msgs(msgs), m_st(st) {}

    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        return std::make_shared<SoakConn>(m_msgs, m_st);
    }
    void on_linkfail(const Address&, std::error_code) override
    {
        m_st->link_fail.fetch_add(1);
    }

private:
    int    m_msgs;
    Stats* m_st;
};

} // namespace

int main(int argc, char** argv)
{
    const int duration = (argc > 1) ? std::atoi(argv[1]) : 60;
    const int conns    = (argc > 2) ? std::atoi(argv[2]) : 50;
    const int msgs     = (argc > 3) ? std::atoi(argv[3]) : 20;

    if (duration <= 0 || conns <= 0 || msgs <= 0) {
        std::fprintf(stderr, "usage: %s [duration_sec>0] [conns>0] [msgs>0]\n", argv[0]);
        return 2;
    }

    if (!initialize(4)) {
        std::fprintf(stderr, "initialize failed\n");
    }

    // ★ 长跑场景必须用 linger_zero=true（RST 关闭）。
    //
    //   `linger_zero=false`（库的默认）走优雅关闭，主动关闭方会进入 TIME_WAIT
    //   并占住本地端口约 2 分钟。本测试每轮 50 个新连接、每秒几十轮，
    //   几百轮后就会把 Windows 的 16384 个动态端口耗尽 —— 表现为
    //   `link_fail` 全量出现、一条数据都发不出去。
    //
    //   这是 TCP 的固有行为，不是库的缺陷；但选默认值时要在
    //   "数据不丢" 与 "端口回收速度" 之间权衡。
    Mechanism::Config cfg;
    cfg.linger_zero = true;
    auto mech = Mechanism::create(cfg);
    if (!mech) {
        std::fprintf(stderr, "create failed\n");
        return 1;
    }

    auto srv = std::make_shared<EchoServer>();
    // backlog 放大：每轮都有 conns 个连接同时到达
    if (auto ec = mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv), 4096)) {
        std::fprintf(stderr, "listen failed: %s\n", ec.message().c_str());
        return 1;
    }
    const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
    if (!bound) {
        std::fprintf(stderr, "no bound address\n");
        return 1;
    }

    // TIME_WAIT 端口预算：Windows 动态端口默认 16384 个，TIME_WAIT 保留 ~120 秒。
    // 因此建连速率必须低于 16384/120 ≈ 136 连接/秒，否则几百轮后端口耗尽
    // （表现为 link_fail 全量出现）。这里按该预算自动加轮间隔。
    const int min_gap_ms = std::max(0, static_cast<int>(1000.0 * conns / 130.0));
    std::printf("soak: %d s, %d conns/round, %d msgs/conn, gap %d ms"
                "  (limited by TIME_WAIT port budget)\n",
                duration, conns, msgs, min_gap_ms);
    std::printf("----------------------------------------------------------\n");
    std::printf("%-8s %-10s %-12s %-12s %-10s\n",
                "round", "msgs", "rss(MB)", "endpoints", "result");

    const std::size_t rss0     = rss_bytes();
    const auto        t_start  = Clock::now();
    const auto        t_end    = t_start + std::chrono::seconds(duration);

    long round       = 0;
    long total_msgs  = 0;
    long bad_rounds  = 0;
    std::size_t rss_last = rss0;

    while (Clock::now() < t_end) {
        auto st      = std::make_shared<Stats>();
        auto factory = std::make_shared<SoakFactory>(msgs, st.get());

        long accepted = 0;
        for (int i = 0; i < conns; ++i) {
            if (!mech->connect(*bound, std::weak_ptr<Linkable>(factory), 5000)) {
                ++accepted;
            }
        }

        // 等本轮全部关闭
        const auto deadline = Clock::now() + std::chrono::seconds(20);
        while (Clock::now() < deadline && st->closed.load() < accepted) {
            std::this_thread::sleep_for(5ms);
        }

        // 收敛检查：连接表是否回落到 0
        bool converged = false;
        for (int i = 0; i < 200; ++i) {
            if (mech->endpoint_count() == 0) {
                converged = true;
                break;
            }
            std::this_thread::sleep_for(5ms);
        }

        ++round;
        total_msgs += st->tx.load();

        const bool ok = converged
                     && st->closed.load() == accepted
                     && st->bad.load() == 0
                     && st->rx.load() == st->tx.load();
        if (!ok) {
            ++bad_rounds;
        }

        // 按端口预算限速：否则 TIME_WAIT 会在几百轮后耗尽本地动态端口
        if (min_gap_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(min_gap_ms));
        }

        // 每 10 轮打印一次（长跑时避免刷屏）
        if (round % 10 == 0 || !ok) {
            rss_last = rss_bytes();
            std::printf("%-8ld %-10ld %-12.2f %-12zu %s\n",
                        round, total_msgs,
                        static_cast<double>(rss_last) / (1024.0 * 1024.0),
                        mech->endpoint_count(),
                        ok ? "ok" : "FAIL");
            std::fflush(stdout);
        }

        // 失败时把各项摊开，便于定位（否则只知道"这一轮不对"）
        if (!ok) {
            std::printf("         detail: accepted=%ld closed=%ld tx=%ld rx=%ld "
                        "bad=%ld link_fail=%ld read_err=%ld converged=%d\n",
                        accepted, st->closed.load(), st->tx.load(), st->rx.load(),
                        st->bad.load(), st->link_fail.load(), st->read_err.load(),
                        converged ? 1 : 0);
            std::fflush(stdout);
        }
    }

    const auto   elapsed = std::chrono::duration<double>(Clock::now() - t_start).count();
    const std::size_t rss1 = rss_bytes();

    std::printf("----------------------------------------------------------\n");
    std::printf("rounds        : %ld\n", round);
    std::printf("total msgs    : %ld (%.0f msg/s)\n",
                total_msgs, elapsed > 0 ? total_msgs / elapsed : 0.0);
    std::printf("failed rounds : %ld\n", bad_rounds);
    std::printf("rss start/end : %.2f MB -> %.2f MB (delta %+.2f MB)\n",
                static_cast<double>(rss0) / (1024.0 * 1024.0),
                static_cast<double>(rss1) / (1024.0 * 1024.0),
                static_cast<double>(rss1 - rss0) / (1024.0 * 1024.0));
    std::printf("verdict       : %s\n", bad_rounds == 0 ? "PASS" : "FAIL");

    mech->close_all();
    mech.reset();
    return bad_rounds == 0 ? 0 : 1;
}
