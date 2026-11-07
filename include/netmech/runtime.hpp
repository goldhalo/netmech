// =============================================================================
//  netmech - io 运行时（可注入的线程池）
// =============================================================================
//  文件    : netmech/runtime.hpp
//  用途    : 定义一个**可显式创建、可注入、可关闭**的 io 运行时。
//  依赖    : 标准库（实现里用 asio，但那在 src 里，公共头不引）。
//  编码    : UTF-8
//
//  ★ 为什么需要它（和 netmech::initialize() 有什么区别）
//  -------------------------------------------------------------------------
//  `initialize(n)` / `shutdown()` 管的是**进程级默认运行时**——单 exe、单
//  .so 的常见场景够用。但有两种情况它不够：
//
//    1. 一个进程里要**多套**互不干扰的 io 线程池（媒体面/信令面隔离，
//       或测试里要多个独立实例）；
//    2. 一个进程里有**多个 .so，各自静态链接了一份 netmech**（两个模块
//       各有一份 `IoPool::global()` 的静态数据），默认运行时会变成
//       "每个 .so 一套线程池"。这时需要宿主创建**一个** Runtime，
//       通过各库的 `create(..., runtime)` 入口传下去，才能共用一套线程。
//
//  ★ 生命周期语义（与默认运行时一致，刻意保持一致）
//  -------------------------------------------------------------------------
//  * `Runtime` 持有的 io_context **永不销毁**（进程结束时随 OS 回收）：
//    这样任何 socket / timer / acceptor 的析构**永远**能访问到有效的
//    context ——"IO 对象比 context 活得久"这一整类析构顺序崩溃在结构上
//    不可能发生；
//  * `shutdown()` 只**停线程 + join**，之后 context 依然有效（库内对象
//    照常可以安全析构，只是回调不会再被调用）；
//  * `shutdown()` 之后用这个 Runtime 发起的 `listen()` / `connect()` 会返回
//    `not_initialized`（与 `netmech::shutdown()` 之后的默认运行时一致）。
//
//  ★ 多 .so 场景（重要，实测结论）
//  -------------------------------------------------------------------------
//  如果每个 .so **各自静态链接**一份 netmech（默认构建），不要把一个 .so 里的
//  Runtime 注入到另一个 .so 使用。Runtime 对象本身能传过去（布局一 致），但
//  asio 的线程局部状态 / 注册表也是每个模块一份 —— 两个模块的代码混用同一个
//  io_context 会在 asio 内部崩溃（实测：SIGSEGV in
//  asio::detail::scheduler::compensating_work_started）。
//
//  正确做法是让 netmech 以**共享库**形式被所有模块共用一份代码：
//      cmake -DNETMECH_LIBRARY_TYPE=SHARED ...
//  这时 `default_runtime()` 天然全进程只有一份，多 .so 直接用它即可；
//  注入 Runtime 只是"多套线程池 / 测试隔离"时才需要。
//
//  （`test/multi_so/` 下就是这个场景的回归用例：只在 SHARED 构建下注册。）
//
//  ★ 线程数由**创建者**决定
//  -------------------------------------------------------------------------
//  `Options::io_threads` 在 `create()` 时一次性生效，之后不可更改（运行中
//  增删线程会破坏"1 个 io_context = 1 条线程、同一连接回调天然串行"的
//  不变量）。传 0 表示 `max(1, hardware_concurrency())`。
//
//  ★ 典型用法
//  -------------------------------------------------------------------------
//  @code
//      // 宿主（exe 或 hub .so）：只建一套线程池，线程数自己定
//      netmech::Runtime::Options opt;
//      opt.io_threads = 4;
//      auto rt = netmech::Runtime::create(opt);
//      if (!rt) { /* 线程创建失败 */ }
//
//      // 把同一个 rt 传给各个库（它们内部用 Mechanism::create(cfg, rt) 等）
//      auto mech = netmech::Mechanism::create(netmech::Mechanism::Config{}, rt);
//
//      // 退出时显式停（幂等）
//      rt->shutdown();
//  @endcode
// =============================================================================
#pragma once

#include <cstddef>
#include <memory>
#include <system_error>

namespace netmech {

class IoPool;   ///< 内部类型（src/io_pool.hpp），公共 API 不会用到

/// 独立的 io 运行时：N 个 io_context，每个绑定 1 条线程。
///
/// @par 线程安全
///  `create()` 可从任意线程调用；`io_threads()` / `running()` 可并发读；
///  `shutdown()` 幂等且可在任意线程调用（**不要在它的 io 回调里调用**：
///  会 join 自己，实现会跳过自身，但那属于坏味道）。
///
/// @par 与 netmech::initialize() 的关系
///  `initialize()` / `shutdown()` 管理的是进程级默认运行时（`default_runtime()`）；
///  本类用于"多个池"或"多 .so 共用一套池"的场景。两者可以并存：
///  默认运行时管理自己那套线程，每个 Runtime 管自己那套。
class Runtime {
public:
    /// 创建参数。
    struct Options {
        /// io 线程数。0 表示 `max(1, hardware_concurrency())`。
        /// 创建后不可更改（见文件头说明）。
        std::size_t io_threads = 0;
    };

    /// 析构：等价于调用一次 `shutdown()`（幂等）。
    ///
    /// @note 不销毁 io_context（文件头的生命周期语义），因此析构是**有界快速**的，
    ///       只做 stop + join。
    ~Runtime();

    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    /// 用默认参数（`io_threads = 0`，即 hardware_concurrency）创建运行时。
    [[nodiscard]] static std::shared_ptr<Runtime> create();

    /// 创建运行时，线程数在此时决定。
    ///
    /// @param[in] opts 参数（见 Options）
    /// @return 非空；线程创建失败时**抛出** `std::system_error`（与
    ///         `Mechanism::create()` 的失败语义一致：这类失败无法用
    ///         error_code 表达得更清楚，继续运行只会得到静默失效）。
    [[nodiscard]] static std::shared_ptr<Runtime> create(const Options& opts);

    /// 同 `create(opts)`，但失败时返回 nullptr 并填 @p ec（不抛异常）。
    [[nodiscard]] static std::shared_ptr<Runtime> create(const Options& opts,
                                                         std::error_code& ec) noexcept;

    /// 实际线程数（归一化之后，恒 >= 1）。
    [[nodiscard]] std::size_t io_threads() const noexcept;

    /// 线程池是否仍在运行（`shutdown()` 之后为 false）。
    [[nodiscard]] bool running() const noexcept;

    /// 停止投递并 join 全部 io 线程（幂等；重复调用无副作用）。
    ///
    /// @note 之后 io_context 依然有效：已存在对象照常析构，但它们的回调不会
    ///       再被执行；新的 `listen()` / `connect()` 返回 `not_initialized`。
    /// @warning 会阻塞直到线程退出；不要在 io 回调里调用。
    void shutdown() noexcept;

    /// 内部实现（IoPool 封装）。公共 API 不应直接使用。
    struct Impl;

    /// @warning **内部 API**（库内工厂用）：取出底层 IoPool 指针。
    ///          公共代码不应使用；不保证跨版本稳定。
    [[nodiscard]] IoPool* pool_ptr() const noexcept;

private:
    explicit Runtime(Impl* impl) noexcept;

    friend Runtime& default_runtime();   // 进程级默认运行时的唯一构造点

    /// 故意**永不释放**：io_context 要与进程同寿（见文件头）。
    Impl* m_impl = nullptr;
};

/// 进程级默认运行时（`netmech::initialize()` / `shutdown()` 管理的那一个）。
///
/// 单 exe / 单 .so 的常见场景直接用它就行；需要多套线程池或跨 .so 共用时，
/// 自己 `Runtime::create()` 并注入。
///
/// @note 线程数由 `netmech::initialize(n)` 决定；没调用过 initialize 时为
///       `max(1, hardware_concurrency())`。首次调用本函数时才真正建线程池。
/// @note 返回的引用在进程生命周期内一直有效。
[[nodiscard]] Runtime& default_runtime();

} // namespace netmech
