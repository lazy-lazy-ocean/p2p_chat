#pragma once
#include <sqlite3.h>
#include <string>
#include <vector>
#include <memory>
#include <iostream>

struct StoredMessage {
    std::string msg_id;
    std::string sender;
    std::string recipient;
    std::string content;
    uint64_t timestamp = 0;
    int is_read = 0;
};

class LocalStorage {
public:
    explicit LocalStorage(const std::string& path) {
        int rc = sqlite3_open(path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            std::cerr << "[localdb] Open failed: " << sqlite3_errmsg(db_) << "\n";
            db_ = nullptr;
            return;
        }
        init();
    }

    ~LocalStorage() { if (db_) sqlite3_close(db_); }

    void save_message(const StoredMessage& msg) {
        if (!db_) return;
        const char* sql =
            "INSERT OR REPLACE INTO messages (msg_id, sender, recipient, content, timestamp, is_read) "
            "VALUES (?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, msg.msg_id.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, msg.sender.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, msg.recipient.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, msg.content.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(msg.timestamp));
        sqlite3_bind_int(stmt, 6, msg.is_read);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    std::vector<StoredMessage> get_messages(const std::string& contact, int limit = 200) {
        std::vector<StoredMessage> result;
        if (!db_) return result;
        const char* sql =
            "SELECT msg_id, sender, recipient, content, timestamp, is_read "
            "FROM messages WHERE sender = ? OR recipient = ? "
            "ORDER BY timestamp ASC LIMIT ?;";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, contact.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, contact.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            StoredMessage m;
            m.msg_id    = rstr(sqlite3_column_text(stmt, 0));
            m.sender    = rstr(sqlite3_column_text(stmt, 1));
            m.recipient = rstr(sqlite3_column_text(stmt, 2));
            m.content   = rstr(sqlite3_column_text(stmt, 3));
            m.timestamp = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
            m.is_read   = sqlite3_column_int(stmt, 5);
            result.push_back(m);
        }
        sqlite3_finalize(stmt);
        return result;
    }

    void mark_read(const std::string& sender) {
        if (!db_) return;
        const char* sql = "UPDATE messages SET is_read = 1 WHERE sender = ? AND is_read = 0;";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, sender.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    int unread_count(const std::string& sender) {
        if (!db_) return 0;
        const char* sql = "SELECT COUNT(*) FROM messages WHERE sender = ? AND is_read = 0;";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, sender.c_str(), -1, SQLITE_STATIC);
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return count;
    }

private:
    sqlite3* db_ = nullptr;

    static std::string rstr(const unsigned char* s) {
        return s ? std::string(reinterpret_cast<const char*>(s)) : std::string();
    }

    void init() {
        const char* sql =
            "CREATE TABLE IF NOT EXISTS messages ("
            "  msg_id TEXT PRIMARY KEY,"
            "  sender TEXT NOT NULL,"
            "  recipient TEXT NOT NULL,"
            "  content TEXT NOT NULL,"
            "  timestamp INTEGER NOT NULL,"
            "  is_read INTEGER DEFAULT 0"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_msg_contact ON messages(sender, recipient, timestamp);";
        char* err = nullptr;
        sqlite3_exec(db_, sql, nullptr, nullptr, &err);
        if (err) { std::cerr << "[localdb] " << err << "\n"; sqlite3_free(err); }
    }
};
