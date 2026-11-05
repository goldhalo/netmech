// =============================================================================
//  worker_pool.cpp —— 业务线程池实现
// =============================================================================
#include <netmech/worker_pool.hpp>

#include <netmech/common.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>
#include <memory>
#include <cstddef>
#include <system_error>
#include <utility>

namespace netmech {

// =============================================================================
//  Impl
// =============================================================================

class WorkerPool::Impl {
public:
    Config cfg;

    // 每线程一个队列：这样 post_to(i) 才能真正落到固定线程上。
    // 用一把锁保护全部队列（简单优先；100 路量级下锁竞争不是瓶颈）。
    std::mutex                              mtx;
    std::condition_variable                 cv;
    std::vector<std::deque<std::function<void()>>> queues;

    std::vector<std::thread>  threads;
    /// 创建后**永不改动**的线程 id 表（供 in_worker_thread() 查询）。
    /// 不能直接查 `threads`：shutdown() 会把它 swap 走并 join，
    /// 关停窗口里查它会返回假，而它的典型用途恰恰是"关停期判断"。
    std::vector<std::thread::id> thread_ids;

    std::atomic<bool>         stop{false};
    std::atomic<bool>         running{false};
    std::atomic<std::size_t>  pending{0};
    std::atomic<std::size_t>  completed{0};
    std::size_t               count = 0;
    /// 轮转游标（每个实例独立）。
    std::atomic<std::size_t>  rr{0};

    bool push(std::function<void()> job, std::size_t which)
    {
        if (!job) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (stop.load(std::memory_order_acquire)) {
                return false;
            }
            if (cfg.max_queue != 0) {
                std::size_t total = 0;
                for (const auto& q : queues) { total += q.size(); }
                if (total >= cfg.max_queue) {
                    return false;   // 反压
                }
            }
            queues[which].push_back(std::move(job));
            pending.fetch_add(1, std::memory_order_relaxed);
        }
        cv.notify_all();
        return true;
    }

    void run(std::size_t idx)
    {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [&] {
                    return stop.load(std::memory_order_acquire) || !queues[idx].empty();
                });
                if (queues[idx].empty()) {
                    if (stop.load(std::memory_order_acquire)) {
                        return;
                    }
                    continue;
                }
                job = std::move(queues[idx].front());
                queues[idx].pop_front();
            }
            if (job) {
                job();
                pending.fetch_sub(1, std::memory_order_relaxed);
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    void shutdown()
    {
        // ★ 锁内**取走**线程句柄，锁外 join —— 老实现是"锁内只置标志、
        //   锁外 join 并 clear()"，而 in_worker_thread() 正在无锁遍历同一个
        //   vector：等到 clear() 析构里面的 std::thread 对象时，遍历方读到的是
        //   已析构对象（UB）。take-then-join 是同库 ~IoPool 的既有写法。
        std::vector<std::thread> local;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (stop.exchange(true, std::memory_order_acq_rel)) {
                return;   // 幂等
            }
            local.swap(threads);
        }
        cv.notify_all();

        // ★ 自 join 防护：从 worker 任务里调 stop() 时，join 自己会抛
        //   std::system_error(resource_deadlock_would_occur)，且异常会穿回
        //   用户任务。跳过自身（它随即从任务返回并自行退出）。
        const std::thread::id self = std::this_thread::get_id();
        for (auto& t : local) {
            if (!t.joinable()) {
                continue;
            }
            if (t.get_id() == self) {
                t.detach();
                continue;
            }
            t.join();
        }
        running.store(false, std::memory_order_release);
    }
};

// =============================================================================
//  WorkerPool
// =============================================================================

WorkerPool::WorkerPool() = default;

WorkerPool::~WorkerPool()
{
    stop();
}

std::shared_ptr<WorkerPool> WorkerPool::create()
{
    std::error_code ec;
    return create(Config{}, ec);
}

std::shared_ptr<WorkerPool> WorkerPool::create(Config cfg, std::error_code& ec)
{
    ec.clear();

    auto self = std::shared_ptr<WorkerPool>(new WorkerPool());
    self->m_impl = std::make_shared<Impl>();
    self->m_impl->cfg = cfg;

    std::size_t n = cfg.threads;
    if (n == 0) {
        n = std::thread::hardware_concurrency();
        if (n == 0) {
            n = 2;
        }
    }
    self->m_impl->count = n;
    self->m_impl->queues.resize(n);

    try {
        self->m_impl->threads.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            auto impl = self->m_impl;   // 线程持有 Impl，保证活到 join
            self->m_impl->threads.emplace_back([impl, i] { impl->run(i); });
        }
    } catch (const std::system_error& e) {
        ec = e.code();
        self->m_impl->shutdown();
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lk(self->m_impl->mtx);
        self->m_impl->thread_ids.reserve(self->m_impl->threads.size());
        for (const auto& t : self->m_impl->threads) {
            self->m_impl->thread_ids.push_back(t.get_id());
        }
    }

    self->m_impl->running.store(true, std::memory_order_release);
    return self;
}

bool WorkerPool::post(std::function<void()> job)
{
    if (!m_impl || m_impl->count == 0) {
        return false;
    }
    // 轮转选择一条线程（简单的负载分摊）。
    //
    // ★ 游标是**实例成员**，不是函数内 static —— 以前多个 WorkerPool 实例
    //   共用一个游标，互相扰动分发分布（"为什么这个池的任务分布这么怪"）。
    const auto idx =
        m_impl->rr.fetch_add(1, std::memory_order_relaxed) % m_impl->count;
    return m_impl->push(std::move(job), idx);
}

bool WorkerPool::post_to(std::size_t index, std::function<void()> job)
{
    if (!m_impl || m_impl->count == 0) {
        return false;
    }
    // ★ 真正保序：同一个 index 永远落到同一条线程的队列，
    //   该队列由这条线程独占消费，所以任务按投递顺序执行。
    return m_impl->push(std::move(job), index % m_impl->count);
}

bool WorkerPool::in_worker_thread() const noexcept
{
    if (!m_impl) {
        return false;
    }
    // ★ 查的是**创建后不变**的 id 表，不是 threads：
    //   shutdown() 会把 threads 里的句柄 swap 走并 join（join 期间线程还在
    //   收尾），如果查 threads 就会在关停窗口里返回假。
    //   加锁是为了与"创建时的填充"互斥（填充阶段之后只读）。
    const auto self = std::this_thread::get_id();
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    for (const auto& id : m_impl->thread_ids) {
        if (id == self) {
            return true;
        }
    }
    return false;
}

std::size_t WorkerPool::thread_count() const noexcept
{
    return m_impl ? m_impl->count : 0;
}

std::size_t WorkerPool::pending() const noexcept
{
    return m_impl ? m_impl->pending.load(std::memory_order_relaxed) : 0;
}

std::size_t WorkerPool::completed() const noexcept
{
    return m_impl ? m_impl->completed.load(std::memory_order_relaxed) : 0;
}

void WorkerPool::stop() noexcept
{
    if (m_impl) {
        m_impl->shutdown();
    }
}

bool WorkerPool::running() const noexcept
{
    return m_impl && m_impl->running.load(std::memory_order_acquire);
}

} // namespace netmech
