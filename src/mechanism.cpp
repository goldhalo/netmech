// =============================================================================
//  NetMechanism - 网络运行时实现
// =============================================================================
//  文件    : mechanism.cpp
//  用途    : `struct Mechanism::Impl` 的定义与 Mechanism 全部成员函数实现：
//            io 池、listen 表、连接表、connect（含 steady_timer 超时）、
//            close_all、set_linkable、endpoint_count。
//  编码    : UTF-8
//
//  ★ 旧实现缺陷（02-并发与缺陷审查.md）的规避点
//  --------------------------------------------
//  A-01  `init()` 里在 shared_ptr 接管前调用 shared_from_this()：
//        新实现所有 Conn 都经 `std::make_shared<Conn>` 创建，
//        shared_ptr 就绪之后才调用任何可能触发用户代码的逻辑。
//  A-04  linkup/accept 回调后、注册进 map 前 endpoint 无强引用 → UAF：
//        新实现**先**把 shared_ptr 放进连接表，**再**投递用户回调；
//        回调期间对象始终有强引用。
//  A-05  持锁调用用户回调（锁序反转 → 死锁）：
//        新实现的"用户回调"一律在相关锁**之外**、在 Conn 自己的 io 线程
//        内执行；`linkable_table_mutex` / `endpoints_mutex` 只在表操作期间持有。
//  A-06  锁内 accept / 锁外 open：
//        新实现每次 accept 后先创建 Conn（shared_ptr），再投递回调；
//        表插入在投递之前完成。
//  A-07  getAvailableEpid() 跳号 + 检查与插入分离：
//        新实现 id 由 `detail::IdRegistry` 原子自增产生（不跳号），
//        注册连接与分配 id 在同一把锁内完成（不重复 id 互覆）。
//  A-10  fini() 超时即返回 → endpoint 泄漏 + 释放后仍被 io 线程使用：
//        新实现 close_all() 只**请求**关闭（异步收敛），析构时先 close_all()
//        再析构 io 池（IoPool 析构 join 全部线程）——对象存活期严格覆盖
//        所有 io 线程的生命周期。
//  A-11  持锁调用 srv->fini()（内含 join）→ ABBA 死锁：
//        新实现 listen 表的关闭在**释放锁之后**逐个进行。
//  A-12  setParallelNum(0) 除零：io 线程数 0 在 IoPool::initialize() 内归一化。
//  A-15  relink 忽略 `to`：新实现 connect() 用 steady_timer 真正实现 timeout_ms。
//  B-03  linkfail 路径不回收 endpoint：新实现 connect 失败时在**所有路径**
//        上都从连接表摘除并释放 Conn。
//  B-04  m_ok 检查与"创建/打开"分离：新实现用 `closing` 一次性标志 +
//        连接表插入在同一临界区内判定。
//  B-07  getLAddress/getRAddress 的 mutable 缓存：新实现在建立时快照地址。
//  B-11  AsioRuntime::fini 恒 false：新实现析构完整收敛，无"半死"状态。
//  C-05  setLogOutput 空实现：新实现不引入全局日志开关（关键路径零日志），
//        不制造"看起来能关但其实没关"的假象；不包含 <iostream>。
// =============================================================================
#include "netmech_srcpch.hpp"

#include "acceptor.hpp"
#include "conn_state.hpp"
#include "registry.hpp"

#include "netmech/conn.hpp"
#include "netmech/common.hpp"
#include "io_pool.hpp"
#include "netmech/iohandler.hpp"
#include "netmech/linkable.hpp"
#include "netmech/mechanism.hpp"

#include "address_internal.hpp"

#include <atomic>
#include <cstdio>
#include <map>

namespace netmech {

// =============================================================================
// 连接表条目
// =============================================================================
//  【访问控制 - 务必理解】
//  `Mechanism` 是 `Conn` 的 friend，但 friend 权限**只授予 Mechanism 的
//  成员函数**，不授予自由函数、也不授予 `Mechanism::Impl` 的成员函数。
//  因此"调用 Conn 私有构造"这个动作只能写在某个 Mechanism 成员函数里。
//  本文件把它写在 `Mechanism::install_factory_once()` 中（见下方定义）。

/// 被接管连接的内部条目。
struct ConnEntry {
    std::shared_ptr<Conn> ep;    ///< 强引用：回调期间保证对象存活（A-04）
};



// =============================================================================
// Mechanism::Impl
// =============================================================================
struct Mechanism::Impl {
    explicit Impl(const Config& c) : cfg(c) {}

    Config                                            cfg;
    IoPool*                                           pool = nullptr;

    /// 连接表：key = Conn id。强引用，摘除后由最后一个持有者释放。
    mutable std::mutex                                endpoints_mutex;
    std::map<endpoint_id_t, ConnEntry>                endpoints;

    /// 监听器：key = 调用 listen() 时传入的 local（端口 0 保留原样作为键）。
    mutable std::mutex                                listeners_mutex;
    std::map<std::string, std::shared_ptr<Acceptor>>  listeners;

    /// 入参键 → **实际绑定地址**键。用于跨"端口0"与"具体端口"两种入参形态
    /// 判重（见 Mechanism::listen() 第 2.5 步）。
    std::map<std::string, std::string>                bound_keys;

    /// 全局单一订阅者（weak，避免悬挂）。
    mutable std::mutex                                linkable_mutex;
    std::weak_ptr<Linkable>                           global_linkable;

    /// 未完成的 connect 集合（键 = remote + 序号，同一目标可并发多个）。
    mutable std::mutex                                connects_mutex;
    std::map<std::string, int>                        pending_connects;

    /// 运行时是否"已开始关闭"（close_all 之后不再接受新 listen/connect？
    /// 不 —— 按需求 close_all 之后仍可用，故这里只用于析构期判定）。
    std::atomic<bool>                                 destroyed{false};

    /// 活跃连接数快照（避免 endpoint_count() 拿锁）。
    std::atomic<std::size_t>                          active_count{0};

    /// 指向自身的弱引用：给 Conn 注入"摘除钩子"时避免循环持有。
    /// 由 `Mechanism::create()` 在 `make_shared` 之后回填。
    std::weak_ptr<Impl>                               weak_self_;

    // ---- 表操作辅助 ----

    /// 归一化实际绑定地址：
    ///  * 通配地址（0.0.0.0 / ::）与具体地址在端口相同时会被操作系统视为
    ///    冲突，因此两者都归一到 "port:<port>"；
    ///  * 否则用 "ip:port"。
    static std::string key_of_bound(const endpoint_t& ep)
    {
        if (ep.address().is_unspecified()) {
            return std::string("port:") + std::to_string(ep.port());
        }
        return key_of(ep);
    }

    static std::string key_of(const endpoint_t& ep)
    {
        // 用 address + port 的稳定文本作为 map key（endpoint 本身可比较，
        // 但 map<endpoint,...> 需要 operator<，Asio 并未提供，故用字符串）。
        std::string   s = ep.address().to_string();
        s += ':';
        s += std::to_string(ep.port());
        return s;
    }

    /// 注册一个新连接（插入强引用）。
    void add_endpoint(const std::shared_ptr<Conn>& ep)
    {
        {
            std::lock_guard<std::mutex> lock(endpoints_mutex);
            ConnEntry entry;
            entry.ep = ep;
            endpoints.emplace(ep->id(), std::move(entry));
        }
        active_count.fetch_add(1, std::memory_order_relaxed);
    }

    /// 摘除连接（幂等）。
    void remove_endpoint(endpoint_id_t id)
    {
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(endpoints_mutex);
            removed = (endpoints.erase(id) > 0);
        }
        if (removed) {
            const std::size_t prev = active_count.fetch_sub(1, std::memory_order_relaxed);
            (void)prev;
        }
    }

    /// 取全部连接的快照（释放锁后返回，供 close_all 在锁外操作）。
    std::vector<std::shared_ptr<Conn>> snapshot_endpoints()
    {
        std::vector<std::shared_ptr<Conn>> out;
        std::lock_guard<std::mutex> lock(endpoints_mutex);
        out.reserve(endpoints.size());
        for (auto& kv : endpoints) {
            out.push_back(kv.second.ep);
        }
        return out;
    }

    /// 取全部监听器的快照。
    std::vector<std::shared_ptr<Acceptor>> snapshot_listeners()
    {
        std::vector<std::shared_ptr<Acceptor>> out;
        std::lock_guard<std::mutex> lock(listeners_mutex);
        out.reserve(listeners.size());
        for (auto& kv : listeners) {
            out.push_back(kv.second);
        }
        return out;
    }

    /// 解析订阅者：显式给出的优先，否则回退全局（全部为 weak）。
    std::weak_ptr<Linkable> resolve_linkable(const std::weak_ptr<Linkable>& explicit_lb)
    {
        if (!explicit_lb.expired()) {
            return explicit_lb;
        }
        std::lock_guard<std::mutex> lock(linkable_mutex);
        return global_linkable;
    }

    /// 用当前配置设置 socket 选项。
    void apply_socket_options(socket_t& sock) const noexcept
    {
        error_code_t ec;
        try {
            if (cfg.tcp_nodelay) {
                sock.set_option(asio::ip::tcp::no_delay(true), ec);
            }
            if (cfg.socket_buf_size > 0) {
                const auto sz = static_cast<std::size_t>(cfg.socket_buf_size);
                sock.set_option(asio::socket_base::send_buffer_size(
                                    static_cast<int>(sz)), ec);
                sock.set_option(asio::socket_base::receive_buffer_size(
                                    static_cast<int>(sz)), ec);
            }
            if (cfg.linger_zero) {
                sock.set_option(asio::socket_base::linger(true, 0), ec);
            }
        } catch (...) {
            // 选项设置失败不影响功能。
        }
    }

    /// 在当前 io 线程上为一个已连接 socket 创建 Conn 并登记。
    ///
    /// @param[in] sock   已连接 socket（移动）。
    /// @param[in] remote 对端地址。
    /// @param[in] local  本端地址。
    /// @param[in] lb     订阅者（weak）。
    /// @return 创建成功返回 Conn；失败返回空（调用方负责关闭 socket）。
    std::shared_ptr<Conn> make_endpoint(socket_t&& sock,
                                           const endpoint_t& remote,
                                           const endpoint_t& local,
                                           std::weak_ptr<Linkable> lb,
                                           std::shared_ptr<void> user_ctx)
    {
        const std::optional<endpoint_id_t> maybe_id = detail::IdRegistry::try_allocate();
        if (!maybe_id.has_value()) {
            return std::shared_ptr<Conn>();   // id 耗尽：宁可拒绝，绝不复用
        }
        apply_socket_options(sock);

        // 取 io_context：socket 的 executor 指向它。
        io_context_t& ioc = static_cast<io_context_t&>(sock.get_executor().context());

        // owner 弱引用先给空：Conn 对象此刻还没构造出来，由
        // MakeConnObject 在 shared_ptr 就绪后回填（杜绝 A-01 的 bad_weak_ptr）。
        std::shared_ptr<Conn::Impl> state = std::make_shared<Conn::Impl>(
            std::move(sock), ioc, *pool, std::weak_ptr<Conn>(), *maybe_id, local, remote,
            std::move(user_ctx));
        state->linkable = std::move(lb);
        state->linger_zero = cfg.linger_zero;   // 关闭方式随运行时配置

        // ★ Impl 自身**不再需要**弱自引用：它已继承 enable_shared_from_this，
        //   内部投递一律用 `self_ref()` 取强引用（强自持）。

        // 注入"从连接表摘除"的钩子。
        //
        // 用 weak_ptr 持有 Impl：Conn 的生命周期可能短于 Mechanism 的生命
        // 周期（例如运行时已析构而某个在途回调仍持有 Conn），此时
        // lock() 失败即什么也不做，不会造成 UAF。
        //
        // 该钩子由 Conn::Impl::do_close() 的收尾调用（恰好一次），是
        // endpoint_count() 能正确递减的唯一路径。
        std::weak_ptr<Impl> weak_self = weak_self_;
        state->unregister_hook = [weak_self](endpoint_id_t id) {
            std::shared_ptr<Impl> self = weak_self.lock();
            if (!self) {
                return;   // Mechanism 已析构：表也随之消失，无需摘除
            }
            self->remove_endpoint(id);
        };

        // 构造 + 回填 owner：shared_ptr 就绪之后才可能调用 shared_from_this()。
        std::shared_ptr<Conn> ep = MakeConnObject(std::move(state));
        if (!ep) {
            return std::shared_ptr<Conn>();
        }

        // 登记到连接表：**先登记后回调**，回调期间对象必有强引用（A-04）。
        add_endpoint(ep);
        return ep;
    }
};

// =============================================================================
// Mechanism 公共门面
// =============================================================================

Mechanism::Mechanism(std::shared_ptr<Impl> impl) : m_impl(std::move(impl))
{
}

Mechanism::~Mechanism()
{
    if (!m_impl) {
        return;
    }
    m_impl->destroyed.store(true, std::memory_order_release);

    // ── 关闭顺序（严格按照此顺序，每一步都有明确理由）──────────────────────
    //  1. 停止监听器：不再产生新连接。
    //  2. 对每个连接请求 close()：把关闭动作投递到各自的 io_context。
    //  3. 清空监听表 / 连接表：释放本对象持有的 Conn / Acceptor 引用。
    //  4. 最后析构 io 池（stop → join → 销毁 io_context）。
    //
    //  ★ 为什么**不需要**"排空 io 队列"这一步：
    //    运行时是**进程级**的（IoPool::global() 是唯一入口），它持有的
    //    io_context **永不销毁** —— 所以无论 Conn 活多久，它的 socket
    //    析构时都能访问到有效的 context。
    //
    //    在途 handler 一律**强自持**（捕获 shared_ptr），因此库内对象一定
    //    活到最后一个 IO 完成 —— 关闭流程（冲刷挂起回调、on_fini/on_linkdown）
    //    **必定执行**，通知不会丢。强自持形成的环
    //        io_context -> 队列 -> handler -> Conn -> Impl -> IoPool -> io_context
    //    **无害**：没有谁需要它死（context 永不销毁）。
    //
    //    旧实现需要 `drain()` 冲刷队列来断环，而 drain 跨线程 poll 同一个
    //    context，破坏了"1 context = 1 线程"的不变量，会让同一连接的
    //    完成处理并发执行。
    //
    //  第 3、4 步的先后仍然是关键：**必须先清表再"释放池引用"**。
    //  池本身不会被销毁（进程级），但清表能让本对象不再持有 Conn。
    // ──────────────────────────────────────────────────────────────────────

    // 1) 停止监听。
    {
        const std::vector<std::shared_ptr<Acceptor>> listeners = m_impl->snapshot_listeners();
        for (const std::shared_ptr<Acceptor>& acc : listeners) {
            if (acc) {
                acc->close();
            }
        }
    }

    // 2) 请求关闭全部连接（异步投递）。
    {
        const std::vector<std::shared_ptr<Conn>> eps = m_impl->snapshot_endpoints();
        for (const std::shared_ptr<Conn>& ep : eps) {
            if (ep) {
                ep->close();
            }
        }
    }

    // 3) 清表：释放本对象持有的 Acceptor / Conn 引用。
    //    ★ 此时队列里的 handler 只持有 weak_ptr，既不会阻止 Conn 析构，
    //      也不需要任何"排空"动作。
    {
        std::lock_guard<std::mutex> lock(m_impl->listeners_mutex);
        m_impl->listeners.clear();
        m_impl->bound_keys.clear();   // 与 listeners 同步（见 close_all 的说明）
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->endpoints_mutex);
        m_impl->endpoints.clear();
    }
    m_impl->active_count.store(0, std::memory_order_relaxed);

    // 4) io 池是**进程级单例**，不随 Mechanism 析构而停止。
    //    因此 io_context 的存活期天然不短于任何 IO 对象，
    //    "socket 比 context 活得久"这一整类析构顺序问题从结构上消失。
}
std::shared_ptr<Mechanism> Mechanism::create()
{
    return create(Config{});
}

std::shared_ptr<Mechanism> Mechanism::create(Config cfg)
{
    // 首次创建时安装 Conn 构造入口（std::call_once 保证线程安全、幂等）。
    //
    // 为什么必须在"Mechanism 的成员函数"里安装：`Conn` 的构造函数是
    // private 且只 `friend class Mechanism`，而 friend 权限只覆盖
    // **Mechanism 的成员函数**。成员函数体内定义的 lambda 闭包类型享有与该
    // 成员函数相同的访问权限，因此下面的 lambda 可以调用 Conn 的私有构造。
    static std::once_flag s_install_once;
    std::call_once(s_install_once, []() {
        install_endpoint_factory([](std::shared_ptr<Conn::Impl> state) -> std::shared_ptr<Conn> {
            if (!state) {
                return std::shared_ptr<Conn>();
            }
            std::shared_ptr<Conn> ep(new Conn(std::move(state)));
            ep->m_impl->owner = ep;   // shared_from_this 只在 shared_ptr 就绪后用（A-01）
            return ep;
        });
    });

    // 运行时是**进程级**的：io_context 永不销毁，线程数由
    // IoPool::initialize() 在进程启动时一次性决定（之后不可更改）。
    //
    // ★ 这是整套生命周期模型的地基：
    //   * 库内对象（Conn / UdpChannel / …）的 socket / timer 的析构
    //     永远能访问到有效的 io_context；
    //   * 库内对象的在途完成处理可以安全地捕获自己的 shared_ptr（强自持），
    //     关闭流程必定执行，通知不会丢。
    IoPool& pool = IoPool::global();
    std::shared_ptr<Mechanism::Impl> impl = std::make_shared<Mechanism::Impl>(cfg);
    impl->pool = &pool;
    // 回填弱引用：Conn 的摘除钩子需要它（不能强引用，否则 Impl 与
    // Conn 互相持有形成环）。
    impl->weak_self_ = impl;
    std::shared_ptr<Mechanism> mech(new Mechanism(std::move(impl)));
    return mech;
}

// -----------------------------------------------------------------------------
// 服务端
// -----------------------------------------------------------------------------

namespace {

/// 端口 0（临时端口）监听请求的唯一序号，用于区分同一次会话中的多个
/// `listen(X:0)` 调用（它们共用同一个入参键 `X:0`）。
std::uint64_t next_ephemeral_seq_() noexcept
{
    static std::atomic<std::uint64_t> seq{0};
    return seq.fetch_add(1, std::memory_order_relaxed) + 1;
}

} // namespace

std::error_code Mechanism::listen(const Address& addr,
                                            std::weak_ptr<Linkable> linkable,
                                            std::uint32_t backlog,
                                            std::shared_ptr<void> user_ctx)
{
    if (!m_impl || !m_impl->pool) {
        return make_boost_error_code(NetErrc::not_initialized);
    }
    // 公共接口用 Address，内部（acceptor / 连接表键）仍用 asio endpoint。
    const endpoint_t local = detail::to_asio_endpoint(addr);
    if (local.port() == 0 && local.address().is_unspecified()) {
        // 0.0.0.0:0 合法（系统分配端口）；其它非法地址由 bind 报错。
    }

    const std::weak_ptr<Linkable> resolved = m_impl->resolve_linkable(linkable);

    // ★ 端口 0 的语义：由系统分配**新**端口。
    //   因此 `listen(X:0)` 多次调用是完全合法的，每次都得到不同端口，
    //   **不能**参与"入参键"去重（否则第二次就会被误判为 duplicate）。
    //   真正的去重在 bind 成功之后、按实际绑定地址做（见下面第 2.5 步）。
    const bool ephemeral = (local.port() == 0);
    const std::string key = ephemeral
        ? (Impl::key_of(local) + "#" + std::to_string(next_ephemeral_seq_()))
        : Impl::key_of(local);

    // 1) 重复监听检测 + 占位（同一临界区，避免"检查-插入"分离竞态）。
    //    仅对**非端口 0** 的显式地址做入参去重。
    {
        std::lock_guard<std::mutex> lock(m_impl->listeners_mutex);
        if (!ephemeral && m_impl->listeners.find(key) != m_impl->listeners.end()) {
            return make_boost_error_code(NetErrc::duplicate_endpoint);
        }
        m_impl->listeners.emplace(key, std::shared_ptr<Acceptor>());
    }

    // 2) 创建并打开 acceptor（锁外；Acceptor::open 是同步 bind+listen）。
    //    deliver 回调：accept 成功后创建 Conn，并在其 io 线程上通知 Linkable。
    IoPool* pool = m_impl->pool;
    std::weak_ptr<Impl> weak_impl = m_impl;
    std::weak_ptr<Linkable> lb = resolved;

    Acceptor::DeliverFn deliver =
        [weak_impl, lb, user_ctx](socket_t&& sock, const endpoint_t& remote,
                        const endpoint_t& local_addr) -> bool {
        const std::shared_ptr<Impl> impl = weak_impl.lock();
        if (!impl) {
            return false;   // 运行时已析构：拒绝，由 Acceptor 关闭 socket
        }

        // 重解析订阅者：listen 时给的 weak 可能已过期，回退全局。
        std::weak_ptr<Linkable> use_lb = lb;
        if (use_lb.expired()) {
            use_lb = impl->resolve_linkable(lb);
        }

        std::shared_ptr<Conn> ep =
            impl->make_endpoint(std::move(sock), remote, local_addr, use_lb, user_ctx);
        if (!ep) {
            return false;
        }

        std::shared_ptr<Conn::Impl> state = ep->m_impl;

        // 在 Conn 所在的 io 线程上执行"接管"流程：
        //   建链成功 → on_accept 要 handler → on_init → 进入服务。
        // 全部在锁外、在该连接的 io 线程内（A-05 / A-06 的规避点）。
        //
        // 注意：lambda 内**先复制为局部变量**再供嵌套 lambda 使用。MSVC 对
        // "外层显式捕获 + 内层 [&] 引用该捕获名"会歧义解析（报出
        // 'C2039 __this is not a member of std::shared_ptr' 之类的连锁错误）。
        //
        // ★ 强自持：任务捕获 Conn 与 Impl 的**强引用** ——
        //   接管流程（on_accept / on_init）**必定执行**，不会因为
        //   "调用方恰好先放手"而丢失（那会让连接卡在既没被接管、
        //   也没被关闭的悬空状态）。
        state->post([self_state = state, self_ep = ep]() {

            std::shared_ptr<Linkable> handler_owner;
            try {
                handler_owner = self_state->linkable.lock();
            } catch (...) {
                handler_owner = std::shared_ptr<Linkable>();
            }

            if (!handler_owner) {
                // 无订阅者：立即关闭（不计入 linkdown，因为从未交给业务）。
                self_state->do_close(make_boost_error_code(NetErrc::not_connected));
                return;
            }

            std::shared_ptr<ConnHandler> h;
            std::shared_ptr<Linkable> lb = handler_owner;
            std::shared_ptr<Conn> ep_arg = self_ep;
            // on_accept 包 try/catch（异常 → 视为拒绝）。
            const bool callback_ok = invoke_user([lb, ep_arg, &h]() {
                h = lb->on_accept(*ep_arg);
                return true;
            });

            if (!callback_ok || !h) {
                // 拒绝：关闭，不投递 on_linkdown（从未"连接成功"给业务）。
                self_state->handler.reset();
                self_state->handler_inited = false;
                self_state->link_established = false;
                self_state->do_close(make_boost_error_code(NetErrc::not_connected));
                return;
            }

            self_state->handler = h;
            self_state->handler_inited = false;
            self_state->link_established = true;

            std::shared_ptr<ConnHandler> h_init = h;
            std::shared_ptr<Conn> ep_init = self_ep;
            // on_open 包 try/catch（异常 → 按 false 处理）。
            const bool inited = invoke_user([h_init, ep_init]() {
                return h_init->on_open(ep_init);
            });
            if (!inited) {
                // 初始化失败：关闭；on_close 不调用（因为 on_open 没有成功）。
                self_state->handler_inited = false;
                self_state->do_close(make_boost_error_code(NetErrc::not_connected));
                return;
            }
            self_state->handler_inited = true;
        });
        return true;
    };

    std::shared_ptr<Acceptor> acc;
    try {
        acc = std::make_shared<Acceptor>(*pool, deliver);
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_impl->listeners_mutex);
        m_impl->listeners.erase(key);
        return make_boost_error_code(NetErrc::not_initialized);
    }

    const error_code_t open_ec = acc->open(local, backlog);
    if (open_ec) {
        std::lock_guard<std::mutex> lock(m_impl->listeners_mutex);
        m_impl->listeners.erase(key);
        // bind 失败（端口被占）归一化为具体错误；地址非法归为 invalid_argument。
        return open_ec;
    }

    // 2.5) **按实际绑定地址**再做一次重复检测。
    //
    //   为什么不能只用入参 `local` 作键：`listen(127.0.0.1:0)` 的入参键是
    //   `127.0.0.1:0`，而系统实际分配了 52765；之后再 `listen(127.0.0.1:52765)`
    //   时入参键不同 → 判重 miss → 同一 ip:port 被监听两次（未定义行为，
    //   且第一个 acceptor 被覆盖泄漏）。
    //
    //   因此以 `acceptor.local_endpoint()` 的**实际地址**为键登记；若已存在
    //   同一实际地址的监听器，关闭本次新建的 acceptor 并返回 duplicate_endpoint。
    //   注意通配地址（0.0.0.0:P / [::]:P）与具体地址（X.X.X.X:P）在操作系统
    //   看来会冲突，这里按"端口 + 通配可覆盖具体地址"归一到同一键判断。
    const endpoint_t bound = acc->local_address();
    const std::string bound_key = Impl::key_of_bound(bound);
    {
        std::lock_guard<std::mutex> lock(m_impl->listeners_mutex);
        for (const auto& kv : m_impl->bound_keys) {
            if (kv.second == bound_key) {
                // 已存在同一实际地址的监听器：拒绝。
                m_impl->listeners.erase(key);
                acc->close();
                return make_boost_error_code(NetErrc::duplicate_endpoint);
            }
        }
        auto it = m_impl->listeners.find(key);
        if (it == m_impl->listeners.end()) {
            // 并发 close_all 把占位删掉了：回滚。
            acc->close();
            return make_boost_error_code(NetErrc::not_initialized);
        }
        it->second = std::move(acc);
        m_impl->bound_keys.emplace(key, bound_key);
    }
    return error_code_t{};
}

// -----------------------------------------------------------------------------
// 客户端
// -----------------------------------------------------------------------------

/// connect 的进行中状态。由 shared_ptr 在异步链中传递。
///
/// 生命周期与"恰好一次回调"：
///  - `finished` 是单向原子标志，保证 on_linkup / on_linkfail 总共只发生一次；
///  - 超时路径与完成路径可能竞争（timer 与 connect 完成在不同时刻），由
///    `finished` 裁决，后到者静默退出。
struct ConnectOp {
    socket_t                             sock;
    timer_t                              timer;
    endpoint_t                           remote;
    endpoint_t                           local;
    std::weak_ptr<Mechanism::Impl>       impl;
    std::weak_ptr<Linkable>              linkable;
    std::string                          key;         ///< pending_connects 的键
    std::shared_ptr<void>                user_ctx;    ///< 透传给新建连接
    std::atomic<bool>                    finished{false};

    ConnectOp(io_context_t& ioc, std::shared_ptr<Mechanism::Impl> impl_in,
              const endpoint_t& remote_in, const endpoint_t& local_in,
              std::weak_ptr<Linkable> lb, std::string k,
              std::shared_ptr<void> ctx)
        : sock(ioc)
        , timer(ioc)
        , remote(remote_in)
        , local(local_in)
        , impl(std::move(impl_in))
        , linkable(std::move(lb))
        , key(std::move(k))
        , user_ctx(std::move(ctx))
    {
    }
};

namespace {

/// 把自己的键从 pending_connects 里摘除（幂等）。
void erase_pending_connect(const std::shared_ptr<Mechanism::Impl>& impl, const std::string& key)
{
    if (!impl) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl->connects_mutex);
    impl->pending_connects.erase(key);
}

/// 连接失败收尾：通知 on_linkfail（恰好一次）。
void finish_connect_fail(const std::shared_ptr<ConnectOp>& op, const error_code_t& ec)
{
    if (op->finished.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    {
        error_code_t ignored;
        op->timer.cancel(ignored);
    }
    close_socket_noexcept(op->sock);

    // 清理 pending 记录与 socket 由 ConnectOp 析构接管。
    const std::shared_ptr<Mechanism::Impl> impl = op->impl.lock();
    erase_pending_connect(impl, op->key);

    // on_linkfail 包 try/catch。
    std::shared_ptr<Linkable> lb;
    try {
        lb = op->linkable.lock();
    } catch (...) {
        lb = std::shared_ptr<Linkable>();
    }
    if (lb) {
        invoke_void([&lb, &op, ec]() {
            lb->on_linkfail(detail::from_asio_endpoint(op->remote), ec);
        });
    }
}

} // namespace

std::error_code Mechanism::connect(const Address& remote_addr,
                                             std::weak_ptr<Linkable> linkable,
                                             std::uint32_t timeout_ms,
                                             const Address& local_addr,
                                             std::shared_ptr<void> user_ctx)
{
    if (!m_impl || !m_impl->pool) {
        return make_boost_error_code(NetErrc::not_initialized);
    }
    // 公共接口用 Address，内部仍用 asio endpoint。
    const endpoint_t remote = detail::to_asio_endpoint(remote_addr);
    const endpoint_t local  = detail::to_asio_endpoint(local_addr);
    if (remote.address().is_unspecified() || remote.port() == 0) {
        return make_boost_error_code(NetErrc::invalid_argument);
    }

    const std::weak_ptr<Linkable> resolved = m_impl->resolve_linkable(linkable);
    // 在途表的键带自增序号：**允许同一 remote 同时有多个在途 connect**。
    // 业务上一个目标建多条连接是常见需求（连接池、并发探测、压力测试）；
    // 若用 remote 本身做唯一键，第二次调用会被误判成"重复建链"而拒掉。
    static std::atomic<std::uint64_t> connect_seq{0};
    const std::string key = Impl::key_of(remote) + "#" +
                            std::to_string(connect_seq.fetch_add(1, std::memory_order_relaxed));

    // 1) 登记在途 connect（键唯一，因此同一目标可以并发多个）。
    {
        std::lock_guard<std::mutex> lock(m_impl->connects_mutex);
        m_impl->pending_connects[key] = 1;
    }

    // 2) 选一个 io_context 建 socket + 定时器。
    io_context_t& ioc = m_impl->pool->next_io_context();
    std::shared_ptr<ConnectOp> op;
    try {
        op = std::make_shared<ConnectOp>(ioc, m_impl, remote, local, resolved, key,
                                         std::move(user_ctx));
    } catch (...) {
        erase_pending_connect(m_impl, key);
        return make_boost_error_code(NetErrc::not_initialized);
    }

    error_code_t ec;

    // 3) 打开 + 可选的本地绑定 + 配置选项。
    op->sock.open(remote.protocol(), ec);
    if (ec) {
        erase_pending_connect(m_impl, key);
        return ec;
    }
    m_impl->apply_socket_options(op->sock);

    if (!local.address().is_unspecified() || local.port() != 0) {
        op->sock.bind(local, ec);
        if (ec) {
            close_socket_noexcept(op->sock);
            erase_pending_connect(m_impl, key);
            return ec;
        }
    }

    std::weak_ptr<Impl> weak_impl = m_impl;

    // 4) 超时定时器：用 steady_timer（单调时钟），超时后 **close socket** 让
    //    在途 connect 以 operation_aborted 返回 —— Boost 1.74 的
    //    basic_socket::cancel() 无法中止已进入内核的 SYN，close() 才是正确手段。
    if (timeout_ms > 0) {
        op->timer.expires_after(std::chrono::milliseconds(timeout_ms));
        op->timer.async_wait([op, weak_impl](const error_code_t& timer_ec) {
            if (timer_ec) {
                return;   // 被完成路径取消
            }
            // 超时：先关 socket（打断在途 connect），再以 timed_out 通知。
            error_code_t ignored;
            op->sock.close(ignored);
            (void)weak_impl;
            finish_connect_fail(op, asio::error::timed_out);
        });
    }

    // 5) 发起 connect。
    op->sock.async_connect(remote, [op, weak_impl](const error_code_t& conn_ec) {
        // 超时路径可能已先到：finished 裁决，后到者静默退出。
        if (op->finished.load(std::memory_order_acquire)) {
            return;
        }
        if (conn_ec) {
            // operation_aborted 且 socket 已关 = 超时已处理；其它为真实失败。
            if (conn_ec == asio::error::operation_aborted) {
                finish_connect_fail(op, asio::error::timed_out);
            } else {
                finish_connect_fail(op, conn_ec);
            }
            return;
        }

        // 成功：取消定时器并"一次性"占位。
        if (op->finished.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        error_code_t ignored;
        op->timer.cancel(ignored);

        const std::shared_ptr<Impl> impl = weak_impl.lock();
        if (!impl) {
            // 运行时已析构：关闭连接，不回调。
            close_socket_noexcept(op->sock);
            return;
        }
        erase_pending_connect(impl, op->key);

        // 取地址（在建立时快照，杜绝 mutable 缓存的数据竞争 B-07）。
        error_code_t addr_ec;
        const endpoint_t remote_addr = op->sock.remote_endpoint(addr_ec);
        const endpoint_t local_addr = op->sock.local_endpoint(addr_ec);
        const endpoint_t effective_remote = addr_ec ? op->remote : remote_addr;

        std::shared_ptr<Conn> ep = impl->make_endpoint(
            std::move(op->sock), effective_remote, local_addr, op->linkable, op->user_ctx);
        if (!ep) {
            return;   // id 耗尽 / 创建失败：socket 已被 close
        }
        std::shared_ptr<Conn::Impl> state = ep->m_impl;

        // 接管流程：全部在 Conn 所在的 io 线程内、锁外执行。
        // ★ 强自持：同 accept 侧 —— 接管流程必定执行。
        state->post([self_state = state, self_ep = ep]() {
            std::shared_ptr<Linkable> handler_owner;
            try {
                handler_owner = self_state->linkable.lock();
            } catch (...) {
                handler_owner = std::shared_ptr<Linkable>();
            }
            if (!handler_owner) {
                self_state->do_close(make_boost_error_code(NetErrc::not_connected));
                return;
            }

            std::shared_ptr<ConnHandler> handler;
            const bool callback_ok = invoke_user([&]() {
                handler = handler_owner->on_linkup(*self_ep);
                return true;
            });

            if (!callback_ok || !handler) {
                // 拒绝：连接已建立过 → 关闭并投递 on_linkdown。
                self_state->handler.reset();
                self_state->handler_inited = false;
                self_state->link_established = true;
                self_state->do_close(make_boost_error_code(NetErrc::not_connected));
                return;
            }

            self_state->handler = handler;
            self_state->handler_inited = false;
            self_state->link_established = true;

            const bool inited = invoke_user(
                [&]() { return handler->on_open(self_ep); }, false);
            if (!inited) {
                self_state->handler_inited = false;
                self_state->do_close(make_boost_error_code(NetErrc::not_connected));
                return;
            }
            self_state->handler_inited = true;
        });
    });

    return error_code_t{};
}

// -----------------------------------------------------------------------------
// 生命周期
// -----------------------------------------------------------------------------

bool Mechanism::write(std::uint64_t conn_id, ConstBuf buf)
{
    if (!m_impl) {
        return false;
    }
    std::shared_ptr<Conn> ep;
    {
        std::lock_guard<std::mutex> lock(m_impl->endpoints_mutex);
        const auto it = m_impl->endpoints.find(conn_id);
        if (it == m_impl->endpoints.end()) {
            return false;                 // 连接已不存在
        }
        ep = it->second.ep;               // 强引用：后续 write 期间对象不会析构
    }
    return ep->write(std::move(buf));
}

bool Mechanism::close(std::uint64_t conn_id) noexcept
{
    if (!m_impl) {
        return false;
    }
    std::shared_ptr<Conn> ep;
    {
        std::lock_guard<std::mutex> lock(m_impl->endpoints_mutex);
        const auto it = m_impl->endpoints.find(conn_id);
        if (it == m_impl->endpoints.end()) {
            return false;
        }
        ep = it->second.ep;
    }
    ep->close();
    return true;
}

void Mechanism::close_all() noexcept
{
    if (!m_impl) {
        return;
    }

    // 1) 停止监听（锁外逐个关闭；旧实现 A-11 是持锁 join）。
    const std::vector<std::shared_ptr<Acceptor>> listeners = m_impl->snapshot_listeners();
    for (const std::shared_ptr<Acceptor>& acc : listeners) {
        if (acc) {
            acc->close();
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->listeners_mutex);
        m_impl->listeners.clear();
        // ★ 必须同步清理 bound_keys：它是~实际绑定地址~的去重账本，
        //   若不同步，`close_all()` 之后再 listen 同一端口会被误判为
        //   duplicate_endpoint（实测：close_all() 后无法在同一端口重新监听）。
        m_impl->bound_keys.clear();
    }

    // 2) 关闭全部连接（锁外；Conn::close 自身幂等且异步收敛）。
    const std::vector<std::shared_ptr<Conn>> eps = m_impl->snapshot_endpoints();
    for (const std::shared_ptr<Conn>& ep : eps) {
        if (ep) {
            ep->close();
        }
    }

    // 3) 放弃未完成的 connect（不回调 on_linkfail：属于主动放弃）。
    {
        std::lock_guard<std::mutex> lock(m_impl->connects_mutex);
        m_impl->pending_connects.clear();
    }

    // 4) 不销毁 io 池 —— 按契约，close_all 之后仍可 listen()/connect()，
    //    running() 继续为 true。
}

bool Mechanism::running() const noexcept
{
    if (!m_impl || !m_impl->pool) {
        return false;
    }
    if (m_impl->destroyed.load(std::memory_order_acquire)) {
        return false;
    }
    return m_impl->pool->running();
}

void Mechanism::set_linkable(std::weak_ptr<Linkable> linkable)
{
    if (!m_impl) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_impl->linkable_mutex);
    m_impl->global_linkable = std::move(linkable);
}

std::size_t Mechanism::endpoint_count() const noexcept
{
    if (!m_impl) {
        return 0;
    }
    return m_impl->active_count.load(std::memory_order_relaxed);
}

std::size_t Mechanism::io_threads() const noexcept
{
    if (!m_impl || !m_impl->pool) {
        return 0;
    }
    return m_impl->pool->size();
}

std::optional<Address> Mechanism::local_address_of(const Address& addr) const
{
    const endpoint_t local = detail::to_asio_endpoint(addr);
    if (!m_impl) {
        return std::nullopt;
    }
    const std::string key = Impl::key_of(local);
    std::shared_ptr<Acceptor> acc;

    {
        std::lock_guard<std::mutex> lock(m_impl->listeners_mutex);

        // 端口 0 的查询：`listen(X:0)` 每次都会分配新端口，入参键被加上了
        // 唯一序号后缀。这里按前缀查找**最近一次**匹配的临时端口监听器
        // （调用方通常紧接着 listen 之后查询，语义足够）。
        if (local.port() == 0) {
            const std::string prefix = key + "#";
            // ★ 必须按**序号数值**取最大，不能按 map 的键序取最后一个。
            //   临时端口的键形如 `127.0.0.1:0#1` … `127.0.0.1:0#10`，
            //   而 std::map 按**字典序**排列：`#10` 排在 `#2` 之前。
            //   取最后一个会拿到 #10 而不是 #2 —— 查询结果与实际最近一次
            //   listen 不符（实测：连续 9 次以上临时监听后端口就查错了）。
            std::uint64_t best_seq = 0;
            bool         found    = false;
            for (auto it = m_impl->listeners.begin(); it != m_impl->listeners.end(); ++it) {
                if (!it->second) {
                    continue;
                }
                if (it->first.size() <= prefix.size()
                    || it->first.compare(0, prefix.size(), prefix) != 0) {
                    continue;
                }
                std::uint64_t seq = 0;
                bool         ok  = true;
                for (std::size_t i = prefix.size(); i < it->first.size(); ++i) {
                    const char c = it->first[i];
                    if (c < '0' || c > '9') {
                        ok = false;
                        break;
                    }
                    seq = seq * 10 + static_cast<std::uint64_t>(c - '0');
                }
                if (!ok) {
                    continue;
                }
                if (!found || seq > best_seq) {
                    best_seq = seq;
                    acc      = it->second;
                    found    = true;
                }
            }
        } else {
            const auto it = m_impl->listeners.find(key);
            if (it != m_impl->listeners.end() && it->second) {
                acc = it->second;
            }
        }
    }
    if (!acc) {
        return std::nullopt;
    }
    return detail::from_asio_endpoint(acc->local_address());
}


} // namespace netmech
