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
//     it is being drained.
//

#include <QCoreApplication>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTest>
#include <QThread>
#include <QThreadPool>

#include "util/AsyncRunner.h"

#include <atomic>
#include <memory>

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
