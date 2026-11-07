#include "update/Installer.h"

#include "update/InstallHelpers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QtGlobal>

#include <memory>
#include <utility>

namespace gitbolt::update {

namespace {

#if defined(Q_OS_MACOS)
constexpr char kBundleId[] = "com.gitbolt.app";
#endif
/// hdiutil's "Resource busy" comes and goes (actions/runner-images#7522,
/// and on desktops too): CI tries attach and detach five times.
constexpr int kAttachAttempts = 5;
constexpr int kDetachAttempts = 5;
constexpr int kAttachTimeoutMs = 3 * 60 * 1000;
constexpr int kCodesignTimeoutMs = 5 * 60 * 1000;
constexpr int kDittoTimeoutMs = 10 * 60 * 1000;
constexpr int kDetachTimeoutMs = 60 * 1000;
/// How long a helper waits for GitBolt to quit (~MainWindow waits for
/// running git work) before it gives up and changes nothing.
constexpr int kHelperWaitSeconds = 10 * 60;
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
    cancel();
}

bool Installer::busy() const {
    return busy_;
}

QString Installer::helperScriptPath() const {
    return QDir(workDir_).filePath(QStringLiteral("install-helper.sh"));
}

QString Installer::logPath() const {
    return QDir(workDir_).filePath(QStringLiteral("install.log"));
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
    if (mountPoint_.isEmpty())
        return false;
    const QString mount = QFileInfo(mountPoint_).canonicalFilePath();
    if (mount.isEmpty())
        return false;
    const QStorageInfo volume(mountPoint_);
    return volume.isValid() && QFileInfo(volume.rootPath()).canonicalFilePath() == mount;
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
    // cancel() and the destructor: no event loop to wait in. A plain
    // detach (even if nothing shows as mounted yet: a killed attach may
    // still be finishing), then a forced one if the volume is there.
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

bool Installer::launchHelper(qint64 pid) {
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
    const QString script = helperScript(pid);
    // One update's story per log. (A relaunched AppImage or deb copy
    // keeps writing its own output to it, and the next update would
    // otherwise append to all of that.)
    if (QFile logFile(logPath()); logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        logFile.close();
    QProcess helper;
    helper.setWorkingDirectory(workDir_);
    helper.setStandardInputFile(QProcess::nullDevice());
#if defined(Q_OS_WIN)
    // Not into the log: the script appends to it, and a handle open for
    // writing (ours) would make every Add-Content a sharing violation.
    helper.setProgram(windowsPowerShell());
    helper.setArguments(powerShellArguments(script));
#else
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
    // A session of its own (startDetached also double-forks, so it
    // outlives GitBolt), default signal dispositions, none of GitBolt's
    // descriptors: the instance lock and its socket die with GitBolt.
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    helper.setUnixProcessParameters(QProcess::UnixProcessFlag::ResetSignalHandlers
                                    | QProcess::UnixProcessFlag::CloseFileDescriptors
                                    | QProcess::UnixProcessFlag::CreateNewSession);
#elif QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    helper.setUnixProcessParameters(QProcess::UnixProcessFlag::ResetSignalHandlers
                                    | QProcess::UnixProcessFlag::CloseFileDescriptors);
#endif
#endif
    if (!helper.startDetached()) {
        emit failed(tr("Couldn't start the installer helper: %1").arg(helper.errorString()));
        return false;
    }
    launched_ = true;
    ready_ = false;
    return true;
}

} // namespace gitbolt::update
