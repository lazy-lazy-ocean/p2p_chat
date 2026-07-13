#pragma once
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

namespace lan_chat {

enum class MsgType : uint8_t {
    // Chat messages (over TCP)
    ChatMessage  = 1,
    MessageAck   = 2,
    ReadReceipt  = 3,

    // UDP discovery
    Hello        = 10,   // "I'm online" broadcast
    Goodbye      = 11,   // "I'm leaving" broadcast
};

NLOHMANN_JSON_SERIALIZE_ENUM(MsgType, {
    {MsgType::ChatMessage,  "chat_message"},
    {MsgType::MessageAck,   "message_ack"},
    {MsgType::ReadReceipt,  "read_receipt"},
    {MsgType::Hello,        "hello"},
    {MsgType::Goodbye,      "goodbye"},
})

} // namespace lan_chat
