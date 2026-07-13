#pragma once
#include <boost/asio.hpp>
#include <deque>
#include <functional>
#include <string>
#include <thread>
#include <memory>
#include <atomic>
#include <unordered_map>
#include <iostream>

#include "shared/protocol.hpp"
#include "shared/message.hpp"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// ============================================================
// One TCP connection to a peer (outbound or inbound)
// ============================================================
class PeerConnection : public std::enable_shared_from_this<PeerConnection> {
public:
    using MsgCallback = std::function<void(const lan_chat::Message&)>;
    using CloseCallback = std::function<void(const std::string& peer_name)>;

    PeerConnection(asio::io_context& ioc, const std::string& peer_name,
                   MsgCallback on_msg, CloseCallback on_close)
        : socket_(ioc), peer_name_(peer_name),
          on_msg_(std::move(on_msg)), on_close_(std::move(on_close)) {}

    void start_from_socket(tcp::socket sock) {
        socket_ = std::move(sock);
        connected_ = true;
        do_read();
    }

    void start_connect(const std::string& host, uint16_t port) {
        host_ = host;
        port_ = port;
        socket_.async_connect(tcp::endpoint(asio::ip::make_address(host), port),
            [self = shared_from_this()](boost::system::error_code ec) {
                if (!ec) {
                    self->connected_ = true;
                    self->do_read();
                }
            });
    }

    void send(const lan_chat::Message& msg) {
        if (!connected_) return;
        auto json = lan_chat::serialize(msg) + "\n"; // Newline delimiter
        auto sp = std::make_shared<std::string>(std::move(json));
        asio::post(socket_.get_executor(),
            [self = shared_from_this(), sp]() {
                self->send_queue_.push_back(sp);
                if (self->send_queue_.size() == 1) self->do_write();
            });
    }

    bool is_connected() const { return connected_; }
    const std::string& peer_name() const { return peer_name_; }

private:
    tcp::socket socket_;
    std::string peer_name_;
    std::string host_;
    uint16_t port_ = 0;
    std::atomic<bool> connected_{false};

    MsgCallback on_msg_;
    CloseCallback on_close_;

    asio::streambuf read_buf_;
    std::deque<std::shared_ptr<std::string>> send_queue_;

    void do_read() {
        asio::async_read_until(socket_, read_buf_, '\n',
            [self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    std::istream is(&self->read_buf_);
                    std::string line;
                    std::getline(is, line);
                    if (!line.empty()) {
                        try {
                            auto msg = lan_chat::parse(line);
                            if (self->on_msg_) self->on_msg_(msg);
                        } catch (...) {}
                    }
                    self->do_read();
                } else {
                    self->connected_ = false;
                    if (self->on_close_) self->on_close_(self->peer_name_);
                }
            });
    }

    void do_write() {
        if (send_queue_.empty()) return;
        auto& sp = send_queue_.front();
        asio::async_write(socket_, asio::buffer(*sp),
            [self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                if (!ec) self->send_queue_.pop_front();
                if (!self->send_queue_.empty()) self->do_write();
            });
    }
};

// ============================================================
// TCP acceptor for incoming peer connections + outbound manager
// ============================================================
class PeerNetwork {
public:
    using MsgCallback = std::function<void(const lan_chat::Message&)>;
    using PeerCallback = std::function<void(const std::string& name, bool online)>;

    PeerNetwork(asio::io_context& ioc, const std::string& my_name,
                MsgCallback on_msg, PeerCallback on_peer_status)
        : ioc_(ioc), acceptor_(ioc), my_name_(my_name),
          on_msg_(std::move(on_msg)), on_peer_status_(std::move(on_peer_status)) {}

    // Start listening for incoming TCP connections
    uint16_t start_listen() {
        // Bind to port 0 = OS picks a free port
        acceptor_ = tcp::acceptor(ioc_, tcp::endpoint(tcp::v4(), 0));
        uint16_t port = acceptor_.local_endpoint().port();
        do_accept();
        std::cout << "[peer] TCP listener on port " << port << "\n";
        return port;
    }

    void stop() {
        acceptor_.close();
        for (auto& [name, conn] : peers_) {
            // Send goodbye before closing (best effort)
            auto goodbye = lan_chat::make_goodbye(my_name_);
            conn->send(goodbye);
        }
        peers_.clear();
    }

    // Connect to a discovered peer
    void connect_to_peer(const std::string& name, const std::string& ip, int port) {
        if (name == my_name_) return;
        if (peers_.count(name)) return; // Already connected

        auto conn = std::make_shared<PeerConnection>(ioc_, name,
            [this](const lan_chat::Message& msg) { on_peer_msg(msg); },
            [this](const std::string& peer) { on_peer_disconnect(peer); });

        conn->start_connect(ip, port);
        peers_[name] = conn;
    }

    // Send message to a specific peer
    void send_to(const std::string& peer, const lan_chat::Message& msg) {
        auto it = peers_.find(peer);
        if (it != peers_.end() && it->second->is_connected()) {
            it->second->send(msg);
        }
    }

    // Broadcast to all peers
    void broadcast(const lan_chat::Message& msg) {
        for (auto& [name, conn] : peers_) {
            if (conn->is_connected()) conn->send(msg);
        }
    }

    bool is_connected_to(const std::string& name) const {
        auto it = peers_.find(name);
        return it != peers_.end() && it->second->is_connected();
    }

private:
    asio::io_context& ioc_;
    tcp::acceptor acceptor_;
    std::string my_name_;
    MsgCallback on_msg_;
    PeerCallback on_peer_status_;
    std::unordered_map<std::string, std::shared_ptr<PeerConnection>> peers_;

    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket sock) {
                if (!ec) {
                    // We don't know the peer name yet — first message will identify them
                    // For now, use a placeholder and extract name from first message
                    auto conn = std::make_shared<PeerConnection>(ioc_, "?",
                        [this](const lan_chat::Message& msg) { on_incoming_msg(msg); },
                        [this](const std::string& peer) { on_peer_disconnect(peer); });
                    conn->start_from_socket(std::move(sock));
                    pending_.push_back(conn);
                }
                if (acceptor_.is_open()) do_accept();
            });
    }

    // First message from a newly accepted incoming connection identifies the peer
    void on_incoming_msg(const lan_chat::Message& msg) {
        // The first message should identify the sender
        if (!msg.from.empty()) {
            // Move from pending to peers
            for (auto it = pending_.begin(); it != pending_.end(); ++it) {
                if ((*it)->peer_name() == "?") {
                    auto conn = *it;
                    pending_.erase(it);
                    conn->send(lan_chat::make_ack(msg.id, 0));
                    peers_[msg.from] = conn;
                    // Rebind the callbacks
                    // We'll create a new wrapper — simpler: just handle in the main callback
                    if (on_peer_status_) on_peer_status_(msg.from, true);
                    // Forward the actual message
                    on_peer_msg(msg);
                    break;
                }
            }
            return;
        }
        on_peer_msg(msg);
    }

    void on_peer_msg(const lan_chat::Message& msg) {
        if (on_msg_) on_msg_(msg);
    }

    void on_peer_disconnect(const std::string& name) {
        if (name != "?" && on_peer_status_) {
            on_peer_status_(name, false);
        }
        peers_.erase(name);
    }

    std::vector<std::shared_ptr<PeerConnection>> pending_;
};
