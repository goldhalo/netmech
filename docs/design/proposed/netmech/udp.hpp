// =============================================================================
//  NetMechanism - UDP 端对外接口（v1.0 定稿）
// =============================================================================
//  文件    : netmech/udp.hpp（v1.0 定稿；实现阶段替换 include/netmech/ 下的旧头）
//  用途    : 定义 UDP 侧全部对外类型。与 TCP 侧完全独立——UDP 无连接，
//            不复用 Conn / ConnHandler 那套拉取式读写模型。
//  依赖    : netmech/common.hpp（地址、缓冲区、错误码）+ C++17 标准库。
//  编码    : UTF-8
//
//  NetMechanism 的 UDP 部分只做一件事：**绑定端口、收发报文、把源地址原样告诉你**。
//  它不建会话、不按对端地址分流、不做"连接"抽象——因为 UDP 根本没有连接，
//  而源地址在真实网络里不可信。
//
//  ★ 为什么库不做会话管理（重要）
//  ---------------------------------------------------------------------------
//  在两个真实场景里，用"对端地址"当身份都会出错：
//
//  1. **SIP 协商端口与实际端口不一致**：SDP 里协商的是 :40000，对端实际从 :40123
//     发过来（协商时只是挑了个当前可用的端口填进去）。
//  2. **经过 NAT / 路由器**：你看到的源地址是运营商的映射端口，跟对端主机端口
//     没有任何对应关系；同一台设备重启后映射还可能变。
//
//  如果库按源地址建会话，上面两种情况都会导致"同一个呼叫被拆成多个会话 / 同一路流
//  被认成新流"，状态机、计费、鉴权全部错乱。所以：
//
//  · **源地址不用来识别身份**，只作为报文的一部分原样透传给你；
//  · 会话标识（SIP 的 Call-ID、RTP 的 SSRC、事务 ID）属于应用层，库不碰；
//  · 库也**不提供**"按地址过滤 / 首个包锁定对端"之类的默认行为——需要的话，
//    在你的 on_read 里自己判断 from，库完全配合。
//
//  ★ 两分钟上手
//  ---------------------------------------------------------------------------
//  #include <netmech/netmech.hpp>
//
//  // 场景一：GB28181 的 RTP —— 一路流一个端口
//  struct RtpHandler : netmech::UdpHandler {
//      void on_read(netmech::UdpEndpoint& ep, std::error_code ec,
//                   netmech::Buf buf, std::size_t n,
//                   const netmech::Address& from) override
//      {
//          if (ec) { return; }                 // datagram_truncated：这条被截断，丢弃
//          parse_rtp(static_cast<const std::uint8_t*>(buf.data), n, from);
//      }
//
//      // 对端地址与上次不同（协商端口与实际不符 / NAT 换映射）时第一时间知道。
//      // 回包用 ep.last_peer()，它才是"对方真正在用的地址"。
//      void on_peer_changed(netmech::UdpEndpoint& ep, const netmech::Address& actual) override
//      {
//          log("rtp peer actual = %s", actual.to_string().c_str());
//      }
//  };
//
//  netmech::UdpConfig cfg;
//  cfg.bind          = netmech::Address::any(10000);
//  cfg.recv_capacity = 1500;                   // 一个 MTU 内，不分片
//  auto ep = netmech::create_udp_endpoint(cfg, handler, nullptr, ec);
//  ep->send_to(target, {nullptr, packet, len});
//
//  // 场景二：SIP 信令 —— 一个端口承载所有呼叫
//  struct SipHandler : netmech::UdpHandler {
//      void on_read(netmech::UdpEndpoint& ep, std::error_code ec,
//                   netmech::Buf buf, std::size_t n,
//                   const netmech::Address& from) override
//      {
//          if (ec) { return; }
//          auto msg = parse_sip(static_cast<const char*>(buf.data), n);
//          auto it  = m_calls.find(msg.call_id);   // ← 用 Call-ID 认呼叫，不用 from
//          if (it != m_calls.end()) { it->second->on_message(msg); }
//          ep.send_to(from, make_response(msg));   // ← 但回包必须用 from
//      }
//      std::unordered_map<std::string, std::shared_ptr<Call>> m_calls;
//  };
//
//  ★ 四条设计原则
//  ---------------------------------------------------------------------------
//  [1] **没有连接，只有端点。** 一个 UdpEndpoint = 一个绑定的本地端口。
//      一路 RTP 流开一个端点，SIP 的所有呼叫共用一个端点——差别全在应用层。
//  [2] **源地址零信任。** from 只用来回包和记日志，不用来识别身份。
//      回包时必须用它（NAT 后这是唯一能到达对端的地址），但这与"身份"是两件事。
//  [3] **推送式交付。** 库自己持续收报文，收到就回调，你不需要（也不能）调 read()。
//      UDP 一个报文就是一条完整消息，没有"读多少"的控制需求。
//  [4] **发送是同步的。** `send_to` 返回 true 只表示已交给内核；UDP 不保证送达，
//      库也不为它建异步队列——丢一帧比积压一帧更符合实时流的要求。
//  [5] **地址是观测，不是身份。** 库告诉你"最近是谁发来的"（`last_peer()`）和
//      "来源变了"（`on_peer_changed`），但不拿它建会话、做过滤、锁对端。
//      谁是同一条流/同一个呼叫，只能由报文内容（SSRC / Call-ID）决定。
//
//  ★ 错误模型
//  ---------------------------------------------------------------------------
//  UDP 没有"链路"，所以 TCP 那套"错误即断链"不成立。所有失败都退化为
//  **单次操作失败**，端点继续可用：
//
//    · 报文超过 recv_capacity     → 截断该报文，on_read 的 ec = datagram_truncated
//    · 发送报文超 kMaxDatagramSize → 本次 send_to 返回 false
//    · 发送缓冲区满                → 本次 send_to 返回 false
//    · 暂时无数据（EAGAIN）        → 不是错误，库继续等
//
//  只有 socket 本身不可恢复（被外部关闭、底层致命错误）才回调 on_close 并终止端点。
// =============================================================================
#pragma once

#include "netmech/common.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace netmech {

// =============================================================================
//  常量
// =============================================================================

/// 单个 UDP 报文的理论上限（IPv4 载荷 65535 - 20 IP 头 - 8 UDP 头 = 65507）。
///
/// IPv6 的载荷上限是 65527，取 65507 以同时兼容两者。
/// `UdpConfig::recv_capacity` 与 `send_to` 的报文长度都不得超过它。
inline constexpr std::size_t kMaxDatagramSize = 65507;

// =============================================================================
//  配置
// =============================================================================

/// 报文数据的交付方式。
///
/// 只影响 `on_read` 里 `Buf::owner` 的填充方式，不影响调用方写法。
enum class DeliverMode {
    /// 库复用一块接收缓冲（大小 = recv_capacity），零分配。
    /// `Buf::owner` 为空；`buf.data` 指向库的内存，**仅在本次 on_read 内有效**，
    /// 下次接收就会覆盖。适合当场解析、统计、转发。
    borrow,

    /// 库为每个报文单独分配一块内存，`Buf::owner` 非空且持有该块。
    /// 你可以把 Buf 存下来、投递到别的线程慢慢处理，最后一份引用释放时内存才回收。
    /// 代价是每包一次堆分配，适合低频、处理耗时的场景（如 SIP 信令）。
    own,
};

/// UDP 端点配置。
struct UdpConfig {
    /// 本地绑定地址。端口 0 表示由系统分配（用 `UdpEndpoint::local_address()` 取回）。
    Address bind;

    /// 单报文接收上限，必须 > 0 且 <= kMaxDatagramSize。
    ///
    /// 超过该长度的报文会被截断（`on_read` 收到 `datagram_truncated`）。
    /// 建议值：RTP 用 1500（一个 MTU 内不分片），SIP 用 4096（大报文头）。
    std::size_t recv_capacity = 2048;

    /// SO_RCVBUF / SO_SNDBUF。0 表示使用系统默认。
    ///
    /// @note 这是内核缓冲，与 recv_capacity（单个报文长度）不是一回事。
    ///       高吞吐场景调大它可以减少丢包。
    std::size_t socket_buf_size = 256 * 1024;

    /// 是否允许 bind 已被占用的地址（SO_REUSEADDR）。多进程共用同一端口时需要。
    bool reuse_address = false;

    /// 是否允许发送广播报文（SO_BROADCAST）。
    bool allow_broadcast = false;

    /// 报文交付方式，见 DeliverMode。
    DeliverMode deliver = DeliverMode::borrow;

    /// 要加入的组播组。为空表示不使用组播。
    std::optional<Address> multicast_group;

    /// 组播使用的本地网卡地址（多网卡机器必填）。为空表示由系统选择。
    std::optional<Address> multicast_interface;

    /// 组播 TTL（仅发送组播时生效）。
    std::uint8_t multicast_hops = 1;

    /// 组播是否回环到本机（仅发送组播时生效）。
    bool multicast_loopback = true;
};

// =============================================================================
//  回调接口
// =============================================================================

class UdpEndpoint;

/// 报文到达 / 端点关闭的处理接口。
///
/// @par 生命周期
///  - 由 `create_udp_endpoint()` 交入，库**强持有**直到端点关闭（close 或析构）。
///    所以回调里的 this 一定有效，不需要 enable_shared_from_this。
///  - 一个 handler 实例可以同时服务多个端点：回调参数里带 `ep`，用 `ep.id()` 区分。
///
/// @par 线程模型
///  - 全部回调在该端点所属的 io 线程上执行；同一端点严格串行。
///  - 不同端点可能在不同 io 线程上并发回调（跨端点共享状态请自行加锁）。
///  - 回调内**不得阻塞**：库在这条线程上继续收报文，阻塞会导致内核缓冲区溢出丢包。
///    处理耗时就把数据拷走、投递到自己的线程。
class UdpHandler {
public:
    virtual ~UdpHandler() = default;

    /// 收到一个完整报文。
    ///
    /// @param[in] ep   收到报文的端点。
    /// @param[in] ec   为空表示正常；`NetErrc::datagram_truncated` 表示报文超过
    ///                 `recv_capacity` 已被截断（此时 buf 里只有前 recv_capacity 字节，
    ///                 内容不完整，通常应当直接丢弃）。
    /// @param[in] buf  报文数据。`buf.size` 在 borrow 模式下是 recv_capacity、
    ///                 own 模式下是实际长度；两种情况都以 @p n 为准。
    /// @param[in] n    本报文的有效字节数。
    /// @param[in] from 发送方地址（报文来源）。
    ///
    /// @par 关于 from（务必看清）
    ///  - **不要**用它识别身份：经过 NAT / 端口协商不一致时它不可信。
    ///    会话标识请从报文内容里取（Call-ID / SSRC / 事务 ID）。
    ///  - **但回包必须用它**：NAT 之后这是唯一能到达对端的地址。
    ///  - `ep.send_to(from, ...)` 是最常见的回包写法。
    ///
    /// @par buf 的生命周期
    ///  - `buf.owner` 非空（own 模式）：你可以持有它，数据在最后一份引用释放前有效。
    ///  - `buf.owner` 为空（borrow 模式）：数据只在本次回调内有效，要留存请自行拷贝。
    virtual void on_read(UdpEndpoint& ep,
                         std::error_code ec,
                         Buf buf,
                         std::size_t n,
                         const Address& from) = 0;

    /// 端点已关闭。每个端点恰好调用一次。
    ///
    /// @param[in] ec 关闭原因：为空表示主动 `close()`；非空表示 socket 发生
    ///                不可恢复错误（此时端点已终止）。
    ///
    /// @note 本回调返回后，库释放对 handler 的强引用。
    virtual void on_close(UdpEndpoint& ep, std::error_code ec)
    {
        (void)ep;
        (void)ec;
    }

    /// 报文来源地址与上一次不同（**先于 on_read 调用**）。
    ///
    /// @param[in] ep   收到报文的端点。
    /// @param[in] from 本次报文的源地址；可用 `ep.last_peer()` 取回同一个值。
    ///
    /// @par 什么时候会触发
    ///  - 端点收到的**第一个**报文（此前没有"上一次"）；
    ///  - 之后任何一次来源地址与上次不同的报文。
    ///
    /// @par 典型用途
    ///  - **RTP/RTCP**：SDP 协商的端口和对端实际发送的端口经常不一致，收到第一个包
    ///    就能拿到真实地址，之后 `send_to(*ep.last_peer(), ...)` 回 RTCP 才发得对。
    ///  - **NAT / 设备重启**：映射端口变化时能第一时间感知并更新本地状态。
    ///
    /// @warning 这是**观测事件，不是身份判定**。地址变了不代表换了一个对端——
    ///          识别身份仍然要靠报文内容（SSRC / Call-ID / 事务 ID）。
    ///
    /// @note 一个端口承载多个对端（如 SIP 信令）时，本回调会随每个不同来源频繁
    ///       触发；不需要就保持默认空实现，库只做一次地址比较，开销可忽略。
    virtual void on_peer_changed(UdpEndpoint& ep, const Address& from)
    {
        (void)ep;
        (void)from;
    }
};

// =============================================================================
//  端点
// =============================================================================

/// 一个绑定了本地地址的 UDP 端点。没有连接概念，收发都是报文。
///
/// @par 生命周期
///  - 由 `create_udp_endpoint()` 创建，返回 shared_ptr 由你持有。
///  - 调用 `close()` 立即停止收发并触发 on_close；最后一个 shared_ptr 释放时
///    若还没关，等效于调用 close()。
///
/// @par 线程模型
///  - `send_to` / `close` / 各查询方法可从**任意线程**调用。
///  - 所有回调在该端点所属的 io 线程上串行执行。
class UdpEndpoint {
public:
    /// 发送一个报文。
    ///
    /// @param[in] dst 目标地址。可以是单播、广播（需 allow_broadcast）、组播（需配置）。
    /// @param[in] buf 报文内容。提交后立即返回，不持有 owner（同步发送）。
    /// @return true  已交给内核。**UDP 不保证送达**，这只是一个入队成功的含义。
    /// @return false 未发出，原因：
    ///         · 端点已关闭；
    ///         · `buf.data` 为空或 `buf.size == 0`；
    ///         · `buf.size > kMaxDatagramSize`；
    ///         · 发送缓冲区已满（`NetErrc::buffer_full`，可稍后重试）；
    ///         · 目标地址非法或不允许（如未开广播却发广播）。
    ///
    /// @par 为什么是同步的
    ///  UDP 一个报文就是一次系统调用，没有"排队等待可写"的必要。为它建异步队列
    ///  只会增加延迟和内存占用，而实时流场景下丢一帧好过积压一帧。
    ///
    /// @par 性能与"不阻塞"的前提
    ///  - socket 始终是**非阻塞**的：发送缓冲满时本调用立即返回 false
    ///    （`NetErrc::buffer_full`），绝不阻塞线程。如果误把 socket 设成阻塞
    ///    模式，一次 send 就能把整条 io 线程（连同绑在上面的所有端点）卡住——
    ///    这是实现层必须守住的一条。
    ///  - 单次调用约 2–5 μs（一次系统调用）。100 路 × 30 fps ≈ 3000 包/秒，
    ///    合计 6–15 ms/秒，约 1%。
    ///  - 在 `on_read` 里直接转发会让下一次接收推迟几微秒（同一线程）。对延迟
    ///    敏感或发送量远大于接收的场景，**可以从业务线程直接调用**——UDP 没有
    ///    顺序要求，不必经过 io 线程。
    ///  - 多个端点按 round-robin 分配到 N 个 io_context，100 路会摊到 N 条线程上，
    ///    不会集中到同一条。
    ///
    /// @note 实现上内部加锁（socket 句柄非线程安全），因此多线程并发发送是安全的，
    ///       但报文之间的相对顺序不保证——UDP 本身也不保证到达顺序。
    bool send_to(const Address& dst, ConstBuf buf);

    /// 本端实际绑定地址（bind 时端口给 0 就靠它取回）。
    [[nodiscard]] Address local_address() const noexcept;

    [[nodiscard]] bool is_open() const noexcept;

    /// 端点 ID（进程内唯一），便于日志与多端点区分。
    [[nodiscard]] std::uint32_t id() const noexcept;

    /// 创建时传入的用户上下文，原样返回。
    ///
    /// 典型用法：把"这条端点属于哪路流/哪个业务域"挂在上面，
    /// 一个 handler 服务多个端点时靠它区分。
    [[nodiscard]] std::shared_ptr<void> user_context() const noexcept;

    /// 关闭端点。幂等。
    ///
    /// 关闭后不再回调 on_read（已收到的报文直接丢弃），触发一次 on_close(ec 为空)。
    void close() noexcept;

    // -------------------------------------------------------------------------
    // 统计（用于丢包率、流量观测）
    // -------------------------------------------------------------------------

    [[nodiscard]] std::uint64_t datagrams_received()  const noexcept;  ///< 成功收到的报文数
    [[nodiscard]] std::uint64_t datagrams_sent()      const noexcept;  ///< send_to 成功的报文数
    [[nodiscard]] std::uint64_t datagrams_truncated() const noexcept;  ///< 因超长被截断的报文数

    /// 最近一次成功收到报文的源地址。从未收到过时返回 nullopt。
    ///
    /// @par 为什么需要它
    ///  应用往往需要"往实际发来数据的地方回包"，而这个地址只有从收到的包里才知道：
    ///  SDP 协商的端口可能与对端实际发送端口不同，经 NAT 后看到的更是映射端口。
    ///  用它就能正确回 RTP/RTCP，而不必依赖协商值。
    ///
    /// @par 与 on_peer_changed 的分工
    ///  - 本方法是**拉取**：需要时查一下，适合"收到包就回包"的路径。
    ///  - `on_peer_changed` 是**事件**：地址变化时主动通知，适合更新状态机/记日志。
    ///
    /// @note 这是观测值，不是身份。多对端共用一个端口时，它只是"最后一个发包的人"。
    [[nodiscard]] std::optional<Address> last_peer() const noexcept;

    /// 最近一次成功收到报文的时间（steady_clock）。从未收到过时返回 time_point{}。
    ///
    /// @note 库**不做保活判断**：UDP 没有链路，"对端还在不在"是应用层的事
    ///       （SIP 用 OPTIONS、RTP 用 RTCP）。本方法只提供观测数据。
    [[nodiscard]] std::chrono::steady_clock::time_point last_recv_time() const noexcept;

    ~UdpEndpoint() = delete;                       ///< 不要析构：端点由库管理
    UdpEndpoint(const UdpEndpoint&) = delete;
    UdpEndpoint& operator=(const UdpEndpoint&) = delete;

private:
    UdpEndpoint() = delete;                        ///< 仅由 create_udp_endpoint 构造
};

/// 创建一个 UDP 端点（同步完成绑定）。
///
/// @param[in]  cfg     配置。非法配置（recv_capacity 为 0 或超过 kMaxDatagramSize、
///                     地址非法）会以 invalid_argument 返回。
/// @param[in]  handler 报文处理接口。库强持有到端点关闭。
/// @param[in]  user_ctx 用户上下文，可从 `ep.user_context()` 取回；可为空。
/// @param[out] ec      失败原因。成功时为空。
/// @return 成功返回非空端点；失败返回 nullptr 并置 @p ec。
///
/// @par 何时算"创建成功"
///  返回时 socket 已 bind 并开始收报文，因此 handler 可能在**本函数返回之前**
///  就收到第一个 on_read。如果 handler 依赖"创建函数返回后"才就绪的状态，
///  请在 handler 里做同步（例如先置一个 ready 标志再放行）。
///
/// @note 一个进程可以创建任意多个端点（SIP 5060 + RTP 10000~20000 几十上百个都正常），
///       它们共享 `netmech::initialize()` 建立的 io 线程池。
[[nodiscard]] std::shared_ptr<UdpEndpoint>
create_udp_endpoint(const UdpConfig&          cfg,
                    std::shared_ptr<UdpHandler> handler,
                    std::shared_ptr<void>     user_ctx,
                    std::error_code&          ec);

} // namespace netmech
