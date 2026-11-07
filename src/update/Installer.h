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
    /// mount point, the helper script and install.log go.
    Installer(PackageInfo package, Version current, QString workDir, QObject* parent = nullptr);
    /// Cleans up like cancel() unless the helper was launched: it then
    /// owns the staged copy.
    ~Installer() override;

    /// Checks `downloadedFile` (already verified by the Downloader) as
    /// the installer of `version`, and stages it. Emits status() as it
    /// goes, then prepared() or failed().
    void prepare(const QString& downloadedFile, const Version& version);
    /// Stops prepare() and undoes it: kills the running step, detaches
    /// the disk image and removes the staged copy. Emits nothing. Also
    /// discards a prepared install that wasn't launched. Waits for
    /// hdiutil's detach, a second or so.
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

} // namespace gitbolt::update
