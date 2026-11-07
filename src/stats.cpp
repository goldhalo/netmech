// =============================================================================
//  stats.cpp —— 可观测性计数器实现
// =============================================================================
//  计数器全是 relaxed 原子；采集关闭时热路径只做一次 relaxed load。
// =============================================================================
#include "netmech_srcpch.hpp"

#include "netmech/stats.hpp"

#include "stats_internal.hpp"

#include <atomic>

namespace netmech {

namespace {

std::atomic<bool>          g_enabled{false};
std::atomic<std::uint64_t> g_closed{0};
std::atomic<std::uint64_t> g_bytes_read{0};
std::atomic<std::uint64_t> g_bytes_written{0};
std::atomic<std::uint64_t> g_packet_reads{0};
std::atomic<std::uint64_t> g_packet_writes{0};

inline bool on() noexcept
{
    return g_enabled.load(std::memory_order_relaxed);
}

} // namespace

void set_io_stats_enabled(bool on) noexcept
{
    g_enabled.store(on, std::memory_order_relaxed);
}

bool io_stats_enabled() noexcept
{
    return on();
}

IoStats collect_io_stats() noexcept
{
    IoStats s;
    s.connections_closed = g_closed.load(std::memory_order_relaxed);
    s.bytes_read         = g_bytes_read.load(std::memory_order_relaxed);
    s.bytes_written      = g_bytes_written.load(std::memory_order_relaxed);
    s.packet_reads       = g_packet_reads.load(std::memory_order_relaxed);
    s.packet_writes      = g_packet_writes.load(std::memory_order_relaxed);
    return s;
}

void reset_io_stats() noexcept
{
    g_closed.store(0, std::memory_order_relaxed);
    g_bytes_read.store(0, std::memory_order_relaxed);
    g_bytes_written.store(0, std::memory_order_relaxed);
    g_packet_reads.store(0, std::memory_order_relaxed);
    g_packet_writes.store(0, std::memory_order_relaxed);
}

namespace detail {

void stats_on_close() noexcept
{
    if (on()) {
        g_closed.fetch_add(1, std::memory_order_relaxed);
    }
}

void stats_on_read(std::uint64_t bytes) noexcept
{
    if (on()) {
        g_bytes_read.fetch_add(bytes, std::memory_order_relaxed);
        g_packet_reads.fetch_add(1, std::memory_order_relaxed);
    }
}

void stats_on_write(std::uint64_t bytes) noexcept
{
    if (on()) {
        g_bytes_written.fetch_add(bytes, std::memory_order_relaxed);
        g_packet_writes.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace detail
} // namespace netmech
