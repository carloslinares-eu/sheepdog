#include "core/jobs/progress.hpp"

#include <algorithm>
#include <utility>

namespace sheepdog::jobs {

namespace {

std::uint64_t remaining(std::uint64_t done, std::uint64_t total) noexcept {
    return total > done ? total - done : 0;
}

// What is left of `total` at the given speed: in bytes when the total has bytes, else in files.
std::optional<Seconds> time_left(const Work& done, const Work& total,
                                 const SpeedWindow::Rates& rates) {
    const bool in_bytes = total.bytes > 0;
    if (!in_bytes && total.files == 0) {
        return std::nullopt;
    }
    const std::uint64_t left =
        in_bytes ? remaining(done.bytes, total.bytes) : remaining(done.files, total.files);
    if (left == 0) {
        return Seconds(0);
    }
    const double rate = in_bytes ? rates.bytes : rates.files;
    if (rate <= 0) {
        return std::nullopt;
    }
    return Seconds(static_cast<double>(left) / rate);
}

} // namespace

void SpeedWindow::restart(Clock::TimePoint start) {
    start_ = start;
    entries_.clear();
}

void SpeedWindow::add(Clock::TimePoint now, const Work& work) {
    while (!entries_.empty() && now - entries_.front().time >= kSpeedWindow) {
        entries_.pop_front();
    }
    if (!entries_.empty() && now - entries_.back().time < kSpeedResolution) {
        entries_.back().work += work;
    } else {
        entries_.push_back({now, work});
    }
}

SpeedWindow::Rates SpeedWindow::rates(Clock::TimePoint now) const {
    const Seconds span = std::min<Clock::Duration>(now - start_, kSpeedWindow);
    if (span <= Seconds(0)) {
        return {};
    }
    Work sum;
    for (const Entry& entry : entries_) {
        if (now - entry.time < kSpeedWindow) {
            sum += entry.work;
        }
    }
    return {.files = static_cast<double>(sum.files) / span.count(),
            .bytes = static_cast<double>(sum.bytes) / span.count()};
}

Progress::Progress(const Clock& clock) : clock_(clock), start_(clock.now()) {
    window_.restart(start_);
}

void Progress::set_job_total(const Work& total) {
    const std::scoped_lock lock(mutex_);
    job_total_ = total;
}

void Progress::begin_step(std::string phase, const Work& total) {
    const std::scoped_lock lock(mutex_);
    phase_ = std::move(phase);
    step_start_ = done_;
    step_total_ = total;
    window_.restart(clock_.now());
}

void Progress::add_total(const Work& work) {
    const std::scoped_lock lock(mutex_);
    step_total_ += work;
}

void Progress::add(const Work& work) {
    const std::scoped_lock lock(mutex_);
    if (state_ != JobState::running) {
        return;
    }
    done_ += work;
    window_.add(clock_.now(), work);
}

void Progress::finish(JobState state) {
    const std::scoped_lock lock(mutex_);
    if (state_ == JobState::running) {
        state_ = state;
        publish(clock_.now());
    }
}

std::shared_ptr<const Snapshot> Progress::snapshot() const {
    const std::scoped_lock lock(mutex_);
    if (state_ == JobState::running) {
        publish(clock_.now());
    }
    return published_;
}

void Progress::publish(Clock::TimePoint now) const {
    auto next = std::make_shared<Snapshot>();
    next->state = state_;
    next->phase = phase_;
    next->done = done_;
    next->total = {.files = std::max(job_total_.files, step_start_.files + step_total_.files),
                   .bytes = std::max(job_total_.bytes, step_start_.bytes + step_total_.bytes)};
    next->step_done = {.files = done_.files - step_start_.files,
                       .bytes = done_.bytes - step_start_.bytes};
    next->step_total = step_total_;
    const SpeedWindow::Rates rates = window_.rates(now);
    next->bytes_per_second = rates.bytes;
    next->files_per_second = rates.files;
    next->elapsed = now - start_;
    if (state_ == JobState::running) {
        next->eta = time_left(next->done, next->total, rates);
        next->step_left = time_left(next->step_done, next->step_total, rates);
    } else if (state_ == JobState::completed) {
        next->eta = Seconds(0);
        next->step_left = Seconds(0);
    }
    published_ = std::move(next);
}

} // namespace sheepdog::jobs
