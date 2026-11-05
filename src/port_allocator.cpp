// =============================================================================
//  port_allocator.cpp —— 端口分配器实现
// =============================================================================
//  探测方式：临时 io_context + 一个 UDP socket，试着 bind 到 0.0.0.0:port。
//  能绑上 ⇒ 认为可用；立刻关闭，把端口号交出去。
//  （UDP 与 TCP 的端口空间在多数系统上独立；本类**按 UDP 探测**，
//    给 TCP 用时请在 bind 失败处重试 —— 见头文件里"探测 ≠ 占位"。）
// =============================================================================
#include <netmech/port_allocator.hpp>

#include "netmech_srcpch.hpp"

#include <vector>

namespace netmech {

struct PortAllocator::Impl {
    mutable std::mutex  mtx;
    std::uint16_t       begin = 0;
    std::uint16_t       end   = 0;
    std::uint16_t       cursor = 0;

    /// 试一整组（count 个连续端口）是否都能绑上。
    /// 调用方保证 begin/end 已经算过，不会溢出。
    static bool probe_run(std::uint16_t first, std::uint16_t count) noexcept
    {
        asio::io_context probe_ctx;
        std::vector<asio::ip::udp::socket> held;
        held.reserve(count);

        for (std::uint16_t i = 0; i < count; ++i) {
            error_code_t ec;
            asio::ip::udp::socket sock(probe_ctx);
            sock.open(asio::ip::udp::v4(), ec);
            if (ec) {
                return false;
            }
            const auto port = static_cast<std::uint16_t>(first + i);
            sock.bind(asio::ip::udp::endpoint(asio::ip::address_v4::any(), port), ec);
            if (ec) {
                return false;   // 这一组里有被占的：整组放弃（held 析构时自动关）
            }
            held.push_back(std::move(sock));   // 持住，保证探测期间不被自己复用
        }
        return true;   // held 在此返回，端口立刻释放
    }
};

PortAllocator::PortAllocator(std::uint16_t begin, std::uint16_t end)
    : m_impl(std::make_shared<Impl>())
{
    m_impl->begin  = begin;
    m_impl->end    = end;
    m_impl->cursor = begin;
}

PortAllocator::~PortAllocator() = default;

std::optional<std::uint16_t> PortAllocator::acquire(std::uint16_t count,
                                                    std::uint16_t alignment) noexcept
{
    if (!m_impl || count == 0) {
        return std::nullopt;
    }
    if (alignment == 0) {
        alignment = 1;
    }

    std::lock_guard<std::mutex> lk(m_impl->mtx);

    const std::uint32_t lo = m_impl->begin;
    const std::uint32_t hi = m_impl->end;
    if (lo > hi) {
        return std::nullopt;
    }
    if (count > (hi - lo + 1)) {
        return std::nullopt;   // 区间装不下一组
    }

    std::uint32_t cur = m_impl->cursor;
    if (cur < lo || cur > hi) {
        cur = lo;
    }

    // 从游标扫到区间尾，然后回绕再扫到游标之前 —— 覆盖整个区间，且优先"往前走"。
    const std::uint32_t span = hi - lo + 1;
    for (std::uint32_t step = 0; step < span; ++step) {
        std::uint32_t base = cur + step;
        if (base > hi) {
            base = lo + (base - hi - 1);   // 回绕
        }
        // 对齐 + 整组放得下
        const std::uint32_t aligned = ((base + alignment - 1) / alignment) * alignment;
        if (aligned + count - 1 > hi) {
            continue;
        }
        const auto first = static_cast<std::uint16_t>(aligned);
        if (!Impl::probe_run(first, count)) {
            continue;
        }
        // 游标前移到本组之后（走到尾回绕）
        const std::uint32_t next = aligned + count;
        m_impl->cursor = static_cast<std::uint16_t>(next > hi ? lo : next);
        return first;
    }
    return std::nullopt;
}

void PortAllocator::release(std::uint16_t first, std::uint16_t count) noexcept
{
    // 本类不维护引用计数：端口"可用"这件事是由 probe 实时判断的，
    // 所以 release 唯一有意义的作用是把游标挪回这组之前 —— 但那样会
    // 造成"刚释放就立刻被下一个连接抢回"的抖动（端口复用太快，
    // 对端可能还留着旧流的状态）。这里刻意只做参数校验，什么都不改。
    (void)first;
    (void)count;
}

std::uint16_t PortAllocator::cursor() const noexcept
{
    if (!m_impl) {
        return 0;
    }
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    return m_impl->cursor;
}

std::uint16_t PortAllocator::begin() const noexcept
{
    return m_impl ? m_impl->begin : 0;
}

std::uint16_t PortAllocator::end() const noexcept
{
    return m_impl ? m_impl->end : 0;
}

} // namespace netmech
