# Changelog

本文件记录所有值得注意的变更。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

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
