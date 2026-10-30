# NetMechanism

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey.svg)](#构建)

> 基于 **C++17 + 独立 Asio** 的 TCP / UDP 通信库。
> **零第三方依赖**：Asio 随仓库提供，`git clone` 即可构建，产物不链接任何第三方 DLL。

---

## 特性

| | |
|---|---|
| **零依赖** | 独立 Asio（header-only）随仓库提供，不需要 Boost，不需要 vcpkg |
| **一个对象一条连接** | TCP 是 `Conn`，UDP 是 `UdpEndpoint`，没有隐藏的"会话表"要维护 |
| **回调天然串行** | 每条连接绑定一个 `io_context`，**1 context = 1 线程**，同一连接的回调不会并发 |
| **反压语义明确** | `write()` 返回 `false` 表示"没受理"，不是出错，也**不会断链** |
| **失败可归因** | 每笔操作的完成回调恰好一次，带缓冲区原样归还；不会静默丢弃 |
| **双平台验证** | MSVC 14.44 与 GCC 15 下均 46 用例 / 604 断言全绿，零警告 |

---

## 30 秒上手

一个能跑的 echo 服务端 + 客户端。完整版见 `example/echo_main.cpp`。

```cpp
#include <netmech/net.hpp>

#include <array>
#include <cstdio>

using namespace netmech;

// 1) 每条连接一个 handler：读写完成都回调到它
class Echo : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override {
        m_ep = ep;
        return ep->read({nullptr, m_rx.data(), m_rx.size()});   // 必须有读在途
    }
    void on_read(std::error_code ec, Buf buf, std::size_t n) override {
        if (ec) return;                                  // 断开 / 保活超时
        m_ep->write({nullptr, buf.data, n});             // 原样回写
    }
    void on_write(std::error_code ec, ConstBuf, std::size_t) override {
        if (!ec) m_ep->read({nullptr, m_rx.data(), m_rx.size()});   // 写完再读
    }
    void on_close() override { m_ep.reset(); }
private:
    std::shared_ptr<Conn>          m_ep;
    std::array<std::uint8_t, 4096> m_rx{};
};

// 2) Linkable 决定"新连接交给谁"
class Server : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_accept(Conn&) override {
        return std::make_shared<Echo>();
    }
};

int main() {
    if (!initialize(4)) return 1;          // 进程级 io 线程数，必须最先调用

    auto mech = Mechanism::create();
    auto srv  = std::make_shared<Server>();

    if (auto ec = mech->listen(Address::any(9000), std::weak_ptr<Linkable>(srv))) {
        std::fprintf(stderr, "listen: %s\n", ec.message().c_str());
        return 1;
    }
    std::printf("listening on 9000\n");
    std::getchar();                        // 按回车退出

    mech->close_all();
    return 0;
}
```

UDP 更简单——**没有连接概念**，一个 `UdpEndpoint` 就是一个本地端口：

```cpp
struct Echo : UdpHandler {
    void on_read(UdpEndpoint& ep, std::error_code ec, Buf buf, std::size_t n,
                 const Address& from) override {
        if (!ec) ep.send_to(from, {nullptr, buf.data, n});   // 用 from 回包
    }
};

UdpConfig cfg;
cfg.bind = Address::any(9001);
std::error_code ec;
auto ep = create_udp_endpoint(cfg, std::make_shared<Echo>(), nullptr, ec);
```

---

## 三个必须知道的约束

### ① 必须保持一笔读在途

TCP 的 FIN **只有在下一次 `read` 时才会被感知**。如果 `on_open` 里不发起读、之后也不再读，
对端关闭时你收不到任何通知，连接会静默挂着。

```cpp
bool on_open(std::shared_ptr<Conn> ep) override {
    return ep->read({nullptr, m_rx.data(), m_rx.size()});   // ← 别省这一行
}
```

### ② `write()` 返回 `false` 不是错误

| 返回值 | 含义 | 后续 |
|---|---|---|
| `true` | 已受理，**必定**回调一次 `on_write` | 等回调 |
| `false` | **未受理**，不会回调，缓冲立即归还 | 原因有三种：连接已关 / 参数非法 / 发送队列满 |

第三种是**反压**——正确反应是等下一个 `on_write` 之后再试，而不是放弃或断链。
`Conn::set_send_queue_limit()` 可调阈值（默认 2 MiB / 256 笔）。

### ③ `close()` 的行为由 `Config::linger_zero` 决定

| 取值 | 关闭方式 | 适用 |
|---|---|---|
| `false`（默认） | 正常四次挥手，已入队数据保证送达 | 请求-应答协议（Modbus 等） |
| `true` | `SO_LINGER{1,0}` 立即 RST，丢弃未发数据 | 高频短连接，避免 TIME_WAIT 堆积 |

> 注意：`linger_zero = false` 时主动关闭方会进入 TIME_WAIT（约 2 分钟）。
> 如果连接建立速率超过 **~136 次/秒**，Windows 的 16384 个动态端口会被耗尽。

---

## 构建

### Windows（MSVC）

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### Linux / WSL

```bash
sudo apt install -y build-essential cmake     # 一次性
./build_linux.sh                              # 配置 + 编译 + ctest
```

要求：CMake ≥ 3.16、支持 C++17 的编译器（MSVC 2019+ / GCC 9+）。

### aarch64 交叉编译

工具链用 Arm GNU Toolchain（`aarch64-none-linux-gnu-`，10.2-2020.11 验证过）：

```bash
cmake -S . -B build-aarch64 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-aarch64 -j$(nproc)
```

工具链装在别处时用 `-DTOOLCHAIN_ROOT=/path/to/gcc-arm-...` 覆盖。

产物是标准 aarch64 ELF，**最低 glibc 2.17**。目标板是精简系统（缺 libstdc++.so）时静态链接：

```bash
cmake -S . -B build-aarch64 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_EXE_LINKER_FLAGS="-static -static-libstdc++ -static-libgcc"
```

#### 在 x86 上用 qemu 验证产物

没板子时可以用 qemu 的 user-mode 模拟先跑一遍：

```bash
sudo apt install -y qemu-user          # 注意包名是 qemu-user（新版已含 static 二进制）
export QEMU_LD_PREFIX=/opt/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc
./build-aarch64/bin/netmech_ut         # binfmt 会自动调起 qemu
```

`QEMU_LD_PREFIX` 必须指向工具链自带的 sysroot，否则会报
`Could not open '/lib/ld-linux-aarch64.so.1'`。

> ⚠️ qemu 只算"准真机"：它能验出逻辑错误和依赖缺失，但**验不出**真实硬件的
> 时序差异（网卡中断合并、CPU 调度、缓存效应）。最终仍要在板子上跑一遍。

### 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `NETMECH_BUILD_TESTS` | ON | 编译单元测试 |
| `NETMECH_BUILD_EXAMPLES` | ON | 编译 `echo` / `udp_echo` 示例 |
| `NETMECH_BUILD_BENCH` | OFF | 编译 `bench/` 下的压测与长跑工具 |
| `NETMECH_WARNINGS_AS_ERRORS` | OFF | 把警告当错误（CI 用） |

---

## 目录结构

```
include/netmech/    公共头（7 个）
  common.hpp          initialize/shutdown、Address、Buf/ConstBuf、NetErrc
  conn.hpp            Conn（TCP 连接）、ReadMode、IdleInfo
  iohandler.hpp       ConnHandler（单连接协议处理器）
  linkable.hpp        Linkable（新连接交给谁）
  mechanism.hpp       Mechanism（运行时：listen / connect / 按 id 操作）
  udp.hpp             UdpEndpoint / UdpHandler / UdpConfig
                      （含端点级定时回调 set_timer 与端口探测）
  net.hpp             汇总头，业务代码只需要包含它

src/                实现（8 个 .cpp + 内部头）
test/               单元测试（46 用例 / 604 断言）
bench/              stress（并发/吞吐）、soak（长跑）、bench_strand（开销基准）
example/            TCP 与 UDP 的 echo 示例
docs/               设计文档与 API 说明
```

---

## 文档

| 文档 | 内容 |
|---|---|
| [`docs/API.md`](docs/API.md) | 逐个接口的语义、返回值、失败原因 |
| [`docs/design/overview-design.md`](docs/design/overview-design.md) | 设计取舍、线程模型、压测数据、实施记录 |

---

## 平台支持

| 平台 | 编译器 | 状态 |
|---|---|---|
| Windows 10/11 x64 | MSVC 2019 / 2022 | ✅ 已验证 |
| Linux x86_64 | GCC 15（WSL2 Ubuntu） | ✅ 已验证 |
| Linux aarch64 | Arm GNU 10.2（交叉） | ✅ 交叉编译通过（运行时待板子验证） |

---

## 致谢

本项目的实现与文档在 **DeepSeek-V4.1** 的协助下完成 —— 架构决策与验收由作者负责，
详见 [AUTHORS.md](AUTHORS.md)。

---

## License

MIT，见 [LICENSE](LICENSE)。
