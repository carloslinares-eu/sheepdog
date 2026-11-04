#pragma once

// Result<T>: the return type of every platform call. It holds either a value or an Error
// (a std::error_code plus the path it concerns). No exceptions are thrown, so results can be
// passed between threads safely.

#include "core/platform/native_path.hpp"

#include <exception>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace sheepdog::platform {

struct Error {
    std::error_code code;
    NativePath path;
    // Optional extra text, such as SQLite's own message or advice on what to do next.
    // The {} stops GCC's -Wmissing-field-initializers for Error{.code, .path}.
    std::string detail{}; // NOLINT(readability-redundant-member-init)

    // "<message>: <detail> (<path as display text>)", for logs and the UI.
    [[nodiscard]] std::string message() const;
};

// An Error from errno (POSIX) or GetLastError() (Windows) for the given path.
[[nodiscard]] Error last_error(NativePath path);

// True when the error means "no such file or folder" on this platform.
[[nodiscard]] bool is_not_found(const std::error_code& code) noexcept;

template <class T> class [[nodiscard]] Result {
    static_assert(!std::is_same_v<T, Error>, "Result<Error> is ambiguous");

public:
    Result(T value) : data_(std::in_place_index<0>, std::move(value)) {}     // NOLINT: implicit
    Result(Error error) : data_(std::in_place_index<1>, std::move(error)) {} // NOLINT: implicit

    [[nodiscard]] bool has_value() const noexcept { return data_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    // Calling value() on an error is a programming error and terminates.
    [[nodiscard]] T& value() & { return *get(); }
    [[nodiscard]] const T& value() const& { return *get(); }
    [[nodiscard]] T&& value() && { return std::move(*get()); }
    [[nodiscard]] T& operator*() & { return *get(); }
    [[nodiscard]] const T& operator*() const& { return *get(); }
    [[nodiscard]] T* operator->() { return get(); }
    [[nodiscard]] const T* operator->() const { return get(); }

    // Calling error() on a value terminates.
    [[nodiscard]] const Error& error() const {
        if (const Error* e = std::get_if<1>(&data_)) {
            return *e;
        }
        std::terminate();
    }

private:
    // Written so that the compiler sees a non-null pointer on every path that returns.
    [[nodiscard]] T* get() {
        if (T* v = std::get_if<0>(&data_)) {
            return v;
        }
        std::terminate();
    }
    [[nodiscard]] const T* get() const {
        if (const T* v = std::get_if<0>(&data_)) {
            return v;
        }
        std::terminate();
    }

    std::variant<T, Error> data_;
};

// Result<void>: success, or an Error.
struct Ok {};
template <> class [[nodiscard]] Result<void> {
public:
    Result(Ok /*ok*/) {}                                             // NOLINT: implicit
    Result(Error error) : error_(std::move(error)), failed_(true) {} // NOLINT: implicit

    [[nodiscard]] bool has_value() const noexcept { return !failed_; }
    explicit operator bool() const noexcept { return !failed_; }
    [[nodiscard]] const Error& error() const {
        if (!failed_) {
            std::terminate();
        }
        return error_;
    }

private:
    Error error_;
    bool failed_ = false;
};

} // namespace sheepdog::platform
