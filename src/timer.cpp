// =============================================================================
//  NetMechanism - 定时器实现
// =============================================================================
//  文件    : timer.cpp
//  编码    : UTF-8
//
//  实现要点（都是保活定时器那套踩出来的）
//  --------------------------------------
//  * **代次计数**：每次 schedule/stop 递增 generation，async_wait 捕获它。
//    旧的一拍回来时代次不符就丢弃——避免"旧定时器把新的顶掉"。
//  * **强自持**：handler 捕获 shared_ptr<Impl>，保证回调执行时对象还活着。
//  * **漂移不累积**：下一拍按 `计划时间 + interval` 排；落后太多（回调阻塞等）
//    则从当前时刻重排，不追帧（否则会形成补偿风暴）。
//  * **owner 弱检查**：owner 失效即停表；有效则在回调期间临时强持有。
// =============================================================================
#include "netmech_srcpch.hpp"

#include "timer.hpp"

#include "netmech/common.hpp"

#include "io_pool.hpp"

namespace netmech {

namespace {

using steady_clock_t = std::chrono::steady_clock;

/// weak_ptr 是否"从未指向任何对象"（空构造），区别于"曾经指向但已失效"。
///
/// 二者的 use_count() 都是 0、expired() 都返回 true，只能靠比较控制块。
bool is_null_weak(const std::weak_ptr<void>& w) noexcept
{
    const std::weak_ptr<void> empty;
    return !w.owner_before(empty) && !empty.owner_before(w);
}

} // namespace

// =============================================================================
//  Impl
// =============================================================================

class Timer::Impl : public std::enable_shared_from_this<Timer::Impl> {
public:
    Impl(asio::io_context&       io,
         std::chrono::milliseconds interval_in,
         std::weak_ptr<void>     owner_in,
         std::function<void()>   cb_in,
         bool                    periodic_in)
        : timer(io)
        , interval(interval_in)
        , owner(std::move(owner_in))
        , cb(std::move(cb_in))
        , periodic(periodic_in)
        , has_owner(!is_null_weak(owner))
    {
    }

    ~Impl()
    {
        // 析构即取消：让挂起的 async_wait 以 operation_aborted 出队。
        error_code_t ignored;
        timer.cancel(ignored);
    }

    void start()
    {
        plan = steady_clock_t::now() + interval;
        running.store(true, std::memory_order_release);
        schedule();
    }

    void restart(std::chrono::milliseconds new_interval) noexcept
    {
        if (new_interval.count() <= 0) {
            return;
        }
        interval = new_interval;
        if (!running.load(std::memory_order_acquire)) {
            start();
            return;
        }
        // 重排：递增代次让旧的一拍失效，再排新的
        error_code_t ignored;
        timer.cancel(ignored);
        plan = steady_clock_t::now() + interval;
        schedule();
    }

    void cancel() noexcept
    {
        running.store(false, std::memory_order_release);
        ++generation;                       // 让在途的那一拍失效
        error_code_t ignored;
        timer.cancel(ignored);
    }

    [[nodiscard]] bool is_running() const noexcept
    {
        return running.load(std::memory_order_acquire);
    }

private:
    void schedule()
    {
        if (!running.load(std::memory_order_acquire)) {
            return;
        }
        const std::uint64_t gen = ++generation;

        timer.expires_at(plan);

        std::shared_ptr<Impl> self;
        try {
            self = shared_from_this();
        } catch (...) {
            running.store(false, std::memory_order_release);
            return;
        }

        timer.async_wait([self = std::move(self), gen](const error_code_t& ec) {
            self->on_tick(ec, gen);
        });
    }

    void on_tick(const error_code_t& ec, std::uint64_t gen)
    {
        // 被 cancel / restart 顶掉的旧拍：直接退出
        if (ec || !running.load(std::memory_order_acquire) || gen != generation) {
            return;
        }

        // ---- owner 检查 ----
        // has_owner == false 表示调用方明确表示"无依赖"，库不检查。
        std::shared_ptr<void> pinned;
        if (has_owner) {
            pinned = owner.lock();
            if (!pinned) {
                // 依赖对象已销毁：自动停表，不再回调。不报错、不通知。
                cancel();
                return;
            }
        }

        // ---- 执行回调 ----
        // pinned 在本次作用域内有效，保证 owner 指向的对象活到回调结束。
        try {
            if (cb) {
                cb();
            }
        } catch (...) {
            // 契约要求回调不抛；这里兜底，避免异常穿出 io 线程。
        }
        (void)pinned;

        if (!periodic) {
            cancel();
            return;
        }

        // ---- 排下一拍（漂移不累积）----
        const auto now = steady_clock_t::now();
        plan += interval;
        if (plan < now) {
            // 落后太多（回调阻塞、系统休眠等）：从当前时刻重排，不追帧
            plan = now + interval;
        }
        schedule();
    }

    timer_t                      timer;
    std::chrono::milliseconds    interval;
    std::weak_ptr<void>          owner;
    std::function<void()>        cb;
    bool                         periodic;
    bool                         has_owner;

    steady_clock_t::time_point   plan{};
    std::uint64_t                generation = 0;
    std::atomic<bool>            running{false};
};

// =============================================================================
//  Timer 门面
// =============================================================================

Timer::Timer() = default;
Timer::~Timer() = default;

std::shared_ptr<Timer> Timer::every(asio::io_context&       io,
                                    std::chrono::milliseconds interval,
                                    std::weak_ptr<void>     owner,
                                    std::function<void()>   cb,
                                    std::error_code&        ec)
{
    ec.clear();
    if (interval.count() <= 0) {
        ec = make_boost_error_code(NetErrc::invalid_argument);
        return nullptr;
    }
    if (!cb) {
        ec = make_boost_error_code(NetErrc::invalid_argument);
        return nullptr;
    }

    auto timer = std::shared_ptr<Timer>(new Timer());
    timer->impl_ = std::make_shared<Impl>(io, interval, std::move(owner), std::move(cb), true);
    timer->impl_->start();
    return timer;
}

std::shared_ptr<Timer> Timer::after(asio::io_context&       io,
                                    std::chrono::milliseconds delay,
                                    std::weak_ptr<void>     owner,
                                    std::function<void()>   cb,
                                    std::error_code&        ec)
{
    ec.clear();
    if (delay.count() <= 0) {
        ec = make_boost_error_code(NetErrc::invalid_argument);
        return nullptr;
    }
    if (!cb) {
        ec = make_boost_error_code(NetErrc::invalid_argument);
        return nullptr;
    }

    auto timer = std::shared_ptr<Timer>(new Timer());
    timer->impl_ = std::make_shared<Impl>(io, delay, std::move(owner), std::move(cb), false);
    timer->impl_->start();
    return timer;
}

void Timer::restart(std::chrono::milliseconds interval) noexcept
{
    if (impl_) {
        impl_->restart(interval);
    }
}

void Timer::cancel() noexcept
{
    if (impl_) {
        impl_->cancel();
    }
}

bool Timer::running() const noexcept
{
    return impl_ && impl_->is_running();
}

} // namespace netmech
