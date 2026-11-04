#pragma once

// A thin RAII wrapper over the SQLite C API: Database owns a connection, Statement a prepared
// statement, Transaction a BEGIN ... COMMIT that rolls back unless committed. Errors come back
// as Result<T>: the code is SQLite's extended result code in sqlite_category(), the path is the
// database file and the detail is sqlite3_errmsg(). No exceptions.
//
// A connection may be used by one thread at a time; it can be moved to another thread (the
// writer does this).

#include "core/platform/native_path.hpp"
#include "core/platform/result.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

struct sqlite3;
struct sqlite3_stmt;

namespace sheepdog::catalog {

using platform::Error;
using platform::NativePath;
using platform::Ok;
using platform::Result;

// The category of SQLite result codes (value = extended result code, e.g. SQLITE_CORRUPT).
[[nodiscard]] const std::error_category& sqlite_category() noexcept;
[[nodiscard]] std::error_code sqlite_error_code(int rc) noexcept;

class Statement {
public:
    Statement() = default;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement();

    // Parameters are 1-based. A failed bind is remembered and returned by the next step().
    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, std::string_view text);
    Statement& bind_blob(int index, std::span<const std::byte> bytes);
    Statement& bind_blob(int index, std::string_view bytes); // native path bytes, ids
    Statement& bind_null(int index);

    // true: a row is ready; false: the statement has finished.
    [[nodiscard]] Result<bool> step();
    // Steps until done (for statements that return no rows), then resets.
    [[nodiscard]] Result<void> run();
    // Ready to run again; bindings are kept until rebound.
    void reset() noexcept;

    // Columns are 0-based and valid after step() returned true.
    [[nodiscard]] bool is_null(int column) const noexcept;
    [[nodiscard]] std::int64_t column_int64(int column) const noexcept;
    [[nodiscard]] std::string column_text(int column) const;
    [[nodiscard]] std::string column_blob(int column) const;

    [[nodiscard]] sqlite3_stmt* handle() const noexcept { return stmt_; }

private:
    friend class Database;
    Statement(sqlite3_stmt* stmt, const NativePath* path) : stmt_(stmt), path_(path) {}
    void note_bind(int rc) noexcept;
    void finalize() noexcept;

    sqlite3_stmt* stmt_ = nullptr;
    const NativePath* path_ = nullptr; // the owning Database's path, for errors
    int bind_rc_ = 0;                  // SQLITE_OK
};

enum class OpenMode : std::uint8_t {
    read_only,  // SQLITE_OPEN_READONLY
    read_write, // the file must exist
    create,     // read-write, created if missing
};

class Database {
public:
    // Opens `path` (native bytes; SQLite takes them as its UTF-8 file name). Extended result
    // codes are turned on and a busy timeout of 5 s is set.
    [[nodiscard]] static Result<Database> open(const NativePath& path, OpenMode mode);

    Database() = default;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database();

    // Runs one or more statements that return no rows.
    [[nodiscard]] Result<void> exec(std::string_view sql);
    [[nodiscard]] Result<Statement> prepare(std::string_view sql);
    // The first column of the first row, e.g. "PRAGMA user_version".
    [[nodiscard]] Result<std::int64_t> query_int(std::string_view sql);
    [[nodiscard]] Result<std::string> query_text(std::string_view sql);

    [[nodiscard]] std::int64_t last_insert_rowid() const noexcept;
    [[nodiscard]] bool is_open() const noexcept { return db_ != nullptr; }
    [[nodiscard]] const NativePath& path() const noexcept { return *path_; }
    [[nodiscard]] sqlite3* handle() const noexcept { return db_; }

    // An Error for result code `rc`, with this connection's message as the detail.
    [[nodiscard]] Error error(int rc) const;

private:
    Database(sqlite3* db, NativePath path);
    void close() noexcept;

    sqlite3* db_ = nullptr;
    // Heap-allocated so that Statements keep a valid pointer when the Database moves.
    std::unique_ptr<NativePath> path_ = std::make_unique<NativePath>();
};

// BEGIN (or BEGIN IMMEDIATE) on construction through begin(); rolls back in the destructor
// unless commit() succeeded.
class Transaction {
public:
    enum class Kind : std::uint8_t { deferred, immediate };

    [[nodiscard]] static Result<Transaction> begin(Database& db, Kind kind = Kind::immediate);

    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&&) = delete;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();

    [[nodiscard]] Result<void> commit();

private:
    explicit Transaction(Database* db) : db_(db) {}
    Database* db_ = nullptr; // null once committed or rolled back
};

} // namespace sheepdog::catalog
