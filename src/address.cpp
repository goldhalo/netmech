// =============================================================================
//  NetMechanism - Address 实现
// =============================================================================
//  文件    : address.cpp
//  用途    : 实现 netmech::Address（IPv4 / IPv6 字面量 + 端口）。
//  编码    : UTF-8
//
//  设计要点
//  --------
//  * 公共头不暴露 asio：Address 内部只存 128 位地址 + 端口 + 地址族，
//    与 asio 的互转全部收敛在本文件。
//  * 所有构造函数都是 noexcept：非法输入退化为 0.0.0.0:0，不抛异常。
//  * **不做域名解析**：需要主机名时由调用方自行解析成 IP 再传进来。
// =============================================================================
#include "netmech_srcpch.hpp"

#include "netmech/common.hpp"

#include "address_internal.hpp"

#include <array>
#include <string>

namespace netmech {

namespace {

/// 把 asio 地址拆成 (高 64 位, 低 64 位, 地址族)。
void store(const asio::ip::address& addr,
           std::uint64_t& hi, std::uint64_t& lo, std::uint8_t& family) noexcept
{
    if (addr.is_v4()) {
        family = 0;
        hi     = 0;
        lo     = addr.to_v4().to_uint();
        return;
    }
    family = 1;
    const auto bytes = addr.to_v6().to_bytes();
    hi = 0;
    lo = 0;
    for (int i = 0; i < 8; ++i) {
        hi = (hi << 8) | static_cast<std::uint64_t>(bytes[static_cast<std::size_t>(i)]);
    }
    for (int i = 8; i < 16; ++i) {
        lo = (lo << 8) | static_cast<std::uint64_t>(bytes[static_cast<std::size_t>(i)]);
    }
}

/// 由 (高, 低, 地址族) 还原 asio 地址。
[[nodiscard]] asio::ip::address load(std::uint64_t hi, std::uint64_t lo,
                                     std::uint8_t family) noexcept
{
    if (family == 0) {
        return asio::ip::address_v4(
            static_cast<asio::ip::address_v4::uint_type>(lo & 0xFFFFFFFFULL));
    }
    std::array<unsigned char, 16> bytes{};
    for (int i = 0; i < 8; ++i) {
        bytes[static_cast<std::size_t>(i)] =
            static_cast<unsigned char>((hi >> (56 - 8 * i)) & 0xFFULL);
    }
    for (int i = 0; i < 8; ++i) {
        bytes[static_cast<std::size_t>(8 + i)] =
            static_cast<unsigned char>((lo >> (56 - 8 * i)) & 0xFFULL);
    }
    return asio::ip::address_v6(bytes);
}

/// 解析十进制端口；非法或越界返回 false。
bool parse_port(std::string_view text, std::uint16_t& out) noexcept
{
    if (text.empty() || text.size() > 5) {
        return false;
    }
    unsigned long value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned long>(c - '0');
        if (value > 65535UL) {
            return false;
        }
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

} // namespace

Address::Address() noexcept = default;

Address::Address(std::string_view ip, std::uint16_t port) noexcept
{
    m_port = port;
    if (ip.empty()) {
        return;   // 空 ip 视为通配 0.0.0.0
    }
    std::error_code ec;
    const asio::ip::address addr = asio::ip::make_address(std::string(ip), ec);
    if (ec) {
        m_port = 0;   // 非法输入退化为 0.0.0.0:0
        return;
    }
    store(addr, m_hi, m_lo, m_family);
}

std::optional<Address> Address::parse(std::string_view text) noexcept
{
    if (text.empty()) {
        return std::nullopt;
    }

    // 约定：最后一段 ":" 之后是端口；IPv6 需写成 [::1]:9000。
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }

    std::string_view ip_part   = text.substr(0, colon);
    const std::string_view port_part = text.substr(colon + 1);

    std::uint16_t port = 0;
    if (!parse_port(port_part, port)) {
        return std::nullopt;
    }

    // 去掉 IPv6 的方括号；不带方括号时无法与端口区分，按 IPv4 处理。
    if (!ip_part.empty() && ip_part.front() == '[') {
        if (ip_part.size() < 2 || ip_part.back() != ']') {
            return std::nullopt;
        }
        ip_part = ip_part.substr(1, ip_part.size() - 2);
    }

    Address addr;
    addr.m_port = port;
    if (ip_part.empty()) {
        return addr;   // ":9000" == 0.0.0.0:9000
    }

    std::error_code ec;
    const asio::ip::address any = asio::ip::make_address(std::string(ip_part), ec);
    if (ec) {
        return std::nullopt;
    }
    store(any, addr.m_hi, addr.m_lo, addr.m_family);
    return addr;
}

Address Address::any(std::uint16_t port) noexcept
{
    Address addr;
    addr.m_port = port;
    return addr;
}

bool Address::is_unspecified() const noexcept
{
    return m_hi == 0 && m_lo == 0;
}

std::string Address::address_string() const
{
    return load(m_hi, m_lo, m_family).to_string();
}

std::string Address::to_string() const
{
    const std::string ip = load(m_hi, m_lo, m_family).to_string();
    if (m_family == 0) {
        return ip + ":" + std::to_string(m_port);
    }
    return "[" + ip + "]:" + std::to_string(m_port);
}


// =============================================================================
//  Address ⇄ asio 的转换（仅实现层可见）
// =============================================================================

namespace detail {

/// 直接读写 Address 的私有表示（common.hpp 里已 friend 本类型）。
struct AddressCodec {
    static asio::ip::address to_asio(const Address& addr) noexcept
    {
        return load(addr.m_hi, addr.m_lo, addr.m_family);
    }

    static Address from_asio(const asio::ip::address& src, std::uint16_t port) noexcept
    {
        Address addr;
        addr.m_port = port;
        store(src, addr.m_hi, addr.m_lo, addr.m_family);
        return addr;
    }
};

asio::ip::address to_asio_address(const Address& addr) noexcept
{
    return AddressCodec::to_asio(addr);
}

Address from_asio_address(const asio::ip::address& addr, std::uint16_t port) noexcept
{
    return AddressCodec::from_asio(addr, port);
}

asio::ip::tcp::endpoint to_asio_endpoint(const Address& addr) noexcept
{
    return asio::ip::tcp::endpoint(AddressCodec::to_asio(addr), addr.port());
}

Address from_asio_endpoint(const asio::ip::tcp::endpoint& ep) noexcept
{
    return AddressCodec::from_asio(ep.address(), ep.port());
}

asio::ip::udp::endpoint to_asio_udp_endpoint(const Address& addr) noexcept
{
    return asio::ip::udp::endpoint(AddressCodec::to_asio(addr), addr.port());
}

Address from_asio_udp_endpoint(const asio::ip::udp::endpoint& ep) noexcept
{
    return AddressCodec::from_asio(ep.address(), ep.port());
}

} // namespace detail

} // namespace netmech
