#pragma once
#include "protocol.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <chrono>
#include <sstream>
#include <random>

namespace lan_chat {

struct Message {
    MsgType     type = MsgType::ChatMessage;
    std::string id;
    uint64_t    timestamp = 0;
    std::string from;          // sender username
    std::string to;            // recipient (empty=broadcast)
    std::string content;       // message body
    int         status = 0;    // 0=ok, >0=error
    int         tcp_port = 0;  // used in Hello messages

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Message, type, id, timestamp, from, to, content, status, tcp_port)
};

// Generate unique message ID
inline std::string generate_msg_id() {
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    static thread_local std::uniform_int_distribution<uint64_t> dis;
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    std::ostringstream oss;
    oss << std::hex << ms << "-" << dis(gen);
    return oss.str();
}

inline uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

inline Message make_hello(const std::string& username, int tcp_port) {
    Message m;
    m.type = MsgType::Hello;
    m.id = generate_msg_id();
    m.timestamp = now_ms();
    m.from = username;
    m.tcp_port = tcp_port;
    return m;
}

inline Message make_goodbye(const std::string& username) {
    Message m;
    m.type = MsgType::Goodbye;
    m.id = generate_msg_id();
    m.timestamp = now_ms();
    m.from = username;
    return m;
}

inline Message make_chat(const std::string& from, const std::string& to,
                          const std::string& content) {
    Message m;
    m.type = MsgType::ChatMessage;
    m.id = generate_msg_id();
    m.timestamp = now_ms();
    m.from = from;
    m.to = to;
    m.content = content;
    return m;
}

inline Message make_ack(const std::string& msg_id, int status = 0) {
    Message m;
    m.type = MsgType::MessageAck;
    m.id = msg_id;
    m.timestamp = now_ms();
    m.status = status;
    return m;
}

inline std::string serialize(const Message& m) {
    return nlohmann::json(m).dump();
}

inline Message parse(const std::string& s) {
    return nlohmann::json::parse(s).get<Message>();
}

} // namespace lan_chat
