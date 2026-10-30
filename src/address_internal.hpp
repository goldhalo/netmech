// =============================================================================
//  NetMechanism - 地址转换（内部头，仅 src/ 使用）
// =============================================================================
//  文件    : address_internal.hpp
//  用途    : 在公共类型 Address 与 asio 地址/端点之间转换。
//            公共头不引 asio，所以转换只能放在实现层。
//  编码    : UTF-8
// =============================================================================
#pragma once

#include <cstdint>

#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>

#include "netmech/common.hpp"

namespace netmech {
namespace detail {

/// Address → asio 地址（不含端口）。
[[nodiscard]] asio::ip::address to_asio_address(const Address& addr) noexcept;

/// asio 地址 + 端口 → Address。
[[nodiscard]] Address from_asio_address(const asio::ip::address& addr,
                                        std::uint16_t port) noexcept;

/// Address → asio tcp 端点。
[[nodiscard]] asio::ip::tcp::endpoint to_asio_endpoint(const Address& addr) noexcept;

/// asio tcp 端点 → Address。
[[nodiscard]] Address from_asio_endpoint(const asio::ip::tcp::endpoint& ep) noexcept;

/// Address → asio udp 端点。
[[nodiscard]] asio::ip::udp::endpoint to_asio_udp_endpoint(const Address& addr) noexcept;

/// asio udp 端点 → Address。
[[nodiscard]] Address from_asio_udp_endpoint(const asio::ip::udp::endpoint& ep) noexcept;

} // namespace detail
} // namespace netmech
