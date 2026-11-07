// ============================================================================
//  test/multi_so/main.cpp —— 多 .so 共用一套 io 线程池的回归用例
//
//  场景（和 modbusmech + httpmech 被两个 SO 分别加载一致）：
//    * 两个 .so 各自静态链接一份 netmech（默认运行时 = 每个 .so 各一套池）；
//    * 宿主创建**一个** `Runtime`（4 线程），通过导出函数注入两个 .so；
//    * 断言：两个 .so 看到同一个 Runtime、进程只多出 4 条 io 线程。
//
//  这正是"静态库 + 多 SO"场景下默认运行时会变成多份、而注入能共用一套的证据。
// ============================================================================
#include <netmech/net.hpp>

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <string>

#include <dlfcn.h>

#ifndef CONSUMER_DIR
#  error "CONSUMER_DIR 未定义（由 CMake 传入）"
#endif

namespace {

int proc_threads()
{
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("Threads:", 0) == 0) {
            return std::stoi(line.substr(8));
        }
    }
    return -1;
}

void* open_lib(const char* base)
{
    const std::string path = std::string(CONSUMER_DIR) + "/" + base;
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
        std::fprintf(stderr, "dlopen(%s) 失败: %s\n", path.c_str(), dlerror());
    }
    return h;
}

} // namespace

int main()
{
    int rc = 0;

    const int before = proc_threads();

    netmech::Runtime::Options opt;
    opt.io_threads = 4;
    auto rt = netmech::Runtime::create(opt);
    if (!rt) {
        std::fprintf(stderr, "Runtime::create 失败\n");
        return 2;
    }

    const int after_runtime = proc_threads();
    std::printf("线程数: 基线=%d, 建 Runtime(4) 后=%d（期望 +4）\n",
                before, after_runtime);
    if (after_runtime != before + 4) {
        std::printf("  [FAIL] 线程数不是 +4\n");
        rc = 1;
    }

    void* a = open_lib("libnetmech_multi_so_a.so");
    void* b = open_lib("libnetmech_multi_so_b.so");
    if (a == nullptr || b == nullptr) {
        return 2;
    }

    using threads_fn = std::size_t (*)(const void*);
    using listen_fn  = int (*)(const void*);

    auto a_threads = reinterpret_cast<threads_fn>(dlsym(a, "consumer_io_threads"));
    auto b_threads = reinterpret_cast<threads_fn>(dlsym(b, "consumer_io_threads"));
    auto a_listen  = reinterpret_cast<listen_fn>(dlsym(a, "consumer_listen"));
    auto b_listen  = reinterpret_cast<listen_fn>(dlsym(b, "consumer_listen"));
    if (a_threads == nullptr || b_threads == nullptr || a_listen == nullptr || b_listen == nullptr) {
        std::fprintf(stderr, "dlsym 失败\n");
        return 2;
    }

    const std::size_t ta = a_threads(rt.get());
    const std::size_t tb = b_threads(rt.get());
    std::printf("so A 看到的 io_threads=%zu, so B=%zu（期望都是 4 = 同一个 Runtime）\n", ta, tb);
    if (ta != 4 || tb != 4) {
        std::printf("  [FAIL] 注入的运行时没有被两个 .so 共用\n");
        rc = 1;
    }

    const int la = a_listen(rt.get());
    const int lb = b_listen(rt.get());
    if (la != 0 || lb != 0) {
        std::printf("  [FAIL] 用注入运行时 listen 失败（A=%d B=%d）\n", la, lb);
        rc = 1;
    }

    const int after_mechanisms = proc_threads();
    std::printf("两个 .so 各建一个 Mechanism 后线程数=%d（期望仍为 %d，不新增线程）\n",
                after_mechanisms, after_runtime);
    if (after_mechanisms != after_runtime) {
        std::printf("  [FAIL] 建 Mechanism 又新起了线程：说明没用注入的池\n");
        rc = 1;
    }

    rt->shutdown();

    std::printf("%s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}
