#pragma once

#include <string>
#include <system_error>
#include <variant>

namespace gitbolt::git {

enum class GitErrorCode {
    Ok = 0,
    GenericError,
    NotFound,
    Exists,
    Ambiguous,
    BufferTooShort,
    User,
    BareRepo,
    UnbornBranch,
    Unmerged,
    NonFastForward,
    InvalidSpec,
    Conflict,
    Locked,
    Modified,
    Auth,
    Certificate,
    Applied,
    Peel,
    Eof,
    Invalid,
    Uncommitted,
    Directory,
    MergeConflict,
    ProcessFailed,
    CheckoutFailed,     // clone fetched, but its checkout or post-checkout hook failed; repo kept
};

class GitError {
public:
    GitError() = default;
    GitError(GitErrorCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    static GitError fromLibgit2(int error_code);
    static GitError fromProcess(int exit_code, const std::string& stderr_output);

    GitErrorCode code() const { return code_; }
    const std::string& message() const { return message_; }
    bool ok() const { return code_ == GitErrorCode::Ok; }
    explicit operator bool() const { return !ok(); }

private:
    GitErrorCode code_ = GitErrorCode::Ok;
    std::string message_;
};

template <typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(GitError error) : data_(std::move(error)) {}

    bool ok() const { return std::holds_alternative<T>(data_); }
    explicit operator bool() const { return ok(); }

    const T& value() const& { return std::get<T>(data_); }
    T& value() & { return std::get<T>(data_); }
    T&& value() && { return std::get<T>(std::move(data_)); }

    const GitError& error() const { return std::get<GitError>(data_); }

    const T& operator*() const& { return value(); }
    T& operator*() & { return value(); }
    T&& operator*() && { return std::move(value()); }

    const T* operator->() const { return &value(); }
    T* operator->() { return &value(); }

private:
    std::variant<T, GitError> data_;
};

template <>
class Result<void> {
public:
    Result() : error_() {}
    Result(GitError error) : error_(std::move(error)), has_error_(true) {}

    bool ok() const { return !has_error_; }
    explicit operator bool() const { return ok(); }
    const GitError& error() const { return error_; }

    static Result success() { return Result(); }

private:
    GitError error_;
    bool has_error_ = false;
};

} // namespace gitbolt::git
