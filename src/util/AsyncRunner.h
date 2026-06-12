#pragma once
#include <QFuture>
#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QtConcurrent>

#include <exception>
#include <functional>
#include <optional>
#include <utility>

namespace gitbolt::util {

/// Submits work to the global thread pool and reports completion on
/// this object's thread via QFutureWatcher.
///
/// Two guarantees the raw QtConcurrent API does not give:
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
///      submitted job finishes. Workers routinely capture pointers
///      into their owner (GitService captures `this` for emits and
///      the repo mutex), so destroying the owner with a job still
///      queued is a use-after-free. Owners flip their cancellation
///      flags before their members destruct, so the wait is short.
///      Declare the AsyncRunner AFTER anything the workers touch, so
///      it is destroyed (and drains) first.
class AsyncRunner : public QObject {
    Q_OBJECT
public:
    explicit AsyncRunner(QObject* parent = nullptr);

    ~AsyncRunner() override {
        for (auto& f : futures_)
            f.waitForFinished();
    }

    template<typename Func>
    void run(Func&& func) {
        pruneFinished();
        auto fut = QtConcurrent::run(
            [fn = std::forward<Func>(func)]() mutable {
                try {
                    fn();
                } catch (const std::exception& e) {
                    qWarning("AsyncRunner: worker threw: %s", e.what());
                } catch (...) {
                    qWarning("AsyncRunner: worker threw a non-std exception");
                }
            });
        auto* watcher = new QFutureWatcher<void>(this);
        connect(watcher, &QFutureWatcher<void>::finished,
                watcher, &QObject::deleteLater);
        watcher->setFuture(fut);
        futures_.append(std::move(fut));
    }

    /// Run `func` on a worker and hand its return value to `callback`
    /// on this object's thread. If the worker throws, the exception is
    /// logged and the callback is NOT invoked (there is no value to
    /// hand it); the watcher is cleaned up either way.
    template<typename T, typename Func, typename Callback>
    void runWithResult(Func&& func, Callback&& callback) {
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
    void pruneFinished() {
        futures_.removeIf(
            [](const QFuture<void>& f) { return f.isFinished(); });
    }

    QList<QFuture<void>> futures_;
};

} // namespace gitbolt::util
