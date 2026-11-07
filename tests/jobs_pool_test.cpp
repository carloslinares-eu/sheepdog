// BoundedPool and TaskGroup: the queue cap, the limits per volume, stops and error collection.
// Catch2 assertions are not thread-safe: tasks only record what happened, in atomics, and the
// test thread checks it afterwards.
#include "core/jobs/pool.hpp"
#include "support/jobs_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using sheepdog::jobs::BoundedPool;
using sheepdog::jobs::ErrorLog;
using sheepdog::jobs::JobErrc;
using sheepdog::jobs::kHddLimit;
using sheepdog::jobs::kKeptErrors;
using sheepdog::jobs::kQueuePerWorker;
using sheepdog::jobs::kSsdLimit;
using sheepdog::jobs::Ok;
using sheepdog::jobs::Result;
using sheepdog::jobs::TaskGroup;
using sheepdog::jobs::volume_limit;
using sheepdog::platform::Media;
using sheepdog::test::eventually;
using sheepdog::test::Gate;
using sheepdog::test::volume;

namespace {

// Counts the tasks inside it and the most there ever were.
class Occupancy {
public:
    void enter() {
        const int now = ++inside_;
        int peak = peak_.load();
        while (now > peak && !peak_.compare_exchange_weak(peak, now)) {
        }
    }
    void leave() { --inside_; }
    [[nodiscard]] int inside() const { return inside_.load(); }
    [[nodiscard]] int peak() const { return peak_.load(); }

private:
    std::atomic<int> inside_{0};
    std::atomic<int> peak_{0};
};

} // namespace

TEST_CASE("volume limits follow the kind of disk", "[unit]") {
    CHECK(kHddLimit == 2);
    CHECK(kSsdLimit == 8);
    CHECK(volume_limit(Media::hdd) == 2);
    CHECK(volume_limit(Media::ssd) == 8);
    CHECK(volume_limit(Media::unknown) == 2);
}

TEST_CASE("the queue holds four tasks per worker", "[unit]") {
    const BoundedPool pool(3);
    CHECK(pool.worker_count() == 3);
    CHECK(pool.queue_capacity() == 12);
    CHECK(kQueuePerWorker == 4);
    CHECK(BoundedPool(0).worker_count() == 1);
    CHECK(BoundedPool().worker_count() >= 1);
}

TEST_CASE("submitting a million tasks never exceeds the queue cap", "[unit][slow]") {
    constexpr std::uint64_t kTasks = 1'000'000;
    constexpr std::uint64_t kGateEvery = 100'000;

    BoundedPool pool(4);
    ErrorLog errors;
    std::atomic<std::uint64_t> started{0};
    // Short tasks would keep the queue nearly empty. So each round begins with one gate task per
    // worker, which wait until the producer has filled the queue: that puts the cap to the test.
    // The queue is full only until the first gate task leaves, and a slower one might never see
    // it, so the first to see it opens the round for all of them.
    std::array<std::atomic<bool>, kTasks / kGateEvery> round_filled{};
    std::atomic<bool> gave_up{false}; // a queue that never fills: fail, and do not wait again
    std::uint64_t most_ahead = 0;
    std::uint64_t refused = 0;
    const auto disk = volume("ssd", Media::ssd);
    {
        TaskGroup tasks(pool, std::stop_token{}, errors);
        for (std::uint64_t i = 0; i < kTasks; ++i) {
            const bool gate = i % kGateEvery < pool.worker_count();
            auto& filled = round_filled[i / kGateEvery];
            const bool queued = tasks.submit(disk, [&, gate]() -> Result<void> {
                started.fetch_add(1, std::memory_order_relaxed);
                const auto queue_was_full = [&] {
                    if (pool.stats().queued == pool.queue_capacity()) {
                        filled = true;
                    }
                    return filled.load();
                };
                if (gate && !gave_up.load() && !eventually(queue_was_full)) {
                    gave_up = true;
                }
                return Ok{};
            });
            refused += queued ? 0 : 1;
            // Measured without the pool's help: what was accepted and has not begun.
            most_ahead = std::max(most_ahead, i + 1 - started.load(std::memory_order_relaxed));
        }
    }
    CHECK_FALSE(gave_up.load());
    CHECK(std::ranges::all_of(round_filled, [](const auto& filled) { return filled.load(); }));
    CHECK(refused == 0);
    CHECK(started.load() == kTasks);
    REQUIRE(eventually([&] { return pool.stats().completed == kTasks; }));
    CHECK(pool.stats().queued == 0);
    CHECK(pool.stats().peak_queued == pool.queue_capacity());
    // A worker counts a task as started a moment after taking it from the queue.
    CHECK(most_ahead <= pool.queue_capacity() + pool.worker_count());
    CHECK(errors.count() == 0);
}

TEST_CASE("a full queue blocks the producer until a stop releases it", "[unit]") {
    BoundedPool pool(2);
    ErrorLog errors;
    std::stop_source stop;
    Gate gate;
    std::atomic<int> ran{0};
    const auto disk = volume("ssd", Media::ssd);
    TaskGroup tasks(pool, stop.get_token(), errors);

    const auto hold = [&]() -> Result<void> {
        gate.pass();
        return Ok{};
    };
    const auto count = [&]() -> Result<void> {
        ++ran;
        return Ok{};
    };
    CHECK(tasks.submit(disk, hold));
    CHECK(tasks.submit(disk, hold));
    REQUIRE(eventually([&] { return pool.stats().running == 2; }));
    for (std::size_t i = 0; i < pool.queue_capacity(); ++i) {
        CHECK(tasks.submit(disk, count));
    }
    CHECK(pool.stats().queued == pool.queue_capacity());

    std::atomic<bool> returned{false};
    std::atomic<bool> accepted{true};
    std::jthread producer([&] {
        accepted = tasks.submit(disk, count);
        returned = true;
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(returned.load());
    CHECK(pool.stats().queued == pool.queue_capacity());

    stop.request_stop();
    REQUIRE(eventually([&] { return returned.load(); }));
    CHECK_FALSE(accepted.load());
    CHECK_FALSE(tasks.submit(disk, count)); // a stopped group takes nothing more
    CHECK(pool.stats().queued == pool.queue_capacity());

    gate.open();
    tasks.wait();
    CHECK(ran.load() == 0); // the stop came before they could start
    CHECK(pool.stats().peak_queued == pool.queue_capacity());
    CHECK(errors.count() == 0);
}

TEST_CASE("room in the queue wakes a blocked producer", "[unit]") {
    BoundedPool pool(1);
    ErrorLog errors;
    Gate gate;
    std::atomic<int> ran{0};
    const auto disk = volume("hdd", Media::hdd);
    TaskGroup tasks(pool, std::stop_token{}, errors);

    CHECK(tasks.submit(disk, [&]() -> Result<void> {
        gate.pass();
        return Ok{};
    }));
    REQUIRE(eventually([&] { return pool.stats().running == 1; }));
    const auto total = static_cast<int>(pool.queue_capacity()) * 3;
    std::jthread producer([&] {
        for (int i = 0; i < total; ++i) {
            (void)tasks.submit(disk, [&]() -> Result<void> {
                ++ran;
                return Ok{};
            });
        }
    });
    REQUIRE(eventually([&] { return pool.stats().queued == pool.queue_capacity(); }));
    gate.open();
    REQUIRE(eventually([&] { return ran.load() == total; }));
    producer.join();
    tasks.wait();
    CHECK(pool.stats().peak_queued <= pool.queue_capacity());
}

TEST_CASE("tasks of one volume stay within its limit", "[unit]") {
    BoundedPool pool(12);
    ErrorLog errors;
    Occupancy hdd;
    Occupancy ssd;
    const auto spinning = volume("hdd", Media::hdd);
    const auto solid = volume("ssd", Media::ssd);
    {
        TaskGroup tasks(pool, std::stop_token{}, errors);
        for (int i = 0; i < 500; ++i) {
            const bool to_hdd = i % 2 == 0;
            Occupancy& occupancy = to_hdd ? hdd : ssd;
            CHECK(tasks.submit(to_hdd ? spinning : solid, [&occupancy]() -> Result<void> {
                occupancy.enter();
                // Busy, because Sleep() on Windows cannot wait this little.
                const auto end = std::chrono::steady_clock::now() + 200us;
                while (std::chrono::steady_clock::now() < end) {
                    std::this_thread::yield();
                }
                occupancy.leave();
                return Ok{};
            }));
        }
    }
    CHECK(hdd.peak() <= 2);
    CHECK(ssd.peak() <= 8);
}

TEST_CASE("an SSD gets eight tasks at a time, and no more", "[unit]") {
    BoundedPool pool(12);
    ErrorLog errors;
    Occupancy occupancy;
    Gate gate;
    const auto disk = volume("ssd", Media::ssd);
    TaskGroup tasks(pool, std::stop_token{}, errors);
    for (int i = 0; i < 12; ++i) {
        CHECK(tasks.submit(disk, [&]() -> Result<void> {
            occupancy.enter();
            gate.pass();
            occupancy.leave();
            return Ok{};
        }));
    }
    REQUIRE(eventually([&] { return occupancy.inside() == 8; }));
    std::this_thread::sleep_for(50ms); // spare workers exist; none may run a ninth task
    CHECK(occupancy.inside() == 8);
    CHECK(pool.stats().queued == 4);
    gate.open();
    tasks.wait();
    CHECK(occupancy.peak() == 8);
}

TEST_CASE("a volume at its limit holds back only its own tasks", "[unit]") {
    BoundedPool pool(4);
    ErrorLog errors;
    Occupancy hdd;
    Gate gate;
    std::atomic<int> others{0};
    const auto slow = volume("slow", Media::hdd);
    const auto fast = volume("fast", Media::ssd);
    TaskGroup tasks(pool, std::stop_token{}, errors);
    for (int i = 0; i < 5; ++i) {
        CHECK(tasks.submit(slow, [&]() -> Result<void> {
            hdd.enter();
            gate.pass();
            hdd.leave();
            return Ok{};
        }));
    }
    REQUIRE(eventually([&] { return hdd.inside() == 2; }));
    // The queue starts with three tasks that cannot run; the new ones must pass them.
    for (int i = 0; i < 6; ++i) {
        CHECK(tasks.submit(fast, [&]() -> Result<void> {
            ++others;
            return Ok{};
        }));
    }
    REQUIRE(eventually([&] { return others.load() == 6; }));
    CHECK(hdd.inside() == 2);
    CHECK(pool.stats().queued == 3);
    gate.open();
    tasks.wait();
    CHECK(hdd.peak() == 2);
    REQUIRE(eventually([&] { return pool.stats().completed == 11; }));
}

TEST_CASE("the tasks of a volume start in the order they came", "[unit]") {
    BoundedPool pool(1);
    ErrorLog errors;
    std::vector<int> order;
    const auto disk = volume("hdd", Media::hdd);
    {
        TaskGroup tasks(pool, std::stop_token{}, errors);
        for (int i = 0; i < 1000; ++i) {
            CHECK(tasks.submit(disk, [&order, i]() -> Result<void> { // one worker: no race
                order.push_back(i);
                return Ok{};
            }));
        }
    }
    REQUIRE(order.size() == 1000);
    CHECK(std::is_sorted(order.begin(), order.end()));
}

TEST_CASE("wait() waits for the group's own tasks only", "[unit]") {
    BoundedPool pool(2);
    ErrorLog errors;
    Gate gate;
    const auto disk = volume("ssd", Media::ssd);
    TaskGroup blocked(pool, std::stop_token{}, errors);
    CHECK(blocked.submit(disk, [&]() -> Result<void> {
        gate.pass();
        return Ok{};
    }));

    std::atomic<int> ran{0};
    {
        TaskGroup quick(pool, std::stop_token{}, errors);
        for (int i = 0; i < 50; ++i) {
            CHECK(quick.submit(disk, [&]() -> Result<void> {
                ++ran;
                return Ok{};
            }));
        }
        quick.wait();
        CHECK(ran.load() == 50);
    }
    gate.open();
}

TEST_CASE("failing and throwing tasks end up in the log", "[unit]") {
    BoundedPool pool(4);
    ErrorLog errors;
    const auto disk = volume("ssd", Media::ssd);
    {
        TaskGroup tasks(pool, std::stop_token{}, errors);
        CHECK(tasks.submit(disk, []() -> Result<void> {
            return sheepdog::jobs::job_error(JobErrc::busy, "a file failed");
        }));
        tasks.wait();
        CHECK(tasks.submit(disk, []() -> Result<void> { throw std::runtime_error("boom"); }));
        tasks.wait();
        CHECK(tasks.submit(disk, []() -> Result<void> { throw 42; }));
        tasks.wait();
        // A stop is nobody's error.
        CHECK(tasks.submit(
            disk, []() -> Result<void> { return sheepdog::jobs::job_error(JobErrc::cancelled); }));
    }
    REQUIRE(eventually([&] { return pool.stats().completed == 4; })); // the workers survived
    REQUIRE(errors.count() == 3);
    const auto list = errors.kept();
    REQUIRE(list.size() == 3);
    CHECK(list[0].detail == "a file failed");
    CHECK(list[1].code == JobErrc::exception);
    CHECK(list[1].detail == "boom");
    CHECK(list[2].code == JobErrc::exception);
    CHECK(list[2].detail == "unknown exception");
}

TEST_CASE("the log keeps the first errors and counts them all", "[unit]") {
    BoundedPool pool(4);
    ErrorLog errors;
    const int total = static_cast<int>(kKeptErrors) + 500;
    const auto disk = volume("ssd", Media::ssd);
    {
        TaskGroup tasks(pool, std::stop_token{}, errors);
        for (int i = 0; i < total; ++i) {
            CHECK(tasks.submit(disk, [i]() -> Result<void> {
                return sheepdog::jobs::job_error(JobErrc::busy, std::to_string(i));
            }));
        }
    }
    CHECK(errors.count() == static_cast<std::uint64_t>(total));
    CHECK(errors.kept().size() == kKeptErrors);

    ErrorLog small(2);
    for (const char* text : {"one", "two", "three"}) {
        small.add(sheepdog::jobs::job_error(JobErrc::busy, text));
    }
    CHECK(small.count() == 3);
    REQUIRE(small.kept().size() == 2);
    CHECK(small.kept()[0].detail == "one");
    CHECK(small.kept()[1].detail == "two");
    CHECK(sheepdog::jobs::job_error(JobErrc::busy).message().find("still running") !=
          std::string::npos);
}

TEST_CASE("a task's captures are gone when wait() returns", "[unit]") {
    BoundedPool pool(2);
    ErrorLog errors;
    const auto disk = volume("ssd", Media::ssd);
    TaskGroup tasks(pool, std::stop_token{}, errors);

    // A capture that takes its time to go: wait() cannot overtake it by chance.
    std::atomic<bool> destroyed{false};
    std::shared_ptr<int> capture(new int(0), [&destroyed](const int* value) {
        std::this_thread::sleep_for(20ms);
        delete value;
        destroyed = true;
    });
    CHECK(tasks.submit(disk, [capture = std::move(capture)]() -> Result<void> { return Ok{}; }));
    tasks.wait();
    CHECK(destroyed.load());
}
