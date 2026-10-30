# NetMechanism 概要设计（v1.0）

对应对外契约：`docs/design/proposed/netmech/{common,tcp,udp}.hpp`（v1.0 定稿）。
本文只讲**内部怎么实现**。契约是条件，实现层任何调整都不得破坏头文件里的承诺。

约束：C++17、standalone asio（仓库自带 `thirdparty/asio`）、MSVC + Linux、不引入新依赖。

---

## 1. 模块划分

```
include/netmech/                 ← 契约层：只有声明，不含 asio
  common.hpp                     initialize/shutdown, Address, Buf, ConstBuf, NetErrc
  tcp.hpp                        Mechanism, Conn, ConnHandler, Linkable, ConnectFuture
  udp.hpp                        UdpEndpoint, UdpHandler, UdpConfig
  netmech.hpp                    汇总头

src/                             ← 实现层：只有这里能 include asio
  runtime.cpp                    initialize / shutdown（IoPool 单例的对外入口）
  io_pool.hpp / io_pool.cpp      N × io_context + 1 线程/个 + work_guard
  address.cpp                    Address ⇄ asio 地址（唯一做转换的地方）
  error.cpp                      NetErrc ⇄ std::error_code + 平台错误归类
  tcp/
    mechanism.cpp                Mechanism::Impl：连接表、acceptor 集合、在途 connect
    acceptor.cpp                 accept 循环 + 退避重开
    conn.cpp                     Conn::Impl：socket + 读槽 + 写队列 + 保活
    connect_op.cpp               单次在途 connect（socket + timer）
  udp/
    endpoint.cpp                 UdpEndpoint::Impl：socket + 接收循环 + 缓冲 + peer 跟踪
```

**依赖方向**：`tcp/udp → common → 无`；`tcp` 与 `udp` 互不依赖；`io_pool` 被两者共用。任何一侧都不允许 include 另一侧的实现头。

---

## 2. 线程模型（三条不变量）

| # | 不变量 | 依据 |
|---|---|---|
| T1 | 每个 `io_context` 恰好由 1 条线程 `run()` | 由 `IoPool` 保证 |
| T2 | 一个 `Conn` / `UdpEndpoint` 创建时绑定**一个** `io_context` | `IoPool::next_io_context()` 轮转 |
| T3 | 因此同一对象的全部回调在同一线程上串行 | 由 T1+T2 推出，**不使用 strand** |

跨线程入口只有三处，各有明确手段：

| 入口 | 手段 |
|---|---|
| `Conn::read` / `write` / `close` | 调用线程持**发送队列锁**完成"检查 + 入队"，再 `post` 一次唤醒（见 §5） |
| `UdpEndpoint::send_to` | 内部的 socket 互斥锁，直接同步 `send_to` |
| 各类查询（`is_open` / 统计 / `last_peer`） | 原子量或短临界区读，不阻塞 |

> T1 一旦被打破（比如改成 N 线程 run 同一 context），strand 必须同时恢复，否则"同一连接回调串行"的契约失效。这条写进代码注释，避免后人误改。

---

## 3. 对象与所有权

### 3.1 TCP

```
Mechanism (shared_ptr，用户持有)
└── Mechanism::Impl (shared_ptr)
    ├── IoPool&                      引用，不持有（进程级单例）
    ├── acceptors : vector<shared_ptr<Acceptor>>
    ├── endpoints : map<id, ConnEntry>         ConnEntry 强引用 Conn::Impl
    └── pending : map<key, shared_ptr<ConnectOp>>

Conn::Impl (shared_ptr)
├── socket / timer
├── reads  : ReadQueue      单槽（optional<Request>）
├── writes : WriteQueue     deque + in_flight
├── recv_ka / send_ka
├── handler : shared_ptr<ConnHandler>   库强持有，on_close 后释放
└── linkable: weak_ptr<Linkable>        弱引用，用户可先死
```

**强自持**：投递到 `io_context` 的每个完成处理都捕获 `Conn::Impl` 的 `shared_ptr`，所以连接一定活到最后一个 IO 完成——关闭流程（冲刷挂起回调 → `on_close` → `on_linkdown`）不会丢。由此形成的环 `context → 队列 → Impl → IoPool` 因为 context 永不销毁而无害。

### 3.2 UDP

```
UdpEndpoint::Impl (shared_ptr)
├── socket
├── recv_buf : 一块 recv_capacity 大小的缓冲（borrow 模式复用）
├── handler  : shared_ptr<UdpHandler>
├── last_peer : optional<Address> + 是否已收到过
└── 统计：received / sent / truncated / last_recv_time
```

UDP 端点**不注册到任何全局表**（按契约 U1/U3，库不管会话）。用户持有 `shared_ptr<UdpEndpoint>` 即持有它；最后一个引用释放时若未关闭，等效于 `close()`。

### 3.3 销毁顺序（TCP，固定不变）

```
① 调用方 close()  → 置 closing，post request_close
② io 线程 do_close：
   a. cancel 挂起的 socket 操作与定时器
   b. 读槽里的请求 → on_read(ec=link_closed, buf, 0)
   c. 写队列按 FIFO 逐个 → on_write(ec=link_closed, buf, 已发字节)
   d. on_close(c)
   e. 释放 handler 的 shared_ptr（对象可能在此销毁）
   f. 从 Mechanism 的连接表摘除（ConnEntry 释放）
   g. linkable->on_linkdown(c)
```

`on_linkdown` 在第 e 步之后，所以只给 `Conn&` 不给 handler——与契约 §13 一致。

---

## 4. 状态机

### 4.1 连接状态

现状用 4 个独立 `atomic<bool>`（`closing` / `closed` / `unregistered` / `has_pending_read`）表达，合法组合全靠人记。**本次改为显式枚举**：

```cpp
enum class ConnState : std::uint8_t {
    opening,    // 已创建，尚未 on_open 成功
    active,     // 可读写
    closing,    // 已请求关闭，正在冲刷
    closed,     // 关闭流程完成
};
```

`has_pending_read` 这类**IO 状态**不放进连接状态，留在 `ReadQueue` 自己表达（`in_flight` 有值即为真）。这样"连接状态"与"IO 状态"两层分开，非法组合从结构上消失。

迁移期用 `atomic<ConnState>` + `exchange` 保证幂等；`close()` 只在 `active → closing` 那一次生效。

### 4.2 读写状态机（TCP）

- **读**：单槽。`read()` 时槽位被占则返回 false；完成后取走槽位、回调、槽位清空。
- **写**：`deque<Request>` + `in_flight` 标记。受理即入队；`in_flight == false` 时启动队首；每笔完成回调后 `pop_front` 再启动下一笔。

两者都**不依赖"队列长度"做判据**（旧实现里"长度==1 表示该我发起"在锁外被消费，导致丢唤醒），只看显式标记。

### 4.3 保活状态机（TCP）

```
每 interval 一拍（timer 重投）：
  recv 方向：自上次拍以来收到过数据？
        是 → miss = 0
        否 → miss++ → on_idle(kind=recv, missed=miss, limit=max_miss)
                    → miss >= max_miss ? 进入关闭流程 : 继续下一拍
  send 方向：同上，判据换成"有无成功写出"
```

- `generation` 计数：每次 `enable_*` 递增，timer 回调带 gen，不匹配就丢弃（防陈旧定时器误触发）。
- 首拍在 `delay + interval` 之后；`delay == 0` 表示 `interval` 后第一拍。
- **每一拍未通过都要回调**，这是契约要求（`on_idle` 的 `missed` 从 1 递增到 `limit`）。

### 4.4 UDP 接收循环

```
post(do_receive)
  → async_receive_from(recv_buf, sender)
  → 完成：
      if (ec) { 错误分类；致命 → on_close；否则退避重投 }
      else {
          if (截断) { truncated++; }
          if (sender != last_peer) { last_peer = sender; on_peer_changed(...) }
          deliver(buf, n, sender)      // borrow: 直接给；own: 先 make_shared 再给
          received++; last_recv_time = now
          post(do_receive)             // 立刻再投，保持接收常驻
      }
```

关闭时 `cancel` 让挂起的 `async_receive_from` 以 `operation_aborted` 结束，循环退出。

---

## 5. 发送路径与反压（TCP 关键）

```
调用线程                                 io 线程
─────────                                ─────────
write(buf)
 ├─ closing? → false
 ├─ lock(queue_mutex)          ← 顺序与配额在此锁内一并决定
 │   ├─ 配额检查（bytes/count）→ 超限 false
 │   ├─ reserved += n; queue.push_back(req)
 │   └─ need_post = !pump_scheduled; pump_scheduled = true
 ├─ unlock
 └─ if (need_post) post(pump) ─────────► pump():
                                            取队首 → async_write
                                            → 完成 → on_write(ec, buf, n)
                                            → 释放配额 → 下一笔 or pump_scheduled=false
```

**为什么锁必须在这一侧**：只有"检查 + 入队"都在调用线程的锁内完成，`write()` 才能同步返回准确结果，并且受理顺序 = 入队顺序。（若改成"post 到 io 线程再判断容量"，超限的请求只能事后丢弃，违反"受理即恰好一次回调"——见契约 U19。）

配额（`reserved_bytes` / `reserved_count`）在**完成**时归还，而不是入队时；这样反压反映的是"尚未写完的数据量"。

---

## 6. UDP 实现要点

| 事项 | 做法 |
|---|---|
| 接收缓冲 | borrow 模式复用一块 `recv_capacity` 缓冲；`buf.owner` 留空 |
| own 模式 | 按实际长度 `make_shared<std::vector<uint8_t>>(n)`，`owner` 指向它 |
| 截断检测 | 封一个 `bool datagram_truncated(const std::error_code&, std::size_t n, std::size_t cap)`：Windows 认 `WSAEMSGSIZE` / `asio::error::message_size`，Linux 认 `MSG_TRUNC`；同时在 `n == cap` 时按截断处理兜底 |
| peer 跟踪 | `optional<Address> last_peer`，每次收包比较；不同则先 `on_peer_changed` 再 `on_read` |
| send_to | 内部 `std::mutex` 保护 socket；`send_to` 失败按 `EAGAIN/ENOBUFS → buffer_full`、`EMSGSIZE → invalid_argument` 归类。**socket 必须保持非阻塞**，否则一次发送缓冲满就会卡死整条 io 线程 |
| 组播 | 创建时 `join_group`（可指定网卡），关闭时 `leave_group`；发送组播靠 `UdpConfig::multicast_hops/loopback` |
| Windows ICMP 反馈 | UDP socket 收到 ICMP port unreachable 后，下一次 `recv`/`send` 会返回 `WSAECONNRESET`（Linux 不会）。按**该端点的一次失败**处理，不能当致命错误关闭端点 |
| 端口 0 | bind 后 `local_endpoint()` 回填，供 `local_address()` 查询 |

---

## 7. 与现有代码的映射

| 现有 | 新位置 | 处理 |
|---|---|---|
| `include/netmech/executor.hpp`（IoPool） | `src/io_pool.hpp` | 改内部，`global()` → `IoPool&`，去掉 `enable_shared_from_this` |
| `include/netmech/fwd.hpp` | 删除 | `tcp` 别名、`noncopyable` 移入 common 或删除 |
| `include/netmech/endpoint.hpp` | `tcp.hpp` 的 `Conn` | 重写：Pimpl 保留，接口换掉 |
| `include/netmech/iohandler.hpp` | `tcp.hpp` 的 `ConnHandler` | 重写为 5 个虚函数 |
| `include/netmech/linkable.hpp` | `tcp.hpp` 的 `Linkable` | 返回 `shared_ptr<ConnHandler>`，删 `reject` 类语义 |
| `include/netmech/keepalive.hpp` | 删除 | 参数直接进 `enable_*_keepalive` |
| `include/netmech/protocol_traits.hpp` | `src/` | 内部编译期探测 |
| `src/endpoint_state.hpp` | `src/tcp/conn_state.hpp` | `ReadQueue`/`WriteQueue`/`RecvKeepAlive`/`SendKeepAlive` 保留，状态改枚举 |
| `src/acceptor.*` | `src/tcp/acceptor.*` | 逻辑基本不动，落盘路径调整 |
| `src/mechanism.cpp` | `src/tcp/mechanism.cpp` | 连接表保留，`Address` 替换 `tcp::endpoint` |
| `src/udp_channel.cpp` + `src/datagram_receiver.cpp` | `src/udp/endpoint.cpp` | 两套合并为一套，去掉会话表与 peer 索引 |
| `src/io_pool.cpp` | `src/io_pool.cpp` | `shared_ptr` 全部改引用 |

---

## 8. 实施顺序

每步结束时项目必须可编译、现有测试可跑（渐进替换，不留半成品）。

| 步 | 内容 | 验证 | 状态 |
|---|---|---|---|
| 0 | 建立基线：cl 直接编译库 + 测试，跑全量用例 | 90 用例 / 3180 断言全绿 | ✅ |
| 1 | `io_pool`：IoPool 由 `shared_ptr` 单例改为**引用**单例，全部持有者改 `IoPool*` | 90 用例 / 3119 断言全绿；库与测试零警告 | ✅ |
| 2a | `common.hpp` 落地：`Address` / `Buf` / `ConstBuf` / `NetErrc` / `initialize` / `shutdown`；`error.hpp` 退化为兼容壳；新增 `src/address.cpp`、`src/runtime.cpp` | 100 用例 / 3156 断言全绿（新增 10 个 `Address` 用例）；零警告 | ✅ |
| 2b | `ConnHandler` 接口替换 `IoHandler`；`Endpoint::read/write` 改为 Buf/ConstBuf；保活分级 | 62 用例 / 1787 断言全绿（11 个旧测试移入 `test/legacy`，新写 3 个用例） | ✅ |
| 2c | `Endpoint` → `Conn`（文件同步改名 `conn.hpp` / `conn.cpp` / `conn_state.hpp`）；`Linkable::on_accept/on_linkup` 改返回值；`Conn::user_context`；`Mechanism::write/close(id)` | 64 用例 / 1808 断言全绿（新增 user_context 透传、按 id 收发/关闭两组用例） | ✅ |
| 2d | `Address` 替换公共接口的 `tcp::endpoint`；`Conn::id()` 与 `endpoint_id_t` 扩到 `uint64` | 64 用例 / 1808 断言全绿 | ✅ |
| 2e | ~~`ConnectFuture` / `cancel_connect`~~ **已取消**（见 §8.2） | — | ❌ |
| 3 | UDP：新 `UdpEndpoint` 落地（推送式接收、`send_to`、`last_peer`/`on_peer_changed`、截断、组播） | 75 用例 / 2023 断言全绿（新增 3 个 UDP 用例） | ✅ |
| 4a | 清理：删旧 UDP 两套 API + 2 个实现 + 4 个测试；`IoPool` 移入 `src/`；`error`/`fwd`/`buffer`/`keepalive`/`protocol_traits` 全部收敛（公共头 14 → 7） | 32 用例 / 342 断言全绿（用例数下降是删掉了已不存在功能的旧测试） | ✅ |
| 4b | 更新 `example/`（TCP echo + UDP echo）到新接口；CMakeLists 的构建条件修正 | 两个示例实跑通过（全链路日志核对） | ✅ |
| 5  | 补齐回归与新增能力：`Timer`（端点级定时）、`find_free_port_run`、`create_udp_endpoint_on`、SSRC 冲突/端口占用/协商流程的用例 | **46 用例 / 604 断言全绿**；Windows + Linux + aarch64 三平台零警告 | ✅ |

> 上表每一步的用例数是**当时的快照**（删旧测试时会下降），最终状态见
> [README](../../README.md) 与 [CHANGELOG](../../CHANGELOG.md)。

---

## 8.1 实施记录

**第 1 步（IoPool 收口）已完成**

- `IoPool::global()` 由 `std::shared_ptr<IoPool>` 改为 `IoPool&`；去掉 `enable_shared_from_this`。
- 线程池启动失败由"返回空指针"改为 `std::terminate()` + stderr 说明（没有 io 线程时任何网络对象都无法工作，静默失效更难查）。
- 全部持有者改为 `IoPool*`：`Acceptor::pool_`、`Endpoint::Impl::pool`、`Mechanism::Impl::pool`、`UdpChannel::pool_`、`DatagramReceiver::pool_`；构造入参统一为 `IoPool&`。
- `Acceptor` 里原来的 `context_of(const shared_ptr<IoPool>&)` 退化分支（pool 为空时用 thread_local 兜底）已删除——池不可能为空。
- `~Mechanism` 不再 `pool.reset()`；池是进程级单例，不随实例析构而停止。
- 测试侧：`test_global_pool.cpp`、`test_io_pool_safety.cpp` 按新语义重写；四个 UDP 测试里的 `ScopedPool` 简化为空壳（仅保留 `operator*` / `get` / `shared` 供既有调用点使用）。

**第 2a 步（common.hpp 落地）已完成**

- `include/netmech/common.hpp` 新增：`initialize` / `shutdown`、`Address`、`Buf` / `ConstBuf`、`NetErrc` 及错误码桥接。
- `include/netmech/error.hpp` 退化为 4 行兼容壳（`#include "netmech/common.hpp"`），旧代码无需改动。
- `src/address.cpp`：`Address` 用 128 位 + 端口 + 地址族存放，与 asio 的转换全部收敛在此文件；公共头不暴露 asio 地址类型。
- `src/runtime.cpp`：`initialize` / `shutdown` 转调 `IoPool`，把"实现层"隔在 `common.hpp` 之外。
- 新增 `test/test_address.cpp`（10 个用例）：构造、parse（IPv4 / IPv6 方括号 / 仅端口 / 6 类非法输入）、`to_string` 往返、比较、`is_unspecified`。

**第 3 步（UDP 单端点）已完成**

- `include/netmech/udp.hpp`：`UdpEndpoint` / `UdpHandler` / `UdpConfig` / `DeliverMode` / `kMaxDatagramSize`，
  与 `IDatagramChannel` / `IDatagramReceiver` 两套旧 API 并存（旧头暂留，最后一步删）。
- `src/udp_endpoint.cpp`：单端点实现 —— 持续 `async_receive_from` 的推送式投递、同步 `send_to`（非阻塞）、
  `last_peer` + `on_peer_changed`、borrow/own 两种交付形态、截断检测、组播加入、收发/截断统计。

**两个实现期发现（都已修/已记录）：**

1. **错误码类别不一致（真 bug）**。库内部发的是 `make_boost_error_code()`（独立 category + `kNetErrcBase` 偏移），
   而调用方写 `ec == NetErrc::xxx` 时比较的是 `net_category()` —— 两者 category 不同，**永远不相等**。
   也就是说在此之前，任何"用枚举判断库返回的错误码"的代码都是失效的。
   已把 `make_boost_error_code` 统一为转发 `make_error_code`（名字保留，调用点不动），
   并在 `from_boost_error_code` 里先认 `net_category`、再兼容历史 boost 类别。
2. **截断检测不能只依赖 `message_size`**。实测 Windows 上某些路径不报该错误，报文被静默截断。
   现在判定是 `ec == message_size || n >= recv_capacity` —— 读满缓冲一律按截断处理，
   宁可误报也不把半条报文当完整的交出去。

**第 2b 步（ConnHandler 接口替换）已完成**

- `include/netmech/iohandler.hpp` 重写为 `ConnHandler`：`on_open` / `on_read(ec, Buf, n)` / `on_write(ec, ConstBuf, n)` / `on_close` / `on_idle(IdleInfo)`。
- `Endpoint` 的 `async_read_some` / `async_read_exact` / `async_write(buf, lambda)` 合并为 `read(Buf, ReadMode)` / `write(ConstBuf)`；每笔操作不再带 lambda。
- `ReadQueue::Request` / `WriteQueue::Request` 去掉 per-op handler，改存 `Buf` / `ConstBuf`；完成时经 `notify_read` / `notify_write` 统一派发到 handler 虚函数。
- 保活改为**分级预警**：两个 tick 在每拍未通过时回调 `on_idle{kind, missed, limit}`，仅在最后一拍后断链；`SendKeepAlive` 增加 `max_miss` / `miss`，`enable_send_keepalive` 增加 `max_miss = 3`，与接收侧对称。
- 旧测试：11 个引用旧接口的文件移入 `test/legacy/`（不参与构建）；新增 `test/test_conn_basic.cpp` 覆盖 echo 往返、关闭通知恰好一次、单读在途约束。

**实施方式说明**：本项目 CMake 走 MSBuild，被本机安全策略拦截，改用 `cl` 直编（rsp 响应文件）+ 手工链接，编译选项与 CMake 一致（/W4 /permissive- /utf-8 /MD）。

**2b 的规模（实测）**：`IoHandler` 52 处、`Endpoint` 95 处、`Linkable` 182 处、`Mechanism` 112 处，跨 19 个测试文件。这不是一次改动能安全完成的量，必须再拆。

**踩到的一个坑（写测试时）**：`IoPool::next_io_context()` 是 round-robin，连续两次调用会返回**不同**的 `io_context`。不能拿它比较地址来判断"是不是同一个池"——要验证的是"取到的 context 可用"。这一条已写进测试注释。

---

## 8.2 实施期的设计偏离：取消 `ConnectFuture`

原设计（`docs/design/proposed/netmech/tcp.hpp` v1.0）提供了 `ConnectFuture`
作为 connect 的同步等待句柄，参数带 `HandlerFactory`。

**实现阶段取消，理由：**

1. **`Conn` 自身没有任何完成通知。** read/write 的完成回调全部在
   `ConnHandler` 上。所以"只拿一个 Conn、不接 handler"的用法不存在——
   拿到连接也只能盲发盲收。
2. **handler 必须有且只有一个来源。** 带 `HandlerFactory` 的 connect 等于开了
   第二条创建路径，与 `Linkable::on_linkup` 冲突：同一件事两套写法，且两套
   的生命周期规则还不一样。
3. **去掉 factory 之后，future 只剩一个等待壳。** 参数与异步 `connect`
   完全一致（同样需要 `linkable`），唯一差别是"多一个能 wait 的句柄"。
   而"连接失败要能知道"这件事，`Linkable::on_linkfail` 已经给了。
   加一层 future 只是把回调换成轮询，没有本质收益，却要多维护一层 promise
   与跨线程状态。

**当前形态**（与设计文档的这一处不同，其余一致）：

```cpp
// 唯一入口：结果走 on_linkup / on_linkfail，handler 由它们创建
std::error_code connect(const Address& remote,
                        std::weak_ptr<Linkable> linkable,
                        std::uint32_t timeout_ms = 3000,
                        const Address& local = Address{},
                        std::shared_ptr<void> user_ctx = nullptr);
```

`NetErrc::handler_rejected` / `cancelled` 两个枚举值保留（语义仍有用：
`on_open` 返回 false、将来若恢复取消能力），但当前无调用点。

---

## 9. 风险与对策

| 风险 | 说明 | 对策 |
|---|---|---|
| 测试迁移面大 | 现有 24 个测试文件全部针对旧 API | 第 2 步一次性改完 TCP 用例，不留"新旧并存"状态 |
| 静态初始化顺序 | IoPool 若用函数局部 static，析构可能与 io 线程 join 打架 | 用**永不销毁**的堆对象（`static IoPool* = new IoPool(n)`），进程退出由 OS 回收 |
| 跨平台截断检测 | Windows/Linux 语义不同 | 封 `datagram_truncated()` 一处判断，两侧各写一个分支 + 单测 |
| 状态枚举迁移出错 | `closing/closed` 拆分不当会丢回调（历史 P0） | 保留"closing 只置意图、closed 由 io 线程置"的分离，写进注释并配单测 |
| 回调里重入 | 用户可能在 `on_read` 里 `close()` | 所有状态变更走 post，回调返回后统一收敛 |
| 无在途读则感知不到对端关闭 | TCP 的 FIN 只在 read 时被感知；handler 若从不读，连接会静默挂着 | 已写进 `ConnHandler::on_open` 的 @warning；保活可作为兜底 |

---

## 10. 压力测试记录（首批）

工具：`bench/stress.cpp`。它不是单元测试，目的是**压到出问题**：N 个客户端同时发起连接，
每个串行发 M 条并逐条校验回显内容，最后检查连接表是否收敛。

```
用法: stress [conns] [msgs_per_conn] [msg_size] [churn]
      churn=1 表示连上立刻 close（压关闭竞态）
```

| 场景 | 耗时 | 结果 | 吞吐 |
|---|---|---|---|
| 10 × 10 × 64 B | 0.024 s | PASS | 4.2k msg/s |
| 100 × 100 × 64 B | 0.310 s | PASS | 32.2k msg/s |
| 500 × 100 × 64 B | 1.684 s | PASS | 29.7k msg/s |
| 1000 × 50 × 64 B | 1.711 s | PASS | 29.2k msg/s |
| 2000 × 20 × 64 B | 1.815 s | PASS | 22.0k msg/s |
| 100 × 50 × 4096 B | 0.191 s | PASS | 26.1k msg/s / **102 MiB/s** |
| 2000 连接 churn（连上即断） | 0.966 s | PASS | — |

全部场景 `bad_content = 0`、`write_rejected = 0`、`read_error = 0`、`endpoint_count` 归零。

**对吞吐数字的说明**：这是**请求-响应**模式（发一条 → 等回显 → 再发下一条），
每轮受一个回环 RTT 限制，所以 ~30k msg/s 反映的是 RTT 与并发度，**不是库的吞吐上限**。

### pipeline 模式（不等回显连续写）

```
stress 100 1000 64 0 1     # 100 连接 × 1000 条 × 64B，pipeline
```

结果：0.739 s，100/100 连接全关，`tx = 100000`、`write_rejected = 0`、`bad_content = 0`、
`endpoint_count` 归零 —— 也就是 **6.4 MB 上行 + 6.4 MB 回显、零差错**。

但吞吐只有 16.4k msg/s（1.0 MiB/s），**比串行模式还低**。原因是压测的服务端是
"读一条 → 写一条 → 再读"的串行 echo，每一轮仍要等一次回写完成；
客户端灌得再快也会被服务端的处理节奏卡住。所以这个数字测的依然是往返链路，
不是库的发送能力。要测真正的上限，需要单向模式（server 只收不回）。

### 发送路径：批量提交（scatter-gather）

原实现每笔 write 一次 `async_write`，小包高频场景下系统调用开销直接吃掉吞吐。
改为**一批最多 16 笔 / 64 KB 合成一个 buffer sequence**，一次提交：

```
pump_write():
  收集队首连续 N 笔（N ≤ 16，累计 ≤ 64KB）
  → async_write(socket, bufs /* array<const_buffer,16> */, handler)
  → 记录 in_flight_count = N
```

**难点在回调归因**：ASIO 一次 `async_write` 只回调一次，返回的是**总字节数** n；
而契约要求"发了几笔就回调几次"。`handle_write_done` 按 FIFO 从队首逐笔扣 n：

```
remaining = n
while (未完成笔 && 已归因笔数 < in_flight_count):
    若 该笔剩余量 ≤ remaining → 这一笔完整写完，出队，记入 finished
    否则                       → 这一笔只写出一部分，推进 sent，下轮续传
```

- 完整写完的笔：归还配额、`bytes_written += bytes`、**各自回调一次** `on_write(ec, buf, bytes)`
- 部分写出的那笔：`Request::sent` 推进，下次从断点续传（不会重发已发出的部分）
- 关闭时 `drain()` 归还的笔带真实 `sent`（不是恒为 0）

**踩的坑**：第一版写成 `asio::async_write(socket, asio::buffer(bufs.data(), count), ...)`，
这会把 `count` 当**字节数**，发给对端的是 `const_buffer` 对象自身的内存 ——
接收端全是乱码，三个用例当场失败。必须直接传数组本身（`asio::async_write(socket, bufs, ...)`），
未使用的元素是零长度 buffer，asio 自动跳过。

**效果**（pipeline 模式，100 连接 × 1000 条 × 64B = 6.4 MB 单向）：

| | 耗时 | 发送 | 接收 |
|---|---|---|---|
| 批量前 | 0.739 s | — | — |
| 批量后 | **0.199 s** | 502k msg/s | 30.7 MiB/s |

**3.7 倍**。串行模式（等回显再发下一条）无提升——每笔之间隔着一次 RTT，
队列里永远只有一笔，没有可合并的东西（实测 30.7k msg/s 与批量前持平）。

### 单向模式（server 只收不回）

```
stress conns msgs size 0 0 1     # 最后一个参数 sink=1
```

这才测得出发送侧的上限（没有回程流量）：

| 场景 | 耗时 | 发送 | 接收 |
|---|---|---|---|
| 100 × 20000 × 64B（128 MB） | 2.63 s | 761k msg/s | 46.4 MiB/s |
| 500 × 5000 × 64B（160 MB） | 3.68 s | 680k msg/s | 41.5 MiB/s |
| 1000 × 2000 × 64B（128 MB） | 3.13 s | 640k msg/s | 39.1 MiB/s |

全部 `bad_content = 0`、`link_fail = 0`、`endpoint_count` 归零。规模上去吞吐略降（640k vs 761k），
是 1000 条连接挤在 4 个 io 线程上的调度开销，不是缺陷。

### 两个与并发规模相关的发现

**① 默认 `backlog = 128` 撑不住几百个同时到达的连接。**
500 个并发 connect 时出现 `link_fail = 39`（`opened 461/500`）—— 握手阶段就被拒。
把 `listen(..., backlog)` 显式调到 4096 后降到 0。这不是 bug（backlog 本来就可配），
但**默认值对"上千连接同时重连"的场景偏小**，值得在文档里提醒。

**② `Mechanism::Config::linger_zero` 默认为 `true`。**
也就是 `close()` 走 **RST** 而不是优雅 FIN，内核缓冲里未发出的数据会被丢弃。
已加用例 `peer_reset_reports_error_and_closes` 验证：对端 RST 后本端读以错误码归还、
`on_close` 恰好一次。但对"发完应答再关"的协议（如 Modbus 响应）需要注意：
**如果 close 时还有未发出的数据，会丢**。

### 长跑（soak）

工具：`bench/soak.cpp`。循环"建 N 个连接 → 各发 M 条 → 全关 → 检查收敛"，定期打印工作集。

```
soak [duration_sec] [conns] [msgs]
```

60 s / 20 连接 / 10 条：**322 轮、64,400 条消息、0 失败轮次**，
RSS 9.58 → 10.06 MB（前 10 轮涨起来后平稳，无持续增长）。

### 修掉一个真实缺陷：`linger_zero` 之前根本不生效

`close_socket_noexcept` 在流协议下是 **先 `shutdown(SHUT_RDWR)` 再 `close`**（为了修 A-16 的数据/FIN 乱序）。
但 `shutdown` **已经发出了 FIN**，连接随之进入正常关闭流程 —— 之后再 close 时
`SO_LINGER{1,0}` 已经来不及发 RST，主动关闭方照样进 TIME_WAIT。
也就是说 `Config::linger_zero` 这个开关一直是"名义存在、实际无效"的。

已修：`close_socket_noexcept(sock, graceful)` 增加开关，`close()` 路径按
`graceful = !linger_zero` 选择——要 RST 就**跳过 shutdown 直接 close**。

**但有一个 Windows 特有的坑要记住**：即使跳过 shutdown，`linger{1,0}` 在
**发送缓冲为空**时仍可能走正常关闭（发 FIN）。soak 实测：50 连接/轮跑到 291 轮后
`link_fail` 全量出现，`netstat` 显示 14,475 个 TIME_WAIT —— 动态端口（16384 个）被吃满。

结论：**建连速率必须低于 `16384 / 120 ≈ 136 连接/秒`**，否则几百轮后端口耗尽。
soak 因此按该预算自动加轮间隔（`gap = conns/130` 秒）。这是 TCP/OS 的固有约束，
不是库能规避的；对 Modbus 长连接场景（连接少、不频繁重连）不构成问题。

### 批量发送参数扫描（Linux，单向 sink，100 × 20000 × 64B）

**第一轮：固定 64 KB，扫 `kMaxWriteBatch`**

| batch | 吞吐 | 相对 batch=1 |
|---|---|---|
| 1（等价旧实现） | 298k msg/s | 1.0× |
| 2 | 520k msg/s | 1.7× |
| 4 | 951k msg/s | 3.2× |
| 8 | 1.55M msg/s | 5.2× |
| **16（当前值）** | **2.14M msg/s** | **7.2×** |
| 32 | 2.11M msg/s | 7.1× |
| 64 | 2.07M msg/s | 6.9× |

**拐点在 16**：8→16 还有 38% 提升，16→32 已经回落。
再往上只是增加单次系统调用的 buffer 数组长度，收益为负（asio 内部对
buffer sequence 的遍历成本 + 尾延迟变差）。

**第二轮：固定 batch=16，扫 `kMaxWriteBatchBytes`**

| 上限 | 吞吐 |
|---|---|
| 16 KB | 2.04M |
| 32 KB | 2.04M |
| **64 KB（当前值）** | **2.13M** |
| 128 KB | 2.15M |
| 256 KB | 2.15M |

64 KB → 128 KB 只有 **0.9%** 提升，不值得为它牺牲尾延迟特性（单次提交的数据量越大，
队首大包阻塞后续小包的时间越长）。**保持 64 KB。**

结论：`kMaxWriteBatch = 16` / `kMaxWriteBatchBytes = 64 KB` 这两个"拍脑袋"的默认值，
实测下来就是拐点附近，不需要调整。

### 仓库清理

- `test/legacy/`（17 个旧测试）已删除。里面的用例要么覆盖的功能已在设计中砍掉
  （旧 UDP 会话 API、strand、池内部），要么已按新接口重写（见上表）。
- `bench/` 三个文件在 `-DNETMECH_BUILD_BENCH=ON` 下均能编译：
  `stress`（并发/吞吐）、`soak`（长跑）、`bench_strand`（strand 开销基准，
  文件头记录了"本库不用 strand"的量化依据，保留）。
- 删源文件后 **必须清 `build-linux/` 再构建**：cmake 的 `CONFIGURE_DEPENDS`
  会重新配置，但残留的 `.o` 仍会被链接进去，表现为"测试莫名其妙失败一次、
  重跑又好"。

### 跨平台验证（Linux / GCC 15）

`build_linux.sh` 在 WSL2（Ubuntu + GCC 15.2 + CMake 4.2）上跑通：**38 用例 / 566 断言全过，零警告**。

**GCC 抓到 3 个 MSVC 放过的问题**（全部已修）：

1. **`create(Config cfg = Config{})` 在 GCC 下非法**。
   `Config` 是嵌套类型，它的**默认成员初始化器在外部类定义完成之前不可用**，
   GCC 直接报 `default member initializer ... required before the end of its enclosing class`。
   拆成 `create()` / `create(Config)` 两个重载（MSVC 因为实现更宽容而没报）。
2. **`-Wswitch`：新增的 `handler_rejected` / `cancelled` / `datagram_truncated` 没在
   `net_errc_message` 的 switch 里处理**。MSVC 的 `/W4` 默认不报这个，GCC 的 `-Wall` 会。
3. **未使用的静态函数**（`expected_auto_threads`，删池测试后遗留）。

### 性能对比：Windows vs Linux（单向 sink，100 × 20000 × 64B）

| 平台 | 耗时 | 发送 | 接收 |
|---|---|---|---|
| Windows / MSVC 14.44 + IOCP | 2.63 s | 761k msg/s | 46.4 MiB/s |
| Linux / GCC 15.2 + epoll | **1.05 s** | **1.90M msg/s** | **115.9 MiB/s** |

Linux 快 **2.5 倍**。回环 + epoll 在这个"纯写"场景下比 IOCP 路径短，
差异主要来自完成通知机制，不是库本身的算法差异。

长跑（60 s / 20 连接 / 10 条）：371 轮、74,200 条、**0 失败**，
RSS 4.48 → 4.94 MB（+0.46 MB，平稳）。

### 压测过程中修的两个**测试代码** bug（不是库的问题）

1. **把"一次 read"当成"一条消息"**。TCP 是字节流，pipeline 下一次 `read` 会带回几十条消息。
   按次数计数导致 `rx = 12083` 而 `tx = 100000`，看着像丢数据，实际字节数完全正确。
   改成按字节累加判定。
2. `rx_bytes` 只累加到了连接对象的成员，忘了同步到全局统计。

### 压测直接暴露的问题

**同一目标地址无法并发 connect**。第一跑 `stress 10 10 64` 就只受理了 1 个连接：
`Mechanism::connect` 用 `remote` 作为在途表的唯一键，第二次调用被判为
`duplicate_endpoint` 拒掉。对"一个目标建多条连接"（连接池、并发探测）来说是硬伤。

已修：在途表的键改为 `remote + 自增序号`，去掉重复判定。修改后 2000 个并发连接全部受理。

### 尚未覆盖

- **pipeline 吞吐**（不等回显连续写）—— 需要新压测模式
- **对端异常断开**（RST）—— 需要注入异常
- **长时间运行**（小时级）—— 观察内存曲线
- `test/legacy/test_protocol_abstraction.cpp` 未重写：它测的是内部 `protocol_traits`
  （该头已移入 `src/`），属于实现细节，不再由公共测试覆盖

### 回归测试回填（原 legacy 用例）

`test/conn_regress.cpp` 按新接口重写了 4 个历史缺陷的守卫：

| 用例 | 守的是什么 |
|---|---|
| `regress_relisten_same_port_after_close_all` | `close_all()` 之后能**立刻**重新 listen 同一端口（TIME_WAIT 不能挡住重启） |
| `regress_local_address_of_returns_latest_ephemeral` | 端口 0 多次 listen 时，查询返回**最近一次**绑定（不能被入参地址键覆盖） |
| `regress_write_callbacks_exactly_once_on_failure` | 断链时每笔已受理的 write **恰好一次**回调（B-15 / A-03 的静默丢弃与双飞行） |
| `regress_no_handler_leak_across_cycles` | 30 轮建连/销毁后所有 handler 都已析构（原先的 tcp_leak） |

`test_protocol_abstraction.cpp` 不重写：它验证的是 `protocol_traits` 里
`close_socket_noexcept` 对 tcp/udp 的适配，属实现细节；该头移入 `src/` 后
不应再由公共测试覆盖。
