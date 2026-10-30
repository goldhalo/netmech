// ============================================================================
//  test_framework.hpp —— NetMechanism 极简单元测试框架
//
//  不依赖 gtest/gmock，单头文件，C++17 可编译。
//  特性：
//    CHECK(cond)             条件为真，否则记失败并继续
//    CHECK_EQ(a, b)          相等比较（失败时打印两侧值）
//    CHECK_NE(a, b)          不等
//    CHECK_GE/LE/GT/LT       大小比较
//    REQUIRE(cond)           条件为假则**终止当前用例**（用于后续代码不可执行的场景）
//    RUN_TEST(fn)            注册并运行一个用例
//    TEST_MAIN()             生成 main()，打印汇总并按失败数返回非 0
//    EXPECT_OK(ec)           要求 error_code 为空
//    EXPECT_EC(ec, errc)     要求 error_code 等于指定值
//
//  用法：
//      TEST_FRAMEWORK_BEGIN()          // 可选：自定义 main 前后钩子
//      RUN_TEST(my_case);
//      TEST_FRAMEWORK_END()
//  或者直接：
//      int main() { return netmech_test::run_all(); }
// ============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>


namespace netmech_test
{

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
struct Stats
{
    std::atomic<std::uint64_t> checks{0};
    std::atomic<std::uint64_t> failures{0};
    std::atomic<std::uint64_t> cases_run{0};
    std::atomic<std::uint64_t> cases_failed{0};
};

inline Stats& stats()
{
    static Stats s;
    return s;
}

// 串行化输出，避免多线程测试里判定失败时输出交错
inline std::mutex& output_mutex()
{
    static std::mutex m;
    return m;
}

// 当前用例名与"当前用例是否已失败"
struct CaseContext
{
    std::string name;
    bool failed = false;
    bool aborted = false;
};
inline CaseContext& current_case()
{
    static CaseContext c;
    return c;
}

// 失败记录行（供最后打印）
inline std::vector<std::string>& failure_log()
{
    static std::vector<std::string> v;
    return v;
}

// ---------------------------------------------------------------------------
// 断言原语
// ---------------------------------------------------------------------------
inline void record_failure(const char* file, int line, const std::string& msg)
{
    stats().failures.fetch_add(1);
    current_case().failed = true;

    std::ostringstream oss;
    oss << "    [FAIL] " << current_case().name << " @ " << file << ":" << line << "\n"
        << "           " << msg;
    std::lock_guard<std::mutex> lk(output_mutex());
    std::cout << oss.str() << std::endl;
    failure_log().push_back(oss.str());
}

template <typename T>
std::string to_display(const T& v)
{
    try
    {
        std::ostringstream oss;
        oss << v;
        return oss.str();
    }
    catch (...)
    {
        return "<to_display 失败>";
    }
}

inline std::string to_display(const std::error_code& ec)
{
    try
    {
        std::ostringstream oss;
        oss << "ec{" << ec.value() << ", \"" << ec.message() << "\"}";
        return oss.str();
    }
    catch (...)
    {
        // 某些自定义 category 的 message() 在特定代码页下可能分配失败
        std::ostringstream oss;
        oss << "ec{" << ec.value() << ", <message 不可用>}";
        return oss.str();
    }
}

inline std::string to_display(bool b) { return b ? "true" : "false"; }

// MSVC /W4：CHECK(1 == 1) 这类字面量条件会触发 C4127（条件表达式是常量）。
// 这是测试断言宏的固有形态，属噪音，在头文件范围内关掉。
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4127)   // conditional expression is constant
#endif

/// 条件检查（失败时继续执行当前用例）
///
/// 注意：失败信息通过 lambda **延迟构造** —— 断言通过时绝不调用
/// expr_text，避免 to_display()/error_code::message() 在热路径上做
/// 不必要的字符串分配（曾导致断言通过时反而抛 std::bad_alloc）。
#define NETMECH_CHECK_IMPL(cond, expr_text)                                        \
    do {                                                                           \
        ::netmech_test::stats().checks.fetch_add(1);                               \
        if (!(cond)) {                                                             \
            std::string _nm_msg;                                                   \
            try {                                                                  \
                _nm_msg = (expr_text);                                             \
            } catch (...) {                                                        \
                _nm_msg = "<构造失败信息时抛异常>";                                \
            }                                                                      \
            ::netmech_test::record_failure(__FILE__, __LINE__, _nm_msg);            \
        }                                                                          \
    } while (0)

#define CHECK(cond) NETMECH_CHECK_IMPL((cond), "CHECK(" #cond ")")

#define CHECK_EQ(a, b)                                                             \
    NETMECH_CHECK_IMPL((a) == (b),                                                 \
        "CHECK_EQ(" #a ", " #b ") -> "                                             \
        + ::netmech_test::to_display(a) + " vs " + ::netmech_test::to_display(b))

#define CHECK_NE(a, b)                                                             \
    NETMECH_CHECK_IMPL((a) != (b),                                                 \
        "CHECK_NE(" #a ", " #b ") -> both " + ::netmech_test::to_display(a))

#define CHECK_GE(a, b)                                                             \
    NETMECH_CHECK_IMPL((a) >= (b),                                                 \
        "CHECK_GE(" #a ", " #b ") -> "                                             \
        + ::netmech_test::to_display(a) + " vs " + ::netmech_test::to_display(b))

#define CHECK_LE(a, b)                                                             \
    NETMECH_CHECK_IMPL((a) <= (b),                                                 \
        "CHECK_LE(" #a ", " #b ") -> "                                             \
        + ::netmech_test::to_display(a) + " vs " + ::netmech_test::to_display(b))

#define CHECK_GT(a, b)                                                             \
    NETMECH_CHECK_IMPL((a) > (b),                                                  \
        "CHECK_GT(" #a ", " #b ") -> "                                             \
        + ::netmech_test::to_display(a) + " vs " + ::netmech_test::to_display(b))

#define CHECK_LT(a, b)                                                             \
    NETMECH_CHECK_IMPL((a) < (b),                                                  \
        "CHECK_LT(" #a ", " #b ") -> "                                             \
        + ::netmech_test::to_display(a) + " vs " + ::netmech_test::to_display(b))

// 必须成立：不成立则中止当前用例（防止空指针继续往下跑）
#define REQUIRE(cond)                                                              \
    do {                                                                           \
        ::netmech_test::stats().checks.fetch_add(1);                               \
        if (!(cond)) {                                                             \
            ::netmech_test::record_failure(__FILE__, __LINE__, "REQUIRE(" #cond ")"); \
            ::netmech_test::current_case().aborted = true;                         \
            throw ::netmech_test::CaseAborted{};                                   \
        }                                                                          \
    } while (0)

struct CaseAborted
{
};

// 对 std::error_code 的便捷判断
#define EXPECT_OK(ec)                                                              \
    NETMECH_CHECK_IMPL(!(ec), "EXPECT_OK(" #ec ") -> " + ::netmech_test::to_display(ec))
#define EXPECT_EC(ec, expected)                                                    \
    NETMECH_CHECK_IMPL((ec) == (expected),                                         \
        "EXPECT_EC(" #ec ", " #expected ") -> " + ::netmech_test::to_display(ec))

/// 断言两个值相等，失败时输出**自定义**说明（比 CHECK_EQ 更适合打印原因）
#define CHECK_MSG(cond, msg)                                                       \
    do {                                                                           \
        ::netmech_test::stats().checks.fetch_add(1);                               \
        if (!(cond)) {                                                             \
            ::netmech_test::record_failure(__FILE__, __LINE__,                     \
                                           std::string(#cond) + " : " + (msg));    \
        }                                                                          \
    } while (0)

/// 断言失败并记录自定义说明（用于"走到这里必然有问题"的分支）
#define FAIL_MSG(msg) ::netmech_test::record_failure(__FILE__, __LINE__, (msg))

// ---------------------------------------------------------------------------
// 用例注册与执行
// ---------------------------------------------------------------------------
using TestFn = std::function<void()>;

struct TestCase
{
    const char* name;
    TestFn fn;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> v;
    return v;
}

struct Registrar
{
    Registrar(const char* name, TestFn fn) { registry().push_back(TestCase{name, fn}); }
};

inline void run_one(const TestCase& tc)
{
    {
        std::lock_guard<std::mutex> lk(output_mutex());
        std::cout << "[ RUN      ] " << tc.name << std::endl;
    }

    current_case() = CaseContext{};
    current_case().name = tc.name;
    stats().cases_run.fetch_add(1);

    const auto t0 = std::chrono::steady_clock::now();
    try
    {
        tc.fn();
    }
    catch (const CaseAborted&)
    {
        // REQUIRE 已记录失败并中止，正常退出
    }
    catch (const std::exception& e)
    {
        record_failure("<case>", 0, std::string("未捕获异常: ") + e.what());
    }
    catch (...)
    {
        record_failure("<case>", 0, "未捕获的未知异常");
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();

    const bool failed = current_case().failed;
    if (failed)
        stats().cases_failed.fetch_add(1);

    std::lock_guard<std::mutex> lk(output_mutex());
    std::cout << (failed ? "[  FAILED  ] " : "[       OK ] ") << tc.name
              << "  (" << ms << " ms)" << std::endl;
}

/// 运行全部已注册用例；返回进程退出码（全部通过 = 0）
///
/// @param filter 非空时，只运行用例名**包含**该子串的用例（大小写敏感）。
/// @note 每个用例结束都会立即打印一行结果并 flush —— 这样即使后续用例
///       触发访问违例（SEH，catch(...) 抓不到）导致进程崩溃，已完成用例
///       的统计也不会丢。
inline int run_all(const std::string& filter = std::string())
{
    std::vector<TestCase> selected;
    for (const auto& tc : registry())
    {
        if (filter.empty() || std::string(tc.name).find(filter) != std::string::npos)
            selected.push_back(tc);
    }

    std::cout << "==========================================================\n"
              << " NetMechanism 单元测试\n";
    if (!filter.empty())
        std::cout << " 过滤: \"" << filter << "\"  (匹配 " << selected.size()
                  << " / " << registry().size() << " 个用例)\n";
    std::cout << "==========================================================" << std::endl;

    if (selected.empty())
    {
        std::cout << " 没有匹配的用例" << std::endl;
        return 1;
    }

    for (const auto& tc : selected)
    {
        run_one(tc);
        // ★ 立即输出进度统计并 flush：SEH 崩溃后这些信息仍可见
        const std::uint64_t c = stats().cases_run.load();
        const std::uint64_t cf = stats().cases_failed.load();
        const std::uint64_t a = stats().checks.load();
        const std::uint64_t af = stats().failures.load();
        {
            std::lock_guard<std::mutex> lk(output_mutex());
            std::cout << "           [进度] 用例 " << c << " 运行 / " << cf
                      << " 失败; 断言 " << a << " 运行 / " << af
                      << " 失败; 剩余 " << (selected.size() - c) << std::endl;
        }
    }

    const std::uint64_t checks = stats().checks.load();
    const std::uint64_t failures = stats().failures.load();
    const std::uint64_t cases = stats().cases_run.load();
    const std::uint64_t cases_failed = stats().cases_failed.load();

    std::lock_guard<std::mutex> lk(output_mutex());
    std::cout << "----------------------------------------------------------\n"
              << " 用例: " << (cases - cases_failed) << " 通过 / " << cases_failed
              << " 失败 / " << cases << " 总计\n"
              << " 断言: " << (checks - failures) << " 通过 / " << failures
              << " 失败 / " << checks << " 总计\n";
    if (failures == 0)
        std::cout << " 结果: PASS" << std::endl;
    else
        std::cout << " 结果: FAIL" << std::endl;
    std::cout << "----------------------------------------------------------" << std::endl;

    return failures == 0 ? 0 : 1;
}

} // namespace netmech_test

// ---------------------------------------------------------------------------
// 便捷宏
// ---------------------------------------------------------------------------
#define NETMECH_CONCAT_IMPL(a, b) a##b
#define NETMECH_CONCAT(a, b) NETMECH_CONCAT_IMPL(a, b)

#define TEST_CASE_REGISTER(fn, name)                                               \
    static ::netmech_test::Registrar NETMECH_CONCAT(_nm_reg_, __LINE__)(name, fn)

/// 定义一个静态函数并注册：RUN_TEST(名字) { ... }
/// 用法：在 main 里调用 RUN_TEST(...) 之前，需要先定义用例函数；
/// 这里采用"立即执行注册"风格，要求 RUN_TEST 出现在 main 内部（常见做法），
/// 故同时提供 DEFINE_TEST(name) 宏用于命名空间级别的用例定义。
#define RUN_TEST(fn)                                                               \
    do {                                                                           \
        ::netmech_test::run_one(::netmech_test::TestCase{#fn, fn});                \
    } while (0)

/// 在命名空间级别定义并注册用例：NETMECH_TEST(name) { ... }
#define NETMECH_TEST(name)                                                         \
    static void name();                                                            \
    TEST_CASE_REGISTER(name, #name);                                               \
    static void name()

#define TEST_MAIN()                                                                \
    int main()                                                                     \
    {                                                                              \
        try {                                                                      \
            return ::netmech_test::run_all();                                      \
        }                                                                          \
        catch (const std::exception& e) {                                          \
            std::cerr << "测试框架异常: " << e.what() << std::endl;                \
            return 2;                                                              \
        }                                                                          \
    }

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
