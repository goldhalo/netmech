// =============================================================================
//  netmech - TLS 配置（v0.91 引入）
// =============================================================================
//  文件    : netmech/tls.hpp
//  用途    : 定义 TlsConfig —— 进程内 TLS 终结所需的全部配置。
//  依赖    : **只用 C++17 标准库**。公共头一行 asio / OpenSSL 都不引。
//  编码    : UTF-8
//
//  ★ 定位（v0.91）
//  ---------------
//  * **进程内 TLS 终结**：netmech 自己完成握手，业务拿到的 Conn 已经是
//    解密后的字节流，`ConnHandler` 完全不用改。
//  * **自签 + 指纹 pin 优先**：工业设备基本用自签证书，没有公网 CA。
//    因此默认形态是"不校验证书链，但必须给出对端证书的 SHA-256 指纹"。
//    没有 pin、也没开链校验的配置会被**直接拒绝**（见下）。
//  * **只做 TLS over TCP**。UDP 的 DTLS **不在** 本版本范围内。
//  * 依赖 OpenSSL（构建时 `NETMECH_TLS=ON`）。抽象层已经留好：
//    将来换 mbedTLS / BearSSL 只需替换传输实现，本结构体不用改。
//
//  ★ 三条硬规则
//  -------------
//  [1] **服务端**必须同时提供 cert_chain_file 与 private_key_file，
//      否则 listen() 直接返回 invalid_argument。
//  [2] **客户端**必须二选一：给 pinned_sha256，或把 verify_certificate
//      置为 true 走证书链校验。两者都没有 → connect() 返回 invalid_argument。
//      这条规则存在的唯一目的：避免"以为加密了，其实谁都能中间人"。
//  [3] **指纹格式**宽松解析：大小写不敏感，允许 `:`、空格、`-` 分隔，
//      例如 `AB:CD:...` 与 `abcd...` 等价。只比对 32 字节 SHA-256。
//
//  ★ 与 Conn 生命周期的关系
//  ------------------------
//  握手发生在 `linkable->on_accept()` / `on_linkup()` **之前**。
//  即：业务拿到 Conn 的那一刻，握手已经成功；握手失败只会走
//  `Linkable::on_accept_failed()`（服务端）或 `on_linkfail()`（客户端），
//  **不会**产生 on_linkdown。
// =============================================================================
#pragma once

#include "netmech/export.hpp"

#include <cstdint>
#include <string>

namespace netmech {

/// TLS 协议版本下限。
enum class TlsVersion {
    tls12 = 0,  ///< TLS 1.2（默认；兼容性最好，工业现场的老设备多为 1.2）
    tls13 = 1,  ///< TLS 1.3（需要 OpenSSL 1.1.1+；握手更快、套件更少）
};

/// 一次 TLS 终结所需的配置。
///
/// @par 服务端最小配置
/// @code
///   TlsConfig tls;
///   tls.cert_chain_file = "server.crt";   // PEM，可含中间证书
///   tls.private_key_file = "server.key";  // PEM（PKCS#8 或传统格式）
///   opts.tls = tls;
///   mech->listen(Address::any(8443), link, opts);
/// @endcode
///
/// @par 客户端最小配置（自签 + 指纹 pin）
/// @code
///   TlsConfig tls;
///   tls.pinned_sha256 = "AB:CD:...";      // 服务端证书的 SHA-256
///   tls.server_name   = "device.local";   // 可选：SNI
///   opts.tls = tls;
///   mech->connect(Address{"10.0.0.5", 8443}, link, opts);
/// @endcode
///
/// @note 本结构体是**值类型**：可以在多次 listen/connect 之间复用、修改。
///       真正的 `SSL_CTX` 由库在 listen()/connect() 时按此配置构建，
///       调用方不需要（也无法）持有 OpenSSL 对象。
struct TlsConfig {
    // -------------------------------------------------------------------------
    // 证书与私钥（服务端必需；客户端做双向认证时同样需要）
    // -------------------------------------------------------------------------

    /// 证书链文件（PEM）。**服务端必填**。
    ///
    /// @note 文件里可以包含多张证书（叶子在前、中间证书在后）。
    /// @note 路径由 OpenSSL 直接打开，因此：
    ///       Windows 下 UTF-8 中文路径**不受支持**（OpenSSL 用 ANSI 打开）；
    ///       请放在 ASCII 路径下，或先用相对路径切换工作目录。
    std::string cert_chain_file;

    /// 私钥文件（PEM）。**服务端必填**。
    std::string private_key_file;

    /// 私钥口令（私钥被加密时必需）。空 = 私钥无口令。
    ///
    /// @warning 口令以明文保存在结构体里，进程内存 dump 可见。
    ///          生产环境更推荐用**无口令私钥 + 严格文件权限**。
    std::string private_key_password;

    // -------------------------------------------------------------------------
    // 信任锚
    // -------------------------------------------------------------------------

    /// CA 证书文件（PEM）。仅在 `verify_certificate == true` 时使用。
    ///
    /// @note 为空且 `verify_certificate == true` 时，使用 OpenSSL 的
    ///       **系统默认 CA 路径**（`SSL_CTX_set_default_verify_paths`）。
    /// @note 本版本只支持**单文件**（PEM 里可以拼多张 CA），不支持 CA 目录。
    std::string ca_file;

    // -------------------------------------------------------------------------
    // 指纹 pin（自签场景的主要手段）
    // -------------------------------------------------------------------------

    /// 对端证书的 SHA-256 指纹（十六进制）。
    ///
    /// - 非空 ⇒ 握手后必须与对端**叶子证书**的指纹完全一致，否则以
    ///   `NetErrc::tls_cert_rejected` 拒绝；
    /// - 空 ⇒ 不做指纹校验（此时客户端必须开 `verify_certificate`）。
    ///
    /// @note 允许 `:` / 空格 / `-` 分隔，大小写不敏感。
    /// @note pin 的是**整张证书**（不是公钥、也不是 SPKI），换证书就得换 pin。
    std::string pinned_sha256;

    // -------------------------------------------------------------------------
    // 名字与版本
    // -------------------------------------------------------------------------

    /// SNI 主机名（客户端使用）。
    ///
    /// - 非空 ⇒ 握手中发送 SNI（虚拟主机、多证书部署需要）；
    /// - 同时 `verify_certificate == true` 时，还要求证书的 SAN/CN 匹配该名字。
    ///
    /// @note netmech 的 `Address` **不含域名**（不做 DNS 解析），因此 SNI
    ///       必须在这里单独给。服务端会忽略本字段。
    std::string server_name;

    /// 协议版本下限，默认 TLS 1.2。
    ///
    /// @note SSLv3 / TLS1.0 / TLS1.1 在本库中**无法启用**（硬编码最小值），
    ///       它们早已被主流对端拒绝，且存在已知攻击面。
    TlsVersion min_version = TlsVersion::tls12;

    /// 是否校验对端证书链（默认 false —— 自签场景）。
    ///
    /// - `false` ⇒ 握手阶段不做链校验（`verify_none`），安全性完全依赖
    ///   `pinned_sha256`。**没有 pin 时会被拒绝**（见文件头规则 [2]）；
    /// - `true`  ⇒ `verify_peer`，用 `ca_file`（或系统默认 CA）校验。
    bool verify_certificate = false;

    /// 握手超时（毫秒）。0 = 使用默认值 10000。
    ///
    /// @note **不能关闭**：没有超时的握手会被一个不回应任何数据的 TCP
    ///       连接永久占住，占满配额后整个监听器就废了。
    std::uint32_t handshake_timeout_ms = 10000;
};

} // namespace netmech
