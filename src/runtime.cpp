// =============================================================================
//  netmech - 进程级运行时入口
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

#include <cstddef>

namespace netmech {

bool initialize(std::size_t io_threads) noexcept
{
    // 语义：只登记线程数，真正建池推迟到第一次取用（IoPool::global()）。
    // 这样"没用到网络的进程"不会白起 N 条线程。
    return IoPool::initialize(io_threads);
}

void shutdown() noexcept
{
    // 两段：先停止投递并唤醒线程，再真正 join（兑现 common.hpp 的
    // "线程 join / 阻塞直到所有 io 线程退出"）。
    //
    // 只 stop 不 join 是不够的：`std::thread` 在 joinable 状态下析构会
    // 直接 std::terminate()，而且调用方拿不到"线程真的走了"这个保证。
    //
    // context 本身**不销毁**（进程级单例），此后对已存在对象的析构仍然安全。
    IoPool::global().stop_all();
    IoPool::global().join_threads();
}

} // namespace netmech
