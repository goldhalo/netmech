// =============================================================================
//  netmech/protocol_traits.hpp —— 协议类型抽象层（P1）
// =============================================================================
//  文件    : netmech/protocol_traits.hpp
//  用途    : 把「协议相关的类型与操作」从实现中抽离出来，使同一套
//            io_context / 状态机 / 生命周期代码能同时服务 TCP（流）与 UDP（数据报）。
//  依赖    : Boost 1.74 + 标准库。不含 <iostream>。
//  编码    : UTF-8
//
//  ── 为什么需要这一层 ────────────────────────────────────────────────────────
//  P1 之前，实现里用的是两个**固定指向 TCP** 的别名：
//      using endpoint_t = asio::ip::tcp::endpoint;
//      using socket_t   = asio::ip::tcp::socket;
//  这导致 UDP 无法复用任何代码。改为**协议模板参数**后：
//      TCP:  protocol_traits<asio::ip::tcp>
//      UDP:  protocol_traits<asio::ip::udp>
//
//  ── 与 Boost.Asio 自带 Protocol 概念的关系 ─────────────────────────────────
//  `ip::tcp` 与 `ip::udp` 本身就已满足 Asio 的 Protocol 概念（提供
//  `endpoint` / `socket` / `acceptor` 等嵌套类型）。所以本 traits **不重造轮子**，
//  只做两件事：
//    1. 统一暴露 `endpoint_t` / `socket_t` 等别名，让实现代码只写一处；
//    2. 提供**协议能力标记**（is_stream / has_acceptor）与**协议特定操作**
//       （如 TCP 的 shutdown/shutdown_both、UDP 的 datagram 收发），
//       让上层用 `if constexpr` 分支而不是到处写 `#ifdef` 或特化重载。
//
//  ★ 注意：不要用 asio::generic::basic_endpoint 做统一地址类型。
//    实测（Boost 1.74 / MSVC 2019）它**不能**从 ip::udp::endpoint 构造，
//    会报 C2440。统一地址请走「模板参数 + 下层转换」的路线。
// =============================================================================
#pragma once

#include <cstddef>
#include <type_traits>

#include <asio/basic_datagram_socket.hpp>
#include <asio/basic_socket_acceptor.hpp>
#include <asio/basic_stream_socket.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>

namespace netmech {

// -----------------------------------------------------------------------------
// 协议能力标记
// -----------------------------------------------------------------------------

/// 判断 T 是否为 Asio 的流式协议（如 ip::tcp）。
///
/// 依据是 `Protocol::socket` 是否派生自 `basic_stream_socket`。
/// 用处：`if constexpr (is_stream_protocol_v<P>) { ... }` 分支处理
/// shutdown / read_exact 等仅对流有意义的操作。
template <typename Protocol, typename = void>
struct is_stream_protocol : std::false_type {};

template <typename Protocol>
struct is_stream_protocol<Protocol, std::void_t<typename Protocol::socket>>
    : std::is_base_of<asio::basic_stream_socket<Protocol>,
                      typename Protocol::socket> {};

template <typename Protocol>
inline constexpr bool is_stream_protocol_v = is_stream_protocol<Protocol>::value;

/// 判断 T 是否为 Asio 的数据报协议（如 ip::udp）。
///
/// 依据是 `Protocol::socket` 是否派生自 `basic_datagram_socket`。
template <typename Protocol, typename = void>
struct is_datagram_protocol : std::false_type {};

template <typename Protocol>
struct is_datagram_protocol<Protocol, std::void_t<typename Protocol::socket>>
    : std::is_base_of<asio::basic_datagram_socket<Protocol>,
                      typename Protocol::socket> {};

template <typename Protocol>
inline constexpr bool is_datagram_protocol_v =
    is_datagram_protocol<Protocol>::value;

/// 协议是否提供 acceptor（TCP 有，UDP 无）。
///
/// 实测事实（Boost 1.74）：`asio::ip::udp` **没有** `acceptor` 嵌套类型，
/// 只有 `socket` / `endpoint` / `resolver`。所以这里既要按语义区分（UDP 无
/// accept 概念），也要保证在 UDP 下**不去实例化** `Protocol::acceptor`。
///
/// 实现上用 `is_stream_protocol_v` 判定，并用 SFINAE 让 acceptor_type 在
/// 无 acceptor 的协议下退化为 void，避免任何误用导致的硬编译错误。
template <typename Protocol>
struct has_acceptor : std::bool_constant<is_stream_protocol_v<Protocol>> {};

template <typename Protocol>
inline constexpr bool has_acceptor_v = has_acceptor<Protocol>::value;

/// 检测 Protocol 是否定义了嵌套 `acceptor` 类型。
template <typename Protocol, typename = void>
struct has_acceptor_type : std::false_type {};

template <typename Protocol>
struct has_acceptor_type<Protocol, std::void_t<typename Protocol::acceptor>>
    : std::true_type {};

/// 惰性选择 acceptor 类型：只有真正存在 `Protocol::acceptor` 时才去引用它，
/// 否则退化为 `void`。这样 `protocol_traits<ip::udp>` 可以被安全实例化。
template <typename Protocol, bool = has_acceptor_type<Protocol>::value>
struct acceptor_selector {
    using type = typename Protocol::acceptor;
};

template <typename Protocol>
struct acceptor_selector<Protocol, false> {
    using type = void;
};

// -----------------------------------------------------------------------------
// 统一类型别名
// -----------------------------------------------------------------------------

/// 协议类型集合：所有协议相关的类型从这里取，实现层不再直接写 `tcp::`。
template <typename Protocol>
struct protocol_traits {
    using protocol_type  = Protocol;
    using endpoint_type  = typename Protocol::endpoint;
    using socket_type    = typename Protocol::socket;
    using resolver_type  = typename Protocol::resolver;

    /// 流协议才有 acceptor；UDP 下退化为 void，防止误实例化。
    using acceptor_type = typename acceptor_selector<Protocol>::type;

    static constexpr bool is_stream   = is_stream_protocol_v<Protocol>;
    static constexpr bool is_datagram = is_datagram_protocol_v<Protocol>;
    static constexpr bool has_accept  = has_acceptor_v<Protocol>;
};

// -----------------------------------------------------------------------------
// 常用协议的 traits 实例（公共便利别名）
// -----------------------------------------------------------------------------

/// TCP 协议 traits。
using tcp_traits = protocol_traits<asio::ip::tcp>;

/// UDP 协议 traits。
using udp_traits = protocol_traits<asio::ip::udp>;

/// 协议无关的 endpoint / socket 取值写法。
template <typename Protocol>
using endpoint_of_t = typename protocol_traits<Protocol>::endpoint_type;

template <typename Protocol>
using socket_of_t = typename protocol_traits<Protocol>::socket_type;

// -----------------------------------------------------------------------------
// 协议特定操作：关闭
// -----------------------------------------------------------------------------
//  流协议：先 shutdown(both) 再 close —— 让对端立即收到 FIN/RST，
//          避免"数据与 FIN 乱序"（旧实现缺陷 A-16）。
//  数据报：UDP 无半关闭概念，直接 close 即正确。
// -----------------------------------------------------------------------------

/// 幂等的 socket 关闭：任何线程、任何次数调用都安全，绝不抛异常。
///
/// @param[in] graceful 是否走"优雅关闭"（先 shutdown 再 close）。
///
/// @par 为什么需要这个开关
///  流协议下 `shutdown(SHUT_RDWR)` 会**先发出 FIN**，连接随之进入正常关闭流程，
///  之后再 close 时 `SO_LINGER{1,0}` 已经来不及发 RST —— 主动关闭方照样进
///  TIME_WAIT。所以想真正用 RST 快速释放端口，必须**跳过 shutdown 直接 close**。
template <typename Protocol>
inline void close_socket_noexcept(
    typename protocol_traits<Protocol>::socket_type& sock,
    bool graceful = true) noexcept
{
    using socket_type = typename protocol_traits<Protocol>::socket_type;

    std::error_code ignored;
    try {
        if (sock.is_open()) {
            if constexpr (is_stream_protocol_v<Protocol>) {
                // 只有流协议才有 shutdown；`shutdown_both` 是 socket_type 的枚举值。
                // graceful=false 表示"立刻断"（SO_LINGER{1,0} 已设），此时**不能** shutdown，
                // 否则 FIN 先发出去，RST 就发不成了。
                if (graceful) {
                    sock.shutdown(socket_type::shutdown_both, ignored);
                }
            }
            sock.close(ignored);
        }
    } catch (...) {
        // 忽略：close 语义上必须成功返回。
    }
}

} // namespace netmech
