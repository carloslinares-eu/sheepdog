#include "core/jobs/job.hpp"

#include <algorithm>
#include <atomic>
#include <system_error>
#include <utility>

namespace sheepdog::jobs {

namespace {

// Constant-initialised, so it can be used during the destruction of a static JobRunner.
constinit std::atomic<bool> g_file_job_running{false};

JobState end_state(const Result<void>& result) {
    if (result) {
        return JobState::completed;
    }
    return result.error().code == JobErrc::cancelled ? JobState::cancelled : JobState::failed;
}

} // namespace

JobContext::JobContext(std::stop_token stop, Progress& progress, ErrorLog& errors,
                       BoundedPool& pool)
    : stop_(std::move(stop)), progress_(progress), errors_(errors), pool_(pool) {}

FileChangeLock::FileChangeLock(FileChangeLock&& other) noexcept
    : held_(std::exchange(other.held_, false)) {}

FileChangeLock& FileChangeLock::operator=(FileChangeLock&& other) noexcept {
    if (this != &other) {
        release();
        held_ = std::exchange(other.held_, false);
    }
    return *this;
}

FileChangeLock::~FileChangeLock() {
    release();
}

std::optional<FileChangeLock> FileChangeLock::try_acquire() noexcept {
    if (g_file_job_running.exchange(true, std::memory_order_acquire)) {
        return std::nullopt;
    }
    std::optional<FileChangeLock> lock(std::in_place);
    lock->held_ = true;
    return lock;
}

void FileChangeLock::release() noexcept {
    if (std::exchange(held_, false)) {
        g_file_job_running.store(false, std::memory_order_release);
    }
}

Job::Job(Key /*key*/, std::string name, JobKind kind, const Clock& clock, BoundedPool& pool,
         FileChangeLock lock, JobBody body)
    : name_(std::move(name)), kind_(kind), progress_(clock), pool_(pool), lock_(std::move(lock)),
      thread_([this, body = std::move(body)](const std::stop_token& stop) mutable {
          run(stop, std::move(body));
      }) {}

void Job::run(const std::stop_token& stop, JobBody body) {
    JobContext context(stop, progress_, errors_, pool_);
    Result<void> result = catch_exceptions([&] { return body(context); });
    body = nullptr; // what it captured goes before the job counts as ended
    if (!result) {
        errors_.add(result.error());
    } else if (stop.stop_requested()) {
        result = job_error(JobErrc::cancelled);
    }
    lock_.release();
    progress_.finish(end_state(result));
    {
        const std::scoped_lock lock(mutex_);
        result_ = std::move(result);
        ended_ = true;
    }
    end_seen_.notify_all();
}

bool Job::finished() const {
    const std::scoped_lock lock(mutex_);
    return ended_;
}

Result<void> Job::wait() const {
    std::unique_lock lock(mutex_);
    end_seen_.wait(lock, [this] { return ended_; });
    return result_;
}

JobRunner::JobRunner(const Clock& clock, std::size_t workers) : clock_(clock), pool_(workers) {}

JobRunner::~JobRunner() {
    std::vector<std::shared_ptr<Job>> jobs;
    {
        const std::scoped_lock lock(mutex_);
        closing_ = true;
        jobs.swap(jobs_);
    }
    for (const auto& job : jobs) {
        job->request_stop();
    }
    for (const auto& job : jobs) {
        (void)job->wait();
    }
}

Result<std::shared_ptr<Job>> JobRunner::start(std::string name, JobKind kind, JobBody body) {
    FileChangeLock lock;
    if (kind == JobKind::changes_files) {
        auto acquired = FileChangeLock::try_acquire();
        if (!acquired) {
            return job_error(JobErrc::busy, "cannot start \"" + name + "\"");
        }
        lock = std::move(*acquired);
    }
    const std::scoped_lock guard(mutex_);
    if (closing_) {
        return job_error(JobErrc::cancelled, "cannot start \"" + name + "\"");
    }
    std::erase_if(jobs_, [](const auto& job) { return job->finished(); });
    try {
        auto job = std::make_shared<Job>(Job::Key{}, std::move(name), kind, clock_, pool_,
                                         std::move(lock), std::move(body));
        jobs_.push_back(job);
        return job;
    } catch (const std::system_error& e) {
        return Error{.code = e.code(), .path = {}, .detail = "cannot start a job thread"};
    }
}

} // namespace sheepdog::jobs
