# NetMechanism UDP 使用指南

> 面向使用者的实操文档。设计依据见 [UDP支持评估](UDP支持评估.md)，实现演进见 P1/P2/P3 交付记录。

---

## 一、先选对工具：三种 UDP 用法

本库提供三个层次的 UDP 能力，**按你的场景选一个**：

| 你的场景 | 用这个 | 一句话 |
|---|---|---|
| 已知对端地址，双向收发 | `DatagramChannel`（单对端） | 像 TCP 客户端，但无连接 |
| 服务端，要接受任意来源 | `DatagramReceiver` + `Session` | 像 TCP 服务端，但按源地址区分会话 |
| 只需要一个裸 socket 收发任意地址 | `DatagramChannel`（多对端） | 自己管对端地址 |

```cpp
// 1) 客户端式：已知对端
auto ch = make_datagram_channel(pool, cfg, &peer, local_bind, ec);
ch->async_send(buffer(data), udp::endpoint() /*忽略*/, cb);
ch->async_receive(buffer(buf), cb);   // cb 里有 from

// 2) 服务端式：多会话
auto recv = make_datagram_receiver(pool, rcfg, handler, ec);
// handler->on_session_open(session) 里拿到每个对端的会话

// 3) 裸 socket 式：一个 socket 收发任意地址
auto ch = make_datagram_channel(pool, cfg, nullptr /*无默认对端*/, local_bind, ec);
ch->async_send(buffer(data), peer_addr, cb);   // 必须给地址
```

---

## 二、必须知道的三条语义

### 2.1 数据报是原子的

UDP 没有"流"的概念。一次 `async_receive` 收到的是**完整的一个数据报**，
不存在"收了一半"。

因此：
- **没有 `async_read_exact`**（那对数据报无意义）；
- 若你的 buffer 小于数据报 → 返回 `datagram_truncated`，`n` 为 buffer 大小；
- **buffer 至少要等于协议最大包长**，否则会被截断。

```cpp
std::array<char, 2048> buf;   // 若协议包最长 1500，这个够用
```

### 2.2 buffer 生命周期由你负责

库**不拷贝**内存。buffer 必须存活到**回调被调用为止**：

```cpp
// ✗ 错误：临时对象，async_send 返回后立即销毁
ch->async_send(asio::buffer(std::string("hi")), peer, cb);

// ✓ 正确：用 shared_ptr 持有，并让回调捕获它
auto data = std::make_shared<std::string>("hi");
ch->async_send(asio::buffer(*data), peer,
               [data](error_code, size_t) { /* data 在此存活 */ });
```

### 2.3 返回 false = 未受理，不会回调

`async_send` / `async_receive` 返回 `false` 时：

| 原因 | 你的处理 |
|---|---|
| 发送队列满 | 稍后重试 / 丢弃 / 记录丢包 |
| 已有接收在途 | 等当前接收回调后再挂（**单笔在途约束**） |
| 已关闭 | 忽略 |

**此时 buffer 仍归你**，库不会再访问它。**队列满不会断开连接**——
这是刻意的（UDP 下"丢弃"比"断链"更合适）。

---

## 三、服务端：`DatagramReceiver`

### 3.1 最小可用示例

```cpp
class MyHandler : public SessionHandler {
public:
    bool on_session_open(std::shared_ptr<IDatagramSession> s) override {
        arm_read(s);          // 开始收
        return true;          // 返回 false 即拒绝（限流/白名单）
    }
    void on_session_close(std::shared_ptr<IDatagramSession>) override {}
    bool on_session_timeout(std::shared_ptr<IDatagramSession>) override {
        return true;          // true = 由 Receiver 回收
    }
private:
    void arm_read(std::shared_ptr<IDatagramSession> s) {
        auto buf = std::make_shared<std::array<char, 2048>>();
        s->async_receive(asio::buffer(buf->data(), buf->size()),
            [this, s, buf](error_code ec, size_t n) {
                if (ec) return;           // operation_aborted = 会话已关
                // 处理 n 字节...
                auto out = std::make_shared<std::string>(buf->data(), n);
                s->async_send(asio::buffer(*out),
                    [this, s, out](error_code, size_t) {
                        arm_read(s);      // 写完成后才续读
                    });
            });
    }
};

ReceiverConfig cfg;
cfg.local        = udp::endpoint(udp::v4(), 9000);
cfg.max_sessions = 256;
auto recv = make_datagram_receiver(pool, cfg, std::make_shared<MyHandler>(), ec);
```

### 3.2 `ReceiverConfig` 各项怎么调

| 参数 | 默认 | 怎么调 |
|---|---|---|
| `max_sessions` | 1024 | **必须设**。UDP 源地址可伪造，不设上限会被打爆内存。按真实并发数 × 2~3 |
| `idle_timeout` | 30s | 业务多久没有交互算"死会话"。0 = 不自动回收（需业务自己 close） |
| `idle_check_interval` | 1s | 回收的颗粒度。太小浪费 CPU，太大回收不及时 |
| `session_send_queue_bytes/count` | 2MB / 256 | 每个会话的背压阈值。高吞吐调大，内存紧张调小 |
| `socket_buf_size` | 256KB | 系统收发缓冲。高吞吐场景可调大（如 1MB） |

### 3.3 会话的生命周期

```
第一个数据报到达
    ↓
on_session_open()  ← 返回 false 则拒绝，不建会话
    ↓
[业务收发]  ← 每收到数据报刷新 last_activity
    ↓
以下任一：主动 close() / 空闲超时 / 保活超时 / Receiver 关闭
    ↓
on_session_close()  ← 保证与 open 成对，每个会话恰好一次
```

**注意**：UDP 没有"断开"信号。对端崩溃时你不会收到任何通知——
这正是 `idle_timeout` 和保活存在的意义。

### 3.4 保活：判断对端还活着的唯一手段

```cpp
bool on_session_open(std::shared_ptr<IDatagramSession> s) override {
    // 10 秒没有任何数据报 → 判定对端失联，回调 on_session_timeout
    s->enable_recv_keepalive(std::chrono::seconds(10), 3);
    return true;
}
```

> **注意**：保活超时默认会关闭会话。如果你希望"超时但保留"，
> 在 `on_session_timeout` 里 `return false`。

---

## 四、客户端：`DatagramChannel`

```cpp
// 单对端（推荐）：socket 会 connect，发送不用给地址
const udp::endpoint peer{make_address("10.0.0.1"), 9000};
auto ch = make_datagram_channel(pool, cfg, &peer, udp::endpoint(udp::v4(), 0), ec);

ch->async_send(asio::buffer(*data), udp::endpoint() /*被忽略*/, cb);
ch->async_receive(asio::buffer(*buf), cb);
```

**单对端的好处**：内核会过滤掉非该对端的数据报，更安全。

**何时用多对端**：你需要向任意地址发送（如向多个设备广播）。

---

## 五、常见陷阱

| 陷阱 | 后果 | 正确做法 |
|---|---|---|
| buffer 用临时对象 | 悬垂读取 / 堆损坏 | `shared_ptr` 持有并让回调捕获 |
| 连续调多次 `async_receive` | 第二次返回 false，数据被前一笔"吃掉" | 在回调里续挂下一笔 |
| 没设 `max_sessions` | 被伪造源地址打爆内存 | 必设 |
| 没有 `idle_timeout` 和保活 | 对端崩溃后会话永久残留 | 至少设其一 |
| 以为 `async_send` 成功 = 对方收到 | 误判送达 | UDP 无送达保证，业务需自证（如应用层 ACK） |
| 用 `DatagramChannel` 做服务端 | 只能收单对端 | 用 `DatagramReceiver` |

---

## 六、性能与调优

| 场景 | 建议 |
|---|---|
| 高包速率（如视频流） | 调大 `socket_buf_size`（1MB+）；提高 `max_sessions`；`io_threads` 设为核数 |
| 低延迟控制命令 | `socket_send_buf` 不必大；减少 `idle_check_interval` 无关；优先保证不丢包 |
| 嵌入式（内存受限） | 调小 `session_send_queue_bytes`，`io_threads=1~2`，`max_sessions` 按实际需要 |

**理解背压**：发送队列满时 `async_send` 返回 false。这不是错误——是库在告诉你
"发得太快了"。正确响应是**丢弃或等待**，而不是重试到爆内存。

---

## 七、完整可运行示例

`example/udp_echo_main.cpp`（构建产物 `netmech_udp.exe`）：

```powershell
netmech_udp                    # 一体化演示：server + 2 个 client
netmech_udp server 9000        # 只跑 server
netmech_udp client 9000 5      # 跑 client，发 5 轮
```

该示例演示了本节所有要点的**正确写法**，可作为新业务的起点。

---

*本文档随 P4 产出。若发现与实现不符，以实现与单元测试为准并反馈。*
