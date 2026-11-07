#pragma once

// Jobs: long-running work (a scan, a hash pass, an organise batch), each on a std::jthread of its
// own. A job reports progress, collects errors and hands work to the shared pool through its
// JobContext. It checks for a stop per item, and per chunk with read_chunks().
//
// Concrete jobs live with their subject (scanner, executor); this is the framework around them.

#include "core/jobs/clock.hpp"
#include "core/jobs/errors.hpp"
#include "core/jobs/pool.hpp"
#include "core/jobs/progress.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace sheepdog::jobs {

enum class JobKind : std::uint8_t {
    read_only,     // reads files, writes at most to the catalog; any number run side by side
    changes_files, // only one at a time, across the whole process
};

// What a job body works with. Its accessors may be called from the job's pool tasks as well.
class JobContext {
public:
    JobContext(std::stop_token stop, Progress& progress, ErrorLog& errors, BoundedPool& pool);

    [[nodiscard]] const std::stop_token& stop_token() const noexcept { return stop_; }
    [[nodiscard]] bool stop_requested() const noexcept { return stop_.stop_requested(); }
    [[nodiscard]] Progress& progress() const noexcept { return progress_; }
    // For errors the job survives, such as one unreadable file.
    [[nodiscard]] ErrorLog& errors() const noexcept { return errors_; }
    // A group of tasks for the shared pool, see TaskGroup: `auto tasks = context.tasks();`
    [[nodiscard]] TaskGroup tasks() const { return {pool_, stop_, errors_}; }

private:
    std::stop_token stop_;
    Progress& progress_;
    ErrorLog& errors_;
    BoundedPool& pool_;
};

// The job's work, on the job's thread. An error return or an exception ends the job as failed.
using JobBody = std::function<Result<void>(JobContext&)>;

// Held by the one job that changes files. It covers the whole process, so a second JobRunner
// cannot get around it; another Sheepdog process is not covered.
class FileChangeLock {
public:
    FileChangeLock() = default; // holds nothing
    FileChangeLock(const FileChangeLock&) = delete;
    FileChangeLock& operator=(const FileChangeLock&) = delete;
    FileChangeLock(FileChangeLock&& other) noexcept;
    FileChangeLock& operator=(FileChangeLock&& other) noexcept;
    ~FileChangeLock();

    // Empty while another job holds the lock.
    [[nodiscard]] static std::optional<FileChangeLock> try_acquire() noexcept;
    void release() noexcept;

private:
    bool held_ = false;
};

class Job {
    struct Key {
        explicit Key() = default;
    };

public:
    // JobRunner::start() makes these.
    Job(Key key, std::string name, JobKind kind, const Clock& clock, BoundedPool& pool,
        FileChangeLock lock, JobBody body);
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    ~Job() = default;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] JobKind kind() const noexcept { return kind_; }

    // Poll this; its state says when the job has ended. When it no longer says running, the
    // job's tasks are done and the job has let go of the file-change lock.
    [[nodiscard]] std::shared_ptr<const Snapshot> snapshot() const { return progress_.snapshot(); }

    // What went wrong so far: what the job and its tasks reported, and the error that ended it.
    [[nodiscard]] const ErrorLog& errors() const noexcept { return errors_; }

    // Asks the job to stop. It ends once it notices; this does not wait for that.
    void request_stop() noexcept { thread_.request_stop(); }

    // Blocks until the job has ended and says how: Ok for completed, a JobErrc::cancelled error
    // for a stop, else the error that ended it. Not to be called from the job's own thread.
    [[nodiscard]] Result<void> wait() const;

private:
    friend class JobRunner;

    void run(const std::stop_token& stop, JobBody body);
    [[nodiscard]] bool finished() const;

    std::string name_;
    JobKind kind_;
    Progress progress_;
    ErrorLog errors_;
    BoundedPool& pool_;
    FileChangeLock lock_;

    mutable std::mutex mutex_;
    mutable std::condition_variable end_seen_;
    bool ended_ = false;
    Result<void> result_ = Ok{}; // how it ended, once ended_ is set

    std::jthread thread_; // last: it uses everything above
};

// Starts jobs and owns the pool they share. One per application is enough. The clock must outlive
// the runner.
class JobRunner {
public:
    explicit JobRunner(const Clock& clock = system_clock(),
                       std::size_t workers = default_worker_count());
    JobRunner(const JobRunner&) = delete;
    JobRunner& operator=(const JobRunner&) = delete;
    // Stops the jobs still running and waits for them.
    ~JobRunner();

    // Starts `body` on a thread of its own. A job that changes files is refused with
    // JobErrc::busy while another one runs, rather than queued: its plan was checked against the
    // disk as it is now, and a double click must not run a batch twice. The name is for display.
    // While the runner is being destroyed (a job starting a job) the answer is JobErrc::cancelled.
    [[nodiscard]] Result<std::shared_ptr<Job>> start(std::string name, JobKind kind, JobBody body);

    [[nodiscard]] BoundedPool& pool() noexcept { return pool_; }

private:
    const Clock& clock_;
    BoundedPool pool_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<Job>> jobs_; // finished ones go at the next start()
    bool closing_ = false;                   // the destructor has begun: no more jobs
};

} // namespace sheepdog::jobs
