// =============================================================================
//  netmech - 串口
// =============================================================================
//  文件    : netmech/serial.hpp
//  用途    : 打开一个串口，异步读写。接口与 `Conn` 同构（拉模式读 + 队列写）。
//  依赖    : Asio 的 `asio::serial_port`（内部已处理 Windows/Linux 的平台差异）
//
//  ★ 和 Conn 的关系
//  ---------------------------------------------------------------------------
//  两者都是"一条双向字节流"，所以接口刻意保持一致：
//      read(Buf)   —— 拉模式：自己给缓冲，完成时 on_read 回调
//      write(Buf)  —— 异步入队，完成时 on_write 回调
//      close()     —— 幂等
//
//  区别：串口没有地址（没有 local/remote），也没有 keepalive（串口没有"断"的概念，
//  线拔了只会读到错误，不会收到 FIN）。
//
//  ★ 线程模型
//  ---------------------------------------------------------------------------
//  与 netmech 其它部分一致：端口绑定在某个 `io_context` 上，回调在该 io 线程执行。
//  **同一串口的事件严格串行**，不需要加锁。不同串口可能在不同线程上并发。
//
//  ★ 数据边界
//  ---------------------------------------------------------------------------
//  串口是**纯字节流**，没有报文边界。`on_read` 返回的是"当前读到了多少"，
//  不是"一条完整的帧"。帧边界（比如 Modbus RTU 的 3.5 字符静默）由上层判断。
// =============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <system_error>

#include <netmech/common.hpp>

namespace netmech {

class SerialPort;

// =============================================================================
//  配置
// =============================================================================

/// 校验位。
enum class SerialParity {
    none,
    odd,
    even,
};

/// 停止位。
enum class SerialStopBits {
    one,
    two,
};

struct SerialConfig {
    /// 设备名：Linux 是 `"/dev/ttyS0"` / `"/dev/ttyUSB0"`；Windows 是 `"COM3"`。
    std::string device;

    /// 波特率。常见值：9600 / 19200 / 38400 / 57600 / 115200。
    ///
    /// @note 非标准波特率（如 250000）是否可用取决于驱动；Asio 会尝试设置，
    ///       失败时 `create` 返回错误。
    std::uint32_t baud_rate = 9600;

    /// 数据位，5~8。Modbus RTU 固定 8。
    std::uint8_t data_bits = 8;

    SerialParity   parity    = SerialParity::none;
    SerialStopBits stop_bits = SerialStopBits::one;

    /// 硬件流控（RTS/CTS）。
    ///
    /// @note RS-485 半双工**不要**开这个——方向由 RTS 控制，开了流控会打架。
    ///       USB 转 485 的转换器通常自动控制方向，保持 false 即可。
    bool flow_control = false;

    /// 发送队列上限。串口速率低，队列用来吸收突发写入。
    std::size_t send_queue_bytes = 64 * 1024;
    std::size_t send_queue_count = 64;
};

// =============================================================================
//  事件
// =============================================================================

/// 串口事件。全部在该串口所在的 io 线程上回调，**同一串口严格串行**。
class SerialHandler {
public:
    virtual ~SerialHandler() = default;

    /// 读完成。
    ///
    /// @param[in] ec  空表示成功读到数据；非空表示出错（设备被拔、权限变化等）。
    ///                **非空时这条串口随即进入关闭流程**。
    /// @param[in] buf 原样传回调用方提交的那份 Buf。
    /// @param[in] n   实际读到的字节数。
    ///
    /// @note 读到多少就是多少——串口没有报文边界，可能是半个帧、也可能是几帧粘一起。
    ///       要收下一条，必须在**本回调返回前**再调一次 `read()`。
    virtual void on_read(std::error_code ec, Buf buf, std::size_t n) = 0;

    /// 写完成。
    ///
    /// @param[in] ec 空表示已全部写入内核缓冲；非空表示写失败。
    /// @param[in] n  实际写入的字节数。
    ///
    /// @note "写入内核缓冲"不等于"已从线上发出"。RS-485 方向控制需要的是后者，
    ///       那要额外等 `tcdrain` / `FlushFileBuffers`——见 `drain()`。
    virtual void on_write(std::error_code ec, ConstBuf buf, std::size_t n)
    {
        (void)ec;
        (void)buf;
        (void)n;
    }

    /// 串口关闭。恰好一次。
    virtual void on_close() {}
};

// =============================================================================
//  串口
// =============================================================================

class SerialPort {
public:
    /// 打开串口。
    ///
    /// @param[in] cfg     配置（设备名、波特率、帧格式）
    /// @param[in] handler 事件处理器（库强持有到关闭）
    /// @param[out] ec     失败原因
    /// @return 失败返回 nullptr
    ///
    /// 失败原因：设备不存在、无权限（Linux 需要 `dialout` 组）、波特率不支持、
    /// 已被其它进程占用（串口是**独占**的）。
    [[nodiscard]] static std::shared_ptr<SerialPort>
    create(const SerialConfig&         cfg,
           std::shared_ptr<SerialHandler> handler,
           std::error_code&            ec);

    ~SerialPort();
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    // ---- 读 ----

    /// 发起一笔读（拉模式）。
    ///
    /// @return false **未受理**，buf 立即归你。原因：
    ///         1. 串口已关闭；
    ///         2. `buf.data == nullptr` 或 `buf.size == 0`；
    ///         3. 已有读在途（同一串口同时只允许一笔）。
    ///
    /// @par 为什么只允许一笔
    ///  同 `Conn`：多笔在途时完成回调无法确定对应哪一笔，缓冲尺寸不同就会错配。
    bool read(Buf buf);

    // ---- 写 ----

    /// 发起一笔写（异步入队，FIFO）。
    ///
    /// @return false 未受理（已关闭 / 队列满 / 参数非法）。
    ///
    /// @note 提交后 `buf` 的内容不得修改，直到 `on_write` 返回。
    bool write(ConstBuf buf);

    /// 等待发送真正完成（所有字节移出硬件）。
    ///
    /// @param[out] ec 失败原因
    /// @return true 表示已排空；false 表示失败或超时
    ///
    /// @par 什么时候需要它
    ///  RS-485 半双工：发完必须确认"最后一个字节已从线上发出"才能放开方向控制。
    ///  `on_write` 只代表"进了内核缓冲"，不代表"发出去了"。
    ///
    /// @warning **阻塞调用**。不要在 io 线程里调——会卡住整个 io_context。
    ///          正确用法是在独立线程里调，或者在 `on_write` 之后用定时器等一拍。
    bool drain(std::error_code& ec);

    /// 丢弃接收/发送缓冲里的残留。
    void flush_input();
    void flush_output();

    // ---- 查询 ----

    [[nodiscard]] bool               is_open() const noexcept;
    [[nodiscard]] std::uint64_t      id() const noexcept;
    [[nodiscard]] const std::string& device() const noexcept;

    // ---- 关闭 ----

    /// 关闭串口。幂等。关闭后不再回调 `on_read`，触发一次 `on_close`。
    void close() noexcept;

private:
    SerialPort();
    class Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace netmech
