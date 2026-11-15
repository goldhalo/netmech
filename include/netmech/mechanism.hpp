// =============================================================================
//  netmech - 网络运行时
// =============================================================================
//  文件    : netmech/mechanism.hpp
//  用途    : 定义 Mechanism 类（线程池 + accept 循环 + connect 超时控制）及其
//            Config。
//  依赖    : 独立 Asio + 标准库。不含 <iostream>。
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
//  * 最后一个 shared_ptr 被释放时触发析构：等价于 close_all()（停监听、请求
//    关闭全部连接、清表），但**不**停止 io 线程 —— 运行时是进程级/注入的，
//    生命周期不归单个 Mechanism 管。析构**不会 join**，因此很快返回；
//    线程的回收由 `netmech::shutdown()`（默认运行时）或 `Runtime::shutdown()`
//    （注入运行时）负责。
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


#include "netmech/export.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// 本头**不引 asio / OpenSSL**：只暴露 netmech::Address / std::error_code /
// netmech::TlsConfig（后者也只是纯 C++ 值类型）。

#include "netmech/common.hpp"
#include "netmech/linkable.hpp"
#include "netmech/tls.hpp"
// ↑ 本头直接用 std::weak_ptr<Linkable> 作为参数类型。缺这个 include 时，
//   单独 #include <netmech/mechanism.hpp> 会报 "Linkable was not declared"
//   —— 只有按 netmech.hpp 的顺序（linkable 在前）引入时才碰巧编得过。
//   （原先这里重复 include 了两次 common.hpp，一并去掉。）

// SOMAXCONN 在不同平台上取值不同（Windows 0x7fffffff / Linux 128 / 部分嵌入式
// 平台未定义）。本模块**刻意不使用**平台值作为默认 backlog，见下方
// kDefaultBacklog 的说明。这里保留兜底定义，仅为避免极少数平台缺失该宏时的
// 编译问题（已不参与默认值计算）。
#ifndef SOMAXCONN
#    define SOMAXCONN 128
#endif

namespace netmech {

class Runtime;   ///< 可注入的 io 运行时（netmech/runtime.hpp）


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

// =============================================================================
//  监听 / 连接选项（v0.91）
// =============================================================================
//  为什么用结构体而不是继续加形参：
//    * 位置参数再往下加（backlog / user_ctx / tls / …）就没人记得住顺序了；
//    * `listen(addr, lb, {})` 这类写法在"uint32_t backlog"与"TlsConfig"之间
//      **会产生重载歧义**，结构体把这个问题从语言层面消掉；
//    * 以后加字段（ALPN、限流、证书热加载）不需要再动函数签名。

/// 一次 `listen()` 的可选参数。
struct ListenOptions {
    /// 已连接但尚未 accept 的队列长度上限。0 = 用 `kDefaultBacklog`。
    std::uint32_t         backlog = kDefaultBacklog;

    /// 透传给每个新连接的用户上下文（`Conn::user_context()` 可取回）。
    std::shared_ptr<void> user_ctx;

    /// 有值 ⇒ **该监听器接受 TLS 连接**（进程内 TLS 终结）。
    ///
    /// @note 服务端必须有 `cert_chain_file` + `private_key_file`，
    ///       否则 `listen()` 直接返回 `invalid_argument`。
    /// @note 同一个 `Mechanism` 上可以同时有明文监听器与 TLS 监听器。
    std::optional<TlsConfig> tls;
};

/// 一次 `connect()` 的可选参数。
struct ConnectOptions {
    /// 连接超时（毫秒），只覆盖 **TCP 建链**，不含 TLS 握手。
    /// 0 = 不设超时（依赖 SYN 重传，不建议）。
    std::uint32_t timeout_ms = 3000;

    /// 本地绑定地址；默认 0.0.0.0:0 = 由系统选择。
    Address       local{};

    /// 透传给该连接的用户上下文。
    std::shared_ptr<void> user_ctx;

    /// 有值 ⇒ **该连接使用 TLS**（客户端握手）。
    ///
    /// @note 必须给出 `pinned_sha256` 或 `verify_certificate = true`，
    ///       否则 `connect()` 直接返回 `invalid_argument`（见 tls.hpp 规则 [2]）。
    /// @note 握手超时由 `TlsConfig::handshake_timeout_ms` 单独控制：
    ///       建链成功后开始计时，与上面的 `timeout_ms` 无关。
    std::optional<TlsConfig> tls;
};

/// 网络运行时。整个进程内通常只需要一个实例。
///
/// @par 创建
///  - 只能通过 `Mechanism::create()` 获得 shared_ptr，不可直接构造/拷贝。
///
/// @par 线程安全性
///  - 全部 public 方法线程安全，可从任意线程调用。
///  - listen() 有"重复地址"检测：并发监听同一地址时以先到者成功、
///    后到者返回 `NetErrc::duplicate_endpoint` 为准。connect() **不去重**：
///    同一 remote 允许多个在途 connect（连接池/并发探测是常见业务需求）。
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
class NETMECH_API Mechanism : public std::enable_shared_from_this<Mechanism>, private ::netmech::noncopyable {
public:
    /// 运行时配置。所有字段都有嵌入式友好的默认值。
    ///
    /// @note ★ io 线程数**不在这里配置** —— 运行时是**进程级**的，
    ///       线程数由 `IoPool::initialize(threads)` 在进程启动时**一次性**决定，
    ///       之后**不可更改**（契约见 `netmech/common.hpp` 的 initialize/shutdown）。
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
        /// @note TLS 连接的 OpenSSL 每连接还有约 30–40 KiB 的 SSL/BIO 开销，
        ///       容量规划时**不要**只按本项估算。
        std::size_t socket_buf_size  = 256 * 1024;

        /// 同时进行中的 TLS 握手数上限（整个 Mechanism 所有监听器合计）。
        ///
        /// - `0` = 不限（不推荐：握手是 CPU 密集操作，且每个 io_context
        ///   只有 1 条线程，握手风暴会把同 context 上正常连接的读写一起拖住）；
        /// - 达到上限时，新连接**立即**被拒绝（服务端走
        ///   `on_accept_failed`，客户端走 `on_linkfail`），不排队 ——
        ///   排队只会把 CPU 挤得更死。
        ///
        /// @note 只对 TLS 生效；明文连接不占配额。
        /// @note 默认 64：按"4 条 io 线程 × 16"估的起点，按实测调整。
        std::size_t max_inflight_tls_handshakes = 64;

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

    /// 用**指定的运行时**创建实例（可注入；线程数由该 Runtime 决定）。
    ///
    /// @param[in] cfg     配置（见上）
    /// @param[in] runtime io 运行时；传 nullptr 等价于 `create(cfg)`（用默认运行时）
    /// @return 非空；失败语义同 `create(Config)`。
    ///
    /// @par 什么时候需要它
    ///  - 一个进程里要多套互不干扰的 io 线程池（媒体面/信令面隔离、测试）；
    ///  - 一个进程里要多套互不干扰的 io 线程池（媒体面/信令面隔离、测试）。
    ///
    /// @par 多 .so 进程（重要）
    ///  如果各 .so **各自静态链接** netmech，`IoPool::global()` 是每个模块
    ///  各一份的静态数据 —— 而且**不能**靠"把一个模块的 Runtime 注入给另一个
    ///  模块"来共用：asio 的线程局部状态/注册表同样每模块一份，混用同一个
    ///  io_context 会崩（实测 SIGSEGV in asio scheduler）。
    ///  正确做法是 netmech 构建成**共享库**（`-DNETMECH_LIBRARY_TYPE=SHARED`），
    ///  全进程一份代码，`default_runtime()` 天然只有一份。
    ///
    /// @note 本实例会持有该运行时直到析构，因此宿主即使先释放自己的
    ///       shared_ptr，Mechanism 仍然安全（线程池不会消失）。
    static std::shared_ptr<Mechanism> create(Config cfg, std::shared_ptr<Runtime> runtime);

    /// 析构：等价于 close_all()（停监听、请求关闭全部连接、清表），
    /// 但**不停止 / 不 join io 线程** —— 运行时是进程级/注入的，生命周期
    /// 不归单个 Mechanism 管（与本文件头"生命周期与线程池存活"一致）。
    ///
    /// @note **快速返回**，不等待 io 线程退出。线程的回收由
    ///       `netmech::shutdown()`（默认运行时）或 `Runtime::shutdown()`
    ///       （注入运行时）负责。
    /// @note 在途回调由"强自持"机制保证执行完毕（关闭流程 / on_close /
    ///       on_linkdown 不会丢），但那发生在 io 线程上、异步于析构。
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
    /// @param[in] backlog  已连接但尚未 accept 的队列长度上限。传 0 用本库默认值
    ///                    （`kDefaultBacklog = 128`；**刻意不用** SOMAXCONN，理由见本文件开头）。
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

    /// 带选项的 `listen()`（v0.91：用于启用进程内 TLS 终结）。
    ///
    /// @param[in] local    监听地址；语义同上一个重载。
    /// @param[in] linkable 订阅者；语义同上一个重载。
    /// @param[in] opts     选项（backlog / user_ctx / tls）。
    /// @return 受理成功返回空 error_code。
    ///         失败可能值在上一个重载的基础上增加：
    ///         * `NetErrc::invalid_argument` —— TLS 配置不合法（缺证书/私钥、
    ///           指纹格式错）；
    ///         * `NetErrc::tls_unsupported` —— 本构建未启用 TLS
    ///           （`-DNETMECH_TLS=OFF`），但传了 `opts.tls`；
    ///         * 证书/私钥文件打不开时原样返回底层错误码。
    ///
    /// @par TLS 的代价
    ///  - 每个 TLS 监听器持有**一份** `SSL_CTX`（证书与私钥只加载一次）；
    ///  - 握手在该连接绑定的 io 线程上同步完成（CPU 密集），受
    ///    `Config::max_inflight_tls_handshakes` 限制；
    ///  - 从 `SSL_CTX` 到 `Conn` 的引用关系保证证书配置在连接存活期内有效。
    std::error_code listen(const Address& local,
                           std::weak_ptr<Linkable> linkable,
                           const ListenOptions& opts);

    /// 停止**某一个**监听器。
    ///
    /// 作用范围刻意做小：
    ///  - 只停这一个 accept 循环，同一实例上的其它监听器不受影响；
    ///  - **不动已经建立的连接** —— 它们属于业务层，什么时候关由业务决定
    ///    （想一起关就在自己的会话表里逐个 close）。
    ///
    /// 加这个接口的原因：`close_all()` 是"整台机器断电"，而业务上更需要
    /// "关掉我这个 HTTP 服务"，两者不该共用一个开关。
    ///
    /// @param[in] local 当初 `listen()` 传入的地址；也可以传**实际绑定地址**
    ///                  （`listen(..:0)` 之后用 `listening_address()` 取到的那个）。
    /// @return 空 = 已停止（**幂等**：本来就没在监听也算成功）；
    ///         `NetErrc::invalid_argument` = 地址非法。
    ///
    /// @note 传端口 0 的入参时同样要求"唯一匹配"：同一 IP 上并存多个 `:0`
    ///       监听时不做任何动作（避免猜错对象）。
    ///
    /// @par 线程安全性
    ///  - 可从任意线程调用。外部线程调用时**有界等待**（至多 500ms）
    ///    accept 循环收敛，保住"返回后端口已释放"的语义（unlisten 后
    ///    立刻重 listen 同一端口不会撞 address_in_use）；在 io 线程上
    ///    调用时只投递不等待（不能等自己）。
    ///  - 与 `listen()` 并发调用同一地址时，两者之一生效，不会出现半个监听器。
    std::error_code unlisten(const Address& local);

    /// 查询某个监听的**实际**绑定地址。
    ///
    /// 用途：`listen(Address{"0.0.0.0", 0})` 由系统分配端口后，把真实端口读回来
    /// （日志、测试、服务发现都要它）。
    ///
    /// @param[in] requested 当初 `listen()` 传入的地址；
    ///                      也可以传实际绑定地址做反查。
    /// @return 找到返回实际地址（含系统分配的端口）；没有在监听返回 `std::nullopt`。
    ///
    /// @note **端口 0 的入参要求"唯一匹配"**：同一 IP 上并存两个 `:0` 监听时，
    ///       "问 :0 对应哪个端口"本身没有唯一答案，此时返回 `nullopt`。
    ///       需要逐个确认请用 `listening_addresses()`，或直接传实际地址。
    ///
    /// @note 线程安全。
    [[nodiscard]] std::optional<Address> listening_address(const Address& requested) const;

    /// 当前在监听的**全部**实际地址（诊断 / 测试用）。
    [[nodiscard]] std::vector<Address> listening_addresses() const;

    /// 本实例是否启用了 `Config::linger_zero`（关闭时发 RST、丢未发数据）。
    ///
    /// 用途：上层协议（典型是 HTTP）需要"写应答后再关闭"的语义，一旦
    /// `linger_zero == true`，最后那一包会被 RST 丢掉，且**没有任何错误信号**。
    /// 上层可以在启动时查一下这个值，选择拒绝启动或给出明确告警。
    ///
    /// @note 线程安全。
    [[nodiscard]] bool linger_zero() const noexcept;

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
    ///         * `NetErrc::not_initialized`  运行时尚未初始化或已 shutdown；
    ///         * `NetErrc::invalid_argument` 地址非法（通配地址 / 端口 0）；
    ///         * `NetErrc::link_closed`      受理期间被 close_all() 放弃；
    ///         * 其余为底层 open/bind 错误。
    ///
    /// @note **不去重**：同一 remote 允许多个在途 connect，每笔独立对应
    ///       一次 on_linkup/on_linkfail（连接池、并发探测、压力测试是常见
    ///       需求）。`duplicate_endpoint` 仅用于 listen()。
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

    /// 带选项的 `connect()`（v0.91：用于客户端 TLS）。
    ///
    /// @param[in] remote   目标地址（IP 字面量；本库不做 DNS 解析）。
    /// @param[in] linkable 订阅者；语义同上一个重载。
    /// @param[in] opts     选项（timeout_ms / local / user_ctx / tls）。
    /// @return 受理成功返回空 error_code。失败可能值增加：
    ///         * `NetErrc::invalid_argument` —— TLS 配置不合法（既没 pin
    ///           也没开链校验、指纹格式错）；
    ///         * `NetErrc::tls_unsupported` —— 本构建未启用 TLS。
    ///
    /// @par TLS 握手失败怎么通知
    ///  TCP 建链成功后先做握手；握手失败（证书被拒 / 超时 / 协议不符）走
    ///  `Linkable::on_linkfail(remote, ec)`，**不会**有 `on_linkup`，
    ///  也**不会**有 `on_linkdown` —— 这条连接从未交给业务。
    ///  可用 `from_boost_error_code(ec)` 归类（`tls_handshake_failed` /
    ///  `tls_handshake_timeout` / `tls_cert_rejected` …）。
    std::error_code connect(const Address& remote,
                            std::weak_ptr<Linkable> linkable,
                            const ConnectOptions& opts);

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

    /// listen 的实际实现（两个公开重载都走这里）。
    ///
    /// @param[in] tls 为空 = 明文监听器。
    std::error_code listen_impl(const Address& local,
                                std::weak_ptr<Linkable> linkable,
                                std::uint32_t backlog,
                                std::shared_ptr<void> user_ctx,
                                const std::optional<TlsConfig>& tls);

    /// connect 的实际实现（两个公开重载都走这里）。
    std::error_code connect_impl(const Address& remote,
                                 std::weak_ptr<Linkable> linkable,
                                 std::uint32_t timeout_ms,
                                 const Address& local,
                                 std::shared_ptr<void> user_ctx,
                                 const std::optional<TlsConfig>& tls);

    /// 指向内部实现状态的句柄（private，外部不可访问）。
    std::shared_ptr<Impl> m_impl;
};

} // namespace netmech
