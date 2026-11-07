#pragma once

// BoundedPool: the worker threads all jobs share, so that the limits below hold across jobs.
// TaskGroup: how one job hands them work and waits for it.
//
// At most workers x 4 tasks wait in the queue. A producer that outruns the workers blocks in
// submit(), so a job over a million files never holds a million tasks in memory. At most
// volume_limit() tasks of one volume run at a time; a volume at its limit holds back only its own
// tasks, which tasks of other volumes overtake. Tasks of one volume start in submission order.

#include "core/jobs/errors.hpp"
#include "core/platform/fs.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace sheepdog::jobs {

inline constexpr std::size_t kQueuePerWorker = 4;
inline constexpr std::size_t kHddLimit = 2;
inline constexpr std::size_t kSsdLimit = 8;

// How many tasks may touch one volume at a time. A volume of unknown type counts as a spinning
// disk: parallel reads ruin an HDD, but only slow down an SSD a little.
[[nodiscard]] std::size_t volume_limit(platform::Media media) noexcept;

// One worker per hardware thread.
[[nodiscard]] std::size_t default_worker_count() noexcept;

class BoundedPool {
public:
    struct Stats {
        std::size_t queued = 0;      // waiting now
        std::size_t running = 0;     // running now
        std::size_t peak_queued = 0; // the most tasks that ever waited at the same time
        std::uint64_t completed = 0; // finished so far
    };

    explicit BoundedPool(std::size_t workers = default_worker_count());
    BoundedPool(const BoundedPool&) = delete;
    BoundedPool& operator=(const BoundedPool&) = delete;
    // Every TaskGroup must be gone by now.
    ~BoundedPool();

    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_; }
    [[nodiscard]] std::size_t queue_capacity() const noexcept { return workers_ * kQueuePerWorker; }
    [[nodiscard]] Stats stats() const;

private:
    friend class TaskGroup;

    using Task = std::function<void()>;

    struct Volume {
        std::size_t limit = kHddLimit;
        std::size_t running = 0;
    };
    struct Queued {
        Volume* volume = nullptr;
        Task task;
    };

    // Called through TaskGroup, whose tasks never throw. False if `stop` fired while waiting for
    // room in the queue; `task` is then dropped.
    [[nodiscard]] bool submit(const platform::VolumeInfo& volume, Task task,
                              const std::stop_token& stop);
    void work();
    void shut_down();
    [[nodiscard]] std::deque<Queued>::iterator next_runnable();

    std::size_t workers_;

    mutable std::mutex mutex_;
    std::condition_variable work_ready_;     // a task was queued, or stopping
    std::condition_variable_any space_free_; // the queue is below its cap, or stopping
    std::deque<Queued> queue_;
    std::map<std::string, Volume, std::less<>> volumes_; // by VolumeInfo::uid; nodes never move
    std::size_t running_ = 0;
    std::size_t peak_queued_ = 0;
    std::uint64_t completed_ = 0;
    bool stopping_ = false;

    std::vector<std::jthread> threads_; // last: they use everything above
};

// The tasks one job gave the pool. Tasks run on worker threads, so whatever they touch must be
// thread-safe; declare the group after what they use, because it waits for them on destruction,
// also when the job leaves early with an error. Tasks must not submit further tasks: with every
// worker waiting for room in the queue nothing would ever make room.
class TaskGroup {
public:
    using Task = std::function<Result<void>()>;

    // `stop` is the job's stop token. The pool and the log must outlive the group.
    TaskGroup(BoundedPool& pool, std::stop_token stop, ErrorLog& errors);
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    ~TaskGroup();

    // Queues `task`, blocking while the queue is full. False once a stop was requested, also while
    // blocked here: the task is dropped, and the job should end. A task that fails or throws goes
    // into the log. After a stop, tasks still queued are skipped; a running one must check the
    // stop token itself (read_chunks() does).
    [[nodiscard]] bool submit(const platform::VolumeInfo& volume, Task task);

    // Blocks until every task submitted so far has finished or was skipped. A task's captured
    // objects are destroyed by then.
    void wait();

private:
    struct State;

    BoundedPool& pool_;
    std::shared_ptr<State> state_; // shared with the queued tasks, which outlive wait() briefly
};

} // namespace sheepdog::jobs
