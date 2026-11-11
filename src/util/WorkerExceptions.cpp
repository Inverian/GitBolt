#include "util/WorkerExceptions.h"

#include <QMutex>
#include <QMutexLocker>
#include <QtGlobal>

#include <any>
#include <filesystem>
#include <future>
#include <memory>
#include <new>
#include <optional>
#include <regex>
#include <stdexcept>
#include <system_error>
#include <typeinfo>
#include <variant>

namespace gitbolt::util {

namespace {

template <typename T>
bool is(const std::exception* exception) {
    return dynamic_cast<const T*>(exception) != nullptr;
}

struct Kind {
    const char* name;
    bool (*matches)(const std::exception*);
};

// Most derived first: bad_array_new_length is a bad_alloc, future_error
// a logic_error, filesystem_error a system_error, which is a
// runtime_error, and so on.
const Kind kKinds[] = {
    {"bad_array_new_length", is<std::bad_array_new_length>},
    {"bad_alloc", is<std::bad_alloc>},
    {"bad_any_cast", is<std::bad_any_cast>},
    {"bad_cast", is<std::bad_cast>},
    {"bad_typeid", is<std::bad_typeid>},
    {"bad_function_call", is<std::bad_function_call>},
    {"bad_optional_access", is<std::bad_optional_access>},
    {"bad_variant_access", is<std::bad_variant_access>},
    {"bad_weak_ptr", is<std::bad_weak_ptr>},
    {"bad_exception", is<std::bad_exception>},
    {"future_error", is<std::future_error>},
    {"out_of_range", is<std::out_of_range>},
    {"length_error", is<std::length_error>},
    {"invalid_argument", is<std::invalid_argument>},
    {"domain_error", is<std::domain_error>},
    {"logic_error", is<std::logic_error>},
    {"filesystem_error", is<std::filesystem::filesystem_error>},
    {"system_error", is<std::system_error>},
    {"regex_error", is<std::regex_error>},
    {"range_error", is<std::range_error>},
    {"overflow_error", is<std::overflow_error>},
    {"underflow_error", is<std::underflow_error>},
    {"runtime_error", is<std::runtime_error>},
};

constexpr const char* kOtherStd = "std_exception";
constexpr const char* kNonStd = "non_std";

QMutex& sinkMutex() {
    static QMutex mutex;
    return mutex;
}

std::function<void(const char*)>& sink() {
    static std::function<void(const char*)> function;
    return function;
}

} // namespace

const char* workerExceptionKind(const std::exception* exception) {
    if (!exception)
        return kNonStd;
    for (const Kind& kind : kKinds) {
        if (kind.matches(exception))
            return kind.name;
    }
    return kOtherStd;
}

QStringList workerExceptionKinds() {
    QStringList names;
    for (const Kind& kind : kKinds)
        names << QString::fromLatin1(kind.name);
    names << QString::fromLatin1(kOtherStd) << QString::fromLatin1(kNonStd);
    return names;
}

void setWorkerExceptionSink(std::function<void(const char* kind)> function) {
    const QMutexLocker lock(&sinkMutex());
    sink() = std::move(function);
}

void reportWorkerException(const std::exception* exception) {
    const char* kind = workerExceptionKind(exception);
    const char* what = exception ? exception->what() : nullptr;
    if (exception)
        qWarning("AsyncRunner: a worker threw %s: %s", kind, what ? what : "");
    else
        qWarning("AsyncRunner: a worker threw %s", kind);
    // Under the lock, so a sink being cleared never runs after.
    const QMutexLocker lock(&sinkMutex());
    if (sink())
        sink()(kind);
}

} // namespace gitbolt::util
