#include "dialogs/CommitDialog.h"

#include "conf/SettingsService.h"
#include "services/GitService.h"
#include "widgets/CommitEditorWidget.h"
#include "widgets/StagingWidget.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QLabel>
#include <QMessageBox>
#include <QScreen>
#include <QShowEvent>
#include <QSplitter>
#include <QVBoxLayout>

namespace gitbolt::dialogs {

namespace {
constexpr const char* kGeometryKey = "commitDialogGeom/v1";
constexpr const char* kSplitterKey = "commitSplitter/v1";
} // namespace

CommitDialog::CommitDialog(services::GitService* svc,
                           conf::SettingsService* settings,
                           QWidget* parent)
    : QDialog(parent, Qt::Window | Qt::WindowCloseButtonHint |
                       Qt::WindowMinMaxButtonsHint)
    , svc_(svc)
    , settings_(settings)
{
    setWindowTitle(tr("Commit"));
    setModal(false);
    resize(720, 620);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    splitter_ = new QSplitter(Qt::Vertical, this);
    splitter_->setChildrenCollapsible(false);

    staging_ = new widgets::StagingWidget(this);
    editor_  = new widgets::CommitEditorWidget(this);

    splitter_->addWidget(staging_);
    splitter_->addWidget(editor_);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);

    layout->addWidget(splitter_, 1);

    // Inline error surface — hidden until a commit fails. We prefer
    // this over QMessageBox so the user's typed message stays visible
    // alongside the failure reason.
    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    errorLabel_->setStyleSheet(QStringLiteral(
        "QLabel { background:#5a1f1f; color:#ffdada; padding:6px 10px; }"));
    errorLabel_->hide();
    layout->addWidget(errorLabel_);

    // --- Staging <-> GitService wiring ---
    // Every signal hooked here is auto-disconnected when the dialog
    // (and thus the staging widget it parents) is destroyed.
    connect(staging_, &widgets::StagingWidget::stageRequested,
            svc_,     &services::GitService::stageFile);
    connect(staging_, &widgets::StagingWidget::unstageRequested,
            svc_,     &services::GitService::unstageFile);
    connect(staging_, &widgets::StagingWidget::stageAllRequested,
            svc_,     &services::GitService::stageAll);
    connect(staging_, &widgets::StagingWidget::unstageAllRequested,
            svc_,     &services::GitService::unstageAll);
    connect(staging_, &widgets::StagingWidget::discardRequested,
            this,     [this](const QString& path) {
                // Parent the confirmation on the dialog so it centers
                // on the dialog, not the MainWindow behind it.
                const auto answer = QMessageBox::question(this,
                    tr("Discard changes?"),
                    tr("Discard all uncommitted changes to %1?\n\n"
                       "This cannot be undone.").arg(path),
                    QMessageBox::Discard | QMessageBox::Cancel,
                    QMessageBox::Cancel);
                if (answer == QMessageBox::Discard)
                    svc_->discardFile(path);
            });

    // --- GitService -> Staging ---
    connect(svc_, &services::GitService::statusReady,
            staging_, &widgets::StagingWidget::setEntries);

    // --- Commit editor -> GitService ---
    // IMPORTANT: forward the `amend` flag. The previous MainWindow
    // connection silently dropped it by declaring a single-arg lambda.
    connect(editor_, &widgets::CommitEditorWidget::commitRequested,
            this,    [this](const QString& msg, bool amend) {
                if (msg.trimmed().isEmpty()) {
                    errorLabel_->setText(tr("Commit message cannot be empty."));
                    errorLabel_->show();
                    return;
                }
                errorLabel_->hide();
                pendingCommit_ = true;
                editor_->setEnabled(false);
                svc_->commitChanges(msg, amend);
            });

    // --- GitService -> commit result ---
    connect(svc_, &services::GitService::commitComplete,
            this, [this](bool success, const QString& message) {
                if (!pendingCommit_)
                    return; // some other code path committed; ignore
                pendingCommit_ = false;
                editor_->setEnabled(true);
                if (success) {
                    editor_->clear();
                    errorLabel_->hide();
                    close();
                } else {
                    errorLabel_->setText(
                        tr("Commit failed: %1").arg(message));
                    errorLabel_->show();
                }
            });

    connect(svc_, &services::GitService::operationFailed,
            this, [this](const QString& op, const QString& err) {
                if (op.startsWith(QLatin1String("commit"), Qt::CaseInsensitive) ||
                    op.startsWith(QLatin1String("stage"),  Qt::CaseInsensitive) ||
                    op.startsWith(QLatin1String("unstage"),Qt::CaseInsensitive) ||
                    op.startsWith(QLatin1String("discard"),Qt::CaseInsensitive)) {
                    errorLabel_->setText(op + tr(" failed: ") + err);
                    errorLabel_->show();
                    editor_->setEnabled(true);
                    pendingCommit_ = false;
                }
            });
}

CommitDialog::~CommitDialog() = default;

void CommitDialog::refresh()
{
    if (svc_)
        svc_->refreshStatus();
}

void CommitDialog::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);

    if (!restored_ && settings_) {
        const QByteArray geom = settings_->restoreDialogGeometry(
            QString::fromLatin1(kGeometryKey));
        if (!geom.isEmpty())
            restoreGeometry(geom);

        const QByteArray split = settings_->restoreSplitterState(
            QString::fromLatin1(kSplitterKey));
        if (!split.isEmpty())
            splitter_->restoreState(split);

        validateGeometryOnScreen();
        restored_ = true;
    }

    // Always refresh status on show so the staging panels reflect
    // whatever the working tree looks like right now.
    refresh();
}

void CommitDialog::closeEvent(QCloseEvent* e)
{
    if (settings_) {
        settings_->saveDialogGeometry(
            QString::fromLatin1(kGeometryKey), saveGeometry());
        settings_->saveSplitterState(
            QString::fromLatin1(kSplitterKey), splitter_->saveState());
    }
    QDialog::closeEvent(e);
}

// If the dialog was last closed on a monitor that is no longer
// attached, restoreGeometry will happily place it off-screen and
// the user won't be able to see it. Detect that case and fall back
// to a centered-on-parent layout.
void CommitDialog::validateGeometryOnScreen()
{
    const QPoint center = frameGeometry().center();
    if (QGuiApplication::screenAt(center) != nullptr)
        return;

    resize(720, 620);
    if (auto* p = parentWidget()) {
        const QRect pg = p->geometry();
        move(pg.center().x() - width() / 2,
             pg.center().y() - height() / 2);
    }
}

} // namespace gitbolt::dialogs
