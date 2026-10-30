// =============================================================================
//  NetMechanism - 错误码类别实现
// =============================================================================
//  文件    : error.cpp
//  用途    : 实现 net_category() / net_boost_category() / net_errc_message()
//            这三个**非 inline** 函数（inline 的 make_error_code /
//            make_boost_error_code / from_boost_error_code 由公共头提供，
//            此处**不得**重复定义，否则 ODR 冲突）。
//  编码    : UTF-8
//
//  ★ 两套 category 的关系
//  ----------------------
//  * `std::error_category`（net_category）用于 std 侧互操作；
//  * `std::error_category`（net_boost_category）用于库内部 Asio
//    原生错误码（Mechanism::listen/connect 的返回值类型）。
//  两者 message() 文本完全一致，值域经 `kNetErrcBase` 偏移区分，互不冲突。
//
//  ★ 线程安全
//  ----------
//  两个 category 均为函数内 static（C++11 起保证线程安全的一次性初始化），
//  生命周期覆盖整个进程，可在全局析构期安全调用。
// =============================================================================
#include "netmech_srcpch.hpp"

#include "netmech/common.hpp"

#include <string>
#include <system_error>

namespace netmech {

std::string net_errc_message(int ev) noexcept
{
    // 注意：本函数在最底层，任何异常都必须吞掉（noexcept）。
    // 用 switch 而非查表，避免静态初始化顺序问题。
    switch (static_cast<NetErrc>(ev)) {
    case NetErrc::ok:
        return "success";
    case NetErrc::not_initialized:
        return "network runtime not initialized";
    case NetErrc::already_initialized:
        return "network runtime already initialized";
    case NetErrc::invalid_argument:
        return "invalid argument";
    case NetErrc::duplicate_endpoint:
        return "duplicate endpoint";
    case NetErrc::buffer_full:
        return "send buffer full";
    case NetErrc::not_connected:
        return "not connected";
    case NetErrc::link_closed:
        return "link closed";
    case NetErrc::accept_failed:
        return "accept failed";
    case NetErrc::keepalive_timeout:
        return "keepalive timeout";
    case NetErrc::handler_rejected:
        return "handler rejected (factory returned null, or on_open returned false)";
    case NetErrc::cancelled:
        return "operation cancelled";
    case NetErrc::datagram_truncated:
        return "datagram truncated (payload larger than receive buffer)";
    }
    return "unknown net error";
}

namespace {

/// std 侧类别。
class NetCategoryImpl final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "netmech"; }

    [[nodiscard]] std::string message(int ev) const override
    {
        try {
            return net_errc_message(ev);
        } catch (...) {
            // noexcept 语义的兜底：绝不让异常穿过 error_category。
            return std::string("unknown net error");
        }
    }
};

/// boost 侧类别。
///
/// @note 传入的值已含 `kNetErrcBase` 偏移，因此 message() 先做减法还原，
///       再复用同一份文本，保证两套 category 的消息一致。
class NetBoostCategoryImpl final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "netmech"; }

    [[nodiscard]] std::string message(int ev) const override
    {
        try {
            return net_errc_message(ev - kNetErrcBase);
        } catch (...) {
            return std::string("unknown net error");
        }
    }
};

} // namespace

const std::error_category& net_category() noexcept
{
    // 函数内 static：C++11 起线程安全的一次性初始化，且无静态初始化顺序问题。
    static const NetCategoryImpl instance;
    return instance;
}

const std::error_category& net_boost_category() noexcept
{
    static const NetBoostCategoryImpl instance;
    return instance;
}

} // namespace netmech
