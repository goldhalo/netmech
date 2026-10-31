// =============================================================================
//  netmech - 业务线程池
// =============================================================================
//  文件    : netmech/worker_pool.hpp
//  用途    : 把"网络收发"和"业务处理"分到两组线程上。
//
//  ★ 为什么需要它
//  ---------------------------------------------------------------------------
//  netmech 的 io 线程（`IoPool`）只应该做**收发**：读字节、投递、写字节。
//  任何耗时的事（解析、数据库、磁盘、加锁等待）放在 io 线程上都会拖慢
//  **所有**连接——因为一个 io_context 只有一条线程。
//
//  实测：在回调里 sleep 50ms，同一个 pool 上其它连接的帧间隔会跟着抖。
//
//  ★ 为什么库不自动投递
//  ---------------------------------------------------------------------------
//  回调参数（`span` / `string_view`）指向接收缓冲，**回调返回即失效**。
//  跨线程前必须拷贝，而"拷什么"是业务相关的：
//
//      void on_request(RtspSession& s, const RtspRequest& req) override {
//          auto own = std::make_shared<MyReq>(req.method, req.uri, req.body);
//          pool->post([own, &s] { handle(*own, s); });   // ← 必须自己拷
//      }
//
//  所以本类只提供**线程池**，不替应用决定投递什么。
//
//  ★ 和 IoPool 的关系
//  ---------------------------------------------------------------------------
//  两组线程，职责分离：
//
//      IoPool（io 线程）         WorkerPool（业务线程）
//      ├─ read / write           ├─ 协议状态机
//      ├─ 定时器                 ├─ 数据库 / 文件
//      └─ 投递 ────────────────► └─ 上报 / 转发
//
//  两者互不阻塞。业务慢了只会积压在队列里，不会拖慢 io。
// =============================================================================
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <system_error>

namespace netmech {

/// 业务线程池。
///
/// @par 线程模型
///  固定 N 条线程，从队列取任务执行。任务之间**不保证顺序**（多线程并发），
///  需要保序时用 `post_to(index, job)` 指定固定线程。
class WorkerPool {
public:
    struct Config {
        /// 线程数。0 表示按 CPU 核数（`hardware_concurrency()`）。
        std::size_t threads = 0;

        /// 队列上限（任务个数）。0 表示无界。
        ///
        /// @note 有界队列 + `post()` 返回 false 是一种**反压**：
        ///       业务跟不上时，调用方（io 线程）能立刻知道，而不是无限堆积
        ///       把内存吃光。
        std::size_t max_queue = 0;
    };

    /// 创建。
    ///
    /// @note 刻意不写成 `create(Config cfg = Config{})`：嵌套类型的默认成员
    ///       初始化器在外部类定义完成前不可用（GCC 报错、MSVC 放过）。
    [[nodiscard]] static std::shared_ptr<WorkerPool> create();
    [[nodiscard]] static std::shared_ptr<WorkerPool> create(Config cfg,
                                                            std::error_code& ec);

    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /// 投递一个任务。
    ///
    /// @return false 表示未受理（已停止 / 队列满）。**调用方要处理这个返回值**：
    ///         丢弃、计数、或者放慢上游。
    bool post(std::function<void()> job);

    /// 投递到**指定线程**（按 `index % 线程数` 取模）。
    ///
    /// @par 什么时候需要
    ///  同一个会话的回调要保持顺序（比如 RTSP 的 OPTIONS → DESCRIBE → SETUP
    ///  必须按序处理）。用会话 id 做 index 就能保证落到同一条线程。
    bool post_to(std::size_t index, std::function<void()> job);

    /// 当前是否在**业务线程**上执行。
    ///
    /// @note 用来避免"业务线程里又 post 一次"的多余跳转。
    [[nodiscard]] bool in_worker_thread() const noexcept;

    [[nodiscard]] std::size_t thread_count() const noexcept;

    /// 排队中 + 正在执行的任务数（诊断用）。
    [[nodiscard]] std::size_t pending() const noexcept;

    /// 已完成的任务数。
    [[nodiscard]] std::size_t completed() const noexcept;

    /// 停止：不再受理新任务，已排队的跑完，join 全部线程。
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;

private:
    WorkerPool();
    class Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace netmech
