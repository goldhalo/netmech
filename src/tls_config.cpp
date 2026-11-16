// =============================================================================
//  netmech - TLS 配置校验
// =============================================================================
//  文件    : src/tls_config.cpp
//  用途    : 校验 TlsConfig 是否自洽 + 指纹文本归一化。
//  依赖    : 只用标准库 —— **不引 OpenSSL**，因此 `NETMECH_TLS=OFF` 的构建
//            同样参与编译。理由见 tls_internal.hpp 的说明：
//            配置错误（invalid_argument）与构建缺失（tls_unsupported）
//            必须能被区分开，否则排查方向会完全跑偏。
//  编码    : UTF-8
//
//  ★ 为什么客户端必须有 pin 或链校验
//  ---------------------------------
//  "握手成功"不等于"对端可信"。自签场景下若既不 pin 也不校验证书链，
//  OpenSSL 会接受任何证书 —— 这条连接是加密的，但中间人可以随便插。
//  本库宁可让 connect() 直接返回 invalid_argument，也不提供这种
//  "看起来安全"的配置。
// =============================================================================
#include "netmech_srcpch.hpp"

#include "tls_internal.hpp"

#include <cctype>

namespace netmech {

namespace tls_detail {

std::string normalize_fingerprint(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (char ch : in) {
        if (ch == ':' || ch == '-' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

bool is_hex_string(const std::string& s) noexcept
{
    if (s.empty()) {
        return false;
    }
    for (char ch : s) {
        if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) {
            return false;
        }
    }
    return true;
}

} // namespace tls_detail

error_code_t validate_tls_config(const TlsConfig& cfg, bool server)
{
    // 指纹格式（两侧共用）：SHA-256 = 32 字节 = 64 个十六进制字符。
    if (!cfg.pinned_sha256.empty()) {
        const std::string norm = tls_detail::normalize_fingerprint(cfg.pinned_sha256);
        if (norm.size() != 64 || !tls_detail::is_hex_string(norm)) {
            return make_boost_error_code(NetErrc::invalid_argument);
        }
    }

    // ALPN：线上格式用**一个字节**表示长度，所以每项必须是 1..255。
    // 空串在握手时才暴露（表现为协商不出东西），不如在受理阶段就拒掉。
    for (const std::string& proto : cfg.alpn) {
        if (proto.empty() || proto.size() > 255) {
            return make_boost_error_code(NetErrc::invalid_argument);
        }
    }

    if (server) {
        // 服务端没有证书/私钥就无法完成握手 —— 在 listen() 时就说清楚，
        // 而不是等第一个连接来了才通过 on_accept_failed 报出来。
        if (cfg.cert_chain_file.empty() || cfg.private_key_file.empty()) {
            return make_boost_error_code(NetErrc::invalid_argument);
        }
        return error_code_t{};
    }

    // 客户端：pin 与链校验必须二选一（见文件头）。
    if (cfg.pinned_sha256.empty() && !cfg.verify_certificate) {
        return make_boost_error_code(NetErrc::invalid_argument);
    }
    return error_code_t{};
}

} // namespace netmech
