# 贡献指南

感谢参与。本文说明构建、验证与几条约定的硬性约束。

---

## 环境要求

| | 最低 | 推荐 |
|---|---|---|
| CMake | 3.16 | 3.20+ |
| C++ | C++17 | — |
| Windows 编译器 | MSVC 2019 | MSVC 2022 |
| Linux 编译器 | GCC 9 | GCC 12+ |

Asio 随仓库提供（`thirdparty/asio`），**不需要**安装 Boost 或任何包管理器。

---

## 构建与测试

### Windows（MSVC）

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### Linux / WSL

```bash
sudo apt install -y build-essential cmake      # 一次性
./build_linux.sh                                # 配置 + 编译 + ctest
```

### 常用选项

| 选项 | 默认 | 用途 |
|---|---|---|
| `NETMECH_BUILD_TESTS` | ON | 单元测试 |
| `NETMECH_BUILD_EXAMPLES` | ON | `echo` / `udp_echo` 示例 |
| `NETMECH_BUILD_BENCH` | OFF | `bench/` 下的压测与长跑工具 |
| `NETMECH_WARNINGS_AS_ERRORS` | OFF | CI 用，把警告当错误 |

---

## 提交前的检查清单

1. **两个平台都编译且零警告**
   ```bash
   # Windows
   cl /W4 /permissive- ...        # 或 cmake --build
   # Linux
   ./build_linux.sh
   ```
   `/W4`（MSVC）和 `-Wall -Wextra`（GCC）之外不加任何抑制。
   确实需要抑制的第三方头警告，用**定向** `#pragma` 并在旁边写清原因。

2. **单元测试全绿**（当前 46 用例 / 604 断言）

3. **改了公共接口 → 同步更新** `docs/API.md` 与 `README.md` 的示例

4. **改了行为契约 → 补一条测试**。尤其这几类，历史上都栽过：
   - 写回调恰好一次（不是零次、不是两次）
   - 关闭时在途操作要各自以错误码归还
   - 反压后能恢复（配额释放后重新受理）
   - 连接表在关闭后归零

---

## 代码约定

### 通用

- **注释用中文**，解释"为什么"而不是"是什么"。代码本身能说清的事不写注释。
- **不引入第三方依赖**。这是本库的核心卖点之一。
- 公共头**不暴露 asio 类型**（`common.hpp` 只引 `<asio/error.hpp>` 取错误码常量）。

### 命名

| 类别 | 风格 | 例 |
|---|---|---|
| 类型 | PascalCase | `Conn`、`UdpEndpoint` |
| 成员变量 | 有前缀 | 公共类 `m_xxx`；内部 `Impl` 直接 `xxx` |
| 常量 | `k` 前缀 | `kMaxWriteBatch` |
| 枚举 | PascalCase + 值小写 | `ReadMode::some` |

### 生命周期约定

- 库内对象**强自持**：投递到 io_context 的完成处理捕获自己的 `shared_ptr`，
  保证对象活到最后一个 IO 完成。**不要**改成捕获裸 `this`。
- 关闭流程必须**幂等**，且保证"已受理 ⇒ 恰好一次回调"。
- 回调里不得抛异常（库会吞掉，但那是兜底不是许可）。

### 错误码

- 一律用 `std::error_code`，不要返回 `int`。
- 新增 `NetErrc` 时**必须**同步 `net_errc_message()` 的 switch——
  GCC 的 `-Wswitch` 会报，MSVC 不会（容易漏）。

---

## 测试写法

测的是**契约**，不是实现细节。

```cpp
NETMECH_TEST(名字说明测什么)
{
    // 1. 摆好场景
    // 2. 触发
    // 3. 断言契约（而不是断言"我的实现恰好这样"）
}
```

- 断言里尽量带上下文（`CHECK_MSG(cond, "为什么这条会失败")`）。
- 异步场景用轮询等待（见 `test_support.hpp` 的 `wait_until`），
  **不要**用固定 `sleep` 赌时序。
- 测内部实现的东西（如 `protocol_traits`）不要放进公共测试。

---

## 性能相关

改发送/接收路径后，跑一下基准：

```bash
# Linux
g++ -std=c++17 -O2 -Iinclude -Ithirdparty/asio/include -pthread \
    src/*.cpp bench/stress.cpp -o /tmp/stress
/tmp/stress 100 20000 64 0 0 1      # 单向 sink 吞吐
/tmp/stress 100 1000 64 0 1         # pipeline（echo 往返）
```

基线（Linux / GCC 15，100 连接 × 20000 条 × 64 字节）：**~2.1M msg/s**。
明显低于这个数先怀疑回归。

> 注意：`bench/soak` 在高频短连接场景受 TIME_WAIT 端口预算限制
> （建连速率需 < ~136 次/秒），它已自动限速，别以为是被限速拖慢的。

---

## 提交信息

```
<范围>: <一句话说明>

<为什么这么改；如果修 bug，说明症状与根因>
```

例：

```
conn: 批量提交发送队列，减少小包场景的系统调用

每笔 write 一次 async_write，队列积压时系统调用开销吃掉吞吐。
改成队首最多 16 笔合成一个 buffer sequence，完成时按字节数逐笔归因，
回调次数不变。实测 pipeline 场景 3.7 倍提升。
```

---

## 报告问题

请附上：

1. 平台与编译器版本
2. 最小复现代码
3. 期望行为 vs 实际行为
4. 如果是崩溃/UAF，尽量给调用栈

**不要**贴整个项目的日志——定位不到。
