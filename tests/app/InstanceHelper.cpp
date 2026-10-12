//
// InstanceHelper — one end of the single-instance hand-off in a process
// of its own, for TestSingleInstance: what GitBolt does as the instance
// that holds the lock, or as a later launch, through the same code
// (src/app/SingleInstance.h) but with no window and no settings. So the
// test runs the hand-off between real processes on every platform —
// Windows' named pipes, a process killed while a launch waits on it,
// Windows' own reading of a command line — and never touches the
// user's GitBolt.
//
//   owner <lock file> <server name>
//       Takes the lock and listens. Prints "ready", then
//       "path:<path>" for each hand-off. Commands on stdin: "busy <ms>"
//       stops its event loop that long (printing "busy" first), as git
//       on GitBolt's GUI thread does; "quit" exits.
//   launch <lock file> <server name> <path>
//       Claims the lock or hands <path> over, and prints the outcome:
//       owner, delivered, unresponsive, unguarded or cancelled.
//   path <argument>
//       Prints the repository path GitBolt makes of its argument.
//

#include <QCoreApplication>
#include <QLockFile>
#include <QStringList>
#include <QThread>

#include "app/SingleInstance.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

using gitbolt::app::Claim;

namespace {

void say(const QString& line)
{
    std::fputs(qPrintable(line + QLatin1Char('\n')), stdout);
    std::fflush(stdout);
}

QString claimName(Claim claim)
{
    switch (claim) {
    case Claim::Owner:        return QStringLiteral("owner");
    case Claim::Delivered:    return QStringLiteral("delivered");
    case Claim::Unresponsive: return QStringLiteral("unresponsive");
    case Claim::Unguarded:    return QStringLiteral("unguarded");
    case Claim::Cancelled:    return QStringLiteral("cancelled");
    }
    return {};
}

int runOwner(QCoreApplication& app, const QString& lockPath, const QString& serverName)
{
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        say(QStringLiteral("error: lock held"));
        return 3;
    }
    gitbolt::app::InstanceServer server;
    if (!server.listen(serverName)) {
        say(QStringLiteral("error: ") + server.errorString());
        return 4;
    }
    QObject::connect(&server, &gitbolt::app::InstanceServer::pathReceived, &app,
                     [](const QString& path) { say(QStringLiteral("path:") + path); });

    // stdin has no notifier on Windows: a thread reads it and hands each
    // line to the event loop, which takes it once it turns.
    std::thread([&app] {
        std::string line;
        while (std::getline(std::cin, line)) {
            const QString command = QString::fromStdString(line).trimmed();
            QMetaObject::invokeMethod(&app, [command] {
                if (command.startsWith(QStringLiteral("busy "))) {
                    say(QStringLiteral("busy"));
                    QThread::msleep(command.mid(5).toULong());
                } else if (command == QStringLiteral("quit")) {
                    QCoreApplication::quit();
                }
            }, Qt::QueuedConnection);
        }
    }).detach();

    say(QStringLiteral("ready"));
    return app.exec();
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    // As GitBolt reads them: on Windows from the command line itself.
    const QStringList args = app.arguments();
    const QString mode = args.value(1);
    if (mode == QStringLiteral("owner") && args.size() == 4)
        return runOwner(app, args.at(2), args.at(3));
    if (mode == QStringLiteral("launch") && args.size() == 5) {
        QLockFile lock(args.at(2));
        lock.setStaleLockTime(0);
        say(claimName(gitbolt::app::claimInstance(lock, args.at(3), args.at(4))));
        return 0;
    }
    if (mode == QStringLiteral("path") && args.size() == 3) {
        say(gitbolt::app::repoPathArgument(args.at(2)));
        return 0;
    }
    say(QStringLiteral("usage: owner <lock> <server> | launch <lock> <server> <path> | path <arg>"));
    return 2;
}
