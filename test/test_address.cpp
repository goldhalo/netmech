// ============================================================================
//  test_address.cpp —— netmech::Address 单元测试
//
//  覆盖：
//    * 默认构造 / any() / (ip, port) 构造
//    * parse()：IPv4、IPv6（带方括号）、仅端口、各类非法输入
//    * to_string() 往返
//    * 比较运算符、is_v4()、is_unspecified()
//
//  Address 是纯值类型，不涉及 io，因此这些用例不需要 io 线程池。
// ============================================================================
#include <netmech/common.hpp>

#include <string>

#include "test_framework.hpp"

using namespace netmech;

// ---------------------------------------------------------------------------
// 1. 默认构造 = 0.0.0.0:0
// ---------------------------------------------------------------------------
NETMECH_TEST(address_default_is_wildcard)
{
    const Address a;
    CHECK_EQ(a.port(), 0);
    CHECK(a.is_v4());
    CHECK(a.is_unspecified());
    CHECK_EQ(a.to_string(), std::string("0.0.0.0:0"));
}

// ---------------------------------------------------------------------------
// 2. any(port)
// ---------------------------------------------------------------------------
NETMECH_TEST(address_any_with_port)
{
    const Address a = Address::any(9000);
    CHECK_EQ(a.port(), 9000);
    CHECK(a.is_unspecified());
    CHECK_EQ(a.to_string(), std::string("0.0.0.0:9000"));
}

// ---------------------------------------------------------------------------
// 3. (ip, port) 构造
// ---------------------------------------------------------------------------
NETMECH_TEST(address_from_ip_and_port)
{
    const Address a("127.0.0.1", 502);
    CHECK_EQ(a.port(), 502);
    CHECK(a.is_v4());
    CHECK(!a.is_unspecified());
    CHECK_EQ(a.to_string(), std::string("127.0.0.1:502"));
}

// ---------------------------------------------------------------------------
// 4. 非法 ip 退化为通配，不抛异常
// ---------------------------------------------------------------------------
NETMECH_TEST(address_bad_ip_degrades_to_wildcard)
{
    const Address a("not-an-ip", 80);
    CHECK(a.is_unspecified());
    CHECK_EQ(a.port(), 0);
}

// ---------------------------------------------------------------------------
// 5. parse "1.2.3.4:502"
// ---------------------------------------------------------------------------
NETMECH_TEST(address_parse_ipv4)
{
    const auto a = Address::parse("1.2.3.4:502");
    REQUIRE(a.has_value());
    CHECK_EQ(a->port(), 502);
    CHECK(a->is_v4());
    CHECK_EQ(a->to_string(), std::string("1.2.3.4:502"));
}

// ---------------------------------------------------------------------------
// 6. parse IPv6（带方括号）
// ---------------------------------------------------------------------------
NETMECH_TEST(address_parse_ipv6)
{
    const auto a = Address::parse("[::1]:9000");
    REQUIRE(a.has_value());
    CHECK_EQ(a->port(), 9000);
    CHECK(!a->is_v4());
    CHECK_EQ(a->to_string(), std::string("[::1]:9000"));
}

// ---------------------------------------------------------------------------
// 7. parse ":9000" == 0.0.0.0:9000
// ---------------------------------------------------------------------------
NETMECH_TEST(address_parse_port_only)
{
    const auto a = Address::parse(":9000");
    REQUIRE(a.has_value());
    CHECK_EQ(a->port(), 9000);
    CHECK(a->is_unspecified());
}

// ---------------------------------------------------------------------------
// 8. parse 非法输入
// ---------------------------------------------------------------------------
NETMECH_TEST(address_parse_rejects_invalid)
{
    CHECK(!Address::parse("").has_value());            // 空
    CHECK(!Address::parse("1.2.3.4").has_value());     // 无端口
    CHECK(!Address::parse("1.2.3.4:").has_value());    // 端口为空
    CHECK(!Address::parse("1.2.3.4:abc").has_value()); // 端口非数字
    CHECK(!Address::parse("1.2.3.4:70000").has_value()); // 端口越界
    CHECK(!Address::parse("bad-ip:80").has_value());   // ip 非法
    CHECK(!Address::parse("[::1:80").has_value());     // 方括号不闭合
}

// ---------------------------------------------------------------------------
// 9. 比较运算符
// ---------------------------------------------------------------------------
NETMECH_TEST(address_equality)
{
    const Address a("10.0.0.1", 502);
    const Address b("10.0.0.1", 502);
    const Address c("10.0.0.1", 503);
    const Address d("10.0.0.2", 502);

    CHECK(a == b);
    CHECK(!(a != b));
    CHECK(a != c);
    CHECK(a != d);
}

// ---------------------------------------------------------------------------
// 10. 端口 0 与通配地址是可区分的
// ---------------------------------------------------------------------------
NETMECH_TEST(address_zero_port_is_not_unspecified_when_ip_given)
{
    const Address a("192.168.1.10", 0);
    CHECK_EQ(a.port(), 0);
    CHECK(!a.is_unspecified());   // ip 明确，只是端口未定
}
