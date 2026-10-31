// =============================================================================
//  NetMechanism - 汇总头
// =============================================================================
//  文件    : netmech/net.hpp
//  用途    : 一次性引入全部公共接口。业务代码通常只需要
//              #include <netmech/net.hpp>
//  依赖    : 标准库 + 独立 Asio（只有 common.hpp 引 <asio/error.hpp>，
//            用于错误码常量，不拖入网络栈）。
//  编码    : UTF-8
//
//  ★ 三条契约（先记住这三条，其余都是细节）
//  ---------------------------------------------------------------------------
//   1. 回调串行   一条连接的全部回调都在同一个 io 线程上，不会并发；
//   2. buffer 归你  read/write 返回 true 后，缓冲必须活到完成回调。
//                  用 Buf::owner 可以让库替你持有（异步场景推荐）；
//   3. false = 没受理  不回调，缓冲立刻归你。**反压也走这条，不是错误、不断链。**
//
//  ★ 最小服务端（完整可跑版本见 example/echo_main.cpp）
//  ---------------------------------------------------------------------------
//    class MyHandler : public ConnHandler {
//    public:
//        bool on_open(std::shared_ptr<Conn> ep) override {
//            m_ep = ep;
//            return ep->read({nullptr, m_rx.data(), m_rx.size()});   // 必须有读在途
//        }
//        void on_read(std::error_code ec, Buf buf, std::size_t n) override {
//            if (ec) { return; }                                     // 断开 / 保活超时
//            m_ep->write({nullptr, buf.data, n});
//        }
//        void on_write(std::error_code ec, ConstBuf, std::size_t) override {
//            if (!ec) { m_ep->read({nullptr, m_rx.data(), m_rx.size()}); }
//        }
//        void on_close() override { m_ep.reset(); }
//    private:
//        std::shared_ptr<Conn>          m_ep;
//        std::array<std::uint8_t, 4096> m_rx{};
//    };
//
//    class MyLinkable : public Linkable {
//    public:
//        std::shared_ptr<ConnHandler> on_accept(Conn&) override {
//            return std::make_shared<MyHandler>();   // 返回 nullptr = 拒绝该连接
//        }
//    };
//
//    initialize(4);                                            // 进程级，最先调用
//    auto mech = Mechanism::create();
//    auto link = std::make_shared<MyLinkable>();
//    mech->listen(Address::any(9000), std::weak_ptr<Linkable>(link));
// =============================================================================

#pragma once

// 基础类型与错误码
#include "netmech/common.hpp"

// 协议抽象（P1：为 UDP 铺路；对既有 TCP 用户完全无影响）

// 抽象接口
#include "netmech/linkable.hpp"
#include "netmech/iohandler.hpp"
#include "netmech/udp.hpp"
#include "netmech/serial.hpp"
#include "netmech/worker_pool.hpp"

// 具体类型
#include "netmech/conn.hpp"
#include "netmech/mechanism.hpp"

// -----------------------------------------------------------------------------
// 命名空间速记
// -----------------------------------------------------------------------------
//  netmech 内已提供：
//    netmech::tcp            == asio::ip::tcp
//    netmech::ip             == asio::ip
//    netmech::mutable_buffer == asio::mutable_buffer
//    netmech::const_buffer   == asio::const_buffer
//
//  因此调用方可写：netmech::tcp::endpoint，而不必写全限定名。
