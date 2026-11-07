// =============================================================================
//  netmech - 可观测性钩子（默认关闭，极低成本）
// =============================================================================
//  默认**不采集**：热路径只多一次 relaxed 原子读（enabled 标志）。
//  调 `set_io_stats_enabled(true)` 后 `collect_io_stats()` 即可拿到累计值；
//  生产环境可只在排障窗口打开，或定期采样后关闭。
// =============================================================================
#pragma once

#include <cstdint>

namespace netmech {

/// io 累计统计（进程级；多 io 线程并发安全）。
struct IoStats {
    std::uint64_t connections_closed = 0;  ///< 关闭的连接数
    std::uint64_t bytes_read = 0;          ///< 成功读到的字节数
    std::uint64_t bytes_written = 0;       ///< 成功写出的字节数
    std::uint64_t packet_reads = 0;        ///< 读完成回调次数
    std::uint64_t packet_writes = 0;       ///< 写完成回调次数
};

/// 是否开启采集（默认 false）。可运行时切换。
void set_io_stats_enabled(bool on) noexcept;
bool io_stats_enabled() noexcept;

/// 读取累计值（不重置）。
[[nodiscard]] IoStats collect_io_stats() noexcept;

/// 清零累计值。
void reset_io_stats() noexcept;

} // namespace netmech
