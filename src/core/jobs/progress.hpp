#pragma once

// The progress of one job. The job and its pool tasks report finished work from any thread; the
// GUI or CLI polls snapshot() (the Activity bar every 150 ms) and gets an immutable Snapshot that
// it can keep and read without a lock.
//
// A job is a series of steps ("Copying", "Verifying"), each with its own phase name and totals.
// All times come from the injected Clock.

#include "core/jobs/clock.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace sheepdog::jobs {

using Seconds = std::chrono::duration<double>;

// The length of the speed window, and how finely it keeps work apart.
inline constexpr std::chrono::seconds kSpeedWindow{5};
inline constexpr std::chrono::milliseconds kSpeedResolution{100};

// An amount of work.
struct Work {
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;

    Work& operator+=(const Work& other) noexcept {
        files += other.files;
        bytes += other.bytes;
        return *this;
    }
    friend bool operator==(const Work&, const Work&) = default;
};

enum class JobState : std::uint8_t {
    running,
    completed, // ran to its end; single files may still have failed (see the job's ErrorLog)
    cancelled, // stopped on request
    failed,    // gave up with an error
};

// What a job looks like at one moment. A total of 0 means "not known (yet)".
struct Snapshot {
    JobState state = JobState::running;
    std::string phase; // the current step, or the last one once the job has ended
    Work done;         // the whole job
    Work total;
    Work step_done; // the current step
    Work step_total;
    // Work finished in the last 5 s of the step, per second (the step's age while that is shorter).
    double bytes_per_second = 0;
    double files_per_second = 0;
    Seconds elapsed{0}; // since the job started; stands still once it has ended
    // Time left at the current speed: `eta` for the whole job, `step_left` for the step. Counted
    // in bytes when the total has bytes, else in files. Empty while the total or the speed is 0.
    // Without set_job_total(), `eta` is `step_left`: work of finished steps is no total for what
    // the current step measures. With it, `eta` counts in bytes only if the job total has bytes.
    // Both are 0 once a job has completed and empty once it was cancelled or failed.
    std::optional<Seconds> eta;
    std::optional<Seconds> step_left;
};

// Work per second over the last kSpeedWindow. Not thread-safe; Progress guards it. It stays small
// however fast work arrives: work within kSpeedResolution is added up into one entry, which leaves
// the window as a whole.
class SpeedWindow {
public:
    struct Rates {
        double files = 0;
        double bytes = 0;
    };

    // Starts over: everything before `start` is forgotten.
    void restart(Clock::TimePoint start);
    void add(Clock::TimePoint now, const Work& work);
    [[nodiscard]] Rates rates(Clock::TimePoint now) const;
    [[nodiscard]] std::size_t entries() const noexcept { return entries_.size(); }

private:
    struct Entry {
        Clock::TimePoint time;
        Work work;
    };

    Clock::TimePoint start_;
    std::deque<Entry> entries_;
};

// Thread-safe. The job thread calls set_job_total(), begin_step() and add_total(); any thread may
// call add() and snapshot().
class Progress {
public:
    // The job's elapsed time starts here.
    explicit Progress(const Clock& clock);
    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;

    // The work of the whole job, for a job that knows its later steps in advance. Without it the
    // job's total is the work done in finished steps plus the current step's total.
    void set_job_total(const Work& total);

    // Ends the current step and starts the next. `total` is its work, when known. The speed starts
    // over, because the next step may run at a very different pace.
    void begin_step(std::string phase, const Work& total = {});

    // The current step turned out to hold more work (a walk found more files, say).
    void add_total(const Work& work);

    // Work finished. Ignored once the job has ended.
    void add(const Work& work);

    // The job has ended (`state` is not running): the runner's last call. Later snapshots are all
    // this one.
    void finish(JobState state);

    // Publishes the current state and returns it, so a job that stalls still shows its speed
    // falling and its time running on. Never null.
    [[nodiscard]] std::shared_ptr<const Snapshot> snapshot() const;

private:
    void publish(Clock::TimePoint now) const;

    const Clock& clock_;
    Clock::TimePoint start_;

    mutable std::mutex mutex_;
    JobState state_ = JobState::running;
    std::string phase_;
    Work done_;
    Work job_total_;  // from set_job_total()
    Work step_start_; // done_ when the current step began
    Work step_total_;
    SpeedWindow window_;
    mutable std::shared_ptr<const Snapshot> published_; // replaced whole, under mutex_
};

} // namespace sheepdog::jobs
