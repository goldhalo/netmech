// ============================================================================
//  bench_strand.cpp —— strand 开销基准
//
//  用途：量化「用 strand 串行化」与「依赖 1 io_context = 1 线程」之间的
//        性能差异，为设计决策提供数据依据。
//
//  背景：
//    `IoPool` 是 **1 个 io_context 配 1 条线程**（见 io_pool.cpp 的 start()）。
//    Asio 保证：同一 io_context 的 handler 只会被一条线程执行 —— 除非有
//    多条线程同时 run() 同一个 io_context。
//    因此在当前线程模型下，strand 提供的"额外串行保证"其实用不上，
//    它只是额外付出内部 mutex 的开销。
//
//  实测结论（Windows / MSVC 2019 /O2 / Asio 1.30.2）：
//    * 纯投递 post + poll_one : 无 strand 约 380 ns/op，+strand 约 520 ns/op
//                               → strand 增量约 +140 ns/次（相对 +37%）
//    * UDP async_send_to      : 增量同样是 ~140 ns，但分母 ~1700 ns，
//                               故相对开销摊薄到 +8% ~ +30%
//
//  结论：strand 的增量是**绝对 ~140ns/次投递**，与操作本身多贵无关。
//        高频小包场景下这个开销才显著。
//
//  使用方法：
//      bench_strand.exe post plain      # 纯投递，无 strand
//      bench_strand.exe post strand     # 纯投递，带 strand
//      bench_strand.exe send plain      # UDP 发送，无 strand
//      bench_strand.exe send strand     # UDP 发送，带 strand
//
//  ⚠ 每个进程只跑一种配置：跨轮复用 io_context 会因残留 handler 污染测量，
//    且极易触发"socket 晚于 io_context 析构"的崩溃（0xC0000409）。
// ============================================================================
#include <asio.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

using clk = std::chrono::steady_clock;
using udp = asio::ip::udp;

/// 每次测量的操作次数。
static constexpr int kOps = 100000;

static double elapsed_ms(clk::time_point t0)
{
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

// ---------------------------------------------------------------------------
// 1. 纯投递：post + poll_one 交替（最纯的 strand 开销测量）
// ---------------------------------------------------------------------------
template <bool UseStrand>
static void bench_post(const char* label)
{
    asio::io_context ioc;
    auto guard  = asio::make_work_guard(ioc);
    auto strand = asio::make_strand(ioc);
    int count   = 0;

    const auto t0 = clk::now();
    for (int i = 0; i < kOps; ++i) {
        if constexpr (UseStrand) {
            asio::post(strand, [&count] { ++count; });
        } else {
            asio::post(ioc, [&count] { ++count; });
        }
        ioc.poll_one();
    }
    const double ms = elapsed_ms(t0);
    guard.reset();

    std::printf("  %-30s %9.2f ms  (%7.1f ns/op)\n", label, ms, ms * 1e6 / kOps);
}

// ---------------------------------------------------------------------------
// 2. UDP 发送热路径
// ---------------------------------------------------------------------------
template <bool UseStrand>
static void bench_send(const char* label)
{
    asio::io_context ioc;
    auto guard = asio::make_work_guard(ioc);
    udp::socket sock(ioc, udp::endpoint(udp::v4(), 0));

    auto strand = asio::make_strand(ioc);
    // 发往一个本机未监听的端口：UDP 无连接，async_send_to 会立即完成
    // （可能带 ICMP 拒绝，但完成回调照样触发，不影响测量）。
    // ⚠ 不放 sink 接收端：递归 async_receive 会在 poll() 里不断自我重挂，
    //   把发送完成回调"饿死"，导致 done 永远达不到 kOps。
    const udp::endpoint target(asio::ip::make_address_v4("127.0.0.1"), 9);
    char payload[64] = {0};
    int done = 0;

    const auto t0 = clk::now();
    for (int i = 0; i < kOps; ++i) {
        if constexpr (UseStrand) {
            sock.async_send_to(asio::buffer(payload), target,
                asio::bind_executor(strand,
                    [&done](std::error_code, std::size_t) { ++done; }));
        } else {
            sock.async_send_to(asio::buffer(payload), target,
                [&done](std::error_code, std::size_t) { ++done; });
        }
        ioc.poll();
    }
    int spins = 0;
    while (done < kOps && spins < kOps * 8) {
        ioc.poll();
        ++spins;
    }
    const double ms = elapsed_ms(t0);

    // 安全收尾：先关 socket → 排空 → 释放 guard
    std::error_code ignored;
    sock.close(ignored);
    ioc.restart();
    while (ioc.poll_one() != 0) {
    }
    guard.reset();

    std::printf("  %-30s %9.2f ms  (%7.1f ns/op)  done=%d\n",
                label, ms, ms * 1e6 / kOps, done);
}
// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    const char* what      = (argc > 1) ? argv[1] : "post";
    const bool  use_strand = (argc > 2) && (std::strcmp(argv[2], "strand") == 0);

    if (std::strcmp(what, "post") == 0) {
        if (use_strand) {
            bench_post<true>("post + strand");
        } else {
            bench_post<false>("post 无 strand");
        }
    } else if (std::strcmp(what, "send") == 0) {
        if (use_strand) {
            bench_send<true>("UDP send + strand");
        } else {
            bench_send<false>("UDP send 无 strand");
        }
    } else {
        std::printf("用法: %s <post|send> <plain|strand>\n", argv[0]);
        return 1;
    }
    return 0;
}
