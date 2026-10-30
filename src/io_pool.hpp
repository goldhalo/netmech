// =============================================================================
//  NetMechanism - 执行上下文（io 线程池）
// =============================================================================
//  文件    : netmech/executor.hpp
//  用途    : 定义 IoPool —— N 个 io_context + work_guard + 1 线程/个 的运行时
//            执行底座；以及 timer / strand 工厂约定。
//  依赖    : Boost 1.74 + 标准库。不含 <iostream>。
//  编码    : UTF-8
//
//  ★ 与旧 CIoServicePool 的关键差异（缺陷 A-09 / A-12 / B-02）
//  ---------------------------------------------------------
//  1. **线程数 0 归一化**：旧 `setParallelNum(0)` 让 `m_currIdx % m_poolSize`
//     整数除零（MSVC x64 下 = 0xC0000094 崩溃）。新实现把 0 归一化为
//     `max(1, hardware_concurrency())`，并在归一化**之后**才允许任何取模。
//     另外统一用 `fetch_add` + 位运算取模，不对 0 做除法。
//     ★ 线程数由 `initialize(threads)` **一次性**决定，之后不可更改
//       （契约见类注释）。
//  2. **线程数不乘 4**：旧默认 `hardware_concurrency()*4`，嵌入式不友好。新默认
//     就是 `hardware_concurrency()`（最少 1）。
//  3. **work_guard**：每个 io_context 都持有 executor_work_guard，保证 run() 不会
//     因为"暂时没有待处理任务"而返回（旧实现 accept 侧无 work guard → 一次
//     accept 失败后 4 个线程全部退出 = 服务器静默熄火）。
//  4. **关停顺序：先 stop 再 join，且绝不持锁 join**：旧 `CIoServicePool::fini()`
//     在持有 `m_mutex` 的情况下 `join()`，而 `getIoService()` 也要这把锁 →
//     io 线程里任何回调若申请该锁即永久死锁（B-02）。新实现：
//        reset work_guard → 逐个 io_context::stop() → 收集 thread 句柄到局部
//        → 释放锁 → join → 再清理容器。
//     `fini()` 由析构调用，且可重复调用（幂等）。
//  5. **提供 `next_io_context()`**：调用方据此把自己的 socket / timer 绑定到
//     某个 io_context 上。由于每个 io_context 只由 1 条线程 `run()`，
//     绑定其上的连接回调天然串行 —— 这是本库**不使用 strand** 的根据。
// =============================================================================
#pragma once

// -----------------------------------------------------------------------------
// 依赖：本头是**公共头**，因此不得包含 src/ 下的内部前置头
// （netmech_srcpch.hpp 只在库内部可见，公共头引用它会导致下游
//  "C1083: 无法打开包括文件 netmech_srcpch.hpp"）。
// 这里显式引入本头真正需要的 Boost.Asio 组件与标准库。
// -----------------------------------------------------------------------------
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/strand.hpp>

namespace netmech {

// 公共头所需的类型别名（与库内部同名，保持 ABI/语义一致）。
using io_context_t  = asio::io_context;
using io_executor_t = io_context_t::executor_type;
using strand_t      = asio::strand<io_executor_t>;
using work_guard_t  = asio::executor_work_guard<io_executor_t>;

/// 进程级 io 运行时：`N` 个 `io_context`，每个绑定 1 条线程，并各持一枚 work_guard。
///
/// @par ★ 进程级不变量（整套生命周期模型的地基）★
///
///  本类的**唯一实例**由 `global()` 创建，生命周期 = **进程**：
///  - 它持有的 `io_context` **永不销毁**（线程可以停，context 不会死）；
///  - 因此任何绑定在某个 context 上的 socket / timer / acceptor，
///    其析构函数**永远**能访问到一个有效的 context ——
///    这条"io 对象活得比 context 久"的经典崩溃（0xC0000409 / 0xC0000005）
///    在结构上就不可能发生；
///  - 想停止运行时用 `stop_all()`（进程退出前调用即可）。
///    **`stop_all()` 之后 context 依然有效**，库内对象照常可以安全析构。
///
/// @par ★★ 契约：线程数是**进程级、一次性**的，此后不可更改 ★★
///
///  - 线程数**只由 `initialize(threads)` 决定一次**，且必须在第一次使用
///    运行时（`global()`）**之前**调用；
///  - 一旦运行时建立，线程数**永久固定**。`initialize()` 再次调用会
///    **返回 false 且不改变任何东西**（同值时视为幂等成功）；
///  - `global()` **不接受**线程数参数 —— 刻意不提供"事后设置"的入口，
///    以免出现"改了却没生效"的静默失效；
///  - **为什么不可更改**：io_context 与线程是 1:1 绑定的；运行中增删线程会
///    破坏「1 context = 1 线程」的不变量，进而破坏「同一连接回调串行」
///    的公开契约。
///  - 想要自定义线程数，就在**进程启动时**先调 `initialize(n)`；
///    忘了调就用默认值 `max(1, hardware_concurrency())`。
///
/// @par 线程模型
///  - 每个 io_context 恰好被 1 条线程 `run()`；因此**同一 io_context 内不会出现
///    因多线程并发导致的完成处理重入**。
///  - IO 路径**不使用 strand**（v1.3.0 起）：在"1 context = 1 线程"的不变量下
///    strand 的串行保证是冗余的，而它本身要加锁（每次投递 +~140ns）。
///    ⚠ 若将来改成 `N` 线程 run 同一个 io_context，**必须**同时恢复 strand，
///    否则"同一连接回调串行"的契约会被破坏。
///  - 每个连接/会话在创建时从池里取**一个** io_context 并绑定自己的
///    socket / timer，因此该连接的所有回调都发生在**同一条 io 线程**上。
///  - 不同的连接可能落在**不同**的 io_context 上（round-robin）→ 跨连接可并发；
///    但落在**同一个** io_context 上的多个连接会共享那条线程，彼此也是串行的。
///
/// @par 生命周期模型：库内对象**强自持**
///
///  本库把对象分成两类，两类的规则不同：
///
///  - **库内对象**（`Conn` / `Acceptor` / `UdpChannel` /
///    `DatagramReceiver` / `DatagramSession`）：由库创建、由库掌控。
///    它们投放到 io_context 的**在途完成处理一律捕获自己的 `shared_ptr`**
///    （强自持），因此：
///      * 库内对象**一定活到最后一个 IO 完成** —— 关闭流程（冲刷挂起回调、
///        `on_fini` / `on_linkdown` / `on_session_close`）**必定执行**，
///        通知不会丢；
///      * 库内对象之间的**父子引用**（Conn → Mechanism）用**弱引用**
///        （否则成环）；
///      * 强自持形成的环（context → 队列 → handler → 对象 → 池 → context）
///        **无害** —— 因为 context 永不销毁，没有谁需要它死。
///
///  - **外部对象**（用户实现的 `Linkable` / `IoHandler` / `SessionHandler`、
///    用户给的 buffer 与回调 lambda）：库控制不了，只能二选一：
///      * **定契约**：库**强引用**持有（`IoHandler` / `SessionHandler`）→
///        库保证它活着到终结，但你必须遵守约束（见各接口注释）；
///      * **弱回调**：库**弱引用**持有（`Linkable`）→ 它没了就不再投递，
///        这是**正确行为**而不是缺陷。
///
/// @note Debug 构建（`_DEBUG`）下本类会在析构时检测"是否仍有 handler 持有
///       对象"，并在发生误用时通过 `assert` 尽早暴露，而不是留到运行期崩溃。
class IoPool {
public:
    // -------------------------------------------------------------------------
    // 进程级运行时
    // -------------------------------------------------------------------------

    /// ★ 一次性设定进程级 io 线程数。**必须在第一次使用运行时之前调用。**
    ///
    /// @param[in] threads 期望线程数。`0` = 自动：`max(1, hardware_concurrency())`。
    ///                    任何值都会被归一化到 >= 1。
    /// @return true  = 本次调用**决定**了线程数（首次调用，或与已生效值一致）；
    ///         false = 运行时**已经建立**，线程数早已固定 —— 本次调用**无效**
    ///                 （不改变任何东西；同值时返回 true 以保持幂等）。
    ///
    /// @note 线程数**定了就不改**：io_context 与线程 1:1 绑定，运行中增删线程会
    ///       破坏「1 context = 1 线程」的不变量，进而破坏「同一连接回调串行」。
    /// @note 只想用默认值就不必调用本函数。
    /// @note 线程安全；可重复调用。
    [[nodiscard]] static bool initialize(std::size_t threads) noexcept;

    // -------------------------------------------------------------------------
    // 进程级运行时（取用入口）
    // -------------------------------------------------------------------------
    //  ── 为什么只有进程级这一种 ──────────────────────────────────────────────
    //  网络栈应当**活到进程结束**。把运行时的生命周期拉长到最长，可以从
    //  根本上消除一整类"IO 对象比 io_context 活得久"的析构顺序问题：
    //     * 任何 socket / timer / acceptor 都绑定在某个 io_context 上；
    //     * 它们的析构函数**必须访问**该 io_context；
    //     * 若 io_context 先被销毁，这些对象的析构就是未定义行为
    //       （典型症状：0xC0000409，且只在多轮创建-销毁后才复现）。
    //  让 context 永不销毁，这个前提天然成立，调用方无需关心任何销毁顺序。
    //
    //  同时它也是"库内对象强自持"能够成立的前提：强自持会形成
    //      io_context -> 队列 -> handler -> 对象 -> 池 -> io_context
    //  这个环。若池可以随时销毁，环会让池永不析构（线程永不 join）；
    //  池是进程级之后，环**无害** —— 没有谁需要它死。
    //
    //  ── 用法 ────────────────────────────────────────────────────────────────
    //      // 直接用全局池（Mechanism 内部就是这么用的）
    //      auto mech = Mechanism::create();
    //
    //      // 想自定义线程数：进程启动时先定一次
    //      IoPool::initialize(4);
    //      auto mech = Mechanism::create();
    //
    //  @return 单例指针，永不为空。
    //  @note 线程安全；可重复调用，始终返回同一实例。
    //  @note 该池的 `io_context` **永不销毁**；要停止运行时用 `stop_all()`。
    //  @note 线程数**不在这里设定** —— 见 `initialize()` 与类注释的契约。
    [[nodiscard]] static IoPool& global();

    /// 当前线程是否属于本池的 io 线程。
    ///
    /// 用途：在回调里判断"我是不是在 io 线程上"，从而避免在那里做
    /// 阻塞操作（如等待本池关停 —— 那会自我死锁）。
    ///
    /// @note 线程安全；池已关停时返回 false。
    [[nodiscard]] bool is_io_thread() const noexcept;

    /// 析构（**正常路径不会被调用** —— 本类唯一实例由 `global()` 创建且进程级）。
    ///
    /// 保留它只是为了在内部误用时有一个安全的收尾路径。
    /// @note 真正对外的关停入口是 `stop_all()`。
    ~IoPool();

    IoPool(const IoPool&) = delete;
    IoPool& operator=(const IoPool&) = delete;

    /// 实际线程数（归一化之后的值，恒 >= 1）。
    /// @note 线程安全。
    std::size_t size() const noexcept { return size_; }

    /// 线程池是否仍在运行（未被 stop）。
    /// @note 线程安全。
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    /// 轮询取下一个 io_context（round-robin）。
    ///
    /// @return 非空 io_context 引用；即使“已 stop”也仍然有效（context 永不销毁），
    ///         但此时投递的任务不会再被执行。
    /// @warning 仅限建立连接/接收器时使用；线程安全（原子自增 + 取模）。
    io_context_t& next_io_context() noexcept;

    /// 为指定 io_context 建立一条新的 strand。
    ///
    /// @param[in] ioc 目标 io_context。
    /// @return strand 对象（可能为空 strand；空 strand 投递的任务会以异常结束，
    ///         因此本库保证传入的 ioc 一定来自 next_io_context()）。
    /// @note **本库的 IO 路径不使用 strand**（见类注释的线程模型）；此接口保留给
    ///       需要自行串行化的调用方。在"1 context = 1 线程"的模型下它通常是
    ///       冗余的 —— 只有当多个线程会 run 同一个 io_context 时才需要它。
    static strand_t make_strand_of(io_context_t& ioc) noexcept;

    /// 停止全部 io 线程；幂等。通常在**进程退出前**调用一次。
    ///
    /// 顺序严格为：释放 work_guard → `stop()` 所有 io_context → 释放内部锁 →
    /// `join()` 所有线程。**join 时不持有任何内部互斥量**，因此 io 线程里申请
    /// 本类锁不会与关停互锁。
    ///
    /// @note **`stop_all()` 之后 `io_context` 依然有效**（只是不再执行新任务）：
    ///       库内对象照常可以安全析构，它们的 socket / timer 析构不会出问题。
    /// @note 停止后**不要**再建立新连接（投递的任务不会被执行）。
    void stop_all() noexcept;

protected:
    /// 由 create() 调用。
    explicit IoPool(std::size_t threads);

private:
    /// 启动线程；失败返回 false（已启动的线程会被回收）。
    bool start(std::size_t request_threads);

    std::vector<std::shared_ptr<io_context_t>>         contexts_;   ///< 各 io_context
    std::vector<std::shared_ptr<work_guard_t>>         guards_;     ///< 各 work_guard（保活）
    std::vector<std::thread>                           threads_;    ///< 各工作线程
    std::size_t                                        size_ = 0;   ///< 归一化后的线程数
    std::atomic<bool>                                  running_{false};
    std::atomic<std::size_t>                           rr_{0};      ///< round-robin 游标
    /// 保护 contexts_ / guards_ / threads_ / io_thread_ids_。
    /// 声明为 mutable：is_io_thread() 是 const 查询但需要加锁读取 id 列表。
    mutable std::mutex                                 mutex_;
    /// 是否已请求停止（stop_all() 或析构的第一阶段置位）。
    /// 与 `joined_` 分开：曾因合并成一个标志导致"停过就不再 join"的崩溃。
    bool                                               stop_requested_ = false;

    /// 是否已完成 join（线程已回收）。保证 join 只做一次。
    bool                                               joined_ = false;

    /// 本池 io 线程的 id 集合。
    ///
    /// 用途：检测"在 io 线程上析构本池"这一未定义用法 —— 那会导致
    /// `std::thread::join()` 等待自己（未定义行为：死锁或抛异常）。
    /// 只在 start() 期间写入、其余时间只读（读时持 mutex_）。
    std::vector<std::thread::id>                       io_thread_ids_;
};

} // namespace netmech
