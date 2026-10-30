# NetMechanism API 参考

> 目标读者：使用本库的开发者。按头文件分节，每节列接口签名、返回值、失败原因与约束。

---

## 0. 通用约定

### 命名空间与头文件

所有接口都在 `namespace netmech`。业务代码只需：

```cpp
#include <netmech/net.hpp>
```

### 错误处理

| 位置 | 形式 |
|---|---|
| 可能失败的操作（`listen` / `connect` / `read` / `write`） | 返回 `bool` 或 `std::error_code` |
| 异步完成回调 | 参数 `std::error_code ec`，**空表示成功** |
| 纯查询 / 关闭 | `noexcept`，无返回值 |

错误码用 `NetErrc`（`std::error_code` 的枚举桥接），可以直接比较：

```cpp
if (ec == NetErrc::link_closed) { ... }      // 可用
if (ec == asio::error::eof)    { ... }       // 底层错误同样可比
```

### 线程模型

- **1 个 `io_context` = 1 条 io 线程**。库不使用 strand。
- 同一条连接的全部回调（`on_open` / `on_read` / `on_write` / `on_close` / `on_idle`）**串行执行**，不需要自己加锁。
- 不同连接可能在不同 io 线程上并发，**跨连接共享的状态需要自己加锁**。
- `initialize()` / `shutdown()` / `close()` / `write()` 可从任意线程调用。

### 三条贯穿全库的契约

1. **回调串行** —— 一条连接的回调不会并发。
2. **buffer 归调用方** —— `read`/`write` 返回 `true` 后，缓冲必须活到完成回调；
   用 `Buf::owner` 让库替你持有（异步场景推荐）。
3. **`false` = 没受理** —— 不回调，缓冲立刻归你。反压也走这条，**不是错误、不断链**。

---

## 1. `netmech/common.hpp`

进程级初始化、地址、缓冲区视图、错误码。

### 1.1 `initialize` / `shutdown`

```cpp
[[nodiscard]] bool initialize(std::size_t io_threads) noexcept;
void shutdown() noexcept;
```

**`initialize(io_threads)`** —— 初始化进程级 io 运行时。**必须在任何网络对象之前调用**，
一个进程只调用一次。

| 参数 | 说明 |
|---|---|
| `io_threads` | 期望的 io 线程数。`0` 表示 `max(1, hardware_concurrency())` |

| 返回 | 含义 |
|---|---|
| `true` | 本次调用决定了线程数（首次） |
| `false` | 已初始化过且请求值不同（**同值视为幂等成功**） |

> 线程数在运行时**不可更改**。这是"1 context = 1 线程、回调天然串行"的地基。

**`shutdown()`** —— 停止 io 线程并 join。幂等。调用后所有连接关闭、在途操作以取消类错误归还。
**不支持重新初始化**——避免"线程数改了但旧 context 还在跑"的半死状态。

> ⚠️ `shutdown()` 会阻塞到所有 io 线程退出，**不要在 io 回调里调用**。

### 1.2 `Address`

不依赖 asio 的 IP 端点值类型。

```cpp
class Address {
public:
    Address() noexcept;                                    // 0.0.0.0:0
    Address(std::string_view ip, std::uint16_t port) noexcept;

    static std::optional<Address> parse(std::string_view text) noexcept;
    static Address any(std::uint16_t port) noexcept;

    std::uint16_t port() const noexcept;
    bool          is_v4() const noexcept;
    bool          is_unspecified() const noexcept;
    std::string   to_string() const;                       // "1.2.3.4:502"
};
```

| 用法 | 结果 |
|---|---|
| `Address()` | `0.0.0.0:0` |
| `Address("1.2.3.4", 502)` | 正常；**ip 非法时退化为 `0.0.0.0:0`，不抛异常** |
| `Address::parse("1.2.3.4:502")` | `optional`，解析失败返回 `nullopt` |
| `Address::parse("[::1]:9000")` | IPv6 **必须带方括号**（否则无法与端口区分） |
| `Address::parse(":9000")` | 等价于 `0.0.0.0:9000` |
| `Address::any(9000)` | `0.0.0.0:9000` |

**不做域名解析**——地址来自配置，基本都是 IP 字面量。

### 1.3 `Buf` / `ConstBuf`

库**不拷贝**数据，只记录指针 + 长度 + 可选 owner。

```cpp
struct Buf {
    std::shared_ptr<void> owner;   // 可空
    void*                 data;
    std::size_t           size;
};

struct ConstBuf {                  // 写操作用
    std::shared_ptr<void> owner;   // 可空
    const void*           data;
    std::size_t           size;
};
```

| `owner` | 语义 |
|---|---|
| 非空 | 库持有它直到完成回调返回，你提前析构也安全 |
| 空 | **你必须保证 `data` 在完成回调返回前有效**（成员数组或栈上缓冲都行，看调用方式） |

推荐写法（异步、跨作用域）：

```cpp
auto block = std::make_shared<std::array<std::uint8_t, 4096>>();
ep->write({block, block->data(), block->size()});     // owner 保证活到 on_write
```

> ⚠️ `owner` 不要传长期存活的对象（如 `shared_from_this()`），会把整个对象钉在一条在途操作上。

### 1.4 `NetErrc`

```cpp
enum class NetErrc {
    ok = 0,
    not_initialized,       // 未 initialize
    already_initialized,   // 重复 initialize 且参数不同
    invalid_argument,      // 参数非法（空 buffer、端口 0 等）
    duplicate_endpoint,    // listen 的地址已被占用
    buffer_full,           // 发送队列满（反压）—— 不断链
    not_connected,         // 连接尚未建立
    link_closed,           // 连接已关闭
    accept_failed,         // accept 系统调用失败
    keepalive_timeout,     // 保活超时
    handler_rejected,      // handler 工厂返回空 / on_open 返回 false
    cancelled,             // 被显式取消
    datagram_truncated,    // UDP 报文超过接收缓冲，已截断（仅该报文）
};
```

---

## 2. `netmech/conn.hpp`

TCP 连接。

### 2.1 `ReadMode`

```cpp
enum class ReadMode {
    some,    // 收到 ≥1 字节就完成：on_read 成功时 n <= 请求长度
    exact,   // 收满请求长度才完成：on_read 成功时 n == 请求长度（定长协议用）
};
```

### 2.2 `IdleInfo`

保活检查未通过时的信息。

```cpp
enum class IdleKind { recv, send };

struct IdleInfo {
    IdleKind      kind;
    std::uint32_t missed;   // 连续第几次未通过，从 1 开始
    std::uint32_t limit;    // 配置的 max_miss

    bool has_grace() const noexcept { return missed < limit; }
};
```

`has_grace() == true` 表示**还有挽回余地**（库不会因此关闭连接）。

### 2.3 `Conn`

一条已建立的 TCP 连接。由库创建并持有，通过 `ConnHandler::on_open` 或 `Linkable` 交出。

#### 标识与地址

```cpp
std::uint64_t id() const noexcept;                    // 进程内唯一，不复用
Address local_address() const;                        // 关闭后返回缓存值
Address remote_address() const;
std::shared_ptr<void> user_context() const noexcept;  // listen/connect 时传入的
```

#### 读

```cpp
bool read(Buf buf, ReadMode mode = ReadMode::some);
```

| 返回 | 含义 |
|---|---|
| `true` | 已受理，库将**恰好调用一次** `on_read` |
| `false` | **未受理**，不回调，`buf` 立刻归你 |

`false` 的三种原因：

1. 连接已关闭；
2. `buf.data == nullptr` 或 `buf.size == 0`；
3. **已有一笔读在途**（同一连接同一时刻只允许一笔）。

> **只允许一笔读在途**：多笔在途时完成回调无法确定对应哪一笔请求，
> 一旦错配就会把 A 的数据当作 B 的帧头解析且无法恢复。一对一让这个歧义消失。
>
> ⚠️ **必须保持一笔读在途**：TCP 的 FIN 只在下一次 `read` 时被感知。
> 不读就永远不知道对端关了。

#### 写

```cpp
bool write(ConstBuf buf);
```

| 返回 | 含义 |
|---|---|
| `true` | 已受理，库将**恰好调用一次** `on_write` |
| `false` | **未受理**，不回调，缓冲立刻归你 |

`false` 的原因：连接已关闭 / 参数非法（空 buffer）/ **发送队列满（反压）**。

> **反压不是错误**：队列满时返回 `false`，连接**不断**。正确反应是等 `on_write`
> 之后再试，而不是放弃或主动断链。
>
> **顺序保证**：所有被受理的 write，数据在 TCP 流上的顺序、以及 `on_write` 的完成顺序，
> 与**受理顺序**严格一致。多线程并发调用也确定（受理顺序 = 队列锁获取顺序）。

#### 发送队列上限（反压阈值）

```cpp
void set_send_queue_limit(std::size_t bytes, std::size_t count);
```

- 默认 2 MiB / 256 笔（由 `Mechanism::Config` 决定）。
- 两个是**与**关系：任一超限即拒。
- `0` 表示该维度不限。

#### 保活

```cpp
void enable_recv_keepalive(std::chrono::milliseconds delay,
                           std::chrono::milliseconds interval,
                           std::uint32_t max_miss);
void disable_recv_keepalive();

void enable_send_keepalive(std::chrono::milliseconds delay,
                           std::chrono::milliseconds interval,
                           std::uint32_t max_miss = 3);
void disable_send_keepalive();
```

两侧行为对称：

- 每隔 `interval` 检查"自上次检查以来收/发方向有无进展"；
- **每次未通过都回调 `on_idle()`**，`missed` 从 1 递增；
- **只在第 `max_miss` 次回调返回之后**才以 `keepalive_timeout` 断链；
- 前面几次是处置窗口（发心跳、记日志、主动 close）。

参数非法（`interval <= 0` / `max_miss == 0` / `delay < 0`）时**静默忽略**，保持原状。

#### 关闭

```cpp
void close() noexcept;
bool is_open() const noexcept;
```

`close()` 幂等、`noexcept`、可从任意线程调用（包括回调内部）。行为：

1. 立刻让 `is_open()` 返回 `false`；
2. 取消挂起的读/写/保活定时器；
3. 每笔已受理未完成的读写以错误码归还**恰好一次**；
4. 在 io 线程上投递 `on_close()` 与 `on_linkdown()`（若连接曾被接管）；
5. 关闭 socket——是否走 RST 由 `Config::linger_zero` 决定。

> `is_open()` 是**瞬间快照**：返回 true 后对端可能立刻关闭。
> 不要用它替代对 `read`/`write` 返回值的检查。

---

## 3. `netmech/iohandler.hpp`

单连接的协议处理器。

```cpp
class ConnHandler {
public:
    virtual ~ConnHandler() = default;

    virtual bool on_open(std::shared_ptr<Conn> ep) = 0;
    virtual void on_read (std::error_code ec, Buf buf,      std::size_t n) = 0;
    virtual void on_write(std::error_code ec, ConstBuf buf, std::size_t n) = 0;
    virtual void on_close() = 0;
    virtual void on_idle(const IdleInfo& info) {}
};
```

### 3.1 `on_open`

```cpp
virtual bool on_open(std::shared_ptr<Conn> ep) = 0;
```

连接就绪，在库接管后、投递任何读写事件**之前**调用一次。

| 返回 | 后果 |
|---|---|
| `true` | 库继续驱动该连接 |
| `false` | 库立即关闭连接，**此后不会有 `on_close`** |

典型实现：保存 `ep`，然后发起第一笔读。

### 3.2 `on_read`

```cpp
virtual void on_read(std::error_code ec, Buf buf, std::size_t n) = 0;
```

| 参数 | 说明 |
|---|---|
| `ec` | 空 = 收到数据；非空 = 该缓冲归还（`link_closed` / `keepalive_timeout` 等），**连接随即关闭** |
| `buf` | 原样传回你提交的那份 `Buf`（owner / data / size 均不变） |
| `n` | 已收到字节数。成功时 > 0；失败时通常 0，也可能是部分数据 |

**保证**：

- 每笔被受理的 `read()` **恰好**触发一次本回调；
- 返回 `false` 的 `read()` **不会**触发；
- 本回调返回后，**必须显式再次调用 `read()`** —— 库不会自动续读。

### 3.3 `on_write`

```cpp
virtual void on_write(std::error_code ec, ConstBuf buf, std::size_t n) = 0;
```

`ec` 空表示全部写出（`n == buf.size`）；非空表示归还，`n` 为已写出字节数。

**顺序保证**：多笔 write 的回调顺序与受理顺序严格一致（FIFO）。出错时剩余请求
也会按序各回调一次（`ec` 非空），不会静默丢弃。

### 3.4 `on_close`

```cpp
virtual void on_close() = 0;
```

本连接的最后一次回调。

**保证**：

- 在所有在途读写的完成回调（`ec` 非空）**之后**调用；
- 每个被接管的连接**恰好**一次；`on_open` 返回 `false` 的连接不调用；
- 本回调返回后库释放 handler 的强引用，对象可能就此销毁。

### 3.5 `on_idle`

```cpp
virtual void on_idle(const IdleInfo& info) {}
```

保活检查未通过。可选实现，默认空。

**每拍都调**，不是只在超时时调一次。`info.missed` 从 1 递增到 `limit`，
第 `limit` 次返回之后才断链。

---

---

## 4. `netmech/linkable.hpp`

连接事件的订阅者：决定"新建立的连接交给谁"。

```cpp
class Linkable {
public:
    virtual ~Linkable() = default;

    virtual std::shared_ptr<ConnHandler> on_accept(Conn& c)      { return nullptr; }
    virtual std::shared_ptr<ConnHandler> on_linkup(Conn& c)      { return nullptr; }
    virtual void on_linkdown(Conn& c)                            {}
    virtual void on_linkfail(const Address& remote, std::error_code ec) {}
};
```

| 回调 | 触发时机 |
|---|---|
| `on_accept` | **服务端**：accept 到一条新连接 |
| `on_linkup` | **客户端**：connect 成功 |
| `on_linkdown` | 曾经接管的连接关闭（此前必然 `on_accept`/`on_linkup` 过） |
| `on_linkfail` | 客户端 connect 失败（超时 / 拒绝 / 不可达） |

### 返回值语义（`on_accept` / `on_linkup`）

- **返回非空 handler** → 库接管该连接，随后调用 `handler->on_open(c)`；
- **返回空** → 视为**拒绝**，库立刻关闭该连接，且**不会**再给 `on_linkdown`
  （因为没有"连接成功"过）。

> ⚠️ 不要返回已被其它连接占用的 handler 实例——一个 handler 只服务一条连接。

### 生命周期

`Linkable` 以 `std::weak_ptr` 形式交给库（`listen` / `connect` 的参数）。
**你必须自己持有它**，否则它析构后库就找不到订阅者（连接会被拒绝或通知丢失）。

```cpp
auto linkable = std::make_shared<MyLinkable>();     // 你自己持有
mech->listen(addr, std::weak_ptr<Linkable>(linkable));
```

---

## 5. `netmech/mechanism.hpp`

运行时：创建、监听、连接、按 id 操作。

### 5.1 `Mechanism::Config`

```cpp
struct Config {
    std::size_t send_queue_bytes = 2 * 1024 * 1024;   // 单连接发送队列字节上限
    std::size_t send_queue_count = 256;               // 单连接发送队列笔数上限
    bool        tcp_nodelay      = true;              // 关闭 Nagle
    bool        linger_zero      = false;             // true = close 时发 RST
    std::size_t socket_buf_size  = 256 * 1024;        // SO_SNDBUF / SO_RCVBUF，0=不设
};
```

**`linger_zero` 的取舍**：

| 取值 | 关闭方式 | 适用 |
|---|---|---|
| `false`（默认） | 正常四次挥手，**已入队的数据保证送达** | 请求-应答协议（Modbus 等） |
| `true` | `SO_LINGER{1,0}` 立即 RST，**丢弃未发数据** | 高频短连接，避免 TIME_WAIT 堆积 |

> ⚠️ `false` 时主动关闭方进入 TIME_WAIT（约 2 分钟）。连接建立速率超过
> **约 136 次/秒**时，Windows 的 16384 个动态端口会被耗尽。

### 5.2 创建

```cpp
static std::shared_ptr<Mechanism> create();          // 默认配置
static std::shared_ptr<Mechanism> create(Config cfg);
```

`create` 之前必须已调用 `initialize()`。返回 `nullptr` 表示运行时创建失败。

### 5.3 监听

```cpp
std::error_code listen(const Address& local,
                       std::weak_ptr<Linkable> linkable,
                       std::uint32_t backlog = 128,
                       std::shared_ptr<void> user_ctx = nullptr);
```

| 参数 | 说明 |
|---|---|
| `local` | 绑定地址。端口 `0` 表示由系统分配（每次调用得到**新**端口） |
| `linkable` | 决定新连接交给谁（弱引用，**你必须持有**） |
| `backlog` | listen 队列长度。**几百个连接同时到达时默认值不够**，会有一部分在握手阶段被拒 |
| `user_ctx` | 任意上下文，会原样挂到每条 accept 出来的 `Conn` 上 |

失败原因：`duplicate_endpoint`（同地址已监听）、`invalid_argument`、系统错误。

绑定成功后可用 `local_address_of(local)` 拿到**实际**地址（端口 0 时必需）。

### 5.4 连接

```cpp
std::error_code connect(const Address& remote,
                        std::weak_ptr<Linkable> linkable,
                        std::uint32_t timeout_ms = 3000,
                        const Address& local = Address{},
                        std::shared_ptr<void> user_ctx = nullptr);
```

**异步**：返回 `true` 语义的"已受理"后，结果通过 `on_linkup` / `on_linkfail` 通知。

| 参数 | 说明 |
|---|---|
| `remote` | 目标地址，**不能是通配地址或端口 0** |
| `timeout_ms` | 握手超时 |
| `local` | 本地绑定地址（默认由系统选源端口） |
| `user_ctx` | 原样挂到新连接上 |

> 同一个目标地址**可以并发发起多个 connect**（连接池、并发探测）。

### 5.5 运行时信息

```cpp
[[nodiscard]] std::size_t  endpoint_count() const noexcept;   // 当前活跃连接数
[[nodiscard]] std::size_t  io_threads()     const noexcept;   // 进程级 io 线程数
[[nodiscard]] bool         running()        const noexcept;   // 运行时是否可用
```

### 5.6 本地地址查询

```cpp
[[nodiscard]] std::optional<Address>
local_address_of(const Address& local) const;
```

查询某个监听地址**实际绑定**的地址。

- 端口 `0` 时返回**最近一次** listen 分配到的端口；
- 未监听返回 `nullopt`。

### 5.7 按连接 id 操作

```cpp
bool write(std::uint64_t conn_id, ConstBuf buf);
bool close(std::uint64_t conn_id) noexcept;
```

业务侧常常只有"设备表号"，没有 `Conn` 对象——用这两个接口可以避免自己维护
`id → Conn*` 表，以及摘除时的竞态窗口。

| 接口 | 返回 false 的含义 |
|---|---|
| `write` | 连接不存在 / 已关闭 / 队列满（与 `Conn::write` 语义相同） |
| `close` | 该 id 不存在（已关闭或从未存在） |

> 与 `Conn::write` 共用同一条发送队列和同一把锁，**受理顺序对所有线程确定**，
> 两条入口可以混用。

### 5.8 关闭

```cpp
void close_all() noexcept;
```

关闭所有监听器与连接。幂等。之后仍可重新 `listen`（不进入"终态"）。

---

## 6. `netmech/udp.hpp`

### 6.1 概念

- 一个 `UdpEndpoint` = **一个绑定的本地端口**。
- **没有连接、没有会话**：源地址只是观测值，不用来识别身份。
- **推送式**：库自己持续收包，你只实现 `on_read`，不需要调 `read()`。

> 不要按源地址维护会话表——NAT 重绑定、端口协商不一致都会让"按 peer 分会话"
> 变成不可靠的假设。需要业务多路复用时，在**应用层**加标识。

### 6.2 `UdpConfig`

```cpp
struct UdpConfig {
    Address                bind;                     // 绑定地址（端口 0 = 系统分配）
    std::size_t            recv_capacity = 2048;     // 单报文接收缓冲上限
    std::size_t            socket_buf_size = 256 * 1024;
    bool                   reuse_address = false;
    bool                   allow_broadcast = false;
    DeliverMode            deliver = DeliverMode::borrow;
    std::optional<Address> multicast_group;          // 非空则加入该组播组
    std::optional<Address> multicast_interface;
    std::uint8_t           multicast_hops = 1;
    bool                   multicast_loopback = true;
};
```

`DeliverMode`：

| 取值 | 含义 |
|---|---|
| `borrow`（默认） | `on_read` 的 `Buf` 指向库的复用缓冲，**回调返回即失效** |
| `own` | 每包独立分配，`Buf::owner` 非空，可跨线程/延后处理 |

### 6.3 `UdpHandler`

```cpp
class UdpHandler {
public:
    virtual ~UdpHandler() = default;

    virtual void on_read(UdpEndpoint& ep, std::error_code ec, Buf buf,
                         std::size_t n, const Address& from) = 0;

    virtual void on_peer_changed(UdpEndpoint& ep, const Address& from) {}
    virtual void on_close(UdpEndpoint& ep, std::error_code ec) {}
};
```

| 回调 | 说明 |
|---|---|
| `on_read` | 每个报文一次。`ec == datagram_truncated` 表示该报文超过 `recv_capacity`，已被截断（仅此条受影响） |
| `on_peer_changed` | **来源地址变化时**调用（含首次）。空实现即可 |
| `on_close` | 端点关闭，恰好一次 |

### 6.4 `UdpEndpoint`

```cpp
bool send_to(const Address& dst, ConstBuf buf);
Address local_address() const noexcept;
bool    is_open() const noexcept;
std::uint32_t id() const noexcept;
std::shared_ptr<void> user_context() const noexcept;
void    close() noexcept;

std::uint64_t datagrams_received()  const noexcept;
std::uint64_t datagrams_sent()      const noexcept;
std::uint64_t datagrams_truncated() const noexcept;

std::optional<Address> last_peer() const noexcept;
std::chrono::steady_clock::time_point last_recv_time() const noexcept;
```

**`send_to`**：同步非阻塞。返回 `true` 只表示**已交给内核**，不代表对方收到。

返回 `false`：端点已关闭 / 参数非法（空 buffer）/ 报文超过 `kMaxDatagramSize` /
发送缓冲满（UDP 下这是正常的瞬时状态，稍后重试即可）。

**`last_peer()`**：最近一次收到报文的**真实来源地址**。回包时用它最稳——
比配置里的目标地址可靠（NAT 之后可能不一致）。

### 6.5 创建

```cpp
std::shared_ptr<UdpEndpoint>
create_udp_endpoint(const UdpConfig&            cfg,
                    std::shared_ptr<UdpHandler> handler,
                    std::shared_ptr<void>       user_ctx,
                    std::error_code&            ec);

// 跟随另一个端点的 io 线程
std::shared_ptr<UdpEndpoint>
create_udp_endpoint_on(const UdpEndpoint&          anchor,
                       const UdpConfig&            cfg,
                       std::shared_ptr<UdpHandler> handler,
                       std::shared_ptr<void>       user_ctx,
                       std::error_code&            ec);
```

失败原因：`handler` 为空、`recv_capacity` 非法（0 或 > `kMaxDatagramSize`）、
bind 失败、组播加入失败。

`kMaxDatagramSize = 65507`（UDP 载荷上限）。

### 6.6 为什么需要 `create_udp_endpoint_on`

`create_udp_endpoint` 从 io 池**轮转**取 context，所以连续创建的两个端点会落在
**不同线程**上。这对独立业务无所谓，但对"一个会话两个端口"就是问题：

```cpp
// RTP / RTCP 要共享 tracker、scheduler 等状态
auto rtp  = create_udp_endpoint(rtp_cfg, ...);            // 线程 A
auto rtcp = create_udp_endpoint(rtcp_cfg, ...);           // 线程 B ← 状态被并发访问
```

用 `_on` 让第二个跟随第一个：

```cpp
auto rtp  = create_udp_endpoint(rtp_cfg, ...);                    // 线程 A
auto rtcp = create_udp_endpoint_on(*rtp, rtcp_cfg, ...);          // 也是线程 A
rtcp->set_timer(5s, owner, [..]{ /* 也在线程 A */ });
```

三者完全串行，共享状态**不需要加锁**。

> 为什么是"跟随端点"而不是"指定线程编号"：库不对外暴露 `io_context` 和线程编号。
> 跟随端点只表达"我要和它同一个线程"这个意图，内部线程模型怎么实现留在库里。

---

## 7. 端点定时与端口探测

### 7.1 为什么定时能力挂在 `UdpEndpoint` 上

`io_context` 在库内部，应用层拿不到。要定时就只能自己起线程——那样回调不在 io 线程上，
与收包回调共享的状态全得加锁。

所以定时能力直接做在 `UdpEndpoint` 上，**不单独暴露一个 Timer 类**：

- 回调天然在**该端点的 io 线程**上执行，与 `on_read` 同线程，共享状态无需加锁；
- 注册/取消也投递到该线程执行，因此可以从任意线程安全调用；
- 每个端点最多一个定时回调，重复设置是**替换**不是叠加。

### 7.2 接口

```cpp
class UdpEndpoint {
    void set_timer(std::chrono::milliseconds interval,
                   std::weak_ptr<void>     owner,
                   std::function<void()>   cb);
    void clear_timer();
};
```

| 参数 | 说明 |
|---|---|
| `interval` | 间隔。**`<= 0` 视为取消** |
| `owner` | 回调的**依赖对象**（弱引用），见下 |
| `cb` | 回调，在端点自己的 io 线程上执行 |

### 7.3 `owner` 的语义

| 情况 | 行为 |
|---|---|
| `owner` 有效 | 正常回调，**回调期间临时强持有**，`cb` 里可以安全使用它 |
| `owner` 已失效 | 定时器**自动停止**，不再回调（不报错、不通知） |
| `owner` 传空 | 库不检查，你必须自己保证 `cb` 内不触碰已销毁对象 |

库能保证端点活着，**保证不了你注册的回调里捕获的东西还活着**——把依赖关系显式写出来，
库替你在依赖消失时收尾。

### 7.4 其他行为

- **替换语义**：重复 `set_timer` 会取消旧的、起新的，不会叠加。
- **漂移不累积**：按"上次计划时间 + interval"排下一拍。
- **端点关闭时自动停表**，不需要先 `clear_timer`。
- 可从任意线程调用；从网络回调里调用时 `dispatch` 同步执行，零开销。

### 7.5 `find_free_port_run` —— 端口探测

RTP 这类协议需要在**协商之前**把端口定下来（SIP/SDP 里要写），所以需要先探一对可用的：

```cpp
// 在 [5000, 6000] 里找一对连续可用端口，起点必须偶数
auto base = netmech::find_free_port_run(5000, 6000, /*count=*/2, /*alignment=*/2);
if (!base) { /* 范围内没有 */ }

// 把 *base 和 *base + 1 写进 SDP

// 协商完成后按它们绑定；这一步失败就直接失败，不重试
cfg.bind = Address(ip, *base);
auto ep = create_udp_endpoint(cfg, handler, nullptr, ec);
```

| 参数 | 说明 |
|---|---|
| `lo` / `hi` | 搜索区间（含两端） |
| `count` | 需要连续几个端口（RTP 用 2） |
| `alignment` | 起点对齐：`1` 任意，`2` 偶数（RFC 3550 建议 RTP 用偶数） |

> ⚠️ 这是**探测**不是预留。返回之后到真正 `bind` 之间端口可能被抢（TOCTOU）。
> 此时 `create_udp_endpoint` 直接失败——这是预期行为，不要重试：协商结果已定，
> 重试也改变不了对端的计算。

### 7.6 发送语义的变化

配合定时能力，`send_to` 也改成了无锁实现：

```cpp
ep->set_timer(5s, owner, [ep = ep.get()] {
    ep->send_to(peer, {nullptr, pkt.data(), pkt.size()});   // 同一线程，零开销
});
```

| 返回 | 含义 |
|---|---|
| `true` | **已受理**（可能同步发出，也可能排队到 io 线程） |
| `false` | 参数非法 / 端点已关闭 |

真实发送失败（发送缓冲满）计入 `stats().datagrams_send_failed()`。
对实时流这是合理取舍——宁可丢包降码率，也不阻塞调用线程。
