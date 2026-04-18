#pragma once

#include <QDialog>

class QLabel;
class QSplitter;
class QCloseEvent;
class QShowEvent;

namespace gitbolt::services { class GitService; }
namespace gitbolt::conf     { class SettingsService; }
namespace gitbolt::widgets {
class StagingWidget;
class CommitEditorWidget;
} // namespace gitbolt::widgets

namespace gitbolt::dialogs {

/// A modeless top-level dialog that hosts the staging view and the
/// commit message editor, matching the GitExtensions "Commit" window.
/// It owns its own signal wiring to GitService so MainWindow doesn't
/// have to — all commit/staging connections live here and are torn
/// down automatically when the dialog is destroyed.
class CommitDialog : public QDialog {
    Q_OBJECT
public:
    CommitDialog(services::GitService* svc,
                 conf::SettingsService* settings,
                 QWidget* parent = nullptr);
    ~CommitDialog() override;

    /// Kick a fresh status refresh so the staging view reflects the
    /// current working-tree state. Called on show().
    void refresh();

protected:
    void showEvent(QShowEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    void validateGeometryOnScreen();

    services::GitService*       svc_      = nullptr;
    conf::SettingsService*      settings_ = nullptr;

    QSplitter*                  splitter_    = nullptr;
    widgets::StagingWidget*     staging_     = nullptr;
    widgets::CommitEditorWidget* editor_     = nullptr;
    QLabel*                     errorLabel_  = nullptr;

    // Distinguishes "this dialog asked for a commit" from any other
    // code path that might also emit commitComplete (e.g. a future
    // CLI integration). Set true when the user clicks Commit here,
    // and reset on the next commitComplete signal. The dialog only
    // closes itself when this flag is true.
    bool pendingCommit_ = false;

    // Prevents restoreGeometry from running twice across repeated
    // show/hide cycles of the cached dialog instance.
    bool restored_ = false;
};

} // namespace gitbolt::dialogs
