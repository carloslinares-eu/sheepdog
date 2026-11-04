#include "core/catalog/writer.hpp"

#include <algorithm>
#include <utility>

namespace sheepdog::catalog {

namespace {

constexpr std::string_view kUpsertFile = R"sql(
INSERT INTO file(root_id, rel_path, display, name_key, size, mtime_ns, btime_ns, fs_id, kind,
                 status, last_seen)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, 'present', ?10)
ON CONFLICT(root_id, rel_path) DO UPDATE SET
    display   = excluded.display,
    name_key  = excluded.name_key,
    quick_fp  = iif(size = excluded.size AND mtime_ns = excluded.mtime_ns, quick_fp, NULL),
    blake3    = iif(size = excluded.size AND mtime_ns = excluded.mtime_ns, blake3, NULL),
    meta_ver  = iif(size = excluded.size AND mtime_ns = excluded.mtime_ns, meta_ver, 0),
    size      = excluded.size,
    mtime_ns  = excluded.mtime_ns,
    btime_ns  = excluded.btime_ns,
    fs_id     = excluded.fs_id,
    kind      = excluded.kind,
    status    = 'present',
    last_seen = excluded.last_seen
)sql";

} // namespace

CatalogWriter::CatalogWriter(Database db, WriterOptions options)
    : db_(std::move(db)), options_(options) {
    options_.batch_size = std::max<std::size_t>(options_.batch_size, 1);
    options_.queue_capacity = std::max(options_.queue_capacity, options_.batch_size);
    thread_ = std::jthread([this] { run(); });
}

CatalogWriter::~CatalogWriter() {
    {
        const std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    work_ready_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    // The statement goes before the connection it belongs to.
    upsert_.reset();
}

void CatalogWriter::upsert_file(FileRow row) {
    push(std::move(row));
}

void CatalogWriter::execute(Op op) {
    push(std::move(op));
}

void CatalogWriter::push(Item item) {
    {
        std::unique_lock lock(mutex_);
        space_free_.wait(lock, [this] { return queue_.size() < options_.queue_capacity; });
        queue_.push_back(std::move(item));
        // Wake the writer for a new batch, or when the batch it waits for is full.
        if (queue_.size() != 1 && queue_.size() != options_.batch_size) {
            return;
        }
    }
    work_ready_.notify_one();
}

Result<void> CatalogWriter::flush() {
    std::unique_lock lock(mutex_);
    ++flush_waiters_;
    work_ready_.notify_one();
    idle_.wait(lock, [this] { return queue_.empty() && in_flight_ == 0; });
    --flush_waiters_;
    if (first_error_) {
        Error error = std::move(*first_error_);
        first_error_.reset();
        return error;
    }
    return Ok{};
}

WriterStats CatalogWriter::stats() const {
    const std::scoped_lock lock(mutex_);
    return stats_;
}

void CatalogWriter::run() {
    std::deque<Item> batch;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            work_ready_.wait(lock, [this] { return !queue_.empty() || stopping_; });
            if (queue_.empty()) {
                return; // stopping, and everything is written
            }
            // Let the batch fill up, but not for longer than max_delay.
            work_ready_.wait_for(lock, options_.max_delay, [this] {
                return queue_.size() >= options_.batch_size || stopping_ || flush_waiters_ > 0;
            });
            const std::size_t n = std::min(queue_.size(), options_.batch_size);
            const auto end = queue_.begin() + static_cast<std::ptrdiff_t>(n);
            batch.assign(std::make_move_iterator(queue_.begin()), std::make_move_iterator(end));
            queue_.erase(queue_.begin(), end);
            in_flight_ = n;
        }
        space_free_.notify_all();

        auto written = write_batch(batch);
        {
            const std::scoped_lock lock(mutex_);
            if (written) {
                stats_.committed += in_flight_;
                ++stats_.transactions;
            } else {
                stats_.failed += in_flight_;
                if (!first_error_) {
                    first_error_ = written.error();
                }
            }
            in_flight_ = 0;
        }
        batch.clear();
        idle_.notify_all();
    }
}

Result<void> CatalogWriter::write_batch(std::deque<Item>& batch) {
    auto tx = Transaction::begin(db_);
    if (!tx) {
        return tx.error();
    }
    for (Item& item : batch) {
        Result<void> done = Ok{};
        if (const auto* row = std::get_if<FileRow>(&item)) {
            done = write_file(*row);
        } else {
            done = std::get<Op>(item)(db_);
        }
        if (!done) {
            return done; // the Transaction rolls the whole batch back
        }
    }
    return tx->commit();
}

Result<void> CatalogWriter::write_file(const FileRow& row) {
    if (!upsert_) {
        auto stmt = db_.prepare(kUpsertFile);
        if (!stmt) {
            return stmt.error();
        }
        upsert_ = std::move(*stmt);
    }
    Statement& s = *upsert_;
    s.bind(1, row.root_id)
        .bind_blob(2, std::string_view(row.rel_path))
        .bind(3, std::string_view(row.display))
        .bind(4, std::string_view(row.name_key))
        .bind(5, static_cast<std::int64_t>(row.size))
        .bind(6, row.mtime_ns)
        .bind(7, row.btime_ns);
    if (row.fs_id.empty()) {
        s.bind_null(8);
    } else {
        s.bind_blob(8, std::string_view(row.fs_id));
    }
    s.bind(9, static_cast<std::int64_t>(row.kind)).bind(10, row.last_seen);
    return s.run();
}

} // namespace sheepdog::catalog
