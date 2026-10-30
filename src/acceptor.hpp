// =============================================================================
//  NetMechanism - 监听器（内部实现）
// =============================================================================
//  文件    : acceptor.hpp
//  用途    : 在独占的 io_context（= 1 条 io 线程）上循环 async_accept；
//            接受失败时**退避重试**，
//            绝不静默熄火。
//  编码    : UTF-8
//
//  旧实现缺陷（A-09 / A-06 / A-01）的规避说明
//  ------------------------------------------
//  1. **失败不 rearm（A-09）**：旧 `CAsioTcpServer::handle_accept` 只有
//     `if (!e) {...}`，没有 else → 一次 accept 出错（EMFILE / ECONNABORTED /
//     fd 耗尽）之后再没有任何在途操作，线程 run() 全部返回 →
//     **服务器永久不再 accept，而对外看起来一切正常**。
//     新实现：任何错误（除 `operation_aborted`，那是本端主动关闭）都会：
//        a) 关闭 acceptor（监听 socket 可能已被内核废弃）；
//        b) 重建 acceptor 并重新 bind/listen；
//        c) 启动 `steady_timer` 指数退避（100ms 起，封顶 5s，带抖动）；
//        d) 到期后重新 async_accept。
//     退避期间若本端 close()，cancel 定时器并以 operation_aborted 结束。
//  2. **锁内 accept / 锁外 open（A-06）**：旧 `handle_passiveLink` 在
//     `m_mutexEpid` 之外 `open()`、之后再插入 map，两者之间是真实 UAF 窗。
//     新实现不存在"临时 endpoint 槽位"：每次 accept 得到一个**已连接 socket**，
//     立即移动交给 Mechanism，由后者在 `make_shared<Conn>` **之后**再
//     投递用户回调。`enable_shared_from_this` 永远在 shared_ptr 就绪后使用
//     （A-01 亦由此根除）。
//  3. **裸线程 + this（A-09）**：所有完成处理都通过 `shared_from_this()` 持有
//     自身，对象存活不依赖外部持有顺序。
//  4. **accept 与业务共用 io 池**：不再固定 4 条 accept 线程（设计约定 §4），
//     acceptor 跑在 Mechanism 的 io 池上，由本对象独占的那个 io_context
//     （只有 1 条线程 run）串行化。
// =============================================================================
#pragma once

#include "netmech_srcpch.hpp"

#include "io_pool.hpp"

#include <atomic>

namespace netmech {

/// 一个监听端点：绑定 local 后循环 accept。
///
/// @par 线程安全
///  - `open()` / `close()` 可从任意线程调用（内部互斥 + post 到 io_context）。
///  - 所有 accept 完成处理都在本对象的 io 线程上串行执行。
class Acceptor : public std::enable_shared_from_this<Acceptor> {
public:
    /// 每次 accept 成功时的交付点。由 Mechanism 提供。
    ///
    /// @param[in] sock   已连接 socket（移动语义：被调用后本对象不再使用）。
    /// @param[in] remote 对端地址。
    /// @param[in] local  本端地址。
    /// @return true 表示已接管（无论随后是否拒绝该连接）；false 表示接管失败
    ///         （例如运行时已关闭），本对象会负责关闭该 socket。
    /// @note 实现内部必须自行包 try/catch。
    using DeliverFn = std::function<bool(socket_t&& sock, const endpoint_t& remote,
                                         const endpoint_t& local)>;

    /// @param[in] pool  io 池**强引用**（本对象据此取 io_context，并保证
    ///                 io_context 的存活期不短于本对象）。
    /// @param[in] deliver 交付回调（非空）。
    Acceptor(IoPool& pool, DeliverFn deliver);

    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    /// 绑定并开始监听（同步完成 bind+listen，异步仅 accept 循环）。
    ///
    /// @param[in] local   本地地址。
    /// @param[in] backlog listen 队列长度；0 归一化为 SOMAXCONN。
    /// @return 成功返回值初始化的 error_code（判假）；失败返回具体错误。
    /// @note 重复调用返回 `already_initialized`。
    error_code_t open(const endpoint_t& local, std::uint32_t backlog);

    /// 关闭监听：停止 accept 循环、cancel 退避定时器、关闭 acceptor。幂等。
    void close() noexcept;

    /// 监听是否处于打开状态。
    [[nodiscard]] bool is_open() const noexcept;

    /// 本地地址（未 open 时返回空 endpoint）。
    [[nodiscard]] endpoint_t local_address() const;

    /// 是否因错误正在退避重试中（诊断/测试用）。
    [[nodiscard]] bool is_backoff() const noexcept;

    /// 因错误产生的退避次数（诊断/测试用）。
    [[nodiscard]] std::uint32_t retry_count() const noexcept;

    /// 底层 acceptor（仅诊断/测试用；不要直接调用 async_accept）。
    asio::ip::tcp::acceptor& socket() noexcept;

private:
    /// 发起一次 async_accept（io 线程内）。
    void do_accept();

    /// accept 完成处理（io 线程内）。
    void handle_accept(const error_code_t& ec);

    /// 安排退避重试（io 线程内）。
    void schedule_reopen(const error_code_t& ec);

    /// 重建 acceptor 并重新 bind/listen（io 线程内）。
    bool reopen_acceptor();

    /// 停止 accept 循环并关闭 acceptor（io 线程内）。
    void close_impl() noexcept;

    /// 配置新接受的 socket 选项。
    void configure_socket(socket_t& sock) noexcept;

private:

    // ★ 成员顺序即初始化顺序；**析构顺序相反**。这里的顺序是刻意安排的：
    //
    //   `pool_` 必须声明在 `acceptor_` / `retry_timer_` / `pending_socket_`
    //   **之前**，因此**最后**析构。原因是这些 IO 对象的析构函数会访问它们
    //   所属的 io_context；若池先析构（io_context 随之销毁），随后再析构
    //   socket/timer 就是"在销毁 A 的过程中依赖 A" → 退出期 0xC0000005。
    //
    //   `pool_` 持**强引用**（不是 weak）：`ioc_` 是裸引用，只有池活着它才
    //   有效。"弱引用 + 裸引用"是悬垂引用的温床 —— 这里直接用强引用把
    //   不变量编码进类型里。
    IoPool*                                              pool_;       ///< 进程级单例，永不为空；io_context 生存期由它保证
    io_context_t&                                         ioc_;       ///< 引用 pool_ 持有的 context（由 pool_ 保证有效）
    asio::ip::tcp::acceptor                              acceptor_;
    timer_t                                               retry_timer_;
    socket_t                                              pending_socket_;   ///< 在途 accept 槽位
    DeliverFn                                             deliver_;

    mutable std::mutex                                  mutex_;
    endpoint_t                                          local_;            ///< 期望绑定的地址
    endpoint_t                                          bound_local_;      ///< 实际绑定地址
    error_code_t                                        open_error_;
    std::uint32_t                                       backlog_ = 0;
    bool                                                opened_ = false;
    bool                                                stopping_ = false; ///< 本端主动关闭

    std::atomic<bool>                                   backoff_{false};
    std::atomic<std::uint32_t>                          retry_attempts_{0};
    std::atomic<std::uint32_t>                          retry_errors_{0};
};

} // namespace netmech
