# NetMechanism 类图（现状还原）

本目录是由源码反向导出的**结构基线**，用于在正式走「需求分析 → 概要设计 → 详细设计」流程前，先把"现在到底长什么样"固化下来。图上内容全部来自源码，不含设计意图推断（推断部分另见各图 note）。

## 产物清单

| 文件 | 内容 | 尺寸 |
|---|---|---|
| `00_overview.puml/.png/.svg` | 分层总览：L0 调用方 / L1 公共契约 / L2 内部实现 / L3 基础设施 | 2236×834 |
| `01_tcp_connection.puml/.png/.svg` | TCP 子系统：Mechanism/Endpoint 门面 + Acceptor/ConnectOp + 读写队列 + 双向保活 | 1611×1714 |
| `02_datagram.puml/.png/.svg` | UDP 子系统：无连接 `UdpChannel` 与会话化 `DatagramReceiver`/`DatagramSession` 两套 API | 2057×1392 |
| `03_support.puml/.png/.svg` | 基础设施：线程池、endpoint id 分配、error_category、编译期协议探测 | 1437×945 |
| `raw/netmech_all_raw.puml` | hpp2plantuml 原样输出，**未做任何修正**，仅用于比对工具行为 | — |

为什么不出一张全量图：34 个类 + 34 条关系的单图实测宽 9327px，PlantUML 布局会把类横向铺开成一条"带子"，无法阅读；kroki 的 PNG 还会在 4096px 处裁切。按子系统切分后每张都在 2200px 内。

## 重新生成

```bash
# 1. 解析源码，得到工具原始输出（自动处理 BOM）
python tools/uml/export_class_diagram.py \
    --src include/netmech --src src --deps \
    -o docs/design/raw/netmech_all_raw.puml

# 2. 校正后渲染（本地无 java 时走 kroki）
curl -s -X POST -H "Content-Type: text/plain" \
     --data-binary @docs/design/01_tcp_connection.puml \
     https://kroki.io/plantuml/svg -o docs/design/01_tcp_connection.svg
```

本地装 VS Code 的 `PlantUML` 插件可直接预览 `.puml` 并导出任意分辨率，不必联网。

## hpp2plantuml 实测缺陷（0.8.6 + robotpy-cppheaderparser 5.1.2）

这些都是这次导出实际踩到的，`rtpmech` 会一模一样地复现：

1. **UTF-8 BOM 直接解析失败**。所有头文件带 BOM 时，CppHeaderParser 把 `\ufeff` 当成非法 token，报 `CppParseError` 且 `Error around:` 后面是空的，看不出是 BOM 问题。处理方式见 `tools/uml/export_class_diagram.py`（去 BOM 镜像，不动源文件）。
2. **相邻类被误判为嵌套**。上一类的 `};` 之后隔几个空行出现的新类，会被挂到前一个类名下。本次误判 3 处：
   - `ReceiverConfig::IDatagramReceiver` → 实际是 `netmech::IDatagramReceiver`
   - `KeepAliveParams::SendKeepAliveParams` → 实际是 `netmech::SendKeepAliveParams`
   - `RecvKeepAlive::SendKeepAlive` → 实际是 `netmech::SendKeepAlive`

   真嵌套只有 `Endpoint::Impl`、`ReadQueue::Request`、`WriteQueue::Request` 三处。
3. **模板特化产生垃圾类**。生成 6 个无法读的类名，例如 `class is_error_code_enumnetmech::NetErrc`（源自 `std::is_error_code_enum<NetErrc>` 特化）、`class is_stream_protocolProtocol,std::void_ttypenameProtocol::socket`。已全部剔除，只保留可读的 `protocol_traits`。
4. **只扫头文件**。`.cpp` 内定义的 6 个实现类全部缺失：`UdpChannel`、`DatagramReceiver`、`DatagramSession`、`Mechanism::Impl`、`NetCategoryImpl`、`NetBoostCategoryImpl`。其中前三个是实现接口的关键类，缺了类图就看不出"接口 → 实现"的落点，已人工补齐。
5. **接口识别不出**。`Linkable` / `IoHandler` / `SessionHandler` / `IDatagram*` 的析构与成员函数都是虚函数但非纯虚，工具一律输出成普通 `class`；图中已按设计意图标注 `<<interface>>`。
6. 成员名以 `_` 结尾会刷 `WARN unresolved`（本次 5 条），属噪声，可忽略。

## 类清单（34 个）

**L1 公共契约（include/netmech，13 头）**

| 类 | 类型 | 职责 |
|---|---|---|
| `Mechanism` | 门面 | 创建 listen/connect，持有连接表与线程池 |
| `Mechanism::Config` | struct | 套接字与发送队列参数 |
| `Endpoint` | 门面 | 单连接读写、双向保活、优雅关闭 |
| `Linkable` | 接口 | 连接事件回调（up/accept/down/fail） |
| `IoHandler` | 接口 | 单连接协议处理器，由 Linkable 产出 |
| `IDatagramChannel` | 接口 | 无连接 UDP：单 socket + 默认对端 |
| `IDatagramSession` | 接口 | 会话化 UDP：按 peer 多路分解 |
| `IDatagramReceiver` | 接口 | 会话容器 + 工厂函数 |
| `SessionHandler` | 接口 | 会话 open/timeout/close 事件 |
| `ReceiverConfig` / `DatagramConfig` | struct | UDP 两套 API 的配置 |
| `KeepAliveParams` / `SendKeepAliveParams` | struct | 接收/发送保活参数 |
| `NetErrc` / `DatagramErrc` | enum | 错误码（映射 std::error_code） |
| `noncopyable` | 基类 | 禁拷贝，protected 析构 |
| `protocol_traits` | trait | 编译期探测 socket/acceptor 是否存在 |

**L2 内部实现（src）**

| 类 | 职责 |
|---|---|
| `Mechanism::Impl` | 连接表（id→Endpoint）、acceptor 集合、在途 connect |
| `Endpoint::Impl` | 单连接全部 IO 状态：socket + 读队列 + 写队列 + 双向保活 |
| `Acceptor` | accept 循环，失败按 backoff 退避重开 |
| `ConnectOp` | 一次在途 connect（socket + 超时 timer） |
| `ConnEntry` | 连接表项，强引用 Endpoint |
| `ReadQueue` / `WriteQueue` | 读状态（1 在途槽位）/ 写 FIFO（1 在途），显式状态机 |
| `RecvKeepAlive` / `SendKeepAlive` | 双向保活状态机，generation 防陈旧回调 |
| `IoPool` | 线程池，**1 io_context = 1 线程**，故内部不用 strand |
| `detail::IdRegistry` | endpoint id 全进程唯一、单调递增、不复用 |
| `UdpChannel` | `IDatagramChannel` 唯一实现 |
| `SendQueue` / `PendingRecv` | 通道发送 FIFO / 接收在途槽 |
| `DatagramReceiver` | `IDatagramReceiver` 唯一实现，首包建会话 |
| `ReceiverCore` | 接收器共享状态：socket + 两张会话索引表 |
| `DatagramSession` | `IDatagramSession` 唯一实现 |
| `SessionCore` | 会话共享状态，与 Receiver 共享 socket |
| `SessionSendQueue` / `SessionKeepAlive` | 会话级发送队列 / 单侧保活 |
| `NetCategoryImpl` / `NetBoostCategoryImpl` | `std::error_category` 实现（std 与 asio 错误码两套） |

## 结构上的几点观察（供概要设计阶段处理）

- **门面层干净**：`Mechanism`/`Endpoint` 只有 `shared_ptr<Impl>`，公共头无实现细节泄漏；`Impl` 前向声明在 class 内，是标准 Pimpl。
- **两套 UDP API 并存**：`IDatagramChannel`（无会话）与 `IDatagramReceiver`+`IDatagramSession`（有会话）职责重叠度高，各自带一套 `*Config`、`*SendQueue`、`*KeepAlive`。这是当前类数偏多的主因，概要设计时应明确二者取舍。
- **契约与实现 1:1，但创建入口分散且风格不一**：`make_datagram_channel`（udp_channel.cpp，`error_code&` 出参）、`make_datagram_receiver`（datagram_receiver.cpp，`error_code&` 出参）、`MakeEndpointObject` + `install_endpoint_factory`（endpoint.cpp，全局函数指针注入，还要由 mechanism.cpp 回填注册）、以及 `DatagramReceiver` 内部再直接 `make_shared`。四种创建方式并存，详细设计阶段应统一 factory 约定。
- **无显式的连接/会话状态枚举**：`Endpoint::Impl` 用 4 个独立 `atomic<bool>`（`closing` / `closed` / `unregistered` / `has_pending_read`）表达连接生命周期，`RecvKeepAlive`/`SendKeepAlive` 再各自带一个 `progressed`。这属于详细设计要补的"状态机规格"——当前代码评审时最难验证的就是这几个标志的合法组合。
