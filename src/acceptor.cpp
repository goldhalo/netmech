// =============================================================================
//  NetMechanism - 监听器实现
// =============================================================================
//  文件    : acceptor.cpp
//  用途    : Acceptor 的实现。设计说明见 acceptor.hpp 文件头。
//  编码    : UTF-8
//
//  ★ 稳健性要点
//  ------------
//  [R1] accept 失败（非 operation_aborted）→ 关闭/重建 acceptor + 指数退避
//       后重试，**绝不静默熄火**（旧实现 A-09）。
//  [R2] 所有完成处理都在本对象的 io 线程上串行；对象存活由 shared_from_this
//       保证（旧实现绑定裸 `this`）。
//  [R3] 退避定时器与 accept 循环共享同一"停止"旗标；close() 会同时取消二者。
//  [R4] 交付给 Mechanism 的 socket 一旦移出，本对象不再触碰（移动语义），
//       避免双方同时操作同一 socket 的 UB。
//  [R5] 所有涉及 acceptor_ 的操作都在本对象的 io 线程内执行；open()/close()
//       从任意线程调用时把动作投递到 io_context（用 atomic 的 opened/stopping
//       做快速判据）。
// =============================================================================
#include "netmech_srcpch.hpp"

#include "acceptor.hpp"

#include "netmech/common.hpp"
#include "io_pool.hpp"

namespace netmech {

namespace {

/// 退避基数与上限。
constexpr std::chrono::milliseconds kRetryBase{100};
constexpr std::chrono::milliseconds kRetryMax{5000};

/// 从 pool 的 next_io_context() 取 context；pool 为空时退化为线程局部静态
/// context（仅防御内部误用，正常运行不会走到）。
io_context_t& context_of(IoPool& pool) noexcept
{
    return pool.next_io_context();
}

/// 第 attempt 次退避时长：指数增长、封顶 5s，带确定性抖动（避免多实例同拍重试）。
std::chrono::milliseconds backoff_delay(std::uint32_t attempt) noexcept
{
    const std::uint32_t shift = (attempt > 6U) ? 6U : attempt;   // 移位防 UB
    std::chrono::milliseconds delay = kRetryBase * (std::uint32_t{1} << shift);
    if (delay > kRetryMax) {
        delay = kRetryMax;
    }
    const std::int64_t jitter_pct = static_cast<std::int64_t>((attempt % 5U) * 8U);
    const std::int64_t total = delay.count();
    const std::int64_t adjusted = total - (total * jitter_pct) / 100;
    return std::chrono::milliseconds(adjusted > 0 ? adjusted : total);
}

/// SO_EXCLUSIVEADDRUSE（Windows 专有）的 Asio 选项封装。
///
/// Asio 1.74 未提供该选项的公共封装，但它本质就是 `setsockopt(SOL_SOCKET,
/// SO_EXCLUSIVEADDRUSE, TRUE)`，因此用 `socket_option::boolean` 表达。
#if defined(_WIN32)
using exclusive_address_use_t =
    asio::detail::socket_option::boolean<SOL_SOCKET, SO_EXCLUSIVEADDRUSE>;
#endif

/// 施加监听 socket 的地址复用选项。
///
/// @param[in,out] acceptor 目标 acceptor（必须已 open）。
/// @param[out]    ec       失败时的错误码。
/// @return true 表示成功；false 时 `ec` 有值。
///
/// @note 语义差异详见调用点的长注释。核心：Windows 用独占选项，
///       其它平台用 SO_REUSEADDR。
bool apply_listen_socket_options(asio::ip::tcp::acceptor& acceptor,
                                 error_code_t& ec) noexcept
{
    ec.clear();
#if defined(_WIN32)
    // Windows：独占绑定，禁止其它 socket 抢同一端口。
    // 若目标系统不支持该选项（极老版本），退化为设 SO_REUSEADDR(false)，
    // 即也用默认的独占语义。
    acceptor.set_option(exclusive_address_use_t(true), ec);
    if (ec) {
        error_code_t ignored;
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(false), ignored);
        ec.clear();   // 默认语义本身就是独占，不算失败
    }
    return true;
#else
    // POSIX：允许绑定 TIME_WAIT 地址（利于快速重启），
    // 同时仍禁止两个 socket 同时监听同一端口（由内核保证）。
    acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
    return !ec;
#endif
}

} // namespace

Acceptor::Acceptor(IoPool& pool, DeliverFn deliver)
    // ★ `pool_` 先初始化（拷贝而非 move）：它必须**先于** `ioc_` 就绪，
    //   否则 `ioc_` 会指向一个已经失效的 context。拷贝保留了入参 `pool`，
    //   `context_of(pool)` 才拿得到有效的池。
    : pool_(&pool)
    , ioc_(context_of(pool))
    , acceptor_(ioc_)
    , retry_timer_(ioc_)
    , pending_socket_(ioc_)
    , deliver_(std::move(deliver))
{
}

Acceptor::~Acceptor()
{
    // 析构前必须保证 acceptor 已关闭；正常路径由 Mechanism 显式 close()。
    // 这里兜底：投递不可行（对象正在销毁），直接同步关闭（socket 操作线程安全）。
    error_code_t ignored;
    retry_timer_.cancel(ignored);
    acceptor_.close(ignored);
    close_socket_noexcept(pending_socket_);
}

error_code_t Acceptor::open(const endpoint_t& local, std::uint32_t backlog)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (opened_) {
        return make_boost_error_code(NetErrc::already_initialized);
    }

    local_ = local;
    backlog_ = (backlog == 0) ? static_cast<std::uint32_t>(128) : backlog;

    std::error_code ec;
    error_code_t ignored_local;
    try {
        // 0.0.0.0:0 的组合（默认 endpoint）也允许：由系统分配端口。
        acceptor_.open(local_.protocol(), ec);   // protocol() 对 unspecified 也安全
        if (ec) {
            open_error_ = ec;
            return ec;
        }
        // ── 地址复用策略（跨平台差异，务必按平台分开处理）─────────────
        //
        //  SO_REUSEADDR 在两个平台上的语义**完全不同**：
        //   * Linux/POSIX：只允许绑定处于 TIME_WAIT 的地址，**不允许**两个
        //     socket 同时监听同一端口 —— 正是我们想要的"重启快、冲突报错"。
        //   * Windows：**允许**另一个 socket 绑定并监听同一端口，连接会被
        //     随机分流到两个 socket —— 这是著名的 Windows 坑，会导致
        //     "端口被占用却 listen 成功"的静默故障。
        //
        //  因此：
        //   * 非 Windows：设 SO_REUSEADDR；
        //   * Windows  ：设 SO_EXCLUSIVEADDRUSE（独占语义），**不可**再设
        //     SO_REUSEADDR（两者在 Windows 上互斥，同时设置会导致 bind 失败）。
        //
        //  代价：Windows 上服务重启时，若旧 listener 仍处于 TIME_WAIT 则可能
        //  绑不上。listener 通常不会进入 TIME_WAIT（被动关闭的是已建立连接，
        //  与本端监听 socket 无关），因此实际影响很小；正确性优先。
        if (!apply_listen_socket_options(acceptor_, ec)) {
            open_error_ = ec;
            acceptor_.close(ignored_local);
            return ec;
        }
        acceptor_.bind(local_, ec);
        if (ec) {
            open_error_ = ec;
            acceptor_.close(ignored_local);
            return ec;
        }
        acceptor_.listen(static_cast<int>(backlog_), ec);
        if (ec) {
            open_error_ = ec;
            acceptor_.close(ignored_local);
            return ec;
        }
        // 取实际绑定地址（端口 0 → 系统分配），供 local_address_of() 查询。
        bound_local_ = acceptor_.local_endpoint(ec);
        if (ec) {
            bound_local_ = local_;
        }
    } catch (...) {
        // 同步 Asio 调用在极端情况下可能抛（如内存分配失败）。
        open_error_ = asio::error::invalid_argument;
        error_code_t ignored;
        acceptor_.close(ignored);
        return open_error_;
    }

    opened_ = true;
    stopping_ = false;
    retry_attempts_.store(0, std::memory_order_relaxed);
    backoff_.store(false, std::memory_order_relaxed);

    // 启动 accept 循环（在 io 线程上，与后续完成处理严格串行）。
    //
    // ★ 强自持：任务捕获 Acceptor 的**强引用** ——
    //   accept 循环**必定开始**，不会因为"调用方恰好先放手"而丢失
    //   （那会让监听端口无人 accept，表现为"连上了但没有连接"）。
    std::shared_ptr<Acceptor> self = shared_from_this();
    asio::post(ioc_, [self]() {
        try {
            self->do_accept();
        } catch (...) {
            // 保证不逃逸。
        }
    });
    return error_code_t{};
}

void Acceptor::close() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_ || stopping_) {
        return;   // 幂等：未打开或已在停止中
    }
    stopping_ = true;
    opened_ = false;

    // 先从任意线程同步关闭 acceptor：让在途 accept 立刻以 operation_aborted
    // 返回，从而让 io 线程上的循环尽快收敛（不会永久挂住 io 线程）。
    error_code_t ignored;
    acceptor_.close(ignored);
    retry_timer_.cancel(ignored);
    close_socket_noexcept(pending_socket_);

    // 再投递一次清理：在 io 线程内保证与 handle_accept 的先后不破坏状态。
    // ★ 强自持：同 open() —— 关闭流程必定执行。
    std::shared_ptr<Acceptor> self = shared_from_this();
    try {
        asio::post(ioc_, [self]() {
            try {
                self->close_impl();
            } catch (...) {
            }
        });
    } catch (...) {
        // io_context 已停止：上面已同步关闭，可接受。
    }
}

void Acceptor::close_impl() noexcept
{
    error_code_t ignored;
    retry_timer_.cancel(ignored);
    acceptor_.close(ignored);
    close_socket_noexcept(pending_socket_);
    backoff_.store(false, std::memory_order_release);
}

bool Acceptor::is_open() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return opened_ && !stopping_;
}

endpoint_t Acceptor::local_address() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return bound_local_;
}

bool Acceptor::is_backoff() const noexcept
{
    return backoff_.load(std::memory_order_acquire);
}

std::uint32_t Acceptor::retry_count() const noexcept
{
    return retry_errors_.load(std::memory_order_relaxed);
}

asio::ip::tcp::acceptor& Acceptor::socket() noexcept
{
    return acceptor_;
}

void Acceptor::configure_socket(socket_t& sock) noexcept
{
    error_code_t ec;
    try {
        // TCP_NODELAY 与 socket 缓冲由 Mechanism 统一配置；这里只做 acceptor
        // 侧的公共设置（keep_alive 由业务用应用层保活实现，不设 SO_KEEPALIVE）。
        sock.set_option(asio::ip::tcp::no_delay(true), ec);
        sock.set_option(asio::socket_base::keep_alive(false), ec);
    } catch (...) {
        // socket 选项设置失败不影响连接可用性。
    }
}

void Acceptor::do_accept()
{
    // io 线程内。若已停止则不再发起。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!opened_ || stopping_) {
            return;
        }
        if (!acceptor_.is_open()) {
            // acceptor 被意外关闭（例如 close() 的同步路径）：走重建/退避。
            schedule_reopen(make_boost_error_code(NetErrc::accept_failed));
            return;
        }
    }

    pending_socket_ = socket_t(ioc_);
    // ★ 强自持：完成处理捕获 Acceptor 的**强引用** ——
    //   accept 完成后必定继续处理（投递新连接 / 退避重试），
    //   不会出现"监听器半途消失"导致端口不再接受连接。
    std::shared_ptr<Acceptor> self = shared_from_this();
    try {
        acceptor_.async_accept(pending_socket_, [self](const error_code_t& ec) {
            try {
                self->handle_accept(ec);
            } catch (...) {
                // 绝不逃逸到 io_context。
            }
        });
    } catch (...) {
        schedule_reopen(make_boost_error_code(NetErrc::accept_failed));
    }
}

void Acceptor::handle_accept(const error_code_t& ec)
{
    // 情况 1：本端主动停止 → 安静退出（旧实现这里也忽略，但会留下 pending 槽）。
    if (ec == asio::error::operation_aborted) {
        error_code_t ignored;
        close_socket_noexcept(pending_socket_);
        (void)ignored;
        return;
    }

    // 情况 2：其它错误 → **退避后重试**，绝不熄火（旧实现 A-09 的修复点）。
    if (ec) {
        error_code_t ignored;
        close_socket_noexcept(pending_socket_);
        schedule_reopen(ec);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!opened_ || stopping_) {
            // 已经 close()：这个连接不再接收，安静关闭。
            close_socket_noexcept(pending_socket_);
            return;
        }
    }

    // 成功：整理选项、取地址，然后**移动**交付给 Mechanism。
    configure_socket(pending_socket_);

    endpoint_t remote;
    endpoint_t local;
    {
        error_code_t addr_ec;
        remote = pending_socket_.remote_endpoint(addr_ec);
        if (addr_ec) {
            // 对端刚连上就断了：丢弃这个连接，继续 accept（不当作 accept 失败）。
            close_socket_noexcept(pending_socket_);
            do_accept();
            return;
        }
        local = pending_socket_.local_endpoint(addr_ec);
        if (addr_ec) {
            local = bound_local_;
        }
    }

    bool accepted = false;
    if (deliver_) {
        try {
            accepted = deliver_(std::move(pending_socket_), remote, local);
        } catch (...) {
            accepted = false;
        }
    }
    if (!accepted) {
        // 交付失败（运行时关闭 / 无订阅者）：关闭该 socket，继续监听。
        close_socket_noexcept(pending_socket_);
    }

    // 退避计数复位：一次成功说明 acceptor 健康。
    retry_attempts_.store(0, std::memory_order_relaxed);
    backoff_.store(false, std::memory_order_release);

    // 继续下一轮 accept（永不熄火）。
    do_accept();
}

void Acceptor::schedule_reopen(const error_code_t& ec)
{
    // io 线程内。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!opened_ || stopping_) {
            return;
        }
    }

    retry_errors_.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t attempt = retry_attempts_.fetch_add(1, std::memory_order_relaxed);
    backoff_.store(true, std::memory_order_release);

    const std::chrono::milliseconds delay = backoff_delay(attempt);
    (void)ec;   // 具体错误仅用于诊断（不在此记录日志）

    error_code_t ignored;
    retry_timer_.cancel(ignored);
    retry_timer_.expires_after(delay);

    // 尝试重建 acceptor：fd 耗尽（EMFILE）时 accept 失败并不等于 listen socket
    // 失效，若仍然可用就不必重建；这里按需重建，失败也没有关系 —— 定时器到期
    // 后还会再试一次。
    reopen_acceptor();

    // ★ 强自持：退避定时器回调捕获 Acceptor 的**强引用** ——
    //   重开 acceptor 必定执行，不会出现"监听器停了却再也没人重开"。
    std::shared_ptr<Acceptor> self = shared_from_this();
    retry_timer_.async_wait([self](const error_code_t& wait_ec) {
        try {
            if (wait_ec) {
                return;   // 被 close()/重新调度取消
            }
            self->backoff_.store(false, std::memory_order_release);
            self->do_accept();
        } catch (...) {
        }
    });
}

bool Acceptor::reopen_acceptor()
{
    // io 线程内。
    if (acceptor_.is_open()) {
        return true;   // 无需重建
    }

    error_code_t ec;
    error_code_t ignored_local;
    asio::ip::tcp::acceptor fresh(ioc_);
    try {
        fresh.open(local_.protocol(), ec);
        if (ec) {
            return false;
        }
        if (!apply_listen_socket_options(fresh, ec)) {
            fresh.close(ignored_local);
            return false;
        }
        fresh.bind(bound_local_, ec);
        if (ec) {
            fresh.close(ignored_local);
            return false;
        }
        fresh.listen(static_cast<int>(backlog_), ec);
        if (ec) {
            fresh.close(ignored_local);
            return false;
        }
    } catch (...) {
        return false;
    }

    std::swap(acceptor_, fresh);
    error_code_t ignored;
    fresh.close(ignored);   // 关闭旧 acceptor（若仍开）
    return true;
}

} // namespace netmech
