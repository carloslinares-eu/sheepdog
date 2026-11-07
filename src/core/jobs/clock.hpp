#pragma once

// The time source of the job framework. Whatever measures time takes a Clock, so tests can drive
// the progress maths with a fake clock instead of sleeping.

#include <chrono>

namespace sheepdog::jobs {

class Clock {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Duration = std::chrono::steady_clock::duration;

    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    virtual ~Clock() = default;

    // Never runs backwards. Called from several threads at once.
    [[nodiscard]] virtual TimePoint now() const = 0;
};

// The real, monotonic clock. It lives as long as the program.
[[nodiscard]] const Clock& system_clock() noexcept;

} // namespace sheepdog::jobs
