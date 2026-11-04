#include "core/catalog/sqlite.hpp"

#include <sqlite3.h>

#include <climits>
#include <utility>

namespace sheepdog::catalog {

namespace {

class SqliteCategory final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "sqlite"; }
    [[nodiscard]] std::string message(int rc) const override { return sqlite3_errstr(rc); }
};

int clamp_size(std::size_t n) noexcept {
    return n > static_cast<std::size_t>(INT_MAX) ? INT_MAX : static_cast<int>(n);
}

} // namespace

const std::error_category& sqlite_category() noexcept {
    static const SqliteCategory category;
    return category;
}

std::error_code sqlite_error_code(int rc) noexcept {
    return {rc, sqlite_category()};
}

// --- Statement ------------------------------------------------------------------------------

Statement::Statement(Statement&& other) noexcept
    : stmt_(std::exchange(other.stmt_, nullptr)), path_(other.path_),
      bind_rc_(std::exchange(other.bind_rc_, SQLITE_OK)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        finalize();
        stmt_ = std::exchange(other.stmt_, nullptr);
        path_ = other.path_;
        bind_rc_ = std::exchange(other.bind_rc_, SQLITE_OK);
    }
    return *this;
}

Statement::~Statement() {
    finalize();
}

void Statement::finalize() noexcept {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

void Statement::note_bind(int rc) noexcept {
    if (rc != SQLITE_OK && bind_rc_ == SQLITE_OK) {
        bind_rc_ = rc;
    }
}

Statement& Statement::bind(int index, std::int64_t value) {
    note_bind(sqlite3_bind_int64(stmt_, index, value));
    return *this;
}

Statement& Statement::bind(int index, std::string_view text) {
    note_bind(
        sqlite3_bind_text64(stmt_, index, text.data(), text.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
    return *this;
}

Statement& Statement::bind_blob(int index, std::span<const std::byte> bytes) {
    // A zero-length BLOB, not NULL: sqlite3_bind_blob64 with a null pointer would bind NULL.
    static constexpr char kEmpty = 0;
    const void* data = bytes.empty() ? static_cast<const void*>(&kEmpty) : bytes.data();
    note_bind(sqlite3_bind_blob64(stmt_, index, data, bytes.size(), SQLITE_TRANSIENT));
    return *this;
}

Statement& Statement::bind_blob(int index, std::string_view bytes) {
    return bind_blob(index, std::as_bytes(std::span(bytes.data(), bytes.size())));
}

Statement& Statement::bind_null(int index) {
    note_bind(sqlite3_bind_null(stmt_, index));
    return *this;
}

Result<bool> Statement::step() {
    sqlite3* db = sqlite3_db_handle(stmt_);
    if (bind_rc_ != SQLITE_OK) {
        const int rc = std::exchange(bind_rc_, SQLITE_OK);
        return Error{.code = sqlite_error_code(rc), .path = *path_, .detail = "bind failed"};
    }
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    Error error{.code = sqlite_error_code(rc), .path = *path_, .detail = sqlite3_errmsg(db)};
    sqlite3_reset(stmt_);
    return error;
}

Result<void> Statement::run() {
    for (;;) {
        auto more = step();
        if (!more) {
            return more.error();
        }
        if (!*more) {
            break;
        }
    }
    reset();
    return Ok{};
}

void Statement::reset() noexcept {
    sqlite3_reset(stmt_);
}

bool Statement::is_null(int column) const noexcept {
    return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
}

std::int64_t Statement::column_int64(int column) const noexcept {
    return sqlite3_column_int64(stmt_, column);
}

std::string Statement::column_text(int column) const {
    const auto* text = sqlite3_column_text(stmt_, column);
    const int size = sqlite3_column_bytes(stmt_, column);
    return text == nullptr
               ? std::string()
               : std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(size));
}

std::string Statement::column_blob(int column) const {
    const void* blob = sqlite3_column_blob(stmt_, column);
    const int size = sqlite3_column_bytes(stmt_, column);
    return blob == nullptr
               ? std::string()
               : std::string(static_cast<const char*>(blob), static_cast<std::size_t>(size));
}

// --- Database -------------------------------------------------------------------------------

Database::Database(sqlite3* db, NativePath path)
    : db_(db), path_(std::make_unique<NativePath>(std::move(path))) {}

Database::Database(Database&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), path_(std::move(other.path_)) {
    other.path_ = std::make_unique<NativePath>();
}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        close();
        db_ = std::exchange(other.db_, nullptr);
        std::swap(path_, other.path_);
    }
    return *this;
}

Database::~Database() {
    close();
}

void Database::close() noexcept {
    if (db_ != nullptr) {
        // sqlite3_close_v2 defers the close until unfinalized statements are gone.
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

Result<Database> Database::open(const NativePath& path, OpenMode mode) {
    int flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE;
    switch (mode) {
    case OpenMode::read_only:
        flags |= SQLITE_OPEN_READONLY;
        break;
    case OpenMode::read_write:
        flags |= SQLITE_OPEN_READWRITE;
        break;
    case OpenMode::create:
        flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        break;
    }
    sqlite3* raw = nullptr;
    const int rc = sqlite3_open_v2(path.bytes().c_str(), &raw, flags, nullptr);
    if (rc != SQLITE_OK) {
        Error error{.code = sqlite_error_code(rc),
                    .path = path,
                    .detail = raw != nullptr ? sqlite3_errmsg(raw) : ""};
        sqlite3_close_v2(raw);
        return error;
    }
    sqlite3_extended_result_codes(raw, 1);
    sqlite3_busy_timeout(raw, 5000);
    return Database(raw, path);
}

Error Database::error(int rc) const {
    return Error{.code = sqlite_error_code(rc), .path = *path_, .detail = sqlite3_errmsg(db_)};
}

Result<void> Database::exec(std::string_view sql) {
    const std::string text(sql);
    char* message = nullptr;
    const int rc = sqlite3_exec(db_, text.c_str(), nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        Error error{.code = sqlite_error_code(rc),
                    .path = *path_,
                    .detail = message != nullptr ? message : sqlite3_errmsg(db_)};
        sqlite3_free(message);
        return error;
    }
    return Ok{};
}

Result<Statement> Database::prepare(std::string_view sql) {
    sqlite3_stmt* stmt = nullptr;
    // The size is passed, so the text need not be NUL-terminated.
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    const int rc = sqlite3_prepare_v3(db_, sql.data(), clamp_size(sql.size()),
                                      SQLITE_PREPARE_PERSISTENT, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return error(rc);
    }
    return Statement(stmt, path_.get());
}

Result<std::int64_t> Database::query_int(std::string_view sql) {
    auto stmt = prepare(sql);
    if (!stmt) {
        return stmt.error();
    }
    auto row = stmt->step();
    if (!row) {
        return row.error();
    }
    return *row ? stmt->column_int64(0) : std::int64_t{0};
}

Result<std::string> Database::query_text(std::string_view sql) {
    auto stmt = prepare(sql);
    if (!stmt) {
        return stmt.error();
    }
    auto row = stmt->step();
    if (!row) {
        return row.error();
    }
    return *row ? stmt->column_text(0) : std::string();
}

std::int64_t Database::last_insert_rowid() const noexcept {
    return sqlite3_last_insert_rowid(db_);
}

// --- Transaction ----------------------------------------------------------------------------

Result<Transaction> Transaction::begin(Database& db, Kind kind) {
    auto begun = db.exec(kind == Kind::immediate ? "BEGIN IMMEDIATE" : "BEGIN");
    if (!begun) {
        return begun.error();
    }
    return Transaction(&db);
}

Transaction::Transaction(Transaction&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

Transaction::~Transaction() {
    if (db_ != nullptr && sqlite3_get_autocommit(db_->handle()) == 0) {
        static_cast<void>(db_->exec("ROLLBACK"));
    }
}

Result<void> Transaction::commit() {
    auto done = db_->exec("COMMIT");
    if (done) {
        db_ = nullptr;
    }
    return done;
}

} // namespace sheepdog::catalog
