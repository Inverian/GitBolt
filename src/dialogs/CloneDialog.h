#pragma once
#include "git/Repository.h"
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
/// then runs `git_clone()` on a background thread (via QtConcurrent)
/// while showing a busy status message. On success the dialog closes
/// with `QDialog::Accepted` and `clonedPath()` returns the absolute
/// path of the new working directory; on failure it stays open with
/// the error message visible so the user can correct the URL/path
/// and retry without re-entering everything.
class CloneDialog : public QDialog {
    Q_OBJECT
public:
    explicit CloneDialog(QWidget* parent = nullptr);

    /// Absolute path the repo was cloned to (only valid after the
    /// dialog returns QDialog::Accepted).
    QString clonedPath() const { return clonedPath_; }

protected:
    /// While a clone is running, Cancel / Esc / the window close
    /// button all become "cancel the clone" (flip the atomic flag
    /// libgit2 polls) instead of closing the dialog — the worker
    /// holds a pointer to this dialog, so it must stay alive until
    /// the clone loop actually stops.
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

    /// Throttles the UI-thread progress updates. libgit2 fires the
    /// transfer_progress callback many times per second; we only
    /// repaint when at least ~50 ms has passed since the last update
    /// (or when the phase changes). Keeps the dialog smooth without
    /// dropping updates that matter.
    QElapsedTimer progressThrottle_;
    int           lastPhase_ = -1;

    /// Clone-cancellation state. cancelFlag_ is shared with the
    /// worker (libgit2 polls it from its progress callbacks);
    /// cloning_ gates reject()/closeEvent(); cancelRequested_
    /// distinguishes "user cancelled" from a genuine clone error
    /// when the worker finishes.
    std::shared_ptr<std::atomic<bool>> cancelFlag_;
    bool cloning_         = false;
    bool cancelRequested_ = false;
};

} // namespace gitbolt::dialogs
