// =============================================================================
//  NetMechanism - 内部公共前置头
// =============================================================================
//  文件    : netmech_srcpch.hpp
//  用途    : 统一 Asio 头文件引入顺序、统一 Asio 类型别名、统一"用户回调异常
//            隔离"safety net。库内所有 .cpp / .hpp 都应**先**包含本头再包含其它。
//  编码    : UTF-8
//
//  为什么需要"先包含"
//  ----------------
//  Windows 上 `windows.h` 会被 Asio 间接引入；Asio 必须在 windows.h 之后使用
//  WinSock，否则会出现 `common_type` 等类型缺失。这里集中控制引入顺序，
//  避免各编译单元各自为政。
//
//  C++17 约束
//  ----------
//  本模块仅使用 C++17 特性（无 coroutine / concept / ranges / std::span /
//  std::format），目标平台包含嵌入式板卡。Asio 侧使用 Boost 1.74 的经典接口：
//  `asio::io_context` + `asio::strand<executor>` + `executor_work_guard`，
//  **不使用** C++20 才出现的 `any_io_executor`。
// =============================================================================
#pragma once

// -----------------------------------------------------------------------------
// Boost.Asio（必须先于其它可能引入 winsock 的头）
// -----------------------------------------------------------------------------
#include <asio.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/executor_work_guard.hpp>
#include "protocol_traits.hpp"
// executor.hpp 现在自带 Boost 依赖（它必须是自包含的公共头）。
// 先包含它，下面的 io_context_t / strand_t / work_guard_t 别名即由其提供，
// 本头不再重复定义，避免 downstream 出现歧义或重定义。
#include "io_pool.hpp"

// -----------------------------------------------------------------------------
// 标准库（只引必需项，保持编译期轻量）
// -----------------------------------------------------------------------------
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace netmech {

// -----------------------------------------------------------------------------
// Asio 类型别名（Boost 1.74 经典接口，非 C++20 any_io_executor）
// -----------------------------------------------------------------------------
//  这些别名放在 `netmech` 而不是 `detail`：executor.hpp 是公共头，
//  IoPool 的签名需要直接引用它们。
//
//  ★ `io_context_t` / `io_executor_t` / `strand_t` / `work_guard_t` 已由
//    上面的 <netmech/executor.hpp> 提供，此处**不再重复定义**（公共头自包含，
//    内部头复用它的定义，保证两者永远是同一个类型）。

/// 保活 / 连接超时定时器：一律使用 steady_clock，规避系统时钟被 NTP 回拨
/// 或用户改表导致的"定时器静默延后 / 连续补拍"（旧实现用 deadline_timer，即
/// posix_time 系统时钟，见缺陷 B-14）。
using timer_t = asio::basic_waitable_timer<std::chrono::steady_clock>;

using endpoint_t = asio::ip::tcp::endpoint;
using socket_t = asio::ip::tcp::socket;
using resolver_t = asio::ip::tcp::resolver;
using mutable_buffer_t = asio::mutable_buffer;
using const_buffer_t = asio::const_buffer;
using error_code_t = std::error_code;

// -----------------------------------------------------------------------------
// 协议模板参数化（P1）
// -----------------------------------------------------------------------------
//  上面三个 `*_t` 别名保留为**TCP 的默认形态**，供既有 TCP 代码零改动使用；
//  新增代码请改用 `tcp_traits` / `udp_traits`，以及
//  `protocol_traits<P>::endpoint_type` 这类**协议无关**写法。
//
//  这样做的目的（见 UDP支持评估.md §3.2）：
//    * 让同一套 io_context / 状态机 / 生命周期代码能服务多种协议；
//    * 避免一次性大改 900 行实现文件带来的回归风险 —— P1 只做**引入**不做**替换**。
//
//  `ip::tcp` 与 `ip::udp` 本身就满足 Asio 的 Protocol 概念，因此这里只做
//  别名收敛，不重造类型。统一地址类型的实测结论见 protocol_traits.hpp 文件头：
//  **不要**用 asio::generic::basic_endpoint（UDP 下无法转换，C2440）。
//
//  `tcp_traits` / `udp_traits` / `endpoint_of_t` / `socket_of_t` 均已由
//  netmech/protocol_traits.hpp 在 `netmech` 中提供，此处不再重复定义。
using tcp_protocol_t = asio::ip::tcp;
using udp_protocol_t = asio::ip::udp;

static_assert(tcp_traits::is_stream && tcp_traits::has_accept,
              "TCP 必须是流协议且提供 acceptor");
static_assert(udp_traits::is_datagram && !udp_traits::has_accept,
              "UDP 必须是数据报协议且没有 acceptor");

// -----------------------------------------------------------------------------
// 用户回调安全网：异常绝不逃逸到 io_context
// -----------------------------------------------------------------------------
//  旧实现里 `handle_accept` 直接绑到裸线程入口，一旦用户代码抛异常就穿过
//  `io_context::run()` → 线程函数抛异常 → `std::terminate`（缺陷 B-05）。
//  新实现的**每一个**进入用户代码的入口都必须经过下面两个函数之一：
//    * invoke_user()  —— 有返回值语义（如 IoHandler::on_init 返回 bool）
//    * invoke_void()  —— 无返回值语义（linkup / linkdown / 读回调 / 写回调 …）
//  捕获后不得再次抛出，也不做阻塞式日志（只在调试期由宏开关控制计数）。
// -----------------------------------------------------------------------------

/// 调用一个返回 bool 的用户回调；异常时返回 fallback（默认 false = 拒绝/失败）。
template <typename Fn>
inline bool invoke_user(Fn&& fn, bool fallback = false) noexcept
{
    try {
        return static_cast<bool>(fn());
    } catch (...) {
        return fallback;
    }
}

/// 调用一个无返回值的用户回调；异常被吞掉（等价于"该回调未生效"）。
template <typename Fn>
inline void invoke_void(Fn&& fn) noexcept
{
    try {
        fn();
    } catch (...) {
        // 有意为之：异常绝不穿过 io_context。
    }
}

/// 幂等的 socket 关闭：任何线程、任何次数调用都安全，绝不抛异常。
///
/// P1 起改为**协议无关**：实现已迁移到 `netmech/protocol_traits.hpp` 的
/// `close_socket_noexcept<Protocol>()`，按协议自动选择：
///   * 流协议（TCP）：先 shutdown(both) 再 close，避免"数据与 FIN 乱序"（A-16）
///   * 数据报（UDP）：UDP 无半关闭语义，直接 close
/// 这里的 TCP 版本保留为兼容入口，既有调用点无需改动。
inline void close_socket_noexcept(socket_t& sock, bool graceful = true) noexcept
{
    ::netmech::close_socket_noexcept<tcp_protocol_t>(sock, graceful);
}

} // namespace netmech
