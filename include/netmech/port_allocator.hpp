// =============================================================================
//  netmech - 端口分配器（给定区间，探测可用端口，计数器前进 + 回绕）
// =============================================================================
//  文件    : netmech/port_allocator.hpp
//  用途    : 媒体面（RTP/RTCP、组播等）常见的"从固定区间里挑端口"的需求。
//  依赖    : 标准库（实现里用 asio 探测，但那在 src 里，公共头不引）。
//  编码    : UTF-8
//
//  ★ 和"让系统分配（bind 端口 0）"的区别
//    · 系统分配：拿到的端口是内核临时端口，通常落在 32768+，防火墙/ACL
//      往往只放行了固定区间（比如 20000-20100）。媒体面通常需要"我告诉你
//      我从哪一段里挑"。
//    · 本分配器：在 [begin, end] 里按**递增游标**找可用端口，找到就前进游标；
//      走到头回绕。刚释放的端口不会立刻被重新选中（游标已经往前走了），
//      这对"关掉再开"频繁发生的媒体面很关键。
//
//  ★ 探测 ≠ 占位（重要）
//    本类是**探测**：试 bind 一下看能不能用，然后立刻释放，把端口号交给你。
//    真正的占用发生在你 bind 的时候。中间有一个极小的窗口可能被别人抢走
//    （尤其同进程内多线程同时申请，或者别的进程正好也在抢）。
//    所以**调用方必须在 bind 失败时重试**：再 acquire 一次或换下一个。
//    想要"探测即占用"的话，得把 socket 一起交出（API 会复杂很多），本类不做。
// =============================================================================
#pragma once


#include "netmech/export.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace netmech {

/// 端口分配器：线程安全。
///
/// @par 典型用法
/// @code
///     // 从 20000 到 20100 里挑一对连续端口（RTP 偶数 / RTCP 紧随其后）
///     netmech::PortAllocator alloc(20000, 20100);
///     for (int attempt = 0; attempt < 8; ++attempt) {
///         auto first = alloc.acquire(/*count=*/2, /*alignment=*/2);
///         if (!first) break;                      // 区间里没有可用配对了
///         if (bind_pair(*first)) break;           // 真正绑上了才算数
///     }
/// @endcode
class NETMECH_API PortAllocator {
public:
    /// @param[in] begin 区间起点（含）
    /// @param[in] end   区间终点（含）。begin > end 视为空区间（acquire 恒返回 nullopt）
    PortAllocator(std::uint16_t begin, std::uint16_t end);

    ~PortAllocator();
    PortAllocator(const PortAllocator&)            = delete;
    PortAllocator& operator=(const PortAllocator&) = delete;

    /// 申请 `count` 个**连续**的可用端口。
    ///
    /// @param[in] count     要几个（RTP+RTCP 传 2）
    /// @param[in] alignment 首端口必须能被它整除（RTP/RTCP 配对传 2 ⇒ 首端口为偶数）。
    ///                      传 0 视为 1。
    /// @return 首个端口号；区间里找不到合适的就返回 `nullopt`（**不会**去区间外找）。
    ///
    /// @note 成功后游标前移到"本组端口之后"；走到尾自动回绕到起点。
    /// @note 线程安全。
    [[nodiscard]] std::optional<std::uint16_t> acquire(std::uint16_t count     = 1,
                                                       std::uint16_t alignment = 1) noexcept;

    /// 归还一个端口（可选）。
    ///
    /// @param[in] first 之前 acquire 返回的首端口
    /// @param[in] count 那一组的个数
    ///
    /// @note 归还**不会**把游标退回去（避免"刚关就立刻被同一连接抢回"的抖动）。
    ///       它只是把端口重新标记为"可用区间的候选"，等价于"下次扫到它时允许选中"。
    ///       本类不维护引用计数 —— 重复归还无副作用。
    void release(std::uint16_t first, std::uint16_t count = 1) noexcept;

    /// 游标当前所在（下一次扫描的起点）。诊断用。
    [[nodiscard]] std::uint16_t cursor() const noexcept;

    [[nodiscard]] std::uint16_t begin() const noexcept;
    [[nodiscard]] std::uint16_t end() const noexcept;

    struct Impl;

private:
    std::shared_ptr<Impl> m_impl;
};

} // namespace netmech
