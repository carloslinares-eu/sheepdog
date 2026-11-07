#include "core/jobs/errors.hpp"

namespace sheepdog::jobs {

namespace {

class JobCategory final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "job"; }
    [[nodiscard]] std::string message(int code) const override {
        switch (static_cast<JobErrc>(code)) {
        case JobErrc::cancelled:
            return "the job was stopped";
        case JobErrc::busy:
            return "another job that changes files is still running";
        case JobErrc::exception:
            return "a job failed with an exception";
        }
        return "unknown job error";
    }
};

} // namespace

const std::error_category& job_category() noexcept {
    static const JobCategory category;
    return category;
}

std::error_code make_error_code(JobErrc code) noexcept {
    return {static_cast<int>(code), job_category()};
}

Error job_error(JobErrc code, std::string detail) {
    return Error{.code = make_error_code(code), .path = {}, .detail = std::move(detail)};
}

void ErrorLog::add(Error error) {
    if (error.code == JobErrc::cancelled) {
        return;
    }
    const std::scoped_lock lock(mutex_);
    ++count_;
    if (errors_.size() < capacity_) {
        errors_.push_back(std::move(error));
    }
}

std::uint64_t ErrorLog::count() const {
    const std::scoped_lock lock(mutex_);
    return count_;
}

std::vector<Error> ErrorLog::kept() const {
    const std::scoped_lock lock(mutex_);
    return errors_;
}

} // namespace sheepdog::jobs
