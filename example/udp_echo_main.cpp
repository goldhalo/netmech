// =============================================================================
//  NetMechanism - UDP echo 示例
// =============================================================================
//  文件    : example/udp_echo_main.cpp
//  用法    : udp_echo_main server <port>         启动回显服务端
//            udp_echo_main client <host> <port>  发一条报文并等回显
//
//  演示的 UDP 侧要点
//  ----------------
//  * 一个 `UdpEndpoint` = 一个绑定的本地端口；**没有连接、没有会话**。
//  * 库自己持续收报文（推送式），你只实现 `UdpHandler::on_read`，不必调 read()。
//  * `from` 只用来回包，**不要**拿它识别身份（NAT / 端口协商不一致时不可信）。
//  * `last_peer()` 能拿到"实际发来数据的地址"，回包用它最稳。
// =============================================================================
#include <netmech/net.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

using namespace netmech;

namespace {

/// 服务端：收到就原样回给来源地址。
class EchoSink : public UdpHandler {
public:
    void on_read(UdpEndpoint& ep, std::error_code ec, Buf buf, std::size_t n,
                 const Address& from) override
    {
        if (ec) {
            std::printf("[drop] %s\n", ec.message().c_str());
            return;
        }
        std::printf("[recv %zu from %s] %.*s\n", n, from.to_string().c_str(),
                    static_cast<int>(n), static_cast<const char*>(buf.data));
        // 回包必须用 from（NAT 之后这是唯一能到达对端的地址）
        (void)ep.send_to(from, {nullptr, buf.data, n});
    }

    void on_peer_changed(UdpEndpoint&, const Address& from) override
    {
        std::printf("[new peer] %s\n", from.to_string().c_str());
    }

    void on_close(UdpEndpoint&, std::error_code ec) override
    {
        std::printf("[closed] %s\n", ec.message().c_str());
    }
};

/// 客户端：收到回显就标记完成。
class EchoClientSink : public UdpHandler {
public:
    void on_read(UdpEndpoint&, std::error_code ec, Buf buf, std::size_t n,
                 const Address& from) override
    {
        if (ec) {
            return;
        }
        std::printf("[echo %zu from %s] %.*s\n", n, from.to_string().c_str(),
                    static_cast<int>(n), static_cast<const char*>(buf.data));
        got.store(true);
    }

    std::atomic<bool> got{false};
};

int run_server(std::uint16_t port)
{
    UdpConfig cfg;
    cfg.bind          = Address::any(port);
    cfg.recv_capacity = 2048;      // 单报文上限；一个 MTU 内不分片

    std::error_code ec;
    auto ep = create_udp_endpoint(cfg, std::make_shared<EchoSink>(), nullptr, ec);
    if (!ec) {
        std::printf("listening on %s\n", ep->local_address().to_string().c_str());
    } else {
        std::fprintf(stderr, "create failed: %s\n", ec.message().c_str());
        return 1;
    }

    std::printf("press Enter to quit\n");
    (void)std::getchar();

    ep->close();
    return 0;
}

int run_client(const std::string& host, std::uint16_t port)
{
    UdpConfig cfg;
    cfg.bind = Address::any(0);    // 系统分配本地端口

    std::error_code ec;
    auto sink = std::make_shared<EchoClientSink>();
    auto ep   = create_udp_endpoint(cfg, sink, nullptr, ec);
    if (ec) {
        std::fprintf(stderr, "create failed: %s\n", ec.message().c_str());
        return 1;
    }

    const char payload[] = "hello udp";
    if (!ep->send_to(Address(host, port), {nullptr, payload, sizeof(payload) - 1})) {
        std::fprintf(stderr, "send failed\n");
        return 1;
    }

    for (int i = 0; i < 200 && !sink->got.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!sink->got.load()) {
        std::fprintf(stderr, "timeout\n");
    }
    ep->close();
    return sink->got.load() ? 0 : 1;
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

    if (!initialize(2)) {
        std::fprintf(stderr, "initialize failed\n");
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
