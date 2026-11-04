// =============================================================================
//  netmech - 错误码类别实现
// =============================================================================
//  文件    : error.cpp
//  用途    : 实现 net_category() / net_boost_category() / from_boost_error_code()
//            这几个**非 inline** 函数（inline 的 make_error_code /
//            make_boost_error_code 由公共头提供，此处**不得**重复定义）。
//
//  ★ 为什么 from_boost_error_code 的实现放在 .cpp
//    它要拿 `asio::error::eof` 这类常量和 `std::error_code` 比相等，而这些
//    常量只存在于 Asio 头里。放进公共头就意味着**装出去的 netmech 强制
//    使用者装一套 Asio**；移到 .cpp 后，公共头只剩一个 `std::error_code`
//    参数，语义与行为完全不变。
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

// from_boost_error_code 的实现要拿 asio 的错误常量比相等。
// 只在 .cpp 里引 —— 公共头一行 asio 都不出现。
#include <asio/error.hpp>

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

std::optional<NetErrc> from_boost_error_code(const std::error_code& ec) noexcept
{
    if (!ec) {
        return std::nullopt;
    }

    // 1) 本库自己产生的错误码：先认 net_category，再兼容历史的 boost 类别。
    if (ec.category() == net_category()) {
        const int ev = ec.value();
        if (ev >= static_cast<int>(NetErrc::ok)
            && ev <= static_cast<int>(NetErrc::datagram_truncated)) {
            return static_cast<NetErrc>(ev);
        }
        return std::nullopt;
    }
    if (ec.category() == net_boost_category()) {
        const int ev = ec.value() - kNetErrcBase;
        if (ev >= static_cast<int>(NetErrc::ok)
            && ev <= static_cast<int>(NetErrc::datagram_truncated)) {
            return static_cast<NetErrc>(ev);
        }
        return std::nullopt;
    }

    // 2) 把常见的 Asio / 系统连接类错误归一到模块语义，便于上层统一处理。
    if (ec == asio::error::eof
        || ec == asio::error::connection_reset
        || ec == asio::error::connection_aborted
        || ec == asio::error::broken_pipe
        || ec == asio::error::operation_aborted) {
        return NetErrc::link_closed;
    }
    if (ec == asio::error::not_connected) {
        return NetErrc::not_connected;
    }
    if (ec == asio::error::timed_out) {
        return NetErrc::keepalive_timeout;
    }
    if (ec == asio::error::invalid_argument) {
        return NetErrc::invalid_argument;
    }
    return std::nullopt;
}

} // namespace netmech
