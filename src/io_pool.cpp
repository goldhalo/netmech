// =============================================================================
//  NetMechanism - io 线程池实现
// =============================================================================
//  文件    : io_pool.cpp
//  用途    : IoPool 的实现。设计说明见本文件头。
//  编码    : UTF-8
// =============================================================================
#include "netmech_srcpch.hpp"

#include "io_pool.hpp"

namespace netmech {

namespace {

/// 把请求线程数归一化为合法值：0 → max(1, hardware_concurrency())，其余保底为 1。
///
/// 旧实现 `setParallelNum(0)` 会让 `m_currIdx % m_poolSize` 整数除零（A-12）；
/// 这里把归一化收敛到唯一入口，任何取模都在归一化之后进行。
std::size_t normalize_threads(std::size_t requested) noexcept
{
    if (requested != 0) {
        return requested;
    }
    const unsigned hw = std::thread::hardware_concurrency();
    return (hw == 0) ? 1U : static_cast<std::size_t>(hw);
}

// ── 进程级「一次性」状态 ─────────────────────────────────────────────────────
//
// 线程数由**首次**确定，之后**不可更改**：
//   * io_context 与线程是 1:1 绑定的；
//   * 运行中增删线程会破坏「1 context = 1 线程」的不变量，
//     进而破坏「同一连接回调串行」的契约。
// 因此这里用 g_created 把「已经定过了」这件事显式记下来，
// 让误改**响起来**（initialize 返回 false），而不是静默失效。
std::mutex    g_init_mutex;
std::size_t   g_requested = 0;   ///< initialize() 请求的线程数（0 = 自动）
std::size_t   g_effective = 0;   ///< 建池时实际生效的线程数
bool          g_created   = false;///< 池是否已建立

} // namespace

bool IoPool::initialize(std::size_t threads) noexcept
{
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_created) {
        // 池已建立 → 线程数**已定**，本次调用不改变任何东西。
        // 只有请求值与实际一致时才算「成功」（幂等），否则失败。
        return g_effective == normalize_threads(threads);
    }
    g_requested = threads;
    return true;
}

IoPool& IoPool::global()
{
    // 进程级单例，**故意不析构**。
    //
    // 理由（关键，两条）：
    //
    //  1) 消除"IO 对象比 io_context 活得久"这一整类崩溃：
    //     若让这个实例在静态析构期释放，则 ~IoPool 会销毁 io_context；
    //     而此时仍可能有其他静态/全局对象持有 socket/timer —— 它们会在
    //     ~IoPool 之后析构并访问已销毁的 context，产生与本次要消除的崩溃
    //     **完全相同**的问题，只是从运行期转移到了退出期。
    //     让池随进程自然结束（由 OS 回收线程与内存）是最安全的：
    //       * 不存在 ~IoPool，也就不存在"context 先死"的窗口；
    //       * 线程在 main 返回后被 OS 结束，不阻塞退出。
    //
    //  2) 让「库内对象强自持」成立：
    //     库内对象投放到 io_context 的在途完成处理会捕获自己的 shared_ptr，
    //     于是形成
    //         io_context -> 队列 -> handler -> 对象 -> 池 -> io_context
    //     这个环。若池可以随时销毁，环会让池永不析构（线程永不 join）；
    //     池是进程级之后，环**无害** —— 没有谁需要它死。
    //     代价是库内对象一定活到最后一个 IO 完成 —— 这正是我们要的：
    //     关闭流程（冲刷挂起回调、on_fini/on_linkdown）必定执行，通知不会丢。
    //
    // 代价：泄漏检测工具会报告少量 "still reachable" 内存。
    //       这是刻意选择，不是泄漏 bug。
    static IoPool* const instance = []() -> IoPool* {
        // 取走一次性决定的线程数（未 initialize 过则用自动值）。
        std::size_t threads = 0;
        {
            std::lock_guard<std::mutex> lock(g_init_mutex);
            threads     = normalize_threads(g_requested);
            g_effective = threads;
            g_created   = true;
        }
        // 直接 new，不经过 shared_ptr —— 对象的生命周期完全由此静态指针
        // 控制，永不释放（见上方理由）。
        IoPool* pool = new IoPool(threads);
        if (!pool->start(threads)) {
            delete pool;
            return nullptr;
        }
        return pool;
    }();

    // 线程创建失败属于启动期致命错误：一个 io 线程都起不来，任何网络对象
    // 都无法工作，继续运行只会得到难以定位的静默失效。直接终止并说明原因。
    if (!instance) {
        std::fputs("netmech: 无法启动 io 线程池（线程创建失败）\n", stderr);
        std::terminate();
    }
    return *instance;
}

IoPool::IoPool(std::size_t threads)
{
    // 归一化在 global() 里做一次；这里再收敛一次，保证 size_ 恒 >= 1。
    size_ = normalize_threads(threads);
}

bool IoPool::start(std::size_t request_threads)
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);

        // 预分配：避免在持锁期间 push_back 触发重分配导致其它线程持有的引用失效。
        contexts_.reserve(request_threads);
        guards_.reserve(request_threads);
        threads_.reserve(request_threads);

        for (std::size_t i = 0; i < request_threads; ++i) {
            contexts_.emplace_back(std::make_shared<io_context_t>());
        }
        // work_guard 必须在启动线程**之前**创建：否则 run() 可能因队列为空立即
        // 返回（旧实现 accept 侧正是缺了 work guard → 线程僵死，A-09）。
        for (std::size_t i = 0; i < request_threads; ++i) {
            guards_.emplace_back(std::make_shared<work_guard_t>(
                asio::make_work_guard(*contexts_[i])));
        }

        // 先置位 running_ 再起线程：线程入口第一行就会读它。
        running_.store(true, std::memory_order_release);

        for (std::size_t i = 0; i < request_threads; ++i) {
            io_context_t* const ctx = contexts_[i].get();
            // 线程函数只捕获裸指针：io_context 的存活由本对象（shared_ptr 链）保证。
            // 本类析构保证 join 之后才销毁 contexts_，故无悬垂。
            threads_.emplace_back([ctx]() {
                try {
                    ctx->run();
                } catch (...) {
                    // 完成处理内部已全部为 noexcept（用户回调被 try/catch 包裹），
                    // 这里只是最后一道防线：绝不让异常终止线程/进程。
                }
            });
            // 记录 id，供 is_io_thread() 与析构期的误用检测使用。
            io_thread_ids_.push_back(threads_.back().get_id());
        }
        return true;
    } catch (...) {
        // 线程创建失败：回收已创建的线程，避免泄漏。
        running_.store(false, std::memory_order_release);
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            threads.swap(threads_);
        }
        for (auto& th : threads) {
            if (th.joinable()) {
                th.join();
            }
        }
        return false;
    }
}

IoPool::~IoPool()
{
    // 关停分两段，中间**不持有** mutex_：
    //   段1（持锁）：置 stopped_、取走 guard 与 contexts。
    //   段2（无锁）：stop() 各 io_context。
    //   段3（无锁）：join 全部线程。
    // 旧 CIoServicePool::fini() 在持 m_mutex 时 join，而 getIoService() 也要这把锁
    // → io 线程内任何需要取池锁的路径都会永久死锁（B-02）。此处顺序彻底规避。
    std::vector<std::shared_ptr<work_guard_t>> guards;
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (joined_) {
            return;   // 已经 join 过（例如显式调用了 join_threads()）
        }
        joined_         = true;
        stop_requested_ = true;
        guards.swap(guards_);
        threads.swap(threads_);
        // contexts_ 有意**不**取出：stop() 需要它们，且析构尾部仍需保证对象存活
        // 到 join 之后；contexts_ 的析构发生在本函数返回之后（成员析构顺序），
        // 而那时线程已经 join 完毕。
    }

    running_.store(false, std::memory_order_release);

    // 先 reset work_guard：让 run() 在队列排空后可以自然返回。
    guards.clear();

    // 再 stop：立即丢弃尚未开始执行的完成处理，并唤醒阻塞在 run() 的线程。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& ctx : contexts_) {
            if (ctx) {
                ctx->stop();
            }
        }
    }

    // 修改后的代码
    // 最后 join —— 全程不持锁。
    //
    // ★★ 危险用法检测：绝不能在**本池的 io 线程**上析构本池。
    //
    //   `std::thread::join()` 等待自身是**未定义行为**（典型表现：抛
    //   `std::system_error` 或永久死锁）。而 "在 io 回调里释放最后一个
    //   shared_ptr<IoPool>" 恰恰会走到这里 —— 例如：
    //
    //       mech->close_all();
    //       mech.reset();     // 若此刻在 on_linkdown 等回调里 → 危险
    //
    //   检测到该误用后的处理策略：
    //     * Debug (`_DEBUG`)：触发 assert，让问题在开发期立刻暴露；
    //     * Release          ：跳过对"自己"的 join（detach 由线程自身结束时
    //                          完成），并继续 join 其它线程。
    //     这样最坏情况下是"该线程稍后自行退出"，而不是整个进程死锁。
    //
    //   ⚠ 正确的用法是：pool 必须在**非 io 线程**上释放（通常在主线程）。
    // 检测：是否在**本池的 io 线程**上析构。
    //
    //  `std::thread::join()` 等待自身是未定义行为（死锁或抛异常）。
    //  而"在 io 回调里释放最后一个 shared_ptr<IoPool>"恰好会走到这里。
    //
    //  处理策略：
    //    * 在 io 线程上：**不 join、不 detach**（detach 会让线程在
    //      ~IoPool 返回后继续访问已销毁的 io_context → 必然 UAF）。
    //      改为把整个池的清理工作**转交**给一个 detach 出去的清理线程：
    //      由它先 join 掉原 io 线程，再销毁 contexts_。
    //      这样当前 io 线程可以立即从 ~IoPool 返回并退出回调栈，
    //      后续所有销毁都在别的线程上完成 —— 无 UAF、无死锁。
    //    * 正常情况（外部线程）：直接 join，行为不变。
    const std::thread::id self = std::this_thread::get_id();
    bool on_io_thread = false;
    for (const auto& th : threads) {
        if (th.joinable() && th.get_id() == self) {
            on_io_thread = true;
            break;
        }
    }

    if (on_io_thread) {
        // ── 误用路径：在 io 线程上析构 ──────────────────────────────────
        //   把"join 原线程 + 销毁 contexts"整体搬到清理线程。
        //
        //   注意：这里必须把 contexts_ **移出**本对象，否则 ~IoPool 返回后
        //   成员会析构，而清理线程还要用它。
        std::vector<std::shared_ptr<io_context_t>> ctxs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ctxs.swap(contexts_);
        }

#if defined(_DEBUG)
        // Debug 下尽早暴露误用：这不是"可用"的路径，只是"不崩"的兜底。
        assert(false &&
               "IoPool 在本池的 io 线程上被析构！"
               "请勿在回调里释放最后一个 shared_ptr<IoPool>（见 executor.hpp）。");
#endif

        std::thread([threads = std::move(threads), ctxs = std::move(ctxs)]() mutable {
            for (auto& th : threads) {
                if (th.joinable()) {
                    th.join();      // ← 等原 io 线程退出回调栈
                }
            }
            // 此时所有 io 线程已结束，可安全销毁 context（及其队列）。
            for (auto& ctx : ctxs) {
                ctx.reset();
            }
        }).detach();

        return;   // 本线程立即返回，不再触碰任何成员
    }

    // ── 正常路径：在外部线程上析构 ──────────────────────────────────────
    for (auto& th : threads) {
        if (th.joinable()) {
            th.join();
        }
    }

    // ★ 关键：join 之后线程已达屏障，此时才真正**销毁 io_context**。
    //
    //   `io_context::stop()` 只是让 run() 尽快返回。**尚未执行**的 handler
    //   仍留在队列里、仍持有它们捕获的对象（例如 `shared_ptr<Conn>`）。
    //   这些 handler 会等到 io_context **析构时**才被释放 —— 也就是说，
    //   被捕获对象的析构函数会在"context 正在销毁的过程中"执行。
    //
    //   若该对象的析构又要访问同一个 io_context（例如它持有
    //   `tcp::socket` / `udp::socket` / `steady_timer` 这些绑定到该 context
    //   的 IO 对象），就是"在销毁 A 的过程中依赖 A" → 未定义行为
    //   （实测表现为退出期 0xC0000409 / 0xC0000005，且**只在多轮创建-销毁时复现**）。
    //
    //   ★ 本库的解法是**进程级运行时**：本类的唯一实例由 `global()` 创建、
    //     生命周期 = 进程，它持有的 `io_context` **永不销毁**。于是
    //     "IO 对象的析构访问了已销毁的 context"这一整类问题在结构上
    //     就不可能发生 —— 无论那些 IO 对象属于谁、活多久。
    //
    //   这同时让「库内对象强自持」成立：在途完成处理捕获自己的 shared_ptr，
    //   会形成引用环
    //     （io_context -> 队列 -> handler -> Conn -> Impl -> IoPool -> io_context）
    //   但环**无害** —— 没有谁需要它死（context 永不销毁）。
    //
    //   历史上这里需要 `drain()` 冲刷队列来断环，而 `drain()` 跨线程
    //   `poll()` 同一个 context，破坏了"1 context = 1 线程"的核心不变量，
    //   还会让**同一连接**的完成处理并发执行。
    //
    //   ⚠ 若将来要让本池**可销毁**，就必须同时把库内对象改回弱捕获
    //     并重新引入某种排空机制 —— 两者必须**配套**，不能只改一半。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& ctx : contexts_) {
            ctx.reset();          // 队列中的 handler 在此刻被析构并释放其捕获物
        }
        contexts_.clear();
    }
}

void IoPool::stop_all() noexcept
{
    // ── 语义（重要，曾经是个 bug）──────────────────────────────────────────
    //  本函数只负责「停止投递新任务并唤醒 io 线程」，**不 join**。
    //  线程仍会在析构（或显式 join）时被回收。
    //
    //  ⚠ 历史 bug：旧实现里把 `stopped_` 置为 true，导致随后的 ~IoPool
    //     在开头就 `if (stopped_) return;`，于是 `threads_` 从未 join ——
    //     `std::thread` 在 joinable 状态下析构会直接 `std::terminate()`
    //     （表现为 0xC0000409）。现已把「是否已 stop」与「是否已 join」
    //     拆成两个独立标志。
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_) {
        return;   // 幂等
    }
    stop_requested_ = true;
    running_.store(false, std::memory_order_release);
    guards_.clear();
    for (const auto& ctx : contexts_) {
        if (ctx) {
            ctx->stop();
        }
    }
}


io_context_t& IoPool::next_io_context() noexcept
{
    // size_ 恒 >= 1（构造时已归一化），因此下面的取模不会除零。
    const std::size_t idx = rr_.fetch_add(1, std::memory_order_relaxed) % size_;
    // contexts_ 只在 start() 期间被写入，之后只读；start() 返回前不暴露 this
    // （create() 返回后才可见），因此此处无需加锁。
    return *contexts_[idx];
}

bool IoPool::is_io_thread() const noexcept
{
    const std::thread::id self = std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::thread::id& id : io_thread_ids_) {
        if (id == self) {
            return true;
        }
    }
    return false;
}

strand_t IoPool::make_strand_of(io_context_t& ioc) noexcept
{
    return asio::make_strand(ioc);
}

// =============================================================================
//  关于 strand：当前线程模型下它是冗余的
// =============================================================================
//  本类保证「1 个 io_context 只由 1 条线程 run()」（见 start() 中的循环）。
//  Asio 的语义是：**同一个 io_context 的 handler 只会被一条线程执行**，
//  除非有多条线程同时 run() 同一个 io_context。
//
//  因此在本类的线程模型下：
//    * 同一 context 上投递的所有 handler **天然串行**；
//    * strand 提供的"额外串行保证"**用不上**；
//    * 而 strand 内部本身要用 mutex（asio/detail/strand_executor_service.hpp
//      里有 `mutex* mutex_` 与 193 个 mutex 的共享池），因此是纯开销。
//
//  实测（bench/bench_strand.cpp，Windows/MSVC /O2/Asio 1.30.2）：
//    * 纯投递 post + poll_one：无 strand ~380 ns/op，+strand ~520 ns/op
//      → strand 增量约 **+140 ns/次投递**（相对 +37%）
//    * UDP async_send_to：增量同为 ~140 ns，但分母 ~1700 ns，
//      相对开销摊薄到 +8% ~ +30%
//
//  ★ 因此 netmech 的 IO 路径**不再使用 strand**，改为依赖上述线程模型不变量。
//    若将来有人把 IoPool 改成"多线程 run 同一个 io_context"，
//    必须同时恢复 strand，否则「同一连接回调串行」的契约会被破坏。
// =============================================================================

} // namespace netmech
