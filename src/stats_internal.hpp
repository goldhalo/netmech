// 热路径用的内部计数入口（公共头只暴露采集接口）。
#pragma once

#include <cstdint>

namespace netmech {
namespace detail {

void stats_on_close() noexcept;
void stats_on_read(std::uint64_t bytes) noexcept;
void stats_on_write(std::uint64_t bytes) noexcept;

} // namespace detail
} // namespace netmech
