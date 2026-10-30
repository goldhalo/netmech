# NetMechanism 对外接口（草案）

评审材料，**尚未实现**。通过后按本目录结构移入 `include/netmech/`，并按第四节改造现有代码。

| 文件 | 内容 | 依赖 |
|---|---|---|
| [`netmech/common.hpp`](netmech/common.hpp) | `initialize` / `shutdown` / `Address` / `Buf` / `ConstBuf` / `NetErrc` | C++17 标准库 |
| [`netmech/tcp.hpp`](netmech/tcp.hpp) | `Mechanism` / `Conn` / `ConnHandler` / `Linkable` / `ConnectFuture` / `ReadMode` / `IdleInfo` | common.hpp |
| [`netmech/udp.hpp`](netmech/udp.hpp) | `UdpEndpoint` / `UdpHandler` / `UdpConfig` / `create_udp_endpoint` / `kMaxDatagramSize` | common.hpp |

验证方式：MSVC 19.44.35207 `/std:c++17 /W3 /utf-8`；派生 `ConnHandler` / `Linkable` / `UdpHandler` 并逐一 `override`；`common` + `tcp` + `udp` 三个头同时包含无重定义。全部通过。

---

# 一、TCP 侧决策

| # | 决定 | 理由 |
|---|---|---|
| 1 | `IoPool` 移出公共头，改内部单例，持有者用 `IoPool&` | 进程级、永不销毁，用 shared_ptr 只是多余的原子操作；同时切断 `Impl → IoPool → context → 队列 → Impl` 的强引用环 |
| 2 | `initialize()` 必须显式调用，未调用时 `create()` 返回 nullptr | 线程数是进程级一次性配置，不给隐式默认值，避免"改了却没生效" |
| 3 | `Endpoint` → `Conn` | 现状 `netmech::Endpoint` 与 `netmech::tcp::endpoint` 并存，读代码极易混 |
| 4 | `IoHandler` → `ConnHandler`，回调改为 `on_open/on_read/on_write/on_close/on_idle` | 去掉"用户传 lambda 捕获 this"的形态，`enable_shared_from_this` 和 `bind_weak` 一起消失 |
| 5 | 读/写对称，成功与失败共用同一个完成回调（靠 `ec` 区分） | 完成回调若不带错误码，就必须再拆出一对"归还"回调，使用方在每个回调里都要判断这次属于哪种情形。带上 `ec` 后一个回调覆盖两种，且"部分收到 + 断开"这种状态可以表达 |
| 6 | 服务端与客户端的关闭统一走 `on_linkdown` | 若按"主动连接 / 被动接入"拆成两个回调，使用方必须两处都实现，漏一个就收不到关闭通知。来源差异用 `Conn::user_context()` 就能区分 |
| 7 | `Linkable` 返回 `shared_ptr<ConnHandler>`，库强持有到 `on_close` | 出参 `T*&` 是 C 风格；传了智能指针又不持有等于白传。handler 由库管，业务对象用 `weak_ptr` |
| 8 | 新增 `Buf::owner`（可空） | owner 非空 = 库把引用带进完成处理，调用方提前析构也不悬垂/泄漏；为空 = 零开销老路径。**共享所有权不等于拷贝数据**，零拷贝没丢 |
| 9 | `Address` 替代底层 `tcp::endpoint` | 公共头不再传递包含 asio |
| 10 | `make_boost_error_code` → `make_error_code`，`from_boost_error_code` → `to_net_errc` | 本库用 standalone asio + `std::error_code`，与 Boost 无关 |
| 11 | 删除 `KeepAliveParams` / `SendKeepAliveParams` | `enable_*_keepalive()` 直接收裸参数，这两个 struct 全库无调用者 |
| 12 | `protocol_traits.hpp` 移入 `src/` | 编译期探测，对用户零价值 |
| 13 | 上下文挂 `Conn::user_context()`，不做成回调参数 | `connect` 是一对一、`listen` 是一对多；挂在 Conn 上两类回调签名得以统一，服务端连接天然继承监听器上下文 |
| 14 | 新增 `ConnectFuture`（异步 connect 保留） | 并发连接多台设备不能串行等，必须"先批量发起、再统一 wait_for"；future 就绪 = 握手完成 + handler 创建 + `on_open` 执行完 |
| 15 | `ConnectFuture::conn()` 返回 `shared_ptr<Conn>` | Conn 本身强自持，多持一份只延长已关闭对象的存活，不会悬垂 |
| 16 | 新增 `Mechanism::write(id, ConstBuf)` / `close(id)` | 业务侧往往只有设备表号，按 id 发可免掉自己维护 `id → Conn*` 表及摘除时的 race 窗口 |
| 17 | cancel 只覆盖"握手中的 connect" | 底层取消只能做到整条连接粒度，无法只撤销一笔 read；`read_exact` 半途取消还会丢已读数据 |
| 18 | 错误模型：未受理 vs 已受理出错，二分处理 | 前者不改连接状态，`buffer_full` 反压**不断链**；后者（ec != 0）立即断链并归还全部在途 op |
| 19 | write 的受理顺序由**发送队列锁**保证，不做纯 post | 若只在 io 线程入队，`write()` 返回 true 时尚未判断容量，超限请求只能被丢弃 → 违反"受理即恰好一次回调"。锁内完成"容量检查 + 入队"是唯一同时满足【同步返回是否受理】和【FIFO 顺序确定】的做法 |
| 20 | `on_idle` 改为"每次未通过都回调"，共 `max_miss` 次，最后一次之后才断链 | 只在超时那一刻回调一次的话，应用收到通知时连接马上就要断了，没有处置窗口。分级预警后前 `max_miss-1` 次可以发心跳、记日志或主动 `close()` |

---

# 二、UDP 侧决策

| # | 决定 | 理由 |
|---|---|---|
| U1 | **库不做会话管理**，不按 peer 分流，无 `find_session_by_peer` | 真实网络里源地址不可信：① SIP 协商端口与实际发送端口常常不一致（协商时只是挑了个当时可用的端口填进 SDP）；② 经 NAT/路由后看到的是运营商映射端口，与主机端口毫无对应关系。按地址建会话会让"一个呼叫拆成多个会话 / 同一路流被认成新流" |
| U2 | 源地址**零信任**：`from` 只用于回包和日志，不用于识别身份 | 会话标识（Call-ID / SSRC / 事务 ID）属于应用层。但**回包必须用 `from`**——NAT 之后这是唯一能到达对端的地址 |
| U3 | 一套 `UdpEndpoint` 覆盖两种拓扑，不拆两套 API | 一路一端口（RTP）与一端口多流（SIP）的差别全在应用层：前者固定目标 `send_to`，后者用 Call-ID 查自己的表 |
| U4 | 推送式交付：库自己持续收，收到就回调，用户不调 `read()` | UDP 一个报文就是一条完整消息，没有"读多少"的控制需求。也避免了 TCP 里"忘了续读就停收"这个陷阱 |
| U5 | 交付模式可配：`DeliverMode::borrow`（零分配，仅回调内有效）/ `own`（每包分配，可跨线程持有） | RTP 高频走 borrow 当场统计；SIP 低频走 own 可丢队列给业务线程 |
| U6 | 发送同步：`send_to` 返回 true 只表示已交给内核 | UDP 一次系统调用就完事，为它建异步队列只增加延迟和内存；实时流场景丢一帧好过积压一帧 |
| U7 | 库不做保活，只提供 `last_recv_time()` 观测 | UDP 没有链路，"对端还在不在"是应用层的事（SIP 用 OPTIONS、RTP 用 RTCP），库替应用判断只会误杀 |
| U8 | 无"错误即断链"：所有失败退化为单次操作失败 | 报文超长 → 截断 + `datagram_truncated`；发送失败 → 返回 false；只有 socket 本身不可恢复才 `on_close` 并终止 |
| U9 | 报文上限 `kMaxDatagramSize = 65507` | IPv4 载荷上限，同时兼容 IPv6（65527）；超过则拒绝发送 / 接收时截断 |
| U10 | 组播在 `UdpConfig` 里配置（组、网卡、TTL、回环） | 一个端点创建时加入、关闭时离开，不额外提供 join/leave 接口 |
| U11 | 提供 `last_peer()` 与 `on_peer_changed()` | 应用常需要“往实际发来数据的地方回包”，而这个地址只能从收到的包里得知：SDP 协商的端口常与对端实际发送端口不同，经 NAT 后更是映射端口。`last_peer()` 是拉取（回包路径用），`on_peer_changed()` 是事件（状态机/日志用）。二者都只是**观测**，不参与身份判定。思路参考 JRTPLIB 的 `OnNewSource`——它在发现新源时顺带把该源的实际 RTP/RTCP 地址交出来 |

---

# 三、待讨论 / 未决

1. **`on_linkdown` 的调用时机**（TCP）：草案定在 handler 释放之后，只给 `Conn&`。若希望在关闭时还能拿到 handler 做统计，需要调整顺序。
2. **跨线程 `write`**（TCP）：`Conn::write` / `Mechanism::write(id)` 标了"可从任意线程调用"，实现要 post 到该连接的 io 线程。这是现成能力，重构时不能丢。
3. **`cancel_connect` 的实现代价**（TCP）：低。`pending_connects` 已有摘表逻辑，`ConnectOp` 已有 `finished` 原子标志，补的是"cancel timer + close socket + set_value(cancelled)"约 30 行。
4. **`own` 模式的分配策略**（UDP）：每包 `make_shared` 还是接对象池？前者简单、后者省分配。若 SIP 只走 own、RTP 只走 borrow，可以先不做池。
5. **截断检测的平台差异**（UDP）：Windows 认 `WSAEMSGSIZE` / `asio::error::message_size`，Linux 靠 `MSG_TRUNC`。需要在实现层封一个函数。

---

# 四、实施时要动的文件

| 动作 | 文件 |
|---|---|
| 新建 | `include/netmech/common.hpp`、`tcp.hpp`、`udp.hpp`、`netmech.hpp`（汇总）、`src/io_pool.hpp`、`src/runtime.cpp` |
| 改名 | `endpoint.hpp` 内容并入 `tcp.hpp`；`net.hpp` → `netmech.hpp` |
| 重写 | `iohandler.hpp`（→ `ConnHandler`）、`linkable.hpp`、`mechanism.hpp` |
| 删除 | `keepalive.hpp`、`executor.hpp`（内容移 `src/io_pool.hpp`）、`protocol_traits.hpp`（移 `src/`） |
| 改内部 | `src/endpoint.cpp`、`mechanism.cpp`、`acceptor.cpp`、`io_pool.cpp`、`endpoint_state.hpp`：`shared_ptr<IoPool>` → `IoPool&`，`tcp::endpoint` → `Address`，读写回调改虚函数派发 |
| 大改 | `src/datagram_receiver.cpp`、`src/udp_channel.cpp`：合并为单端点模型，去掉会话表与 peer 索引 |
