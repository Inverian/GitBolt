#include "app/SingleInstance.h"

#include "util/LockFiles.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>

namespace gitbolt::app {

QString instanceRuntimeDir()
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
    const QString runtime =
        QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (!runtime.isEmpty())
        return runtime;
#endif
    return QDir::tempPath();
}

QString instanceServerName(const QDir& runtimeDir, const QString& instanceName)
{
#ifdef Q_OS_WIN
    const QByteArray scope = QCryptographicHash::hash(
        runtimeDir.absolutePath().toLower().toUtf8(),
        QCryptographicHash::Sha256);
    return instanceName + QLatin1Char('-')
           + QString::fromLatin1(scope.toHex().left(16));
#else
    return runtimeDir.filePath(instanceName);
#endif
}

Handoff handOffToRunningInstance(const QString& serverName, const QString& path,
                                 const HandoffWait& wait)
{
    QLocalSocket sock;
    sock.connectToServer(serverName);
    if (!sock.waitForConnected(1000))
        return Handoff::NoListener;

    // In steps of 100 ms, so `wait` gets its say (and whatever it shows
    // its events) while the owner is busy.
    QElapsedTimer waited;
    waited.start();
    const auto goOn = [&wait, &waited]() { return !wait || wait(waited.elapsed()); };

    sock.write(path.toUtf8() + '\n');
    while (sock.bytesToWrite() > 0) {
        if (sock.state() != QLocalSocket::ConnectedState)
            return Handoff::HungUp;
        if (!goOn())
            return Handoff::Cancelled;
        sock.waitForBytesWritten(100);
    }

    while (!sock.canReadLine()) {
        if (sock.state() != QLocalSocket::ConnectedState)
            return Handoff::HungUp;
        if (!goOn())
            return Handoff::Cancelled;
        sock.waitForReadyRead(100);
    }
    return Handoff::Delivered;
}

Claim claimInstance(QLockFile& lock, const QString& serverName, const QString& path,
                    int patienceMs, const HandoffWait& wait)
{
    QDeadlineTimer patience(patienceMs);
    int hangUps = 0;
    while (!lock.tryLock(0)) {
        // Can't create the lock at all (unwritable directory, full
        // disk): nothing the user could fix from a dialog, and no
        // reason to refuse to start.
        if (lock.error() != QLockFile::LockFailedError)
            return Claim::Unguarded;
        const Handoff handoff = handOffToRunningInstance(serverName, path, wait);
        if (handoff == Handoff::Delivered)
            return Claim::Delivered;
        if (handoff == Handoff::Cancelled)
            return Claim::Cancelled;
        // The owner hung up: alive a moment ago, however long this
        // waited on it, and on its way out now.
        if (handoff == Handoff::HungUp && ++hangUps <= 3) {
            patience = QDeadlineTimer(patienceMs);
        } else if (patience.hasExpired()) {
            return Claim::Unresponsive;
        }
        // Nobody listening, or the owner hung up unanswered: retry —
        // unless the lock is stale in a way its contents can't prove,
        // which removal settles.
        if (util::removeUnprovableStaleLock(lock))
            continue;
        QThread::msleep(100);
    }
    return Claim::Owner;
}

InstanceServer::InstanceServer(QObject* parent)
    : QObject(parent)
{
    connect(&server_, &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket* sock = server_.nextPendingConnection()) {
            connect(sock, &QLocalSocket::readyRead, this, [this, sock]() {
                if (!sock->canReadLine())
                    return;  // rest of the path in flight
                const QString path = QString::fromUtf8(sock->readLine()).trimmed();
                // The sender's receipt, flushed now: opening the repo
                // may sit in a dialog for a while.
                sock->write("ok\n");
                sock->flush();
                sock->disconnectFromServer();
                emit pathReceived(path);
            });
            connect(sock, &QLocalSocket::disconnected, sock, &QObject::deleteLater);
        }
    });
}

bool InstanceServer::listen(const QString& serverName)
{
    QLocalServer::removeServer(serverName);  // stale socket
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    return server_.listen(serverName);
}

QString InstanceServer::errorString() const
{
    return server_.errorString();
}

QString repoPathArgument(const QString& argument)
{
    QString path = argument;
#ifdef Q_OS_WIN
    if (path.endsWith(QLatin1Char('"'))) {
        path.chop(1);
        path += QLatin1Char('\\');
    }
#endif
    // A bare "." becomes the full path, not the literal ".".
    return QFileInfo(path).absoluteFilePath();
}

} // namespace gitbolt::app
