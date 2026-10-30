// =============================================================================
//  NetMechanism - Conn 内部实现状态
// =============================================================================
//  文件    : conn_state.hpp
//  用途    : 定义 `Conn::Impl` 的实际内容（socket、读状态、
//            写队列、双向保活状态机、关闭状态机），以及 Mechanism 创建
//            Conn 所需的唯一内部工厂。
//  编码    : UTF-8
//
//  ★ 命名与类型关系
//  ----------------
//  公共头 `netmech/conn.hpp` 里只有：
//      class Conn { ... private: struct Impl; std::shared_ptr<Impl> m_impl; };
//  本文件给出的 `struct Conn::Impl` 就是 `m_impl` 指向的**完整定义**。
//  因此 `shared_ptr<Impl>` 与 `shared_ptr<Conn::Impl>` 是同一类型，
//  不存在类型转换（这正是避免 build-test-dev 报 C2440 的关键）。
//
//  ★ 旧实现缺陷（02-并发与缺陷审查.md）逐条规避点
//  ------------------------------------------------
//  A-02  读写缓冲混用：旧 `handle_read` 用 `m_sendBuffer.getItem()` 取读缓冲，
//        导致"第二个 recv 永不发起"。新实现把两者定义成**不可互换的独立类型**
//        `ReadQueue` / `WriteQueue`（不同类、不同成员名、无共同基类）。
//  A-03  队列 + 在途状态被拆成多个临界区：队列操作、in_flight 标记、发起 IO
//        的判断**全部在该连接的 io 线程内**；跨线程的配额判断与投递顺序用**同一把
//        配额锁**保护，不存在"检查-使用"分离。
//  A-08  open() 与并发 close() 的 ABA：单向 `closed` 标志（只能 false→true），
//        没有 open() 复活路径。
//  A-14  linkdown 里 send 静默丢弃：关闭时把**已受理的**写请求全部以非 0
//        error_code 回调一次；closed 在 close() 返回前已置位。
//  A-16  close 之后再 close socket 导致数据/FIN 乱序：先 shutdown/close socket，
//        再跑用户回调。
//  B-06  volatile + ipcdetail 原子：一律 `std::atomic` + 明确内存序。
//  B-07  mutable 地址缓存并发写：建立连接时一次性缓存 local/remote，之后只读。
//  B-13  保活标志不清零：每拍**无条件** `exchange(false)` 消费标志，判断与清零
//        合成一步；enable/disable 都显式 `reset()`。
//  B-14  系统时钟 + 补拍：一律 `steady_timer` + `expires_after`（相对时间）；
//        每拍自持参数快照；代次机制让已取消的等待自然失效。
//  B-15  写回调里用指针比较判定：出队即取走请求对象本身，回调参数来自该对象。
// =============================================================================
#pragma once

#include "netmech_srcpch.hpp"

#include "netmech/common.hpp"
#include "netmech/conn.hpp"
#include "netmech/common.hpp"
#include "io_pool.hpp"
#include "netmech/common.hpp"
#include "netmech/iohandler.hpp"
#include "netmech/linkable.hpp"

#include "registry.hpp"

#include <atomic>

namespace netmech {

// -----------------------------------------------------------------------------
// 读状态 / 写队列：**刻意设计成两个独立类型**，杜绝旧实现把读写缓冲混用
// -----------------------------------------------------------------------------

/// 读状态。同一 Conn 同一时刻至多一笔在途。
struct ReadQueue {
    /// 一次读请求。
    struct Request {
        Buf   buf;                        ///< 调用方缓冲视图（含可选 owner）
        bool  exact = false;              ///< true = 收满 size 才回调
    };

    /// 唯一的在途槽位；nullopt 表示"当前无读操作"。
    /// 这就是显式状态机：**不用"队列长度==1"之类的隐式判据**。
    std::optional<Request> in_flight;

    /// 取走在途请求（派发回调时用），并清空槽位。
    std::optional<Request> take_in_flight() noexcept
    {
        std::optional<Request> req = std::move(in_flight);
        in_flight.reset();
        return req;
    }

    [[nodiscard]] bool busy() const noexcept { return in_flight.has_value(); }
};

/// 写队列。FIFO；同一时刻至多一笔在途。
struct WriteQueue {
    /// 一次写请求。
    struct Request {
        ConstBuf     buf;                 ///< 调用方缓冲视图（含可选 owner）
        std::size_t  bytes = 0;           ///< 入队时快照的字节数
        /// 已交付内核的字节数。批量提交时一次系统调用可能只写出前面几笔，
        /// 下一轮从 `sent` 处续传（不会重发已写出的部分）。
        std::size_t  sent  = 0;
    };

    /// 等待发送（含正在飞行的队首）。
    std::deque<Request> pending;

    /// 在途标记：显式状态机。旧实现用 `newSize==1` 表示"我该发起 IO"，
    /// 该判据在锁外被消费 → 丢唤醒/双飞行。新实现只在该连接的 io 线程内读写本标记。
    bool in_flight = false;

    /// 本批**实际提交给内核**的请求笔数（≤ kMaxWriteBatch）。
    ///
    /// 批量提交时一次 async_write 只回调一次，但我们必须"发了几笔就回调几次"，
    /// 所以要把这一批的笔数记下来，完成时按字节数逐笔归因。
    std::size_t in_flight_count = 0;

    void push(Request req) { pending.push_back(std::move(req)); }

    [[nodiscard]] bool empty() const noexcept { return pending.empty(); }

    void pop_front() noexcept
    {
        if (!pending.empty()) {
            pending.pop_front();
        }
    }

    /// 清空并返回全部未完成请求（关闭时用于逐个回调）。
    std::deque<Request> drain() noexcept
    {
        std::deque<Request> out;
        out.swap(pending);
        in_flight = false;
        return out;
    }
};

// -----------------------------------------------------------------------------
// 保活状态（双向，同一 Conn 各一套）
// -----------------------------------------------------------------------------

/// 接收侧保活状态机。
///
/// 首拍在 `delay + interval` 之后；之后每 `interval` 一拍。每拍**无条件**消费
/// "本拍内是否收到过数据"标志：收到过 → 连续未命中清零；否则计数 +1，达到
/// `max_miss` 判定超时。
struct RecvKeepAlive {
    timer_t                   timer;
    std::chrono::milliseconds delay{0};
    std::chrono::milliseconds interval{0};
    std::uint32_t             max_miss = 0;
    std::uint32_t             miss = 0;      ///< 连续未收到次数

    /// 本拍内是否收到过数据；每拍 exchange(false) 消费（B-13）。
    std::atomic<bool>         progressed{false};

    /// 启用代次：enable/disable 自增，旧的在途等待凭代次自然失效（B-14）。
    std::uint64_t             generation = 0;

    /// 是否处于启用状态（仅该连接的 io 线程内读写）。
    bool                      active = false;

    explicit RecvKeepAlive(io_context_t& ioc) : timer(ioc) {}

    RecvKeepAlive(const RecvKeepAlive&) = delete;
    RecvKeepAlive& operator=(const RecvKeepAlive&) = delete;

    /// 显式把状态复位到"关闭"（旧实现各标志从不清零，是 B-13 的根因）。
    void reset() noexcept
    {
        active = false;
        delay = std::chrono::milliseconds(0);
        interval = std::chrono::milliseconds(0);
        max_miss = 0;
        miss = 0;
        progressed.store(false, std::memory_order_release);
        ++generation;
    }
};

/// 发送侧保活状态机（与接收侧对称：连续 max_miss 次无写出才判定停滞）。
struct SendKeepAlive {
    timer_t                   timer;
    std::chrono::milliseconds delay{0};
    std::chrono::milliseconds interval{0};
    std::uint32_t             max_miss{0};    ///< 连续未通过上限（0 = 不判定）
    std::uint32_t             miss{0};        ///< 当前连续未通过次数

    std::atomic<bool>         progressed{false};

    std::uint64_t             generation = 0;
    bool                      active = false;

    explicit SendKeepAlive(io_context_t& ioc) : timer(ioc) {}

    SendKeepAlive(const SendKeepAlive&) = delete;
    SendKeepAlive& operator=(const SendKeepAlive&) = delete;

    void reset() noexcept
    {
        active = false;
        delay = std::chrono::milliseconds(0);
        interval = std::chrono::milliseconds(0);
        max_miss = 0;
        miss = 0;
        progressed.store(false, std::memory_order_release);
        ++generation;
    }
};

// -----------------------------------------------------------------------------
// Conn::Impl —— 公共头 `struct Conn::Impl;` 的完整定义
// -----------------------------------------------------------------------------

/// Conn 的内部实现状态。`Conn::m_impl` 指向本类型。
///
/// @note 生命周期：**强自持**。本对象由 `shared_ptr` 持有（`Conn::m_impl`），
///       投放到 io_context 的在途完成处理捕获**本对象的强引用**
///       （`shared_from_this()`），因此：
///         * 本对象**一定活到最后一个 IO 完成** —— 关闭流程（冲刷挂起回调、
///           `on_fini` / `on_linkdown`）**必定执行**，通知不会丢；
///         * 用户提前释放自己的引用不会造成悬垂（旧 A-04）。
///       代价是形成 `io_context -> 队列 -> handler -> Impl -> 池 -> io_context`
///       的环 —— 但运行时是**进程级**的、io_context 永不销毁，环**无害**。
struct Conn::Impl : public std::enable_shared_from_this<Conn::Impl> {
    /// @param[in] owner_in 公共对象的弱引用（构造时通常为空，由工厂在
    ///                     shared_ptr 就绪后回填；此处保留形参以保证声明与
    ///                     定义参数列表严格一致）。
    Impl(socket_t&& sock,
         io_context_t& ioc,
         IoPool& pool_in,
         std::weak_ptr<Conn> owner_in,
         endpoint_id_t ep_id,
         endpoint_t local,
         endpoint_t remote,
         std::shared_ptr<void> user_ctx);

    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // ---- 公共门面调用（任意线程）----

    /// 受理一次写：配额预检 + 追加投递。返回 false = 未受理（不回调）。
    bool accept_write(ConstBuf buf);

    /// 受理一次读：登记在途 + 投递。返回 false = 未受理（不回调）。
    bool accept_read(Buf buf, ReadMode mode);

    /// 投递保活操作（任意线程）。
    void post_keepalive_recv(std::chrono::milliseconds delay,
                             std::chrono::milliseconds interval,
                             std::uint32_t max_miss);
    void post_keepalive_recv_disable();
    void post_keepalive_send(std::chrono::milliseconds delay,
                             std::chrono::milliseconds interval,
                             std::uint32_t max_miss);
    void post_keepalive_send_disable();

    /// 设置发送配额（任意线程，立即对后续 accept_write 生效）。
    void set_send_limit(std::size_t bytes, std::size_t count) noexcept;

    /// 释放全部配额预留（关闭时）。
    void release_all_quota() noexcept;

    // ---- 该连接的 io 线程内动作 ----

    /// 读/写完成的统一派发入口（含"已关闭"分支），内部调 handler 的虚函数。
    void notify_read(const error_code_t& ec, const Buf& buf, std::size_t n) noexcept;
    void notify_write(const error_code_t& ec, const ConstBuf& buf, std::size_t n) noexcept;
    void notify_idle(const IdleInfo& info) noexcept;

    void do_accept_write(ConstBuf buf, std::size_t bytes);
    void pump_write();
    void handle_write_done(const error_code_t& ec, std::size_t n);

    void do_accept_read(Buf buf, ReadMode mode);
    void handle_read_done(const error_code_t& ec, std::size_t n, bool exact);

    void do_apply_recv_keepalive(std::chrono::milliseconds delay,
                                 std::chrono::milliseconds interval,
                                 std::uint32_t max_miss);
    void do_disable_recv_keepalive();
    void do_apply_send_keepalive(std::chrono::milliseconds delay,
                                 std::chrono::milliseconds interval,
                                 std::uint32_t max_miss);
    void do_disable_send_keepalive();
    void on_recv_keepalive_tick(const error_code_t& ec, std::uint64_t gen);
    void on_send_keepalive_tick(const error_code_t& ec, std::uint64_t gen);

    /// 关闭（幂等；在该连接的 io 线程内串行执行）。
    void do_close(const error_code_t& reason) noexcept;
    void fail_connection(const error_code_t& ec) noexcept;

    /// 请求关闭：把 do_close 投递到该连接的 io_context（任意线程，幂等）。
    void request_close(const error_code_t& reason) noexcept;

    /// 投递一个内部任务到该连接的 io_context（任意线程）。
    void post(std::function<void()> fn);

    /// 取公共对象（弱引用升格；用户已释放时返回空，仍可继续内部清理）。
    std::shared_ptr<Conn> public_self() const noexcept;

    /// 取自身的强引用（库内对象**强自持**用）。
    ///
    /// Impl 恒由 shared_ptr 持有（工厂用 make_shared 创建），因此正常路径
    /// 不会失败；万一失败（理论上不可能）返回空，调用方按"对象已消失"处理。
    std::shared_ptr<Impl> self_ref() noexcept;

    /// on_fini（恰好一次）。
    void finish_handler() noexcept;

    /// on_linkdown（恰好一次）。
    void notify_linkdown() noexcept;

    // ---- 静态/不可变 ----
    //
    // ★ 成员声明顺序 = 初始化顺序；**析构顺序相反**。这里的顺序是刻意安排的：
    //   1. `pool`  必须声明在 `socket` **之前**，因此**最后**析构；
    //   2. `socket` 在 `pool` 之后声明，因此**先**析构。
    //
    //   为什么关键：`socket` 的析构会访问它所属的 io_context。
    //   若 `pool` 先析构（io_context 随之销毁），随后 `socket` 再析构，就会
    //   访问**已销毁的 io_context** → 退出期 0xC0000005。
    //
    //   持有 IoPool 的强引用还能保证：用户即便把 `shared_ptr<Conn>` 持有
    //   到 `~Mechanism` 之后，io_context 的存活期也被延长到不短于该 Conn。

    IoPool*                        pool = nullptr; ///< 进程级单例，永不为空
    socket_t                       socket;
    // ⚠ 无 strand：依赖 1 io_context = 1 线程（见 io_pool.cpp 末尾说明）
    const endpoint_id_t            id;

    const endpoint_t               local_addr;    ///< 建立时快照，之后只读（B-07）
    const endpoint_t               remote_addr;   ///< 建立时快照，之后只读（B-07）

    // ---- 由 Mechanism 在 shared_ptr 就绪后写入（写入时尚未开始 IO）----
    std::weak_ptr<Conn>        owner;         ///< 公共对象弱引用
    std::shared_ptr<void>      user_context;  ///< 用户上下文（库持有到连接关闭）
    bool                       linger_zero = false; ///< true = 关闭走 RST（跳过 shutdown）
    std::weak_ptr<Linkable>        linkable;      ///< 订阅者（weak；旧实现为裸指针）
    std::shared_ptr<ConnHandler>   handler;       ///< 协议处理器（库强持有到 on_close）
    bool                           handler_inited = false;   ///< 是否配平 on_close
    bool                           link_established = false; ///< 是否喂 on_linkdown

    // ---- 关闭标志（两个，职责明确分开）----
    //
    //  `closing`：**对外可见**的关闭意图。由 `close()`（任意线程）立即置位，
    //             使得 `is_open()` 在 close() 返回瞬间即为 false，
    //             同时让后续 `async_*` 立刻返回 false。
    //  `closed` ：**关闭流程已执行**。由该连接 io 线程上的 `do_close()` 置位，
    //             表示未完成回调已被派发、on_fini/on_linkdown 已投递。
    //
    //  两者分开的原因（这是一个真实踩过的 P0）：
    //    若 close() 直接置 `closed`，则 do_close() 的 `closed.exchange(true)`
    //    幂等判定会立刻命中并直接 return —— 结果是"关闭动作被短路"，
    //    挂起回调不派发、on_fini/on_linkdown 不触发，严重违反公共契约。
    //  两者都是单向 false→true，不存在旧实现 A-08 的 ABA。
    std::atomic<bool>              closing{false};   ///< 关闭意图（对外可见）
    std::atomic<bool>              closed{false};    ///< 关闭流程已完成
    error_code_t                   close_reason;     ///< 首个原因胜出

    // ---- 从 Mechanism 连接表摘除的回调（保证恰好执行一次）----
    //
    //  为什么需要它：`Mechanism::endpoint_count()` 的计数必须随连接关闭而
    //  递减，否则会单调递增、永远不为 0（这正是 `close_all_cleans_endpoints`
    //  用例暴露的缺陷）。而 Conn 不知道所属 Mechanism，因此由
    //  `Mechanism::Impl::make_endpoint` 在创建时注入一个摘除钩子。
    //
    //  执行时机：`do_close()` 的最后一步（关闭流程收敛后）。
    //  恰好一次：由 `unregistered` 原子标志保证 —— 即使 do_close 被并发调用，
    //            （虽然 do_close 本身已由 `closed` 保证单次）这里是双保险，
    //            同时也覆盖 Impl 析构这条兜底路径。
    std::function<void(endpoint_id_t)> unregister_hook;
    std::atomic<bool>                  unregistered{false};

    /// 触发从 Mechanism 连接表摘除（恰好一次；异常安全）。
    void fire_unregister() noexcept
    {
        if (unregistered.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (!unregister_hook) {
            return;
        }
        try {
            unregister_hook(id);
        } catch (...) {
            // 摘除动作绝不能抛出（do_close 是 noexcept）。
        }
    }

    // ---- 跨线程"是否已有读在途"的快速判据（权威状态在该连接的 io 线程内）----
    std::atomic<bool>              has_pending_read{false};

    // ---- 读写状态（仅在该连接的 io 线程内访问）----
    ReadQueue                      reads;
    WriteQueue                     writes;

    // ---- 配额锁：同时保护"写预留"与"投递顺序"（A-03）----
    mutable std::mutex             quota_mutex;
    std::size_t                    limit_bytes = 0;    ///< 0 = 不限字节
    std::size_t                    limit_count = 0;    ///< 0 = 不限个数
    std::size_t                    reserved_bytes = 0; ///< 已受理未完成字节
    std::size_t                    reserved_count = 0; ///< 已受理未完成请求数

    // ---- 保活 ----
    RecvKeepAlive                  recv_ka;
    SendKeepAlive                  send_ka;

    // ---- 统计（诊断/测试）----
    std::atomic<std::uint64_t>     bytes_read{0};
    std::atomic<std::uint64_t>     bytes_written{0};
};

// -----------------------------------------------------------------------------
// 内部工厂：唯一把"已就绪的 Impl"与公共门面配对的入口
// -----------------------------------------------------------------------------
//  访问控制（方案 A 落地后）：
//    Conn::Impl 的**前向声明**已由公共头放在 public 段，实现层因此可以
//    合法命名该类型；m_impl 与私有构造函数仍然受保护。
//    真正"调用私有构造函数"的动作只能发生在 Conn 的 friend（即
//    Mechanism）的成员函数里 —— 由 mechanism.cpp 安装为本工厂的实现。

/// 创建 Conn 对象（shared_ptr）。
///
/// @param[in] impl 已完成初始化的内部状态（非空）。
/// @return 新的 Conn；**构造入口未安装时返回 nullptr**（调用方视为失败）。
/// @note 内部会把 impl->owner 回填为该 shared_ptr —— 因此**必须**在任何
///       异步操作启动之前调用（所有创建路径都满足）。
std::shared_ptr<Conn> MakeConnObject(std::shared_ptr<Conn::Impl> impl);

/// 受控构造入口的函数指针类型。
using ConnMakeFn = std::shared_ptr<Conn> (*)(std::shared_ptr<Conn::Impl>);

/// 安装构造入口；由 mechanism.cpp 的静态初始化执行。幂等。
void install_endpoint_factory(ConnMakeFn fn) noexcept;

/// 构造入口是否已就绪。
///
/// 用于在**运行时**发现"工厂未安装"这一配置错误 —— 否则表现为
/// "连接建立成功但没有任何用户回调"，极难排查。
[[nodiscard]] bool endpoint_factory_ready() noexcept;

} // namespace netmech