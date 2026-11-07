// =============================================================================
//  runtime.cpp —— io 运行时（可注入线程池）的实现
// =============================================================================
//  分层：
//      Runtime（公共，可注入）
//          └── IoPool（内部，N 个 io_context + N 条线程）
//
//  默认运行时（`default_runtime()`）包的是 `IoPool::global()` —— 也就是
//  `initialize()` / `shutdown()` 一直管理的那一个，行为完全不变；
//  `Runtime::create()` 则**新建**一套独立线程池。
// =============================================================================
#include "netmech_srcpch.hpp"

#include "netmech/common.hpp"
#include "netmech/runtime.hpp"

#include "io_pool.hpp"
#include "runtime_internal.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>
#include <memory>
#include <system_error>
#include <utility>

namespace netmech {

// =============================================================================
//  Impl
// =============================================================================

struct Runtime::Impl {
    explicit Impl(IoPool* p) noexcept : pool(p) {}

    /// 池指针由 Runtime 负责创建；**Runtime 析构时不删**（见文件头：
    /// io_context 与进程同寿，池对象随进程回收）。
    IoPool* pool = nullptr;
};

// =============================================================================
//  Runtime
// =============================================================================

Runtime::Runtime(Impl* impl) noexcept : m_impl(impl)
{
}

Runtime::~Runtime()
{
    // 幂等；这里只停线程 + join，不销毁 io_context（见 runtime.hpp 的
    // 生命周期说明）。m_impl / pool 刻意泄漏（与 IoPool::global() 的做法一致）：
    // 进程结束时由 OS 回收，换掉一整类"IO 对象比 context 活得久"的崩溃。
    shutdown();
}

std::shared_ptr<Runtime> Runtime::create(const Options& opts,
                                         std::error_code& ec) noexcept
{
    ec.clear();

    std::size_t threads = opts.io_threads;
    if (threads == 0) {
        threads = std::thread::hardware_concurrency();
        if (threads == 0) {
            threads = 1;
        }
    }

    IoPool* pool = nullptr;
    try {
        pool = new IoPool(threads);
    } catch (const std::exception&) {
        ec = make_boost_error_code(NetErrc::not_initialized);
        return nullptr;
    } catch (...) {
        ec = make_boost_error_code(NetErrc::not_initialized);
        return nullptr;
    }

    if (!pool->start(threads)) {
        delete pool;
        ec = make_boost_error_code(NetErrc::not_initialized);
        return nullptr;
    }

    Impl* impl = new Impl(pool);
    try {
        return std::shared_ptr<Runtime>(new Runtime(impl));
    } catch (...) {
        // 构造 Runtime 失败：停掉刚起的线程，避免留下孤儿线程池。
        pool->stop_all();
        pool->join_threads();
        delete impl;   // 注意：Impl 不拥有 pool（见结构体注释），所以单独释放
        ec = make_boost_error_code(NetErrc::not_initialized);
        return nullptr;
    }
}

std::shared_ptr<Runtime> Runtime::create()
{
    return create(Options{});
}

std::shared_ptr<Runtime> Runtime::create(const Options& opts)
{
    std::error_code ec;
    std::shared_ptr<Runtime> rt = create(opts, ec);
    if (!rt) {
        // 线程创建失败属于启动期致命错误（与 IoPool::global() 的既有口径一致）：
        // 一个 io 线程都起不来，任何网络对象都无法工作。
        std::fputs("netmech: 无法创建 io 运行时（线程创建失败）\n", stderr);
        std::terminate();
    }
    return rt;
}

std::size_t Runtime::io_threads() const noexcept
{
    return (m_impl != nullptr && m_impl->pool != nullptr) ? m_impl->pool->size() : 0;
}

bool Runtime::running() const noexcept
{
    return m_impl != nullptr && m_impl->pool != nullptr && m_impl->pool->running();
}

void Runtime::shutdown() noexcept
{
    if (m_impl == nullptr || m_impl->pool == nullptr) {
        return;
    }
    m_impl->pool->stop_all();
    m_impl->pool->join_threads();
}

// =============================================================================
//  进程级默认运行时
// =============================================================================

Runtime& default_runtime()
{
    // 包住 IoPool::global()（即 initialize()/shutdown() 一直管理的那一个）。
    // 故意不释放：默认运行时与进程同寿。
    static Runtime* const instance = []() -> Runtime* {
        Runtime::Impl* impl = new Runtime::Impl(&IoPool::global());
        return new Runtime(impl);
    }();
    return *instance;
}

namespace detail {

std::shared_ptr<Runtime> default_runtime_handle() noexcept
{
    // no-op deleter：默认运行时是进程级的，库内对象持有它只为"取池"，
    // 不应该（也无法）延长它的生命周期。
    static const std::shared_ptr<Runtime> handle(&default_runtime(), [](Runtime*) {});
    return handle;
}

} // namespace detail

// =============================================================================
//  内部访问器
// =============================================================================

IoPool* Runtime::pool_ptr() const noexcept
{
    return (m_impl != nullptr) ? m_impl->pool : nullptr;
}

IoPool& detail::runtime_pool(Runtime& rt) noexcept
{
    return *rt.pool_ptr();
}

// =============================================================================
//  兼容旧 API 的两个全局函数（实现不变，语义见 common.hpp）
// =============================================================================

bool initialize(std::size_t io_threads) noexcept
{
    // 语义：只登记默认运行时的线程数，真正建池推迟到第一次取用。
    return IoPool::initialize(io_threads);
}

void shutdown() noexcept
{
    // 兑现 common.hpp 的"停掉全部 io 线程并 join"。
    // context 本身不销毁（进程级），此后对已存在对象的析构仍然安全。
    IoPool::global().stop_all();
    IoPool::global().join_threads();
}

} // namespace netmech
