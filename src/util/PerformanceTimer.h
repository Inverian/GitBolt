#pragma once

#include <QDebug>
#include <QElapsedTimer>
#include <QString>

namespace gitbolt::util {

/// RAII timer for performance measurement.
/// Logs elapsed time to qDebug if it exceeds a configurable threshold.
///
/// Usage:
///   PerformanceTimer timer("loadCommitLog");
///   // ... expensive work ...
///   // destructor logs if elapsed > threshold
class PerformanceTimer {
public:
    explicit PerformanceTimer(const char* label)
        : label_(label) {
        timer_.start();
    }

    explicit PerformanceTimer(const QString& label)
        : label_(label) {
        timer_.start();
    }

    ~PerformanceTimer() {
        const qint64 elapsed = timer_.elapsed();
        if (elapsed >= thresholdMs_)
            qDebug() << "[perf]" << label_ << "took" << elapsed << "ms";
    }

    // Non-copyable, non-movable
    PerformanceTimer(const PerformanceTimer&) = delete;
    PerformanceTimer& operator=(const PerformanceTimer&) = delete;

    /// Set the global threshold in milliseconds.
    /// Only operations exceeding this will be logged.
    static void setThreshold(int ms) { thresholdMs_ = ms; }
    static int threshold() { return thresholdMs_; }

    /// Read current elapsed time without stopping the timer.
    qint64 elapsed() const { return timer_.elapsed(); }

private:
    QString label_;
    QElapsedTimer timer_;
    static inline int thresholdMs_ = 100;
};

} // namespace gitbolt::util
