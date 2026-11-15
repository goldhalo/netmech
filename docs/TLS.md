# TLS（进程内终结，v0.91）

> 一句话：`listen()` / `connect()` 传一个 `TlsConfig`，业务拿到的 `Conn`
> 就**已经是解密后的字节流** —— `ConnHandler` 一行都不用改。

---

## 1. 定位与边界

| 项 | 结论 |
|---|---|
| 形态 | **进程内 TLS 终结**（netmech 自己握手），不是"交给 nginx" |
| 版本 | TLS 1.2 / 1.3（SSLv3、TLS1.0、TLS1.1 **无法启用**） |
| 信任模型 | **自签 + 指纹 pin** 优先；也支持 CA 证书链校验 |
| 传输 | 只做 **TLS over TCP** |
| 不做 | DTLS（UDP）、ALPN、客户端会话缓存、证书热加载 |

**为什么现在做**：0.90 之前 `Conn` 直接持有 `asio::ip::tcp::socket`，读写关闭
全直达 socket，外部没有任何接缝能插入 TLS。v0.91 先抽出内部的
`Transport` 抽象（明文实现行为与 0.90 逐字等价），TLS 只是第二个实现。

---

## 2. 构建开关

### 2.1 默认 **不编** TLS

```bash
# 什么都不加 = 不编 TLS：产物、依赖、体积与 0.90 完全一致
cmake -S . -B build

# 显式要求才编（缺 OpenSSL 会直接报错，不会悄悄降级）
cmake -S . -B build -DNETMECH_TLS=ON
```

* `NETMECH_TLS` 默认 **OFF**。不显式开，就**不会**去找 OpenSSL、也**不会**
  引入任何第三方依赖。
* `NETMECH_TLS=OFF` 的构建里公共 API **照样存在**：传 `tls` 选项会得到
  `NetErrc::tls_unsupported`（配置写错仍是 `invalid_argument`，两者分开，
  便于区分"没编进来"和"你写错了"）。
* 显式 `ON` 但找不到 OpenSSL → **配置期硬失败**。
  理由：用户已经明确表达了意图，静默降级只会得到"编出来还是不支持 TLS"。

需要 **OpenSSL 1.1.1+**（TLS 1.3 需要 1.1.1+；建议 3.x LTS）。

### 2.2 OpenSSL 必须由构建方准备（只有一条路）

**netmech 不带 OpenSSL**，也不提供"仓内自带一份"的第二条路径。
Windows 与 Linux 行为完全一致：先装好 OpenSSL，再 `-DNETMECH_TLS=ON`。

```bash
# Linux
sudo apt install -y libssl-dev            # 或发行版等价命令
cmake -S . -B build -DNETMECH_TLS=ON

# Windows（vcpkg）
vcpkg install openssl:x64-windows
cmake -S . -B build -DNETMECH_TLS=ON -DOPENSSL_ROOT_DIR=<vcpkg 前缀>

# 非标准安装位置
cmake -S . -B build -DNETMECH_TLS=ON -DOPENSSL_ROOT_DIR=/opt/openssl
```

CMake 用标准 `find_package(OpenSSL)` 探测，因此：

* **默认动态链接**：发行物依赖 `libssl` / `libcrypto`。Windows 上要随包带
  `libssl-3-x64.dll` / `libcrypto-3-x64.dll`，或让用户自己装；
* **想要单文件交付**（不依赖 OpenSSL 运行时），把 OpenSSL 静态链进去即可，
  用的还是标准 CMake 变量：

  ```bash
  cmake -S . -B build -DNETMECH_TLS=ON \
        -DOPENSSL_USE_STATIC_LIBS=TRUE -DOPENSSL_ROOT_DIR=/opt/openssl-static
  ```

  两条注意：① 静态 OpenSSL 的 **CRT 必须与本库一致**（本库固定 `/MD`，
  所以不能用 vcpkg 的 `x64-windows-static`，它是 `/MT`）；
  ② 一旦静态化，**OpenSSL 的 CVE 就由我们自己负责**，发版前要确认版本。

> 曾经考虑过"把 OpenSSL 放 `thirdparty/`、CMake 自己识别静态库并链进 netmech"，
> 但那会引入平台差异（lib 命名、CRT、Linux 还要 `--exclude-libs` 隐藏符号），
> 组合问题比省下的步骤多，**已放弃**。

### 2.3 分发形态

| 形态 | 发行物 | 使用方要装 OpenSSL |
|---|---|---|
| 动态（默认） | 库 + 系统 OpenSSL | 是（导出包 `find_dependency(OpenSSL)`） |
| 静态链入 OpenSSL | 单个库文件 | 否 |

**发布物只有 netmech 自己的库**（Windows 的 `netmech.dll`、Linux 的
`libnetmech.so`），OpenSSL 的运行时由使用方提供，两边完全对等：

| 平台 | 使用方需要提供 | 实测（SHARED + TLS，Release） |
|---|---|---|
| Windows | `libssl-3-x64.dll` + `libcrypto-3-x64.dll`（放在 PATH 或 exe 同目录） | `netmech.dll` **443 KB**；`dumpbin /dependents` 确认依赖这两个 DLL；导出表里 OpenSSL 符号 **0** 个 |
| Linux | 系统 `libssl.so.3` + `libcrypto.so.3`（发行版包） | `libnetmech.so` 通过 DT_NEEDED 依赖它们 |

静态链入的代价有实测参考（Linux x64 / OpenSSL 3.0.15 / `no-legacy` / Release）：
`libnetmech.so` 从 ~0.4 MB 涨到 **5.3 MB**（strip 后；未 strip 6.0 MB）。
另外，netmech 被**静态链进多个 `.so`** 时，每个 `.so` 会各带一份 OpenSSL，
内存与 CVE 面都按份数放大 —— 这种部署建议仍用动态链接。

> **Windows 上的 `OPENSSL_Applink` 坑（与本库无关，但很容易踩）**：
> 应用若自己把 `FILE*` 交给 OpenSSL（例如 `PEM_write_X509(FILE*, ...)`、
> `BIO_new_fp`），在 OpenSSL 是独立 DLL 时会直接 abort 并打印
> `OPENSSL_Uplink: no OPENSSL_Applink` —— 因为 `FILE*` 属于调用方的 CRT。
> 解决：改用 `BIO_new_file` / `PEM_write_bio_*`，或把 OpenSSL 的
> `applink.c` 编进应用。
> **netmech 的公共 API 不暴露任何 `FILE*`**，所以库本身不受影响；
> 受影响的只是"自己用 OpenSSL C API 读写证书文件"的调用方。

> **为什么不做"TLS 单独一个可选库"**：那需要在公共 API 上加一层后端注册钩子
> （`set_tls_backend(factory)`），并让 `Conn` 走一次间接调用才能建传输。
> 收益是 netmech 核心永远零依赖，代价是接口面积 + 一个运行期失败点
> （忘了注册 → 运行时才炸）。当前需求是"一个进程内把 TLS 终结掉"，
> 现方案已经够用，故不做。

### 2.4 CI 覆盖范围

| Job | 平台 | 是否门禁 |
|---|---|---|
| `linux-tls` | Ubuntu + 发行版 `libssl-dev` | ✅ 门禁 |
| `sanitizers-tls` / `tsan-tls` | Ubuntu + ASan/UBSan/TSan | ✅ 门禁 |
| `windows-tls` | Windows + choco 装的 OpenSSL | ⚠️ **非门禁** |

Windows 的 TLS 为什么不做门禁：runner 上 choco 装出来的 OpenSSL 版本与打包形态
（DLL 导入库 / `/MT` 静态库）不由我们控制，曾因此把构建打红，而同一份代码在
本机（自编 OpenSSL 3.0.15 + MSVC + `/WX`）96 个用例全绿。TLS 的逻辑门禁放在
Linux 三个 job 上；Windows 那个 job 只作为"环境漂移"的提示信号。

本机自测（推荐发版前跑一遍）：

```bat
perl Configure VC-WIN64A shared no-tests no-legacy no-asm --prefix=D:\ossl
nmake && nmake install_sw
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DNETMECH_TLS=ON -DOPENSSL_ROOT_DIR=D:\ossl -DNETMECH_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
set PATH=D:\ossl\bin;%PATH%
build\bin\netmech_ut.exe
```

---

## 3. 服务端

```cpp
#include <netmech/net.hpp>
using namespace netmech;

TlsConfig tls;
tls.cert_chain_file = "server.crt";   // PEM，可含中间证书（叶子在前）
tls.private_key_file = "server.key";
tls.private_key_password = "";        // 私钥无口令时留空

ListenOptions opts;
opts.tls = tls;                       // 有值 = 这个监听器只接受 TLS
opts.backlog = 128;

auto ec = mech->listen(Address::any(8443), link, opts);
```

* `cert_chain_file` / `private_key_file` 缺一个 → `listen()` 返回
  `invalid_argument`（不会留下半个监听器）。
* 证书在 `listen()` 时加载**一次**，之后每条连接共用同一个 `SSL_CTX`。
* 同一个 `Mechanism` 上可以有明文监听器与 TLS 监听器并存。

### 握手失败怎么知道

服务端在**接管之前**失败（对端不是 TLS、证书被拒、握手超时、并发配额满）时，
不会调用 `on_accept`，而是：

```cpp
class MyLinkable : public Linkable {
    void on_accept_failed(const Address& remote, std::error_code ec) override {
        // 建议至少计数 + 日志：否则"一个连接都收不到"是完全静默的
    }
    std::shared_ptr<ConnHandler> on_accept(Conn& c) override { /* ... */ }
};
```

**一定要接这个回调。** 它默认是空实现，不接就等于把 TLS 握手失败吞掉了。

---

## 4. 客户端（自签 + 指纹 pin）

```cpp
TlsConfig tls;
tls.pinned_sha256 = "AB:CD:EF:...";   // 服务端证书的 SHA-256
tls.server_name   = "device.local";   // 可选：SNI

ConnectOptions opts;
opts.timeout_ms = 3000;               // 只覆盖 TCP 建链
opts.tls = tls;

auto ec = mech->connect(Address{"10.0.0.5", 8443}, link, opts);
if (ec) { /* 未受理：配置错 / 未编译 TLS / 地址非法 */ }
```

规则（`tls.hpp` 文件头规则 [2]）：

> 客户端**必须**二选一：给 `pinned_sha256`，或把 `verify_certificate` 置 `true`。
> 两者都没有 → `connect()` 返回 `invalid_argument`。

这条限制存在的唯一目的：避免"以为加密了，其实谁都能中间人"。

### 指纹怎么算

SHA-256，对象是**整张叶子证书**（不是公钥、不是 SPKI）：

```bash
openssl x509 -in server.crt -noout -fingerprint -sha256
# SHA256 Fingerprint=AB:CD:...
```

库对格式宽松：大小写不敏感，允许 `:` / 空格 / `-` 分隔。

### 用 CA 链校验（可选）

```cpp
tls.verify_certificate = true;
tls.ca_file = "ca.pem";     // 留空 = 用系统默认 CA 路径
tls.server_name = "device.local";   // 同时做主机名校验
```

服务端置 `verify_certificate = true` 等价于要求**客户端证书**（mTLS）。

---

## 5. 握手语义（重要）

* **`on_accept` / `on_linkup` 发生在握手成功之后**。业务拿到 `Conn` 时 TLS 已经
  建立，`read()` 直接就是明文。
* 客户端：`connect()` 的 `timeout_ms` **只覆盖 TCP 建链**；握手有自己的超时
  （`TlsConfig::handshake_timeout_ms`，默认 10s，不能关闭）。
* 握手失败的通知：
  * 服务端 → `on_accept_failed(remote, ec)`；
  * 客户端 → `on_linkfail(remote, ec)`；
  * 两侧都**不会**有 `on_linkdown`（连接从未交给业务）。
* 握手在该连接绑定的 **io 线程**上完成。RSA/ECDSA 运算是 CPU 密集的，
  并发数由 `Config::max_inflight_tls_handshakes`（默认 64）限制，超限直接拒绝
  而不是排队。

---

## 6. 关闭语义与已知取舍

| 场景 | 行为 |
|---|---|
| 本端 `close()` | 尽力发一次 `close_notify`（非阻塞），随后 `shutdown` + `close` |
| 对端不发 close_notify 就断 | 映射为 `NetErrc::tls_truncated`（**不**静默当普通 EOF） |
| `linger_zero = true` | 直接 RST，`close_notify` 一定丢（这本就是"丢尾部数据"的语义） |

**为什么不等对端回 close_notify**：`Conn::close()` 的契约是"同步、noexcept、
不阻塞"，而等待是异步的、且恶意对端可以永不回应 —— 那会挂住 io 线程。

**内存**：每条 TLS 连接除 `socket_buf_size` 之外还有约 30–40 KiB 的
OpenSSL `SSL` + BIO 开销。几百条连接的管理机要实测。

---

## 7. 手工验证（双机 / 本机回环）

```bash
# 1) 生成自签证书（默认 EC P-256，比 RSA 快很多）
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout server.key -out server.crt -days 365 -nodes -subj "/CN=netmech-test"

# 2) 取指纹
openssl x509 -in server.crt -noout -fingerprint -sha256

# 3) 服务端 listen 时带上 cert/key，客户端 connect 时带上面那串指纹
```

自动化用例（`test/test_tls_e2e.cpp`，只在 `NETMECH_TLS=ON` 时执行）：

* `tls_e2e_handshake_and_echo` —— 自签 + 正确指纹：握手成功、TLS 通道回环数据正确；
* `tls_e2e_wrong_pin_rejected` —— 指纹不匹配：`tls_cert_rejected`，且**没有** `on_linkup`；
* `tls_e2e_plain_peer_rejected` —— 明文客户端打 TLS 端口：服务端
  `on_accept_failed = tls_handshake_timeout`，且没有 `on_accept`。

证书是运行时用 OpenSSL C API 生成的（EC P-256 自签），临时文件用完即删，
不往仓库里塞 PEM，也不依赖 `openssl` 命令行。

---

## 8. 还没做的（后续）

* ALPN（HTTP/2、`h2` 协商）；
* 客户端会话缓存 / 恢复（降低重复握手成本）；
* 证书热加载（现在换证书要重新 `listen`）；
* mbedTLS / BearSSL 后端（抽象层已就位，`Transport` 再加一个实现即可）；
* DTLS（UDP）。
