#pragma once

// The error codes of the job framework and ErrorLog, the list that collects what went wrong
// during a job. No exception crosses a thread: job bodies and tasks are called through
// catch_exceptions(), and whatever fails ends up in the job's ErrorLog.

#include "core/platform/result.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace sheepdog::jobs {

enum class JobErrc : std::uint8_t {
    cancelled = 1, // a stop was requested; not a failure, so ErrorLog does not keep it
    busy,          // another job that changes files is running
    exception,     // a job or task threw; the detail holds what()
};

} // namespace sheepdog::jobs

template <> struct std::is_error_code_enum<sheepdog::jobs::JobErrc> : std::true_type {};

namespace sheepdog::jobs {

using platform::Error;
using platform::Ok;
using platform::Result;

[[nodiscard]] const std::error_category& job_category() noexcept;
[[nodiscard]] std::error_code make_error_code(JobErrc code) noexcept;

// An Error that concerns no particular file.
[[nodiscard]] Error job_error(JobErrc code, std::string detail = {});

// How many errors one job keeps. A job can fail on every file of a million; the first thousand
// say all there is to say, and count() still tells how many there were.
inline constexpr std::size_t kKeptErrors = 1000;

// Safe to use from any thread.
class ErrorLog {
public:
    explicit ErrorLog(std::size_t capacity = kKeptErrors) : capacity_(capacity) {}

    // Keeps `error` unless the log is full. A JobErrc::cancelled error is ignored: a stop is what
    // somebody asked for, not something that went wrong.
    void add(Error error);

    // All errors added so far, also those not kept.
    [[nodiscard]] std::uint64_t count() const;
    // The first errors, in the order they came.
    [[nodiscard]] std::vector<Error> kept() const;

private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::vector<Error> errors_;
    std::uint64_t count_ = 0;
};

// Returns fn()'s Result<void>; an exception comes back as a JobErrc::exception error.
template <class Fn> [[nodiscard]] Result<void> catch_exceptions(Fn&& fn) {
    try {
        return std::forward<Fn>(fn)();
    } catch (const std::exception& e) {
        return job_error(JobErrc::exception, e.what());
    } catch (...) {
        return job_error(JobErrc::exception, "unknown exception");
    }
}

} // namespace sheepdog::jobs
