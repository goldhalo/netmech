// =============================================================================
//  NetMechanism - 连接事件订阅者
// =============================================================================
//  文件    : netmech/linkable.hpp
//  用途    : 定义 Linkable 抽象接口。调用方实现本接口，并以 std::weak_ptr 的
//            形式交给 Mechanism（全局单一订阅者）或单次 listen/connect。
//  依赖    : Boost 1.74 + 标准库。不含 <iostream>。
//  编码    : UTF-8
//
//  ★ 与旧 IAsioLinkable 的关键差异
//  -------------------------------
//  * 旧接口用裸指针 `IAsioEndPoint*` + `IAsioProtocolHandler*&`，连接对象随时
//    可能被内部释放，回调里保留指针即悬垂。
//  * 新接口一律 `std::shared_ptr<Conn>`，**按值传入**：回调期间对象保证
//    存活；若调用方想延长生命周期（例如交给业务线程慢慢处理），把该
//    shared_ptr 存起来即可，库不会因此泄漏（Conn 由 socket 关闭后的
//    内部状态机自行收敛）。
//  * 订阅关系用 `std::weak_ptr<Linkable>`：订阅者析构后库自动停止投递，
//    不会访问已销毁对象。
//
//  ★ 线程模型（重要）
//  -----------------
//  * on_accept / on_linkup / on_linkdown 都在**该 Conn 所在的 io 线程**上
//    串行执行，因此同一连接的事件绝不会并发。
//    ★ 机制：`IoPool` 保证每个 io_context 恰好由 **1 条线程** `run()`，而每个
//      连接在创建时绑定其中一个 io_context —— 本库**不使用 strand**。
//  * 不同连接的回调**可能**在不同 io 线程上并发执行。若 on_* 内部访问多个
//    连接共享的状态，**调用方需自行加锁**。
//  * on_linkfail 在发起 connect 的那个 io 线程上执行（此时尚未建立连接，
//    不存在对应的 Conn）。
//  * 回调**不得阻塞**：阻塞会拖住该 io_context 上的所有连接。耗时工作请把
//    shared_ptr<Conn> 投递到业务线程池。
//  * 回调**不得抛出异常**。库会在回调外层包 try/catch 兜底并把异常吞掉并
//    （尽力）转成 on_linkdown，但依赖该兜底属于设计缺陷，请勿依赖。
// =============================================================================
#pragma once

#include <memory>

#include <asio/ip/tcp.hpp>

#include "netmech/common.hpp"

namespace netmech {

class Conn;          ///< 一条已建立的连接（完整定义见 netmech/conn.hpp）
class ConnHandler;   ///< 协议处理器（完整定义见 netmech/iohandler.hpp）

/// 连接事件的订阅者。
///
/// 用途：由业务层实现，用于接管新建立的连接（服务端 accept / 客户端 linkup），
/// 并感知连接断开与连接失败。
///
/// @par 生命周期
///  - 订阅者由调用方用 shared_ptr 持有，交给库的是 weak_ptr。订阅者析构后，
///    库不再投递任何事件（并会跳过该订阅者，不产生空悬调用）。
///  - @c Mechanism::set_linkable() 设置的是**全局单一订阅者**，会被
///    listen()/connect() 未显式给出 linkable 的调用使用。
///
/// @par 线程安全性
///  见文件头"线程模型"。单个连接的 on_* 串行；跨连接可能并发。
///
/// @par 返回值语义
///  - on_accept / on_linkup 通过**出参** @p out 决定是否接管该连接：
///      * 置为非空 shared_ptr<ConnHandler>  -> 接受连接，库将对该 handler 调用
///        on_open()，随后由 handler 驱动读写；
///      * 保持为空                        -> **拒绝该连接**，库会立即关闭
///        （服务端）/ 视为连接失败（客户端），不再投递后续事件。
class Linkable {
public:
    /// 虚析构函数。允许通过 std::shared_ptr<Linkable> 正确析构派生类。
    virtual ~Linkable() = default;

    /// 服务端接受了一个新连接时的回调。
    ///
    /// @param[in] ep  新建立的连接对象。**按值传入**，回调期间保证有效；
    ///                若需在回调返回后继续使用，请保存该 shared_ptr。
    ///                `c.is_open()` 在此刻为 true。
    /// @return 协议处理器。
    ///         * 非空 -> 库接管该连接，随后调用 `handler->on_open(c)`；
    ///         * 空   -> 视为**拒绝**，库立刻关闭该连接（不给 on_linkdown
    ///           回调，因为没有"连接成功"过）。
    /// @warning 不要返回已被其它连接占用的 handler 实例。
    ///
    /// @par 前置条件
    ///  - 仅当 Mechanism::listen() 受理成功且底层 accept 成功时调用。
    ///
    /// @par 线程安全性
    ///  - 在 @p ep 所在的 io 线程上执行；同一连接串行，跨连接可能并发。
    ///  - 回调内**不要**调用 `ep->close()` 后仍期望 on_linkdown 被触发；
    ///    主动 close() 会以 `operation_aborted`-类错误结束挂起操作。
    ///
    /// @par 异常
    ///  - 不应抛出。库包 try/catch；一旦抛出，该连接按"拒绝"处理。
    virtual std::shared_ptr<ConnHandler> on_accept(Conn& c)
    {
        (void)c;
        return nullptr;
    }

    /// 客户端连接建立成功（connect 完成、握手无需额外步骤）时的回调。
    ///
    /// @param[in] ep  已连接的 Conn，按值传入，语义同 on_accept。
    /// @param[out] out 协议处理器出口；置空表示拒绝（此时库主动关闭连接，
    ///                 并**会**随后投递 on_linkdown，因为连接已建立过）。
    ///
    /// @par 前置条件
    ///  - Mechanism::connect() 在 timeout_ms 内成功完成三次握手。
    ///
    /// @par 线程安全性
    ///  - 在 @p ep 所在的 io 线程上执行。
    ///
    /// @par 异常
    ///  - 不应抛出；库兜底同 on_accept。
    virtual std::shared_ptr<ConnHandler> on_linkup(Conn& c)
    {
        (void)c;
        return nullptr;
    }

    /// 连接断开（含对端关闭、本端 close()、IO 错误）时的回调。
    ///
    /// @param[in] ep 已（或正在）关闭的连接。可能正是调用方在 on_accept/
    ///                on_linkup 中保存的那个 shared_ptr，也可能是另一个
    ///                副本；用 `id()` 比较身份，**不要**用指针地址。
    ///
    /// @par 保证
    ///  - 对每个被接管的连接**恰好调用一次**（拒绝的连接不调用）。
    ///  - 在 @p ep 所在的 io 线程上执行。
    ///  - 在此回调中 `ep->is_open()` 恒为 false。
    ///
    /// @par 异常
    ///  - 不应抛出；库包 try/catch 并吞掉异常以保证不 terminate。
    virtual void on_linkdown(Conn& c)
    {
        (void)c;
    }

    /// 客户端连接失败（超时、拒绝连接、网络不可达等）时的回调。
    ///
    /// @param[in] remote 连接的目标地址（调用 connect 时传入的 remote）。
    /// @param[in] ec     失败原因。若为超时，错误码来自 steady_timer 的
    ///                   `asio::error::timed_out`；可用
    ///                   `from_boost_error_code(ec)` 归类。
    ///
    /// @par 保证
    ///  - 与 connect() 一对一：一次成功的 connect 调用若失败则恰好回调一次。
    ///  - 此时**没有** Conn 被创建，故签名中不含 Conn。
    ///
    /// @par 线程安全性
    ///  - 在发起 connect 的 io 线程上执行，可能是任意一条 io 线程，
    ///    与其它连接的无锁回调**可能并发**。
    ///
    /// @par 异常
    ///  - 不应抛出；库兜底吞掉。
    virtual void on_linkfail(const Address& remote, std::error_code ec)
    {
        (void)remote;
        (void)ec;
    }
};

} // namespace netmech
