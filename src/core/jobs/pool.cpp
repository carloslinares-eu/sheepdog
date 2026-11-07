#include "core/jobs/pool.hpp"

#include <algorithm>
#include <utility>

namespace sheepdog::jobs {

std::size_t volume_limit(platform::Media media) noexcept {
    return media == platform::Media::ssd ? kSsdLimit : kHddLimit;
}

std::size_t default_worker_count() noexcept {
    return std::max<std::size_t>(std::thread::hardware_concurrency(), 1);
}

BoundedPool::BoundedPool(std::size_t workers) : workers_(std::max<std::size_t>(workers, 1)) {
    threads_.reserve(workers_);
    try {
        for (std::size_t i = 0; i < workers_; ++i) {
            threads_.emplace_back([this] { work(); });
        }
    } catch (...) {
        shut_down(); // the workers that did start would wait for ever
        throw;
    }
}

BoundedPool::~BoundedPool() {
    shut_down();
}

void BoundedPool::shut_down() {
    {
        const std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    work_ready_.notify_all();
    space_free_.notify_all();
    threads_.clear();
}

BoundedPool::Stats BoundedPool::stats() const {
    const std::scoped_lock lock(mutex_);
    return {.queued = queue_.size(),
            .running = running_,
            .peak_queued = peak_queued_,
            .completed = completed_};
}

bool BoundedPool::submit(const platform::VolumeInfo& volume, Task task,
                         const std::stop_token& stop) {
    std::unique_lock lock(mutex_);
    const bool room = space_free_.wait(
        lock, stop, [this] { return queue_.size() < queue_capacity() || stopping_; });
    if (!room || stopping_) {
        return false;
    }
    Volume& target = volumes_.try_emplace(volume.uid).first->second;
    target.limit = volume_limit(volume.media);
    queue_.push_back({&target, std::move(task)});
    peak_queued_ = std::max(peak_queued_, queue_.size());
    lock.unlock();
    work_ready_.notify_one();
    return true;
}

// The queue holds workers x 4 entries at most, so a plain scan is cheap.
std::deque<BoundedPool::Queued>::iterator BoundedPool::next_runnable() {
    return std::ranges::find_if(
        queue_, [](const Queued& queued) { return queued.volume->running < queued.volume->limit; });
}

void BoundedPool::work() {
    std::unique_lock lock(mutex_);
    for (;;) {
        auto next = queue_.end();
        work_ready_.wait(lock, [&] {
            next = next_runnable();
            return next != queue_.end() || stopping_;
        });
        if (next == queue_.end()) {
            return;
        }
        Volume& volume = *next->volume;
        Task task = std::move(next->task);
        queue_.erase(next);
        ++volume.running;
        ++running_;
        lock.unlock();
        space_free_.notify_all();

        task();
        task = nullptr; // what the task captured goes outside the lock as well

        // No need to wake a worker for the slot this frees: this one looks at the queue next.
        lock.lock();
        --volume.running;
        --running_;
        ++completed_;
    }
}

struct TaskGroup::State {
    State(std::stop_token stop_token, ErrorLog& error_log)
        : stop(std::move(stop_token)), errors(error_log) {}

    void begin() {
        const std::scoped_lock lock(mutex);
        ++pending;
    }

    void end() {
        const std::scoped_lock lock(mutex);
        if (--pending == 0) {
            idle.notify_all();
        }
    }

    void run(Task& task) {
        if (!stop.stop_requested()) {
            const Result<void> done = catch_exceptions(task);
            if (!done) {
                errors.add(done.error());
            }
        }
        task = nullptr; // before wait() returns: its captures may refer to the job's objects
        end();
    }

    std::stop_token stop;
    ErrorLog& errors;
    std::mutex mutex;
    std::condition_variable idle;
    std::size_t pending = 0;
};

TaskGroup::TaskGroup(BoundedPool& pool, std::stop_token stop, ErrorLog& errors)
    : pool_(pool), state_(std::make_shared<State>(std::move(stop), errors)) {}

TaskGroup::~TaskGroup() {
    wait();
}

bool TaskGroup::submit(const platform::VolumeInfo& volume, Task task) {
    if (state_->stop.stop_requested()) {
        return false;
    }
    state_->begin();
    bool queued = false;
    try {
        queued = pool_.submit(
            volume, [state = state_, task = std::move(task)]() mutable { state->run(task); },
            state_->stop);
    } catch (...) {
        state_->end();
        throw;
    }
    if (!queued) {
        state_->end();
    }
    return queued;
}

void TaskGroup::wait() {
    std::unique_lock lock(state_->mutex);
    state_->idle.wait(lock, [this] { return state_->pending == 0; });
}

} // namespace sheepdog::jobs
