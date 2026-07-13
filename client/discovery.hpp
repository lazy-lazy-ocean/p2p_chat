#pragma once
#include <boost/asio.hpp>
#include <functional>
#include <string>
#include <thread>
#include <memory>
#include <atomic>
#include <iostream>

namespace asio = boost::asio;
using udp = asio::ip::udp;

// UDP broadcast discovery: announces presence, discovers peers
class UdpDiscovery {
public:
    using PeerCallback = std::function<void(const std::string& username,
                                             const std::string& ip, int tcp_port)>;
    using ByeCallback = std::function<void(const std::string& username)>;

    UdpDiscovery() = default;
    ~UdpDiscovery() { stop(); }

    void set_on_peer(PeerCallback cb) { on_peer_ = std::move(cb); }
    void set_on_bye(ByeCallback cb) { on_bye_ = std::move(cb); }

    void start(const std::string& username, int tcp_port, uint16_t udp_port = 19833) {
        username_ = username;
        tcp_port_ = tcp_port;
        udp_port_ = udp_port;
        should_run_ = true;

        ioc_ = std::make_unique<asio::io_context>();

        // Open UDP socket
        socket_ = std::make_unique<udp::socket>(*ioc_,
            udp::endpoint(udp::v4(), udp_port_));
        socket_->set_option(udp::socket::reuse_address(true));
        socket_->set_option(asio::socket_base::broadcast(true));

        do_receive();

        // Start broadcast timer (every 3 seconds)
        broadcast_timer_ = std::make_unique<asio::steady_timer>(*ioc_);
        do_broadcast();

        thread_ = std::thread([this]() {
            try { ioc_->run(); }
            catch (const std::exception& e) {
                std::cerr << "[discovery] " << e.what() << "\n";
            }
        });

        std::cout << "[discovery] Listening on UDP " << udp_port_ << "\n";
    }

    void stop() {
        should_run_ = false;
        if (ioc_) ioc_->stop();
        if (thread_.joinable()) thread_.join();
        socket_.reset();
        ioc_.reset();
    }

private:
    std::unique_ptr<asio::io_context> ioc_;
    std::unique_ptr<udp::socket> socket_;
    std::unique_ptr<asio::steady_timer> broadcast_timer_;
    std::thread thread_;
    std::atomic<bool> should_run_{false};

    std::string username_;
    int tcp_port_ = 0;
    uint16_t udp_port_ = 19833;
    PeerCallback on_peer_;
    ByeCallback on_bye_;

    udp::endpoint sender_ep_;
    char recv_buf_[2048];

    void do_receive() {
        socket_->async_receive_from(
            asio::buffer(recv_buf_), sender_ep_,
            [this](boost::system::error_code ec, std::size_t len) {
                if (!ec && len > 0) {
                    handle_packet(std::string(recv_buf_, len));
                }
                if (should_run_) do_receive();
            });
    }

    void handle_packet(const std::string& data) {
        try {
            auto msg = lan_chat::parse(data);
            if (msg.from == username_) return; // Ignore self

            if (msg.type == lan_chat::MsgType::Hello) {
                std::string ip = sender_ep_.address().to_string();
                if (on_peer_) on_peer_(msg.from, ip, msg.tcp_port);
            } else if (msg.type == lan_chat::MsgType::Goodbye) {
                if (on_bye_) on_bye_(msg.from);
            }
        } catch (...) {}
    }

    void do_broadcast() {
        if (!should_run_) return;

        auto hello = lan_chat::make_hello(username_, tcp_port_);
        auto data = lan_chat::serialize(hello);

        auto ep = udp::endpoint(asio::ip::address_v4::broadcast(), udp_port_);
        socket_->async_send_to(asio::buffer(data), ep,
            [this](boost::system::error_code, std::size_t) {});

        broadcast_timer_->expires_after(std::chrono::seconds(3));
        broadcast_timer_->async_wait([this](boost::system::error_code ec) {
            if (!ec) do_broadcast();
        });
    }
};
