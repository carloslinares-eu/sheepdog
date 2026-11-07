#pragma once

// Helpers for the job framework tests.

#include "core/jobs/job.hpp"
#include "core/platform/fs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace sheepdog::test {

// A clock that moves only when the test says so.
class FakeClock final : public jobs::Clock {
public:
    [[nodiscard]] TimePoint now() const override { return TimePoint(Duration(ticks_.load())); }
    void advance(Duration by) { ticks_.fetch_add(by.count()); }

private:
    std::atomic<Duration::rep> ticks_{0};
};

// Waits (for real) until `condition` holds; false if it still does not after 60 s, which only a
// test that hangs would reach. For what another thread is about to do.
template <class Condition> [[nodiscard]] bool eventually(Condition condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

// Holds tasks back until the test opens it, or for 60 s: a failing test ends instead of hanging.
class Gate {
public:
    void open() { open_ = true; }
    void pass() const {
        (void)eventually([this] { return open_.load(); });
    }

private:
    std::atomic<bool> open_{false};
};

[[nodiscard]] inline platform::VolumeInfo volume(std::string uid, platform::Media media) {
    platform::VolumeInfo info;
    info.uid = std::move(uid);
    info.media = media;
    return info;
}

// Starts a job and fails the test if it is refused. Call from the test's thread.
[[nodiscard]] inline std::shared_ptr<jobs::Job> start_job(jobs::JobRunner& runner,
                                                          jobs::JobKind kind, jobs::JobBody body) {
    auto job = runner.start("test", kind, std::move(body));
    REQUIRE(job.has_value());
    return *job;
}

} // namespace sheepdog::test
