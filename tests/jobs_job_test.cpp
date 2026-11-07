// The job runner: threads, stops, error collection and the file-change lock.
// Catch2 assertions are not thread-safe: job bodies only record what happened, and the test
// thread checks it afterwards.
#include "core/jobs/job.hpp"
#include "support/jobs_support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using sheepdog::jobs::FileChangeLock;
using sheepdog::jobs::Job;
using sheepdog::jobs::job_error;
using sheepdog::jobs::JobContext;
using sheepdog::jobs::JobErrc;
using sheepdog::jobs::JobKind;
using sheepdog::jobs::JobRunner;
using sheepdog::jobs::JobState;
using sheepdog::jobs::Ok;
using sheepdog::jobs::Result;
using sheepdog::jobs::system_clock;
using sheepdog::jobs::Work;
using sheepdog::platform::Media;
using sheepdog::test::eventually;
using sheepdog::test::FakeClock;
using sheepdog::test::Gate;
using sheepdog::test::start_job;
using sheepdog::test::volume;

namespace {

// Waits for a stop, as a job in the middle of its work would. A stop that never comes makes the
// test fail instead of hang.
Result<void> until_stopped(JobContext& context) {
    if (!eventually([&] { return context.stop_requested(); })) {
        return job_error(JobErrc::busy, "no stop came");
    }
    return Ok{};
}

Result<void> quickly(JobContext& /*context*/) {
    return Ok{};
}

} // namespace

TEST_CASE("a job runs on its own thread and reports its end", "[unit]") {
    JobRunner runner;
    std::thread::id body_thread;
    auto job = start_job(runner, JobKind::read_only, [&](JobContext& context) -> Result<void> {
        body_thread = std::this_thread::get_id();
        context.progress().begin_step("Working", {.files = 2, .bytes = 300});
        context.progress().add({.files = 2, .bytes = 300});
        return Ok{};
    });
    CHECK(job->name() == "test");
    CHECK(job->kind() == JobKind::read_only);
    REQUIRE(job->wait().has_value());
    CHECK(body_thread != std::this_thread::get_id());

    const auto snapshot = job->snapshot();
    CHECK(snapshot->state == JobState::completed);
    CHECK(snapshot->phase == "Working");
    CHECK(snapshot->done == Work{.files = 2, .bytes = 300});
    CHECK(snapshot->eta == sheepdog::jobs::Seconds(0));
    CHECK(job->errors().count() == 0);
    CHECK(job->wait().has_value()); // any number of times
}

TEST_CASE("a stop ends a job as cancelled, without an error", "[unit]") {
    JobRunner runner;
    auto job = start_job(runner, JobKind::read_only, until_stopped);
    CHECK(job->snapshot()->state == JobState::running);
    job->request_stop();
    const auto result = job->wait();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == JobErrc::cancelled);
    CHECK(job->snapshot()->state == JobState::cancelled);
    CHECK(job->errors().count() == 0);
}

TEST_CASE("a body can report the stop as its result", "[unit]") {
    JobRunner runner;
    auto job = start_job(runner, JobKind::read_only,
                         [](JobContext&) -> Result<void> { return job_error(JobErrc::cancelled); });
    REQUIRE_FALSE(job->wait().has_value());
    CHECK(job->snapshot()->state == JobState::cancelled);
    CHECK(job->errors().count() == 0);
}

TEST_CASE("an error ends a job as failed", "[unit]") {
    JobRunner runner;
    auto job = start_job(runner, JobKind::read_only, [](JobContext&) -> Result<void> {
        return job_error(JobErrc::busy, "gave up");
    });
    const auto result = job->wait();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().detail == "gave up");
    CHECK(job->snapshot()->state == JobState::failed);
    CHECK_FALSE(job->snapshot()->eta.has_value());
    REQUIRE(job->errors().count() == 1);
    CHECK(job->errors().kept()[0].detail == "gave up");
}

TEST_CASE("an error wins over a stop", "[unit]") {
    JobRunner runner;
    auto job = start_job(runner, JobKind::read_only, [](JobContext& context) -> Result<void> {
        (void)until_stopped(context);
        return job_error(JobErrc::busy, "failed while stopping");
    });
    job->request_stop();
    CHECK_FALSE(job->wait().has_value());
    CHECK(job->snapshot()->state == JobState::failed);
}

TEST_CASE("an exception ends a job as failed, not the program", "[unit]") {
    JobRunner runner;
    auto throwing = start_job(runner, JobKind::read_only, [](JobContext&) -> Result<void> {
        throw std::runtime_error("boom");
    });
    const auto result = throwing->wait();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == JobErrc::exception);
    CHECK(result.error().detail == "boom");
    CHECK(throwing->snapshot()->state == JobState::failed);

    // An empty body throws std::bad_function_call.
    auto empty = start_job(runner, JobKind::read_only, nullptr);
    CHECK_FALSE(empty->wait().has_value());
    CHECK(empty->snapshot()->state == JobState::failed);
}

TEST_CASE("a job gathers the errors of its tasks and goes on", "[unit]") {
    JobRunner runner(system_clock(), 4);
    const auto disk = volume("ssd", Media::ssd);
    auto job = start_job(runner, JobKind::read_only, [&](JobContext& context) -> Result<void> {
        auto tasks = context.tasks();
        for (int i = 0; i < 100; ++i) {
            const bool queued = tasks.submit(disk, [&context, i]() -> Result<void> {
                context.progress().add({.files = 1, .bytes = 0});
                if (i % 10 == 0) {
                    return job_error(JobErrc::busy, std::to_string(i));
                }
                return Ok{};
            });
            if (!queued) {
                break;
            }
        }
        context.errors().add(job_error(JobErrc::busy, "the job's own"));
        return Ok{};
    });
    CHECK(job->wait().has_value()); // single files failing do not fail the job
    CHECK(job->snapshot()->state == JobState::completed);
    CHECK(job->snapshot()->done.files == 100);
    CHECK(job->errors().count() == 11);
}

TEST_CASE("a job that ends early still waits for its tasks", "[unit]") {
    constexpr int kTasks = 6; // more than the workers: some are still queued when the body ends
    Gate gate;
    std::atomic<int> tasks_done{0};
    std::atomic<int> done_as_body_left{-1};
    std::atomic<bool> ending{false};
    const auto disk = volume("ssd", Media::ssd);

    // What a body's locals see: this goes after the group, as they do.
    struct OnLeaving {
        std::atomic<int>& from;
        std::atomic<int>& to;
        ~OnLeaving() { to = from.load(); }
    };
    bool throws = false;
    SECTION("with an error") {}
    SECTION("with an exception") {
        throws = true;
    }
    JobRunner runner(system_clock(), 2);
    auto job = start_job(runner, JobKind::read_only, [&](JobContext& context) -> Result<void> {
        const OnLeaving on_leaving{.from = tasks_done, .to = done_as_body_left};
        auto tasks = context.tasks();
        for (int i = 0; i < kTasks; ++i) {
            const bool queued = tasks.submit(disk, [&]() -> Result<void> {
                gate.pass();
                ++tasks_done;
                return Ok{};
            });
            if (!queued) {
                break;
            }
        }
        ending = true;
        if (throws) {
            throw std::runtime_error("gave up");
        }
        return job_error(JobErrc::busy, "gave up");
    });

    REQUIRE(eventually([&] { return ending.load(); }));
    std::this_thread::sleep_for(20ms); // time to fail, for a group that does not wait
    CHECK(job->snapshot()->state == JobState::running);
    CHECK(done_as_body_left.load() == -1);
    gate.open();

    const auto result = job->wait();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().detail == "gave up");
    CHECK(done_as_body_left.load() == kTasks);
    CHECK(tasks_done.load() == kTasks);
    CHECK(job->snapshot()->state == JobState::failed);
    CHECK(job->errors().count() == 1);
}

TEST_CASE("elapsed time comes from the injected clock", "[unit]") {
    FakeClock clock;
    JobRunner runner(clock, 2);
    Gate gate;
    auto job = start_job(runner, JobKind::read_only, [&](JobContext& context) -> Result<void> {
        context.progress().begin_step("Copying", {.files = 0, .bytes = 4000});
        context.progress().add({.files = 0, .bytes = 1000}); // at t = 0, reported at once
        gate.pass();
        return Ok{};
    });
    REQUIRE(eventually([&] { return job->snapshot()->done.bytes == 1000; }));
    clock.advance(4s);
    auto snapshot = job->snapshot();
    CHECK(snapshot->elapsed.count() == Catch::Approx(4));
    CHECK(snapshot->bytes_per_second == Catch::Approx(250));
    REQUIRE(snapshot->eta.has_value());
    CHECK(snapshot->eta->count() == Catch::Approx(12));
    CHECK(snapshot->step_left->count() == Catch::Approx(12));

    gate.open();
    REQUIRE(job->wait().has_value());
    clock.advance(1h);
    CHECK(job->snapshot()->elapsed.count() == Catch::Approx(4));
}

TEST_CASE("only one job that changes files runs at a time", "[unit]") {
    JobRunner runner;
    auto first = start_job(runner, JobKind::changes_files, until_stopped);

    SECTION("a second one is refused") {
        const auto second = runner.start("Second", JobKind::changes_files, quickly);
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == JobErrc::busy);
        CHECK(second.error().message().find("Second") != std::string::npos);
    }
    SECTION("a second JobRunner does not get around it") {
        JobRunner other;
        const auto second = other.start("Second", JobKind::changes_files, quickly);
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == JobErrc::busy);
    }
    SECTION("jobs that only read run alongside") {
        auto reader = start_job(runner, JobKind::read_only, quickly);
        CHECK(reader->wait().has_value());
        CHECK(first->snapshot()->state == JobState::running);
    }
    first->request_stop();
    CHECK(first->wait().error().code == JobErrc::cancelled);
}

TEST_CASE("the file-change lock is free again as soon as a job has ended", "[unit]") {
    JobRunner runner;
    const auto next_job_starts = [&] {
        auto next = runner.start("Next", JobKind::changes_files, quickly);
        REQUIRE(next.has_value());
        CHECK((*next)->wait().has_value());
    };
    SECTION("completed") {
        CHECK(start_job(runner, JobKind::changes_files, quickly)->wait().has_value());
        next_job_starts();
    }
    SECTION("cancelled") {
        auto job = start_job(runner, JobKind::changes_files, until_stopped);
        job->request_stop();
        CHECK_FALSE(job->wait().has_value());
        next_job_starts();
    }
    SECTION("failed") {
        auto job = start_job(runner, JobKind::changes_files,
                             [](JobContext&) -> Result<void> { throw std::runtime_error("boom"); });
        CHECK_FALSE(job->wait().has_value());
        next_job_starts();
    }
    SECTION("seen in a snapshot") {
        auto job = start_job(runner, JobKind::changes_files, quickly);
        REQUIRE(eventually([&] { return job->snapshot()->state != JobState::running; }));
        next_job_starts();
    }
}

TEST_CASE("the file-change lock holds under a race for it", "[unit]") {
    std::atomic<int> holders{0};
    std::atomic<int> overlaps{0};
    std::atomic<int> rounds{0};
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&] {
                for (int n = 0; n < 2000; ++n) {
                    auto lock = FileChangeLock::try_acquire();
                    if (lock.has_value()) {
                        overlaps += holders.fetch_add(1) == 0 ? 0 : 1;
                        ++rounds;
                        --holders;
                    }
                }
            });
        }
    }
    CHECK(overlaps.load() == 0);
    CHECK(rounds.load() > 0);

    auto lock = FileChangeLock::try_acquire();
    REQUIRE(lock.has_value());
    CHECK_FALSE(FileChangeLock::try_acquire().has_value());
    FileChangeLock moved = std::move(*lock);
    lock.reset(); // a moved-from lock holds nothing
    CHECK_FALSE(FileChangeLock::try_acquire().has_value());
    moved.release();
    CHECK(FileChangeLock::try_acquire().has_value());
}

TEST_CASE("destroying the runner stops its jobs and waits for them", "[unit]") {
    std::shared_ptr<Job> job;
    std::atomic<bool> stopped{false};
    {
        JobRunner runner;
        job = start_job(runner, JobKind::changes_files, [&](JobContext& context) -> Result<void> {
            (void)until_stopped(context);
            stopped = true;
            return Ok{};
        });
    }
    CHECK(stopped.load());
    CHECK(job->snapshot()->state == JobState::cancelled);
    CHECK(FileChangeLock::try_acquire().has_value()); // the lock is free too
}

TEST_CASE("the runner forgets finished jobs, and handles outlive that", "[unit]") {
    JobRunner runner;
    auto kept = start_job(runner, JobKind::read_only, quickly);
    REQUIRE(kept->wait().has_value());
    for (int i = 0; i < 100; ++i) {
        CHECK(start_job(runner, JobKind::read_only, quickly)->wait().has_value());
    }
    CHECK(kept->snapshot()->state == JobState::completed);
    CHECK(kept.use_count() == 1);
}

TEST_CASE("a job that starts another job while the runner goes away gets an answer", "[unit]") {
    std::atomic<bool> refused{false};
    {
        JobRunner runner;
        auto job = start_job(runner, JobKind::read_only, [&](JobContext& context) -> Result<void> {
            (void)until_stopped(context);
            for (int i = 0; i < 100 && !refused; ++i) {
                const auto late = runner.start("Late", JobKind::read_only, quickly);
                refused = !late.has_value() && late.error().code == JobErrc::cancelled;
                std::this_thread::sleep_for(100us);
            }
            return Ok{};
        });
        // Leaving the scope stops the job, which then finds the runner closing.
    }
    CHECK(refused.load());
}

TEST_CASE("jobs, tasks and pollers run side by side, and stop at any moment", "[unit]") {
    JobRunner runner(system_clock(), 4);
    const auto ssd = volume("ssd", Media::ssd);
    const auto hdd = volume("hdd", Media::hdd);
    std::atomic<std::uint64_t> polls{0};
    std::atomic<int> went_backwards{0};
    std::atomic<std::shared_ptr<Job>> current;

    // Polls the running job as the Activity bar does, only much faster.
    // A stop rather than a flag of ours: the jthread sends it if a REQUIRE leaves the test early.
    std::jthread poller([&](const std::stop_token& stop) {
        std::uint64_t last = 0;
        std::shared_ptr<Job> last_job;
        while (!stop.stop_requested()) {
            auto job = current.load();
            if (job == nullptr) {
                continue;
            }
            const auto snapshot = job->snapshot();
            ++polls;
            if (job != last_job) {
                last_job = job;
                last = 0;
            }
            went_backwards += snapshot->done.files < last ? 1 : 0;
            last = snapshot->done.files;
        }
    });

    const auto body = [&](JobContext& context) -> Result<void> {
        auto tasks = context.tasks();
        context.progress().begin_step("Working", {.files = 1000, .bytes = 0});
        for (int i = 0; i < 1000; ++i) {
            const auto task = [&context, i]() -> Result<void> {
                context.progress().add({.files = 1, .bytes = 10});
                if (i % 100 == 0) {
                    throw std::runtime_error("boom");
                }
                return Ok{};
            };
            if (!tasks.submit(i % 2 == 0 ? ssd : hdd, task)) {
                break;
            }
        }
        return Ok{};
    };
    for (int round = 0; round < 40; ++round) {
        // Three jobs compete for the queue, so their producers block and wake each other.
        const std::vector<std::shared_ptr<Job>> jobs = {
            start_job(runner, JobKind::changes_files, body),
            start_job(runner, JobKind::read_only, body),
            start_job(runner, JobKind::read_only, body),
        };
        current.store(jobs[static_cast<std::size_t>(round) % jobs.size()]);
        std::this_thread::sleep_for(std::chrono::microseconds(round * 50));
        for (std::size_t i = 0; i < jobs.size(); ++i) { // the bits of `round` pick the jobs to stop
            if (((round >> i) & 1) != 0) {
                jobs[i]->request_stop();
            }
        }
        for (const auto& job : jobs) {
            const auto result = job->wait();
            const auto snapshot = job->snapshot();
            CHECK(snapshot->state ==
                  (result.has_value() ? JobState::completed : JobState::cancelled));
            if (result.has_value()) {
                CHECK(snapshot->done.files == 1000);
                CHECK(job->errors().count() == 10);
            }
            CHECK(snapshot->done.bytes == snapshot->done.files * 10);
        }
    }
    poller.request_stop();
    poller.join();
    current.store(nullptr);
    CHECK(polls.load() > 0);
    CHECK(went_backwards.load() == 0);
    REQUIRE(eventually([&] { return runner.pool().stats().running == 0; }));
    CHECK(runner.pool().stats().queued == 0);
    CHECK(runner.pool().stats().peak_queued <= runner.pool().queue_capacity());
}
