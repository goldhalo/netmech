// =============================================================================
//  NetMechanism - 连接对象
// =============================================================================
//  文件    : netmech/conn.hpp
//  用途    : 定义 Conn 类（一条 TCP 连接的句柄 + 异步读写接口）。
//  依赖    : Boost 1.74 + 标准库。不含 <iostream>。
//  编码    : UTF-8
//
//  ★★ 三条铁律（读代码前先记住）★★
//  ---------------------------------------------------------------------------
//  [1] **Conn 只能由 std::shared_ptr 持有。**
//      构造函数是 private，且 private 继承 ::netmech::noncopyable。只能由
//      Mechanism 内部用 std::make_shared 创建并通过 shared_ptr 交给你。
//      绝不会有裸指针泄漏出来；回调里拿到 shared_ptr 就绝不会悬垂。
//
//  [2] **buffer 归调用方所有，库不拷贝。**
//      从 async_* 返回 true 起，到回调被调用为止，buffer 指向的内存必须有效。
//      详见 buffer.hpp 头部的完整契约。
//
//  [3] **async_* 返回 false 表示"未受理"，此时不会调用回调。**
//      false 的常见原因：连接已关闭 / buffer 为空 / 发送队列超限（反压）。
//      返回 false 时 buffer 立即归调用方自由处置。
//
//  ★ 线程模型
//  ----------
//  * 每个 Conn 在创建时绑定**一个 io_context**；而 `IoPool` 保证每个
//    io_context 恰好由 **1 条线程** `run()`。因此该 Conn 的所有回调
//    （读写完成、保活超时、close 完成）都发生在**同一条 io 线程**上、天然串行，
//    `in_flight` 标记与队列的读写也全在这条线程内完成，
//    因此不存在旧代码"队首即飞行中"的数据竞态。
//    ★ 本库**不使用 strand**（机制与前提见 executor.hpp 的线程模型说明）。
//  * `async_*` / `close()` / `set_send_queue_limit()` / `enable_*_keepalive()` /
//    `is_open()` / `id()` **可以从任意线程调用**（内部用 post 或原子量
//    保证安全）。
//  * **回调绝不重入**：回调里再调用 async_* 是安全的（会排队，不会破坏状态）。
//
//  ★ 读写顺序保证
//  -------------
//  * 多次 `async_write` 的完成顺序与受理顺序**一致**（FIFO）。旧实现里
//    "只有队列长度为 1 才发起"的隐式判据被替换为显式状态机：
//    `pending 队列 + in_flight 标记`，完成一个再发起下一个。
//  * `async_read_*` 同一时刻**只允许一笔**在途；在有读在途时再调用会被拒绝
//    （返回 false）。这是刻意的约束，避免旧代码里"随机一个读回调被误当作
//    另一个请求"的缺陷。
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include <asio/ip/tcp.hpp>

#include "netmech/common.hpp"
#include "netmech/common.hpp"
#include "netmech/iohandler.hpp"
#include "netmech/linkable.hpp"

namespace netmech {

// 注：`tcp` 别名（== asio::ip::tcp）定义在 fwd.hpp 中，全模块统一使用。

/// 一条已建立的 TCP 连接。（类文档见文件头"三条铁律"与"线程模型"。）
///
/// @par 创建
///  - 不能直接构造；由 `Linkable::on_accept()` / `Linkable::on_linkup()` 收到。
///
/// @par 销毁
///  - 关闭后内部持有的 socket 会被释放；当所有 shared_ptr 副本都被释放时
///    对象销毁。库内部不会长时间持有已被 on_linkdown 通知过的连接。
///  - ★ **强自持**：库投放到 io_context 的在途完成处理捕获本对象的**强引用**，
///    因此连接对象**一定活到最后一个 IO 完成** —— 关闭流程（冲刷挂起回调、
///    `on_close` / `on_linkdown`）**必定执行**，通知不会丢。
///    即使你先释放自己的 `shared_ptr<Conn>`，`on_linkdown` 依然会被投递，
///    且参数里的 `ep` **一定是一个有效的 Conn**。
///
/// @par 线程安全性
///  - 全部 public 方法线程安全（可从任意线程调用）。
///  - 全部回调在该 Conn 所在的 io 线程上串行执行（handle 之间不并发）。
class Conn : public std::enable_shared_from_this<Conn>, private ::netmech::noncopyable {
public:
    /// Conn 必定通过 shared_ptr 释放，析构函数公开以便 shared_ptr 能调用。
    ///
    /// @note 析构时会确保 socket 已关闭（若之前未 close()，等效于调用 close()）。
    /// @note 不要在回调内部释放**最后一个** shared_ptr 的持有者以外的地方
    ///       依赖析构顺序；库自身持有的引用会在 on_linkdown 后释放。
    ~Conn();

    /// 唯一的全局连接 ID（进程内单调递增，不会回绕复用）。
    ///
    /// @return 该连接的 ID，从 1 开始。
    /// @note 线程安全、noexcept、可从任意线程调用。
    /// @note 推荐用它作为用户在 map 里索引连接的键，而**不是**用 Conn 指针
    ///       地址（shared_ptr 副本地址不同）。
    std::uint64_t id() const noexcept;

    /// 本端地址（已建立连接的实际本地 ip:port）。
    ///
    /// @return 本地 tcp::endpoint。连接已关闭后返回缓存值（仍可用作日志）。
    /// @note 不抛异常。内部错误（如 socket 已失效）时返回默认构造的
    ///       tcp::endpoint（地址为 0.0.0.0:0）。
    /// @note 线程安全。
    [[nodiscard]] Address local_address() const;

    /// 对端地址。
    ///
    /// @return 远端 tcp::endpoint。语义同 local_address()。
    /// @note 线程安全。
    [[nodiscard]] Address remote_address() const;

    /// 创建这条连接时传入的用户上下文，原样返回（可为空）。
    ///
    ///  - 客户端：等于 `Mechanism::connect(..., user_ctx)` 传入的那个。
    ///  - 服务端：继承监听器的上下文，即 `Mechanism::listen(..., user_ctx)` 那个。
    ///
    /// @note 库持有该 shared_ptr 直到连接关闭；用它可以把"设备对象 / 监听器句柄"
    ///       安全地挂到连接上，而不必事后按地址反查。
    [[nodiscard]] std::shared_ptr<void> user_context() const noexcept;

    // -------------------------------------------------------------------------
    // 读
    // -------------------------------------------------------------------------

    /// 发起一笔读。同一时刻**只允许一笔**在途。
    ///
    /// @param[in] buf  目标缓冲。库往里写数据，不拷贝。
    /// @param[in] mode some = 收到多少算多少；exact = 收满 buf.size 才完成。
    /// @return true  已受理：库将恰好调用一次 `ConnHandler::on_read`。
    /// @return false **未受理**，不回调，buf 立即归你。原因仅三种：
    ///         1. 连接已关闭；
    ///         2. `buf.data == nullptr` 或 `buf.size == 0`；
    ///         3. **已有一笔读在途**（同一连接同一时刻只允许一笔读）。
    ///
    /// @par 为什么只允许一笔
    ///  多笔读在途时，完成回调无法确定对应哪一笔请求：缓冲区尺寸不同、帧长不同，
    ///  一旦错配就会把 A 的数据当作 B 的帧头解析，且无法恢复。只允许一笔在途，
    ///  等价于让"回调 ↔ 请求"成为一对一，这个歧义从结构上消失。
    ///
    /// @par buffer 生命周期
    ///  - `buf.owner` 非空：库持有它直到 on_read 返回，调用方提前析构也安全。
    ///  - `buf.owner` 为空：调用方必须保证 data 的内存在 on_read 返回前有效。
    ///  - 返回 false 时，库不访问该内存，也不增加 owner 的引用计数。
    ///
    /// @par 线程安全性
    ///  - 可从任意线程调用；完成通知在该连接所在的 io 线程上执行。
    bool read(Buf buf, ReadMode mode = ReadMode::some);

    // -------------------------------------------------------------------------
    // 写
    // -------------------------------------------------------------------------

    /// 发起一笔写（异步入队，FIFO）。
    ///
    /// @param[in] buf 源数据。提交后**内容不得修改**，直到 on_write 返回。
    /// @return true  已受理：库将恰好调用一次 `ConnHandler::on_write`。
    /// @return false **未受理**，不回调，buf 立即归你。原因：
    ///         * 连接已关闭（`NetErrc::link_closed`）；
    ///         * `buf.data == nullptr` 或 `buf.size == 0`（`NetErrc::invalid_argument`）；
    ///         * 发送队列达到上限 **反压**（`NetErrc::buffer_full`）——**不会因此断链**。
    ///
    /// @par 顺序保证（契约级，测试据此断言）
    ///  所有被受理的 write，其数据在 TCP 流上的顺序、以及 on_write 的完成顺序，
    ///  与**受理顺序**严格一致；绝不交错、绝不乱序、出错后也不重排。
    ///
    ///  受理顺序 = 调用线程在**发送队列锁**内完成"容量检查 + 入队"的先后，
    ///  因此多线程并发调用同样有确定顺序（锁序列化），**不依赖** io 线程调度。
    ///
    /// @par 为什么必须在调用线程加锁入队
    ///  若改为"只 post 到 io 线程再入队"，`write()` 返回 true 时尚未判断队列容量：
    ///  等 io 线程发现超限，这个已受理的请求只能被丢弃，违反"受理即恰好一次回调"。
    ///  锁内完成"检查 + 入队"是唯一能同时保证【同步返回是否受理】与
    ///  【FIFO 顺序确定】的做法。
    ///
    /// @par buffer 生命周期
    ///  - 返回 true 后，`buf` 指向的内存必须保持有效且**内容不被修改**，
    ///    直到 on_write 返回；`buf.owner` 非空时由库负责延长其生命周期。
    ///  - 返回 false 时立即归还。
    ///
    /// @par 线程安全性
    ///  - 可从任意线程调用；完成通知在该连接所在的 io 线程上执行。
    bool write(ConstBuf buf);

    // -------------------------------------------------------------------------
    // 发送队列配额
    // -------------------------------------------------------------------------

    /// 设置发送队列上限（反压阈值）。
    ///
    /// @param[in] bytes 队列中**待发送**字节总数的上限；0 表示"不限制字节数"。
    /// @param[in] count 队列中**待发送请求个数**的上限；0 表示"不限制个数"。
    /// @note 两者是**与**关系：任一超限即拒绝新的 async_write（返回 false）。
    ///       若两者都传 0 则等于不限制（不推荐，可能 OOM）。
    /// @note 该设置只影响后续 async_write 的受理判断，不影响已在队列中的请求。
    /// @note 可在 Linkable::on_accept/on_linkup 中调用做业务级配额。
    /// @note 线程安全：内部 post 到该连接的 io_context 生效；从调用返回后即保证后续
    ///       async_write 使用新配额（实现方须保证这一点）。
    void set_send_queue_limit(std::size_t bytes, std::size_t count);

    // -------------------------------------------------------------------------
    // 保活
    // -------------------------------------------------------------------------

    /// 启用接收侧保活。
    ///
    /// 每次检查未通过都会回调 `ConnHandler::on_idle()`
    /// （`IdleInfo.kind == recv`，`missed` 从 1 递增到 `max_miss`）；
    /// **第 `max_miss` 次回调返回之后**才以 `NetErrc::keepalive_timeout`
    /// 结束连接 —— 前面几次是处置窗口（发心跳、记日志、主动 close）。
    ///
    /// @param[in] delay    连接建立后到首次检查的延迟（可为 0）。
    /// @param[in] interval 检查间隔，必须 > 0。
    /// @param[in] max_miss 允许连续未收到数据的次数，必须 >= 1。
    /// @note 参数非法时**静默忽略**（`interval > 0`、`max_miss >= 1`、`delay >= 0`）。
    /// @note 重复调用会以新参数重启定时器（等价于"重新协商"）。
    /// @note 线程安全。
    void enable_recv_keepalive(std::chrono::milliseconds delay,
                               std::chrono::milliseconds interval,
                               std::uint32_t max_miss);

    /// 关闭接收侧保活（取消定时器，幂等）。
    ///
    /// @note 关闭后不再触发 `ConnHandler::on_idle()`。线程安全。
    void disable_recv_keepalive();

    /// 启用发送侧保活（与接收侧对称）。
    ///
    /// 每隔 `interval` 检查"自上次检查以来是否成功写出过"；每次未通过都回调
    /// `ConnHandler::on_idle()`（`kind == send`），第 `max_miss` 次之后才断链。
    ///
    /// @param[in] delay    连接建立后到首次检查的延迟（可为 0）。
    /// @param[in] interval 检查间隔，必须 > 0。
    /// @param[in] max_miss 允许的连续未写出次数，必须 >= 1。
    /// @note 参数非法时静默忽略。重复调用以新参数重启定时器。
    /// @note 线程安全。
    void enable_send_keepalive(std::chrono::milliseconds delay,
                               std::chrono::milliseconds interval,
                               std::uint32_t max_miss = 3);

    /// 关闭发送侧保活（取消定时器，幂等）。
    ///
    /// @note 线程安全。
    void disable_send_keepalive();

    // -------------------------------------------------------------------------
    // 关闭
    // -------------------------------------------------------------------------

    /// 主动关闭连接。**幂等**，重复调用安全。
    ///
    /// 行为：
    ///  1. 立即把 `is_open()` 置为 false（从任意线程观察都一致）；
    ///  2. cancel 所有挂起的读/写/保活定时器；
    ///  3. 所有已受理但未完成的 async_* 回调都会以
    ///     `asio::error::operation_aborted` 被调用**恰好一次**；
    ///  4. 在该连接的 io 线程上投递 on_close() 与 on_linkdown()（若连接曾被接管）；
    ///  5. 关闭底层 socket。
    ///
    /// @par 前置条件
    ///  - 无。可在任意线程、任意状态（包括已关闭）调用。
    ///
    /// @par 线程安全性
    ///  - 线程安全。可在任意回调内部调用（包括 on_accept/on_linkup 内部）。
    ///
    /// @par 异常
    ///  - noexcept，绝无异常。底层 shutdown/close 的错误被忽略。
    ///
    /// @note 从 on_linkdown 里再次调用 close() 是无害的（幂等）。
    void close() noexcept;

    /// 连接是否仍然打开。
    ///
    /// @return true 表示 socket 尚未关闭且没有挂起的致命错误。
    /// @note 这是一个**瞬间快照**：返回 true 后对端可能立刻关闭。不要用它
    ///       替代对 async_* 返回值的检查（存在 race，这是 TCP 的固有问题）。
    /// @note 线程安全（内部为 atomic 读取）、noexcept。
    bool is_open() const noexcept;

    // -------------------------------------------------------------------------
    // 内部实现类型（仅供实现层使用，不属于公共 API）
    // -------------------------------------------------------------------------

    /// 内部实现类型的前向声明（完整定义位于实现文件中）。
    ///
    /// @warning **非公共 API**：调用方不得引用、构造或依赖本类型。它的名字
    ///          之所以出现在 public 段，仅仅是 C++ 语言层面的需要——private
    ///          嵌套类型的**名字**只有类成员与 `friend` 才能使用，而实现层的
    ///          工厂函数（`MakeConnObject`）与命名空间作用域内的辅助类型
    ///          （如 `ConnectOp`）需要写到 `std::shared_ptr<Conn::Impl>`
    ///          这个类型名。
    /// @note 把它放到 public 只暴露了"类型名字可被提及"，**并未**暴露实现：
    ///       完整定义仍在 `.cpp` 中，`m_impl` 成员与构造函数仍然是 private。
    /// @note 未来若内部结构变更，本前向声明保持稳定，不影响 ABI 契约。
    struct Impl;

private:
    /// 仅允许 Mechanism 创建。
    ///
    /// 构造函数是 private，配合 private 继承 ::netmech::noncopyable，确保调用方
    /// 只能通过 `std::shared_ptr<Conn>` 持有本对象（铁律 [1]）。
    ///
    /// @param[in] impl 由 Mechanism 内部构造并注入的实现状态。
    ///                 @c nullptr 表示未初始化的空壳（仅用于内部占位）。
    explicit Conn(std::shared_ptr<Impl> impl);

    // Mechanism 需要调用私有构造/初始化，声明为友元。
    friend class Mechanism;

    /// 指向内部实现状态的**不透明**句柄。
    ///
    /// Impl 的完整定义位于实现文件的匿名命名空间内，头文件只暴露
    /// `std::shared_ptr<Impl>`，因此：
    ///  - 调用方包含本头时**不会**拖入 Boost.Asio 细节（编译更快）；
    ///  - 内部状态布局可以自由演进而不影响 ABI 稳定性要求（本项目不保证 ABI）。
    std::shared_ptr<Impl> m_impl;
};

} // namespace netmech
