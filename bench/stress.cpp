// =============================================================================
//  NetMechanism - 压力测试
// =============================================================================
//  文件    : bench/stress.cpp
//  用法    : stress [conns] [msgs_per_conn] [msg_size]
//            默认  100 连接 × 100 条 × 64 字节
//
//  它不是单元测试，是**故意压到出问题**的那种跑法：
//    * N 个客户端**同时**发起连接（不是串行）
//    * 每个连接串行发 M 条，逐条校验回显内容
//    * 全部关闭后统计收敛情况（连接表、内存）
//
//  关注三类失败：
//    1. 连接建立失败 / 超时
//    2. 回显内容错乱（长度或内容与发出不符 —— 说明写乱了或读串了）
//    3. 结束后连接表没回落（泄漏）
// =============================================================================
#include <netmech/net.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace netmech;

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Stats {
    std::atomic<long>   opened{0};      ///< 成功建立的连接数
    std::atomic<long>   closed{0};      ///< 已关闭的连接数
    std::atomic<long>   tx{0};          ///< 成功受理的写
    std::atomic<long>   rx{0};          ///< 收到的回显
    std::atomic<long>   bad_content{0}; ///< 内容/长度不符
    std::atomic<long>   write_rejected{0}; ///< 写被拒（反压）
    std::atomic<long>   read_error{0};  ///< 带错误码的读完成
    std::atomic<long>   link_fail{0};
    std::atomic<long>   rx_bytes{0};    ///< pipeline 模式下的回显字节总数   ///< 连接失败
};

/// 服务端：原样回写。
class EchoHandler : public ConnHandler {
public:
    /// @param[in] st   sink 模式下用来统计收到的字节；可为空
    /// @param[in] sink true = 只收不回（测发送侧上限）
    EchoHandler(Stats* st = nullptr, bool sink = false) : m_st(st), m_sink(sink) {}

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
        if (m_sink) {
            if (m_st) {
                m_st->rx_bytes.fetch_add(static_cast<long>(n));
            }
            (void)m_ep->read({nullptr, m_rx.data(), m_rx.size()});
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
    std::array<std::uint8_t, 4096> m_rx{};
    Stats*                         m_st = nullptr;
    bool                           m_sink = false;
};

class EchoServer : public Linkable {
public:
    EchoServer(Stats* st = nullptr, bool sink = false) : m_st(st), m_sink(sink) {}

    std::shared_ptr<ConnHandler> on_accept(Conn&) override
    {
        return std::make_shared<EchoHandler>(m_st, m_sink);
    }

private:
    Stats* m_st = nullptr;
    bool   m_sink = false;
};

/// 客户端：串行发 M 条，逐条校验回显。
class StressConn : public ConnHandler {
public:
    StressConn(int id, int msgs, std::size_t size, Stats* st,
               bool churn, bool pipeline, bool sink)
        : m_id(id)
        , m_msgs(msgs)
        , m_st(st)
        , m_churn(churn)
        , m_pipeline(pipeline)
        , m_sink(sink)
    {
        m_tx.assign(size, 0);
    }

    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        m_st->opened.fetch_add(1);
        if (m_churn) {
            // 连上立刻断：压 close 路径的竞态与收敛
            ep->close();
            return true;
        }
        if (m_sink || m_pipeline) {
            send_more();          // 反压时会在 on_write 里续发
            if (m_sink) {
                return true;      // sink：不读（对端不回），等写完再关
            }
            return ep->read({nullptr, m_rx.data(), m_rx.size()});
        }
        return send_next();
    }

    void on_read(std::error_code ec, Buf buf, std::size_t n) override
    {
        if (ec) {
            m_st->read_error.fetch_add(1);
            return;
        }
        m_st->rx_bytes.fetch_add(static_cast<long>(n));   // 所有模式都按字节统计
        if (m_pipeline) {
            // ★ TCP 是字节流：一次 read 可能带回**多条**消息，不能按"一次读=一条"计数。
            //   这里按字节累加，够总量就收工。
            m_rx_bytes += static_cast<long>(n);
            m_st->rx.fetch_add(1);
            if (m_rx_bytes >= static_cast<long>(m_msgs) * static_cast<long>(m_tx.size())) {
                m_ep->close();
            } else {
                if (m_seq < m_msgs) {
                    send_more();   // 反压后续发
                }
                (void)m_ep->read({nullptr, m_rx.data(), m_rx.size()});
            }
            return;
        }
        // 逐字节比对：发出去什么，回显就该是什么
        bool ok = (n == m_tx.size());
        if (ok) {
            const auto* p = static_cast<const std::uint8_t*>(buf.data);
            for (std::size_t i = 0; i < n; ++i) {
                if (p[i] != m_tx[i]) {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok) {
            m_st->bad_content.fetch_add(1);
        }
        m_st->rx.fetch_add(1);

        ++m_seq;
        (void)send_next();
    }

    void on_write(std::error_code ec, ConstBuf, std::size_t) override
    {
        if (ec) {
            m_st->read_error.fetch_add(1);
            return;
        }
        if (m_sink) {
            ++m_acks;
            if (m_seq < m_msgs) {
                send_more();      // 之前被反压挡住了，现在续发
            } else if (m_acks >= m_msgs && m_ep) {
                // 全部写完 → 优雅关闭（必须等 ack，否则数据可能还在用户态队列）
                m_ep->close();
            }
        }
    }

    void on_close() override
    {
        m_st->closed.fetch_add(1);
        m_ep.reset();
    }

private:
    /// 连发直到配额用尽（反压）或发完。
    ///
    /// ★ 反压不是失败：`write()` 返回 false 只表示"这一刻队列满了"，
    ///   应当**等 on_write 回调后从断点续发**，而不是直接放弃。
    ///   第一版在这里 break 掉，导致 500 连接时大量连接永远等不到"全部 ack"而卡死。
    void send_more()
    {
        while (m_seq < m_msgs) {
            if (!m_ep || !m_ep->is_open()) {
                return;
            }
            if (!send_one(m_seq)) {
                return;   // 配额满：等 on_write 再续
            }
            ++m_seq;
        }
    }

    /// 填一条并发出去；未受理返回 false。
    bool send_one(int seq)
    {
        if (!m_ep || !m_ep->is_open()) {
            return false;
        }
        m_tx[0] = static_cast<std::uint8_t>(seq & 0xFF);
        m_tx[1] = static_cast<std::uint8_t>(m_id & 0xFF);
        m_tx[2] = static_cast<std::uint8_t>((m_id >> 8) & 0xFF);
        if (!m_ep->write({nullptr, m_tx.data(), m_tx.size()})) {
            m_st->write_rejected.fetch_add(1);   // 只是记录发生过反压
            return false;
        }
        m_st->tx.fetch_add(1);
        return true;
    }

    bool send_next()
    {
        if (m_seq >= m_msgs || !m_ep || !m_ep->is_open()) {
            if (m_ep) {
                m_ep->close();
            }
            return true;
        }
        // 每条内容带序号与连接号，便于定位串包
        m_tx[0] = static_cast<std::uint8_t>(m_seq & 0xFF);
        m_tx[1] = static_cast<std::uint8_t>(m_id & 0xFF);
        m_tx[2] = static_cast<std::uint8_t>((m_id >> 8) & 0xFF);
        if (!m_ep->write({nullptr, m_tx.data(), m_tx.size()})) {
            m_st->write_rejected.fetch_add(1);
            m_ep->close();
            return true;
        }
        m_st->tx.fetch_add(1);
        return m_ep->read({nullptr, m_rx.data(), m_rx.size()});
    }

    std::shared_ptr<Conn>          m_ep;
    int                            m_id;
    int                            m_msgs;
    int                            m_seq = 0;
    long                           m_rx_bytes = 0;   ///< pipeline 模式按字节判完成
    Stats*                         m_st;
    bool                           m_churn = false;
    bool                           m_pipeline = false;
    bool                           m_sink = false;
    int                            m_acks = 0;
    std::vector<std::uint8_t>      m_tx;
    std::array<std::uint8_t, 4096> m_rx{};
};

class StressFactory : public Linkable {
public:
    StressFactory(int msgs, std::size_t size, Stats* st,
                  bool churn, bool pipeline, bool sink)
        : m_msgs(msgs), m_size(size), m_st(st)
        , m_churn(churn), m_pipeline(pipeline), m_sink(sink)
    {
    }

    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        return std::make_shared<StressConn>(m_seq++, m_msgs, m_size, m_st,
                                            m_churn, m_pipeline, m_sink);
    }

    void on_linkfail(const Address&, std::error_code) override
    {
        m_st->link_fail.fetch_add(1);
    }

private:
    int         m_msgs;
    std::size_t m_size;
    Stats*      m_st;
    bool        m_churn = false;
    bool        m_pipeline = false;
    bool        m_sink = false;
    int         m_seq = 0;
};

} // namespace

int main(int argc, char** argv)
{
    const int         conns = (argc > 1) ? std::atoi(argv[1]) : 100;
    const int         msgs  = (argc > 2) ? std::atoi(argv[2]) : 100;
    const std::size_t size  = (argc > 3) ? static_cast<std::size_t>(std::atoi(argv[3])) : 64;
    const bool        churn    = (argc > 4) && (std::atoi(argv[4]) != 0);
    const bool        pipeline = (argc > 5) && (std::atoi(argv[5]) != 0);
    const bool        sink     = (argc > 6) && (std::atoi(argv[6]) != 0);

    if (conns <= 0 || msgs <= 0 || size < 4) {
        std::fprintf(stderr, "usage: %s [conns>0] [msgs>0] [msg_size>=4]\n", argv[0]);
        return 2;
    }

    if (!initialize(4)) {
        std::fprintf(stderr, "initialize failed\n");
    }

    auto mech = Mechanism::create();
    if (!mech) {
        std::fprintf(stderr, "create failed\n");
        return 1;
    }

    Stats st;
    auto srv = std::make_shared<EchoServer>(&st, sink);
    // backlog 必须显式放大：默认 128 撑不住几百个同时到达的 SYN，
    // 会有一部分连接在握手阶段被拒（表现为 link_fail）。
    if (auto ec = mech->listen(Address("127.0.0.1", 0), std::weak_ptr<Linkable>(srv), 4096)) {
        std::fprintf(stderr, "listen failed: %s\n", ec.message().c_str());
        return 1;
    }
    const auto bound = mech->local_address_of(Address("127.0.0.1", 0));
    if (!bound) {
        std::fprintf(stderr, "no bound address\n");
        return 1;
    }

    auto  factory = std::make_shared<StressFactory>(msgs, size, &st, churn, pipeline, sink);
    if (churn) {
        std::printf("mode: churn (close immediately after open)\n");
    } else if (sink) {
        std::printf("mode: sink (server only receives, client closes after all writes acked)\n");
    } else if (pipeline) {
        std::printf("mode: pipeline (write without waiting for echo)\n");
    }

    std::printf("stress: %d conns x %d msgs x %zu bytes = %ld messages\n",
                conns, msgs, size, static_cast<long>(conns) * msgs);

    const auto t0 = Clock::now();

    // 一次性发起全部连接（并发，不串行等待）
    long accepted = 0;
    for (int i = 0; i < conns; ++i) {
        if (!mech->connect(*bound, std::weak_ptr<Linkable>(factory), 5000)) {
            ++accepted;
        }
    }
    std::printf("connect accepted: %ld / %d\n", accepted, conns);

    // 等全部连接关闭；顺便做**停滞检测** —— 卡住时立刻返回结果，不要干等超时。
    const auto deadline = t0 + std::chrono::seconds(120);
    long last_tx = -1;
    int  stall   = 0;
    while (Clock::now() < deadline) {
        if (st.closed.load() >= conns) {
            // 客户端全部关闭 ≠ 服务端已读完：给接收侧一点收尾时间，
            // 否则 sink 模式统计到的 rx_bytes 会偏小。
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            break;
        }
        const long cur = st.tx.load();
        if (cur == last_tx && cur < static_cast<long>(conns) * msgs) {
            if (++stall >= 300) {   // 3 秒没有任何进展
                std::printf("\n[stall] no progress for 3s at tx=%ld\n", cur);
                break;
            }
        } else {
            stall = 0;
            last_tx = cur;
        }
        std::this_thread::sleep_for(10ms);
    }

    const auto t1 = Clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();

    std::printf("\n--- result ---\n");
    std::printf("duration      : %.3f s\n", sec);
    std::printf("opened        : %ld / %d\n", st.opened.load(), conns);
    std::printf("closed        : %ld / %d\n", st.closed.load(), conns);
    std::printf("tx / rx       : %ld / %ld (expect %ld)\n",
                st.tx.load(), st.rx.load(), static_cast<long>(conns) * msgs);
    std::printf("bad_content   : %ld\n", st.bad_content.load());
    std::printf("write_rejected: %ld\n", st.write_rejected.load());
    std::printf("read_error    : %ld\n", st.read_error.load());
    std::printf("link_fail     : %ld\n", st.link_fail.load());
    // 发送按"受理条数/秒"，接收按"实际字节/秒" —— 批量提交后一次 read 可能带回多条，
    // 用 read 次数算吞吐会严重失真。
    std::printf("throughput    : %.0f msg/s (tx), %.2f MiB/s (rx)\n",
                sec > 0 ? st.tx.load() / sec : 0.0,
                sec > 0 ? static_cast<double>(st.rx_bytes.load()) / sec / (1024.0 * 1024.0) : 0.0);

    // 收敛检查：关闭全部之后连接表应当回落
    mech->close_all();
    bool converged = false;
    for (int i = 0; i < 500; ++i) {
        if (mech->endpoint_count() == 0) {
            converged = true;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    std::printf("endpoint_count: %zu (%s)\n", mech->endpoint_count(),
                converged ? "converged" : "STILL NON-ZERO");

    // churn: 不该有收发；pipeline: 受理 + 被拒 == 总量，且回显字节数 == 受理字节数；
    // 串行模式: 一条一个回显，先收后发严格配平。
    const long total = static_cast<long>(conns) * msgs;
    const bool send_ok = churn || (st.tx.load() == total);
    const bool recv_ok = churn ? (st.rx.load() == 0)
                        : (pipeline || sink)
                              ? (st.rx_bytes.load() >= st.tx.load() * static_cast<long>(size))
                              : (st.rx.load() == st.tx.load());
    const bool ok = (st.bad_content.load() == 0)
                 && (st.closed.load() == conns)
                 && send_ok && recv_ok
                 && converged;
    std::printf("verdict       : %s\n", ok ? "PASS" : "FAIL");

    mech.reset();
    return ok ? 0 : 1;
}
