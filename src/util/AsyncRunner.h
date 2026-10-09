#pragma once
#include <QFuture>
#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QThread>
#include <QtConcurrent>

#include <exception>
#include <functional>
#include <optional>
#include <utility>

namespace gitbolt::util {

/// Submits work to the global thread pool. run() is fire-and-forget;
/// runWithResult() hands the result back on this object's thread via
/// QFutureWatcher.
///
/// Three guarantees the raw QtConcurrent API does not give:
///
///   1. Exception containment. QtConcurrent transports a worker's
///      exception into the future; querying result() rethrows it in
///      the watcher slot, and an exception escaping a slot through
///      the event loop terminates the process. Fire-and-forget run()
///      conversely swallowed it without a trace. Both paths now trap,
///      log via qWarning, and keep the app alive — workers are
///      expected to report failures through their own channels
///      (Result<T>, operationFailed signals); an exception reaching
///      here is a bug, not a control path.
///
///   2. Destructor quiescence. ~AsyncRunner blocks until every
///      running job finishes, and drops queued jobs that have not
///      started. Workers routinely capture pointers into their owner
///      (GitService captures `this` for emits and the repo mutex), so
///      destroying the owner with a job still running is a
///      use-after-free. Owners flip their cancellation flags before
///      their members destruct, so the wait is short.
///      Declare the AsyncRunner AFTER anything the workers touch, so
///      it is destroyed (and drains) first.
///
///   3. Owner-thread bookkeeping. run() and runWithResult() belong on
///      this object's thread: the drain list is unsynchronized, and a
///      watcher can only be parented to this object from its thread.
///      Workers that called back into run() (GitService's follow-up
///      refreshes) printed "QObject: Cannot create children for a
///      parent that is in a different thread", leaked the watcher on
///      a pool thread with no event loop, and appended to the drain
///      list while this thread pruned or drained it. A call from
///      another thread is now re-posted to this one, so the job still
///      runs and the drain still covers it (unless the runner is
///      destroyed first, in which case the job never starts), and it
///      warns: the caller
///      is reading its own state off-thread too, which is the real
///      bug. A warning rather than an assert, because the runner can
///      still do its part correctly and a debug session should not
///      die over it; the unit tests fail on the warning instead.
class AsyncRunner : public QObject {
    Q_OBJECT
public:
    explicit AsyncRunner(QObject* parent = nullptr);

    ~AsyncRunner() override {
        // Refuse new work, drop queued jobs, then wait for the running
        // ones on a detached copy of the list. Without the cancel,
        // waitForFinished() would run a job that has not started yet
        // right here (QThreadPool work stealing), during the owner's
        // teardown: it would emit into receivers that are half gone
        // and could call back into run() while the list is iterated.
        // A canceled QtConcurrent::run task returns without running.
        closing_ = true;
        QList<QFuture<void>> pending = std::exchange(futures_, {});
        for (auto& f : pending)
            f.cancel();
        for (auto& f : pending)
            f.waitForFinished();
    }

    template<typename Func>
    void run(Func&& func) {
        if (!onOwnThread("run")) {
            QMetaObject::invokeMethod(
                this,
                [this, fn = std::forward<Func>(func)]() mutable {
                    run(std::move(fn));
                },
                Qt::QueuedConnection);
            return;
        }
        if (closing_)
            return;
        pruneFinished();
        // No watcher: nothing happens on completion, and the drain
        // list alone is what the destructor waits on.
        futures_.append(QtConcurrent::run(
            [fn = std::forward<Func>(func)]() mutable {
                try {
                    fn();
                } catch (const std::exception& e) {
                    qWarning("AsyncRunner: worker threw: %s", e.what());
                } catch (...) {
                    qWarning("AsyncRunner: worker threw a non-std exception");
                }
            }));
    }

    /// Run `func` on a worker and hand its return value to `callback`
    /// on this object's thread. If the worker throws, the exception is
    /// logged and the callback is NOT invoked (there is no value to
    /// hand it); the watcher is cleaned up either way.
    template<typename T, typename Func, typename Callback>
    void runWithResult(Func&& func, Callback&& callback) {
        if (!onOwnThread("runWithResult")) {
            QMetaObject::invokeMethod(
                this,
                [this, fn = std::forward<Func>(func),
                 cb = std::forward<Callback>(callback)]() mutable {
                    runWithResult<T>(std::move(fn), std::move(cb));
                },
                Qt::QueuedConnection);
            return;
        }
        if (closing_)
            return;
        pruneFinished();
        // The value travels through shared state rather than
        // QFuture<T>::result() so (a) the future type stays
        // QFuture<void> and joins the same drain list, and (b) a
        // worker exception can't re-throw into the event loop.
        auto slot = std::make_shared<std::optional<T>>();
        auto fut = QtConcurrent::run(
            [fn = std::forward<Func>(func), slot]() mutable {
                try {
                    slot->emplace(fn());
                } catch (const std::exception& e) {
                    qWarning("AsyncRunner: worker threw: %s", e.what());
                } catch (...) {
                    qWarning("AsyncRunner: worker threw a non-std exception");
                }
            });
        auto* watcher = new QFutureWatcher<void>(this);
        connect(watcher, &QFutureWatcher<void>::finished, this,
                [watcher, slot, cb = std::forward<Callback>(callback)]() mutable {
            if (slot->has_value())
                cb(std::move(**slot));
            watcher->deleteLater();
        });
        watcher->setFuture(fut);
        futures_.append(std::move(fut));
    }

private:
    // Guarantee 3: true on this object's thread; otherwise warns and
    // returns false, and the caller re-posts itself to this thread
    // (queued on `this`, so it is dropped if the runner dies first —
    // a job that never started needs no draining).
    bool onOwnThread(const char* method) const {
        if (QThread::currentThread() == thread())
            return true;
        qWarning("AsyncRunner::%s called off the runner's thread; "
                 "re-posted to it", method);
        return false;
    }

    void pruneFinished() {
        futures_.removeIf(
            [](const QFuture<void>& f) { return f.isFinished(); });
    }

    // Both touched only on this object's thread (guarantee 3).
    QList<QFuture<void>> futures_;
    bool closing_ = false;
};

} // namespace gitbolt::util
