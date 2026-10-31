// ============================================================================
//  test_worker_pool.cpp —— 业务线程池
// ============================================================================
#include <netmech/worker_pool.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "test_framework.hpp"

using namespace netmech;
using namespace netmech_test;

namespace {
using namespace std::chrono_literals;

bool wait_for(std::atomic<int>& v, int want, std::chrono::milliseconds t = 3000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + t;
    while (v.load() < want && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return v.load() >= want;
}
} // namespace

// ---------------------------------------------------------------------------
// 1. 基本投递
// ---------------------------------------------------------------------------
NETMECH_TEST(worker_pool_runs_jobs)
{
    std::error_code ec;
    WorkerPool::Config cfg;
    cfg.threads = 4;
    auto pool = WorkerPool::create(cfg, ec);
    REQUIRE(pool != nullptr);
    CHECK_EQ(pool->thread_count(), 4u);
    CHECK(pool->running());

    std::atomic<int> done{0};
    for (int i = 0; i < 100; ++i) {
        CHECK(pool->post([&done] { done.fetch_add(1); }));
    }
    CHECK(wait_for(done, 100));
    CHECK_EQ(pool->completed(), 100u);

    pool->stop();
    CHECK(!pool->running());
}

// ---------------------------------------------------------------------------
// 2. post_to 保序（同一个 index 的任务按投递顺序执行）
// ---------------------------------------------------------------------------
NETMECH_TEST(worker_pool_post_to_preserves_order)
{
    std::error_code ec;
    WorkerPool::Config cfg;
    cfg.threads = 4;
    auto pool = WorkerPool::create(cfg, ec);
    REQUIRE(pool != nullptr);

    constexpr int kJobs = 200;
    std::vector<int> order;
    std::mutex mtx;
    std::atomic<int> done{0};

    // 全部投到 index 7（→ 线程 3），必须按序
    for (int i = 0; i < kJobs; ++i) {
        pool->post_to(7, [&, i] {
            {
                std::lock_guard<std::mutex> lk(mtx);
                order.push_back(i);
            }
            done.fetch_add(1);
        });
    }
    REQUIRE(wait_for(done, kJobs));

    CHECK_EQ(static_cast<int>(order.size()), kJobs);
    bool sorted = true;
    for (int i = 0; i < kJobs; ++i) {
        if (order[static_cast<std::size_t>(i)] != i) { sorted = false; break; }
    }
    CHECK(sorted);   // ★ 保序

    pool->stop();
}

// ---------------------------------------------------------------------------
// 3. 有界队列反压
// ---------------------------------------------------------------------------
NETMECH_TEST(worker_pool_backpressure)
{
    std::error_code ec;
    WorkerPool::Config cfg;
    cfg.threads   = 1;
    cfg.max_queue = 4;
    auto pool = WorkerPool::create(cfg, ec);
    REQUIRE(pool != nullptr);

    // 占住唯一的线程
    std::atomic<bool> release{false};
    std::atomic<bool> blocked{false};
    pool->post([&] {
        blocked.store(true);
        while (!release.load()) { std::this_thread::sleep_for(1ms); }
    });
    for (int i = 0; i < 100 && !blocked.load(); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(blocked.load());

    // 队列容量 4：投 10 个，应该有被拒的
    int accepted = 0;
    for (int i = 0; i < 10; ++i) {
        if (pool->post([] {})) { ++accepted; }
    }
    CHECK(accepted <= 4);
    CHECK(accepted < 10);   // 至少有一个被拒（反压生效）

    release.store(true);
    pool->stop();
}

// ---------------------------------------------------------------------------
// 4. in_worker_thread
// ---------------------------------------------------------------------------
NETMECH_TEST(worker_pool_in_worker_thread)
{
    std::error_code ec;
    WorkerPool::Config cfg;
    cfg.threads = 2;
    auto pool = WorkerPool::create(cfg, ec);
    REQUIRE(pool != nullptr);

    std::atomic<int> inside{0};
    std::atomic<int> outside{0};

    // 主线程（不是 worker）
    if (!pool->in_worker_thread()) { outside.store(1); }

    pool->post([&] {
        if (pool->in_worker_thread()) { inside.store(1); }
    });

    for (int i = 0; i < 200 && inside.load() == 0; ++i) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK_EQ(outside.load(), 1);
    CHECK_EQ(inside.load(), 1);

    pool->stop();
}

// ---------------------------------------------------------------------------
// 5. stop 之后 post 返回 false
// ---------------------------------------------------------------------------
NETMECH_TEST(worker_pool_rejects_after_stop)
{
    std::error_code ec;
    auto pool = WorkerPool::create(WorkerPool::Config{}, ec);
    REQUIRE(pool != nullptr);

    pool->stop();
    CHECK(!pool->post([] {}));
    CHECK(!pool->running());
    pool->stop();   // 幂等
}

// ---------------------------------------------------------------------------
// 6. 默认线程数 = CPU 核数
// ---------------------------------------------------------------------------
NETMECH_TEST(worker_pool_default_threads)
{
    std::error_code ec;
    auto pool = WorkerPool::create(WorkerPool::Config{}, ec);
    REQUIRE(pool != nullptr);
    CHECK(pool->thread_count() >= 1u);
    pool->stop();
}
