// =============================================================================
//  NetMechanism - 单连接协议处理器
// =============================================================================
//  文件    : netmech/iohandler.hpp
//  用途    : 定义 ConnHandler —— 每条连接一个实例，负责处理该连接上的
//            建立 / 读完成 / 写完成 / 关闭 / 保活事件。
//  依赖    : 标准库 + netmech/common.hpp（Buf / ConstBuf / NetErrc）。
//  编码    : UTF-8
//
//  ★ 设计要点
//  ----------
//  * 读写完成**回调到本接口的虚函数**，调用方不需要传 lambda。
//    因此 handler 内部不需要 `enable_shared_from_this`，也不用捕获 this。
//  * 每笔 read/write 的缓冲区随完成回调**原样带回**，调用方据此知道是哪一块内存。
//  * `on_close` 明确表示"连接已结束"，与"库在初始化你"是两件事。
//
//  ★ 生命周期
//  ----------
//  * 由 Linkable::on_accept / on_linkup 创建并以 shared_ptr 交出；
//    **库强持有**到连接关闭，因此回调期间 this 一定有效。
//  * 想放弃连接请调用 `ep->close()`，不要指望"释放 handler 就能关连接"。
//
//  ★ 线程模型
//  ----------
//  * 本接口的全部方法都在该连接所在的 io 线程上执行，同一实例严格串行。
//  * 不同连接可能在不同 io 线程上并发（跨连接共享状态请自行加锁）。
//  * 回调内不得阻塞、不得抛异常。
// =============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <system_error>

#include "netmech/common.hpp"

namespace netmech {

class Conn;

// =============================================================================
//  读模式
// =============================================================================

/// 读模式。
enum class ReadMode {
    some,    ///< 收到至少 1 字节即完成：on_read 成功时 n <= 请求长度
    exact,   ///< 收满请求长度才完成：on_read 成功时 n == 请求长度（定长协议用）
};

// =============================================================================
//  保活信息
// =============================================================================

/// 保活方向。
enum class IdleKind {
    recv,   ///< 接收侧：连续若干次检查都没收到任何字节
    send,   ///< 发送侧：连续若干次检查都没有任何成功写出
};

/// 一次保活检查未通过时的信息。
struct IdleInfo {
    IdleKind      kind{IdleKind::recv};
    std::uint32_t missed = 0;   ///< 连续第几次未通过，从 1 开始
    std::uint32_t limit  = 0;   ///< 配置的 max_miss；missed == limit 时库随后关闭连接

    /// 还有没有挽回余地：`missed < limit` 为 true（此时库不会关闭连接）。
    [[nodiscard]] bool has_grace() const noexcept { return missed < limit; }
};

// =============================================================================
//  协议处理器
// =============================================================================

/// 一条 TCP 连接的协议处理器。
///
/// @par 弱引用的正确位置
///  业务对象（设备模型、会话上下文）应由本 handler 用 weak_ptr 持有：
///  @code
///      std::weak_ptr<Device> m_dev;
///      void on_read(std::error_code ec, netmech::Buf buf, std::size_t n) override {
///          if (ec) { return; }              // buf 已归还
///          auto dev = m_dev.lock();
///          if (!dev) { return; }            // 业务对象没了：不做，也不崩
///          dev->on_frame(buf.data, n);
///          m_ep->read({nullptr, m_rx, sizeof m_rx});
///      }
///  @endcode
class ConnHandler {
public:
    virtual ~ConnHandler() = default;

    /// 连接就绪。库在接管连接后、投递任何读写事件前调用一次。
    ///
    /// @param[in] ep 该连接对象（shared_ptr，回调期间保证有效）。
    /// @return true  初始化成功，库继续驱动该连接；
    ///         false 初始化失败，库立即关闭该连接（此后**不会**有 on_close）。
    ///
    /// @note 在此时发起第一笔读是典型用法。
    ///
    /// @warning **必须保持一笔读在途**：TCP 的 FIN 只有在下一次 read 时才会被感知。
    ///          如果 on_open 里不发起读、之后也不再读，对端关闭时连接不会被发现，
    ///          会一直静默挂着（直到保活超时或本端主动 close）。
    ///          这是 TCP 的固有行为，不是库的缺陷。
    virtual bool on_open(std::shared_ptr<Conn> ep) = 0;

    /// 读完成 / 读归还。
    ///
    /// @param[in] ec  为空表示成功收到数据；非空表示该 buf 归还，原因为：
    ///                `NetErrc::link_closed`（断开）或 `keepalive_timeout`。
    ///                **只要 ec 非空，整条连接随即进入关闭流程**。
    /// @param[in] buf 原样传回调用方提交的那份 Buf（owner / data / size 均不变）。
    /// @param[in] n   已收到字节数。成功时 > 0；失败时通常为 0，也可能是部分数据。
    ///
    /// @par 保证
    ///  - 每一笔被受理的 read()（返回 true）**恰好**触发一次本回调。
    ///  - 返回 false 的 read() **不会**触发本回调。
    ///  - 本回调返回后，若想继续读必须**显式再次调用** read() ——库不会自动续读。
    virtual void on_read(std::error_code ec, Buf buf, std::size_t n) = 0;

    /// 写完成 / 写归还。
    ///
    /// @param[in] ec  为空表示全部写出；非空表示该 buf 归还，n 为已写出字节数。
    /// @param[in] buf 原样传回调用方提交的那份 ConstBuf。
    /// @param[in] n   已写出字节数。成功时 == buf.size；失败时可能为部分值。
    ///
    /// @par 顺序保证
    ///  多笔 write 的回调顺序与受理顺序严格一致（FIFO），出错时剩余请求也会
    ///  按序各回调一次（ec 非空），不会静默丢弃。
    virtual void on_write(std::error_code ec, ConstBuf buf, std::size_t n) = 0;

    /// 连接关闭。本连接的最后一次回调。
    ///
    /// @par 保证
    ///  - 在所有在途 read/write 的完成回调（ec 非空）**之后**调用。
    ///  - 每个被接管的连接**恰好**调用一次；on_open 返回 false 的连接不调用。
    ///  - 本回调返回后，库释放对本 handler 的强引用，对象可能就此销毁。
    virtual void on_close() = 0;

    /// 保活检查未通过（可选）。**每次未通过都会回调一次**，不是只在超时时调一次。
    ///
    /// 一次保活超时的全过程会回调 `limit` 次，`info.missed` 从 1 递增到 `limit`；
    /// **仅在第 `limit` 次回调返回之后**，库才以 `keepalive_timeout` 关闭连接。
    ///
    /// 例：`enable_recv_keepalive(0ms, 10s, 3)` → 第 10s / 20s / 30s 各回调一次，
    /// 第 3 次之后断链；前两次共 20s 是处置窗口（发心跳、记日志、主动 close）。
    virtual void on_idle(const IdleInfo& info) { (void)info; }
};

} // namespace netmech
