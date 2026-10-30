// =============================================================================
//  NetMechanism - TCP echo 示例
// =============================================================================
//  文件    : example/echo_main.cpp
//  用法    : echo_main server <port>         启动回显服务端
//            echo_main client <host> <port>  连上去发一条消息并打印回显
//
//  演示的新接口要点
//  ----------------
//  * 实现两个接口：`ConnHandler`（单连接协议）+ `Linkable`（决定新连接交给谁）。
//  * 读写完成**回调到虚函数**，不需要 enable_shared_from_this，也不用 lambda。
//  * `read()` / `write()` 返回 false 表示"未受理"，此时不会有回调。
//  * 缓冲区由调用方持有（`Buf::owner` 为空时须自己保证生命周期）。
// =============================================================================
#include <netmech/net.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

using namespace netmech;

namespace {

/// 回显处理器：收到什么就写回什么，写完继续读。
class EchoHandler : public ConnHandler {
public:
    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        std::printf("[open] %s -> %s\n",
                    ep->local_address().to_string().c_str(),
                    ep->remote_address().to_string().c_str());
        // 必须保持一笔读在途，否则感知不到对端关闭。
        return ep->read({nullptr, m_rx.data(), m_rx.size()});
    }

    void on_read(std::error_code ec, Buf buf, std::size_t n) override
    {
        if (ec) {
            return;   // 断开 / 保活超时：连接随后关闭
        }
        std::printf("[recv %zu] %.*s\n", n, static_cast<int>(n),
                    static_cast<const char*>(buf.data));
        if (!m_ep->write({nullptr, buf.data, n})) {
            m_ep->close();
        }
    }

    void on_write(std::error_code ec, ConstBuf, std::size_t n) override
    {
        if (ec) {
            return;
        }
        std::printf("[sent %zu]\n", n);
        (void)m_ep->read({nullptr, m_rx.data(), m_rx.size()});   // 写完再读
    }

    void on_close() override { std::printf("[close]\n"); }

private:
    std::shared_ptr<Conn>          m_ep;
    std::array<std::uint8_t, 4096> m_rx{};
};

/// 客户端处理器：连上后发一条消息，收到回显就关闭。
class ClientHandler : public ConnHandler {
public:
    explicit ClientHandler(std::string msg) : m_msg(std::move(msg)) {}

    bool on_open(std::shared_ptr<Conn> ep) override
    {
        m_ep = ep;
        std::printf("[connected] %s\n", ep->remote_address().to_string().c_str());
        (void)ep->write({nullptr, m_msg.data(), m_msg.size()});
        return ep->read({nullptr, m_rx.data(), m_rx.size()});
    }

    void on_read(std::error_code ec, Buf buf, std::size_t n) override
    {
        if (ec) {
            return;
        }
        std::printf("[echo] %.*s\n", static_cast<int>(n),
                    static_cast<const char*>(buf.data));
        done.store(true);
        m_ep->close();
    }

    void on_write(std::error_code, ConstBuf, std::size_t) override {}
    void on_close() override { done.store(true); }

    std::atomic<bool> done{false};

private:
    std::shared_ptr<Conn>          m_ep;
    std::string                    m_msg;
    std::array<std::uint8_t, 4096> m_rx{};
};

/// 服务端：每条新连接给一个 EchoHandler。
class EchoServer : public Linkable {
public:
    std::shared_ptr<ConnHandler> on_accept(Conn&) override
    {
        return std::make_shared<EchoHandler>();
    }
    void on_linkdown(Conn& c) override
    {
        std::printf("[linkdown] id=%llu\n", static_cast<unsigned long long>(c.id()));
    }
};

/// 客户端：连上后创建 ClientHandler。
class EchoClient : public Linkable {
public:
    explicit EchoClient(std::string msg) : m_msg(std::move(msg)) {}

    std::shared_ptr<ConnHandler> on_linkup(Conn&) override
    {
        auto h = std::make_shared<ClientHandler>(m_msg);
        last = h;
        return h;
    }

    void on_linkfail(const Address& remote, std::error_code ec) override
    {
        std::printf("[linkfail] %s: %s\n", remote.to_string().c_str(), ec.message().c_str());
        failed.store(true);
    }

    std::shared_ptr<ClientHandler> last;
    std::atomic<bool>              failed{false};

private:
    std::string m_msg;
};

int run_server(std::uint16_t port)
{
    auto mech = Mechanism::create();
    if (!mech) {
        std::fprintf(stderr, "create failed\n");
        return 1;
    }

    auto srv = std::make_shared<EchoServer>();
    const std::error_code ec = mech->listen(Address::any(port), std::weak_ptr<Linkable>(srv));
    if (ec) {
        std::fprintf(stderr, "listen failed: %s\n", ec.message().c_str());
        return 1;
    }

    const auto bound = mech->local_address_of(Address::any(port));
    std::printf("listening on %s\n", bound ? bound->to_string().c_str() : "?");
    std::printf("press Enter to quit\n");
    (void)std::getchar();

    mech->close_all();
    return 0;
}

int run_client(const std::string& host, std::uint16_t port)
{
    auto mech = Mechanism::create();
    if (!mech) {
        std::fprintf(stderr, "create failed\n");
        return 1;
    }

    auto cli = std::make_shared<EchoClient>("hello from netmech");
    const std::error_code ec =
        mech->connect(Address(host, port), std::weak_ptr<Linkable>(cli), 3000);
    if (ec) {
        std::fprintf(stderr, "connect not accepted: %s\n", ec.message().c_str());
        return 1;
    }

    // 等结果（示例用轮询；业务代码里通常由 on_linkup/on_linkfail 驱动）
    for (int i = 0; i < 300 && !cli->failed.load(); ++i) {
        if (cli->last && cli->last->done.load()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    mech->close_all();
    return cli->failed.load() ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage:\n"
                     "  %s server <port>\n"
                     "  %s client <host> <port>\n",
                     argv[0], argv[0]);
        return 2;
    }

    // 进程级 io 线程数：必须在任何网络对象之前调用。
    if (!initialize(2)) {
        std::fprintf(stderr, "initialize failed (already initialized with another size?)\n");
    }

    const std::string mode = argv[1];
    if (mode == "server") {
        return run_server(static_cast<std::uint16_t>(std::atoi(argv[2])));
    }
    if (mode == "client" && argc >= 4) {
        return run_client(argv[2], static_cast<std::uint16_t>(std::atoi(argv[3])));
    }

    std::fprintf(stderr, "bad arguments\n");
    return 2;
}
