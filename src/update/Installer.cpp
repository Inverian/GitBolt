#include "update/Installer.h"

#include "update/InstallHelpers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QOperatingSystemVersion>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QVersionNumber>
#include <QtGlobal>

#include <memory>
#include <optional>
#include <utility>

#if defined(Q_OS_UNIX)
#include <unistd.h>   // getuid
#endif
#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // CREATE_NO_WINDOW
#include <objbase.h>  // CoInitializeEx
#include <shellapi.h> // ShellExecuteExW

#include <QDateTime>

#include <string>
#endif

namespace gitbolt::update {

namespace {

#if defined(Q_OS_MACOS)
constexpr char kBundleId[] = "com.gitbolt.app";
#endif
/// hdiutil's "Resource busy" comes and goes (actions/runner-images#7522,
/// on desktops too), so attach and detach are tried five times.
constexpr int kAttachAttempts = 5;
constexpr int kDetachAttempts = 5;
constexpr int kAttachTimeoutMs = 3 * 60 * 1000;
constexpr int kCodesignTimeoutMs = 5 * 60 * 1000;
constexpr int kDittoTimeoutMs = 10 * 60 * 1000;
constexpr int kDetachTimeoutMs = 60 * 1000;
/// The longest tool output a message quotes.
constexpr qsizetype kMaxQuoted = 400;

QString hdiutil() { return QStringLiteral("/usr/bin/hdiutil"); }
QString codesign() { return QStringLiteral("/usr/bin/codesign"); }
QString ditto() { return QStringLiteral("/usr/bin/ditto"); }

QString quoted(const QString& output) {
    const QString text = output.trimmed();
    return text.size() > kMaxQuoted ? text.left(kMaxQuoted) + QStringLiteral("…") : text;
}

bool resourceBusy(int exitCode, const QString& output) {
    // EBUSY (16) is hdiutil's exit status for it; its text may be
    // localised, strerror's isn't.
    return exitCode == 16 || output.contains(QLatin1String("Resource busy"));
}

// Removes what is at `path` without following a link there: a link is
// removed itself, a folder with everything in it.
bool removePath(const QString& path) {
    const QFileInfo info(path);
    if (info.isSymLink() || (info.exists() && !info.isDir()))
        return QFile::remove(path);
    if (info.isDir())
        return QDir(path).removeRecursively();
    return true;
}

bool samePath(const QString& a, const QString& b) {
    const QString ca = QFileInfo(a).canonicalFilePath();
    return !ca.isEmpty() && ca == QFileInfo(b).canonicalFilePath();
}

// A volume is mounted right at `path` (not merely somewhere above it).
bool isMountPoint(const QString& path) {
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty())
        return false;
    const QStorageInfo volume(canonical);
    return volume.isValid() && QFileInfo(volume.rootPath()).canonicalFilePath() == canonical;
}

#if defined(Q_OS_UNIX)
// Gives a detached process of ours its own session, default signal
// handling and none of GitBolt's descriptors: the instance lock and its
// socket must die with GitBolt.
void detachFromGitBolt(QProcess& process) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    process.setUnixProcessParameters(QProcess::UnixProcessFlag::ResetSignalHandlers
                                     | QProcess::UnixProcessFlag::CloseFileDescriptors
                                     | QProcess::UnixProcessFlag::CreateNewSession);
#elif QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    process.setUnixProcessParameters(QProcess::UnixProcessFlag::ResetSignalHandlers
                                     | QProcess::UnixProcessFlag::CloseFileDescriptors);
#else
    Q_UNUSED(process)
#endif
}
#endif

#if defined(Q_OS_MACOS)
/// cancel()'s detached cleanup, for /bin/sh -c. Arguments: the killed
/// step's pid or 0, the mount point or "", the renamed staged copy or "".
/// It waits for that step to end (a zombie counts), so nothing still
/// writes into what it removes. A killed attach may mount late, so it
/// checks a few times and forces the last detaches; rmdir removes the
/// mount point only if empty.
constexpr char kCleanupScript[] =
    "pid=$1 mnt=$2 doomed=$3\n"
    "n=0\n"
    "while [ \"$pid\" -gt 0 ] && [ $n -lt 100 ] && kill -0 \"$pid\" 2>/dev/null; do\n"
    "    case $(ps -o stat= -p \"$pid\" 2>/dev/null) in\n"
    "    Z*) break ;;\n"
    "    esac\n"
    "    sleep 0.1\n"
    "    n=$((n + 1))\n"
    "done\n"
    "if [ -n \"$mnt\" ]; then\n"
    "    i=1\n"
    "    while [ $i -le 8 ]; do\n"
    "        if [ -d \"$mnt\" ] && [ \"$(stat -f %d \"$mnt\")\" != \"$(stat -f %d \"$mnt/..\")\" ]; then\n"
    "            if [ $i -le 4 ]; then\n"
    "                /usr/bin/hdiutil detach \"$mnt\"\n"
    "            else\n"
    "                /usr/bin/hdiutil detach -force \"$mnt\"\n"
    "            fi\n"
    "        elif [ $i -ge 3 ]; then\n"
    "            break\n"
    "        fi\n"
    "        sleep 1\n"
    "        i=$((i + 1))\n"
    "    done\n"
    "    rmdir \"$mnt\"\n"
    "fi\n"
    "if [ -n \"$doomed\" ]; then\n"
    "    rm -rf \"$doomed\"\n"
    "fi\n";
#endif

#if defined(Q_OS_WIN)
// By absolute path: CreateProcess looks in the application's folder
// before System32.
QString windowsPowerShell() {
    QString root = qEnvironmentVariable("SystemRoot");
    if (root.isEmpty())
        root = QStringLiteral("C:\\Windows");
    return root + QStringLiteral("\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
}
#endif

} // namespace

Installer::Installer(PackageInfo package, Version current, QString workDir, QObject* parent)
    : QObject(parent), package_(std::move(package)), current_(current), workDir_(std::move(workDir)) {}

Installer::~Installer() {
    cancelNow();
}

bool Installer::busy() const {
    return busy_;
}

bool Installer::needsNewerSystem() const {
    return needsNewerSystem_;
}

QString Installer::helperScriptPath() const {
    return QDir(workDir_).filePath(QStringLiteral("install-helper.sh"));
}

QString Installer::logPath() const {
    return QDir(workDir_).filePath(QStringLiteral("install.log"));
}

QString Installer::resultPath() const {
    return QDir(workDir_).filePath(QStringLiteral("install-result"));
}

void Installer::finishLater(const std::function<void()>& emitter) {
    QMetaObject::invokeMethod(this, emitter, Qt::QueuedConnection);
}

void Installer::prepare(const QString& downloadedFile, const Version& version) {
    if (busy_ || launched_) {
        const quint64 run = run_;
        finishLater([this, run] {
            if (run == run_)
                emit failed(launched_ ? tr("The update is already being installed.")
                                      : tr("GitBolt is already getting an update ready."));
        });
        return;
    }
    cancel();   // a prepared update that was never launched
    ++run_;
    busy_ = true;
    file_ = QFileInfo(downloadedFile).absoluteFilePath();
    version_ = version;
    needsNewerSystem_ = false;

    if (!package_.canInstall) {
        fail(package_.whyNot.isEmpty() ? tr("This copy of GitBolt can't install updates.") : package_.whyNot);
        return;
    }
    const QString expected = assetNameFor(package_.kind, version);
    const QFileInfo info(file_);
    if (expected.isEmpty() || info.fileName() != expected || !info.isFile() || info.isSymLink()) {
        fail(tr("The download isn't the installer of GitBolt %1 for this system (%2).")
                 .arg(version.toString(), expected));
        return;
    }

    switch (package_.kind) {
    case PackageKind::MacBundle: {
#if defined(Q_OS_MACOS)
        const quint64 run = run_;
        finishLater([this, run] {
            if (run == run_)
                attach(1);
        });
#else
        fail(tr("A disk image can only be installed on macOS."));
#endif
        return;
    }
    case PackageKind::AppImage: {
        // The helper renames it over the old one, so it must be in the
        // same folder (a rename, not a copy), and not be the old one.
        const QFileInfo target(package_.target);
        if (!samePath(info.absolutePath(), target.absolutePath())) {
            fail(tr("The new AppImage must be downloaded next to the old one, in %1.").arg(target.absolutePath()));
            return;
        }
        if (samePath(file_, package_.target)) {
            fail(tr("The download has the running AppImage's name, %1: GitBolt can't replace a file with itself.")
                     .arg(target.fileName()));
            return;
        }
        const auto mode = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup
                          | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther;
        if (!QFile::setPermissions(file_, mode)) {
            fail(tr("Couldn't make the new AppImage executable."));
            return;
        }
        staged_ = file_;
        succeed();
        return;
    }
    case PackageKind::WindowsInstall:
    case PackageKind::Deb:
        staged_ = file_;
        succeed();
        return;
    case PackageKind::Unknown:
        break;
    }
    fail(package_.whyNot);
}

void Installer::succeed() {
    const quint64 run = run_;
    finishLater([this, run] {
        if (run != run_)
            return;
        busy_ = false;
        ready_ = true;
        emit prepared();
    });
}

void Installer::fail(const QString& message) {
    const quint64 run = run_;
    auto finish = [this, run, message] {
        if (run != run_)
            return;
        removeStaged();
        removeMountPoint();
        staged_.clear();
        busy_ = false;
        ready_ = false;
        emit failed(message);
    };
    if (attached_)
        detach(1, finish);
    else
        finishLater(finish);
}

void Installer::cancel() {
    // Once launched, the staged copy is the helper's.
    if (launched_)
        return;
    ++run_;
    // This waits only if the background cleanup can't be started.
    const qint64 killed = abandonProcess();
    if ((attached_ || stagedCreated_) && !cleanUpInBackground(killed)) {
        if (attached_)
            detachNow();
        removeStaged();
    }
    removeMountPoint();
    staged_.clear();
    busy_ = false;
    ready_ = false;
}

void Installer::cancelNow() {
    if (launched_)
        return;
    ++run_;
    if (QProcess* process = std::exchange(process_, nullptr)) {
        process->kill();
        process->waitForFinished(5000);
    }
    if (attached_)
        detachNow();
    removeStaged();
    removeMountPoint();
    staged_.clear();
    busy_ = false;
    ready_ = false;
}

qint64 Installer::abandonProcess() {
    QProcess* process = std::exchange(process_, nullptr);
    if (!process)
        return 0;
    const qint64 pid = process->processId();
    // Unparented, so it doesn't go with this Installer: QProcess's
    // destructor waits for a running process.
    process->disconnect(this);
    process->setParent(nullptr);
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            process->deleteLater();
    });
    if (process->state() == QProcess::NotRunning)
        process->deleteLater();
    else
        process->kill();
    return pid;
}

bool Installer::cleanUpInBackground(qint64 killedPid) {
#if defined(Q_OS_MACOS)
    // Rename the staged copy first: a new prepare() of this version
    // stages under the same name, which the cleanup's rm must never
    // reach. removeStaleInstallFiles still matches the new name.
    QString doomed;
    if (stagedCreated_ && !staged_.isEmpty()) {
        const QFileInfo staged(staged_);
        if (staged.exists() || staged.isSymLink()) {
            const QString aside = QDir(staged.absolutePath())
                                      .filePath(QStringLiteral(".GitBolt-%1-cancelled-%2.app")
                                                    .arg(version_.toString())
                                                    .arg(QRandomGenerator::global()->generate(), 8, 16, u'0'));
            if (QFileInfo::exists(aside) || QFileInfo(aside).isSymLink() || !QDir().rename(staged_, aside))
                return false;
            doomed = aside;
        }
    }
    const QString mount = attached_ ? mountPoint_ : QString();
    if (!doomed.isEmpty() || !mount.isEmpty()) {
        QProcess cleanup;
        cleanup.setProgram(QStringLiteral("/bin/sh"));
        cleanup.setArguments({QStringLiteral("-c"), QString::fromLatin1(kCleanupScript),
                              QStringLiteral("gitbolt-update-cleanup"), QString::number(killedPid), mount, doomed});
        cleanup.setStandardInputFile(QProcess::nullDevice());
        cleanup.setStandardOutputFile(QProcess::nullDevice());
        cleanup.setStandardErrorFile(QProcess::nullDevice());
        detachFromGitBolt(cleanup);
        if (!cleanup.startDetached()) {
            // cancel() removes it here after all, under its new name.
            if (!doomed.isEmpty())
                staged_ = doomed;
            return false;
        }
    }
    stagedCreated_ = false;
    attached_ = false;
    if (!mount.isEmpty())
        mountPoint_.clear();   // the cleanup's to remove
    return true;
#else
    Q_UNUSED(killedPid)
    return false;
#endif
}

void Installer::runTool(const QString& program, const QStringList& arguments, int timeoutMs, ToolDone done) {
    auto* process = new QProcess(this);
    process_ = process;
    process->setProgram(program);
    process->setArguments(arguments);
    process->setProcessChannelMode(QProcess::MergedChannels);
    // No tool may wait on input (an image with a licence would).
    process->setStandardInputFile(QProcess::nullDevice());
    auto* timer = new QTimer(process);
    timer->setSingleShot(true);
    auto timedOut = std::make_shared<bool>(false);
    connect(timer, &QTimer::timeout, process, [process, timedOut] {
        *timedOut = true;
        process->kill();
    });
    const quint64 run = run_;
    auto ended = std::make_shared<bool>(false);
    auto finish = [this, process, run, done, ended, timedOut](int exitCode, const QString& extra) {
        if (*ended)
            return;
        *ended = true;
        ToolResult result;
        result.exitCode = *timedOut ? -1 : exitCode;
        result.output = QString::fromLocal8Bit(process->readAll()).trimmed();
        if (!extra.isEmpty())
            result.output += (result.output.isEmpty() ? QString() : QStringLiteral("\n")) + extra;
        if (*timedOut)
            result.output += QStringLiteral(" (%1 didn't finish in time)").arg(QFileInfo(process->program()).fileName());
        if (process_ == process)
            process_ = nullptr;
        process->deleteLater();
        if (run == run_)
            done(result);
    };
    connect(process, &QProcess::finished, this, [finish](int exitCode, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit ? exitCode : -1, QString());
    });
    connect(process, &QProcess::errorOccurred, this, [finish, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(-1, process->errorString());
    });
    process->start();
    timer->start(timeoutMs);
}

// ---------------------------------------------------------------------------
// macOS
// ---------------------------------------------------------------------------

void Installer::attach(int attempt) {
    if (attempt == 1) {
        // A slot may cancel() right here.
        const quint64 run = run_;
        emit status(tr("Checking the download…"));
        if (run != run_)
            return;
        QDir().mkpath(workDir_);
        QTemporaryDir mount(QDir(workDir_).filePath(QStringLiteral("mount-XXXXXX")));
        if (!mount.isValid()) {
            fail(tr("Couldn't make a folder to open the disk image in, in %1.").arg(workDir_));
            return;
        }
        mount.setAutoRemove(false);
        mountPoint_ = mount.path();
    }
    attached_ = true;
    runTool(hdiutil(),
            {QStringLiteral("attach"), QStringLiteral("-readonly"), QStringLiteral("-nobrowse"),
             QStringLiteral("-noautoopen"), QStringLiteral("-mountpoint"), mountPoint_, file_},
            kAttachTimeoutMs, [this, attempt](const ToolResult& result) {
                if (result.exitCode == 0) {
                    checkMounted();
                    return;
                }
                if (attempt < kAttachAttempts && resourceBusy(result.exitCode, result.output)) {
                    const quint64 run = run_;
                    QTimer::singleShot(attempt * 1000, this, [this, run, attempt] {
                        if (run == run_)
                            attach(attempt + 1);
                    });
                    return;
                }
                fail(tr("Couldn't open the disk image: %1").arg(quoted(result.output)));
            });
}

void Installer::checkMounted() {
    const QString app = QDir(mountPoint_).filePath(QStringLiteral("GitBolt.app"));
    const QFileInfo info(app);
    if (info.isSymLink() || !info.isDir()) {
        fail(tr("The disk image has no GitBolt.app."));
        return;
    }
    const QString problem = checkBundle(app, tr("in the disk image"));
    if (!problem.isEmpty()) {
        fail(problem);
        return;
    }
    runTool(codesign(), {QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"), app},
            kCodesignTimeoutMs, [this](const ToolResult& result) {
                if (result.exitCode != 0) {
                    fail(tr("GitBolt.app in the disk image fails its signature check, so something in it "
                            "changed after it was built: %1")
                             .arg(quoted(result.output)));
                    return;
                }
                stage();
            });
}

void Installer::stage() {
    const quint64 run = run_;
    emit status(tr("Copying GitBolt…"));
    if (run != run_)
        return;   // cancelled by a slot
    // Beside the installed bundle, hidden, so the helper's swap is a
    // rename on one volume.
    const QFileInfo target(package_.target);
    staged_ = QDir(target.absolutePath()).filePath(QStringLiteral(".GitBolt-%1.app").arg(version_.toString()));
    // Left over from an update that stopped half-way.
    removePath(staged_);
    if (QFileInfo::exists(staged_) || QFileInfo(staged_).isSymLink()) {
        fail(tr("Couldn't remove %1, left over from an earlier update.").arg(staged_));
        return;
    }
    stagedCreated_ = true;
    runTool(ditto(), {QDir(mountPoint_).filePath(QStringLiteral("GitBolt.app")), staged_}, kDittoTimeoutMs,
            [this](const ToolResult& result) {
                if (result.exitCode != 0) {
                    fail(tr("Couldn't copy GitBolt next to the installed copy: %1").arg(quoted(result.output)));
                    return;
                }
                checkStaged();
            });
}

void Installer::checkStaged() {
    const QString problem = checkBundle(staged_, tr("in the copy"));
    if (!problem.isEmpty()) {
        fail(problem);
        return;
    }
    runTool(codesign(), {QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"), staged_},
            kCodesignTimeoutMs, [this](const ToolResult& result) {
                if (result.exitCode != 0) {
                    fail(tr("The copy of GitBolt.app fails its signature check: %1").arg(quoted(result.output)));
                    return;
                }
                const quint64 run = run_;
                detach(1, [this, run] {
                    if (run != run_)
                        return;
                    removeMountPoint();
                    succeed();
                });
            });
}

QString Installer::checkBundle(const QString& bundle, const QString& where) {
#if defined(Q_OS_MACOS)
    const QString plist = QDir(bundle).filePath(QStringLiteral("Contents/Info.plist"));
    if (!QFileInfo(plist).isFile())
        return tr("GitBolt.app %1 has no Info.plist.").arg(where);
    // NativeFormat reads a .plist file on macOS, XML or binary.
    const QSettings info(plist, QSettings::NativeFormat);
    const QString id = info.value(QStringLiteral("CFBundleIdentifier")).toString();
    if (id != QLatin1String(kBundleId))
        return tr("The app %1 isn't GitBolt: its identifier is %2, not %3.")
            .arg(where, quoted(id), QLatin1String(kBundleId));
    const QString shortVersion = info.value(QStringLiteral("CFBundleShortVersionString")).toString();
    if (shortVersion != version_.toString())
        return tr("GitBolt.app %1 is version %2, not %3.").arg(where, quoted(shortVersion), version_.toString());
    // Checked now, while GitBolt runs: once swapped in, LaunchServices
    // wouldn't start it and the old copy would be in the Trash.
    // Normalised, so 14.4 and 14.4.0 are the same.
    const QString minimum = info.value(QStringLiteral("LSMinimumSystemVersion")).toString().trimmed();
    const QVersionNumber needs = QVersionNumber::fromString(minimum).normalized();
    const QOperatingSystemVersion os = QOperatingSystemVersion::current();
    const QVersionNumber running(qMax(0, os.majorVersion()), qMax(0, os.minorVersion()), qMax(0, os.microVersion()));
    if (!needs.isNull() && running.normalized() < needs) {
        const QString shown = running.microVersion() > 0 ? running.toString()
                                                         : QStringLiteral("%1.%2").arg(running.majorVersion())
                                                               .arg(running.minorVersion());
        needsNewerSystem_ = true;
        return tr("GitBolt %1 needs macOS %2 or later, and this Mac has macOS %3, so it can't be installed here.")
            .arg(version_.toString(), quoted(minimum), shown);
    }
    const QString executable = info.value(QStringLiteral("CFBundleExecutable")).toString();
    if (executable.isEmpty() || executable.contains(u'/') || executable == QLatin1String(".")
        || executable == QLatin1String("..")
        || !QFileInfo(QDir(bundle).filePath(QStringLiteral("Contents/MacOS/") + executable)).isFile())
        return tr("GitBolt.app %1 has no program to start.").arg(where);
    executable_ = QStringLiteral("Contents/MacOS/") + executable;
    return {};
#else
    Q_UNUSED(bundle)
    return tr("GitBolt.app %1 can only be checked on macOS.").arg(where);
#endif
}

bool Installer::mounted() const {
    return !mountPoint_.isEmpty() && isMountPoint(mountPoint_);
}

void Installer::detach(int attempt, const std::function<void()>& then) {
    if (!mounted()) {
        attached_ = false;
        then();
        return;
    }
    // After the last plain try, once with -force: a mount left behind
    // is a hidden volume until logout, which no update needs.
    const bool force = attempt > kDetachAttempts;
    QStringList arguments{QStringLiteral("detach")};
    if (force)
        arguments << QStringLiteral("-force");
    arguments << mountPoint_;
    runTool(hdiutil(), arguments, kDetachTimeoutMs, [this, attempt, force, then](const ToolResult& result) {
        if (result.exitCode == 0 || !mounted()) {
            attached_ = false;
            then();
            return;
        }
        if (force) {
            qWarning("update: couldn't detach %s: %s", qPrintable(mountPoint_), qPrintable(result.output));
            then();
            return;
        }
        const quint64 run = run_;
        QTimer::singleShot(1000, this, [this, run, attempt, then] {
            if (run == run_)
                detach(attempt + 1, then);
        });
    });
}

void Installer::detachNow() {
    // Synchronous. The plain detach runs even if nothing shows as mounted
    // yet (a killed attach may still be finishing); -force only if mounted.
    attached_ = false;
    if (mountPoint_.isEmpty())
        return;
    for (const bool force : {false, true}) {
        if (force && !mounted())
            break;
        QStringList arguments{QStringLiteral("detach")};
        if (force)
            arguments << QStringLiteral("-force");
        arguments << mountPoint_;
        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.setStandardInputFile(QProcess::nullDevice());
        process.start(hdiutil(), arguments);
        if (!process.waitForFinished(15000)) {
            process.kill();
            process.waitForFinished(1000);
        }
    }
}

void Installer::removeStaged() {
    if (!stagedCreated_)
        return;
    stagedCreated_ = false;
    if (!staged_.isEmpty() && !removePath(staged_))
        qWarning("update: couldn't remove %s", qPrintable(staged_));
}

void Installer::removeMountPoint() {
    if (mountPoint_.isEmpty() || mounted())
        return;
    QDir().rmdir(mountPoint_);   // only if empty: never what was mounted there
    mountPoint_.clear();
}

// ---------------------------------------------------------------------------
// The helper
// ---------------------------------------------------------------------------

QString Installer::helperScript(qint64 pid) const {
    HelperInputs in;
    in.pid = pid;
    in.waitSeconds = kHelperWaitSeconds;
    in.logFile = logPath();
    in.target = package_.target;
    in.staged = staged_;
    in.newVersion = version_.toString();
    in.resultFile = resultPath();
    switch (package_.kind) {
    case PackageKind::MacBundle:
        in.oldVersion = current_.toString();
        in.trashDir = QDir(QDir::homePath()).filePath(QStringLiteral(".Trash"));
        in.executable = executable_;
        // Test sessions talk to the new copy through the instance name
        // and the bridge, which a LaunchServices launch wouldn't pass.
        in.execDirectly = !qEnvironmentVariable("GITBOLT_INSTANCE_NAME").isEmpty();
        return macHelperScript(in);
    case PackageKind::AppImage:
        in.extractAndRun = appImageRunsExtracted(qEnvironmentVariable("APPDIR"),
                                                 qEnvironmentVariableIsSet("APPIMAGE_EXTRACT_AND_RUN"));
        return appImageHelperScript(in);
    case PackageKind::Deb:
        in.pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        in.executable = QStringLiteral("/usr/bin/gitbolt");
        return debHelperScript(in);
    case PackageKind::WindowsInstall:
        return windowsHelperScript(in);
    case PackageKind::Unknown:
        break;
    }
    return {};
}

bool Installer::launchHelper(qint64 pid, quintptr window) {
    if (!ready_ || launched_ || pid <= 0) {
        emit failed(tr("There's no update ready to install."));
        return false;
    }
    if (package_.kind == PackageKind::Deb && QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty()) {
        emit failed(package_.whyNot.isEmpty() ? tr("Installing the update needs pkexec.") : package_.whyNot);
        return false;
    }
    if (!QDir().mkpath(workDir_)) {
        emit failed(tr("Couldn't create %1.").arg(workDir_));
        return false;
    }
    // One update per log: a relaunched AppImage or deb copy keeps writing
    // its output to it.
    if (QFile logFile(logPath()); logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        logFile.close();
    // An earlier update's result would pass for this one's.
    QFile::remove(resultPath());
#if defined(Q_OS_WIN)
    if (package_.kind != PackageKind::WindowsInstall) {
        emit failed(tr("This copy of GitBolt can't install updates."));
        return false;
    }
    // Both parts start now, while GitBolt is still in front for UAC.
    HelperInputs in;
    in.pid = pid;
    in.waitSeconds = kHelperWaitSeconds;
    in.logFile = logPath();
    in.target = package_.target;
    in.staged = staged_;
    in.newVersion = version_.toString();
    in.resultFile = resultPath();
    const WindowsLaunch launch = launchWindowsUpdate(in, window, QProcessEnvironment::systemEnvironment());
    // Where Windows won't elevate (a standard user under "deny elevation
    // requests"), only an administrator can install it.
    const QString byHand = tr("To install it yourself, download %1 from the release page and run it as an "
                              "administrator.")
                               .arg(assetNameFor(package_.kind, version_));
    switch (launch.outcome) {
    case WindowsLaunch::Outcome::Started:
        if (!launch.error.isEmpty())
            qWarning("update: GitBolt won't be started again after the update: %s", qPrintable(launch.error));
        break;
    case WindowsLaunch::Outcome::Declined:
        emit declined(byHand);
        return false;
    case WindowsLaunch::Outcome::Failed: {
        // The system's own sentence ("Access is denied."), in its language.
        QString why = launch.error.trimmed();
        if (!why.endsWith(u'.'))
            why += u'.';
        emit failed(tr("Windows couldn't start GitBolt's installer as an administrator: %1").arg(why)
                    + QLatin1Char(' ') + byHand);
        return false;
    }
    }
#else
    Q_UNUSED(window)
    const QString script = helperScript(pid);
    QProcess helper;
    helper.setWorkingDirectory(workDir_);
    helper.setStandardInputFile(QProcess::nullDevice());
    QSaveFile file(helperScriptPath());
    if (!file.open(QIODevice::WriteOnly) || file.write(script.toUtf8()) < 0 || !file.commit()) {
        emit failed(tr("Couldn't write the installer helper, %1.").arg(helperScriptPath()));
        return false;
    }
    // Before the script's own redirect, sh's errors go there too.
    helper.setStandardOutputFile(logPath(), QIODevice::Append);
    helper.setStandardErrorFile(logPath(), QIODevice::Append);
    helper.setProgram(QStringLiteral("/bin/sh"));
    helper.setArguments({helperScriptPath()});
    detachFromGitBolt(helper);
    if (!helper.startDetached()) {
        emit failed(tr("Couldn't start the installer helper: %1").arg(helper.errorString()));
        return false;
    }
#endif
    launched_ = true;
    ready_ = false;
    return true;
}

WindowsLaunch launchWindowsUpdate(HelperInputs in, quintptr window, const QProcessEnvironment& environment) {
    WindowsLaunch launch;
#if defined(Q_OS_WIN)
    // ShellExecuteExW may hand the verb to a shell extension, which needs
    // COM: GitBolt's GUI thread has it already (S_FALSE), a test's may not.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const std::wstring file = windowsPowerShell().toStdWString();
    const std::wstring parameters = powerShellParameters(windowsElevatedScript(in)).toStdWString();
    SHELLEXECUTEINFOW info = {};
    info.cbSize = static_cast<DWORD>(sizeof(info));
    // Its pid, for the helper; and done before this returns, since
    // GitBolt quits right after.
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    // UAC's prompt comes up over this window; with no window in front it
    // is only a flashing taskbar button.
    info.hwnd = reinterpret_cast<HWND>(window);
    info.lpVerb = L"runas";
    info.lpFile = file.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    const BOOL started = ShellExecuteExW(&info);
    const DWORD error = started ? DWORD(ERROR_SUCCESS) : GetLastError();
    if (SUCCEEDED(com))
        CoUninitialize();
    if (!started) {
        // UAC's No, by its code: the text is in the user's language.
        if (error == ERROR_CANCELLED) {
            launch.outcome = WindowsLaunch::Outcome::Declined;
            return launch;
        }
        launch.error = qt_error_string(static_cast<int>(error));
        return launch;
    }
    // The helper gets only its pid, no handle: an over-the-shoulder
    // administrator's process may not be the user's to open.
    if (info.hProcess) {
        launch.elevatedPid = static_cast<qint64>(GetProcessId(info.hProcess));
        CloseHandle(info.hProcess);
    }
    launch.outcome = WindowsLaunch::Outcome::Started;

    // The helper, not elevated. Its output doesn't go to the log: a write
    // handle of ours on it would refuse every Out-File of Write-Log.
    in.elevatedPid = launch.elevatedPid;
    QProcess helper;
    helper.setProgram(windowsPowerShell());
    helper.setArguments(powerShellArguments(windowsHelperScript(in)));
    helper.setWorkingDirectory(QFileInfo(in.logFile).absolutePath());
    helper.setStandardInputFile(QProcess::nullDevice());
    helper.setProcessEnvironment(environment);
    // CREATE_NO_WINDOW: a console nobody sees, not one that flashes until
    // -WindowStyle Hidden applies. Windows ignores it beside
    // CREATE_NEW_CONSOLE or DETACHED_PROCESS, so those are cleared.
    helper.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags &= ~static_cast<unsigned long>(CREATE_NEW_CONSOLE | DETACHED_PROCESS);
        args->flags |= CREATE_NO_WINDOW;
    });
    if (!helper.startDetached(&launch.helperPid)) {
        // Too late to stop: the administrator part installs once
        // GitBolt quits. Only the restart is lost, and the log says so.
        launch.helperPid = 0;
        launch.error = helper.errorString();
        if (QFile log(in.logFile); log.open(QIODevice::WriteOnly | QIODevice::Append))
            log.write((QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                       + QStringLiteral(" couldn't start the helper that starts GitBolt again (")
                       + launch.error + QStringLiteral("): start GitBolt yourself once it has quit\n"))
                          .toUtf8());
    }
#else
    Q_UNUSED(in)
    Q_UNUSED(window)
    Q_UNUSED(environment)
    launch.error = QStringLiteral("Only Windows has an administrator part.");
#endif
    return launch;
}

// ---------------------------------------------------------------------------
// Leftovers
// ---------------------------------------------------------------------------

namespace {

// The entries of `folder` whose name `pattern` matches, hidden ones
// included; never a link or junction, whatever it points at.
QFileInfoList ownEntries(const QString& folder, const QRegularExpression& pattern, QDir::Filters kinds) {
    QFileInfoList found;
    const QFileInfoList entries = QDir(folder).entryInfoList(kinds | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo& entry : entries) {
        if (entry.isSymLink() || entry.isJunction() || !pattern.match(entry.fileName()).hasMatch())
            continue;
        found << entry;
    }
    return found;
}

// `entry` is `target`, by either spelling.
bool isTarget(const QFileInfo& entry, const QString& target) {
    return QDir::cleanPath(entry.absoluteFilePath()) == QDir::cleanPath(QFileInfo(target).absoluteFilePath())
           || samePath(entry.absoluteFilePath(), target);
}

#if defined(Q_OS_MACOS)
// A forced detach of a mount nothing of ours uses any more, bounded.
void forceDetach(const QString& mountPoint) {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(hdiutil(), {QStringLiteral("detach"), QStringLiteral("-force"), mountPoint});
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(1000);
    }
}
#endif

// A path, and a newline: anything longer isn't a record of ours.
constexpr qint64 kMaxPendingRecordBytes = 64 * 1024;

QString pendingRecordPath(const QString& workDir) {
    return QDir(workDir).filePath(QLatin1String(kPendingDownloadFile));
}

// Removes the download a leftover record names (its GitBolt crashed or
// was killed) only if it is still a regular file of this user's, with
// the release's AppImage name, and not the running AppImage (the user
// may have started that very file). The record goes either way.
void removePendingDownload(const PackageInfo& package, const QString& workDir) {
    const QString record = pendingRecordPath(workDir);
    const QFileInfo recordInfo(record);
    if (!recordInfo.exists() && !recordInfo.isSymLink())
        return;
    QByteArray bytes;
    if (QFile file(record); !recordInfo.isSymLink() && file.open(QIODevice::ReadOnly))
        bytes = file.read(kMaxPendingRecordBytes + 1);
    if (!QFile::remove(record))
        qWarning("update: couldn't remove %s", qPrintable(record));
    if (package.kind != PackageKind::AppImage || package.target.isEmpty() || bytes.size() > kMaxPendingRecordBytes)
        return;
    const QString path = QString::fromUtf8(bytes).trimmed();
    if (path.isEmpty() || path.contains(u'\n') || !QDir::isAbsolutePath(path))
        return;
    const QFileInfo entry(path);
    static const QRegularExpression image(QStringLiteral("^GitBolt-(.+)-x86_64\\.AppImage$"));
    const QRegularExpressionMatch named = image.match(entry.fileName());
    const std::optional<Version> version = named.hasMatch() ? Version::parse(named.captured(1)) : std::nullopt;
    if (!version || assetNameFor(PackageKind::AppImage, *version) != entry.fileName())
        return;
    if (entry.isSymLink() || entry.isJunction() || !entry.isFile() || isTarget(entry, package.target))
        return;
#if defined(Q_OS_UNIX)
    if (entry.ownerId() != ::getuid())
        return;
#endif
    if (!QFile::remove(path))
        qWarning("update: couldn't remove %s", qPrintable(path));
}

} // namespace

void recordPendingDownload(const QString& workDir, const QString& file) {
    if (workDir.isEmpty() || !QDir::isAbsolutePath(workDir) || !QDir().mkpath(workDir)) {
        qWarning("update: no folder to record the download %s in", qPrintable(file));
        return;
    }
    // A new file of its own: whatever has the name goes first (a link
    // itself, never what it points to).
    forgetPendingDownload(workDir);
    QFile record(pendingRecordPath(workDir));
    if (!record.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || record.write(QFileInfo(file).absoluteFilePath().toUtf8() + '\n') < 0 || !record.flush())
        qWarning("update: couldn't record the download %s in %s", qPrintable(file), qPrintable(record.fileName()));
}

void forgetPendingDownload(const QString& workDir) {
    if (workDir.isEmpty())
        return;
    const QString record = pendingRecordPath(workDir);
    const QFileInfo info(record);
    if ((info.exists() || info.isSymLink()) && !QFile::remove(record))
        qWarning("update: couldn't remove %s", qPrintable(record));
}

void removeStaleInstallFiles(const PackageInfo& package, const QString& workDir) {
    if (package.kind == PackageKind::MacBundle && !package.target.isEmpty()) {
        // Staged copies (cancel()'s renamed ones too) and swapped-aside
        // old ones. A helper is done with them before it starts GitBolt;
        // while it waits, its GitBolt holds the instance lock, so a new one
        // is folded into that. Only one started between that GitBolt's exit
        // and the helper's swap can race it. Only this user's: another
        // user's GitBolt may be mid-update from the same folder.
        static const QRegularExpression staged(QStringLiteral("^\\.GitBolt-.+\\.app(\\.old)?$"));
        const QFileInfo target(package.target);
        for (const QFileInfo& entry : ownEntries(target.absolutePath(), staged, QDir::Dirs)) {
            if (isTarget(entry, package.target))
                continue;
#if defined(Q_OS_UNIX)
            if (entry.ownerId() != ::getuid())
                continue;
#endif
            if (!QDir(entry.absoluteFilePath()).removeRecursively())
                qWarning("update: couldn't remove %s", qPrintable(entry.absoluteFilePath()));
        }
    }

    // Disk image mount points, detached first if mounted; rmdir removes
    // only an empty folder, never what was mounted there.
    if (!workDir.isEmpty() && QDir::isAbsolutePath(workDir)) {
        static const QRegularExpression mount(QStringLiteral("^mount-.+$"));
        for (const QFileInfo& entry : ownEntries(workDir, mount, QDir::Dirs)) {
#if defined(Q_OS_MACOS)
            if (isMountPoint(entry.absoluteFilePath()))
                forceDetach(entry.absoluteFilePath());
#endif
            if (!isMountPoint(entry.absoluteFilePath()))
                QDir().rmdir(entry.absoluteFilePath());
        }
    }

    if (package.kind == PackageKind::AppImage && !package.target.isEmpty()) {
        // This user's unfinished downloads beside the AppImage (another
        // user's may be downloading there now). A finished one goes only
        // if recorded (below): users keep their AppImages under that name.
        static const QRegularExpression part(QStringLiteral("^\\.GitBolt-.+-x86_64\\.AppImage\\.part$"));
        const QString folder = QFileInfo(package.target).absolutePath();
        for (const QFileInfo& entry : ownEntries(folder, part, QDir::Files)) {
            if (isTarget(entry, package.target))
                continue;
#if defined(Q_OS_UNIX)
            if (entry.ownerId() != ::getuid())
                continue;
#endif
            QFile::remove(entry.absoluteFilePath());
        }
    }
    if (!workDir.isEmpty() && QDir::isAbsolutePath(workDir))
        removePendingDownload(package, workDir);
}

} // namespace gitbolt::update
