#pragma once

#include "update/Package.h"
#include "update/Version.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;

namespace gitbolt::update {

/// Puts a downloaded, checksum-verified installer in place over the
/// running copy, in two steps: prepare() checks and stages it while
/// GitBolt runs, and launchHelper() starts a detached helper (see
/// InstallHelpers.h) that finishes once GitBolt has quit. The UI calls
/// qApp->quit() after launchHelper() returns true.
///
/// Trust (no Developer ID, no Authenticode, so no publisher check is
/// possible here, nor of the release's Sigstore attestations): the
/// Downloader has already matched the file's size and SHA-256 to the
/// release. On top of that, prepare() checks that the file is the
/// asset this package installs from (assetNameFor), and on macOS that
/// the app in the disk image, and again the staged copy, passes
/// `codesign --verify --deep --strict` (its ad-hoc seal, so nothing in
/// the bundle changed after it was built), is com.gitbolt.app, and is
/// the release's version.
///
/// macOS, step by step: attach the DMG read-only at a mount point in
/// workDir (hdiutil's intermittent "Resource busy" is retried, as CI
/// does), check GitBolt.app there, ditto it to <folder>/.GitBolt-<v>.app
/// beside the installed bundle (same volume, so the helper's swap is
/// two renames), check the copy, detach. Other packages need nothing
/// staged: the AppImage was downloaded beside the old one and is made
/// executable; the .deb and the Windows installer stay where they are.
///
/// GUI thread; one prepare() at a time. Every step runs in a QProcess,
/// and every outcome is signalled from the event loop, never from
/// inside prepare().
class Installer : public QObject {
    Q_OBJECT
public:
    /// `workDir`: the downloads folder (<cache>/updates), where the
    /// mount point, the helper script, install.log and install-result
    /// go.
    Installer(PackageInfo package, Version current, QString workDir, QObject* parent = nullptr);
    /// Cleans up unless the helper was launched (it then owns the
    /// staged copy), and synchronously: this may run at quit, with no
    /// event loop left. After cancel() there is nothing left to wait for.
    ~Installer() override;

    /// Checks `downloadedFile` (already verified by the Downloader) as
    /// the installer of `version`, and stages it. Emits status() as it
    /// goes, then prepared() or failed(). macOS also refuses, before
    /// GitBolt quits, an app whose LSMinimumSystemVersion is above the
    /// running macOS: once swapped in, it wouldn't start.
    void prepare(const QString& downloadedFile, const Version& version);
    /// Stops prepare() and undoes it: kills the running step, detaches
    /// the disk image and removes the staged copy. Emits nothing. Also
    /// discards a prepared install that wasn't launched. Returns at
    /// once (it runs on the GUI thread, from the dialog's Cancel and
    /// Close): the step is killed without waiting for it, and on macOS
    /// the staged copy is renamed out of the way and, with the disk
    /// image's detach, left to a detached cleanup process that outlives
    /// GitBolt if it quits meanwhile. A leftover from a cleanup that
    /// never finished goes at the next start (removeStaleInstallFiles).
    void cancel();
    /// After prepared(): starts the detached helper, which waits for
    /// `pid` (this process's, QCoreApplication::applicationPid()) to
    /// exit before it touches anything. False, with failed(), if it
    /// couldn't be started; nothing was changed then.
    bool launchHelper(qint64 pid);

    /// prepare() is running.
    bool busy() const;
    /// The helper's script and log, in workDir: install-helper.sh (or
    /// none on Windows, where the script is passed on the command line)
    /// and install.log.
    QString helperScriptPath() const;
    QString logPath() const;
    /// <workDir>/install-result, where the helper writes how the update
    /// went for the GitBolt it starts next (one line, "<status>
    /// <version> <reason>": see InstallHelpers.h). Not in a version's
    /// folder, so removeOldDownloads leaves it; launchHelper() deletes
    /// an old one first.
    QString resultPath() const;

signals:
    void prepared();
    void failed(const QString& message);
    /// What prepare() is doing, for the dialog: "Checking the
    /// download…", "Copying GitBolt…".
    void status(const QString& text);

private:
    struct ToolResult {
        int exitCode = -1;   ///< -1: didn't start, crashed or timed out
        QString output;      ///< stdout and stderr, merged
    };
    using ToolDone = std::function<void(const ToolResult&)>;

    void runTool(const QString& program, const QStringList& arguments, int timeoutMs, ToolDone done);
    void succeed();
    void fail(const QString& message);
    void finishLater(const std::function<void()>& emitter);
    /// The destructor's cancel(): everything undone before it returns.
    void cancelNow();
    /// Kills the running step and lets it go: it deletes itself once it
    /// has ended, not with this Installer. Its pid, or 0.
    qint64 abandonProcess();
    /// macOS: hands the disk image's detach and the staged copy's
    /// removal to a detached process (waiting first for `killedPid`, the
    /// step just killed). False if it couldn't start one.
    bool cleanUpInBackground(qint64 killedPid);

    // macOS
    void attach(int attempt);
    void checkMounted();
    void stage();
    void checkStaged();
    void detach(int attempt, const std::function<void()>& then);
    void detachNow();
    bool mounted() const;
    QString checkBundle(const QString& bundle, const QString& where);
    void removeStaged();
    void removeMountPoint();

    QString helperScript(qint64 pid) const;

    PackageInfo package_;
    Version current_;
    QString workDir_;

    QString file_;            ///< what prepare() was given
    Version version_;
    QString mountPoint_;      ///< macOS: <workDir>/mount-XXXXXX while it exists
    bool attached_ = false;   ///< hdiutil attach ran (it may have mounted even if killed)
    QString staged_;          ///< the new copy prepared() leaves for the helper
    bool stagedCreated_ = false;  ///< staged_ is a copy this Installer made (macOS)
    QString executable_;      ///< macOS: Contents/MacOS/<CFBundleExecutable> of the staged copy

    QProcess* process_ = nullptr;
    quint64 run_ = 0;         ///< bumped by every prepare() and cancel(); stale callbacks see a new one
    bool busy_ = false;
    bool ready_ = false;      ///< prepared() was emitted, launchHelper() not yet called
    bool launched_ = false;
};

/// At startup, removes what an update that never finished left behind:
/// a crash, a force quit or a logout during prepare() or while the
/// helper waited (the helper's own cleanup dies with it). Nothing that
/// is a link is followed or removed, and nothing outside these folders:
///   - macOS: in the installed bundle's folder, the hidden staged copies
///     (.GitBolt-<v>.app, and those cancel() renamed, .GitBolt-<v>-…app)
///     and swapped-aside old copies (.GitBolt-<v>.app.old) that this
///     user owns; and in `workDir`, the disk image mount points
///     (mount-XXXXXX), detached first (forced) if still mounted, then
///     removed only if empty;
///   - AppImage: in the AppImage's folder, the Downloader's
///     .GitBolt-<v>-x86_64.AppImage.part files, and the
///     GitBolt-<v>-x86_64.AppImage files of versions up to `current`,
///     never the running AppImage (package.target);
///   - nothing for the other packages: their downloads are in
///     workDir/<v>/, which removeOldDownloads clears.
/// Synchronous (a forced detach is bounded); the app calls it once,
/// before any Installer exists.
void removeStaleInstallFiles(const PackageInfo& package, const QString& workDir, const Version& current);

} // namespace gitbolt::update
