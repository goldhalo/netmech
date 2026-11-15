# Changelog

本文件记录所有值得注意的变更。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

---

## [0.91.0] - 2026-11-15

### 新增

**进程内 TLS 终结（可选，`NETMECH_TLS`，默认 **OFF**）**

- `include/netmech/tls.hpp`：`TlsConfig` / `TlsVersion`。公共头仍然**不引**
  asio / OpenSSL，配置是纯 C++ 值类型。
- `Mechanism::listen(addr, linkable, ListenOptions)` /
  `Mechanism::connect(addr, linkable, ConnectOptions)`：`options.tls` 有值即启用 TLS。
  旧的位置参数重载**完全不变**，既有调用点零改动。
- **自签 + 指纹 pin** 是默认形态：`TlsConfig::pinned_sha256` 给 SHA-256 指纹
  （允许 `:`/空格/`-` 分隔、大小写不敏感）。客户端"既不 pin 也不校验证书链"
  会被 `listen()/connect()` 直接拒绝（`invalid_argument`），避免"假 TLS"。
- `Linkable::on_accept_failed(remote, ec)`：服务端在**接管前**协商失败
  （握手失败/超时/并发配额已满）时的回调。默认空实现，不影响既有派生类。
- 错误码：`tls_handshake_failed` / `tls_handshake_timeout` / `tls_cert_rejected` /
  `tls_unsupported` / `tls_truncated`。
- `Config::max_inflight_tls_handshakes`（默认 64）：全局并发握手上限。
  握手是 CPU 密集操作，而每个 io_context 只有 1 条线程 —— 无上限时一批恶意
  连接就能把正常连接的读写一起拖住。
- 构建：`-DNETMECH_TLS` **默认 OFF**；显式 `ON` 且缺 OpenSSL 时**配置期硬失败**
  （不再静默降级）。**OpenSSL 一律由构建方准备**（`find_package(OpenSSL)`，
  Windows/Linux 行为一致），导出包追加 `find_dependency(OpenSSL)`。
  想单文件交付就静态链 OpenSSL：`-DOPENSSL_USE_STATIC_LIBS=TRUE`
  （CRT 必须同为 `/MD`）。
  *"仓内自带 OpenSSL 静态库"的方案已否决*：它带来平台差异（lib 命名、CRT、
    Linux 需 `--exclude-libs`），组合问题多于收益。

### 变更

**内部：`Conn` 不再直接持有 socket，改为面向 `Transport` 抽象**

- 新增内部接口 `src/transport.hpp`：`async_read_some` / `async_read_exact` /
  `async_write` / `async_handshake` / `close(graceful)`。明文实现是薄转发，
  **行为与 0.90 逐字等价**（86 个既有用例、932 条断言全绿即为证据）。
- 收益：明文与 TLS 可以在同一个 `Mechanism` 上共存；将来换 mbedTLS/BearSSL
  只需再加一个实现（OpenSSL 只出现在 `tls_transport.cpp` 一个 TU 里）。
- `Conn::Impl` 的成员顺序约束从 "`pool` 在 `socket` 之前" 改为
  "`pool` 在 `transport` 之前"，理由不变（析构必须先于 io_context）。

**`-Werror` / `/WX` 只作用于库本体与 `example/`，不再约束 `test/`**

- 库要上生产、示例会被使用者照抄，这两处必须零告警；
- 测试的价值是"行为能过"，而第三方头在新版本里频繁引入弃用警告
  （OpenSSL 4.1 弃用 `X509_check_host`、4.x 把一批 getter 改成返回 `const`），
  为这些去改测试只是拿精力跟工具链赛跑。
- 实现上把 `/WX`、`-Werror` 从 `netmech_compile_options`（PUBLIC interface，
  会一路传给测试/示例/下游）挪到按目标添加：`netmech` 与三个示例目标。
- CI 里的 `NETMECH_WARNINGS_AS_ERRORS=ON` 语义随之收窄；`test/` 仍会打印告警，
  但不再中断构建。

### 修复

**`cmake --install` 在 SHARED 构建下不装 `.dll` / `.so`（打包缺口，0.90 起就存在）**

`install(TARGETS netmech ...)` 只写了 `ARCHIVE DESTINATION lib`，
没有 `RUNTIME`（Windows 的 `.dll`）与 `LIBRARY`（Linux 的 `.so`）。
后果很隐蔽：下游 `find_package(netmech)` 能配置、能编译，**一运行就是
`0xC0000135` / 找不到共享库** —— 因为装出来的包里根本没有库文件本身。
已补上 `RUNTIME DESTINATION bin` 与 `LIBRARY DESTINATION lib`。

### 已知取舍

- **Windows 的 `OPENSSL_Applink`**：应用若自己把 `FILE*` 传给 OpenSSL
  （`PEM_write_X509(FILE*, ...)` / `BIO_new_fp`），在 OpenSSL 为独立 DLL 时会
  abort（`OPENSSL_Uplink: no OPENSSL_Applink`）。netmech 公共 API 不暴露
  `FILE*`，库本身不受影响；测试里的证书生成已改用 `BIO_new_file` 系列。
- **close_notify 是尽力而为**：`Conn::close()` 的契约是"同步、noexcept、不阻塞"，
  而 TLS 优雅关闭需要等待对端回应（恶意对端可以永不回应）。因此关闭时只做一次
  非阻塞 `SSL_shutdown`（发出 close_notify）随即关闭，对端可能观察到 truncation。
  反过来，**收到**不带 close_notify 的 EOF 会被映射成 `NetErrc::tls_truncated`，
  不会静默当成普通 EOF。
- TLS 连接每条约 30–40 KiB 的 OpenSSL 开销，容量规划不要只看 `socket_buf_size`。
- **不做 DTLS**（UDP 侧的 TLS 变体）：`UdpEndpoint` 不受影响。

---

## [未发布]

### 新增

**可注入的 io 运行时 `Runtime`（多套线程池 / 多 .so 部署）**

- `include/netmech/runtime.hpp`：`Runtime::create(Options{io_threads})` 由**调用方**
  决定线程数；`shutdown()` 幂等（停线程 + join，io_context 与进程同寿，语义与
  默认运行时一致）。
- `Mechanism::create(cfg, std::shared_ptr<Runtime>)`：注入入口。单 exe / 单 .so
  仍用 `Mechanism::create(cfg)`（默认运行时），旧代码零改动。
- `netmech::initialize()/shutdown()` 管理的是进程级默认运行时（`default_runtime()`），
  行为不变。
- `test/test_runtime.cpp`：5 个用例（线程数由创建者决定、关闭语义、注入后跨池
  echo、两个 Runtime 互不影响、Mechanism 持有 Runtime 强引用）。

**构建：`NETMECH_LIBRARY_TYPE=STATIC|SHARED`（默认 STATIC）**

- 多 .so 进程必须用 SHARED：**实测结论**——各 .so 各自静态链接 netmech 时，
  代码与静态数据（io 池、asio 的线程局部状态、注册表）都是每模块一份，
  把一个模块的 Runtime 注入到另一个静态链接的模块后，混用同一个 io_context
  会崩（SIGSEGV in `asio::detail::scheduler::compensating_work_started`）。
  SHARED 构建全进程一份代码，`default_runtime()` 天然只有一份。
- `test/multi_so/`：只在 SHARED 构建下注册的回归用例 —— 宿主建一个 `Runtime(4)`，
  两个静态…（共享）链接 netmech 的 .so 分别建 Mechanism，断言进程只多 4 条
  io 线程、两个 .so 看到同一个 Runtime。CI 新增 `Linux / gcc / Shared` job。


**CMake 安装与导出（find_package）**

- `cmake --install` 现在装出完整的一套：头文件、静态库、
  `lib/cmake/netmech/` 下的 `Config` / `ConfigVersion` / `Targets`，
  以及 `share/doc/netmech` 下的 LICENSE / AUTHORS
- 使用者只需要两行：
  ```cmake
  find_package(netmech REQUIRED)
  target_link_libraries(app PRIVATE netmech::netmech)
  ```
- **自带的 Asio 随包安装**（装到 `include/netmech/thirdparty/asio/include`）：
  公共头 `conn.hpp` / `mechanism.hpp` / `linkable.hpp` / `common.hpp` 直接
  `#include <asio/...>`，不带上这份头，装了 netmech 也编不过。放在
  `include/netmech/` 下面而不是 `include/` 根下，是为了不和系统级 Asio 撞车。
- 默认安装前缀：Linux `/usr/local`，Windows `C:/Program Files/netmech`

### 变更（破坏性）

**CMake 目标与工程名统一为 `netmech`**

此前 `project()` 与库目标都叫 `NetMechanism`，而目录、头文件命名空间
（`netmech/`、`netmech::`）、可执行文件（`netmech_ut` / `netmech_echo` /
`netmech_udp` / `serial_check`）全都是小写 `netmech`，两套写法并存。

现在统一：

| 项 | 旧 | 新 |
|---|---|---|
| project() | `NetMechanism` | `netmech` |
| 库目标 | `NetMechanism` | `netmech` |
| 静态库产物 | `libNetMechanism.a` / `NetMechanism.lib` | `libnetmech.a` / `netmech.lib` |
| VS 工程 | `NetMechanism.vcxproj` / `NetMechanismTest.vcxproj` | `netmech.vcxproj` / `netmech_ut.vcxproj` |
| 解决方案 | `NetMechanism.sln` | `netmech.sln` |

`include <netmech/...>`、`namespace netmech`、各可执行文件名**均未改变**。

**对使用方的影响**：`add_subdirectory(netmech)` 后需把
`target_link_libraries(... NetMechanism)` 改成 `netmech`。

### 修复

- `NetmechSerial.vcxproj` 是从 udp 工程复制来的，`TargetName` / `RootNamespace`
  仍是 `netmech_udp`，编译会覆盖 `netmech_udp.exe`。已改名 `serial_check.vcxproj`
  并把产物修正为 `serial_check.exe`。
- 新增 `tools/check_vs_projects.py`：校验 sln/vcxproj 的工程引用、源文件路径、
  GUID、链接库名是否互相自洽（改名后没有 MSBuild 环境时的兜底检查）。
- 串口写的**发起动作**改投递到 io 线程（`src/serial.cpp`）：此前在调用线程直调
  `asio::async_write`，会与 io 线程上在途 `async_read_some` 的完成处理并发访问同一个
  `serial_port` 对象 —— Asio 明令 "shared objects: unsafe"。历史上试过 post 会弄坏
  modbusmech 的 RTU 链路，那是 `serial.cpp` 重写**之前**的结论；本轮重测 netmech 全量
  （845 断言）与 modbusmech 全量（380 断言，含全部 RTU pty 用例）×3 轮、TSan 0 报告
  后正式改掉。
- 串口读写新增"运行时已停"检查：`shutdown()` 之后 `read()` / `write()` 直接返回 false
  （以前会把任务投进已停的队列，调用方永远等不到回调）。
- 文档：`iohandler.hpp` 生命周期段写明"连接关闭后库仍保留 handler 强引用（retired 快照，
  用于兜底派发已受理未执行的通知）"，并给出规则：handler 不要持有 `Conn` 强引用；
  若必须持有，请在 `on_close()` 里释放 —— 否则 `Conn::Impl` 与 handler 互持成环泄漏，
  且 `Mechanism::endpoint_count()` 会正常归零、从计数上看不出来。
- 文档：`serial.hpp` 补齐写缓冲契约（内存必须有效到 `on_write` 返回）与 `close()` 的真实
  语义（在途读会先以 `on_read(ec)` 兑现；底层句柄在 io 线程上释放）。
- 新增回归测试 `serial_cross_thread_writes_with_pending_read`：调用线程持续写 +
  io 线程不停跑读完成，断言每笔受理恰好一次 `on_write`、数据完整到达对端。

---

## [0.9.0] — 首个版本

功能已完整，Windows 与 Linux 双平台验证过，但尚未在真实项目中长期运行。
API 在 1.0.0 之前仍可能微调。

### 新增

**TCP**

- `Conn`：单条连接。`read(Buf, ReadMode)` / `write(ConstBuf)` 不再接收每笔操作的 lambda，
  完成通知回调到 `ConnHandler` 的虚函数。
- `ConnHandler`：`on_open` / `on_read` / `on_write` / `on_close` / `on_idle`。
- `Linkable`：`on_accept` / `on_linkup` 以**返回值**交出 handler（返回空 = 拒绝该连接）。
- `Mechanism`：`create` / `listen` / `connect` / `close_all`，以及按 id 操作
  `write(id, buf)` / `close(id)`。
- 保活**分级预警**：接收与发送两侧对称，每次检查未通过都回调 `on_idle`，
  第 `max_miss` 次之后才断链。
- **批量发送**：队首最多 16 笔 / 64 KB 合成一次系统调用（scatter-gather），
  回调仍按笔逐个归还。小包高频场景实测 7 倍吞吐提升。

**UDP**

- `UdpEndpoint`：一个对象 = 一个绑定端口。推送式接收，不需要调用方 `read()`。
- `UdpHandler`：`on_read` / `on_peer_changed` / `on_close`。
- `last_peer()` / `on_peer_changed`：观测真实来源地址，回包用它最可靠。
- 截断检测：报文超过 `recv_capacity` 时以 `NetErrc::datagram_truncated` 交付。
- 组播：入组、跳数、环回开关。

**串口**

- `SerialPort`：打开串口异步读写，接口与 `Conn` 同构（拉模式读 + 队列写）。
  基于 `asio::serial_port`，与 socket 共用同一个 `io_context`、同一套线程模型。
- `drain()`：RS-485 方向控制用。Asio 没暴露 `tcdrain`，按波特率估算等待时间。
- `serial_check` 示例：双向收发 + 256 字节对拷 + drain 自检。
  Windows 用 com0com、Linux 用 socat/pty 都能跑。

**端点定时**

- `UdpEndpoint::set_timer` / `clear_timer`：每个端点一个定时回调，在**该端点的
  io 线程**上执行（与收包回调同线程，共享状态无需加锁）。用弱引用表达"回调
  依赖谁"，依赖对象销毁或端点关闭时自动停表。
- `find_free_port_run`：在区间内探一段连续可用端口（可要求偶数起点），
  供 RTP 这类"协商前要先定端口"的协议使用。
- `send_to` 改为 `dispatch` 到端点自己的 io 线程，**去掉发送锁**；
  真实失败计入 `datagrams_send_failed()`。

**通用**

- `Address`：不依赖 asio 的地址值类型，公共头不传递网络库细节。
- `Buf` / `ConstBuf`：带可选 `owner` 的缓冲视图，库不拷贝数据。
- `initialize()` / `shutdown()`：进程级 io 运行时入口。
- 错误码统一到 `net_category()`，`ec == NetErrc::xxx` 可直接比较。

### 关键约束（使用前必读）

- **handler 必须保持一笔读在途**，否则感知不到对端关闭（TCP 的 FIN 只在 read 时被看见）。
- **`write()` 返回 `false` 不代表出错**：反压时也返回 false，连接**不断**，
  应等 `on_write` 之后重试。
- **`linger_zero` 默认 `false`**（优雅关闭）。改成 `true` 会发 RST 并丢弃未发数据。

### 已知限制

- Linux aarch64（目标部署平台）尚未验证。
- 只支持 IP 字面量，不做域名解析。
- `ConnectFuture` 这类同步等待接口**不提供**——handler 必须由 `Linkable` 创建，
  再加一层等待句柄没有本质收益（见 `docs/design/overview-design.md` §8.2）。

### 验证情况

| 平台 | 编译器 | 结果 |
|---|---|---|
| Windows 10/11 x64 | MSVC 14.44 | 46 用例 / 604 断言全绿，零警告 |
| Linux x86_64 (WSL2 Ubuntu) | GCC 15.2 | 46 用例 / 604 断言全绿，零警告 |

压力测试（`bench/stress`）：2000 并发连接、200 万条报文、连接风暴（连上即断）
全部通过且连接表收敛。
长跑（`bench/soak`）：Windows 322 轮 / Linux 371 轮，0 失败，内存无持续增长。
