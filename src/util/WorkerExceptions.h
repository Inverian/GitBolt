#pragma once

#include <QStringList>

#include <exception>
#include <functional>

namespace gitbolt::util {

/// What an exception escaping background work (AsyncRunner) is, as a
/// fixed token: the most derived standard type it is ("out_of_range",
/// "bad_alloc", "filesystem_error", …), "std_exception" for another
/// std::exception, "non_std" for null (anything else thrown). Never
/// what(), which can hold paths and git's error text.
const char* workerExceptionKind(const std::exception* exception);

/// Every token workerExceptionKind() returns, for the telemetry
/// allowlist and its tests.
QStringList workerExceptionKinds();

/// Where reportWorkerException() sends the token; an empty function
/// clears it. main() points it at usage statistics and clears it
/// before the telemetry service goes. Thread-safe.
void setWorkerExceptionSink(std::function<void(const char* kind)> sink);

/// Logs the exception (its type and what(): the local log and stderr
/// only) and hands its token to the sink, on the calling thread.
void reportWorkerException(const std::exception* exception);

} // namespace gitbolt::util
