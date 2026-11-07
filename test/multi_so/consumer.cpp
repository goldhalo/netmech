// ============================================================================
//  test/multi_so/consumer.cpp —— "下游库"的最小替身（会被编成两个 .so）
//
//  每个 .so 都**静态链接**一份 netmech（和 modbusmech / httpmech 各自被
//  链进不同 SO 的形态一致）。两个 .so 通过导出函数接收宿主创建的
//  `netmech::Runtime`，用 `Mechanism::create(cfg, rt)` 建机制。
//
//  回归目标：宿主只建**一个** Runtime 时，两个 .so 共用同一套 io 线程池
//  （而不是各起一套）。默认运行时（IoPool::global()）在静态链接下是
//  "每个 .so 各一份静态数据"，所以必须靠注入才能共用 —— 这就是本用例。
// ============================================================================
#include <netmech/net.hpp>

#include <cstddef>
#include <memory>

namespace {

/// 接受连接但立刻放掉（本用例不关心业务，只关心线程池归属）。
struct SilentLink final : netmech::Linkable {
    std::shared_ptr<netmech::ConnHandler> on_accept(netmech::Conn&) override
    {
        return nullptr;
    }
};

std::shared_ptr<netmech::Mechanism> g_mech;   ///< 保持到进程退出（验证线程数不再增长）

} // namespace

extern "C" {

/// 这个 .so 看到的运行时线程数（由注入的 Runtime 决定，而非本模块默认值）。
std::size_t consumer_io_threads(const void* rt_raw)
{
    const auto* rt = static_cast<const netmech::Runtime*>(rt_raw);
    return rt != nullptr ? rt->io_threads() : 0;
}

/// 用注入的运行时建一个 Mechanism 并 listen 临时端口。
/// @return 0 = 成功；-1 参数空；-2 创建失败；-3 listen 失败。
int consumer_listen(const void* rt_raw)
{
    if (rt_raw == nullptr) {
        return -1;
    }
    auto* rt = const_cast<netmech::Runtime*>(static_cast<const netmech::Runtime*>(rt_raw));

    // 非拥有句柄：Runtime 由宿主管理，本模块只借来取池。
    std::shared_ptr<netmech::Runtime> rt_handle(rt, [](netmech::Runtime*) {});

    g_mech = netmech::Mechanism::create(netmech::Mechanism::Config{}, std::move(rt_handle));
    if (!g_mech) {
        return -2;
    }
    static std::shared_ptr<SilentLink> link = std::make_shared<SilentLink>();
    const auto ec = g_mech->listen(netmech::Address::any(0), link);
    return ec ? -3 : 0;
}

} // extern "C"
