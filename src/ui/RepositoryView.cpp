#include "ui/RepositoryView.h"
#include "models/CommitLogModel.h"
#include "services/GitService.h"
#include "widgets/RevisionGraphWidget.h"
#include "widgets/DiffViewerWidget.h"
#include "git/Diff.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <memory>
#include <QLabel>
#include <QListWidget>
#include <QSplitter>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace gitbolt::ui {

RepositoryView::RepositoryView(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

void RepositoryView::setupUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    mainSplitter_ = new QSplitter(Qt::Vertical, this);

    // --- Top: Revision graph ---
    graphWidget_ = new widgets::RevisionGraphWidget(this);
    mainSplitter_->addWidget(graphWidget_);

    // --- Bottom: Diff viewer (left) + Commit details (right) ---
    bottomSplitter_ = new QSplitter(Qt::Horizontal, this);

    // Bottom-left: diff viewer
    diffWidget_ = new widgets::DiffViewerWidget(this);
    bottomSplitter_->addWidget(diffWidget_);

    // Bottom-right: commit detail panel
    detailPanel_ = new QWidget(this);
    auto* detailLayout = new QVBoxLayout(detailPanel_);
    detailLayout->setContentsMargins(4, 4, 4, 4);

    detailBrowser_ = new QTextBrowser(detailPanel_);
    detailBrowser_->setOpenExternalLinks(false);
    detailBrowser_->setPlaceholderText(tr("Select a commit to view details"));
    detailLayout->addWidget(detailBrowser_, 3);

    auto* filesLabel = new QLabel(tr("Changed Files:"), detailPanel_);
    detailLayout->addWidget(filesLabel);

    changedFilesList_ = new QListWidget(detailPanel_);
    detailLayout->addWidget(changedFilesList_, 2);

    bottomSplitter_->addWidget(detailPanel_);
    bottomSplitter_->setStretchFactor(0, 3);
    bottomSplitter_->setStretchFactor(1, 2);

    mainSplitter_->addWidget(bottomSplitter_);
    mainSplitter_->setStretchFactor(0, 3);
    mainSplitter_->setStretchFactor(1, 2);

    layout->addWidget(mainSplitter_);

    // Connect graph selection to our handler
    connect(graphWidget_, &widgets::RevisionGraphWidget::commitSelected,
            this, &RepositoryView::onCommitSelected);
}

void RepositoryView::setGitService(services::GitService* service)
{
    gitService_ = service;
}

void RepositoryView::setCommitLogModel(models::CommitLogModel* model)
{
    commitModel_ = model;
    graphWidget_->setModel(model);
}

widgets::RevisionGraphWidget* RepositoryView::revisionGraph() const
{
    return graphWidget_;
}

void RepositoryView::onCommitSelected(const QString& commitHash)
{
    if (!commitModel_)
        return;

    // Find the commit in the model by scanning for the matching hash
    git::ObjectId targetId = git::ObjectId::fromHex(commitHash.toStdString());

    for (int row = 0; row < commitModel_->rowCount(); ++row) {
        const auto* commit = commitModel_->commitAt(row);
        if (commit && commit->id == targetId) {
            showCommitDetails(*commit);
            showCommitDiff(commit->id);
            return;
        }
    }
}

void RepositoryView::showCommitDetails(const git::CommitData& commit)
{
    // Build HTML for the commit detail browser
    QString html;
    html += QStringLiteral("<h3>%1</h3>").arg(
        QString::fromStdString(commit.summary).toHtmlEscaped());

    if (commit.message.size() > commit.summary.size()) {
        QString body = QString::fromStdString(commit.message).mid(
            static_cast<int>(commit.summary.size())).trimmed();
        if (!body.isEmpty()) {
            html += QStringLiteral("<pre style=\"white-space:pre-wrap;\">%1</pre>")
                .arg(body.toHtmlEscaped());
        }
    }

    html += QStringLiteral("<table>");
    html += QStringLiteral("<tr><td><b>Author:</b></td><td>%1 &lt;%2&gt;</td></tr>")
        .arg(QString::fromStdString(commit.author.name).toHtmlEscaped(),
             QString::fromStdString(commit.author.email).toHtmlEscaped());

    auto authorTime = std::chrono::system_clock::to_time_t(commit.author.when);
    html += QStringLiteral("<tr><td><b>Date:</b></td><td>%1</td></tr>")
        .arg(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(authorTime))
             .toString(Qt::RFC2822Date));

    html += QStringLiteral("<tr><td><b>Commit:</b></td><td><code>%1</code></td></tr>")
        .arg(QString::fromStdString(commit.id.toHex()).toHtmlEscaped());

    if (!commit.parentIds.empty()) {
        QStringList parents;
        for (const auto& pid : commit.parentIds) {
            parents << QString::fromStdString(pid.toShortHex());
        }
        html += QStringLiteral("<tr><td><b>Parents:</b></td><td><code>%1</code></td></tr>")
            .arg(parents.join(", "));
    }
    html += QStringLiteral("</table>");

    detailBrowser_->setHtml(html);
}

void RepositoryView::showCommitDiff(const git::ObjectId& commitId)
{
    // Disconnect any previous file-list selection handler
    disconnect(changedFilesList_, &QListWidget::currentRowChanged, this, nullptr);

    changedFilesList_->clear();
    diffWidget_->clear();

    if (!gitService_ || !gitService_->repository())
        return;

    auto* repo = gitService_->repository();
    auto diffResult = repo->diffCommit(commitId);
    if (!diffResult)
        return;

    // Store the diff result so the file-list lambda can reference it safely
    auto diff = std::make_shared<git::DiffResult>(std::move(*diffResult));

    // Populate changed files list
    for (const auto& file : diff->files) {
        QString prefix;
        switch (file.status) {
            case git::DiffStatus::Added:    prefix = QStringLiteral("[A] "); break;
            case git::DiffStatus::Deleted:  prefix = QStringLiteral("[D] "); break;
            case git::DiffStatus::Modified: prefix = QStringLiteral("[M] "); break;
            case git::DiffStatus::Renamed:  prefix = QStringLiteral("[R] "); break;
            case git::DiffStatus::Copied:   prefix = QStringLiteral("[C] "); break;
            default:                        prefix = QStringLiteral("[?] "); break;
        }
        changedFilesList_->addItem(prefix + QString::fromStdString(file.path()));
    }

    // Show full diff in the diff viewer
    if (!diff->files.empty()) {
        diffWidget_->setDiff(*diff, 0);
    }

    // When user clicks a file in the list, show that file's diff
    connect(changedFilesList_, &QListWidget::currentRowChanged,
            this, [this, diff](int row) {
                if (row >= 0 && row < static_cast<int>(diff->files.size())) {
                    diffWidget_->setDiff(*diff, row);
                }
            });
}

} // namespace gitbolt::ui
