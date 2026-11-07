# netmech 对外契约（契约测试索引）

本文件把公共头里"写明的承诺"集中列出，并指向**钉住它的测试**。
实现改动导致契约变化时，必须同时改这里和测试，否则 CI 会红。

> 原则：凡是 `include/netmech/*.hpp` 里写成"保证 / 恰好一次 / 不会 / 必定"
> 的句子，都应该在这里有一条。

## 1. 回调与线程模型

| # | 契约 | 测试 |
|---|---|---|
| 1.1 | 同一连接的所有回调在该连接所在的 io 线程上串行执行 | `test_conn_basic.cpp` |
| 1.2 | 不同连接可能在不同 io 线程上并发（跨连接共享状态需自行加锁） | `test_conn_*.cpp`、`test_lifecycle.cpp` |
| 1.3 | 回调内不得阻塞、不得抛异常（库内会兜底异常，不让它穿过 io 线程） | `test_conn_*` |

## 2. 连接生命周期（`ConnHandler`）

| # | 契约 | 测试 |
|---|---|---|
| 2.1 | `on_open` 返回 true ⇒ 关闭时 `on_close` **恰好一次** | `test_conn_basic.cpp`、`test_contracts.cpp` |
| 2.2 | `on_open` 返回 false ⇒ **不**调用 `on_close`；但 on_open 里**已受理**的 read/write 必须各回调一次（带错误） | `test_contracts.cpp` |
| 2.3 | 连接关闭：在途 read/write 以错误各归还一次，且先于 `on_close` | `test_contracts.cpp`、`test_conn_flow.cpp` |
| 2.4 | `on_linkdown` 只对"曾交给 Linkable 的连接"调用，且恰好一次 | `test_conn_basic.cpp` |
| 2.5 | `on_idle`：保活**每次**未通过都回调一次（不是只在超时时调一次） | `test_conn_flow.cpp` |

## 3. 读写与缓冲（`Conn`）

| # | 契约 | 测试 |
|---|---|---|
| 3.1 | `read` 同一时刻只允许一笔在途；第二笔返回 false | `test_conn_basic.cpp` |
| 3.2 | `write` 成功受理后 `buf` 内容不得修改、内存必须有效直到 `on_write` 返回 | `test_conn_write.cpp` |
| 3.3 | `write` 完成回调顺序与受理顺序严格一致（FIFO），出错时剩余请求也按序各回调一次 | `test_conn_write.cpp` |
| 3.4 | `buf.owner` 非空时库持有它直到完成回调（调用方可立即释放源数据） | `test_conn_write.cpp` |

## 4. 关闭与关停

| # | 契约 | 测试 |
|---|---|---|
| 4.1 | `Conn::close()` 幂等；`on_close` / `on_linkdown` 恰好一次 | `test_conn_basic.cpp` |
| 4.2 | `Mechanism::close_all()` 只断链、不销毁线程池；之后仍可 listen/connect | `test_mechanism_listeners.cpp` |
| 4.3 | `netmech::shutdown()` 后再 `listen()` / `connect()` 返回 `not_initialized`；在途回调不再执行 | `test_lifecycle.cpp` |
| 4.4 | `UdpEndpoint::close()` 触发 `on_close`；**仅** shared_ptr 析构不触发 | `test_udp_endpoint.cpp` |
| 4.5 | `SerialPort::close()` 幂等；在途读先以 `on_read(ec)` 兑现、后 `on_close` | `test_serial.cpp` |

## 5. 处理器生命周期（重要陷阱）

| # | 契约 | 测试 |
|---|---|---|
| 5.1 | 连接关闭后库仍保留 handler 强引用（retired 快照）直到 `Conn` 析构 | `test_contracts.cpp` |
| 5.2 | 因此 handler **不得**长期持有 `shared_ptr<Conn>`；若持有，必须在 `on_close()` 释放 | `test_conn_lifetime.cpp` |
| 5.3 | 违反 5.2 会成环泄漏，但 `endpoint_count()` 会正常归零（从计数看不出来） | 无（文档级说明） |

## 6. 运行时（`Runtime` / `initialize`）

| # | 契约 | 测试 |
|---|---|---|
| 6.1 | `Runtime::create(Options{io_threads})` 的线程数由创建者决定，之后不可更改 | `test_runtime.cpp` |
| 6.2 | `Runtime::shutdown()` 幂等；之后用它建的机制 listen/connect 返回 `not_initialized` | `test_runtime.cpp` |
| 6.3 | Mechanism/SerialPort/UdpEndpoint 持有注入 Runtime 的强引用（宿主先放手也安全） | `test_runtime.cpp` |
| 6.4 | 多 .so 进程必须让 netmech 以共享库形式被共用；跨静态副本注入会崩 | `test/multi_so/`（仅 SHARED） |
| 6.5 | 默认运行时退出兜底：用户没调 `shutdown()` 时，进程退出前 join 线程（不替代 shutdown） | 无（进程级，`/tmp` 手工验证） |

## 7. 其它

| # | 契约 | 测试 |
|---|---|---|
| 7.1 | `Address` 解析失败降级为通配地址（不抛异常） | `test_address.cpp` |
| 7.2 | `find_free_port_run` 只探测、不预留；返回后到 bind 之间可能被抢（TOCTOU 是预期行为） | `test_port_allocator.cpp` |
| 7.3 | 公共头不泄漏 Asio 类型；每个头可单独包含 | 质量检查 job |

## 维护约定

- 新增"保证"类措辞时，同 PR 必须补对应测试与本表条目；
- 本表条目被实现改动破坏时，CI 会红（对应测试失败），不要靠改文档"修复"。

## 8. 错误与异常语义（明确）

| # | 契约 | 依据 |
|---|---|---|
| 8.1 | **回调内不抛异常**；库内所有用户回调都经 `invoke_void` 包裹，异常不会穿过 io 线程 | `socket_ops`/`conn.cpp` 实现 + 契约测试 |
| 8.2 | 工厂函数失败**返回 nullptr + 填 `ec`**（`Mechanism`/`SerialPort`/`UdpEndpoint`/`WorkerPool`/`Runtime::create(opts,ec)`） | `test_runtime.cpp` 等 |
| 8.3 | 不提供 `ec` 的重载（`Runtime::create(opts)`、`Mechanism::create()`）在线程创建失败等**启动期致命错误**上 `std::terminate()` / 抛 `std::bad_alloc`；这类错误无法用 error_code 表达 | `runtime.cpp` 注释 |
| 8.4 | `NetErrc` 是**唯一**的错误源；`ec == NetErrc::xxx` 可直接比较（std::error_code 的 ADL 比较） | `common.hpp` |
| 8.5 | `make_boost_error_code()` / `from_boost_error_code()` 是**兼容层（legacy）**：仅为既有调用方保留，新代码请用 `std::error_code` + `NetErrc` | `common.hpp` |

> 结论：本库的错误处理是**两套 API 一个源头** —— `std::error_code` 是主接口，
> boost 兼容层只是历史包袱；不要在库内混合抛出异常来表达可恢复错误。
