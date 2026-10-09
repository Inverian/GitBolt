#pragma once
#include "git/CloneProgress.h"
#include <QDialog>
#include <QElapsedTimer>
#include <QString>
#include <atomic>
#include <memory>

class QLineEdit;
class QProgressBar;
class QPushButton;
class QLabel;
class QDialogButtonBox;

namespace gitbolt::dialogs {

/// Modal dialog that prompts for a remote URL and a local destination,
/// then runs `git clone` (GitProcess::clone) on a background thread
/// (via QtConcurrent) while showing its progress. On success the
/// dialog closes with `QDialog::Accepted` and `clonedPath()` returns
/// the path of the new working directory; on failure it stays open
/// with git's error message visible so the user can correct the
/// URL/path and retry without re-entering everything.
///
/// One failure leaves a repository behind: the fetch completed but
/// the checkout failed, and git kept the clone. The dialog then says
/// where it is, and the Clone button becomes "Open Repository" —
/// accepted with that path — until the URL or path is edited.
class CloneDialog : public QDialog {
    Q_OBJECT
public:
    explicit CloneDialog(QWidget* parent = nullptr);
    ~CloneDialog() override;

    /// Absolute path the repo was cloned to (only valid after the
    /// dialog returns QDialog::Accepted). Its checkout may have
    /// failed, if the user chose to open it anyway.
    QString clonedPath() const { return clonedPath_; }

protected:
    /// While a clone is running, Cancel / Esc / the window close
    /// button all become "cancel the clone" (flip the atomic flag
    /// the worker polls) instead of closing the dialog — the dialog
    /// stays open until git has actually stopped and the partial
    /// clone is cleaned up.
    void reject() override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    /// Triggered by the URL field — auto-fills the destination path
    /// with `<defaultParent>/<repo-name>` derived from the URL, but
    /// only if the user hasn't manually edited the path field yet.
    void onUrlChanged(const QString& url);
    void onBrowse();
    void onCloneClicked();

    /// Applied from the worker thread via QMetaObject::invokeMethod —
    /// updates the progress bar and status line.
    void onProgressUpdate(gitbolt::git::CloneProgress progress);

private:
    void setBusy(bool busy, const QString& message = QString());
    void requestCancel();
    void forgetKeptClone();
    static QString defaultParentDir();
    static QString repoNameFromUrl(const QString& url);
    static QString humanBytes(quint64 n);

    QLineEdit*        urlEdit_       = nullptr;
    QLineEdit*        pathEdit_      = nullptr;
    QPushButton*      browseBtn_     = nullptr;
    QLabel*           statusLabel_   = nullptr;
    QProgressBar*     progressBar_   = nullptr;
    QDialogButtonBox* buttons_       = nullptr;

    /// True once the user has typed (or browsed for) a custom path,
    /// so the URL→path autofill stops overwriting their choice.
    bool    pathEditedByUser_ = false;
    QString clonedPath_;

    /// Set when a clone fetched but failed to check out and git kept
    /// the repository: the Clone button reads "Open Repository" and
    /// opens it. Editing the URL or path clears it.
    QString keptClonePath_;

    /// Throttles the UI-thread progress updates. git can report
    /// progress many times per second; we only repaint when at least
    /// ~50 ms has passed since the last update (or when the phase
    /// changes). Keeps the dialog smooth without dropping updates
    /// that matter.
    QElapsedTimer progressThrottle_;
    int           lastPhase_ = -1;

    /// Clone-cancellation state. cancelFlag_ is shared with the
    /// worker (GitProcess::clone polls it while git runs);
    /// cloning_ gates reject()/closeEvent(); cancelRequested_
    /// distinguishes "user cancelled" from a genuine clone error
    /// when the worker finishes.
    std::shared_ptr<std::atomic<bool>> cancelFlag_;
    bool cloning_         = false;
    bool cancelRequested_ = false;
};

} // namespace gitbolt::dialogs
