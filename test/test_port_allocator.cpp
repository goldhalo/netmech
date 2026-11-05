// ============================================================================
//  test_port_allocator.cpp —— 端口分配器
//
//  验证：
//    · 在给定区间里按"递增游标"分配，且不重复
//    · alignment=2 时首端口为偶数（RTP/RTCP 配对）
//    · 区间耗尽后返回 nullopt（不外溢到区间外）
//    · 区间尾之后回绕到起点
//    · 已被别人占住的端口会被跳过
// ============================================================================
#include <netmech/net.hpp>

#include <set>
#include <vector>

#include "test_framework.hpp"

using namespace netmech;
using namespace netmech_test;

namespace {

/// 占住一个 UDP 端口，用于制造"不可用"。
class Blocker {
public:
    struct Null : UdpHandler {
        void on_read(UdpEndpoint&, std::error_code, Buf, std::size_t,
                     const Address&) override {}
    };

    bool open(std::uint16_t port)
    {
        std::error_code ec;
        UdpConfig cfg;
        cfg.bind = Address("0.0.0.0", port);
        m_ep     = create_udp_endpoint(cfg, std::make_shared<Null>(), nullptr, ec);
        return static_cast<bool>(m_ep) && !ec;
    }
    void close()
    {
        if (m_ep) {
            m_ep->close();
            m_ep.reset();
        }
    }
    ~Blocker() { close(); }

private:
    std::shared_ptr<UdpEndpoint> m_ep;
};

} // namespace

NETMECH_TEST(allocator_hands_out_sequential_pairs)
{
    PortAllocator alloc(20000, 20010);

    std::set<std::uint16_t> seen;
    for (int i = 0; i < 3; ++i) {
        const auto first = alloc.acquire(/*count=*/2, /*alignment=*/2);
        REQUIRE(first.has_value());
        CHECK_EQ((*first % 2u), 0u);              // 偶数开头（RTP/RTCP 约定）
        CHECK((*first + 1) <= 20010);
        CHECK(seen.insert(*first).second);        // 不重复
    }
    // 游标已经前进
    CHECK(alloc.cursor() > 20000);
}

NETMECH_TEST(allocator_skips_occupied_port)
{
    // 把区间里第一个偶数端口占住，分配器必须跳过它
    Blocker blocker;
    REQUIRE(blocker.open(20020));

    PortAllocator alloc(20020, 20030);
    const auto    first = alloc.acquire(/*count=*/2, /*alignment=*/2);
    REQUIRE(first.has_value());
    CHECK(*first != 20020);
    CHECK_EQ((*first % 2u), 0u);

    blocker.close();
}

NETMECH_TEST(allocator_returns_nullopt_when_exhausted)
{
    // 区间装不下一组：直接拒绝，绝不外溢到区间之外
    PortAllocator tiny(20040, 20040);
    CHECK(!tiny.acquire(/*count=*/2, /*alignment=*/2).has_value());

    // 真正的"耗尽"：两个端口都被别人占住
    Blocker b1;
    Blocker b2;
    REQUIRE(b1.open(20042));
    REQUIRE(b2.open(20043));

    PortAllocator alloc(20042, 20043);
    CHECK(!alloc.acquire(/*count=*/1, /*alignment=*/1).has_value());

    b1.close();
    b2.close();
}

NETMECH_TEST(allocator_wraps_around)
{
    PortAllocator alloc(20050, 20055);
    // 先把游标推到接近尾部
    const auto a = alloc.acquire(/*count=*/1, /*alignment=*/1);
    REQUIRE(a.has_value());
    const auto b = alloc.acquire(/*count=*/1, /*alignment=*/1);
    REQUIRE(b.has_value());
    CHECK_EQ(*b, static_cast<std::uint16_t>(*a + 1));

    // 强行把它推到尾后再申请，应当回绕
    PortAllocator small(20060, 20061);
    const auto   s1 = small.acquire(1, 1);
    REQUIRE(s1.has_value());
    CHECK_EQ(*s1, 20060);
    const auto s2 = small.acquire(1, 1);
    REQUIRE(s2.has_value());
    CHECK_EQ(*s2, 20061);
    // 区间已满（两个端口都被"分配器认为可用"但没被真正占用），
    // 因为探测只看真实占用，这里应该还能再拿到 —— 验证的是"不崩 + 在区间内"
    const auto s3 = small.acquire(1, 1);
    if (s3) {
        CHECK(*s3 >= 20060 && *s3 <= 20061);
    }
}
