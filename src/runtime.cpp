// =============================================================================
//  NetMechanism - 进程级运行时入口
// =============================================================================
//  文件    : runtime.cpp
//  用途    : 实现 netmech::initialize() / netmech::shutdown()。
//  编码    : UTF-8
//
//  为什么单独一个文件
//  ------------------
//  `initialize` / `shutdown` 是对外契约（声明在 common.hpp），但它们背后是
//  IoPool —— 那是**实现层**的东西，公共头不应该看到能。所以：
//      common.hpp  只声明两个函数
//      runtime.cpp 引用 IoPool 并把两个函数接上
//  这样 IoPool 可以继续演进（甚至将来整体替换），对外契约不变。
// =============================================================================
#include "netmech_srcpch.hpp"

#include "netmech/common.hpp"
#include "io_pool.hpp"

namespace netmech {

bool initialize(std::size_t io_threads) noexcept
{
    // 语义：只登记线程数，真正建池推迟到第一次取用（IoPool::global()）。
    // 这样"没用到网络的进程"不会白起 N 条线程。
    return IoPool::initialize(io_threads);
}

void shutdown() noexcept
{
    // 停止全部 io 线程并 join。context 本身**不销毁**（进程级），
    // 因此此后对已存在对象的析构仍然安全，只是不再执行新任务。
    IoPool::global().stop_all();
}

} // namespace netmech
