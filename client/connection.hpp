#pragma once
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <functional>
#include <string>
#include <thread>
#include <deque>
#include <memory>
#include <atomic>
#include <chrono>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using tcp = asio::ip::tcp;

// WebSocket client with auto-reconnect
class WSConnection {
public:
    using MessageCallback = std::function<void(const std::string&)>;
    using StateCallback = std::function<void(bool connected)>;

    WSConnection() = default;
    ~WSConnection() { disconnect(); }

    void set_on_message(MessageCallback cb) { on_message_ = std::move(cb); }
    void set_on_state(StateCallback cb) { on_state_ = std::move(cb); }

    void connect(const std::string& host, uint16_t port) {
        host_ = host;
        port_ = port;
        should_reconnect_ = true;
        reconnect_backoff_ = std::chrono::seconds(1);
        do_connect();
    }

    void disconnect() {
        should_reconnect_ = false;
        if (ws_ && ws_->is_open()) {
            beast::error_code ec;
            ws_->close(ws::close_code::normal, ec);
        }
        if (ioc_) ioc_->stop();
        if (io_thread_.joinable()) io_thread_.join();
        ioc_.reset();
        ws_.reset();
        if (on_state_) on_state_(false);
    }

    void send(const std::string& msg) {
        if (!ioc_) return;
        asio::post(*ioc_, [this, msg]() {
            send_queue_.push_back(msg);
            if (send_queue_.size() == 1 && ws_ && ws_->is_open()) {
                do_write();
            }
        });
    }

    bool is_connected() const { return connected_; }

private:
    std::unique_ptr<asio::io_context> ioc_;
    std::unique_ptr<ws::stream<beast::tcp_stream>> ws_;
    std::thread io_thread_;
    std::string host_;
    uint16_t port_ = 9833;
    std::atomic<bool> connected_{false};
    std::atomic<bool> should_reconnect_{false};

    MessageCallback on_message_;
    StateCallback on_state_;

    beast::flat_buffer read_buffer_;
    std::deque<std::string> send_queue_;
    std::chrono::seconds reconnect_backoff_{1};
    std::shared_ptr<asio::steady_timer> reconnect_timer_;
    bool writing_ = false;

    void do_connect() {
        ioc_ = std::make_unique<asio::io_context>();
        ws_ = std::make_unique<ws::stream<beast::tcp_stream>>(*ioc_);

        tcp::resolver resolver(*ioc_);
        auto results = resolver.resolve(host_, std::to_string(port_));

        beast::get_lowest_layer(*ws_).async_connect(results,
            [this](beast::error_code ec, auto) {
                if (ec) {
                    schedule_reconnect();
                    return;
                }
                ws_->async_handshake(host_, "/",
                    [this](beast::error_code ec) {
                        if (ec) {
                            schedule_reconnect();
                            return;
                        }
                        connected_ = true;
                        reconnect_backoff_ = std::chrono::seconds(1);
                        if (on_state_) on_state_(true);
                        do_read();
                        // Flush send queue
                        if (!send_queue_.empty()) do_write();
                    });
            });

        io_thread_ = std::thread([this]() {
            try { ioc_->run(); }
            catch (const std::exception& e) {
                // ignore
            }
        });
    }

    void schedule_reconnect() {
        if (!should_reconnect_) return;

        if (on_state_) on_state_(false);
        connected_ = false;

        // Clean up
        if (ioc_) ioc_->stop();
        if (io_thread_.joinable()) io_thread_.join();
        ioc_.reset();
        ws_.reset();

        if (!should_reconnect_) return;

        // Exponential backoff
        auto delay = reconnect_backoff_;
        reconnect_backoff_ = std::min(
            std::chrono::duration_cast<std::chrono::seconds>(reconnect_backoff_ * 2),
            std::chrono::seconds(16));

        std::this_thread::sleep_for(delay);

        if (should_reconnect_) do_connect();
    }

    void do_read() {
        ws_->async_read(read_buffer_,
            [this](beast::error_code ec, std::size_t) {
                if (!ec) {
                    auto text = beast::buffers_to_string(read_buffer_.data());
                    read_buffer_.consume(read_buffer_.size());
                    if (on_message_) on_message_(text);
                    do_read();
                } else {
                    connected_ = false;
                    schedule_reconnect();
                }
            });
    }

    void do_write() {
        if (send_queue_.empty()) {
            writing_ = false;
            return;
        }
        writing_ = true;
        auto& msg = send_queue_.front();
        ws_->async_write(asio::buffer(msg),
            [this](beast::error_code ec, std::size_t) {
                if (!ec) {
                    send_queue_.pop_front();
                    do_write();
                } else {
                    // Reconnect will handle
                }
            });
    }
};
