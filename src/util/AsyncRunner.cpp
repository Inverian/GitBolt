#include "util/AsyncRunner.h"

namespace gitbolt::util {
AsyncRunner::AsyncRunner(QObject* parent, QThreadPool* pool)
    : QObject(parent), pool_(pool ? pool : QThreadPool::globalInstance()) {}
} // namespace gitbolt::util
