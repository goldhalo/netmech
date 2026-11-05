// =============================================================================
//  serial.cpp —— 串口实现（基于 asio::serial_port）
// =============================================================================
//  Asio 的 serial_port 在 Windows 内部用 WaitCommEvent + 线程池、Linux 用
//  termios + poll，对外是统一的异步模型，可以直接挂在 io_context 上。
// =============================================================================
#include <netmech/serial.hpp>

#include "io_pool.hpp"
#include "netmech_srcpch.hpp"

#include <asio/serial_port.hpp>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <termios.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <deque>
#include <mutex>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

namespace netmech {

// =============================================================================
//  Impl
// =============================================================================

class SerialPort::Impl : public std::enable_shared_from_this<SerialPort::Impl> {
public:
    Impl(asio::io_context& io_ctx, IoPool& pool_ref,
         SerialConfig conf, std::shared_ptr<SerialHandler> h)
        : io(io_ctx)
        , port(io_ctx)
        , pool(&pool_ref)
        , cfg(std::move(conf))
        , handler(std::move(h))
    {
    }

    bool open(std::error_code& out_ec)
    {
        error_code_t ec;

        port.open(cfg.device, ec);
        if (ec) {
            out_ec = ec;
            return false;
        }

        // 帧格式
        port.set_option(asio::serial_port_base::baud_rate(cfg.baud_rate), ec);
        if (ec) {
            out_ec = ec;
            return false;
        }
        port.set_option(asio::serial_port_base::character_size(cfg.data_bits), ec);
        if (ec) {
            out_ec = ec;
            return false;
        }

        using parity_t = asio::serial_port_base::parity;
        parity_t::type p = parity_t::none;
        switch (cfg.parity) {
        case SerialParity::none: p = parity_t::none; break;
        case SerialParity::odd:  p = parity_t::odd;  break;
        case SerialParity::even: p = parity_t::even; break;
        }
        port.set_option(parity_t(p), ec);
        if (ec) {
            out_ec = ec;
            return false;
        }

        using stop_t = asio::serial_port_base::stop_bits;
        port.set_option(stop_t(cfg.stop_bits == SerialStopBits::two
                                   ? stop_t::two
                                   : stop_t::one), ec);
        if (ec) {
            out_ec = ec;
            return false;
        }

        port.set_option(asio::serial_port_base::flow_control(
                            cfg.flow_control
                                ? asio::serial_port_base::flow_control::hardware
                                : asio::serial_port_base::flow_control::none), ec);
        if (ec) {
            out_ec = ec;
            return false;
        }

        open_.store(true, std::memory_order_release);
        return true;
    }

    // -------------------------------------------------------------------------
    //  读
    // -------------------------------------------------------------------------

    bool start_read(Buf buf)
    {
        if (!open_.load(std::memory_order_acquire)) {
            return false;
        }
        if (buf.data == nullptr || buf.size == 0) {
            return false;
        }
        if (read_pending.exchange(true)) {
            return false;   // 已有一笔在途
        }
        arm_read(buf);
        return true;
    }

    /// 挂一笔读。**buf 由 lambda 自己持有**（拷贝进闭包），
    /// 不再放进共享成员 —— 那样"名额已释放、共享缓冲还没取走"的窗口里，
    /// 另一线程的 read() 会与 io 线程并发读写同一个 shared_ptr（P0-1）。
    void arm_read(Buf buf)
    {
        auto self = shared_from_this();
        port.async_read_some(
            asio::buffer(buf.data, buf.size),
            [self, buf](const error_code_t& ec, std::size_t n) {
                if (ec == asio::error::operation_aborted
                    && !self->closing.load(std::memory_order_acquire)) {
                    // 被 flush 之类的 cancel 打断：名额不释放，原样重挂这笔读。
                    // （关闭流程里的 abort 不在这里，见下面。）
                    self->arm_read(buf);
                    return;
                }

                self->read_pending.store(false);

                if (ec) {
                    invoke_void([&] { self->handler->on_read(ec, buf, 0); });
                    self->post_close();
                    return;
                }
                invoke_void([&] { self->handler->on_read({}, buf, n); });
            });
    }

    // -------------------------------------------------------------------------
    //  写
    // -------------------------------------------------------------------------

    bool enqueue_write(ConstBuf buf)
    {
        if (!open_.load(std::memory_order_acquire)) {
            return false;
        }
        if (buf.data == nullptr || buf.size == 0) {
            return false;
        }

        bool need_start = false;
        {
            std::lock_guard<std::mutex> lk(send_mtx);
            if (send_q.size() >= cfg.send_queue_count
                || queued_bytes + buf.size > cfg.send_queue_bytes) {
                return false;   // 队列满
            }
            send_q.push_back(buf);
            queued_bytes += buf.size;

            if (!writing) {
                writing = true;
                need_start = true;
            }
        }

        // 必须在锁外发起：do_write 内部还要拿 send_mtx（std::mutex 不可重入），
        // 原来在这里直接调 → 自死锁，write() 永远不返回。
        //
        // ⚠ **已知问题（未修）**：这里是在**调用线程**上发起
        //   `asio::async_write(port, ...)`，而 Asio 明确"shared objects: unsafe"
        //   —— io 线程可能正在跑在途 async_read_some 的完成处理（arm_read 重挂读），
        //   两线程同时碰同一个 serial_port 是标准的数据竞争。
        //   UDP 侧早就改成"投递到端点 io 线程"了，串口是当初漏掉的同类项。
        //
        //   试过改成 `asio::post(port.get_executor(), do_write)`：
        //   netmech 自己的测试全过，但 **modbusmech 的 RTU 链路立刻坏掉** ——
        //   `async_write` 返回成功（n=8、ec=Success）、pty master 端却收不到数据，
        //   直调就正常。同一个 asio，只差发起线程。
        //   没定位到根因前不硬上（RTU 是真实业务链路），所以**回退成直调**，
        //   把这个竞争留在原地并记录在案。
        //
        //   下一步排查方向：Linux pty 上 asio 的 reactive 写就绪通知
        //   （slave 端写 → master 端可读）在 io 线程上是否被 in-flight 的
        //   async_read_some 影响；或者确认是不是 termios/ECHO 造成的假象。
        if (need_start) {
            auto self = shared_from_this();
            do_write();
        }
        return true;
    }

    void do_write()
    {
        ConstBuf front{};
        {
            std::lock_guard<std::mutex> lk(send_mtx);
            if (send_q.empty()) {
                writing = false;
                return;
            }
            front = send_q.front();
        }

        auto self = shared_from_this();
        asio::async_write(
            port, asio::buffer(front.data, front.size),
            [self](const error_code_t& ec, std::size_t n) {
                ConstBuf done{};
                {
                    std::lock_guard<std::mutex> lk(self->send_mtx);
                    if (!self->send_q.empty()) {
                        done = self->send_q.front();
                        self->send_q.pop_front();
                        self->queued_bytes -= done.size;
                    }
                }
                invoke_void([&] { self->handler->on_write(ec, done, n); });

                if (ec == asio::error::operation_aborted
                    && !self->closing.load(std::memory_order_acquire)) {
                    // flush_output 取消掉的写：这一笔的数据确实丢了（已回调告知），
                    // 但端口不该因此关闭 —— 继续发后面排队的。
                    self->do_write();
                    return;
                }
                if (ec) {
                    self->post_close();
                    return;
                }
                self->do_write();   // 继续下一笔
            });
    }

    // -------------------------------------------------------------------------
    //  关闭
    // -------------------------------------------------------------------------

    /// 对外入口：关闭串口。
    ///
    /// 分两段：
    ///   · **调用线程同步**翻 `closed_` / `open_` —— `close()` 返回后
    ///     `is_open()` 立刻是 false、`write()` 立刻返回 false（既有语义，测试在断言它）；
    ///   · **投到 io 线程**做真正碰 `serial_port` 的那一步（`port.close()` + `on_close`）。
    ///
    /// 为什么不能在这里直调 `port.close()`：它与在途的 async_read/write 是**同一个
    /// 对象**，Asio 明确"shared objects: unsafe"；而且 `on_close` 会落在调用线程上，
    /// 破 serial.hpp 的"回调都在串口 io 线程"约定。
    ///
    /// 用 dispatch：已经在 io 线程上时就地执行（读/写错误触发的关闭走这条路）。
    void post_close() noexcept
    {
        if (closed_.exchange(true, std::memory_order_acq_rel)) {
            return;   // 幂等
        }
        // 先置 closing 再翻 open：完成回调靠它区分
        // "端口在关"（abort 属正常收敛）与 "flush 打断"（要重挂）。
        closing.store(true, std::memory_order_release);
        const bool was_open = open_.exchange(false, std::memory_order_acq_rel);

        std::shared_ptr<Impl> self;
        try {
            self = shared_from_this();
        } catch (...) {
            return;   // 不是 shared_ptr 管理（不该发生）：交给 port 析构兜底
        }

        auto fn = [self, was_open] { self->close_on_io(was_open); };
        try {
            asio::dispatch(port.get_executor(), fn);
        } catch (...) {
            // io_context 已停：此时必然没有并发的在途操作（io 线程已退出），
            // 直接关是安全的。
            try { fn(); } catch (...) {}
        }
    }

    /// 真正碰 `serial_port` 的那一步（**只在 io 线程上**）。
    void close_on_io(bool was_open) noexcept
    {
        error_code_t ec;
        try {
            port.close(ec);
        } catch (...) {
        }
        if (was_open && handler) {
            invoke_void([&] { handler->on_close(); });
        }
    }

    /// flush 清缓冲：投递到 io 线程，用平台 API 只清队列、**不 cancel 在途 IO**。
    void post_flush(bool input)
    {
        auto self = shared_from_this();
        asio::post(port.get_executor(), [self, input] {
            if (!self->port.is_open()) {
                return;
            }
#if defined(_WIN32)
            ::PurgeComm(reinterpret_cast<HANDLE>(self->port.native_handle()),
                        input ? PURGE_RXCLEAR : PURGE_TXCLEAR);
#else
            ::tcflush(self->port.native_handle(), input ? TCIFLUSH : TCOFLUSH);
#endif
        });
    }


    // -------------------------------------------------------------------------
    //  其它
    // -------------------------------------------------------------------------

    bool do_drain(std::error_code& out_ec)
    {
        if (!open_.load(std::memory_order_acquire)) {
            out_ec = make_boost_error_code(NetErrc::link_closed);
            return false;
        }

        // Asio 没有暴露 tcdrain / FlushFileBuffers，所以没法"精确等到最后一个
        // 字节移出移位寄存器"。
        //
        // 这里按波特率**估算**一个字（10 bit：1 起始 + 8 数据 + 1 停止）的
        // 传输时间，等这么长就认为发完了。对 RS-485 方向控制够用：
        // 9600bps 下一个字节约 1.04ms，等 2ms 有余量。
        //
        // 想更精确的话，应该在应用层用平台的 tcdrain/FlushFileBuffers。
        const double bits_per_char = 1.0 + static_cast<double>(cfg.data_bits)
                                     + (cfg.stop_bits == SerialStopBits::two ? 2.0 : 1.0)
                                     + (cfg.parity == SerialParity::none ? 0.0 : 1.0);
        const double ms_per_char = bits_per_char * 1000.0
                                   / static_cast<double>(cfg.baud_rate == 0u
                                                             ? 9600u
                                                             : cfg.baud_rate);
        const auto wait_ms = std::max(1, static_cast<int>(ms_per_char * 2.0 + 0.5));

        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        out_ec.clear();
        return true;
    }

    asio::io_context&              io;
    asio::serial_port              port;
    IoPool*                        pool;
    SerialConfig                   cfg;
    std::shared_ptr<SerialHandler> handler;

    std::atomic<bool> open_{false};
    std::atomic<bool> closed_{false};
    std::atomic<bool> closing{false};   ///< 关闭流程已开始（区分 abort 的来源）

    // 读：在途名额（缓冲本身由完成回调的闭包持有，不在这里）
    std::atomic<bool> read_pending{false};

    // 写
    std::mutex        send_mtx;
    std::deque<ConstBuf> send_q;
    std::size_t       queued_bytes = 0;
    bool              writing = false;

};

// =============================================================================
//  SerialPort 门面
// =============================================================================

SerialPort::SerialPort() = default;

SerialPort::~SerialPort()
{
    if (m_impl) {
        // 析构线程上不能直调 port.close()（与在途 IO 同对象竞争）；
        // post_close 会投到 io 线程。若 io 线程已停，它会在 catch 里兜底同步关。
        m_impl->post_close();
    }
}

std::shared_ptr<SerialPort> SerialPort::create(const SerialConfig&      cfg,
                                               std::shared_ptr<SerialHandler> handler,
                                               std::error_code&         ec)
{
    ec.clear();
    if (!handler) {
        ec = make_boost_error_code(NetErrc::invalid_argument);
        return nullptr;
    }
    if (cfg.device.empty() || cfg.data_bits < 5 || cfg.data_bits > 8) {
        ec = make_boost_error_code(NetErrc::invalid_argument);
        return nullptr;
    }

    auto sp = std::shared_ptr<SerialPort>(new SerialPort());
    auto impl = std::make_shared<Impl>(IoPool::global().next_io_context(),
                                       IoPool::global(), cfg, std::move(handler));
    if (!impl->open(ec)) {
        return nullptr;
    }
    sp->m_impl = impl;
    return sp;
}

bool SerialPort::read(Buf buf)
{
    return m_impl && m_impl->start_read(buf);
}

bool SerialPort::write(ConstBuf buf)
{
    return m_impl && m_impl->enqueue_write(buf);
}

bool SerialPort::drain(std::error_code& ec)
{
    return m_impl && m_impl->do_drain(ec);
}

void SerialPort::flush_input()
{
    if (m_impl) {
        m_impl->post_flush(/*input=*/true);
    }
}

void SerialPort::flush_output()
{
    if (m_impl) {
        m_impl->post_flush(/*input=*/false);
    }
}

bool SerialPort::is_open() const noexcept
{
    return m_impl && m_impl->open_.load(std::memory_order_acquire);
}

std::uint64_t SerialPort::id() const noexcept
{
    if (!m_impl) {
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(m_impl.get());
}

const std::string& SerialPort::device() const noexcept
{
    static const std::string kEmpty;
    return m_impl ? m_impl->cfg.device : kEmpty;
}

void SerialPort::close() noexcept
{
    if (m_impl) {
        m_impl->post_close();
    }
}

} // namespace netmech
