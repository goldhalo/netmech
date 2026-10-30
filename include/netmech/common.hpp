// =============================================================================
//  NetMechanism - 公共基础类型
// =============================================================================
//  文件    : netmech/common.hpp
//  用途    : 定义 TCP / UDP 共用的类型：进程级初始化、地址、缓冲区视图、错误码。
//            业务代码一般只 #include <netmech/netmech.hpp>，不必直接包含本头。
//  依赖    : C++17 标准库 + <asio/error.hpp>（仅错误码常量，header-only，
//            不引入 asio 的 socket/io_context，因此不会拖入网络栈）。
//  编码    : UTF-8
// =============================================================================
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/error.hpp>

namespace netmech {

// =============================================================================
//  工具基类
// =============================================================================

/// 禁止拷贝的基类。私有/保护继承即可让派生类不可拷贝。
///
/// 用 `= delete` 表达（C++11 起），不依赖任何第三方库。
class noncopyable {
protected:
    noncopyable() = default;
    ~noncopyable() = default;

public:
    noncopyable(const noncopyable&) = delete;
    noncopyable& operator=(const noncopyable&) = delete;
};

// =============================================================================
//  进程级初始化
// =============================================================================

/// 初始化进程级 io 运行时（线程池 + io_context）。
///
/// **必须在任何网络对象创建之前调用**，且一个进程只调用一次。调用后线程数
/// 固定，运行中不可更改——因为每个 io_context 恰好由 1 条线程 run()，
/// "1 context = 1 线程"是"同一连接的回调天然串行"这条契约的地基。
///
/// @param[in] io_threads 期望的 io 线程数。0 表示 max(1, hardware_concurrency())。
/// @return true  本次调用决定了线程数（首次调用）。
/// @return false 已初始化过且请求值不一致（同值视为幂等成功）。
/// @note 线程安全。
[[nodiscard]] bool initialize(std::size_t io_threads) noexcept;

/// 停止 io 运行时并回收所有 io 线程。幂等。
///
/// 调用后：所有连接与端点一并关闭（在途读写以取消类错误归还）、线程 join，
/// 之后新建对象一律失败。**不支持重新初始化**——避免出现"线程数改了，
/// 但先前创建的 io_context 仍在运行"的半死状态。
///
/// @warning 会阻塞直到所有 io 线程退出。不要在 io 回调里调用。
void shutdown() noexcept;

// =============================================================================
//  地址
// =============================================================================

namespace detail {
struct AddressCodec;   ///< 内部实现：Address ⇄ asio 地址的转换（仅 src/ 可见）
} // namespace detail

/// 一个 IP 端点（地址 + 端口）。不依赖 asio，公共头不传递任何网络库细节。
///
/// @par 构造
///  - 默认构造 = 0.0.0.0:0（通配：listen 让系统选端口，connect 让系统选源端口）。
///  - 支持 IPv4 / IPv6 字面量（`"1.2.3.4"`、`"::1"`，端口单列）。
///  - **不做域名解析**：面向设备直连，地址来自配置，基本都是 IP 字面量。
class Address {
public:
    Address() noexcept;

    /// 由 ip 字面量与端口构造。ip 非法时得到 0.0.0.0:0，不抛异常。
    Address(std::string_view ip, std::uint16_t port) noexcept;

    /// 解析 "1.2.3.4:502" / "[::1]:9000" / ":9000"（等价 0.0.0.0:9000）。
    /// @return 解析失败返回 std::nullopt。
    [[nodiscard]] static std::optional<Address> parse(std::string_view text) noexcept;

    /// 通配地址（0.0.0.0，指定端口）。
    [[nodiscard]] static Address any(std::uint16_t port) noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }
    [[nodiscard]] bool          is_v4() const noexcept { return m_family == 0; }
    [[nodiscard]] bool          is_unspecified() const noexcept;
    [[nodiscard]] std::string   to_string() const;   ///< "1.2.3.4:502" / "[::1]:502"
    /// 只返回 IP 部分（不带端口、IPv6 不带方括号）。
    /// 用于需要把 IP 与端口分开使用的场景（如重新构造带不同端口的地址）。
    [[nodiscard]] std::string   address_string() const;

    friend bool operator==(const Address& a, const Address& b) noexcept
    {
        return a.m_family == b.m_family && a.m_port == b.m_port
            && a.m_hi == b.m_hi && a.m_lo == b.m_lo;
    }
    friend bool operator!=(const Address& a, const Address& b) noexcept { return !(a == b); }

private:
    friend struct detail::AddressCodec;

    // 地址按 128 位存放：IPv4 只用 m_lo 的低 32 位，IPv6 用满 m_hi/m_lo。
    std::uint64_t m_hi     = 0;
    std::uint64_t m_lo     = 0;
    std::uint16_t m_port   = 0;
    std::uint8_t  m_family = 0;   ///< 0 = IPv4, 1 = IPv6
};

// =============================================================================
//  缓冲区
// =============================================================================

/// 读缓冲视图。
///
/// @par owner 的语义（契约，库无法校验）
///  **owner 必须保证 data 在其存活期间有效**。库只负责把 owner 一起带在身上
///  直到完成回调，它不检查 data 是否真的落在 owner 管理的内存里。
///
///  三种合法用法：
///  - 一对一：owner 就是那块内存的所有者（最常见）。
///  - 一个 owner 多个 data：合法，但**最后一份引用释放时才回收**（延迟释放，
///    不是泄漏）。要求每个 data 独立释放，就得每个 data 一个独立 owner。
///  - 别名指针（`std::shared_ptr<X> alias(parent, ptr)`）：共享控制块，data 独立。
///
///  @warning **不要拿长期存活的对象当 owner**（例如 `shared_from_this()`）。
///           它会把整个对象钉在一条在途操作上，容易形成引用环；多个 Buf 共用
///           同一控制块时，引用计数还会挤在同一条 cache line 上争用。
///           Buf 只承载"数据 + 谁保证它活着"，业务上下文请用 user_context。
///
/// @par 性能
///  owner 非空时每笔操作为 1 次原子 inc + 1 次 dec（约 20–40 ns 合计），
///  owner 为空则完全不碰原子量。
struct Buf {
    std::shared_ptr<void> owner;   ///< 可空，语义见上
    void*                 data = nullptr;
    std::size_t           size = 0;
};

/// 写缓冲视图。提交后 data 指向的内容**不得修改**，直到完成回调返回。
struct ConstBuf {
    std::shared_ptr<void> owner;   ///< 可空，语义同 Buf::owner
    const void*           data = nullptr;
    std::size_t           size = 0;
};



// -----------------------------------------------------------------------------
// NetErrc 枚举
// -----------------------------------------------------------------------------

/// 网络模块的对外错误码。
///
/// 语义说明：
///  * @c ok                 成功。底层值为 0；`make_boost_error_code(ok)` 得到的
///                         错误码在布尔上下文中判 **假**（即"无错误"）。
///  * @c not_initialized    运行时尚未初始化或已销毁，操作无法进行。
///  * @c already_initialized 重复初始化。
///  * @c invalid_argument   参数非法，例如空 buffer（size()==0）、端口越界、
///                         保活参数自相矛盾（interval 为 0 等）。
///  * @c duplicate_endpoint 重复监听同一本地地址（同一 ip:port 已被占用为 listen）。
///  * @c buffer_full        发送队列达到上限（反压）。**仅表示本次写未被受理，库不会
///                         因此断开连接**；调用方应稍后重试或自行做流控。
///  * @c not_connected      连接未建立 / 已关闭，异步操作无法发起。
///  * @c link_closed        连接已关闭（对端关闭、本端 close()、或 IO 错误导致关闭）。
///  * @c accept_failed      accept 环节失败（例如达到 fd 上限、监听 socket 失效）。
///  * @c keepalive_timeout  保活超时：接收侧或发送侧在约定时间内未收到/未发出数据。
enum class NetErrc {
    ok = 0,
    not_initialized,
    already_initialized,
    invalid_argument,     ///< setParallelNum(0)、空 buffer 等
    duplicate_endpoint,   ///< 重复 listen 同一地址
    buffer_full,          ///< 发送队列拒绝（反压）
    not_connected,
    link_closed,
    accept_failed,
    keepalive_timeout,     ///< 接收 / 发送保活超时
    handler_rejected,      ///< handler 工厂返回空，或 on_open 返回 false
    cancelled,             ///< 被显式取消
    datagram_truncated,    ///< UDP 报文超过接收缓冲，已被截断（仅该报文受影响）
};

// -----------------------------------------------------------------------------
// 类别与消息
// -----------------------------------------------------------------------------

/// 返回 NetErrc 对应的错误类别单例（进程内唯一）。
///
/// @return NetErrc 类别对象的常量引用；生命周期覆盖整个进程，永不失效。
/// @note noexcept，可在全局析构期安全调用。
/// @note 线程安全：可被任意线程并发调用。
const std::error_category& net_category() noexcept;

/// 本模块在 boost 侧使用的错误码值基址。
///
/// 把 `NetErrc` 的整数值映射到 `[kNetErrcBase, kNetErrcBase + N)` 区间，避免
/// 与 boost/system、POSIX errno 的取值重叠。仅供 make_boost_error_code() /
/// from_boost_error_code() 与实现内部使用，调用方不必关心。
inline constexpr int kNetErrcBase = 0x4E4D0000; // 'NM' 前缀，便于日志辨识

/// 返回 NetErrc 在 boost 侧对应的 error_category 单例。
///
/// @return 内部 boost 类别对象的常量引用；与 `net_category()` 语义一一对应，
///         `message()` 文本相同。
/// @note 线程安全、noexcept。主要用于 make_boost_error_code() 的实现。
const std::error_category& net_boost_category() noexcept;

/// 返回 NetErrc 的人类可读描述。
///
/// @param[in] ev 错误码的整数值（即 `static_cast<int>(NetErrc::xxx)`）。
/// @return 非空的描述字符串；未知值返回 "unknown net error"。
/// @note 线程安全、noexcept。
std::string net_errc_message(int ev) noexcept;

// -----------------------------------------------------------------------------
// 构造 std::error_code
// -----------------------------------------------------------------------------

/// 把 NetErrc 转成 std::error_code。
///
/// @param[in] e 错误码枚举值。
/// @return 携带 `net_category()` 与 `e` 的 std::error_code。
/// @note `make_error_code(NetErrc::ok)` 得到的对象判 **假**（无错误）。
/// @warning 返回值**不能**直接用作 `Mechanism::listen()` / `connect()` 的返回
///          值（那里要求 `std::error_code`），请改用
///          make_boost_error_code()。
/// @note 线程安全、noexcept。
inline std::error_code make_error_code(NetErrc e) noexcept
{
    return std::error_code(static_cast<int>(e), net_category());
}

// -----------------------------------------------------------------------------
// 构造 std::error_code
// -----------------------------------------------------------------------------

/// 把 NetErrc 转成 std::error_code。
///
/// 这是 std 侧语义与库内部 Asio 原生错误码之间的**唯一**桥接点。
///
/// 实现方式：返回 boost 错误码，其 `value()` 为 `static_cast<int>(e)` 加固定偏移
/// `kNetErrcBase`（避免与 boost/system 及系统 errno 的取值区间撞车），
/// 类别为内部自定义的 boost error_category，其 `message()` 直接复用
/// net_errc_message()，因此 `ec.message()` 与 std 侧完全一致、可读。
///
/// @param[in] e 错误码枚举值。
/// @return `e == NetErrc::ok` 时返回**值初始化**的 boost 错误码（布尔值判假）；
///         否则返回携带 NetErrc 语义的 boost 错误码（布尔值判真）。
/// @note 线程安全、noexcept。可在回调内部（可能位于 io 线程上）安全调用。
/// @note 典型用法：
/// @code
///   return netmech::make_boost_error_code(NetErrc::buffer_full);
///   if (!ec) { /* 成功 */ }
/// @endcode
inline std::error_code make_boost_error_code(NetErrc e) noexcept
{
    // 历史名字保留（调用点很多），但**统一走 net_category**：
    // 这样调用方写 `ec == NetErrc::xxx` 能正确比较。
    // 早期版本这里用独立的 boost 类别并加了 kNetErrcBase 偏移，
    // 导致"库返回的 ec"与"用户用枚举比较的 ec"属于不同类别、永远不相等。
    return make_error_code(e);
}

/// 把 boost 错误码反查为 NetErrc。
///
/// 用于在错误回调里把底层错误归类成模块错误（例如把 `asio::error::eof`、
/// `connection_reset` 归为 `link_closed`）。
///
/// @param[in] ec boost 错误码。
/// @return 命中的 NetErrc；`ec` 为假（无错误）或无法归类时返回 `std::nullopt`。
///         **注意**：`ec` 为假时**不返回** `NetErrc::ok`，以免混淆"无错误"与
///         "未知错误"两种情形。
/// @note 线程安全、noexcept。
inline std::optional<NetErrc> from_boost_error_code(
    const std::error_code& ec) noexcept
{
    if (!ec) {
        return std::nullopt;
    }

    // 1) 本库自己产生的 NetErrc 错误码：先认 net_category（当前唯一来源），
    //    再兼容历史的 boost 类别（带 kNetErrcBase 偏移）。
    if (ec.category() == net_category()) {
        const int ev = ec.value();
        if (ev >= static_cast<int>(NetErrc::ok)
            && ev <= static_cast<int>(NetErrc::datagram_truncated)) {
            return static_cast<NetErrc>(ev);
        }
        return std::nullopt;
    }
    if (ec.category() == net_boost_category()) {
        const int ev = ec.value() - kNetErrcBase;
        if (ev >= static_cast<int>(NetErrc::ok)
            && ev <= static_cast<int>(NetErrc::datagram_truncated)) {
            return static_cast<NetErrc>(ev);
        }
        return std::nullopt;
    }

    // 2) 把常见的 Asio/系统连接类错误归一到模块语义，便于上层统一处理。
    if (ec == asio::error::eof
        || ec == asio::error::connection_reset
        || ec == asio::error::connection_aborted
        || ec == asio::error::broken_pipe
        || ec == asio::error::operation_aborted) {
        return NetErrc::link_closed;
    }
    if (ec == asio::error::not_connected) {
        return NetErrc::not_connected;
    }
    if (ec == asio::error::timed_out) {
        return NetErrc::keepalive_timeout;
    }
    if (ec == asio::error::invalid_argument) {
        return NetErrc::invalid_argument;
    }
    return std::nullopt;
}

} // namespace netmech

// -----------------------------------------------------------------------------
// std::error_code 集成
// -----------------------------------------------------------------------------
//  让 `std::error_code ec = NetErrc::link_closed;` 与
//  `std::error_code(NetErrc::x)` 这两类写法适用，靠 ADL 找到
//  netmech::make_error_code。

namespace std {
template <>
struct is_error_code_enum<netmech::NetErrc> : true_type {};
} // namespace std
