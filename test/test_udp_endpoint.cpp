// ============================================================================
//  test_udp_endpoint.cpp —— 新 UdpEndpoint 接口验证
//
//  验证：
//    * 报文收发：send_to → 对端 on_read，n / 内容 / from 正确
//    * 推送式：调用方不调 read()，库自己持续收
//    * last_peer / on_peer_changed：观测到真实来源
//    * 截断：报文超过 recv_capacity 时 on_read 收到 datagram_truncated
// ============================================================================
#include <netmech/net.hpp>
#include <netmech/udp.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

using namespace netmech;
using namespace netmech_test;

namespace {

using namespace std::chrono_literals;

bool wait_until(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

/// 收集报文：内容、来源、错误码、peer 变更次数。
class PacketSink final : public UdpHandler {
public:
    void on_read(UdpEndpoint&, std::error_code ec, Buf buf, std::size_t n,
                 const Address& from) override
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        last_ec = ec;
        last_from = from;
        last_size = n;
        last_payload.assign(static_cast<const char*>(buf.data), n);
        ++count;
    }

    void on_peer_changed(UdpEndpoint&, const Address& from) override
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        peer_changes.push_back(from);
    }

    void on_close(UdpEndpoint&, std::error_code) override { ++closed; }

    std::string snapshot_payload() const
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return last_payload;
    }

    mutable std::mutex          m_mtx;
    std::error_code             last_ec;
    Address                     last_from;
    std::size_t                 last_size = 0;
    std::string                 last_payload;
    std::vector<Address>        peer_changes;
    std::atomic<int>            count{0};
    std::atomic<int>            closed{0};
};

} // namespace

// ---------------------------------------------------------------------------
// 1. 基本收发 + last_peer / on_peer_changed
// ---------------------------------------------------------------------------
NETMECH_TEST(udp_roundtrip_reports_peer)
{
    std::error_code ec;
    auto sink = std::make_shared<PacketSink>();

    UdpConfig rx_cfg;
    rx_cfg.bind = Address("127.0.0.1", 0);
    rx_cfg.recv_capacity = 512;
    auto rx = create_udp_endpoint(rx_cfg, sink, nullptr, ec);
    REQUIRE(!ec);
    REQUIRE(rx != nullptr);

    UdpConfig tx_cfg;
    tx_cfg.bind = Address("127.0.0.1", 0);
    auto tx = create_udp_endpoint(tx_cfg, std::make_shared<PacketSink>(), nullptr, ec);
    REQUIRE(!ec);
    REQUIRE(tx != nullptr);

    const Address target = rx->local_address();
    REQUIRE(target.port() != 0);

    const char payload[] = "udp-hello";
    REQUIRE(tx->send_to(target, {nullptr, payload, sizeof(payload) - 1}));

    REQUIRE(wait_until([&] { return sink->count.load() == 1; }, 3000ms));
    CHECK_EQ(sink->snapshot_payload(), std::string(payload));
    CHECK_EQ(sink->last_size, sizeof(payload) - 1);
    CHECK(!sink->last_ec);

    // 来源 == 发送端的本地地址（回环直连，未经 NAT）
    CHECK(sink->last_from == tx->local_address());

    // last_peer 与 on_peer_changed 都观测到同一个来源
    const auto peer = rx->last_peer();
    REQUIRE(peer.has_value());
    CHECK(*peer == tx->local_address());
    REQUIRE(sink->peer_changes.size() == 1u);
    CHECK(sink->peer_changes[0] == tx->local_address());

    // 统计
    CHECK_EQ(rx->datagrams_received(), 1u);
    CHECK_EQ(tx->datagrams_sent(), 1u);

    // 第二包来自同一来源：不应再次触发 on_peer_changed
    REQUIRE(tx->send_to(target, {nullptr, payload, sizeof(payload) - 1}));
    REQUIRE(wait_until([&] { return sink->count.load() == 2; }, 3000ms));
    CHECK_EQ(sink->peer_changes.size(), 1u);

    rx->close();
    tx->close();
}

// ---------------------------------------------------------------------------
// 2. 报文超过 recv_capacity → 截断并报 datagram_truncated
// ---------------------------------------------------------------------------
NETMECH_TEST(udp_truncates_oversized_datagram)
{
    std::error_code ec;
    auto sink = std::make_shared<PacketSink>();

    UdpConfig rx_cfg;
    rx_cfg.bind = Address("127.0.0.1", 0);
    rx_cfg.recv_capacity = 64;          // 故意很小
    auto rx = create_udp_endpoint(rx_cfg, sink, nullptr, ec);
    REQUIRE(!ec);
    REQUIRE(rx != nullptr);

    UdpConfig tx_cfg;
    tx_cfg.bind = Address("127.0.0.1", 0);
    auto tx = create_udp_endpoint(tx_cfg, std::make_shared<PacketSink>(), nullptr, ec);
    REQUIRE(!ec);
    REQUIRE(tx != nullptr);

    std::vector<std::uint8_t> big(200, 0xAB);
    REQUIRE(tx->send_to(rx->local_address(), {nullptr, big.data(), big.size()}));

    REQUIRE(wait_until([&] { return sink->count.load() == 1; }, 3000ms));
    CHECK_EQ(sink->last_size, 64u);                                  // 只收到缓冲大小
    CHECK(sink->last_ec == NetErrc::datagram_truncated);             // 明确告知被截断
    CHECK_EQ(rx->datagrams_truncated(), 1u);

    // 端点仍可用：再来一条小报文应当正常收到
    const char small[] = "ok";
    REQUIRE(tx->send_to(rx->local_address(), {nullptr, small, sizeof(small) - 1}));
    REQUIRE(wait_until([&] { return sink->count.load() == 2; }, 3000ms));
    CHECK(!sink->last_ec);
    CHECK_EQ(sink->last_size, sizeof(small) - 1);

    rx->close();
    tx->close();
}

// ---------------------------------------------------------------------------
// 3. close 后不再收报文，on_close 恰好一次
// ---------------------------------------------------------------------------
NETMECH_TEST(udp_close_stops_delivery)
{
    std::error_code ec;
    auto sink = std::make_shared<PacketSink>();

    UdpConfig rx_cfg;
    rx_cfg.bind = Address("127.0.0.1", 0);
    auto rx = create_udp_endpoint(rx_cfg, sink, nullptr, ec);
    REQUIRE(!ec);

    UdpConfig tx_cfg;
    tx_cfg.bind = Address("127.0.0.1", 0);
    auto tx = create_udp_endpoint(tx_cfg, std::make_shared<PacketSink>(), nullptr, ec);
    REQUIRE(!ec);

    const Address target = rx->local_address();
    REQUIRE(tx->send_to(target, {nullptr, "a", 1}));
    REQUIRE(wait_until([&] { return sink->count.load() == 1; }, 3000ms));

    rx->close();
    CHECK(!rx->is_open());
    CHECK(wait_until([&] { return sink->closed.load() == 1; }, 3000ms));

    // 关闭后发送仍可能成功（UDP 无确认），但不会再投递
    (void)tx->send_to(target, {nullptr, "b", 1});
    std::this_thread::sleep_for(100ms);
    CHECK_EQ(sink->count.load(), 1);
    CHECK_EQ(sink->closed.load(), 1);

    tx->close();
}
