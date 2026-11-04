# 发布形态评估：让下游库能独立发布

> 状态：**评估稿，未实施**
> 范围：netmech / modbusmech / rtspmech / rtpmech / httpmech / mpegmech
> 问题：除 netmech、mpegmech 外，其余四个都依赖 netmech。发布时想做到"拿一个库就能用"。

---

## 0. 结论（先看这段）

| 问题 | 结论 |
|---|---|
| 要不要改成动态库？ | **不建议**。见 §3 方案 C：netmech 有进程级 IoPool，各 .so 各带一份会变成多组 io 线程；换成 .so 还会引入 ABI、跨 DLL 分配/异常的坑。 |
| 能不能做到"发布时不依赖别的库"？ | **能，而且已有先例**：modbusmech 的公共头里 `netmech::` 出现 **0 次**，内部自己建 `Mechanism`。照抄它就行。 |
| 代价多大？ | 三个库要动公共 API，集中在 **4 个头文件**（rtspmech 1 个、rtpmech 2 个、httpmech 3 个）。最大的改动在 rtspmech：它的 `RtspSession` **继承了** `netmech::ConnHandler`。 |
| 建议路线 | 两步：**先**把 netmech 当"打包依赖"给出（1 天，零 API 改动）；**再**按 modbusmech 模式逐个去 netmech（每库 1–3 天）。 |

---

## 1. 现状盘点

### 1.1 公共头对 netmech 的暴露程度

| 库 | 对外头文件数 | 含 `netmech::` 的头 | 出现次数 | 形态 |
|---|---|---|---|---|
| netmech | 9 | — | — | 基座本身 |
| **modbusmech** | 5 | **0 个** | **0** | ✅ 已经是目标形态 |
| rtspmech | 6 | 1（`session.hpp`） | 9 | ❌ 继承 `netmech::ConnHandler` |
| rtpmech | 9 | 2（`session.hpp`、`rtp_packet.hpp`） | 16 | ⚠️ `netmech::Address` + 内部持 `UdpEndpoint` |
| httpmech | 10 | 3（`server.hpp`、`client.hpp`、`websocket.hpp`） | 9 | ⚠️ `create(mech)` 显式注入 + `Address` |
| mpegmech | — | 0 | 0 | ✅ 与本议题无关 |

### 1.2 泄漏点归类

一共只有**三类**，没有第四类：

| 类型 | 出现位置 | 改造成本 |
|---|---|---|
| ① `netmech::Address` 值类型 | httpmech **5 处**（config / `local()` / `remote()`）、rtpmech **8 处**（config / handler 参数） | 低：需要一个自己的 `Endpoint{host,port}`，纯值语义，无行为 |
| ② `create(std::shared_ptr<netmech::Mechanism>)` 依赖注入 | httpmech server/client/ws-client | 低：改成内部自建 + 保留一个可选注入重载 |
| ③ **继承** `netmech::ConnHandler` | rtspmech `RtspSession`（该库`netmech::Address` 是 **0** 处，问题只有这一条） | **高**：公共类型继承外部接口，等于把 netmech 的 ABI 焊进自己的类型布局 |

### 1.3 modbusmech 是怎么做到的（现成模板）

```cpp
// modbusmech/src/tcp.cpp（内部，不在公共头里）
self->m_mech = netmech::Mechanism::create();

// include/modbusmech/master.hpp：没有任何 netmech 字样
```

对外只给 `Config` / `Master` / `Slave` / `Pdu`，连接细节全在 `Impl` 里。
**这就是"独立发布"的样板**，其余三个库照这个形状改即可。

---

## 2. 三个必须先知道的硬事实

### ① netmech 有**进程级** IoPool 单例

```
netmech::initialize(n)  → 只登记线程数，不建池
IoPool::global()        → 首次用到时才建，线程数 = n ? n : hardware_concurrency()
```

推论（这几条决定了方案选择）：

- **静态库**（4 个库 + netmech 一起链进同一个可执行文件）：链接器只保留**一份** netmech 目标文件 → **一个池**，线程数由"第一次 initialize 或默认值"决定。✅
- **动态库**（每个 .so 静态内嵌 netmech）：每个 .so 一份 `g_requested` / `IoPool` 静态量 → **N 个池、N 组线程**。通讯管理机上 4 个库 = 4 × 16 线程（默认 hw_concurrency），纯浪费。❌
- 因此：**"一个库一个 .so、各自内嵌 netmech"这条路在资源上是错的**；要做 .so，就得共享一个 `netmech.so`。

### ② `initialize()` 不是必需的

不调也能跑（懒建池，默认 `hardware_concurrency`）。
好处：库可以完全自包含（modbusmech 就没调）。
坏处：**默认值偏大**（8 核机器 16 线程），嵌入式上应该由应用显式给个 2–4。

→ 建议：库内部懒初始化时用 `initialize(0)` 的语义，但**在文档里明确写"想控线程数就自己先调一次"**；或者给库加一个 `set_io_threads()` 的转发。

### ③ `netmech::Address` 是纯值类型

`{ip, port, scope}`，构造/比较/`to_string()` 都是值语义，**没有 asio、没有隐藏状态**。
所以"复制一个自己的 Endpoint"成本极低，**不需要**为了它做 pimpl。

### 2.4 第三方 vendored 代码怎么处理（已定）

httpmech 和 rtspmech 都 vendored 了 **picohttpparser**（MIT）。原先它是独立的
`libpicohttpparser.a`，于是使用方要写：

```bash
g++ app.cpp -lhttpmech -lpicohttpparser     # ← 库内部用的东西，凭什么让使用方买单
```

库里用、库里就该收干净。**最终形态（已实施）**：

| 做法 | 说明 |
|---|---|
| 编进同一个 target | `src/pico_impl.cpp` 里 `#include "../thirdparty/picohttpparser/picohttpparser.c"` 以 C++ 方式编入（pico 是干净的 C，上游也建议可 C++ 编译） |
| 符号加前缀 | `src/pico_rename.hpp` 把 5 个全局 C 符号改成 `httpmech_phr_*` / `rtspmech_phr_*`——否则两个库同时链接必然 `multiple definition` |
| 头不外露 | pico 的 include 目录改成 **PRIVATE**，不再装进 `include/<pkg>/thirdparty/` |
| 许可证照发 | MIT 要求分发二进制时保留声明，所以 `LICENSE.picohttpparser` 随 `share/doc/<pkg>/` 安装 |

结果：`<prefix>/lib/` 下**只有一个 .a**，`<prefix>/include/<pkg>/` 下只有本库的头。

> 验证手法：用 `-Wl,--whole-archive` 把 httpmech 和 rtspmech 两个 .a 的全部
> 目标文件强拉进同一个可执行文件——不报重复符号，才算真的隔离干净。

这条同样适用于将来任何 vendored 代码（比如哪天要内嵌 zlib）。

---

## 3. 方案对比

| # | 方案 | 消费者要 netmech 头？ | 要链 netmech？ | 运行时池 | 改动量 | 评价 |
|---|---|---|---|---|---|---|
| **A** | 保持现状（发布时一起给 netmech） | 是 | 是 | 1 个 | 0 | 最省事，但"独立发布"没达成 |
| **B** | 静态库 + **打包 netmech**（bundle） | 是（但由 CMake 自动带） | 是（自动） | 1 个 | 小 | **过渡最优**：`find_package(rtpmech)` 后 netmech 自动就位，用户无感 |
| **C** | 动态库 + 各 .so 内嵌 netmech | 视头改造 | 否 | **N 个** ❌ | 中 | **不推荐**，见 §2① |
| **D** | 动态库 + 共享 `netmech.so` | 是 | 运行时是 | 1 个 | 中 | 需要 ABI 稳定诉求才值得；发布变成 5 个文件 |
| **E** | 静态库 + 公共头去 netmech（modbusmech 模式） | **否** | 否 | 1 个 | 中大 | ✅ **"独立发布"的正解** |
| **F** | E + 门面 API（opaque handle，彻底不暴露类型） | 否 | 否 | 1 个 | 大 | 过度设计：本项目的库都是"自己的应用在用"，不需要 ABI 稳定 |

**方案 E 与 B 的区别**：B 是"我给你 netmech，你不用管"；E 是"我根本不让你看见 netmech"。
E 才能满足"发布时不依赖其他的库"这句话的字面意思。

---

## 4. 推荐路线（两步走）

### 第一步：方案 B —— 打包依赖（1 天，零 API 改动）

每个库的 CMake 加一段"bundle"逻辑：

```cmake
# 安装时把 netmech 的静态库 + 头一起放进本包的私有目录
install(TARGETS netmech EXPORT rtpmechBundled ARCHIVE DESTINATION lib/rtpmech/private)
install(DIRECTORY ${NETMECH_DIR}/include/netmech DESTINATION include/rtpmech/private)

# 导出的 target 带上私有的 include 与 lib
target_include_directories(rtpmech INTERFACE
    $<INSTALL_INTERFACE:include/rtpmech/private>)
```

消费者仍然是 `find_package(rtpmech)` + `target_link_libraries(app PRIVATE rtpmech::rtpmech)`，
但**不用**再自己找 netmech。

- 适合：现在就要发版、且用户是 CMake 工程。
- 局限：头文件里还是会出现 `netmech::Address`（用户翻头文件会看到），"看起来还是依赖"。

> **变体 B'：合并归档** —— 用 `ar -M` 把 `libnetmech.a` 并进 `librtpmech.a`，
> 消费者只链一个 `.a`。听着很美，但有个坑：**两个合并库同时使用时容易撞
> `multiple definition`**（应用既用 rtpmech 又用 modbusmech，两份归档里各有一份
> netmech 目标文件，链接器可能从两处都取）。所以它只适合"一次只用一个库"的场景，
> 不能作为最终形态。

### 第二步：方案 E —— 公共头去 netmech（按库推进）

| 库 | 要做的事 | 工作量 |
|---|---|---|
| **modbusmech** | 无（已是目标形态）。只需补文档：怎么控 io 线程数 | 0.5 天（文档） |
| **httpmech** | ① 新增 `httpmech::Endpoint{host,port}` 替代 `netmech::Address`（**5 处**）；② `create(mech, …)` 改成 `create(cfg, …)` 内部自建，另留 `create_with(mech, …)` 给需要共享池的场景；③ `WebSocketSession::remote()` 返回自己的 Endpoint | 1–2 天 |
| **rtpmech** | ① `Endpoint` 替换 `Address`（config + handler 参数，**8 处**）；② `session.hpp` 不再引 `<netmech/udp.hpp>`——`UdpEndpoint` 已在 Impl 里（改成前向声明 + 组合）；③ `rtp_packet.hpp` 现在整份引 `<netmech/net.hpp>`（注释说"仅用地址类型"），换成自己的 `Endpoint` 后这行可以整条删掉 | 1–2 天 |
| **rtspmech** | `RtspSession : public netmech::ConnHandler` 是硬耦合，两条路：<br>**(a)** 保留继承，但把 `ConnHandler` 的定义搬到自己头里（不现实，等于复制接口）<br>**(b)** 改成**组合 + 内部适配器**：`RtspSession` 不再继承，netmech 的 handler 变成 `detail::ConnAdapter`，对外只暴露 `RtspSession` 自己的 `send_*` / `on_*` | **2–3 天**，需要改调用方（`on_accept` 里返回的对象变了） |

改造顺序建议：**httpmech → rtpmech → rtspmech**（由易到难，前两个能给第三个探路）。

---

## 5. 改造后应该长什么样（以 httpmech 为例）

```cpp
// 现在
auto mech = netmech::Mechanism::create();
auto srv  = httpmech::HttpServer::create(mech, cfg, router);

// 改造后（默认：自己管自己）
auto srv = httpmech::HttpServer::create(cfg, router);

// 改造后（想共享池 / 想统一控制线程数）
auto srv = httpmech::HttpServer::create_with_mechanism(mech, cfg, router);
```

关键：**默认自建、可选注入**。这样"独立发布"和"同进程内共享一份 io 池"两个诉求不冲突。

---

## 6. 验收标准（改造完成的定义）

1. 装到一个干净前缀，`find include/<pkg> -name '*.hpp' | xargs grep -l netmech` **为空**；
2. 写一个只给该前缀的 `-I`、只链一个 `.a` 的消费者，编译 + 链接 + 运行通过（跟这次验证 netmech 去 asio 用的是同一套手法）；
3. CI 的 `quality` 加一条护栏：`include/` 下出现 `netmech::` 或 `#include <netmech/...>` 就红（参见 netmech 那条"不得引 asio"）。

---

## 7. 需要拍板的四件事

1. **发布形态**：只发静态库，还是要 `.so`？
   → 我的判断：静态库。你们的部署形态是"通信管理机上一个可执行文件"，`.so` 只会带来 §2① 的池复制问题。
2. **是否保留"可选注入"**（`create_with_mechanism`）？
   → 建议保留：应用要在一个进程里同时跑 modbus + http，共用一个池是有价值的。
3. **`Endpoint` 用什么形状**：`{std::string host, uint16_t port}`，还是继续用 `{uint32 ip, port}`（更省，但要自己写解析）？
   → 建议照抄 `netmech::Address` 的字段（uint32 + port + scope），实现直接搬。
4. **一族是否同版本发布**：五个仓各自 `0.x` 还是统一版本号？
   → 独立发布之后建议各自版本，但 `netmech` 的兼容性要写清楚（哪版改了 ABI）。

---

## 8. 不做的事（先说清，免得后面扯皮）

- **不做 pimpl 化整个 API**（方案 F）。这些库的使用者就是自己的应用，ABI 稳定没有收益，pimpl 只会让调试更难。
- **不做插件式后端**（"把 netmech 换成别的传输"）。没有第二个传输实现的需求，抽象出来是纯负担。
- **不做 TLS**。这跟发布形态无关，别混在一起谈。
