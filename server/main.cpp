#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <sqlite3.h>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <deque>
#include <string>
#include <chrono>
#include <csignal>
#include <thread>
#include <algorithm>

#include "shared/protocol.hpp"
#include "shared/message.hpp"

namespace asio   = boost::asio;
namespace beast  = boost::beast;
namespace ws     = beast::websocket;
using tcp        = asio::ip::tcp;

// ============================================================
// Persistent storage via SQLite
// ============================================================
class MessageStore {
public:
    MessageStore(const std::string& db_path) {
        int rc = sqlite3_open(db_path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            std::cerr << "[store] Failed to open DB: " << sqlite3_errmsg(db_) << "\n";
            db_ = nullptr;
            return;
        }
        init_db();
    }

    ~MessageStore() {
        if (db_) sqlite3_close(db_);
    }

    void save_message(const lan_chat::Message& msg) {
        if (!db_) return;
        const char* sql =
            "INSERT OR IGNORE INTO messages (msg_id, type, timestamp, sender, recipient, content) "
            "VALUES (?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, msg.id.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt,  2, static_cast<int>(msg.type));
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(msg.timestamp));
        sqlite3_bind_text(stmt, 4, msg.from.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, msg.to.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 6, msg.content.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Retrieve recent messages involving the given user
    std::vector<lan_chat::Message> get_recent(const std::string& username, int limit = 100) {
        std::vector<lan_chat::Message> result;
        if (!db_) return result;

        const char* sql =
            "SELECT msg_id, type, timestamp, sender, recipient, content "
            "FROM messages "
            "WHERE recipient = '' OR recipient = ? OR sender = ? "
            "ORDER BY timestamp DESC LIMIT ?;";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt,  3, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            lan_chat::Message msg;
            msg.id        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            msg.type      = static_cast<lan_chat::MsgType>(sqlite3_column_int(stmt, 1));
            msg.timestamp = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
            msg.from      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            msg.to        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            msg.content   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            result.push_back(msg);
        }
        sqlite3_finalize(stmt);
        // Reverse to get chronological order
        std::reverse(result.begin(), result.end());
        return result;
    }

private:
    sqlite3* db_ = nullptr;

    void init_db() {
        const char* sql =
            "CREATE TABLE IF NOT EXISTS messages ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  msg_id TEXT UNIQUE NOT NULL,"
            "  type INTEGER NOT NULL,"
            "  timestamp INTEGER NOT NULL,"
            "  sender TEXT NOT NULL,"
            "  recipient TEXT NOT NULL DEFAULT '',"
            "  content TEXT NOT NULL DEFAULT ''"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_msg_recipient ON messages(recipient, timestamp);"
            "CREATE INDEX IF NOT EXISTS idx_msg_sender ON messages(sender, timestamp);";
        char* err = nullptr;
        sqlite3_exec(db_, sql, nullptr, nullptr, &err);
        if (err) {
            std::cerr << "[store] SQL error: " << err << "\n";
            sqlite3_free(err);
        }
    }
};

// ============================================================
// Forward declarations
// ============================================================
class ChatSession;

// ============================================================
// Shared server state
// ============================================================
struct ServerState {
    std::unordered_map<std::string, std::shared_ptr<ChatSession>> sessions;
    MessageStore store;
    asio::io_context& ioc;

    ServerState(asio::io_context& ioc, const std::string& db_path)
        : store(db_path), ioc(ioc) {}

    void broadcast_user_status(const std::string& username, lan_chat::MsgType type);
    void send_user_list(std::shared_ptr<ChatSession> session);
    void remove_session(const std::string& username) {
        sessions.erase(username);
    }
    void send_offline_messages(std::shared_ptr<ChatSession> session);
};

// ============================================================
// A single WebSocket session
// ============================================================
class ChatSession : public std::enable_shared_from_this<ChatSession> {
public:
    ChatSession(tcp::socket&& socket, std::shared_ptr<ServerState> state)
        : ws_(std::move(socket))
        , state_(std::move(state))
        , heartbeat_timer_(state_->ioc)
    {}

    void run() {
        ws_.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));
        ws_.set_option(ws::stream_base::decorator(
            [](ws::response_type& res) {
                res.set(beast::http::field::server, "lan-chat/1.0");
            }));
        ws_.binary(false); // Text mode (JSON)

        ws_.async_accept([self = shared_from_this()](beast::error_code ec) {
            if (!ec) {
                self->reset_heartbeat();
                self->do_read();
            } else {
                std::cerr << "[session] Accept failed: " << ec.message() << "\n";
            }
        });
    }

    void queue_message(const std::string& json_str) {
        // Thread-safe: post to strand via asio::post
        asio::post(state_->ioc,
            [self = shared_from_this(), json_str]() {
                self->send_queue_.push_back(json_str);
                if (self->send_queue_.size() == 1) {
                    self->do_write();
                }
            });
    }

    const std::string& username() const { return username_; }

private:
    ws::stream<beast::tcp_stream> ws_;
    std::shared_ptr<ServerState> state_;
    std::string username_;
    beast::flat_buffer read_buffer_;
    asio::steady_timer heartbeat_timer_;
    std::deque<std::string> send_queue_;
    bool writing_ = false;

    void reset_heartbeat() {
        heartbeat_timer_.expires_after(std::chrono::seconds(30));
        heartbeat_timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (!ec) {
                std::cerr << "[session] Heartbeat timeout for " << self->username_ << "\n";
                self->ws_.close(ws::close_code::normal);
            }
        });
    }

    void do_read() {
        ws_.async_read(read_buffer_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (!ec) {
                    self->reset_heartbeat();
                    auto text = beast::buffers_to_string(self->read_buffer_.data());
                    self->read_buffer_.consume(self->read_buffer_.size());
                    self->handle_message(std::move(text));
                    self->do_read();
                } else {
                    self->on_disconnect();
                }
            });
    }

    void handle_message(std::string text) {
        try {
            auto msg = lan_chat::parse_message(text);
            switch (msg.type) {
                case lan_chat::MsgType::Login:
                    handle_login(msg);
                    break;
                case lan_chat::MsgType::Ping:
                    handle_ping();
                    break;
                case lan_chat::MsgType::ChatMessage:
                    handle_chat(msg);
                    break;
                case lan_chat::MsgType::ReadReceipt:
                    handle_read_receipt(msg);
                    break;
                default:
                    break;
            }
        } catch (const std::exception& e) {
            std::cerr << "[session] Parse error: " << e.what() << "\n";
            send_error("Invalid message format");
        }
    }

    void handle_login(lan_chat::Message& msg) {
        auto username = msg.from;
        if (username.empty()) {
            send_error("Username cannot be empty");
            return;
        }

        // Check for duplicate
        if (state_->sessions.count(username)) {
            auto ack = lan_chat::make_ack(msg.id, lan_chat::StatusCode::DuplicateUser,
                                           "User already online");
            queue_message(lan_chat::serialize_message(ack));
            return;
        }

        username_ = username;
        state_->sessions[username] = shared_from_this();

        // Send ack
        auto ack = lan_chat::make_ack(msg.id, lan_chat::StatusCode::Ok);
        queue_message(lan_chat::serialize_message(ack));

        // Send user list
        state_->send_user_list(shared_from_this());

        // Send offline messages
        state_->send_offline_messages(shared_from_this());

        // Broadcast online status
        state_->broadcast_user_status(username_, lan_chat::MsgType::UserOnline);

        std::cout << "[server] " << username_ << " connected ("
                  << state_->sessions.size() << " online)\n";
    }

    void handle_ping() {
        lan_chat::Message pong;
        pong.type = lan_chat::MsgType::Pong;
        pong.timestamp = lan_chat::now_ms();
        queue_message(lan_chat::serialize_message(pong));
    }

    void handle_chat(lan_chat::Message& msg) {
        if (username_.empty()) return;

        // Ensure sender is correct
        msg.from = username_;

        // Persist
        state_->store.save_message(msg);

        // Send ack to sender
        auto ack = lan_chat::make_ack(msg.id, lan_chat::StatusCode::Ok);
        queue_message(lan_chat::serialize_message(ack));

        // Route
        auto json = lan_chat::serialize_message(msg);
        if (msg.to.empty()) {
            // Broadcast
            for (auto& [name, session] : state_->sessions) {
                if (name != username_) {
                    session->queue_message(json);
                }
            }
        } else {
            // Unicast
            auto it = state_->sessions.find(msg.to);
            if (it != state_->sessions.end()) {
                it->second->queue_message(json);
            }
            // If offline, message is already persisted — will be delivered on next login
        }
    }

    void handle_read_receipt(lan_chat::Message& msg) {
        // Forward read receipt to original sender
        auto it = state_->sessions.find(msg.to);
        if (it != state_->sessions.end()) {
            auto json = lan_chat::serialize_message(msg);
            it->second->queue_message(json);
        }
    }

    void send_error(const std::string& detail) {
        auto err = lan_chat::make_ack("", lan_chat::StatusCode::Error, detail);
        queue_message(lan_chat::serialize_message(err));
    }

    void do_write() {
        if (send_queue_.empty()) {
            writing_ = false;
            return;
        }
        writing_ = true;
        auto& msg = send_queue_.front();
        ws_.async_write(asio::buffer(msg),
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (!ec) {
                    self->send_queue_.pop_front();
                    self->do_write();
                } else {
                    self->on_disconnect();
                }
            });
    }

    void on_disconnect() {
        heartbeat_timer_.cancel();
        if (!username_.empty()) {
            state_->remove_session(username_);
            state_->broadcast_user_status(username_, lan_chat::MsgType::UserOffline);
            std::cout << "[server] " << username_ << " disconnected ("
                      << state_->sessions.size() << " online)\n";
        }
    }
};

// ============================================================
// ServerState methods that need full ChatSession definition
// ============================================================
void ServerState::broadcast_user_status(const std::string& username, lan_chat::MsgType type) {
    lan_chat::Message msg;
    msg.type = type;
    msg.id = lan_chat::generate_msg_id();
    msg.timestamp = lan_chat::now_ms();
    msg.from = username;
    auto json = lan_chat::serialize_message(msg);
    for (auto& [name, session] : sessions) {
        if (name != username) {
            session->queue_message(json);
        }
    }
}

void ServerState::send_user_list(std::shared_ptr<ChatSession> session) {
    std::vector<std::string> users;
    for (auto& [name, _] : sessions) {
        users.push_back(name);
    }
    lan_chat::Message msg;
    msg.type = lan_chat::MsgType::UserList;
    msg.id = lan_chat::generate_msg_id();
    msg.timestamp = lan_chat::now_ms();
    msg.content = nlohmann::json(users).dump();
    session->queue_message(lan_chat::serialize_message(msg));
}

void ServerState::send_offline_messages(std::shared_ptr<ChatSession> session) {
    auto msgs = store.get_recent(session->username(), 100);
    for (auto& m : msgs) {
        session->queue_message(lan_chat::serialize_message(m));
    }
}

// ============================================================
// Listener — accepts incoming TCP connections
// ============================================================
class Listener : public std::enable_shared_from_this<Listener> {
public:
    Listener(asio::io_context& ioc, uint16_t port,
             std::shared_ptr<ServerState> state)
        : acceptor_(ioc, tcp::endpoint(tcp::v4(), port))
        , state_(std::move(state))
    {}

    void run() { do_accept(); }

private:
    tcp::acceptor acceptor_;
    std::shared_ptr<ServerState> state_;

    void do_accept() {
        acceptor_.async_accept(
            [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<ChatSession>(std::move(socket), self->state_)->run();
                }
                self->do_accept();
            });
    }
};

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[]) {
    uint16_t port = 9833;
    std::string db_path = "/var/lib/lan-chat/messages.db";

    // Parse CLI args
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--db" && i + 1 < argc) {
            db_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "lan-chat-server [--port PORT] [--db PATH]\n";
            std::cout << "  --port  Listen port (default: 9833)\n";
            std::cout << "  --db    SQLite database path\n";
            return 0;
        }
    }

    // Ensure DB directory exists
    if (auto pos = db_path.find_last_of('/'); pos != std::string::npos) {
        std::string dir = db_path.substr(0, pos);
        mkdir(dir.c_str(), 0755);
    }

    std::cout << "[server] Starting LAN Chat Server on port " << port << "\n";
    std::cout << "[server] Database: " << db_path << "\n";

    asio::io_context ioc;
    auto state = std::make_shared<ServerState>(ioc, db_path);
    auto listener = std::make_shared<Listener>(ioc, port, state);
    listener->run();

    // Signal handling
    asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&ioc](beast::error_code, int sig) {
        std::cout << "\n[server] Shutting down...\n";
        ioc.stop();
    });

    ioc.run();
    std::cout << "[server] Goodbye.\n";
    return 0;
}
