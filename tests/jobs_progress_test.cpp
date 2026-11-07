// The progress maths, on a fake clock: speed over the sliding 5 s window, ETA, time left in the
// step, elapsed time.
#include "core/jobs/progress.hpp"
#include "support/jobs_support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

using namespace std::chrono_literals;
using Catch::Approx;
using sheepdog::jobs::JobState;
using sheepdog::jobs::Progress;
using sheepdog::jobs::Snapshot;
using sheepdog::jobs::Work;
using sheepdog::test::FakeClock;

namespace {

Work bytes(std::uint64_t n) {
    return {.files = 0, .bytes = n};
}

Work files(std::uint64_t n) {
    return {.files = n, .bytes = 0};
}

// `seconds` seconds of work, reported once a second.
void work_for(FakeClock& clock, Progress& progress, int seconds, const Work& per_second) {
    for (int i = 0; i < seconds; ++i) {
        clock.advance(1s);
        progress.add(per_second);
    }
}

double speed(const Progress& progress) {
    return progress.snapshot()->bytes_per_second;
}

} // namespace

TEST_CASE("the speed is measured over the step's age during the first 5 s", "[unit]") {
    FakeClock clock;
    Progress progress(clock);
    CHECK(speed(progress) == 0); // no time has passed yet

    work_for(clock, progress, 1, bytes(1000));
    CHECK(speed(progress) == Approx(1000));
    work_for(clock, progress, 1, bytes(3000));
    CHECK(speed(progress) == Approx(2000)); // 4000 bytes in 2 s

    clock.advance(2s); // nothing arrives, and the window grows
    CHECK(speed(progress) == Approx(1000));
}

TEST_CASE("the speed covers the last 5 s only", "[unit]") {
    FakeClock clock;
    Progress progress(clock);
    work_for(clock, progress, 5, bytes(1000)); // at t = 1..5
    CHECK(speed(progress) == Approx(1000));

    // The window slides on while nothing arrives; each second drops the oldest 1000 bytes.
    clock.advance(1s);
    CHECK(speed(progress) == Approx(800));
    clock.advance(3s);
    CHECK(speed(progress) == Approx(200));
    clock.advance(999ms);
    CHECK(speed(progress) == Approx(200));
    clock.advance(1ms); // the last 1000 bytes are exactly 5 s old now
    CHECK(speed(progress) == 0);
}

TEST_CASE("a new pace replaces the old one within 5 s", "[unit]") {
    FakeClock clock;
    Progress progress(clock);
    work_for(clock, progress, 20, bytes(1000));
    CHECK(speed(progress) == Approx(1000));

    work_for(clock, progress, 1, bytes(6000));
    CHECK(speed(progress) == Approx(2000)); // (4 x 1000 + 6000) / 5
    work_for(clock, progress, 3, bytes(6000));
    CHECK(speed(progress) == Approx(5000));
    work_for(clock, progress, 1, bytes(6000));
    CHECK(speed(progress) == Approx(6000));
}

TEST_CASE("many small pieces are measured to within the window's resolution", "[unit]") {
    FakeClock clock;
    Progress progress(clock);
    for (int i = 0; i < 20'000; ++i) { // 20 s at 1000 bytes per ms
        clock.advance(1ms);
        progress.add(bytes(1000));
    }
    const double expected = 1'000'000;
    CHECK(speed(progress) <= Approx(expected));
    CHECK(speed(progress) >= Approx(expected * 0.98)); // 100 ms of 5 s
}

TEST_CASE("the window stays small however fast work arrives", "[unit]") {
    using sheepdog::jobs::kSpeedResolution;
    using sheepdog::jobs::kSpeedWindow;
    using sheepdog::jobs::SpeedWindow;
    constexpr auto kMostEntries = kSpeedWindow / kSpeedResolution + 1;

    FakeClock clock;
    SpeedWindow window;
    window.restart(clock.now());
    for (int i = 0; i < 100'000; ++i) {
        clock.advance(1ms);
        window.add(clock.now(), bytes(1));
        REQUIRE(window.entries() <= static_cast<std::size_t>(kMostEntries));
    }
    CHECK(window.entries() >= static_cast<std::size_t>(kMostEntries) - 1);
    CHECK(window.rates(clock.now()).bytes <= Approx(1000));
    CHECK(window.rates(clock.now()).bytes >= Approx(980));

    window.restart(clock.now());
    CHECK(window.entries() == 0);
    clock.advance(1s);
    CHECK(window.rates(clock.now()).bytes == 0);
}

TEST_CASE("files have a speed of their own", "[unit]") {
    FakeClock clock;
    Progress progress(clock);
    work_for(clock, progress, 4, {.files = 2, .bytes = 1000});
    const auto snapshot = progress.snapshot();
    CHECK(snapshot->files_per_second == Approx(2));
    CHECK(snapshot->bytes_per_second == Approx(1000));
    CHECK(snapshot->done == Work{.files = 8, .bytes = 4000});
}

TEST_CASE("ETA and time left follow from the totals and the speed", "[unit]") {
    FakeClock clock;
    Progress progress(clock);
    progress.set_job_total({.files = 8, .bytes = 8000});
    progress.begin_step("Copying", {.files = 4, .bytes = 4000});

    // Nothing is known about the speed yet.
    auto snapshot = progress.snapshot();
    CHECK(snapshot->phase == "Copying");
    CHECK_FALSE(snapshot->eta.has_value());
    CHECK_FALSE(snapshot->step_left.has_value());

    work_for(clock, progress, 2, {.files = 1, .bytes = 500});
    snapshot = progress.snapshot();
    CHECK(snapshot->bytes_per_second == Approx(500));
    CHECK(snapshot->done == Work{.files = 2, .bytes = 1000});
    CHECK(snapshot->total == Work{.files = 8, .bytes = 8000});
    CHECK(snapshot->step_done == Work{.files = 2, .bytes = 1000});
    CHECK(snapshot->step_total == Work{.files = 4, .bytes = 4000});
    REQUIRE(snapshot->eta.has_value());
    REQUIRE(snapshot->step_left.has_value());
    CHECK(snapshot->eta->count() == Approx(14));      // 7000 bytes at 500 bytes/s
    CHECK(snapshot->step_left->count() == Approx(6)); // 3000 bytes at 500 bytes/s
    CHECK(snapshot->elapsed.count() == Approx(2));

    // The pace doubles: both estimates follow, with the window's delay.
    work_for(clock, progress, 5, {.files = 0, .bytes = 1000});
    snapshot = progress.snapshot();
    CHECK(snapshot->bytes_per_second == Approx(1000));
    CHECK(snapshot->eta->count() == Approx(2));
    CHECK(snapshot->step_left->count() == Approx(0)); // 6000 of 4000: the step is past its total
    CHECK(snapshot->elapsed.count() == Approx(7));
}

TEST_CASE("the time left is counted in files when the total has no bytes", "[unit]") {
    FakeClock clock;
    Progress progress(clock);
    progress.begin_step("Renaming", files(100));
    work_for(clock, progress, 5, files(4));
    const auto snapshot = progress.snapshot();
    CHECK(snapshot->files_per_second == Approx(4));
    REQUIRE(snapshot->step_left.has_value());
    CHECK(snapshot->step_left->count() == Approx(20));
    REQUIRE(snapshot->eta.has_value());
    CHECK(snapshot->eta->count() == Approx(20));
}

TEST_CASE("the time left is unknown without a total, and without a speed", "[unit]") {
    FakeClock clock;
    Progress progress(clock);
    progress.begin_step("Scanning");
    work_for(clock, progress, 2, {.files = 10, .bytes = 1000});
    auto snapshot = progress.snapshot();
    CHECK(snapshot->total == Work{});
    CHECK(snapshot->files_per_second == Approx(10));
    CHECK_FALSE(snapshot->eta.has_value());
    CHECK_FALSE(snapshot->step_left.has_value());

    // The job stalls: with the speed at 0 no estimate is left, and the time still runs.
    progress.add_total(bytes(5000));
    clock.advance(1s);
    snapshot = progress.snapshot();
    CHECK(snapshot->step_left.has_value());
    clock.advance(10s);
    snapshot = progress.snapshot();
    CHECK(snapshot->bytes_per_second == 0);
    CHECK_FALSE(snapshot->eta.has_value());
    CHECK_FALSE(snapshot->step_left.has_value());
    CHECK(snapshot->elapsed.count() == Approx(13));
}

TEST_CASE("each step has its own counters and starts its speed afresh", "[unit]") {
    FakeClock clock;
    Progress progress(clock);
    progress.begin_step("Fingerprints", {.files = 2, .bytes = 2000});
    work_for(clock, progress, 2, {.files = 1, .bytes = 1000});
    CHECK(speed(progress) == Approx(1000));

    clock.advance(10s); // the time between steps counts for elapsed, not for the speed
    progress.begin_step("Hashes", {.files = 3, .bytes = 60'000});
    auto snapshot = progress.snapshot();
    CHECK(snapshot->phase == "Hashes");
    CHECK(snapshot->bytes_per_second == 0);
    CHECK(snapshot->step_done == Work{});
    CHECK(snapshot->done == Work{.files = 2, .bytes = 2000});
    // Without set_job_total(), the job's total is the finished work plus this step.
    CHECK(snapshot->total == Work{.files = 5, .bytes = 62'000});

    work_for(clock, progress, 1, {.files = 1, .bytes = 20'000});
    snapshot = progress.snapshot();
    CHECK(snapshot->bytes_per_second == Approx(20'000));
    CHECK(snapshot->step_done == Work{.files = 1, .bytes = 20'000});
    REQUIRE(snapshot->step_left.has_value());
    CHECK(snapshot->step_left->count() == Approx(2));
    CHECK(snapshot->elapsed.count() == Approx(13));
}

TEST_CASE("a step can find more work on the way", "[unit]") {
    FakeClock clock;
    Progress progress(clock);
    progress.begin_step("Hashing", bytes(1000));
    progress.add_total(bytes(500));
    progress.add_total(files(3));
    const auto snapshot = progress.snapshot();
    CHECK(snapshot->step_total == Work{.files = 3, .bytes = 1500});
    CHECK(snapshot->total == Work{.files = 3, .bytes = 1500});
}

TEST_CASE("a job total never falls below what the steps add up to", "[unit]") {
    FakeClock clock;
    Progress progress(clock);
    progress.set_job_total(bytes(1000));
    progress.begin_step("One", bytes(5000));
    CHECK(progress.snapshot()->total == bytes(5000));
}

TEST_CASE("a finished job stands still", "[unit]") {
    FakeClock clock;
    Progress progress(clock);
    progress.begin_step("Copying", bytes(1000));
    work_for(clock, progress, 2, bytes(500));

    SECTION("completed: nothing is left") {
        progress.finish(JobState::completed);
        const auto final_snapshot = progress.snapshot();
        CHECK(final_snapshot->state == JobState::completed);
        CHECK(final_snapshot->eta == sheepdog::jobs::Seconds(0));
        CHECK(final_snapshot->step_left == sheepdog::jobs::Seconds(0));

        clock.advance(1h);
        progress.add(bytes(1));
        progress.begin_step("Late", bytes(1));
        const auto later = progress.snapshot();
        CHECK(later == final_snapshot); // the very same object
        CHECK(later->elapsed.count() == Approx(2));
        CHECK(later->done == bytes(1000));
        CHECK(later->phase == "Copying");
    }
    SECTION("stopped: the time left is unknown") {
        progress.finish(JobState::cancelled);
        const auto final_snapshot = progress.snapshot();
        CHECK(final_snapshot->state == JobState::cancelled);
        CHECK_FALSE(final_snapshot->eta.has_value());
        CHECK_FALSE(final_snapshot->step_left.has_value());
        progress.finish(JobState::failed); // the first end counts
        CHECK(progress.snapshot()->state == JobState::cancelled);
    }
}

TEST_CASE("a snapshot does not change once it is published", "[unit]") {
    FakeClock clock;
    Progress progress(clock);
    progress.begin_step("One", bytes(100));
    work_for(clock, progress, 1, bytes(10));
    const std::shared_ptr<const Snapshot> kept = progress.snapshot();

    work_for(clock, progress, 1, bytes(10));
    progress.begin_step("Two", bytes(100));
    const auto fresh = progress.snapshot();
    CHECK(fresh != kept);
    CHECK(fresh->done == bytes(20));
    CHECK(kept->done == bytes(10));
    CHECK(kept->phase == "One");
    CHECK(kept->elapsed.count() == Approx(1));
}
