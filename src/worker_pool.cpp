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

    std::atomic<bool>         stop{false};
    std::atomic<bool>         running{false};
    std::atomic<std::size_t>  pending{0};
    std::atomic<std::size_t>  completed{0};
    std::size_t               count = 0;

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
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (stop.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
        }
        cv.notify_all();
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads.clear();
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

    self->m_impl->running.store(true, std::memory_order_release);
    return self;
}

bool WorkerPool::post(std::function<void()> job)
{
    if (!m_impl || m_impl->count == 0) {
        return false;
    }
    // 轮转选择一条线程（简单的负载分摊）
    static std::atomic<std::size_t> rr{0};
    const auto idx = rr.fetch_add(1, std::memory_order_relaxed) % m_impl->count;
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
    // 简单判断：当前线程 id 是否在池里
    const auto self = std::this_thread::get_id();
    for (const auto& t : m_impl->threads) {
        if (t.get_id() == self) {
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
