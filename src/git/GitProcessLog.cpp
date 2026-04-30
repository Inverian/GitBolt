#include "git/GitProcessLog.h"

namespace gitbolt::git {

GitProcessLog& GitProcessLog::instance() {
    // Function-local static — Meyers singleton. C++11 guarantees
    // thread-safe initialization on first call. Lives until program
    // exit; never freed.
    static GitProcessLog s_instance;
    return s_instance;
}

void GitProcessLog::emitCommand(const QString& workdir,
                                const QStringList& args,
                                int exitCode,
                                qint64 durationMs) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ring_.push_back(GitProcessLogEntry{
            workdir, args, exitCode, durationMs});
        while (ring_.size() > kRingCap)
            ring_.pop_front();
    }
    emit commandLogged(workdir, args, exitCode, durationMs);
}

std::vector<GitProcessLogEntry> GitProcessLog::recent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<GitProcessLogEntry>(ring_.begin(), ring_.end());
}

} // namespace gitbolt::git
