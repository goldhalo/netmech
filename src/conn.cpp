// =============================================================================
//  NetMechanism - Conn 实现
// =============================================================================
//  文件    : endpoint.cpp
//  用途    : public Conn 门面 + 内部状态机实现。设计要点见 conn_state.hpp。
//  编码    : UTF-8
//
//  ★ 本文件必须守住的不变量（验收点）
//  -----------------------------------
//  [V1] 所有进入用户代码的位置都经过 invoke_user / invoke_void
//       （异常绝不逃逸到 io_context，防止 std::terminate）。
//  [V2] 所有回调都在本 Conn 所在的 io 线程上串行执行。
//  [V3] 一次 async_* 返回 true ⇔ 恰好调用一次回调；返回 false ⇒ 绝不调用。
//  [V4] close() 幂等；关闭时把未完成的读/写全部以非 0 错误码回调一次。
//  [V5] 发起 IO 的判断只依赖显式 in_flight 标记，不依赖队列长度。
//  [V6] 用户回调**一律在内部状态更新完成之后**派发：回调里重入 async_* 永远
//       看到一致状态。
//  [V7] 回调过程中可能发生 close()（用户主动或内部错误），因此每次派发前后
//       都要检查 closed。
//
//  ★ "受理返回 true 但随后立刻被 close" 的处理
//  -------------------------------------------
//  公共头约定：返回 true ⇒ 回调恰好一次；返回 false ⇒ 回调绝不发生。
//  受理与 io 线程执行之间可能插入 close()，此时投递的 lambda 仍会执行，
//  由 `do_close` 统一把投递队列里的请求以非 0 错误码回调一次 —— 满足 [V3]。
// =============================================================================
#include "netmech_srcpch.hpp"
#include <cstdio>

#include "conn_state.hpp"
#include "address_internal.hpp"

#include "netmech/conn.hpp"
#include "netmech/common.hpp"
#include "io_pool.hpp"

namespace netmech {

namespace {
/// 一批最多合并几笔写请求。
constexpr std::size_t kMaxWriteBatch = 16;
/// 一批最多提交多少字节（限制单次系统调用的数据量，避免长尾延迟）。
constexpr std::size_t kMaxWriteBatchBytes = 64 * 1024;
} // namespace


namespace {

/// 把 close 原因归一化为非 0 错误码（保证关闭时回调能看到非 0）。
error_code_t normalize_close_error(const error_code_t& ec) noexcept
{
    if (ec) {
        return ec;
    }
    return asio::error::operation_aborted;
}

} // namespace

// =============================================================================
// Conn::Impl：构造/析构与通用投递
// =============================================================================

Conn::Impl::Impl(socket_t&& sock,
                             io_context_t& ioc,
                             IoPool& pool_in,
                             std::weak_ptr<Conn> owner_in,
                             endpoint_id_t ep_id,
                             endpoint_t local,
                             endpoint_t remote,
                             std::shared_ptr<void> user_ctx)
    : pool(&pool_in)
    , socket(std::move(sock))
    , id(ep_id)
    , local_addr(local)
    , remote_addr(remote)
    , owner(std::move(owner_in))
    , user_context(std::move(user_ctx))
    , recv_ka(ioc)
    , send_ka(ioc)
{
}

Conn::Impl::~Impl()
{
    // 走到这里说明所有**在途异步操作**都已结束。
    //
    // ★ 析构顺序的约束（关键）：
    //   `socket` 与两个 timer 的析构都会**访问它们所属的 io_context**。
    //   若此刻 io_context 已被 stop 或已开始析构，就是"在销毁 A 的过程中
    //   依赖 A" —— 表现为退出期 0xC0000005。
    //   因此这里先关闭 socket / 取消定时器，但**不**做任何阻塞等待：
    //   IO 对象（socket、两个 timer）与 `pool` 的析构顺序由成员声明顺序
    //   决定（`pool` 声明在 socket 之前 ⇒ 析构时先析构 recv_ka/send_ka、
    //   再析构 socket、最后 pool）。只要保证"先丢弃 Conn 的强引用、
    //   再拆 io 池"，socket 的析构就发生在 io_context 存活期间。
    error_code_t ignored;
    recv_ka.timer.cancel(ignored);
    send_ka.timer.cancel(ignored);
    // linger_zero=true 表示立刻断：必须跳过 shutdown —— 否则 FIN 先发出去，
    // SO_LINGER{1,0} 的 RST 就发不成，端口照样进 TIME_WAIT。
    { static std::atomic<int> dbg{0};
      if (dbg.fetch_add(1) < 3) {
          std::fprintf(stderr, "[dbg] close linger_zero=%d graceful=%d\n", (int)linger_zero, (int)!linger_zero);
      } }
    close_socket_noexcept(socket, /*graceful=*/!linger_zero);

    // 兜底摘除：若某个路径没能走到 do_close（理论上不应发生），这里保证
    // Mechanism 的连接表不会残留一个已失效的 id（否则 endpoint_count()
    // 会永久偏大）。fire_unregister 自身幂等。
    fire_unregister();
}

std::shared_ptr<Conn::Impl> Conn::Impl::self_ref() noexcept
{
    try {
        return shared_from_this();
    } catch (...) {
        return std::shared_ptr<Conn::Impl>();
    }
}

std::shared_ptr<Conn> Conn::Impl::public_self() const noexcept
{
    try {
        return owner.lock();
    } catch (...) {
        return std::shared_ptr<Conn>();
    }
}

void Conn::Impl::post(std::function<void()> fn)
{
    if (!fn) {
        return;
    }
    asio::post(socket.get_executor(), [fn = std::move(fn)]() mutable {
        try {
            fn();
        } catch (...) {
            // 内部任务本身不会抛；这是最后一道防线，绝不 terminate。
        }
    });
}

// =============================================================================
// 写：受理 / 排队 / 泵 / 完成
// =============================================================================

bool Conn::Impl::accept_write(ConstBuf buf)
{
    if (buf.data == nullptr || buf.size == 0) {
        return false;                              // invalid_argument
    }
    if (closing.load(std::memory_order_acquire)) {
        return false;                              // link_closed / not_connected
    }

    const std::size_t bytes = buf.size;

    // 关键临界区：配额预留与"投递到 io_context 的排队顺序"在**同一把锁**内完成。
    // 这样并发调用者之间的 FIFO 顺序被确定，且 `write()` 能同步返回准确结果。
    std::lock_guard<std::mutex> lock(quota_mutex);

    // 锁内复查 closing：close 会把 closed 置位后再加本锁清账，因此这里看到
    // false 就意味着 close 还没开始清账，本次受理有效。
    if (closing.load(std::memory_order_acquire)) {
        return false;
    }
    if (limit_bytes != 0 && reserved_bytes + bytes > limit_bytes) {
        return false;                              // buffer_full：**不断链**
    }
    if (limit_count != 0 && reserved_count + 1 > limit_count) {
        return false;                              // buffer_full：**不断链**
    }
    reserved_bytes += bytes;
    ++reserved_count;

    try {
        // ★ 强自持：任务捕获 Impl 的强引用 → 连接活到任务执行，
        //   关闭流程（冲刷挂起回调 / on_close / on_linkdown）必定执行。
        std::shared_ptr<Conn::Impl> state = self_ref();
        if (!state) {
            reserved_bytes -= bytes;
            --reserved_count;
            return false;
        }
        // buf 按值捕获：owner 的引用计数 +1，库因此持有这块内存直到完成回调。
        asio::post(socket.get_executor(),
                   [impl = std::move(state), buf, bytes]() mutable {
                       impl->do_accept_write(std::move(buf), bytes);
                   });
    } catch (...) {
        // 投递失败（io_context 已停止）：回滚配额，按未受理返回。
        reserved_bytes -= bytes;
        --reserved_count;
        return false;
    }
    return true;
}

void Conn::Impl::do_accept_write(ConstBuf buf, std::size_t bytes)
{
    if (closing.load(std::memory_order_acquire)) {
        // 受理后、执行前被 close：仍须恰好一次通知（[V3]）。
        notify_write(normalize_close_error(close_reason), buf, std::size_t{0});
        return;
    }

    WriteQueue::Request req;
    req.buf   = std::move(buf);
    req.bytes = bytes;
    writes.push(std::move(req));
    pump_write();
}

void Conn::Impl::pump_write()
{
    // 显式状态机：`in_flight` 为真说明有一批在飞，不再发起；队列空则无事可做。
    // **绝不使用"队列长度==1"之类的隐式判据**（[V5]，旧实现 A-03 的根因）。
    if (writes.in_flight || writes.empty()) {
        return;
    }
    if (closing.load(std::memory_order_acquire)) {
        return;
    }

    // ---- 批量组装：队首连续若干笔，合成一个 buffer sequence ----
    //
    // 为什么值得这么做：小包高频场景下，每笔一次系统调用的开销（~2–5 μs）
    // 会直接吃掉吞吐。合并成一次 writev/WSASend 后，系统调用次数降一个量级。
    //
    // 截断规则：最多 kMaxWriteBatch 笔、且累计不超过 kMaxWriteBatchBytes。
    // 后者的作用是限制单次提交的数据量，避免队首几笔特别大时形成长尾延迟。
    std::array<asio::const_buffer, kMaxWriteBatch> bufs{};
    std::size_t count = 0;
    std::size_t total = 0;

    for (const auto& req : writes.pending) {
        if (count == kMaxWriteBatch) {
            break;
        }
        const std::size_t left = req.bytes - req.sent;
        if (left == 0) {
            continue;   // 已写完（理论上不该出现，防御）
        }
        if (count > 0 && total + left > kMaxWriteBatchBytes) {
            break;
        }
        bufs[count++] = asio::const_buffer(
            static_cast<const std::uint8_t*>(req.buf.data) + req.sent, left);
        total += left;
    }
    if (count == 0) {
        return;
    }

    writes.in_flight = true;
    writes.in_flight_count = count;

    try {
        // ★ 强自持：完成处理捕获 Impl 的强引用。
        std::shared_ptr<Conn::Impl> self = self_ref();
        if (!self) {
            writes.in_flight = false;
            writes.in_flight_count = 0;
            return;
        }
        // ★ 这里必须把 `bufs` **当成 buffer sequence** 传进去，
        //   不能写 `asio::buffer(bufs.data(), count)` —— 那会构造一个
        //   "指向 const_buffer 数组的字节视图"（把 count 当字节数），
        //   发出去的就是 buffer 对象自身的内存，接收端全是乱码。
        //   数组里未使用的元素是零长度 buffer，asio 会自动跳过。
        //
        //   bufs 必须活到完成回调：按值捕获（16 × 16B，无堆分配）。
        asio::async_write(
            socket, bufs,
            [impl = std::move(self), bufs](const error_code_t& ec, std::size_t n) mutable {
                (void)bufs;   // 仅为延长生命周期，不参与计算
                impl->handle_write_done(ec, n);
            });
    } catch (...) {
        // 发起失败（socket 已失效等）：按一次"写失败"走统一错误路径。
        // ★ 不要在这里清 in_flight —— 交给 handle_write_done 收尾。
        handle_write_done(asio::error::bad_descriptor, std::size_t{0});
    }
}

void Conn::Impl::handle_write_done(const error_code_t& ec, std::size_t n)
{
    if (!writes.in_flight) {
        return;   // 防御：状态不一致时直接忽略
    }
    writes.in_flight = false;
    const std::size_t batch = writes.in_flight_count;
    writes.in_flight_count = 0;

    // ---- 按字节数逐笔归因 ----
    //
    // 一次 async_write 只回调一次，n 是**这次系统调用实际交付内核的总字节数**。
    // 队列按 FIFO 顺序被依次填满，所以从队首开始扣 n：
    //   · 某一笔的剩余量 ≤ 剩余 n → 这一笔完整写完，出队并记录
    //   · 否则 → 这一笔只写出一部分，推进 `sent`，下次从断点续传
    //
    // 归因必须是"逐笔"的：契约要求发了几笔就回调几次，不能合并成一次回调 ——
    // 否则调用方无法知道哪块缓冲可以释放。
    std::vector<WriteQueue::Request> finished;
    finished.reserve(batch);

    std::size_t remaining = n;
    while (!writes.pending.empty() && finished.size() < batch) {
        WriteQueue::Request& front = writes.pending.front();
        const std::size_t left = front.bytes - front.sent;
        if (remaining >= left) {
            remaining -= left;
            finished.push_back(std::move(front));
            writes.pending.pop_front();
        } else {
            front.sent += remaining;
            remaining = 0;
            break;
        }
    }

    // 配额与统计：只按"完整写完"的笔计（部分写的那笔仍占着配额）。
    for (const auto& req : finished) {
        {
            std::lock_guard<std::mutex> lock(quota_mutex);
            reserved_bytes = (reserved_bytes >= req.bytes) ? (reserved_bytes - req.bytes) : 0;
            reserved_count = (reserved_count > 0) ? (reserved_count - 1) : 0;
        }
        bytes_written.fetch_add(static_cast<std::uint64_t>(req.bytes), std::memory_order_relaxed);
        if (req.bytes > 0) {
            send_ka.progressed.store(true, std::memory_order_release);
        }
    }

    const bool was_closed = closing.load(std::memory_order_acquire);

    // ---- 逐笔回调：发了几笔就回调几次（契约"受理即恰好一次"）----
    for (auto& req : finished) {
        if (was_closed) {
            notify_write(normalize_close_error(close_reason), req.buf, req.bytes);
        } else {
            notify_write(std::error_code{}, req.buf, req.bytes);
        }
    }

    // 出错 → 关闭（关闭流程会把队列里剩余请求统一以错误归还）
    if (ec) {
        if (!closing.load(std::memory_order_acquire)) {
            fail_connection(ec);
        }
        return;
    }

    // 未出错 → 队列里还有货就继续泵
    if (!closing.load(std::memory_order_acquire)) {
        pump_write();
    }
}

void Conn::Impl::notify_write(const error_code_t& ec, const ConstBuf& buf,
                                  std::size_t n) noexcept
{
    if (!handler || !handler_inited) {
        return;
    }
    // 复制一份强引用：回调里若打开新的写，不会影响本次调用。
    std::shared_ptr<ConnHandler> h = handler;
    try {
        h->on_write(ec, buf, n);
    } catch (...) {
        // 契约要求回调不抛异常；这里兜底，避免异常穿过 io 线程。
    }
}

void Conn::Impl::notify_idle(const IdleInfo& info) noexcept
{
    if (!handler || !handler_inited) {
        return;
    }
    std::shared_ptr<ConnHandler> h = handler;
    try {
        h->on_idle(info);
    } catch (...) {
        // 同上：回调不得抛异常。
    }
}

// =============================================================================
// 读：受理 / 完成
// =============================================================================

bool Conn::Impl::accept_read(Buf buf, ReadMode mode)
{
    if (buf.data == nullptr || buf.size == 0) {
        return false;                              // invalid_argument
    }
    if (closing.load(std::memory_order_acquire)) {
        return false;                              // link_closed
    }

    // 同一时刻只允许一笔读在途（公共契约）。CAS 保证并发调用者中只有一个成功。
    bool expected = false;
    if (!has_pending_read.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
        return false;                              // 已有读在途
    }

    try {
        // ★ 强自持：同 accept_write（任务持有强引用 → 连接活到任务执行）。
        std::shared_ptr<Conn::Impl> state = self_ref();
        if (!state) {
            has_pending_read.store(false, std::memory_order_release);
            return false;
        }
        asio::post(socket.get_executor(),
                   [impl = std::move(state), buf, mode]() mutable {
                       impl->do_accept_read(std::move(buf), mode);
                   });
    } catch (...) {
        has_pending_read.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void Conn::Impl::do_accept_read(Buf buf, ReadMode mode)
{
    if (closing.load(std::memory_order_acquire)) {
        has_pending_read.store(false, std::memory_order_release);
        // 受理后、执行前被 close：仍须恰好一次（[V3]）。
        notify_read(normalize_close_error(close_reason), buf, std::size_t{0});
        return;
    }

    ReadQueue::Request req;
    req.buf   = std::move(buf);
    req.exact = (mode == ReadMode::exact);
    reads.in_flight.emplace(std::move(req));

    const bool use_exact = (mode == ReadMode::exact);
    const asio::mutable_buffer wire(reads.in_flight->buf.data, reads.in_flight->buf.size);

    try {
        // ★ 强自持：完成处理捕获 Impl 的强引用（同 pump_write）。
        std::shared_ptr<Conn::Impl> self = self_ref();
        if (!self) {
            std::optional<ReadQueue::Request> failed = reads.take_in_flight();
            has_pending_read.store(false, std::memory_order_release);
            if (failed.has_value()) {
                notify_read(normalize_close_error(close_reason), failed->buf, std::size_t{0});
            }
            return;
        }
        if (use_exact) {
            asio::async_read(
                socket, wire,
                [impl = std::move(self)](const error_code_t& ec, std::size_t n) mutable {
                    impl->handle_read_done(ec, n, true);
                });
        } else {
            socket.async_read_some(
                wire,
                [impl = std::move(self)](const error_code_t& ec, std::size_t n) mutable {
                    impl->handle_read_done(ec, n, false);
                });
        }
    } catch (...) {
        // 发起失败：**取回**在途请求并以错误通知一次（已受理 ⇒ 必有通知）。
        //
        // ★ 顺序很重要：必须先 take 再 reset。早期写法先 reset()，Request 随之
        //   被销毁 → 用户的读通知被**静默丢弃**，违反"已受理 ⇒ 恰好一次"。
        std::optional<ReadQueue::Request> failed = reads.take_in_flight();
        has_pending_read.store(false, std::memory_order_release);
        if (failed.has_value()) {
            notify_read(normalize_close_error(close_reason), failed->buf, std::size_t{0});
        }
        return;
    }
}

void Conn::Impl::handle_read_done(const error_code_t& ec, std::size_t n, bool exact)
{
    (void)exact;
    if (!reads.in_flight.has_value()) {
        return;   // 关闭流程已取走槽位（通知由关闭流程负责）
    }

    // 取走在途请求：之后所有判断都基于"已出队"的确定状态（[V5]/[V6]）。
    std::optional<ReadQueue::Request> req = reads.take_in_flight();
    has_pending_read.store(false, std::memory_order_release);

    if (!ec) {
        bytes_read.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
        if (n > 0) {
            // 只有真正读到数据才算接收方向有进展（保活语义）。
            recv_ka.progressed.store(true, std::memory_order_release);
        }
    }

    const bool was_closed = closing.load(std::memory_order_acquire);

    if (!was_closed) {
        notify_read(ec, req->buf, n);
    } else {
        notify_read(normalize_close_error(close_reason), req->buf, std::size_t{0});
    }

    // 出错 → 关闭（若用户回调里没有先行关闭）。
    if (ec) {
        if (!closing.load(std::memory_order_acquire)) {
            fail_connection(ec);
        }
    }
}

void Conn::Impl::notify_read(const error_code_t& ec, const Buf& buf,
                                 std::size_t n) noexcept
{
    if (!handler || !handler_inited) {
        return;
    }
    std::shared_ptr<ConnHandler> h = handler;
    try {
        h->on_read(ec, buf, n);
    } catch (...) {
        // 契约要求回调不抛异常；这里兜底，避免异常穿过 io 线程。
    }
}

// =============================================================================
// 保活
// =============================================================================

// -----------------------------------------------------------------------------
//  保活投递
// -----------------------------------------------------------------------------
//  ★ 这四处曾经只捕获裸 `this`（Impl*），存在 UAF 窗口：
//    任务入队后、执行前，`Impl` 可能因用户释放 Conn 而析构，
//    任务执行时解引用已释放的 Impl。
//
//    `Impl::post()` 本身不持有 Impl 的 shared_ptr（它只持有 std::function），
//    所以必须由**调用方**保证存活。
//
//    修复：任务捕获 Impl 的**强引用**，把它的生命延长到任务执行完毕。
//    这样保活配置**不会因为"连接恰好先没了"而丢失**。
// -----------------------------------------------------------------------------

void Conn::Impl::post_keepalive_recv(std::chrono::milliseconds delay,
                                        std::chrono::milliseconds interval,
                                        std::uint32_t max_miss)
{
    // ★ 强自持：任务捕获 Impl 的**强引用** —— 保活配置一定生效，
    //   不会因为"连接恰好先没了"而丢失。
    std::shared_ptr<Conn::Impl> self = self_ref();
    if (!self) {
        return;
    }
    post([impl = std::move(self), delay, interval, max_miss]() mutable {
        impl->do_apply_recv_keepalive(delay, interval, max_miss);
    });
}

void Conn::Impl::post_keepalive_recv_disable()
{
    std::shared_ptr<Conn::Impl> self = self_ref();
    if (!self) {
        return;
    }
    post([impl = std::move(self)]() mutable {
        impl->do_disable_recv_keepalive();
    });
}

void Conn::Impl::post_keepalive_send(std::chrono::milliseconds delay,
                                        std::chrono::milliseconds interval,
                                        std::uint32_t max_miss)
{
    std::shared_ptr<Conn::Impl> self = self_ref();
    if (!self) {
        return;
    }
    post([impl = std::move(self), delay, interval, max_miss]() mutable {
        impl->do_apply_send_keepalive(delay, interval, max_miss);
    });
}

void Conn::Impl::post_keepalive_send_disable()
{
    std::shared_ptr<Conn::Impl> self = self_ref();
    if (!self) {
        return;
    }
    post([impl = std::move(self)]() mutable {
        impl->do_disable_send_keepalive();
    });
}

void Conn::Impl::do_apply_recv_keepalive(std::chrono::milliseconds delay,
                                            std::chrono::milliseconds interval,
                                            std::uint32_t max_miss)
{
    // 非法参数（interval <= 0 / max_miss == 0 / delay < 0）静默忽略，保持原状。
    if (interval.count() <= 0 || max_miss == 0 || delay.count() < 0) {
        return;
    }
    if (closing.load(std::memory_order_acquire)) {
        return;
    }

    // 显式复位全部状态（旧实现 m_hasRecvData / m_recvTimeCurCnt 漏清零 → B-13）。
    recv_ka.reset();
    recv_ka.active = true;
    recv_ka.delay = delay;
    recv_ka.interval = interval;
    recv_ka.max_miss = max_miss;
    recv_ka.miss = 0;

    error_code_t ignored;
    recv_ka.timer.cancel(ignored);

    // 首拍 = delay + interval（与旧实现语义一致：delay 之后按 interval 检查）。
    recv_ka.timer.expires_after(delay + interval);
    const std::uint64_t gen = recv_ka.generation;
    // ★ 强自持：定时器回调捕获 Impl 的**强引用** —— 连接活到这一拍结束。
    std::shared_ptr<Conn::Impl> self = self_ref();
    if (!self) {
        return;
    }
    recv_ka.timer.async_wait([impl = std::move(self), gen](const error_code_t& ec) mutable {
        impl->on_recv_keepalive_tick(ec, gen);
    });
}

void Conn::Impl::do_disable_recv_keepalive()
{
    if (!recv_ka.active) {
        return;
    }
    recv_ka.reset();                        // 显式复位（含代次自增）
    error_code_t ignored;
    recv_ka.timer.cancel(ignored);
}

void Conn::Impl::do_apply_send_keepalive(std::chrono::milliseconds delay,
                                            std::chrono::milliseconds interval,
                                            std::uint32_t max_miss)
{
    // 参数非法（interval <= 0 / delay < 0 / max_miss == 0）时静默忽略，保持原状。
    if (interval.count() <= 0 || delay.count() < 0 || max_miss == 0) {
        return;
    }
    if (closing.load(std::memory_order_acquire)) {
        return;
    }

    send_ka.reset();
    send_ka.active = true;
    send_ka.delay = delay;
    send_ka.interval = interval;
    send_ka.max_miss = max_miss;

    error_code_t ignored;
    send_ka.timer.cancel(ignored);
    send_ka.timer.expires_after(delay + interval);
    const std::uint64_t gen = send_ka.generation;
    // ★ 强自持：同 recv 侧。
    std::shared_ptr<Conn::Impl> self = self_ref();
    if (!self) {
        return;
    }
    send_ka.timer.async_wait([impl = std::move(self), gen](const error_code_t& ec) mutable {
        impl->on_send_keepalive_tick(ec, gen);
    });
}

void Conn::Impl::do_disable_send_keepalive()
{
    if (!send_ka.active) {
        return;
    }
    send_ka.reset();
    error_code_t ignored;
    send_ka.timer.cancel(ignored);
}

void Conn::Impl::on_recv_keepalive_tick(const error_code_t& ec, std::uint64_t gen)
{
    if (ec) {
        // operation_aborted = 被 disable/close/重新 enable 取消：正常退出，不重排。
        return;
    }
    if (!recv_ka.active || recv_ka.generation != gen) {
        return;                              // 旧代次的等待，忽略（B-14）
    }
    if (closing.load(std::memory_order_acquire)) {
        return;
    }

    // 每拍**无条件消费**"本拍内是否收到数据"标志：判断与清零合成一步（B-13）。
    const bool progressed = recv_ka.progressed.exchange(false, std::memory_order_acq_rel);
    if (progressed) {
        recv_ka.miss = 0;
    } else {
        ++recv_ka.miss;

        // 分级预警：**每一拍未通过都通知**，最后一拍之后才断链。
        if (recv_ka.max_miss != 0) {
            IdleInfo info;
            info.kind   = IdleKind::recv;
            info.missed = recv_ka.miss;
            info.limit  = recv_ka.max_miss;
            notify_idle(info);

            if (recv_ka.miss >= recv_ka.max_miss) {
                if (!closing.load(std::memory_order_acquire)) {
                    fail_connection(make_boost_error_code(NetErrc::keepalive_timeout));
                }
                return;
            }
        }
    }

    // 重排下一拍：相对时间（expires_after），不补拍、不受系统时钟影响（B-14）。
    recv_ka.timer.expires_after(recv_ka.interval);
    // ★ 强自持：同 recv 侧（续拍）。
    std::shared_ptr<Conn::Impl> self = self_ref();
    if (!self) {
        return;
    }
    recv_ka.timer.async_wait([impl = std::move(self), gen](const error_code_t& ec2) mutable {
        impl->on_recv_keepalive_tick(ec2, gen);
    });
}

void Conn::Impl::on_send_keepalive_tick(const error_code_t& ec, std::uint64_t gen)
{
    if (ec) {
        return;
    }
    if (!send_ka.active || send_ka.generation != gen) {
        return;
    }
    if (closing.load(std::memory_order_acquire)) {
        return;
    }

    const bool progressed = send_ka.progressed.exchange(false, std::memory_order_acq_rel);
    if (progressed) {
        send_ka.miss = 0;
    } else {
        ++send_ka.miss;

        // 与接收侧对称：每一拍未通过都通知，最后一拍之后才断链。
        if (send_ka.max_miss != 0) {
            IdleInfo info;
            info.kind   = IdleKind::send;
            info.missed = send_ka.miss;
            info.limit  = send_ka.max_miss;
            notify_idle(info);

            if (send_ka.miss >= send_ka.max_miss) {
                if (!closing.load(std::memory_order_acquire)) {
                    fail_connection(make_boost_error_code(NetErrc::keepalive_timeout));
                }
                return;
            }
        }
    }

    send_ka.timer.expires_after(send_ka.interval);
    // ★ 强自持：同 recv 侧（续拍）。
    std::shared_ptr<Conn::Impl> self = self_ref();
    if (!self) {
        return;
    }
    send_ka.timer.async_wait([impl = std::move(self), gen](const error_code_t& ec2) mutable {
        impl->on_send_keepalive_tick(ec2, gen);
    });
}

// =============================================================================
// 关闭
// =============================================================================

void Conn::Impl::request_close(const error_code_t& reason) noexcept
{
    try {
        // ★ 强自持：关闭任务同时捕获 **Impl** 与**公共对象 Conn** 的强引用 ——
        //   这是"关闭流程必定执行、通知不会丢"的关键：
        //     * 抓住 Impl  → do_close 必定跑完（冲刷挂起回调 + on_fini）；
        //     * 抓住 Conn → `on_linkdown(ep)` 拿到的**一定是一个有效的
        //       Conn**。否则调用方先释放 Conn 时，公共对象会先析构，
        //       `owner.lock()` 失败 → on_linkdown 被**静默丢弃**。
        std::shared_ptr<Conn::Impl> state = self_ref();
        if (state) {
            std::shared_ptr<Conn> ep = public_self();
            asio::post(socket.get_executor(),
                       [impl = std::move(state), ep = std::move(ep), reason]() mutable {
                           (void)ep;   // 保持公共对象存活到本任务结束
                           impl->do_close(reason);
                       });
            return;
        }
        do_close(reason);   // 无公共对象（异常路径）：退化为直接执行
    } catch (...) {
        // io_context 已停止：退化为在本线程直接执行（仍幂等、仍串行语义可接受）。
        do_close(reason);
    }
}

void Conn::Impl::fail_connection(const error_code_t& ec) noexcept
{
    // 任何错误都收敛到同一条关闭路径，保证 on_fini / on_linkdown 只发生一次。
    do_close(normalize_close_error(ec));
}

void Conn::Impl::do_close(const error_code_t& reason) noexcept
{
    // 幂等判据用 `closed`（流程已完成标志），而不是 `closing`（意图标志）。
    // 因为 close() 会先把 closing 置位，如果这里用 closing 判定，就会在
    // close() 之后立刻 return，导致挂起回调/on_fini/on_linkdown 全部丢失。
    if (closed.exchange(true, std::memory_order_acq_rel)) {
        return;                              // 幂等：关闭流程已执行过
    }
    closing.store(true, std::memory_order_release);
    if (!close_reason) {
        close_reason = reason;               // 首个原因胜出
    }

    // 1) 先关 socket：此后不会再有新的 IO 完成，且 on_linkdown 里再 async_*
    //    必然返回 false（不会出现旧实现"linkdown 里 send 被静默丢弃"）。
    //    先 shutdown/close 再跑回调，也避免"数据与 FIN 乱序"（A-16）。
    close_socket_noexcept(socket);

    // 2) 取消并复位保活定时器（不依赖回调时序，显式 reset）。
    recv_ka.reset();
    send_ka.reset();
    error_code_t ignored;
    recv_ka.timer.cancel(ignored);
    send_ka.timer.cancel(ignored);

    // 3) 配额清零（后续 async_* 因 closed 直接返回 false，不会再加）。
    release_all_quota();

    // 4) 收集未完成的读/写请求：**必须各自被回调恰好一次**（非 0 错误码）。
    //    旧实现直接 fini() 掉队列，用户回调静默丢失。
    std::optional<ReadQueue::Request> read_req = reads.take_in_flight();
    std::deque<WriteQueue::Request>  write_reqs = writes.drain();
    has_pending_read.store(false, std::memory_order_release);

    const error_code_t user_ec = normalize_close_error(reason);

    // 5) 读通知（若有）：把在途缓冲连同错误码一起归还。
    if (read_req.has_value()) {
        notify_read(user_ec, read_req->buf, std::size_t{0});
    }

    // 6) 写通知：按受理顺序（FIFO）逐个归还，每个恰好一次。
    for (WriteQueue::Request& req : write_reqs) {
        // 已部分写出的那笔，把真实已发字节数带回去（不是恒为 0）
        notify_write(user_ec, req.buf, req.sent);
    }

    // 7) on_close：只有 on_open 成功过的 handler 才配平调用。
    finish_handler();

    // 8) on_linkdown：只有"连接曾成功建立并交给 Linkable"的才通知。
    notify_linkdown();

    // 9) 从 Mechanism 的连接表摘除（恰好一次）。
    //    必须放在最后：此时用户可见的所有通知都已投递完毕，摘除只影响
    //    `endpoint_count()` 的计数与表内强引用，不会提前让对象析构导致
    //    上面第 5~8 步访问悬垂。
    fire_unregister();
}

void Conn::Impl::release_all_quota() noexcept
{
    std::lock_guard<std::mutex> lock(quota_mutex);
    reserved_bytes = 0;
    reserved_count = 0;
}

void Conn::Impl::set_send_limit(std::size_t bytes, std::size_t count) noexcept
{
    std::lock_guard<std::mutex> lock(quota_mutex);
    limit_bytes = bytes;
    limit_count = count;
}

void Conn::Impl::finish_handler() noexcept
{
    if (!handler) {
        return;
    }
    std::shared_ptr<ConnHandler> h = std::move(handler);
    const bool inited = handler_inited;
    handler_inited = false;
    if (inited && h) {
        invoke_void([&h]() { h->on_close(); });
    }
    // h 在此释放（若外部无其它引用，handler 对象随之析构）。
}

void Conn::Impl::notify_linkdown() noexcept
{
    if (!link_established) {
        return;
    }
    link_established = false;

    std::shared_ptr<Linkable> lb;
    try {
        lb = linkable.lock();
    } catch (...) {
        lb = std::shared_ptr<Linkable>();
    }
    if (!lb) {
        return;                              // 订阅者已析构：不再投递（旧实现裸指针悬垂）
    }
    std::shared_ptr<Conn> self = public_self();
    if (!self) {
        return;
    }
    invoke_void([&lb, &self]() { lb->on_linkdown(*self); });
}

// =============================================================================
// 内部工厂
// =============================================================================
//  访问控制：`Conn::Impl` 是 public 头中的 **private 嵌套类型**，私有构造
//  只 friend `Mechanism`（私有构造）。
//  方案 A 落地后：`Conn::Impl` 的**前向声明已移到公共头 public 段**，
//  因此实现层可以合法命名该类型；`m_impl` 与私有构造函数仍然受保护。
//  唯一需要 friend 权限的是"调用私有构造函数"这一动作 —— 它只能发生在
//  `Mechanism` 的成员函数里（见 mechanism.cpp 的 Mechanism::construct_endpoint）。
//  本函数只负责：把 `shared_ptr<void>` 还原为 `shared_ptr<Conn::Impl>`，
//  再转交给那个受控入口。
//
//  ★ 重要：本函数绝不能静默失败。
//    若构造入口未就绪（例如某条创建路径绕过了 Mechanism），这里必须通过
//    `endpoint_factory_ready()` 让调用方感知，而不是返回空指针 ——
//    否则表现为"连接建立了但没有任何回调"（极难排查）。

namespace {

/// 受控构造入口（由 mechanism.cpp 定义）。
/// 返回空表示入口本身不可用（不应发生；调用方须视为失败并关闭 socket）。
using MakeFn = std::shared_ptr<Conn> (*)(std::shared_ptr<Conn::Impl>);

MakeFn g_construct_fn = nullptr;

} // namespace

void install_endpoint_factory(MakeFn fn) noexcept
{
    g_construct_fn = fn;
}

bool endpoint_factory_ready() noexcept
{
    return g_construct_fn != nullptr;
}

std::shared_ptr<Conn> MakeConnObject(std::shared_ptr<Conn::Impl> impl)
{
    const MakeFn fn = g_construct_fn;
    if (!impl || fn == nullptr) {
        return std::shared_ptr<Conn>();
    }
    return fn(std::move(impl));
}

// =============================================================================
// Conn（公共门面）
// =============================================================================

Conn::Conn(std::shared_ptr<Impl> impl) : m_impl(std::move(impl))
{
}

Conn::~Conn() = default;

std::uint64_t Conn::id() const noexcept
{
    return m_impl ? m_impl->id : detail::IdRegistry::kInvalidId;
}

Address Conn::local_address() const
{
    return m_impl ? detail::from_asio_endpoint(m_impl->local_addr) : Address{};
}

Address Conn::remote_address() const
{
    return m_impl ? detail::from_asio_endpoint(m_impl->remote_addr) : Address{};
}

std::shared_ptr<void> Conn::user_context() const noexcept
{
    return m_impl ? m_impl->user_context : std::shared_ptr<void>();
}

bool Conn::read(Buf buf, ReadMode mode)
{
    if (!m_impl) {
        return false;
    }
    return m_impl->accept_read(std::move(buf), mode);
}

bool Conn::write(ConstBuf buf)
{
    if (!m_impl) {
        return false;
    }
    return m_impl->accept_write(std::move(buf));
}

void Conn::set_send_queue_limit(std::size_t bytes, std::size_t count)
{
    if (m_impl) {
        m_impl->set_send_limit(bytes, count);
    }
}

void Conn::enable_recv_keepalive(std::chrono::milliseconds delay,
                                     std::chrono::milliseconds interval,
                                     std::uint32_t max_miss)
{
    if (m_impl) {
        m_impl->post_keepalive_recv(delay, interval, max_miss);
    }
}

void Conn::disable_recv_keepalive()
{
    if (m_impl) {
        m_impl->post_keepalive_recv_disable();
    }
}

void Conn::enable_send_keepalive(std::chrono::milliseconds delay,
                                     std::chrono::milliseconds interval,
                                     std::uint32_t max_miss)
{
    if (m_impl) {
        m_impl->post_keepalive_send(delay, interval, max_miss);
    }
}

void Conn::disable_send_keepalive()
{
    if (m_impl) {
        m_impl->post_keepalive_send_disable();
    }
}

void Conn::close() noexcept
{
    if (!m_impl) {
        return;
    }
    // 幂等：用 closing 做"意图"去重。首次调用置位并投递，后续调用直接返回。
    if (m_impl->closing.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // ★ 关键：这里只置 closing，**不置 closed**。
    //   `closed` 由该连接 io 线程上的 do_close 置位，代表"关闭流程已执行完
    //   （挂起回调已派发、on_fini/on_linkdown 已投递）"。
    //   若在此处就把 closed 置 true，do_close 会因幂等判定直接返回，
    //   结果是挂起回调全部丢失 —— 这正是 build-test-dev 复现的 P0。
    m_impl->request_close(asio::error::operation_aborted);
}

bool Conn::is_open() const noexcept
{
    return m_impl && !m_impl->closing.load(std::memory_order_acquire);
}

} // namespace netmech
