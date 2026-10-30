// =============================================================================
//  NetMechanism - 网络运行时
// =============================================================================
//  文件    : netmech/mechanism.hpp
//  用途    : 定义 Mechanism 类（线程池 + accept 循环 + connect 超时控制）及其
//            Config。
//  依赖    : Boost 1.74 + 标准库。不含 <iostream>。
//  编码    : UTF-8
//
//  ★ 与旧 CAsioRuntime 的关键差异
//  -----------------------------
//  * 不设固定 4 个 accept 线程：accept 与 Conn 在**同一个** io_context 池
//    上运行（旧实现为 accept 单独开线程，空闲时浪费栈）。
//  * 线程池大小 = 进程级的 `IoPool::initialize(threads)`（一次性决定；**0 归一化为
//    max(1, hardware_concurrency())**，**不乘 4**）；
//    不再乘 4（旧实现乘 4 导致嵌入式板子上线程爆炸）。该归一化从根上消除了
//    旧 `setParallelNum(0)` 的除零崩溃。
//  * 单例式全局 runtime 被替换为 `create()` 返回的 shared_ptr：所有权明确，
//    析构即完整关闭（旧 `fini` 恒返回 false 的缺陷被消除）。
//  * `connect()` 真正实现 `timeout_ms`（steady_timer + cancel），而不是像旧
//    `relink` 那样忽略 `to` 参数。
//
//  ★ 生命周期与线程池存活
//  ---------------------
//  * `create()` 是整个库唯一的入口，返回 `std::shared_ptr<Mechanism>`。
//  * 最后一个 shared_ptr 被释放时触发析构 => 等价于 close_all() + join 所有
//    io 线程。因此**析构会阻塞**直到所有 io 线程退出，请勿在 io 回调里释放
//    最后一个引用（会造成自我 join，实现会检测并规避，但仍是坏味道）。
//  * `close_all()` **只断链、不销毁线程池**。调用后 Mechanism 仍然可用，
//    可以继续 listen()/connect()。这正是 `running()` 为 true 的原因。
//    （旧实现 `fini` 之后对象即半死状态，此处刻意改为支持断线重连场景。）
//
//  ★ 订阅者（Linkable）的三层来源，优先级从高到低
//  ---------------------------------------------
//  1. listen()/connect() 调用时**显式传入**的 weak_ptr（单次有效）；
//  2. set_linkable() 设置的全局订阅者（对之后所有未显式传入的调用生效）；
//  3. 都没有 => listen/connect 仍受理，但建立连接后会因"无订阅者"被立即
//     关闭（见 listen()/connect() 的说明）。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <asio/ip/tcp.hpp>

#include "netmech/common.hpp"
#include "netmech/common.hpp"

// SOMAXCONN 在不同平台上取值不同（Windows 0x7fffffff / Linux 128 / 部分嵌入式
// 平台未定义）。本模块**刻意不使用**平台值作为默认 backlog，见下方
// kDefaultBacklog 的说明。这里保留兜底定义，仅为避免极少数平台缺失该宏时的
// 编译问题（已不参与默认值计算）。
#ifndef SOMAXCONN
#    define SOMAXCONN 128
#endif

namespace netmech {

/// listen() 的默认 backlog：**固定 128**，与旧 AsioMechanism 的默认值一致。
///
/// 为什么不取平台 SOMAXCONN：
///  - 平台值差异极大（Windows `0x7fffffff` vs Linux 128），会让同一份调用代码在
///    不同平台上产生**可观测的行为差异**；
///  - 嵌入式板卡的内核 backlog 实现各异，传入超大值可能被静默截断或引发意外；
///  - 固定 128 行为确定、无惊喜，且与旧模块默认值一致，便于迁移对照。
///
/// 需要更大 backlog 时请显式传入 listen() 的第三个参数。
/// 用 inline constexpr 而非宏，避免污染调用方命名空间。
inline constexpr std::uint32_t kDefaultBacklog = 128;

/// 网络运行时。整个进程内通常只需要一个实例。
///
/// @par 创建
///  - 只能通过 `Mechanism::create()` 获得 shared_ptr，不可直接构造/拷贝。
///
/// @par 线程安全性
///  - 全部 public 方法线程安全，可从任意线程调用。
///  - 但 listen()/connect() 有"重复地址"检测，并发调用同一地址时以先到者
///    成功、后到者返回 `NetErrc::duplicate_endpoint` 为准。
///
/// @par 使用示例
/// @code
///   auto mech = netmech::Mechanism::create();          // 默认 Config
///   mech->set_linkable(my_linkable);                     // 全局订阅者
///   auto ec = mech->listen({tcp::v4(), 9000});           // 0.0.0.0:9000
///   if (ec) { /* 处理错误 */ }
///   ...
///   mech->close_all();                                   // 断链，但仍可复用
/// @endcode
class Mechanism : public std::enable_shared_from_this<Mechanism>, private ::netmech::noncopyable {
public:
    /// 运行时配置。所有字段都有嵌入式友好的默认值。
    ///
    /// @note ★ io 线程数**不在这里配置** —— 运行时是**进程级**的，
    ///       线程数由 `IoPool::initialize(threads)` 在进程启动时**一次性**决定，
    ///       之后**不可更改**（详见 `executor.hpp` 的契约）。
    struct Config {
        /// 单连接发送队列的**字节总量**上限（反压阈值），默认 2 MiB。
        /// 超限时 async_write 返回 false（NetErrc::buffer_full），不断链。
        std::size_t send_queue_bytes = 2 * 1024 * 1024;

        /// 单连接发送队列的**请求个数**上限，默认 256。
        /// 与 send_queue_bytes 是与关系：任一超限即拒绝。
        std::size_t send_queue_count = 256;

        /// 是否为新建的 socket 设置 TCP_NODELAY（禁用 Nagle）。
        /// 小包低延迟场景（实时控制、RPC）建议 true。
        bool        tcp_nodelay      = true;

        /// 关闭 socket 时是否使用 SO_LINGER{1, 0}（立即发 RST，**丢弃未发数据**）。
        ///
        /// - `false`（默认）= 走正常四次挥手，保证已入队的数据送达对端。
        /// - `true` = 立即 RST 并释放端口，避免 TIME_WAIT 堆积；
        ///   适合"服务端高频短连接"这类不关心尾部数据的场景。
        ///
        /// @warning 默认值曾经是 `true`。改成 `false` 的原因：`close()` 时
        ///          发送队列里可能还有没进内核的数据（尤其是"写应答后立即关闭"
        ///          的协议），RST 会直接把它们丢掉。对 Modbus 这类请求-应答协议
        ///          是静默丢包，很难排查。
        /// @note 在主动 close() 与析构路径上生效。
        bool        linger_zero      = false;

        /// socket 发送/接收缓冲区大小（SO_SNDBUF / SO_RCVBUF），默认 256 KiB。
        /// 设为 0 表示不设置（使用系统默认）。
        /// @note 嵌入式设备内存紧张时可调小；高吞吐场景可调大。
        std::size_t socket_buf_size  = 256 * 1024;

    };

    /// 创建运行时实例。
    ///
    /// @param[in] cfg 配置。默认构造为嵌入式友好配置。
    /// @return 非空的 shared_ptr<Mechanism>；io 线程池在返回前已启动，
    ///         因此返回后 `running()` 立即为 true。
    /// @note 即使 @p cfg 非法（例如硬件并发数读取失败），本函数也保证返回
    ///       可用对象：io 线程数在 `IoPool::initialize()` 里安全归一化，
    ///       不会抛异常、不会除零。
    /// @note 线程安全：可从任意线程调用；每个实例互相独立。
    ///        通常进程内只应创建一个（便于统一管理线程池）。
    /// @note 失败语义：仅当内存耗尽/线程创建失败时抛出 `std::system_error`
    ///       或 `std::bad_alloc`（标准库行为，无法用 error_code 表达）。
    /// 用默认配置创建运行时。
    static std::shared_ptr<Mechanism> create();

    /// 用指定配置创建运行时。
    ///
    /// @note 这里刻意**不用** `create(Config cfg = Config{})`：
    ///       `Config` 是嵌套类型，它的默认成员初始化器在外部类定义完成前
    ///       不可用，GCC 会直接报错（MSVC 放过了，属于实现宽容度差异）。
    ///       拆成两个重载在两种编译器上都合法。
    static std::shared_ptr<Mechanism> create(Config cfg);

    /// 析构：等价于 close_all() 后再 join 全部 io 线程。
    ///
    /// @warning **会阻塞**，直到所有 io 线程退出。不要在 io 回调内部释放最后
    ///          一个 shared_ptr<Mechanism>（实现会检测到并避免自我 join，
    ///          但仍建议把释放动作放到业务线程）。
    /// @note 任何仍在途的 async_* 回调都会以 operation_aborted 被调用完毕
    ///       之后线程才退出。
    ~Mechanism();

    // -------------------------------------------------------------------------
    // 服务端
    // -------------------------------------------------------------------------

    /// 开始监听本地地址。
    ///
    /// @param[in] local    监听地址。端口 0 表示由系统分配（此时可用
    ///                     `local_address_of(local)` 或监听后续连接里的
    ///                     `Conn::local_address()` 取到实际端口）。
    /// @param[in] linkable 为该监听器服务的订阅者（weak_ptr）。为空
    ///                     （已过期或未设置）时，退回 set_linkable() 设置的
    ///                     全局订阅者；仍为空则新连接会被立即关闭。
    /// @param[in] backlog  已连接但尚未 accept 的队列长度上限，默认 SOMAXCONN。
    /// @return 受理成功返回 `std::error_code{}`（布尔判**假**）。
    ///         失败返回 `make_boost_error_code(...)`，可能值：
    ///         * `NetErrc::duplicate_endpoint` 同一 ip:port 已在本实例监听；
    ///         * `NetErrc::invalid_argument`   地址非法（如端口越界）；
    ///         * `NetErrc::not_initialized`    实例已关闭且无法重建；
    ///         * 其余为底层 bind/listen 的系统错误（原样透传）。
    ///
    /// @par 失败语义
    ///  - 返回非零错误码表示**未受理**，不会产生任何回调。
    ///
    /// @par 线程安全性
    ///  - 可从任意线程调用；并发对同一地址调用时恰好一个成功。
    ///
    /// @par accept 循环的健壮性
    ///  - accept 遇到 `operation_aborted`（主动关闭）以外的一般错误时，
    ///    会**退避后重新 accept**，而不是像旧实现那样永久停止监听。
    std::error_code listen(const Address& local,
                                     std::weak_ptr<Linkable> linkable,
                                     std::uint32_t backlog = kDefaultBacklog,
                                     std::shared_ptr<void> user_ctx = nullptr);

    // -------------------------------------------------------------------------
    // 客户端
    // -------------------------------------------------------------------------

    /// 异步发起连接（本函数**受理后立即返回**，结果通过 Linkable 回调通知）。
    ///
    /// @param[in] remote     目标地址。
    /// @param[in] linkable   该连接对应的订阅者；为空时退回全局订阅者。
    ///                       连接成功后调用 `on_linkup(endpoint, out)`；
    ///                       失败（含超时）调用 `on_linkfail(remote, ec)`。
    /// @param[in] timeout_ms 连接超时（毫秒），默认 3000。0 表示不设超时
    ///                       （依赖系统 SYN 重传超时，不建议）。
    /// @param[in] local      本地绑定地址；默认（0.0.0.0:0）表示由系统选择。
    ///                       需要指定出口网卡/固定源端口时使用。
    /// @return 受理成功返回 `std::error_code{}`。
    ///         失败返回 `make_boost_error_code(...)`，可能值：
    ///         * `NetErrc::not_connected`   运行时已关闭，无法发起；
    ///         * `NetErrc::invalid_argument` 地址非法；
    ///         * `NetErrc::duplicate_endpoint` 同一 remote 已有未完成的 connect
    ///           （避免重复建链，保证一次 connect 对应一次 on_linkup/on_linkfail）；
    ///         * 其余为底层 resolve/bind 错误。
    ///
    /// @par 失败语义
    ///  - 返回非零表示**未受理**：**不会**调用 on_linkup，也**不会**调用
    ///    on_linkfail。调用方须自行处理该错误。
    ///
    /// @par 线程安全性
    ///  - 可从任意线程调用；连接过程全部在 io 池上异步进行。
    ///  - 回调在发起 connect 的 io 线程上执行，可能与其它连接的回调并发。
    ///
    /// @par 超时实现
    ///  - 内部用 `steady_timer(timeout_ms)`，超时后 cancel 挂起的 connect，
    ///    并以 `asio::error::timed_out` 调 on_linkfail。
    ///  - 这是对旧 `relink` 忽略 `to` 参数缺陷的修复。
    std::error_code connect(const Address& remote,
                                      std::weak_ptr<Linkable> linkable,
                                      std::uint32_t timeout_ms = 3000,
                                      const Address& local = Address{},
                                      std::shared_ptr<void> user_ctx = nullptr);

    // -------------------------------------------------------------------------
    // 按连接 ID 操作（业务侧无需保存 Conn*）
    // -------------------------------------------------------------------------

    /// 向指定连接发一笔写。语义与 `Conn::write` 完全一致（FIFO / 反压 / owner 规则）。
    ///
    /// @param[in] conn_id `Conn::id()` 返回的连接编号。
    /// @return false = 未受理：连接不存在（已断开）/ 已关闭 / 队列满 / 参数非法。
    ///         与 `Conn::write` 的 false 语义相同，**都不会断链**。
    ///
    /// @par 为什么需要它
    ///  业务侧常常只有"设备表号"，没有 Conn 对象。用本接口可以避免自己维护
    ///  `id → Conn*` 表、以及在 on_linkdown 里摘除时的竞态窗口。
    ///
    /// @par 顺序保证
    ///  与 `Conn::write` 共用同一条发送队列与同一把锁：受理顺序 = 锁获取顺序，
    ///  对所有线程确定；两条入口可以混用，一并排序。
    ///
    /// @note 实现是一次哈希查找，相对于一次系统调用可忽略。
    /// @note 跨线程安全。
    bool write(std::uint64_t conn_id, ConstBuf buf);

    /// 关闭指定连接。等价于找到该连接后调用 `Conn::close()`。
    /// @return false = 该 id 不存在（已经关闭或从未存在）。
    bool close(std::uint64_t conn_id) noexcept;

    // -------------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------------

    /// 关闭**全部**监听器与已建立的连接。
    ///
    /// 语义：
    ///  1. 停止所有 accept 循环（挂起的 accept 以 operation_aborted 结束）；
    ///  2. 对每个 Conn 调用 close()，其在途回调以 operation_aborted 触发
    ///     完毕，随后投递 on_linkdown()；
    ///  3. **取消**所有未完成的 connect（不调 on_linkfail，因为属于主动放弃）。
    ///
    /// @par 前置条件 / 后继行为
    ///  - 无前置条件，可在任意状态调用（包括从未 listen 过）。
    ///  - **线程池保持存活**：关闭后仍可再次调用 listen()/connect()，且
    ///    `running()` 继续返回 true。这支持"断线后整体重连"的场景。
    ///
    /// @par 线程安全性
    ///  - noexcept、线程安全；可从任意线程或任意回调内部调用。
    ///  - 本函数**不阻塞**等待关闭完成（异步收敛）；若需要等待，请在
    ///    on_linkdown 中计数。
    void close_all() noexcept;

    /// 运行时是否处于可工作状态。
    ///
    /// @return 当且仅当实例未被销毁、io 线程池仍在运行时返回 true。
    /// @note `close_all()` **不会**使本函数变为 false（线程池仍存活、仍可
    ///      再次 listen/connect）。只有析构才会结束运行。
    /// @note 线程安全（内部为 atomic 读取）、noexcept。
    [[nodiscard]] bool running() const noexcept;

    /// 设置全局订阅者（全局单一订阅者）。
    ///
    /// @param[in] linkable 订阅者弱引用。对之后所有**未显式传入** linkable 的
    ///                     listen()/connect() 生效。
    /// @note 覆盖式设置：后一次调用替换前一次。
    /// @note 传入空的 weak_ptr 表示"清除全局订阅者"。
    /// @note 不影响已经建立并已交给具体订阅者的连接。
    /// @note 线程安全。
    void set_linkable(std::weak_ptr<Linkable> linkable);

    /// 当前活跃连接数。
    ///
    /// @return 已建立且尚未关闭的 Conn 数量（快照值）。
    /// @note 已 accept 但尚未进入 on_accept 回调的连接**也**计入（它们是已
    ///       建立连接）。监听器本身**不**计入。
    /// @note 线程安全（内部为 atomic 读取）、noexcept。
    [[nodiscard]] std::size_t endpoint_count() const noexcept;

    // -------------------------------------------------------------------------
    // 辅助查询（非 §5 契约，供测试与业务诊断使用）
    // -------------------------------------------------------------------------

    /// 实际使用的 io 线程数（进程级，**对所有 Mechanism 相同**）。
    ///
    /// @return >= 1 的线程数。
    /// @note 用于验证配置归一化，便于单元测试断言。
    /// @note 线程安全、noexcept。
    [[nodiscard]] std::size_t io_threads() const noexcept;

    /// 查询某个本地地址在本实例中实际的绑定地址（解决端口 0 的问题）。
    ///
    /// 典型用法：`listen({ {tcp::v4()}, 0 })` 成功后，用原 key 查回系统分配
    /// 的实际端口。
    ///
    /// @param[in] local 调用 listen() 时传入的地址（作为查询键）。
    /// @return 实际绑定地址；未监听该地址时返回 `std::nullopt`。
    /// @note 端口 0 与端口 N 视为不同键：用 0 调用 listen 后，必须用 0 来查询。
    /// @note 线程安全。
    [[nodiscard]] std::optional<Address>
    local_address_of(const Address& local) const;

    // -------------------------------------------------------------------------
    // 内部实现类型（仅供实现层使用，不属于公共 API）
    // -------------------------------------------------------------------------

    // 注意：Impl 在此公开仅为让实现层自由函数/结构能在类型位置命名它。
    // 完整定义在 .cpp 中，外部无法构造、无法 sizeof、无法访问成员——标准 opaque handle 用法。
    //
    // 背景（避免后人误以为是封装疏漏）：
    //  - `Impl` 若留在 private 段，则其**名字**只有 Mechanism 的成员和 friend 才能
    //    使用；而 mechanism.cpp 中处于命名空间作用域的辅助结构（如 `ConnectOp`）
    //    必须在**类型声明位置**写出 `std::weak_ptr<Mechanism::Impl>`，自由函数
    //    `erase_pending_connect(...)` 也需要完整类型来解引用 `impl->pending_connects`
    //    等成员。friend + 运行时取值函数对此无能为力（那只能解决"取到值"，
    //    解决不了"在类型位置命名"）。
    //  - 公开前向声明只让"类型名字可被提及"，**未**暴露实现：`m_impl` 成员与
    //    构造函数仍然 private，且 Impl 是不完整类型（无法 sizeof / make_shared /
    //    访问成员）。这与 Conn::Impl 的处理方式一致。
    struct Impl;

private:
    /// 仅允许 create() 构造。
    /// @param[in] impl 内部状态，非空。
    explicit Mechanism(std::shared_ptr<Impl> impl);

    /// 指向内部实现状态的句柄（private，外部不可访问）。
    std::shared_ptr<Impl> m_impl;
};

} // namespace netmech
