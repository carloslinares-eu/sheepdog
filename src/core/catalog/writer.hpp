#pragma once

// The DB-writer thread. It owns the catalog's read-write connection; producers (the scanner,
// later the organiser) queue work and never touch SQLite themselves. Queued work is committed
// in transactions of up to WriterOptions::batch_size items. A batch is committed when it is
// full, when flush() asks for it, or max_delay after its first item arrived, so a trickle of
// work is neither held back nor committed row by row. Producers block while the queue holds
// queue_capacity items. No exceptions leave the thread: the first error is kept and
// returned by flush().

#include "core/catalog/sqlite.hpp"
#include "core/platform/fs.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>

namespace sheepdog::catalog {

// One file as the scanner saw it. Inserted, or updated in place when (root_id, rel_path)
// exists; a changed size or mtime clears quick_fp, blake3 and meta_ver.
struct FileRow {
    std::int64_t root_id = 0;
    std::string rel_path; // native bytes relative to the root
    std::string display;  // UTF-8
    std::string name_key; // NFC + case folding of the name
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::int64_t btime_ns = 0;
    std::string fs_id; // FileId::file bytes, empty when unknown
    platform::EntryKind kind = platform::EntryKind::file;
    std::int64_t last_seen = 0; // ns since 1970 UTC
};

struct WriterOptions {
    std::size_t batch_size = 1000;
    std::size_t queue_capacity = 8000;
    std::chrono::milliseconds max_delay{50};
};

struct WriterStats {
    std::uint64_t committed = 0;    // items in committed transactions
    std::uint64_t transactions = 0; // committed transactions
    std::uint64_t failed = 0;       // items in rolled-back transactions
};

class CatalogWriter {
public:
    // Runs on the writer thread inside the current batch's transaction; must not BEGIN/COMMIT.
    using Op = std::function<Result<void>(Database&)>;

    // Takes over `db` (from open_catalog()) and starts the thread.
    explicit CatalogWriter(Database db, WriterOptions options = {});
    CatalogWriter(const CatalogWriter&) = delete;
    CatalogWriter& operator=(const CatalogWriter&) = delete;
    // Commits everything still queued, then stops the thread and closes the connection.
    ~CatalogWriter();

    void upsert_file(FileRow row);
    void execute(Op op);

    // Waits until everything queued so far is committed (or rolled back). Returns the first
    // error since the previous flush, if any, and clears it.
    [[nodiscard]] Result<void> flush();

    [[nodiscard]] WriterStats stats() const;

private:
    using Item = std::variant<FileRow, Op>;

    void push(Item item);
    void run();
    Result<void> write_batch(std::deque<Item>& batch);
    Result<void> write_file(const FileRow& row);

    Database db_;
    std::optional<Statement> upsert_; // prepared on the writer thread
    WriterOptions options_;

    mutable std::mutex mutex_;
    std::condition_variable work_ready_; // queue not empty, or stopping
    std::condition_variable space_free_; // queue below capacity
    std::condition_variable idle_;       // queue empty and nothing in flight
    std::deque<Item> queue_;
    std::size_t in_flight_ = 0;
    std::size_t flush_waiters_ = 0; // flush() calls waiting: commit without delay
    bool stopping_ = false;
    std::optional<Error> first_error_;
    WriterStats stats_;

    std::jthread thread_; // last: started after everything above exists
};

} // namespace sheepdog::catalog
