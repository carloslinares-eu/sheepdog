#pragma once

// Reading a file in chunks of 1 MiB. Long reads are what a stop has to interrupt, so every job
// that reads file contents goes through read_chunks(): a stop takes effect within one chunk.

#include "core/jobs/errors.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>

namespace sheepdog::jobs {

inline constexpr std::size_t kChunkSize = std::size_t{1024} * 1024;

// platform::FileReader, or a fake one in tests: read() fills the buffer and returns how many
// bytes it put there, 0 at the end.
template <class Source>
concept ChunkSource = requires(Source& source, std::span<std::byte> buffer) {
    { source.read(buffer) } -> std::same_as<Result<std::size_t>>;
};

// Reads `source` to its end and hands each chunk to sink(std::span<const std::byte>), which
// returns Result<void>. Returns the number of bytes read. The stop token is checked before every
// read: the chunk being read when a stop arrives is still delivered, the next one is not, and the
// result is then a JobErrc::cancelled error. The first error of `source` or `sink` ends the read.
template <ChunkSource Source, class Sink>
[[nodiscard]] Result<std::uint64_t> read_chunks(Source& source, const std::stop_token& stop,
                                                Sink&& sink) {
    const auto buffer = std::make_unique_for_overwrite<std::byte[]>(kChunkSize);
    std::uint64_t total = 0;
    for (;;) {
        if (stop.stop_requested()) {
            return job_error(JobErrc::cancelled);
        }
        const Result<std::size_t> got = source.read(std::span<std::byte>(buffer.get(), kChunkSize));
        if (!got) {
            return got.error();
        }
        if (*got == 0) {
            return total;
        }
        const Result<void> taken = sink(std::span<const std::byte>(buffer.get(), *got));
        if (!taken) {
            return taken.error();
        }
        total += *got;
    }
}

} // namespace sheepdog::jobs
