#include "core/jobs/clock.hpp"

namespace sheepdog::jobs {

namespace {

class SteadyClock final : public Clock {
public:
    [[nodiscard]] TimePoint now() const override { return std::chrono::steady_clock::now(); }
};

} // namespace

const Clock& system_clock() noexcept {
    static const SteadyClock clock;
    return clock;
}

} // namespace sheepdog::jobs
