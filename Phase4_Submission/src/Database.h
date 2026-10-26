// DATA ACCESS LAYER (part 1): the only place that talks to SQLite.
// Every value is bound as a parameter, never pasted into the SQL text (NFR7).
#pragma once
#include <sqlite3.h>

#include <string>
#include <variant>
#include <vector>

#include "Domain.h"

using Value = std::variant<std::monostate, long long, std::string>;   // NULL, integer, text
inline Value null() { return std::monostate{}; }

struct Row {
    std::vector<Value> cells;
    bool isNull(std::size_t i) const { return std::holds_alternative<std::monostate>(cells.at(i)); }
    long long num(std::size_t i) const { return isNull(i) ? 0 : std::get<long long>(cells.at(i)); }
    std::string str(std::size_t i) const { return isNull(i) ? "" : std::get<std::string>(cells.at(i)); }
};

class Database {
public:
    explicit Database(const std::string& path);      // ":memory:" works too (used by tests)
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    std::vector<Row> query(const std::string& sql, const std::vector<Value>& params = {});
    long long execute(const std::string& sql, const std::vector<Value>& params = {});  // returns rows changed
    long long lastInsertId() const { return sqlite3_last_insert_rowid(db_); }
    void backupTo(const std::string& path);          // SQLite online backup (consistent copy)
private:
    sqlite3* db_ = nullptr;
};

// RAII transaction: rolls back in the destructor unless commit() was called.
class Transaction {
public:
    explicit Transaction(Database& db) : db_(db) { db_.execute("BEGIN IMMEDIATE"); }
    ~Transaction() { if (!done_) { try { db_.execute("ROLLBACK"); } catch (...) {} } }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit() { db_.execute("COMMIT"); done_ = true; }
private:
    Database& db_;
    bool done_ = false;
};
