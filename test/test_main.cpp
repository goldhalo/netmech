// ============================================================================
//  test_main.cpp —— netmech 单元测试入口
//
//  使用自研极简框架（test_framework.hpp），不引入 gtest。
//
//  用法：
//      netmech_ut.exe                 # 跑全部用例
//      netmech_ut.exe <子串>          # 只跑名字包含该子串的用例（过滤）
//      netmech_ut.exe --list          # 只列出全部用例名
//      netmech_ut.exe -f <子串>       # 同上，显式形式
//
//  退出码：0 = 全部通过；1 = 有失败；2 = 框架自身异常。
// ============================================================================
#include "test_framework.hpp"

#include <netmech/common.hpp>

#include <cstdio>
#include <exception>
#include <string>

namespace
{
void print_usage(const char* exe)
{
    std::printf("用法:\n"
                "  %s                 跑全部用例\n"
                "  %s <子串>          只跑名字包含该子串的用例\n"
                "  %s --list          列出全部用例名\n"
                "  %s -f <子串>       同上，显式形式\n"
                "  %s --help          显示本帮助\n",
                exe, exe, exe, exe, exe);
}

void list_cases()
{
    std::printf("共 %zu 个用例:\n", netmech_test::registry().size());
    int i = 0;
    for (const auto& tc : netmech_test::registry())
        std::printf("  [%02d] %s\n", ++i, tc.name);
}
} // namespace

int main(int argc, char** argv)
{
    std::string filter;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--list" || arg == "-l")
        {
            list_cases();
            return 0;
        }
        if (arg == "-f" || arg == "--filter")
        {
            if (i + 1 >= argc)
            {
                std::printf("错误: %s 需要一个参数\n", argv[i]);
                return 2;
            }
            filter = argv[++i];
            continue;
        }
        if (!arg.empty() && arg[0] == '-')
        {
            std::printf("错误: 未知选项 %s\n", arg.c_str());
            print_usage(argv[0]);
            return 2;
        }
        if (filter.empty())
            filter = arg;      // 裸参数也当作过滤子串
    }

    try
    {
        const int rc = netmech_test::run_all(filter);

        // ★ 退出前停掉进程级 io 池。
        //
        // 不这么做的话，进程退出时可能**还有 io 线程在跑**，而 OpenSSL 在
        // atexit 里会释放它自己的全局锁（CRYPTO_THREAD_lock_free）。两者撞在
        // 一起，TSan 会在 `exit()` 阶段报一条 data race —— 报告指向
        // libcrypto 而非本库代码（实测：TLS 构建的 TSan job 里 96 个用例全绿，
        // 只在进程退出阶段冒一条 CRYPTO_THREAD_lock_free）。
        //
        // 这也不是"为了迁就 sanitizer"：库的契约本来就要求"先关业务对象、
        // 再停运行时"，测试进程同样应该守这条 —— 顺带把 shutdown() 路径
        // （join 所有 io 线程）也纳入每次 CI。
        netmech::shutdown();
        return rc;
    }
    catch (const std::exception& e)
    {
        std::printf("测试框架自身异常: %s\n", e.what());
        return 2;
    }
    catch (...)
    {
        std::printf("测试框架未知异常\n");
        return 2;
    }
}
