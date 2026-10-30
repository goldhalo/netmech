// =============================================================================
//  NetMechanism - 定时器
// =============================================================================
//  文件    : src/timer.hpp（**内部头**，不对外）
//  用途    : 在 io 线程上执行定时回调。**仅供库内部使用**：
//            · UdpEndpoint::set_timer() 的实现
//            · 未来也可承载 Conn 的保活
//            应用层通过 `UdpEndpoint::set_timer()` 使用定时能力，不直接接触本类。
//  依赖    : 标准库。实现内部用 asio 的 steady_timer，但公共头不暴露。
//  编码    : UTF-8
//
//  ★ 两层持有关系（重要）
//  ---------------------------------------------------------------------------
//    · **Timer 自己**由库强持有：async_wait 的 handler 捕获它的 shared_ptr，
//      保证回调执行时 Timer 还活着。
//    · **你的对象（owner）**库只用**弱引用**：库无权延长业务对象的生命周期。
//      回调派发时先 lock()——拿不到就自动停表；拿到了就在本次回调期间临时
//      强持有，保证回调执行过程中对象不会被别处释放。
//
//    这正是 "库能保证 Timer 活着，但保证不了你注册的 cb 里捕获的东西还活着"
//    的解法：把依赖关系显式写出来，让库替你在依赖消失时收尾。
//
//  ★ 线程模型
//  ---------------------------------------------------------------------------
//    · 回调在 io 线程上执行，与网络回调**同一线程**——共享状态不需要加锁。
//    · 可以在回调内部调用 restart() / cancel()。
//    · 回调里不得阻塞、不得抛异常（库会吞掉，但那是兜底不是许可）。
// =============================================================================
#pragma once

#include <asio/io_context.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <system_error>

namespace netmech {

/// io 线程上的定时器。
///
/// @par 典型用法
/// @code
///     // 业务对象自己持有定时器
///     struct Scheduler {
///         std::shared_ptr<netmech::Timer> tick;
///         int count = 0;
///
///         void start() {
///             std::error_code ec;
///             tick = netmech::Timer::every(100ms, weak_from_this(),
///                                          [this] { ++count; }, ec);
///             // weak_from_this() 传给库：本对象销毁后定时器自动停
///         }
///     };
/// @endcode
class Timer {
public:
    /// 每 @p interval 触发一次（首次在 interval 之后）。
    ///
    /// @param[in] interval 间隔，必须 > 0。
    /// @param[in] owner    回调的**依赖对象**（弱引用）。见下方说明。
    /// @param[in] cb       回调。在 io 线程执行。
    /// @param[out] ec      失败原因。
    /// @return 失败返回 nullptr。
    ///
    /// @par owner 的语义
    ///  - `owner` 有效：正常回调；**回调执行期间 owner 被临时强持有**，
    ///    所以 `cb` 里可以安全使用它所指向的对象。
    ///  - `owner` 已失效：定时器**自动停止**，不再回调（不报错、不通知）。
    ///  - 传**空的** `weak_ptr`：库不做检查，此时你必须自己保证 `cb` 内
    ///    不触碰已销毁的对象。**不推荐**，只在生命周期确信长于 Timer 时使用。
    ///
    /// @par 漂移
    ///  下一拍按"上次计划时间 + interval"排，**不累积漂移**。
    ///  回调执行耗时不会把周期越拖越长。
    /// @param[in] io 定时器归属的 io_context。**必须与使用者（如 UdpEndpoint）
    ///            的 io 相同**，否则回调会在另一条线程上执行，共享状态又要加锁。
    [[nodiscard]] static std::shared_ptr<Timer>
    every(asio::io_context&       io,
          std::chrono::milliseconds interval,
          std::weak_ptr<void>     owner,
          std::function<void()>   cb,
          std::error_code&        ec);

    /// @p delay 之后触发一次，然后自动停止。
    ///
    /// 参数与 `owner` 语义同 `every()`：`owner` 失效时本次也不会执行。
    [[nodiscard]] static std::shared_ptr<Timer>
    after(asio::io_context&       io,
          std::chrono::milliseconds delay,
          std::weak_ptr<void>     owner,
          std::function<void()>   cb,
          std::error_code&        ec);

    /// 用新间隔重启（周期性定时器）。在回调内部调用是安全的。
    ///
    /// @note 对 `after()` 创建的定时器，语义是"从此刻起再等 interval"。
    /// @note `interval <= 0` 时静默忽略。
    void restart(std::chrono::milliseconds interval) noexcept;

    /// 取消（幂等）。取消后不再回调，可以重新 restart 复用。
    void cancel() noexcept;

    /// 是否处于活动状态（未取消、未因 owner 失效而停止）。
    [[nodiscard]] bool running() const noexcept;

    /// 析构即取消。**不要**在回调内部释放最后一份 shared_ptr（会自我销毁）。
    ~Timer();

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    Timer();
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace netmech
