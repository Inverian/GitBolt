//
// TestAsyncRunner — the threading guarantees util::AsyncRunner adds
// on top of QtConcurrent:
//
//   - run() / runWithResult() called from another thread are re-posted
//     to the runner's thread (with a warning naming the misuse) instead
//     of parenting a watcher across threads and appending to the drain
//     list from a pool thread,
//   - a re-post still pending when the runner dies is dropped,
//   - the destructor drain survives a job that submits more work while
//     it is being drained,
//   - an exception escaping a worker reaches the sink as its type alone
//     (never what()), from run() and runWithResult(), and a cleared
//     sink hears nothing.
//

#include <QCoreApplication>
#include <QMutex>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSet>
#include <QTest>
#include <QThread>
#include <QThreadPool>

#include "util/AsyncRunner.h"
#include "util/WorkerExceptions.h"

#include <atomic>
#include <memory>
#include <new>
#include <stdexcept>
#include <system_error>
#include <vector>

using gitbolt::util::AsyncRunner;

namespace {

// Runs `fn` on a fresh thread and joins it. Not the global pool: the
// point is a caller that is certainly not the runner's thread, and
// joining a pool future can run the job on the joining thread.
template <typename Fn>
bool callFromOtherThread(Fn fn) {
    std::unique_ptr<QThread> caller(QThread::create(std::move(fn)));
    caller->start();
    return caller->wait(5000);
}

} // namespace

class TestAsyncRunner : public QObject {
    Q_OBJECT

private slots:
    // Unexpected AsyncRunner warnings fail; the tests that provoke one
    // on purpose declare it with ignoreMessage, which QtTest checks
    // first.
    void init() {
        QTest::failOnWarning(
            QRegularExpression(QStringLiteral("Cannot create children")));
        QTest::failOnWarning(QRegularExpression(QStringLiteral("AsyncRunner")));
    }

    void runFromAnotherThreadIsRepostedHere() {
        AsyncRunner runner;
        std::atomic<bool> ran{false};

        QTest::ignoreMessage(QtWarningMsg,
            QRegularExpression(QStringLiteral("AsyncRunner::run called off")));
        QVERIFY(callFromOtherThread([&] { runner.run([&] { ran = true; }); }));

        // Nothing was submitted from the other thread: the job only
        // starts once this thread's event loop runs the re-post.
        QThreadPool::globalInstance()->waitForDone();
        QVERIFY(!ran);
        QTRY_VERIFY(ran);
    }

    void runWithResultFromAnotherThreadCallsBackHere() {
        AsyncRunner runner;
        int value = 0;
        QThread* callbackThread = nullptr;

        QTest::ignoreMessage(QtWarningMsg,
            QRegularExpression(QStringLiteral("AsyncRunner::runWithResult called off")));
        QVERIFY(callFromOtherThread([&] {
            runner.runWithResult<int>(
                [] { return 42; },
                [&](int v) {
                    value = v;
                    callbackThread = QThread::currentThread();
                });
        }));

        QTRY_COMPARE(value, 42);
        QCOMPARE(callbackThread, QThread::currentThread());
    }

    // The re-post is queued on the runner, so it dies with it: the job
    // never starts, and there is nothing to drain.
    void repostPendingAtDestructionIsDropped() {
        std::atomic<bool> ran{false};
        {
            AsyncRunner runner;
            QTest::ignoreMessage(QtWarningMsg,
                QRegularExpression(QStringLiteral("AsyncRunner::run called off")));
            QVERIFY(callFromOtherThread([&] { runner.run([&] { ran = true; }); }));
        }
        QCoreApplication::processEvents();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QVERIFY(!ran);
    }

    // ~AsyncRunner used to wait on every job, and waiting on one that
    // had not started ran it right there (QThreadPool work stealing),
    // during the owner's teardown; a job that then called run() again
    // modified the list the drain was iterating. The drain now cancels
    // queued jobs, so neither the outer jobs nor their submissions run.
    void queuedJobsAreDroppedByTheDrain() {
        auto* pool = QThreadPool::globalInstance();
        const int savedThreads = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        // Park the only pool thread, so the runner's jobs stay queued
        // until the destructor reaches them.
        QSemaphore gate;
        auto gateJob = QtConcurrent::run([&gate] { gate.acquire(); });
        // Restore the global pool on every exit, failed checks included.
        bool released = false;
        const auto restore = qScopeGuard([&] {
            if (!released)
                gate.release();
            gateJob.waitForFinished();
            pool->setMaxThreadCount(savedThreads);
        });

        std::atomic<int> outer{0};
        std::atomic<int> inner{0};
        auto runner = std::make_unique<AsyncRunner>();
        AsyncRunner* r = runner.get();
        for (int i = 0; i < 4; ++i) {
            runner->run([r, &outer, &inner] {
                ++outer;
                r->run([&inner] { ++inner; });
            });
        }
        runner.reset();

        gate.release();
        released = true;
        QVERIFY(pool->waitForDone(5000));

        QCOMPARE(outer.load(), 0);
        QCOMPARE(inner.load(), 0);
    }

    // The sink gets a fixed token per exception, never its text: a
    // runtime_error's what() can hold a path. The local log gets both.
    // runWithResult's callback has no value to take, so it isn't called.
    void workerExceptionsReachTheSink() {
        QMutex mutex;
        QStringList kinds;
        const auto taken = [&] {
            const QMutexLocker lock(&mutex);
            return kinds;
        };
        gitbolt::util::setWorkerExceptionSink([&](const char* kind) {
            const QMutexLocker lock(&mutex);
            kinds << QString::fromLatin1(kind);
        });
        auto clear = qScopeGuard([] { gitbolt::util::setWorkerExceptionSink({}); });

        const auto threw = [](const char* rest) {
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression(
                QStringLiteral("^AsyncRunner: a worker threw %1").arg(QLatin1String(rest))));
        };
        threw("out_of_range: ");
        threw("bad_alloc: ");
        threw("runtime_error: /Users/alice/secret-repo$");
        threw("non_std$");
        bool called = false;
        {
            AsyncRunner runner;
            runner.run([] {
                std::vector<int> none;
                static_cast<void>(none.at(3));
            });
            runner.runWithResult<int>([]() -> int { throw std::bad_alloc(); },
                                      [&](int) { called = true; });
            runner.run([] { throw std::runtime_error("/Users/alice/secret-repo"); });
            runner.runWithResult<int>([]() -> int { throw 42; }, [&](int) { called = true; });
            QTRY_COMPARE(taken().size(), 4);
            QTest::qWait(50);   // the watchers' finished(), had they a value
        }
        QVERIFY(!called);
        QStringList got = taken();
        got.sort();
        QCOMPARE(got, (QStringList{QStringLiteral("bad_alloc"), QStringLiteral("non_std"),
                                   QStringLiteral("out_of_range"), QStringLiteral("runtime_error")}));
        for (const QString& kind : got)
            QVERIFY(gitbolt::util::workerExceptionKinds().contains(kind));

        clear.dismiss();
        gitbolt::util::setWorkerExceptionSink({});
        threw("logic_error: x$");
        {
            AsyncRunner runner;
            runner.run([] { throw std::logic_error("x"); });
            QTRY_VERIFY(!runner.busy());
        }
        QCOMPARE(taken().size(), 4);
    }

    // Most derived first; a type outside the list is "std_exception".
    void workerExceptionKinds() {
        struct Custom : std::exception {};
        const std::out_of_range outOfRange("x");
        const std::bad_array_new_length badLength;
        const std::system_error systemError(std::make_error_code(std::errc::io_error));
        const Custom custom;
        QCOMPARE(gitbolt::util::workerExceptionKind(&outOfRange), "out_of_range");
        QCOMPARE(gitbolt::util::workerExceptionKind(&badLength), "bad_array_new_length");
        QCOMPARE(gitbolt::util::workerExceptionKind(&systemError), "system_error");
        QCOMPARE(gitbolt::util::workerExceptionKind(&custom), "std_exception");
        QCOMPARE(gitbolt::util::workerExceptionKind(nullptr), "non_std");
        const QStringList all = gitbolt::util::workerExceptionKinds();
        QCOMPARE(QSet<QString>(all.cbegin(), all.cend()).size(), all.size());
        static const QRegularExpression token(QStringLiteral("^[a-z_]{1,32}$"));
        for (const QString& kind : all)
            QVERIFY2(token.match(kind).hasMatch(), qPrintable(kind));
    }

    // A job that is already running is waited for: the owner's members
    // it touches stay alive until it returns.
    void runningJobIsWaitedFor() {
        QSemaphore started;
        std::atomic<bool> finished{false};
        {
            AsyncRunner runner;
            runner.run([&] {
                started.release();
                QThread::msleep(100);
                finished = true;
            });
            QVERIFY(started.tryAcquire(1, 5000));
        }
        QVERIFY(finished.load());
    }
};

QTEST_GUILESS_MAIN(TestAsyncRunner)
#include "TestAsyncRunner.moc"
