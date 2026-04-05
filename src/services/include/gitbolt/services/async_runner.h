#pragma once
#include <QObject>
#include <QFuture>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <functional>

namespace gitbolt::services {

class AsyncRunner : public QObject {
    Q_OBJECT
public:
    explicit AsyncRunner(QObject* parent = nullptr);

    template<typename Func>
    void run(Func&& func) {
        auto* watcher = new QFutureWatcher<void>(this);
        connect(watcher, &QFutureWatcher<void>::finished, watcher, &QObject::deleteLater);
        watcher->setFuture(QtConcurrent::run(std::forward<Func>(func)));
    }

    template<typename T, typename Func, typename Callback>
    void runWithResult(Func&& func, Callback&& callback) {
        auto* watcher = new QFutureWatcher<T>(this);
        connect(watcher, &QFutureWatcher<T>::finished, this, [watcher, cb = std::forward<Callback>(callback)]() {
            cb(watcher->result());
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run(std::forward<Func>(func)));
    }
};

} // namespace gitbolt::services
