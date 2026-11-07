// read_chunks(), and how a stop reaches a job in the middle of a read. Fake readers make the
// timing exact: one sleeps through every chunk, the other holds a chunk open for the test.
// Catch2 assertions are not thread-safe: job bodies only record what happened, and the test
// thread checks it afterwards.
#include "core/jobs/chunks.hpp"
#include "core/jobs/job.hpp"
#include "core/platform/fs.hpp"
#include "support/jobs_support.hpp"
#include "support/temp_tree.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using sheepdog::jobs::ChunkSource;
using sheepdog::jobs::job_error;
using sheepdog::jobs::JobContext;
using sheepdog::jobs::JobErrc;
using sheepdog::jobs::JobKind;
using sheepdog::jobs::JobRunner;
using sheepdog::jobs::JobState;
using sheepdog::jobs::kChunkSize;
using sheepdog::jobs::Ok;
using sheepdog::jobs::read_chunks;
using sheepdog::jobs::Result;
using sheepdog::jobs::system_clock;
using sheepdog::platform::FileReader;
using sheepdog::platform::Media;
using sheepdog::test::eventually;
using sheepdog::test::Gate;
using sheepdog::test::start_job;
using sheepdog::test::TempDir;
using sheepdog::test::volume;
using sheepdog::test::write_file;

namespace {

// Delivers `chunks` chunks of 1 MiB, taking `delay` for each. Counts the reads begun.
class SlowReader {
public:
    SlowReader(int chunks, std::chrono::microseconds delay) : left_(chunks), delay_(delay) {}

    Result<std::size_t> read(std::span<std::byte> buffer) {
        ++reads_;
        std::this_thread::sleep_for(delay_);
        if (left_ == 0) {
            return std::size_t{0};
        }
        --left_;
        return std::min(buffer.size(), kChunkSize);
    }

    [[nodiscard]] int reads() const { return reads_.load(); }

private:
    std::atomic<int> reads_{0};
    int left_;
    std::chrono::microseconds delay_;
};

// Delivers 100 chunks and blocks inside read number `hold_at` until the test opens it, so a test
// can put a stop right into a chunk.
class GatedReader {
public:
    explicit GatedReader(int hold_at) : hold_at_(hold_at) {}

    Result<std::size_t> read(std::span<std::byte> buffer) {
        const int read_number = ++reads_;
        if (read_number == hold_at_) {
            inside_gate_ = true;
            gate_.pass();
        }
        return read_number > 100 ? std::size_t{0} : std::min(buffer.size(), kChunkSize);
    }

    [[nodiscard]] bool inside_gate() const { return inside_gate_.load(); }
    void open() { gate_.open(); }
    [[nodiscard]] int reads() const { return reads_.load(); }

private:
    int hold_at_;
    std::atomic<int> reads_{0};
    std::atomic<bool> inside_gate_{false};
    Gate gate_;
};

static_assert(ChunkSource<FileReader>);
static_assert(ChunkSource<SlowReader>);

auto count_chunks(std::uint64_t& bytes) {
    return [&bytes](std::span<const std::byte> chunk) -> Result<void> {
        CHECK(chunk.size() <= kChunkSize);
        bytes += chunk.size();
        return Ok{};
    };
}

// What a job or task that reads a file does: it reports every chunk as progress.
template <class Reader> Result<void> read_file(Reader& reader, const JobContext& context) {
    const auto read = read_chunks(reader, context.stop_token(),
                                  [&](std::span<const std::byte> chunk) -> Result<void> {
                                      context.progress().add({.files = 0, .bytes = chunk.size()});
                                      return Ok{};
                                  });
    if (!read) {
        return read.error();
    }
    return Ok{};
}

} // namespace

TEST_CASE("a stop takes effect within one 1 MiB chunk", "[unit]") {
    JobRunner runner;

    SECTION("a chunk under way is finished, and no other begins") {
        GatedReader reader(3);
        auto job = start_job(runner, JobKind::read_only,
                             [&](JobContext& context) { return read_file(reader, context); });
        REQUIRE(eventually([&] { return reader.inside_gate(); }));
        job->request_stop();
        reader.open();
        const auto result = job->wait();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == JobErrc::cancelled);
        CHECK(job->snapshot()->state == JobState::cancelled);
        CHECK(job->errors().count() == 0);
        CHECK(reader.reads() == 3);
        CHECK(job->snapshot()->done.bytes == 3 * kChunkSize);
    }
    SECTION("a slow reader stops at any moment") {
        for (const auto wait_first : {0ms, 1ms, 7ms, 23ms}) {
            SlowReader reader(1000, 2ms); // two seconds at the very least, if it were not stopped
            auto job = start_job(runner, JobKind::read_only,
                                 [&](JobContext& context) { return read_file(reader, context); });
            std::this_thread::sleep_for(wait_first);
            job->request_stop();
            const int reads_at_stop = reader.reads();
            const auto result = job->wait();
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == JobErrc::cancelled);
            // The check before a read may just have passed: one read can still begin.
            CHECK(reader.reads() <= reads_at_stop + 1);
            CHECK(job->snapshot()->done.bytes <=
                  (static_cast<std::uint64_t>(reads_at_stop) + 1) * kChunkSize);
        }
    }
}

TEST_CASE("a stop reaches the tasks reading in the pool", "[unit]") {
    constexpr int kReaders = 4 + 16; // what the workers run, and what fills the queue
    JobRunner runner(system_clock(), 4);
    std::vector<std::unique_ptr<SlowReader>> readers;
    readers.reserve(kReaders);
    for (int i = 0; i < kReaders; ++i) {
        readers.push_back(std::make_unique<SlowReader>(10'000, 1ms));
    }
    const auto disk = volume("ssd", Media::ssd);
    auto job = start_job(runner, JobKind::read_only, [&](JobContext& context) -> Result<void> {
        auto tasks = context.tasks();
        for (const auto& reader : readers) {
            const bool queued = tasks.submit(
                disk, [&context, source = reader.get()] { return read_file(*source, context); });
            if (!queued) {
                break;
            }
        }
        return Ok{};
    });
    REQUIRE(eventually([&] {
        return std::count_if(readers.begin(), readers.end(),
                             [](const auto& r) { return r->reads() > 1; }) >= 4;
    }));
    job->request_stop();
    std::vector<int> at_stop;
    at_stop.reserve(readers.size());
    for (const auto& reader : readers) {
        at_stop.push_back(reader->reads());
    }
    CHECK_FALSE(job->wait().has_value());
    CHECK(job->snapshot()->state == JobState::cancelled);
    CHECK(job->errors().count() == 0);
    REQUIRE(eventually([&] { return runner.pool().stats().running == 0; }));
    CHECK(runner.pool().stats().queued == 0);
    int started = 0;
    for (std::size_t i = 0; i < readers.size(); ++i) {
        CHECK(readers[i]->reads() <= at_stop[i] + 1);
        started += readers[i]->reads() > 0 ? 1 : 0;
    }
    CHECK(started == 4); // those in the queue never began
}

TEST_CASE("read_chunks reads to the end", "[unit]") {
    SECTION("empty") {
        SlowReader reader(0, 0us);
        std::uint64_t bytes = 0;
        const auto read = read_chunks(reader, std::stop_token{}, count_chunks(bytes));
        REQUIRE(read.has_value());
        CHECK(*read == 0);
        CHECK(reader.reads() == 1);
    }
    SECTION("whole chunks") {
        SlowReader reader(5, 0us);
        std::uint64_t bytes = 0;
        const auto read = read_chunks(reader, std::stop_token{}, count_chunks(bytes));
        REQUIRE(read.has_value());
        CHECK(*read == 5 * kChunkSize);
        CHECK(bytes == 5 * kChunkSize);
    }
    SECTION("stopped before it began") {
        SlowReader reader(5, 0us);
        std::stop_source stop;
        stop.request_stop();
        std::uint64_t bytes = 0;
        const auto read = read_chunks(reader, stop.get_token(), count_chunks(bytes));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().code == JobErrc::cancelled);
        CHECK(reader.reads() == 0);
        CHECK(bytes == 0);
    }
}

TEST_CASE("read_chunks passes errors on", "[unit]") {
    class FailingReader {
    public:
        Result<std::size_t> read(std::span<std::byte> buffer) {
            if (++reads_ > 2) {
                return job_error(JobErrc::busy, "read failed");
            }
            return std::min(buffer.size(), kChunkSize);
        }

    private:
        int reads_ = 0;
    };

    SECTION("from the source") {
        FailingReader reader;
        std::uint64_t bytes = 0;
        const auto read = read_chunks(reader, std::stop_token{}, count_chunks(bytes));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().detail == "read failed");
        CHECK(bytes == 2 * kChunkSize);
    }
    SECTION("from the sink") {
        SlowReader reader(5, 0us);
        const auto read =
            read_chunks(reader, std::stop_token{}, [](std::span<const std::byte>) -> Result<void> {
                return job_error(JobErrc::busy, "write failed");
            });
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().detail == "write failed");
        CHECK(reader.reads() == 1);
    }
}

TEST_CASE("read_chunks reads a real file", "[unit][fs]") {
    const TempDir tmp;
    const auto path = tmp / "big.bin";
    std::string content(2 * kChunkSize + 12345, '\0');
    for (std::size_t i = 0; i < content.size(); ++i) {
        content[i] = static_cast<char>(i * 31 % 251);
    }
    write_file(path, content);

    auto reader = FileReader::open(path);
    REQUIRE(reader.has_value());
    std::string seen;
    std::vector<std::size_t> chunks;
    seen.reserve(content.size());
    const auto read = read_chunks(
        *reader, std::stop_token{}, [&](std::span<const std::byte> chunk) -> Result<void> {
            seen.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
            chunks.push_back(chunk.size());
            return Ok{};
        });
    REQUIRE(read.has_value());
    CHECK(*read == content.size());
    CHECK(seen == content);
    CHECK(chunks == std::vector<std::size_t>{1024 * 1024, 1024 * 1024, 12345}); // 1 MiB each
}
