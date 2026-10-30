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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <deque>
#include <mutex>

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

        // 持有 owner，保证回调前内存有效
        pending_read = buf;

        auto self = shared_from_this();
        port.async_read_some(
            asio::buffer(buf.data, buf.size),
            [self](const error_code_t& ec, std::size_t n) {
                self->read_pending.store(false);
                const Buf b = self->pending_read;
                self->pending_read = {};

                if (ec) {
                    self->handler->on_read(ec, b, 0);
                    self->finish_close();
                    return;
                }
                self->handler->on_read({}, b, n);
            });
        return true;
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

        // ★ 必须在锁外调 do_write：它内部还要拿 send_mtx（std::mutex 不可重入）。
        //   原来在这里直接调，锁没释放 → 自死锁，write() 永远不返回。
        if (need_start) {
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
                self->handler->on_write(ec, done, n);
                if (ec) {
                    self->finish_close();
                    return;
                }
                self->do_write();   // 继续下一笔
            });
    }

    // -------------------------------------------------------------------------
    //  关闭
    // -------------------------------------------------------------------------

    void finish_close()
    {
        if (closed_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        const bool was_open = open_.exchange(false, std::memory_order_acq_rel);
        error_code_t ec;
        port.close(ec);

        if (was_open && handler) {
            handler->on_close();
        }
    }

    void close() { finish_close(); }

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

    // 读
    std::atomic<bool> read_pending{false};
    Buf               pending_read{};

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
        m_impl->close();
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
        error_code_t ec;
        m_impl->port.cancel(ec);
    }
}

void SerialPort::flush_output()
{
    if (m_impl) {
        error_code_t ec;
        m_impl->port.cancel(ec);
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
        m_impl->close();
    }
}

} // namespace netmech
