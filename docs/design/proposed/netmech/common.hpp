// =============================================================================
//  NetMechanism - 公共基础类型
// =============================================================================
//  文件    : netmech/common.hpp（v1.0 定稿；实现阶段替换 include/netmech/ 下的旧头）
//  用途    : 定义 TCP / UDP 共用的类型：进程级初始化、地址、缓冲区视图、错误码。
//            业务代码一般只 #include <netmech/netmech.hpp>，不必直接包含本头。
//  依赖    : C++17 标准库。不包含任何平台或第三方网络库头。
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

namespace netmech {

// =============================================================================
//  进程级初始化
// =============================================================================

/// 初始化进程级 io 运行时（线程池 + io_context）。
///
/// **必须在任何 Mechanism::create() 之前调用**，且一个进程只调用一次。
/// 调用后线程数固定，运行中不可更改——因为每个 io_context 恰好由 1 条线程
/// run()，"1 context = 1 线程"是"同一连接的回调天然串行"这条契约的地基。
///
/// @param[in] io_threads 期望的 io 线程数。0 表示 max(1, hardware_concurrency())。
///                       任何值都会被归一化到 >= 1。
/// @return true  本次调用决定了线程数（首次调用）。
/// @return false 已初始化过。同值时视为幂等成功（返回 true），异值返回 false
///               且**不改变**任何东西。
///
/// @note 不调用本函数而直接 create()，create() 返回 nullptr（Debug 下 assert）。
/// @note 线程安全。
[[nodiscard]] bool initialize(std::size_t io_threads) noexcept;

/// 停止 io 运行时并回收所有 io 线程。幂等。
///
/// 调用后：所有 TCP 连接与 UDP 端点一并关闭（在途读写以取消类错误归还、
/// UDP 端点触发 on_close）、线程 join，之后 create() / create_udp_endpoint()
/// 一律失败。**不支持重新初始化**——避免出现"线程数改了，但先前创建的
/// io_context 仍在运行"的半死状态。
///
/// @warning 会阻塞直到所有 io 线程退出。不要在 io 回调里调用。
/// @note 通常只在 main() 返回前调用一次；不调用也可以，进程退出时由 OS 回收。
void shutdown() noexcept;

// =============================================================================
//  地址
// =============================================================================

/// 一个 IP 端点（地址 + 端口）。库的地址类型，不暴露底层网络实现。
///
/// @par 构造
///  - 默认构造 = 0.0.0.0:0（通配地址：listen 时让系统选端口，connect 时让系统选源端口）。
///  - 支持 IPv4 / IPv6 字面量（`"1.2.3.4"`、`"::1"`，端口单列）。
///  - **不做域名解析**：本库面向设备直连，地址来自配置，基本都是 IP 字面量。
///    需要主机名时请由调用方先解析好再传进来。
class Address {
public:
    Address() noexcept;

    /// 由 ip 字面量与端口构造。ip 非法时得到默认值（0.0.0.0:0），不抛异常。
    Address(std::string_view ip, std::uint16_t port) noexcept;

    /// 解析 "1.2.3.4:502" / "[::1]:9000" / ":9000"（等价 0.0.0.0:9000）。
    /// @return 解析失败返回 std::nullopt。
    [[nodiscard]] static std::optional<Address> parse(std::string_view text) noexcept;

    /// 通配地址（0.0.0.0，指定端口）。
    [[nodiscard]] static Address any(std::uint16_t port) noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] bool          is_v4() const noexcept;
    [[nodiscard]] bool          is_unspecified() const noexcept;
    [[nodiscard]] std::string   to_string() const;   ///< "1.2.3.4:502"

    friend bool operator==(const Address& a, const Address& b) noexcept;
    friend bool operator!=(const Address& a, const Address& b) noexcept;

private:
    // 实现细节：内部按 IPv4 / IPv6 存 128 位地址 + 端口，对外不暴露底层类型。
    std::uint64_t m_hi = 0;   ///< IPv6 高 64 位 / IPv4 存于低 32 位
    std::uint64_t m_lo = 0;
    std::uint16_t m_port = 0;
    std::uint8_t  m_family = 0;   ///< 0 = v4, 1 = v6
};

// =============================================================================
//  缓冲区
// =============================================================================

/// 读缓冲视图。由调用方构造，库只用不改其内容（库往里写收到的数据）。
///
/// @par owner 的两种模式
///  - **owner 为空**：调用方自己保证 data 指向的内存在"提交 → 完成回调返回"
///    期间有效。零开销路径，适合成员数组 / 栈数组。
///  - **owner 非空**：库把这个 shared_ptr 捕获进底层完成处理，只要还有 IO 在途，
///    这块内存就不会释放。调用方（乃至创建它的业务对象）提前析构也不会悬垂、
///    不会泄漏。owner 可以是任何类型（shared_ptr<T> → shared_ptr<void> 隐式转换，
///    deleter 保留在控制块里），库只把它当作不透明的所有权载体。
///
/// @par owner 的语义（契约，库无法校验）
///  **owner 必须保证 data 在其存活期间有效**。库只负责"把 owner 一起带在身上
///  直到完成回调"，它不检查 data 是否真的落在 owner 管理的内存里。
///
///  三种合法用法与各自后果：
///  - 一对一：owner 就是那块内存的所有者（最常见）。
///  - 一个 owner 多个 data：合法，但**最后一份引用释放时才回收**——这是延迟
///    释放，不是泄漏。要求每个 data 独立释放，就得每个 data 一个独立 owner。
///  - 别名指针（`std::shared_ptr<X> alias(parent, ptr)`）：共享同一控制块，
///    data 各自独立。
///
///  @warning **不要拿长期存活的对象当 owner**（例如 `shared_from_this()`）。
///           它会把整个对象钉在一条在途写操作上，容易与 `Conn → handler`
///           形成引用环；而且多个 Buf 共用同一控制块时，引用计数的加减会
///           集中到同一条 cache line，多线程下争用会被显著放大。
///           Buf 只承载"数据 + 谁保证它活着"，业务上下文请用 user_context。
///
/// @par 性能
///  owner 非空时每笔操作为 1 次原子 inc + 1 次原子 dec（约 20–40 ns 合计），
///  owner 为空则完全不碰原子量。真正的成本在首次 make_shared 的分配，
///  **所以应按连接复用一个 owner，而不是每包分配**；也可以配自定义 deleter
///  做对象池。按 3000 笔/秒估算，这部分开销约 0.1 ms/秒，可忽略。
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

// =============================================================================
//  错误码（评审通过后移入 netmech/error.hpp）
// =============================================================================

enum class NetErrc {
    ok = 0,
    not_initialized,       ///< 未调用 initialize()，或已 shutdown()
    already_initialized,   ///< initialize() 重复调用且线程数不一致
    invalid_argument,      ///< 地址非法 / 空 buffer / 保活参数矛盾
    duplicate_endpoint,    ///< 重复 listen 同一地址，或同一 remote 已有在途 connect
    buffer_full,           ///< 发送队列达上限（反压），**不会断链**
    not_connected,         ///< 连接未建立 / 已关闭
    link_closed,           ///< 连接已断开（对端关闭、本端 close、IO 错误）
    accept_failed,         ///< accept 环节失败（fd 耗尽、监听 socket 失效等）
    keepalive_timeout,     ///< 接收 / 发送保活超时
    handler_rejected,      ///< handler 工厂返回空，或 on_open 返回 false
    cancelled,             ///< 被显式取消（ConnectFuture::cancel 等）
    datagram_truncated,    ///< UDP 报文超过接收缓冲，已被截断（仅该报文受影响）
};

/// 构造 std::error_code。
[[nodiscard]] std::error_code make_error_code(NetErrc e) noexcept;

/// 把底层错误码归类成 NetErrc；无法归类或 ec 为空时返回 nullopt。
/// 用于在 on_read/on_write 里判断"这次失败是断开还是别的原因"。
[[nodiscard]] std::optional<NetErrc> to_net_errc(const std::error_code& ec) noexcept;

} // namespace netmech

namespace std {
template <>
struct is_error_code_enum<netmech::NetErrc> : true_type {};
} // namespace std
