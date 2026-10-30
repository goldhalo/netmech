// =============================================================================
//  NetMechanism - TCP 端对外接口（v1.0 定稿）
// =============================================================================
//  文件    : netmech/tcp.hpp（v1.0 定稿；实现阶段替换 include/netmech/ 下的旧头）
//  用途    : 定义 TCP 侧全部对外类型。使用方一般只需 #include <netmech/netmech.hpp>
//            （汇总头）。除本头与 error.hpp 外不暴露其他头，也看不到 asio /
//            线程池 / 队列等实现细节。
//  依赖    : C++17 标准库。**不包含 asio**——地址、缓冲区、完成通知全部自有类型。
//  编码    : UTF-8
//
//  NetMechanism —— 一个回调不捕获 this、缓冲区所有权显式、失败只走 error_code
//  的 TCP 网络库。上层只需要实现两个接口：ConnHandler（单连接协议处理）和
//  Linkable（决定新连接交给谁）。
//
//  ★ 两分钟上手
//  ---------------------------------------------------------------------------
//  #include <netmech/netmech.hpp>
//
//  // 1) 单连接处理器：只实现虚函数，不用 enable_shared_from_this，不用 lambda
//  struct Echo : netmech::ConnHandler {
//      bool on_open(netmech::Conn& c) override { return c.read({nullptr, rx, sizeof rx}); }
//
//      void on_read(netmech::Conn& c, std::error_code ec,
//                   netmech::Buf b, std::size_t n) override
//      {
//          if (ec) { return; }                     // buf 已归还，无需处理
//          c.write({nullptr, b.data, n});          // 原样回写
//          c.read({nullptr, rx, sizeof rx});       // 续读：库不会自动续读
//      }
//      void on_write(netmech::Conn&, std::error_code, netmech::ConstBuf, std::size_t) override {}
//      void on_close(netmech::Conn&) override {}
//
//      std::uint8_t rx[4096];
//  };
//
//  // 2) 订阅者：决定每条新连接交给哪个 handler
//  struct Srv : netmech::Linkable {
//      std::shared_ptr<netmech::ConnHandler> on_accept(netmech::Conn&) override
//      {
//          return std::make_shared<Echo>();        // 返回 nullptr = 拒绝该连接
//      }
//  };
//
//  // 3) 启动
//  netmech::initialize(4);                         // 必须先调；线程数进程级固定
//  auto mech = netmech::Mechanism::create();
//  mech->listen(netmech::Address::any(9000), std::make_shared<Srv>());
//
//  ★ 设计原则（[1]-[5] 在此，[6] 错误模型与 [7] 顺序保证见下文专节）
//  ---------------------------------------------------------------------------
//  [1] **handler 归库管，业务对象归你管。** Linkable 返回 shared_ptr<ConnHandler>，
//      库强持有到连接关闭——所以回调里的 this 一定有效，不需要保命技巧。
//      handler 内部的业务对象（设备模型、会话上下文）用 weak_ptr 持有即可。
//  [2] **读写对称。** 每一笔 read/write 各自恰好一次完成回调；成功与失败共用
//      同一个回调，靠 ec 区分：ec 为空 = 完成，ec 非空 = buf 归还给你。
//  [3] **缓冲区所有权显式化。** Buf::owner 为空 = 调用方保证生命周期（零开销）；
//      非空 = 库把 owner 带进完成处理，调用方先析构也不会悬垂或泄漏。
//  [4] **关闭顺序固定。** 先逐笔归还在途 read/write（ec != 0），再 on_close，
//      再 Linkable::on_linkdown。用户在任何一步都能安全释放内存。
//  [5] **不抛异常。** 对外只通过 std::error_code 与返回值表达失败。
//      唯一例外是 Mechanism::create() 在线程创建失败时抛 std::system_error。
//
//  ★ 错误模型（第 [6] 条原则，务必先读）
//  ---------------------------------------------------------------------------
//  把"没做成"和"做坏了"严格分开，二者对连接的影响完全不同：
//
//    A. **未受理**（read/write 返回 false，不回调）—— 连接状态**不变**：
//       · 已有读在途            （Conn 约束：同一时刻只允许一笔读）
//       · 发送队列达上限         （反压，NetErrc::buffer_full）
//       · data 为空 / size 为 0  （NetErrc::invalid_argument）
//       · 连接已关闭            （NetErrc::link_closed / not_connected）
//
//    B. **已受理但出错**（完成回调 ec != 0）—— 整条连接**立即关闭**：
//       · 对端关闭 / 本端 close / RST / IO 错误 → ec = link_closed
//       · 接收或发送保活超时（第 max_miss 次检查未通过之后） → ec = keepalive_timeout
//       关闭时，其余在途与排队中的 op 按 FIFO 逐个以 ec != 0 回调，
//       全部归还完毕后调用 ConnHandler::on_close。
//
//  保活不是"到点就断"：每次检查未通过都会回调 `ConnHandler::on_idle`
//  （IdleInfo 里带 missed / limit），共回调 max_miss 次，**最后一次之后才断链**。
//  所以应用在第一次未通过时就能发心跳或更新在线状态，而不是最后一刻才收到通知。
//
//  为什么出错就断链：TCP 是字节流，读失败时无法判断对端到底发了多少字节、
//  帧边界在哪里，继续读只会读到错位数据。不存在"部分可用"的中间态，所以除
//  "未受理"之外，任何错误都导致整条连接关闭。反压（buffer_full）属于"未受理"，
//  连接照常可用，调用方降速重试即可。
//
//  ★ 顺序保证（第 [7] 条原则）
//  ---------------------------------------------------------------------------
//  **顺序由库保证，不推给调用方。**
//
//  - 所有被受理（返回 true）的 write，其数据在 TCP 流上的顺序与完成回调的顺序，
//    都严格等于"受理顺序"，绝不交错、绝不乱序、出错后也不重排。
//  - 受理顺序 = 调用线程在**发送队列锁**内完成"容量检查 + 入队"的先后。
//    因此多线程并发调用同样有确定顺序（锁序列化），**不依赖** io 线程调度。
//  - 为什么不能只 post 到 io 线程再入队：那样 `write()` 返回时尚未判断容量，
//    超限的请求只能在 io 线程被丢弃，违反"受理即恰好一次回调"。锁是唯一同时
//    满足【同步返回是否受理】与【FIFO 顺序确定】的做法。
//  - read 侧不存在多笔排序问题：同一连接同一时刻只允许一笔读在途，并发调用时
//    至多一笔返回 true。
// =============================================================================
#pragma once

#include "netmech/common.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <system_error>

namespace netmech {

class Conn;
class ConnHandler;
class Linkable;
class Mechanism;

/// 读模式。
enum class ReadMode {
    some,    ///< 收到至少 1 字节即完成：on_read 成功时 n <= 请求长度
    exact,   ///< 收满请求长度才完成：on_read 成功时 n == 请求长度（固定长度协议用）
};

/// 保活方向。
enum class IdleKind {
    recv,   ///< 接收侧：连续若干次检查都没收到任何字节
    send,   ///< 发送侧：连续若干次检查都没有任何成功写出
};

/// 一次保活检查未通过时的信息。
struct IdleInfo {
    IdleKind      kind;
    std::uint32_t missed = 0;   ///< 连续第几次未通过，从 1 开始
    std::uint32_t limit  = 0;   ///< 配置的 max_miss；missed == limit 时库随后关闭连接

    /// 还有没有挽回余地：`missed < limit` 为 true（此时库不会关闭连接）。
    [[nodiscard]] bool has_grace() const noexcept { return missed < limit; }
};

// =============================================================================
//  协议处理器
// =============================================================================

/// 一条 TCP 连接的协议处理器。每条被接管的连接对应一个实例。
///
/// @par 生命周期
///  - 由 Linkable::on_accept / on_linkup 创建并以 shared_ptr 返回。
///  - **库强持有**该 shared_ptr，直到连接关闭（on_close 返回之后释放）。
///    因此本对象的所有回调期间，this 一定有效——**不需要** enable_shared_from_this，
///    **不需要**在回调里捕获 self。
///  - 想放弃连接：调用 `c.close()`，不要指望"释放 handler 就能关连接"。
///
/// @par 弱引用的正确位置
///  业务对象（设备模型、会话上下文）应由本 handler 用 weak_ptr 持有：
///  @code
///      std::weak_ptr<Device> m_dev;
///      void on_read(Conn& c, std::error_code ec, Buf buf, std::size_t n) override {
///          auto dev = m_dev.lock();
///          if (!dev) { return; }        // 业务对象没了：不做，也不崩
///          dev->on_frame(buf.data, n);  // 拿到了就正常跑；这里出错就是 bug
///          c.read({nullptr, m_rx, sizeof m_rx});
///      }
///  @endcode
///
/// @par 线程模型
///  - 本接口的所有方法都在该连接所属的 io 线程上执行，**同一实例的调用严格串行**。
///  - 不同连接可能在不同 io 线程上并发执行（跨连接共享状态请自行加锁）。
///  - 回调内**不得阻塞**、**不得抛异常**。
class ConnHandler {
public:
    virtual ~ConnHandler() = default;

    /// 连接就绪。库在接管连接后、投递任何读写事件前调用一次。
    ///
    /// 典型用途：在此时发起第一笔 read。
    ///
    /// @param[in] c 该连接对象。
    /// @return true  初始化成功，库继续驱动该连接；
    ///         false 初始化失败，库立即关闭该连接（此后**不会**有 on_close）。
    ///
    /// @note 此时 `c.is_open()` 为 true。
    /// @note 若本方法里发起 read 后又想拒绝连接，请直接 `c.close()`。
    virtual bool on_open(Conn& c) = 0;

    /// 读完成 / 读归还。
    ///
    /// @param[in] ec  为空表示成功收到数据；非空表示该 buf 归还给你，原因为：
    ///                * `NetErrc::link_closed`  连接断开（主动 close / 对端关闭 / IO 错误）；
    ///                * `NetErrc::keepalive_timeout` 保活超时。
    ///                **只要 ec 非空，整条连接随即进入关闭流程**：其余在途与排队的
    ///                op 会按序各回调一次（ec 非空），随后 on_close。你不需要、也
    ///                不应该在这里再调 c.close()。
    /// @param[in] buf 原样传回你提交的那份 Buf（owner / data / size 均不变）。
    /// @param[in] n   已收到的字节数。成功时 > 0（some 模式 1..size，exact 模式 ==size）；
    ///                失败时通常为 0，也可能是已收到的部分数据。
    ///
    /// @par 保证
    ///  - 每一笔被受理的 read()（返回 true）**恰好**触发一次本回调。
    ///  - 返回 false 的 read() **不会**触发本回调。
    ///  - 本回调返回后，若想继续读必须**显式再次调用** c.read() ——库不会自动续读
    ///    （这是刻意的：用 exact 模式控制帧长时，不允许库自作主张地续读）。
    ///
    /// @par buf 的处置
    ///  - `ec` 为空：buf 里的前 n 字节是收到的数据，你可以随便用。
    ///  - `ec` 非空：数据无用，把 buf 视为已归还（可复用 / 可销毁），无需任何操作。
    ///  - `buf.owner` 会在本回调返回后由库释放它自己那一份引用。
    virtual void on_read(Conn& c, std::error_code ec, Buf buf, std::size_t n) = 0;

    /// 写完成 / 写归还。
    ///
    /// @param[in] ec  为空表示全部写出；非空表示该 buf 归还给你，n 为已写出字节数。
    ///                非空时整条连接随即进入关闭流程（见 on_read 的说明）。
    /// @param[in] buf 原样传回你提交的那份 ConstBuf。
    /// @param[in] n   已写出的字节数。成功时 == buf.size；失败时可能为部分值。
    ///
    /// @par 保证
    ///  - 每一笔被受理的 write()（返回 true）**恰好**触发一次本回调。
    ///  - 多笔 write 的回调顺序与受理顺序**严格一致（FIFO）**：第 N 次回调对应
    ///    第 N 笔已受理的 write，即使中途出错也不会乱序。
    ///  - 连接断开时，所有排队中与在途的 write 都会按序各回调一次（ec 非空），
    ///    **不会**静默丢弃。
    virtual void on_write(Conn& c, std::error_code ec, ConstBuf buf, std::size_t n) = 0;

    /// 连接关闭。本连接的最后一次回调。
    ///
    /// @param[in] c 已关闭的连接（`c.is_open()` 恒为 false）。
    ///
    /// @par 保证
    ///  - 在所有在途 read/write 的完成回调（ec 非空）**之后**调用。
    ///  - 每个被接管的连接**恰好**调用一次。on_open 返回 false 的连接不会调用。
    ///  - 本回调返回后，库释放对本 handler 的强引用，对象可能就此销毁。
    ///    不要在此时把 this 交给别的线程长期持有。
    virtual void on_close(Conn& c) = 0;

    /// 保活检查未通过（可选）。**每次未通过都会回调一次**，不是只在超时时调一次。
    ///
    /// @param[in] info 方向、第几次未通过、配置的上限。
    ///
    /// @par 调用次数
    ///  一次保活超时的全过程会回调 `limit` 次，`info.missed` 从 1 递增到 `limit`；
    ///  **仅在 `missed == limit` 的那次回调返回之后**，库才以
    ///  `NetErrc::keepalive_timeout` 关闭连接。
    ///
    ///  例：`enable_recv_keepalive(0s, 10s, 3)` → 第 10s / 20s / 30s 各回调一次，
    ///  第 3 次回调返回后才断链。用户有前两次（共 20s）的处置窗口。
    ///
    /// @par 计数何时清零
    ///  - 接收侧：任一成功读到的字节都算"有活动"，计数归零，下次从 1 重新开始。
    ///  - 发送侧：任一**成功写出**（不是发起写）都算，同样归零。
    ///
    /// @par 可以做什么
    ///  - `info.has_grace()` 为 true（还有余地）：适合发一个心跳/探测包、记日志、
    ///    更新设备在线状态。发出数据后接收侧的计数不受影响，但发送侧会被刷新。
    ///  - `missed == limit`（最后一次）：可以不处理，等库关闭；也可以主动调用
    ///    `c.close()`，这样会走完整的关闭流程（在途 read/write 按序归还），
    ///    比被保活强断更可控。
    ///
    /// @note 默认实现为空。即使不实现，库也会在 `limit` 次之后正常关闭连接。
    /// @note 线程：在该连接所属的 io 线程上执行，与读写完成回调互斥。
    virtual void on_idle(Conn& c, const IdleInfo& info)
    {
        (void)c;
        (void)info;
    }
};

// =============================================================================
//  连接事件订阅者
// =============================================================================

/// 连接事件的订阅者。由用户实现，注册给 Mechanism（库只持 weak_ptr）。
///
/// @par 生命周期
///  - 用户用 shared_ptr 持有，交给库的是 weak_ptr；订阅者析构后库不再投递事件。
///  - 一个 Mechanism 可以注册多个（按 listen/connect 分别指定），也可以用
///    `set_linkable()` 设一个全局默认的。
///
/// @par 职责边界
///  - 本接口负责"新连接交给谁处理"与"全局连接视图"（连接表、重连调度）。
///  - 单条连接内部的协议状态、缓冲、读写节奏，全部归 ConnHandler。
class Linkable {
public:
    virtual ~Linkable() = default;

    /// 服务端：有新连接接入。
    ///
    /// @param[in] c 刚建立、尚未被任何 handler 驱动的连接。
    /// @return 处理该连接的 handler；返回 nullptr 表示**拒绝**（库立即关闭连接，
    ///         不会产生 on_open / on_close 回调）。
    ///
    /// @par 线程
    ///  - 在该连接所属的 io 线程上调用；不同连接可能并发进入本方法。
    ///
    /// @par handler 的所有权
    ///  - 库接管返回值并强持有到连接关闭。你**不需要**另外保存它。
    ///  - 若业务需要"按连接 id 发数据"，请在本方法里把 `&c` 或 handler 指针
    ///    登记进自己的表，并在 on_linkdown 里摘除。
    /// @par 拒绝的语义
    ///  返回 nullptr 表示这条连接**从未被接管**：库立即关闭它，**不会**回调
    ///  on_linkdown。所以如果你在本方法里已经往自己的表里登记了什么，必须在
    ///  返回 nullptr 之前清理掉，否则会残留。推荐先判断（许可、限流、协议匹配）
    ///  再登记。
    virtual std::shared_ptr<ConnHandler> on_accept(Conn& c)
    {
        (void)c;
        return nullptr;
    }

    /// 客户端：连接建立成功。
    ///
    /// 语义与返回值同 on_accept。与 on_accept 分开是为了让"服务端接入"和
    /// "客户端连上"可以有不同的处理策略（例如客户端连上后先发注册包）。
    virtual std::shared_ptr<ConnHandler> on_linkup(Conn& c)
    {
        (void)c;
        return nullptr;
    }

    /// 连接关闭（服务端 / 客户端统一）。
    ///
    /// @param[in] c 已关闭的连接。
    ///
    /// @par 时序
    ///  - 在 ConnHandler::on_close **之后**、且 handler 已被库释放之后调用。
    ///  - 因此这里拿不到 handler，只能拿到连接对象（id / 地址仍可读）。
    ///
    /// @par 用途
    ///  - 从自己的连接表里摘除、更新设备在线状态、决定是否重连。
    ///
    /// @note 主动 `close()` 的连接同样会走到这里——它与"对端断开"在业务上通常
    ///       没有区别，别试图在这里区分是谁先关的（若确实需要，请自己在 close
    ///       之前打标记）。
    /// @note 被 on_accept 返回 nullptr 拒绝的连接**不会**走到这里（从未被接管）。
    virtual void on_linkdown(Conn& c) { (void)c; }

    /// 客户端：建立连接失败。此时没有 Conn，只有目标地址、原因与上下文。
    ///
    /// @param[in] remote   发起 connect 时给的目标地址。
    /// @param[in] ec       失败原因（超时 / 拒绝连接 / 网络不可达 / 地址非法 ...）。
    /// @param[in] user_ctx 发起 connect 时传入的上下文，原样返回；未传则为空。
    ///
    /// @note 与 connect() 受理成功一一对应：受理后要么 on_linkup，要么本回调。
    /// @note 之所以在这里也带上 user_ctx：并发连接多个设备时，仅凭地址无法区分
    ///       是哪一次发起（同一地址可能被多次连接），context 才是可靠标识。
    /// @note 在发起 connect 的 io 线程上执行，可能与其它连接的回调并发。
    virtual void on_linkfail(const Address&               remote,
                             std::error_code              ec,
                             std::shared_ptr<void>        user_ctx)
    {
        (void)remote;
        (void)ec;
        (void)user_ctx;
    }
};

// =============================================================================
//  连接
// =============================================================================

/// 一条已建立的 TCP 连接。
///
/// @par 获取方式
///  - 只能在 Linkable::on_accept / on_linkup（以及 ConnHandler 的各回调）中拿到。
///  - 用户**不持有** Conn 的所有权，不得 delete，也不要长期保存裸指针——
///    连接关闭后该指针即失效。需要长期引用请记 `id()`。
///
/// @par 线程模型
///  - 所有 public 方法都可以从**任意线程**调用（内部 post 到该连接的 io 线程）。
///  - 所有回调都在该连接所属的 io 线程上串行执行。
///  - 回调内再次调用 read/write/close 是安全的（不会重入破坏状态）。
class Conn {
public:
    // -------------------------------------------------------------------------
    // 读写
    // -------------------------------------------------------------------------

    /// 发起一笔读。同一时刻**只允许一笔**在途。
    ///
    /// @param[in] buf  目标缓冲。库往里写数据，不拷贝。
    /// @param[in] mode some = 收到多少算多少；exact = 收满 buf.size 才完成。
    /// @return true  已受理：库将恰好调用一次 on_read。
    /// @return false **未受理**，不回调，buf 立即归你。原因仅三种：
    ///         1. 连接已关闭；
    ///         2. `buf.data == nullptr` 或 `buf.size == 0`；
    ///         3. **已有一笔读在途**（同一连接同一时刻只允许一笔读）。
    ///
    /// @par 为什么只允许一笔
    ///  多笔读在途时，完成回调无法确定自己对应哪一笔请求：缓冲区尺寸可能不同、
    ///  帧长可能不同，一旦错配就会把 A 的数据当作 B 的帧头解析，且无法恢复。
    ///  只允许一笔在途，等价于让"回调 ↔ 请求"成为一对一，这个歧义从结构上消失。
    ///
    /// @par buffer 生命周期
    ///  - `buf.owner` 非空：库持有它直到 on_read 返回，调用方提前析构也安全。
    ///  - `buf.owner` 为空：调用方必须保证 data 的内存在 on_read 返回前有效。
    ///  - 返回 false 时，库不访问该内存，也不增加 owner 的引用计数。
    bool read(Buf buf, ReadMode mode = ReadMode::some);

    /// 发起一笔写（异步入队，FIFO）。
    ///
    /// @param[in] buf 源数据。提交后**内容不得修改**，直到 on_write 返回。
    /// @return true  已受理：库将恰好调用一次 on_write。
    /// @return false **未受理**，不回调，buf 立即归你。原因：
    ///         * 连接已关闭；
    ///         * `buf.data == nullptr` 或 `buf.size == 0`；
    ///         * 发送队列达到上限（反压，`NetErrc::buffer_full`）——**不会因此断链**。
    ///
    /// @par 顺序保证（契约级，测试据此断言）
    ///  - **库保证**：所有被受理（返回 true）的 write，其数据在 TCP 流上的顺序、
    ///    以及 `on_write` 的完成顺序，与**受理顺序**严格一致；绝不交错、绝不乱序。
    ///  - **受理顺序的定义**：库在**调用方线程**上持该连接的发送队列锁完成
    ///    "容量检查 + 入队"，因此受理顺序 = 锁的获取顺序。这对所有线程都是
    ///    确定且可复现的，**不依赖** io 线程的调度时机。
    ///  - 同一线程连续调用 → 受理顺序 == 调用顺序。
    ///  - `Conn::write` 与 `Mechanism::write(id)` 走同一条队列，混用也一并排序。
    ///  - 一旦某一笔写出错，其后已受理的写仍按同一顺序各回调一次（ec 非空），
    ///    不会静默丢弃、也不会重排。
    ///
    /// @par 为什么必须在调用线程加锁入队（实现约束）
    ///  若改为"只 post 到 io 线程再入队"，`write()` 返回 true 时尚未判断队列
    ///  容量：等 io 线程发现超限，这个已受理的请求只能被丢弃，违反"受理即恰好
    ///  一次回调"；要补救就得让 `write()` 阻塞等 io 线程回话，代价更大。
    ///  锁内完成"检查 + 入队"是唯一能同时保证【同步返回是否受理】与
    ///  【FIFO 顺序确定】的做法。
    ///
    /// @par buffer 生命周期
    ///  同 read：owner 非空则库持有，为空则调用方保证。
    bool write(ConstBuf buf);

    // -------------------------------------------------------------------------
    // 关闭与查询
    // -------------------------------------------------------------------------

    /// 主动关闭连接。幂等，noexcept，可在任意线程或任意回调内调用。
    ///
    /// 行为：
    ///  1. `is_open()` 立即变为 false；
    ///  2. cancel 在途读 / 写 / 保活定时器；
    ///  3. 所有被受理的 read/write 各回调一次（ec = link_closed）；
    ///  4. 投递 on_close()；
    ///  5. 关闭 socket，随后投递 Linkable::on_linkdown()。
    ///
    /// @note 从 on_close 里再次调用 close() 是无害的。
    void close() noexcept;

    /// 连接是否仍然打开。**瞬间快照**，不要用它替代对 read/write 返回值的检查。
    [[nodiscard]] bool is_open() const noexcept;

    /// 连接的唯一 ID（进程内单调递增，不复用）。推荐作为连接表的键。
    [[nodiscard]] std::uint64_t id() const noexcept;

    [[nodiscard]] Address local_address() const noexcept;
    [[nodiscard]] Address remote_address() const noexcept;

    /// 发起 listen / connect 时传入的用户上下文，原样返回。
    ///
    ///  - 客户端：等于 `connect(..., user_ctx)` 传入的那个。
    ///  - 服务端：**继承监听器的上下文**，即 `listen(..., user_ctx)` 传入的那个。
    ///    所以它天然回答了"这条连接属于哪个监听器"。
    ///
    /// @note 库持有该 shared_ptr 直到连接关闭；用它可以把"设备对象 / 监听器句柄"
    ///       安全地挂到连接上，而不必事后按地址反查。
    [[nodiscard]] std::shared_ptr<void> user_context() const noexcept;

    // -------------------------------------------------------------------------
    // 发送队列配额（反压）
    // -------------------------------------------------------------------------

    /// 设置发送队列上限。0 表示该维度不限制。两者是"与"关系：任一超限即拒绝。
    ///
    /// @note 只影响后续 write 的受理判断，不改变已在队列中的请求。
    /// @note 典型用法：在 on_open 里按设备类型设置。
    void set_send_queue_limit(std::size_t bytes, std::size_t count) noexcept;

    // -------------------------------------------------------------------------
    // 保活
    // -------------------------------------------------------------------------

    /// 启用接收侧保活：每隔 @p interval 检查一次"自上次检查以来是否收到过数据"，
    /// 连续 @p max_miss 次都没收到才判定链路已死。
    ///
    /// @param[in] delay    建链后到首次检查的延迟（可为 0）。
    /// @param[in] interval 检查间隔，必须 > 0；否则本调用被静默忽略。
    /// @param[in] max_miss 允许的连续未通过次数，必须 >= 1，默认 3。
    ///
    /// @par 回调与关闭时机（关键）
    ///  每次未通过都会调 `ConnHandler::on_idle`（`missed` = 1..max_miss）；
    ///  **第 max_miss 次回调返回后**，库才以 `NetErrc::keepalive_timeout` 关闭连接。
    ///  即：总判定时长 ≈ `delay + interval * max_miss`，但用户从第一次未通过起
    ///  就拿到 `max_miss - 1` 个 interval 的处置窗口，而不是最后一刻才被告知。
    ///
    /// @par 典型配置
    ///  `enable_recv_keepalive(0ms, 10s, 3)` —— 10s/20s/30s 各回调一次，
    ///  第 3 次之后断链；前 20s 可以发心跳挽救。
    ///
    /// @note 重复调用以新参数重启定时器（计数清零）。
    /// @note 线程安全。
    void enable_recv_keepalive(std::chrono::milliseconds delay,
                               std::chrono::milliseconds interval,
                               std::uint32_t max_miss = 3) noexcept;

    /// 关闭接收侧保活（幂等）。
    void disable_recv_keepalive() noexcept;

    /// 启用发送侧保活：每隔 @p interval 检查一次"自上次检查以来是否成功写出过"，
    /// 连续 @p max_miss 次都没有才判定发送方向停滞。
    ///
    /// @param[in] delay    建链后到首次检查的延迟。
    /// @param[in] interval 检查间隔，必须 > 0。
    /// @param[in] max_miss 允许的连续未通过次数，必须 >= 1，默认 3。
    ///
    /// @par 与接收侧的差异
    ///  "成功写出"指一笔 write 真正写完（`on_write` ec 为空），而不是"发起写"。
    ///  回调与关闭时机同接收侧：共回调 `max_miss` 次，最后一次之后断链。
    void enable_send_keepalive(std::chrono::milliseconds delay,
                               std::chrono::milliseconds interval,
                               std::uint32_t max_miss = 3) noexcept;

    /// 关闭发送侧保活（幂等）。
    void disable_send_keepalive() noexcept;

    ~Conn() = delete;                       ///< 不要试图析构：连接由库管理
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

private:
    Conn() = delete;                        ///< 仅由库内部构造
    friend class Mechanism;
};

// =============================================================================
//  连接结果（同步等待用）
// =============================================================================

/// 一次 connect 的最终结果。
struct ConnectResult {
    std::error_code       ec;     ///< 空表示成功
    std::shared_ptr<Conn> conn;   ///< 成功时非空

    explicit operator bool() const noexcept { return !ec && conn != nullptr; }
};

/// 一次 connect 的等待句柄。可拷贝，可多次等待。
///
/// @par 完成时刻
///  future 就绪 = 三次握手完成 **且** HandlerFactory 已创建 handler
///  **且** `ConnHandler::on_open` 已执行完毕。因此 `conn()` 拿到的连接
///  可以立即 write。
///
/// @par 典型用法：批量并发连接
///  @code
///      std::vector<netmech::ConnectFuture> futs;
///      for (auto& dev : m_devices)
///          futs.push_back(mech->connect(dev.addr,
///              [&dev](netmech::Conn&) { return std::make_shared<ModbusHandler>(dev.self); }));
///
///      for (std::size_t i = 0; i < futs.size(); ++i) {
///          if (!futs[i].wait_for(std::chrono::seconds(1))) { continue; }   // 还没连上
///          if (futs[i].error()) { /* 记失败原因 */ }
///          else                 { m_devices[i].conn = futs[i].conn(); }
///      }
///  @endcode
class ConnectFuture {
public:
    /// 默认构造 = 无效 future（ready() 恒为 false，wait_for 立即返回 false）。
    ConnectFuture() noexcept = default;

    ConnectFuture(const ConnectFuture&) = default;
    ConnectFuture& operator=(const ConnectFuture&) = default;
    ConnectFuture(ConnectFuture&&) noexcept = default;
    ConnectFuture& operator=(ConnectFuture&&) noexcept = default;
    ~ConnectFuture() = default;

    /// 阻塞等待，最多 @p timeout。
    ///
    /// @return true  已完成（无论成功还是失败，用 error() 判断）；
    ///         false 本 future 在时限内未完成——**连接仍在进行**，没有被取消。
    ///
    /// @warning **不得在 io 线程上调用**（会等自己，必然死锁）。实现会检测并
    ///          立即返回 false，Debug 下 assert。
    /// @note 超时只是"本次没等到"，可以稍后再次等待，连接可能已经或即将连上。
    ///       想彻底放弃请自行持有并 close() 目标连接，或调用 Mechanism::close_all()。
    [[nodiscard]] bool wait_for(std::chrono::milliseconds timeout) const noexcept;

    /// 不阻塞地查询是否已完成。
    [[nodiscard]] bool ready() const noexcept;

    /// 取消这次连接（仅在尚未完成时有效）。
    ///
    /// @return true  已成功取消：future 立即就绪，`error() == NetErrc::cancelled`；
    ///         false 连接已完成（成功或失败）、已被取消，或本 future 无效。
    ///
    /// @par 能取消什么、不能取消什么
    ///  - **能**：还在三次握手中的 connect（撤销 socket 与超时定时器）。
    ///  - **不能**：已经建立成功的连接 —— 请用 `conn()->close()` 或
    ///    `Mechanism::close(id)`；也不能取消已提交的 read/write
    ///    （那会破坏"恰好一次回调 + FIFO"契约，需要中止请直接 close）。
    ///
    /// @note 尽力而为：若此刻握手恰好完成，本调用可能返回 false 且连接已建立。
    /// @note 线程安全；可从任意线程调用（不得在 io 线程等待自身）。
    [[nodiscard]] bool cancel() noexcept;

    /// 失败原因。仅在 ready() 为 true 时有效；成功时返回空的 error_code。
    /// 可能值：`NetErrc::not_connected`（握手失败/超时）、`invalid_argument`（地址非法）、
    ///         `handler_rejected`（工厂返回空或 on_open 返回 false）。
    [[nodiscard]] std::error_code error() const;

    /// 成功时的连接对象。仅在 ready() 且无错误时非空。
    ///
    /// @note Conn 本身是强自持的，多持一个 shared_ptr 只是延长已关闭对象的存活，
    ///       不会悬垂；关闭后 is_open() 为 false、read/write 返回 false。
    [[nodiscard]] std::shared_ptr<Conn> conn() const;

private:
    friend class Mechanism;

    ConnectFuture(std::shared_future<ConnectResult> f,
                  std::weak_ptr<Mechanism>        owner,
                  Address                        remote) noexcept
        : m_f(std::move(f))
        , m_owner(std::move(owner))
        , m_remote(remote)
    {
    }

    std::shared_future<ConnectResult> m_f;
    std::weak_ptr<Mechanism>          m_owner;    ///< 仅用于 cancel() 转调
    Address                           m_remote;   ///< cancel_connect 的键
};

// =============================================================================
//  运行时
// =============================================================================

/// 网络运行时。整个进程通常只需要一个实例。
///
/// @par 创建
///  只能通过 `Mechanism::create()`；**必须先调用 netmech::initialize()**。
///  最后一个 shared_ptr 释放时析构，等价于 close_all()（不回收线程池，
///  线程池归 netmech::shutdown() 管）。
///
/// @par 线程安全
///  全部 public 方法可从任意线程调用。listen/connect 并发对同一地址调用时，
///  先到者成功、后到者返回 duplicate_endpoint。
class Mechanism {
public:
    /// 运行时配置。所有字段都有嵌入式友好的默认值。
    ///
    /// @note io 线程数**不在这里**：它是进程级、一次性的，见 netmech::initialize()。
    struct Config {
        std::size_t send_queue_bytes = 2 * 1024 * 1024;  ///< 单连接发送队列字节上限
        std::size_t send_queue_count = 256;              ///< 单连接发送请求个数上限
        std::size_t socket_buf_size  = 256 * 1024;       ///< SO_SNDBUF / SO_RCVBUF，0 = 系统默认
        bool        tcp_nodelay      = true;             ///< 禁用 Nagle（小包低延迟场景）
        bool        linger_zero      = true;             ///< close 时发 RST，避免 TIME_WAIT 堆积
    };

    /// 创建运行时。
    ///
    /// @return 非空实例；**未调用 netmech::initialize() 时返回 nullptr**（Debug 下 assert）。
    /// @note 线程池在 initialize() 里已启动，本函数不创建线程。
    /// @note 仅在内存耗尽 / 线程创建失败时抛 std::system_error（标准库行为）。
    static std::shared_ptr<Mechanism> create(Config cfg = Config{});

    ~Mechanism();

    // -------------------------------------------------------------------------
    // 服务端 / 客户端
    // -------------------------------------------------------------------------

    /// 开始监听。
    ///
    /// @param[in] local    监听地址。端口 0 表示由系统分配（用 local_address_of 查回）。
    /// @param[in] linkable 该监听器的订阅者。为空时退回 set_linkable() 设置的全局订阅者；
    ///                     仍为空则新连接会被立即关闭。
    /// @param[in] backlog  已完成三次握手但尚未 accept 的队列长度。
    /// @return 空 error_code 表示受理成功。
    ///         失败可能为：duplicate_endpoint / invalid_argument / 底层 bind-listen 错误。
    ///
    /// @par 一条监听，多条连接
    ///  本方法只注册一个监听端点，之后每条接入的连接都会回调一次
    ///  `linkable->on_accept(c)`。因此这里**没有** "透传 arg" 参数——
    ///  一条监听面对 N 条连接，arg 无处安放；需要上下文请用 `c.id()` 或
    ///  `c.remote_address()` 在自己的表里查，或者用 @p user_ctx（它会被所有
    ///  accept 出来的连接继承，用来区分"哪个监听器"）。
    ///
    /// @par 健壮性
    ///  accept 遇到临时性错误（如 fd 耗尽、连接被对端复位）后会退避并重新
    ///  accept，监听不会因此永久停止；只有本端主动关闭监听器才会终止循环。
    std::error_code listen(const Address& local,
                           std::weak_ptr<Linkable> linkable,
                           std::uint32_t backlog = 128,
                           std::shared_ptr<void> user_ctx = nullptr);

    /// 异步发起连接。受理后立即返回，结果通过 Linkable 回调通知。
    ///
    /// @param[in] remote     目标地址。
    /// @param[in] linkable   订阅者；为空时退回全局订阅者。
    ///                       成功回调 on_linkup，失败回调 on_linkfail。
    /// @param[in] timeout_ms 连接超时（毫秒），0 = 不设超时（依赖系统 SYN 重传，不建议）。
    /// @param[in] local      本地绑定地址；默认通配 = 由系统选择源端口。
    /// @param[in] user_ctx   用户上下文。connect 是"一对一"，所以这里放上下文是
    ///                       明确的——同一个 Linkable 发起多条连接时，
    ///                       靠 `c.user_context()` 就能区分是哪一条。
    ///                       库持有该引用直到连接关闭。
    /// @return 受理成功返回空 error_code。失败：not_connected / invalid_argument /
    ///         duplicate_endpoint（同 remote 已有在途 connect）/ 底层错误。
    ///
    /// @par 失败语义
    ///  返回非空表示**未受理**：既不回调 on_linkup 也不回调 on_linkfail。
    std::error_code connect(const Address& remote,
                            std::weak_ptr<Linkable> linkable,
                            std::uint32_t timeout_ms = 3000,
                            const Address& local = Address{},
                            std::shared_ptr<void> user_ctx = nullptr);

    /// 连接处理器工厂：握手成功后由库在 io 线程上调用，用返回的 handler 驱动连接。
    /// 返回 nullptr 表示拒绝（连接被关闭，future 以 handler_rejected 结束）。
    using HandlerFactory = std::function<std::shared_ptr<ConnHandler>(Conn&)>;

    /// 发起连接并返回等待句柄（同步 / 批量并发用）。
    ///
    /// 与上一个重载的区别：结果不从 Linkable 回调来，而是从 ConnectFuture 取，
    /// 且 handler 由 @p make_handler 现场创建，不需要实现 Linkable。
    ///
    /// @param[in] remote                目标地址。
    /// @param[in] make_handler          handler 工厂（在 io 线程上调用）。
    /// @param[in] handshake_timeout_ms  三次握手超时（毫秒），0 = 不设超时。
    /// @param[in] local                 本地绑定地址。
    /// @param[in] user_ctx              用户上下文，可从 `c.user_context()` 取回。
    /// @return 有效的 ConnectFuture。**本函数不阻塞**，不会因为超时而等待。
    ///
    /// @note 返回的 future 若被丢弃，连接仍会照常建立并由 handler 驱动；
    ///       只是调用方拿不到 `shared_ptr<Conn>`（handler 里仍有 `Conn*`）。
    /// @note 想"发起 N 条、统一等待"就用这个重载：先批量拿到 future，再逐个
    ///       `wait_for()`，避免串行 connect 把总耗时拉成 N × 超时。
    [[nodiscard]] ConnectFuture connect(const Address& remote,
                                        HandlerFactory make_handler,
                                        std::uint32_t handshake_timeout_ms = 3000,
                                        const Address& local = Address{},
                                        std::shared_ptr<void> user_ctx = nullptr);


    // -------------------------------------------------------------------------
    // 按连接 ID 操作（业务侧无需保存 Conn*）
    // -------------------------------------------------------------------------

    /// 向指定连接发一笔写。语义与 `Conn::write` **完全一致**（FIFO、反压、owner 规则）。
    ///
    /// @param[in] conn_id 连接 ID（`Conn::id()`，或 future 拿到的 conn->id()）。
    /// @return false = 未受理：连接不存在（已断开）/ 已关闭 / 队列满 / 参数非法。
    ///         与 `Conn::write` 的 false 语义相同，**都不会断链**。
    ///
    /// @par 为什么需要它
    ///  业务侧常常只有"设备表号"，没有 Conn 对象。用本接口可以避免自己维护
    ///  `id → Conn*` 表、以及在 on_linkdown 里摘除时的 race 窗口。
    ///
    /// @par 顺序保证
    ///  - 与 `Conn::write` **同一条队列、同一把锁**：受理顺序 = 锁获取顺序，
    ///    对所有线程确定且可复现。
    ///  - 同一线程连续调用 → 受理顺序 == 调用顺序。
    ///  - 不同线程并发调用 → 由锁序列化，不存在乱序或交错；先拿到锁的那次
    ///    排在前面。这与 `Conn::write` 的契约完全一致，两条入口可以混用。
    ///
    /// @note 实现是一次哈希查找（~20 ns）+ 一次发送队列加锁，相对于一次系统
    ///       调用可忽略。
    /// @note 跨线程安全。
    bool write(std::uint64_t conn_id, ConstBuf buf);

    /// 关闭指定连接。等价于找到该连接后调用 `Conn::close()`。
    /// @return false = 该 id 不存在（已经关闭或从未存在）。
    bool close(std::uint64_t conn_id) noexcept;

    /// 取消一次**尚未完成**（还在三次握手中）的连接。
    ///
    /// @param[in] remote 发起 connect 时给的目标地址（也是它在途表的键）。
    /// @return true  已取消。异步 connect 会回调 `on_linkfail(remote, cancelled)`；
    ///               future 版立即就绪且 `error() == NetErrc::cancelled`。
    ///         false 该 remote 没有在途连接（已成功 / 已失败 / 从未发起）。
    ///
    /// @par 只覆盖握手中这一种情况
    ///  - 已建立的连接请用 `close(id)` 或 `conn->close()`。
    ///  - **不提供** read/write 的取消：底层取消只能做到整条连接粒度，无法只撤销
    ///    一笔；而 `read_exact` 半途撤销会丢掉已读数据。要中止读写，`close()` 就是
    ///    答案（它会走完整的归还流程）。
    ///
    /// @note 线程安全；不得在 io 线程等待自身。
    [[nodiscard]] bool cancel_connect(const Address& remote) noexcept;

    // -------------------------------------------------------------------------
    // 生命周期与查询
    // -------------------------------------------------------------------------

    /// 关闭全部监听器与连接。**不回收线程池**，调用后仍可再次 listen/connect。
    ///
    /// 在途 connect 被取消且**不**回调 on_linkfail（属于主动放弃）。
    /// 不阻塞：关闭是异步收敛的，需要等待请在 on_linkdown 里计数。
    void close_all() noexcept;

    /// 运行时是否处于可工作状态（close_all() 之后仍为 true）。
    [[nodiscard]] bool running() const noexcept;

    /// 设置全局订阅者，对之后所有未显式传入 linkable 的 listen/connect 生效。
    void set_linkable(std::weak_ptr<Linkable> linkable);

    /// 当前活跃连接数（快照）。
    [[nodiscard]] std::size_t endpoint_count() const noexcept;

    /// 实际使用的 io 线程数（进程级，对所有 Mechanism 相同）。
    [[nodiscard]] std::size_t io_threads() const noexcept;

    /// 查询某个监听地址实际绑定到的地址（解决 listen 传端口 0 的情况）。
    /// 用调用 listen 时的**原地址**作为查询键；未监听该地址返回 nullopt。
    [[nodiscard]] std::optional<Address> local_address_of(const Address& local) const;

private:
    Mechanism() = delete;
};

} // namespace netmech


// =============================================================================
//  使用示例（伪代码，仅示意所有权与调用顺序）
// =============================================================================
//
//  int main() {
//      if (!netmech::initialize(4)) { /* 已经初始化过，检查配置是否一致 */ }
//
//      auto mech = netmech::Mechanism::create();
//      if (!mech) { /* 忘了 initialize() */ }
//
//      auto mgr = std::make_shared<DeviceManager>();      // 实现 Linkable
//      mech->listen(netmech::Address::any(9000), mgr);
//
//      // 客户端：批量并发连接，先全部发起，再逐个等（不要串行 connect）
//      std::vector<netmech::ConnectFuture> futs;
//      for (auto& d : m_devices) {
//          futs.push_back(mech->connect(
//              d.addr,
//              [d](netmech::Conn&) { return std::make_shared<ModbusHandler>(d.self); },
//              3000, {}, d.self));            // d.self 即 user_ctx，回调里 c.user_context() 可取回
//      }
//      for (std::size_t i = 0; i < futs.size(); ++i) {
//          if (!futs[i].wait_for(std::chrono::milliseconds(500))) { continue; }  // 还没连上
//          if (futs[i].error()) { m_devices[i].mark_offline(futs[i].error()); }
//          else                 { m_devices[i].conn = futs[i].conn(); }
//      }
//
//      // ... 业务运行 ...
//
//      mech->close_all();
//      mech.reset();
//      netmech::shutdown();
//  }
//
//  class ModbusHandler : public netmech::ConnHandler {
//  public:
//      explicit ModbusHandler(std::weak_ptr<Device> dev) : m_dev(std::move(dev))
//      {
//          m_rx = std::make_shared<std::array<std::uint8_t, 260>>();   // 按连接复用
//      }
//
//      bool on_open(netmech::Conn& c) override
//      {
//          m_conn = &c;                                    // 裸指针：on_close 后不再回调
//          return c.read({m_rx, m_rx->data(), m_rx->size()}, netmech::ReadMode::some);
//      }
//
//      void on_read(netmech::Conn& c, std::error_code ec,
//                   netmech::Buf buf, std::size_t n) override
//      {
//          if (ec) { return; }                             // buf 已归还，无需处理
//          auto dev = m_dev.lock();
//          if (!dev) { return; }                           // 业务对象不在：不做
//          dev->on_frame(static_cast<const std::uint8_t*>(buf.data), n);
//          c.read({m_rx, m_rx->data(), m_rx->size()});     // 驱动下一轮
//      }
//
//      void on_write(netmech::Conn& c, std::error_code ec,
//                    netmech::ConstBuf buf, std::size_t n) override
//      {
//          if (!ec) { m_inflight -= n; }
//      }
//
//      void on_close(netmech::Conn&) override { m_conn = nullptr; }
//
//  private:
//      std::weak_ptr<Device>                                    m_dev;
//      std::shared_ptr<std::array<std::uint8_t, 260>>           m_rx;
//      netmech::Conn*                                           m_conn = nullptr;
//      std::size_t                                              m_inflight = 0;
//  };
//
//  class DeviceManager : public netmech::Linkable {
//  public:
//      std::shared_ptr<netmech::ConnHandler> on_accept(netmech::Conn& c) override
//      {
//          auto dev = std::make_shared<Device>(c.id(), c.remote_address());
//          auto h   = std::make_shared<ModbusHandler>(dev);
//          { std::lock_guard lk(m_mtx); m_devices[c.id()] = dev; }
//          return h;                                       // 库接管，无需另存
//      }
//      void on_linkdown(netmech::Conn& c) override
//      {
//          std::lock_guard lk(m_mtx); m_devices.erase(c.id());
//      }
//      void on_linkfail(const netmech::Address& r, std::error_code ec) override { /* 重连调度 */ }
//  private:
//      std::mutex m_mtx;
//      std::unordered_map<std::uint64_t, std::shared_ptr<Device>> m_devices;
//  };
