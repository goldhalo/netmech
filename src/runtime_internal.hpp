// =============================================================================
//  runtime_internal.hpp —— Runtime 与 IoPool 之间的内部桥（不安装）
// =============================================================================
//  公共头 netmech/runtime.hpp 不能出现 IoPool（那是 src/ 下的内部类型），
//  但 Mechanism / SerialPort / UdpEndpoint 的实现需要拿到 Runtime 里的池。
//  这两个访问器只给 src/ 下的 .cpp 用。
// =============================================================================
#pragma once

#include <memory>

namespace netmech {

class Runtime;

class IoPool;

namespace detail {

/// 取出 Runtime 持有的 IoPool（= Runtime::pool_ptr() 的引用包装）。
/// @p rt 必须非空。
[[nodiscard]] IoPool& runtime_pool(Runtime& rt) noexcept;

/// 进程级默认运行时的**非拥有** shared_ptr（no-op deleter）：
/// 默认运行时是进程级的，交给库内对象持有时不应延长/缩短它的生命周期。
[[nodiscard]] std::shared_ptr<Runtime> default_runtime_handle() noexcept;

} // namespace detail
} // namespace netmech
