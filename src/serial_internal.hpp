// =============================================================================
//  serial_internal.hpp —— 串口内部测试注入点（不安装）
// =============================================================================
//  公共头 netmech/serial.hpp 不暴露内部类型；本头只给 src/serial.cpp
//  与单元测试（STATIC 构建）用。
// =============================================================================
#pragma once

namespace netmech {
namespace detail {

/// 测试故障注入点（**仅供单元测试**，生产代码不得设置）。
///
/// 非空时 enqueue_write 在把 do_write 投递到 io 线程**之前**调用它一次
/// （仅 need_start 路径）；抛异常即模拟"投递失败（内存不足）"，用于回归
/// 并发写的回滚路径（见 test_serial.cpp 的
/// serial_write_post_failure_does_not_strand_concurrent_writes）。
/// 默认 nullptr：每笔需要发起写的受理多一次空指针检查，开销可忽略。
extern void (*serial_post_fault_hook)();

} // namespace detail
} // namespace netmech