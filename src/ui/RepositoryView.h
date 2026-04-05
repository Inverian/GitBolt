#pragma once
#include "git/Commit.h"
#include "git/Diff.h"
#include <QWidget>
#include <string>

class QLabel;
class QListWidget;
class QSplitter;
class QTextBrowser;

namespace gitbolt::models { class CommitLogModel; }
namespace gitbolt::services { class GitService; }

namespace gitbolt::widgets {
class RevisionGraphWidget;
class DiffViewerWidget;
} // namespace gitbolt::widgets

namespace gitbolt::ui {

class RepositoryView : public QWidget {
    Q_OBJECT
public:
    explicit RepositoryView(QWidget* parent = nullptr);

    void setGitService(services::GitService* service);
    void setCommitLogModel(models::CommitLogModel* model);
    widgets::RevisionGraphWidget* revisionGraph() const;

public slots:
    void onCommitSelected(const QString& commitHash);

private:
    void setupUi();
    void showCommitDetails(const git::CommitData& commit);
    void showCommitDiff(const git::ObjectId& commitId);

    widgets::RevisionGraphWidget* graphWidget_ = nullptr;
    widgets::DiffViewerWidget* diffWidget_ = nullptr;

    // Commit detail panel widgets
    QWidget* detailPanel_ = nullptr;
    QTextBrowser* detailBrowser_ = nullptr;
    QListWidget* changedFilesList_ = nullptr;

    QSplitter* mainSplitter_ = nullptr;
    QSplitter* bottomSplitter_ = nullptr;

    services::GitService* gitService_ = nullptr;
    models::CommitLogModel* commitModel_ = nullptr;
};

} // namespace gitbolt::ui
