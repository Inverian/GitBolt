//
// TestCommitDetails — selecting a commit in the repository view
// (RepositoryView::onCommitSelected), and how much of a commit message
// the views lay out:
//
//   - nothing on the way waits for the repository. The commit's diff
//     (its Stats row and the Diff tab) and its file tree come from the
//     service's read thread (GitService::readAsync), and the "contained
//     in" query's git from the process the service made as it opened.
//     On the GUI thread, a commit with a huge or deeply nested tree
//     froze the window while libgit2 walked it, for minutes, holding
//     the repository from every refresh meanwhile. Here another thread
//     holds the repository for a few seconds, as a long read or refresh
//     does: the selection returns at once, the Stats row says
//     "(counting…)", and the numbers, the changed files and the tree
//     come in once the repository is free;
//   - a diff for a commit selected before comes to nothing;
//   - a subject of one line of 4 MB shows cut, with a note that says
//     so, in a moment: the browser laid it out whole, superlinearly,
//     and the window froze for minutes. The log's Message column, the
//     reflog, the stash list and tooltips, the Cherry Pick and Rebase
//     dialogs and the rebase plan cut it too;
//   - an ordinary subject and body show exactly as they are;
//   - an author name or address of megabytes (git takes an ident of
//     any length) shows cut too, there and in the log, the blame, the
//     Cherry Pick dialog, a tag's tooltip and the reflog;
//   - Copy in the log copies a message whole, as it did before the
//     cell had only its start.
//

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QSettings>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTreeView>

#include "../TestRepoHelper.h"
#include "dialogs/CherryPickDialog.h"
#include "dialogs/RebaseDialog.h"
#include "dialogs/ReflogDialog.h"
#include "dialogs/StashManageDialog.h"
#include "git/Blame.h"
#include "models/BlameModel.h"
#include "models/BranchModel.h"
#include "models/CommitLogModel.h"
#include "services/GitService.h"
#include "ui/RepositoryView.h"
#include "util/MessageText.h"
#include "widgets/InteractiveRebaseWidget.h"
#include "widgets/RevisionGraphWidget.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using gitbolt::models::CommitLogModel;
using gitbolt::services::GitService;
using gitbolt::test::TestRepo;
using gitbolt::ui::RepositoryView;

namespace {

const QChar kEllipsis(0x2026);

// The service's repository lock, held on a thread of its own until
// release() or until `ms` have passed: what a long read or refresh on
// a worker looks like to the GUI thread.
class RepoHeld {
public:
    RepoHeld(std::mutex& repoMutex, int ms) {
        thread_ = std::thread([this, &repoMutex, ms] {
            std::lock_guard<std::mutex> repo(repoMutex);
            std::unique_lock<std::mutex> lock(m_);
            locked_ = true;
            cv_.notify_all();
            cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return released_; });
        });
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [this] { return locked_; });
    }
    ~RepoHeld() {
        release();
        thread_.join();
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(m_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool locked_ = false;
    bool released_ = false;
    std::thread thread_;
};

// The view's browser that shows `text`: "Author:" is in the metadata,
// "Contained in" under the message.
QTextBrowser* browserWith(const QWidget& view, const QString& text) {
    for (QTextBrowser* browser : view.findChildren<QTextBrowser*>()) {
        if (browser->toPlainText().contains(text))
            return browser;
    }
    return nullptr;
}

// The labels of every row under `parent`, depth first.
QStringList rows(const QAbstractItemModel* model, const QModelIndex& parent = {}) {
    QStringList labels;
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex index = model->index(row, 0, parent);
        labels << index.data().toString();
        labels << rows(model, index);
    }
    return labels;
}

gitbolt::git::CommitData commitData(TestRepo& repo, const gitbolt::git::ObjectId& id) {
    auto data = repo.repo().lookupCommit(id);
    if (!data)
        qFatal("lookupCommit failed");
    return *data;
}

QString hex(const gitbolt::git::ObjectId& id) {
    return QString::fromStdString(id.toHex());
}

} // namespace

class TestCommitDetails : public QObject {
    Q_OBJECT

private slots:
    // The dialogs keep their sizes in GitBolt's settings: here, in a
    // folder of the test's own, not the user's.
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    }

    void selectionDoesNotWaitForTheRepository() {
        TestRepo repo;
        repo.writeFile(QStringLiteral("a.txt"), "one\ntwo\n");
        repo.writeFile(QStringLiteral("dir/b.txt"), "three\n");
        QVERIFY(repo.stageAll().ok());
        auto root = repo.commit(QStringLiteral("root"));
        QVERIFY(root.ok());

        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        CommitLogModel model;
        model.setCommits({commitData(repo, *root)});
        RepositoryView view;
        view.setGitService(&svc);
        view.setCommitLogModel(&model);
        view.resize(1000, 700);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        auto* diffFiles = view.findChild<QTreeView*>(QStringLiteral("diff.files"));
        auto* fileTree = view.findChild<QTreeView*>(QStringLiteral("fileTree.view"));
        QVERIFY(diffFiles && fileTree);

        QTextBrowser* details = nullptr;
        {
            RepoHeld held(svc.repoMutex(), 3000);
            QElapsedTimer timer;
            timer.start();
            view.onCommitSelected(hex(*root));
            QVERIFY2(timer.elapsed() < 1000,
                     qPrintable(QStringLiteral("the selection took %1 ms").arg(timer.elapsed())));
            details = browserWith(view, QStringLiteral("Author:"));
            QVERIFY(details);
            QVERIFY2(details->toPlainText().contains(QStringLiteral("(counting…)")),
                     qPrintable(details->toPlainText()));
            QVERIFY(browserWith(view, QStringLiteral("Contained in")));
            QCOMPARE(diffFiles->model()->rowCount(), 0);
            QCOMPARE(fileTree->model()->rowCount(), 0);
            held.release();
        }
        QTRY_VERIFY2(details->toPlainText().contains(QStringLiteral("2 files changed")),
                     qPrintable(details->toPlainText()));
        QVERIFY(details->toPlainText().contains(QStringLiteral("+3")));
        QVERIFY(!details->toPlainText().contains(QStringLiteral("(counting…)")));
        QTRY_COMPARE(rows(diffFiles->model()).size(), 3);   // dir, b.txt, a.txt
        QTRY_COMPARE(rows(fileTree->model()).size(), 3);

        // Shown again (a theme switch renders the commit again), the
        // numbers are there at once.
        view.onCommitSelected(hex(*root));
        QVERIFY(details->toPlainText().contains(QStringLiteral("2 files changed")));
    }

    // Selected one after the other while the repository is busy: the
    // first commit's diff, when its turn comes, is for nothing, and
    // what shows is the second's alone.
    void aDiffForAnEarlierSelectionIsDropped() {
        TestRepo repo;
        auto first = repo.writeAndCommit(QStringLiteral("first.txt"), "1\n", QStringLiteral("first"));
        QVERIFY(first.ok());
        repo.writeFile(QStringLiteral("second.txt"), "2\n");
        repo.writeFile(QStringLiteral("third.txt"), "3\n");
        QVERIFY(repo.stageAll().ok());
        auto second = repo.commit(QStringLiteral("second"));
        QVERIFY(second.ok());

        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        CommitLogModel model;
        model.setCommits({commitData(repo, *second), commitData(repo, *first)});
        RepositoryView view;
        view.setGitService(&svc);
        view.setCommitLogModel(&model);
        view.resize(1000, 700);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        auto* diffFiles = view.findChild<QTreeView*>(QStringLiteral("diff.files"));
        QVERIFY(diffFiles);

        {
            RepoHeld held(svc.repoMutex(), 3000);
            view.onCommitSelected(hex(*first));
            view.onCommitSelected(hex(*second));
            held.release();
        }
        QTextBrowser* details = browserWith(view, QStringLiteral("Author:"));
        QVERIFY(details);
        QTRY_VERIFY2(details->toPlainText().contains(QStringLiteral("2 files changed")),
                     qPrintable(details->toPlainText()));
        QTRY_COMPARE(rows(diffFiles->model()).size(), 2);
        QTest::qWait(300);   // anything late would be in by now
        const QStringList files = rows(diffFiles->model());
        QCOMPARE(files.size(), 2);
        QVERIFY2(!files.join(QLatin1Char('|')).contains(QStringLiteral("first.txt")),
                 qPrintable(files.join(QLatin1Char('|'))));
        QVERIFY(details->toPlainText().contains(QStringLiteral("2 files changed")));

        // And a diff read already, its answer on its way to this thread
        // when the next commit is selected (no event is handled until
        // then).
        view.onCommitSelected(hex(*first));
        QElapsedTimer waited;
        waited.start();
        while (svc.workRunning() && waited.elapsed() < 10000)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        QVERIFY(!svc.workRunning());
        view.onCommitSelected(hex(*second));
        QTRY_COMPARE(rows(diffFiles->model()).size(), 2);
        QTest::qWait(300);
        QCOMPARE(rows(diffFiles->model()).size(), 2);
        QVERIFY(!rows(diffFiles->model()).join(QLatin1Char('|')).contains(QStringLiteral("first.txt")));
        QTRY_VERIFY(details->toPlainText().contains(QStringLiteral("2 files changed")));
    }

    void aHugeSubjectIsShownCut() {
        TestRepo repo;
        const QString subject(4 * 1024 * 1024, QLatin1Char('a'));   // one line, no newline
        auto id = repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", subject);
        QVERIFY(id.ok());
        const auto commit = commitData(repo, *id);
        QCOMPARE(commit.summary.size(), static_cast<size_t>(subject.size()));

        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        CommitLogModel model;
        model.setCommits({commit});
        RepositoryView view;
        view.setGitService(&svc);
        view.setCommitLogModel(&model);
        view.resize(1000, 700);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        QElapsedTimer timer;
        timer.start();
        view.onCommitSelected(hex(*id));
        QTextBrowser* message = browserWith(view, QStringLiteral("Contained in"));
        QVERIFY(message);
        message->document()->documentLayout()->documentSize();
        message->viewport()->grab();
        QVERIFY2(timer.elapsed() < 5000,
                 qPrintable(QStringLiteral("showing it took %1 ms").arg(timer.elapsed())));

        const QString text = message->toPlainText();
        QVERIFY2(text.size() < 3 * gitbolt::util::kMaxParagraphChars, qPrintable(text.left(200)));
        QVERIFY(text.startsWith(QString(gitbolt::util::kMaxParagraphChars, QLatin1Char('a'))
                                + kEllipsis));
        const QString note = QStringLiteral("This subject is %1 characters long: only its start is shown.")
                                 .arg(QLocale().toString(qlonglong(subject.size())));
        QVERIFY2(text.contains(note), qPrintable(text.right(400)));

        // The log's Message column, which the view lays out on every paint.
        const QString cell = model.data(model.index(0, static_cast<int>(
                                            gitbolt::models::CommitLogColumn::Message)))
                                 .toString();
        QCOMPARE(cell, QString(gitbolt::util::kMaxLineChars, QLatin1Char('a')) + kEllipsis);
        model.setShowMessageBody(true);
        QCOMPARE(model.data(model.index(0, static_cast<int>(
                                gitbolt::models::CommitLogColumn::Message)))
                     .toString(),
                 cell);
    }

    // The same for a body with a line of megabytes (1 MB here).
    void aHugeBodyIsShownCut() {
        TestRepo repo;
        const QString line(1024 * 1024, QLatin1Char('b'));
        auto id = repo.writeAndCommit(QStringLiteral("a.txt"), "a\n",
                                      QStringLiteral("Subject\n\nFirst line.\n") + line
                                          + QStringLiteral("\nLast line.\n"));
        QVERIFY(id.ok());
        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        CommitLogModel model;
        model.setCommits({commitData(repo, *id)});
        RepositoryView view;
        view.setGitService(&svc);
        view.setCommitLogModel(&model);
        view.resize(1000, 700);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        QElapsedTimer timer;
        timer.start();
        view.onCommitSelected(hex(*id));
        QTextBrowser* message = browserWith(view, QStringLiteral("Contained in"));
        QVERIFY(message);
        message->document()->documentLayout()->documentSize();
        message->viewport()->grab();
        QVERIFY2(timer.elapsed() < 5000,
                 qPrintable(QStringLiteral("showing it took %1 ms").arg(timer.elapsed())));
        const QString text = message->toPlainText();
        QVERIFY2(text.size() < 3 * gitbolt::util::kMaxParagraphChars, qPrintable(text.left(200)));
        QVERIFY(text.startsWith(QStringLiteral("Subject\n")));
        QVERIFY2(text.contains(QStringLiteral("First line.\n")
                               + QString(gitbolt::util::kMaxParagraphChars, QLatin1Char('b'))
                               + kEllipsis + QStringLiteral("\nLast line.")),
                 qPrintable(text.right(300)));
        QVERIFY2(text.contains(QStringLiteral("This message is ")), qPrintable(text.right(300)));
    }

    void anOrdinaryMessageShowsAsItIs() {
        // Long for a message, but ordinary: a subject of some 300
        // characters, 300 lines of body, and a pasted line of 3,000
        // characters with no space in it.
        QString subject =
            QStringLiteral("Teach the parser <tags> & \"quotes\" — 100% %1 of the time");
        while (subject.size() < 300)
            subject += QStringLiteral(", and then some");
        QString body = QStringLiteral("First paragraph, line one.\nLine two.\n\n"
                                      "    indented();   // kept as it is\n\n");
        for (int i = 0; i < 300; ++i)
            body += QStringLiteral("Line %1 of a long body, with some words in it.\n").arg(i);
        body += QString(3000, QLatin1Char('w'));
        TestRepo repo;
        auto id = repo.writeAndCommit(QStringLiteral("a.txt"), "a\n",
                                      subject + QStringLiteral("\n\n") + body + QLatin1Char('\n'));
        QVERIFY(id.ok());

        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        CommitLogModel model;
        model.setCommits({commitData(repo, *id)});
        RepositoryView view;
        view.setGitService(&svc);
        view.setCommitLogModel(&model);
        view.resize(1000, 700);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.onCommitSelected(hex(*id));

        QTextBrowser* message = browserWith(view, QStringLiteral("Contained in"));
        QVERIFY(message);
        const QString text = message->toPlainText();
        QVERIFY2(text.startsWith(subject + QLatin1Char('\n')), qPrintable(text.left(200)));
        QVERIFY2(text.contains(body), qPrintable(text.left(400)));
        QVERIFY(!text.contains(body + kEllipsis));
        QVERIFY(!text.contains(QStringLiteral("characters long")));
        QCOMPARE(model.data(model.index(0, static_cast<int>(
                                gitbolt::models::CommitLogColumn::Message)))
                     .toString(),
                 subject);
    }

    // The other places a subject (or a stash's or reflog entry's message,
    // which git builds from one) shows: each lays its text out whole, a
    // list on every paint. An ordinary one shows as it is.
    void otherViewsCutAHugeSubject() {
        const std::string huge(4 * 1024 * 1024, 'a');
        const QString cut = QString(gitbolt::util::kMaxLineChars, QLatin1Char('a')) + kEllipsis;
        const std::string ordinary = "Fix the frobnicator";

        gitbolt::git::CommitData commit;
        commit.id = gitbolt::git::ObjectId::fromHex(std::string(40, '1'));
        commit.parentIds = {gitbolt::git::ObjectId::fromHex(std::string(40, '2'))};
        commit.summary = commit.message = huge;
        gitbolt::git::CommitData plain = commit;
        plain.id = gitbolt::git::ObjectId::fromHex(std::string(40, '3'));
        plain.summary = ordinary;
        plain.message = ordinary + "\n\nBody.\n";

        // The stashes in the sidebar, and their tooltips.
        {
            gitbolt::models::BranchModel model;
            model.setStashes({{0, commit.id, "WIP on main: 1111111 " + huge, {}},
                              {1, plain.id, "WIP on main: 3333333 " + ordinary, {}}});
            const QModelIndex stashes =
                model.index(static_cast<int>(gitbolt::models::BranchModel::RootCategory::Stashes), 0);
            QCOMPARE(model.rowCount(stashes), 2);
            const QString shown = model.data(model.index(0, 0, stashes)).toString();
            QCOMPARE(shown.size(), gitbolt::util::kMaxLineChars + 1);
            QVERIFY(shown.endsWith(kEllipsis));
            QVERIFY(model.data(model.index(0, 0, stashes), Qt::ToolTipRole).toString().size()
                    < 2 * gitbolt::util::kMaxParagraphChars);
            QCOMPARE(model.data(model.index(1, 0, stashes)).toString(),
                     QStringLiteral("WIP on main: 3333333 Fix the frobnicator"));
        }
        // An annotated tag's message, in its tooltip.
        {
            gitbolt::models::BranchModel model;
            gitbolt::git::TagInfo tag;
            tag.name = "v1";
            tag.fullRefName = "refs/tags/v1";
            tag.type = gitbolt::git::TagType::Annotated;
            tag.message = huge + "\n";
            model.setTags({tag});
            const QModelIndex tags =
                model.index(static_cast<int>(gitbolt::models::BranchModel::RootCategory::Tags), 0);
            QVERIFY(model.data(model.index(0, 0, tags), Qt::ToolTipRole).toString().size()
                    < 2 * gitbolt::util::kMaxParagraphChars);
        }
        // The reflog: "commit: <subject>".
        {
            gitbolt::dialogs::ReflogDialog dialog;
            gitbolt::git::ReflogEntry entry;
            entry.message = "commit: " + huge;
            gitbolt::git::ReflogEntry other;
            other.message = "commit: " + ordinary;
            dialog.setEntries({entry, other});
            auto* table = dialog.findChild<QTableWidget*>();
            QVERIFY(table);
            QCOMPARE(table->item(0, 4)->text().size(), gitbolt::util::kMaxLineChars + 1);
            QCOMPARE(table->item(1, 4)->text(), QStringLiteral("commit: Fix the frobnicator"));
        }
        // Cherry Pick's Message row.
        {
            gitbolt::dialogs::CherryPickDialog dialog;
            dialog.setCommitDetails(commit);
            bool found = false;
            for (QLabel* label : dialog.findChildren<QLabel*>()) {
                if (label->text().startsWith(QStringLiteral("aaaa"))) {
                    QCOMPARE(label->text(), cut);
                    found = true;
                }
            }
            QVERIFY(found);
            dialog.setCommitDetails(plain);
            bool foundPlain = false;
            for (QLabel* label : dialog.findChildren<QLabel*>())
                foundPlain = foundPlain || label->text() == QStringLiteral("Fix the frobnicator");
            QVERIFY(foundPlain);
        }
        // The Rebase dialog's list of the commits to replay.
        {
            gitbolt::dialogs::RebaseDialog dialog;
            dialog.setCommitsToRebase({commit, plain}, gitbolt::git::ObjectId(), commit.id, "main");
            QListWidget* preview = nullptr;
            for (QListWidget* list : dialog.findChildren<QListWidget*>()) {
                if (list->count() == 2)
                    preview = list;
            }
            QVERIFY(preview);
            QVERIFY(preview->item(0)->text().endsWith(cut));
            QVERIFY(preview->item(0)->text().size() < gitbolt::util::kMaxLineChars + 20);
            QVERIFY(preview->item(1)->text().endsWith(QStringLiteral("  Fix the frobnicator")));
        }
        // The rebase plan.
        {
            gitbolt::widgets::InteractiveRebaseWidget widget;
            widget.setCommits({commit, plain}, gitbolt::git::ObjectId(), commit.id, "main");
            auto* list = widget.findChild<QListView*>();
            QVERIFY(list);
            const QAbstractItemModel* plan = list->model();
            QCOMPARE(plan->rowCount(), 2);
            QVERIFY(plan->data(plan->index(0, 0)).toString().endsWith(cut));
            QCOMPARE(plan->data(plan->index(0, 0),
                                gitbolt::widgets::RebaseListModel::CommitMessageRole)
                         .toString(),
                     cut);
            QCOMPARE(plan->data(plan->index(1, 0),
                                gitbolt::widgets::RebaseListModel::CommitMessageRole)
                         .toString(),
                     QStringLiteral("Fix the frobnicator"));
        }
        // Manage Stashes.
        {
            gitbolt::dialogs::StashManageDialog dialog;
            dialog.setStashes({{0, commit.id, "WIP on main: 1111111 " + huge, {}}});
            auto* list = dialog.findChild<QListWidget*>();
            QVERIFY(list);
            QCOMPARE(list->count(), 1);
            QVERIFY(list->item(0)->text().size() < gitbolt::util::kMaxLineChars + 20);
        }
    }

    // A commit whose author's name is one line of 1 MB, and whose
    // committer's address is another: the browser laid them out whole,
    // superlinearly (40 s for the name alone), and the log's Author
    // column laid the name out on every paint.
    void aHugeIdentIsShownCut() {
        TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("Subject")).ok());
        const std::string name(1024 * 1024, 'A');
        const std::string address = std::string(1024 * 1024, 'c') + "@example.com";
        const auto id = repo.rewriteHeadIdents(name + " <author@example.com>",
                                               "Committer <" + address + ">");
        const auto commit = commitData(repo, id);
        QCOMPARE(commit.author.name, name);
        QCOMPARE(commit.committer.email, address);

        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        CommitLogModel model;
        model.setCommits({commit});
        RepositoryView view;
        view.setGitService(&svc);
        view.setCommitLogModel(&model);
        view.resize(1000, 700);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        QElapsedTimer timer;
        timer.start();
        view.onCommitSelected(hex(id));
        QTextBrowser* details = browserWith(view, QStringLiteral("Author:"));
        QVERIFY(details);
        details->document()->documentLayout()->documentSize();
        details->viewport()->grab();
        QVERIFY2(timer.elapsed() < 5000,
                 qPrintable(QStringLiteral("showing it took %1 ms").arg(timer.elapsed())));

        const QString text = details->toPlainText();
        QVERIFY2(text.size() < 4 * gitbolt::util::kMaxLineChars, qPrintable(text.left(200)));
        QVERIFY2(text.contains(QString(gitbolt::util::kMaxLineChars, QLatin1Char('A')) + kEllipsis
                               + QStringLiteral(" <author@example.com>")),
                 qPrintable(text.left(200)));
        QVERIFY2(text.contains(QStringLiteral("Committer <")
                               + QString(gitbolt::util::kMaxLineChars, QLatin1Char('c')) + kEllipsis
                               + QLatin1Char('>')),
                 qPrintable(text.right(400)));

        // The log's Author column, which the view lays out on every
        // paint; Copy takes the name whole.
        const QModelIndex author =
            model.index(0, static_cast<int>(gitbolt::models::CommitLogColumn::Author));
        QCOMPARE(model.data(author).toString(),
                 QString(gitbolt::util::kMaxLineChars, QLatin1Char('A')) + kEllipsis);
        QCOMPARE(model.data(author, CommitLogModel::FULL_TEXT_ROLE).toString().size(),
                 qsizetype(name.size()));
    }

    // The other places an author's or tagger's name shows (1 MB here).
    // The blame gave every line of a hunk a copy of its own (64K here,
    // a copy per line being what it costs), and every one of them was
    // laid out on every paint. An ordinary one shows as it is.
    void otherViewsCutAHugeIdent() {
        const std::string huge(1024 * 1024, 'A');
        const QString cut = QString(gitbolt::util::kMaxLineChars, QLatin1Char('A')) + kEllipsis;

        // The blame: two hunks, the first of 300 lines.
        {
            gitbolt::git::BlameResult result;
            result.path = "a.txt";
            for (int i = 0; i < 301; ++i)
                result.lines.push_back("line " + std::to_string(i));
            gitbolt::git::BlameHunk hostile;
            hostile.commitId = gitbolt::git::ObjectId::fromHex(std::string(40, '1'));
            hostile.signature.name = std::string(64 * 1024, 'A');
            hostile.signature.when = std::chrono::system_clock::now();
            hostile.startLine = 1;
            hostile.lineCount = 300;
            gitbolt::git::BlameHunk plain = hostile;
            plain.commitId = gitbolt::git::ObjectId::fromHex(std::string(40, '2'));
            plain.signature.name = "Ada Lovelace";
            plain.startLine = 301;
            plain.lineCount = 1;
            result.hunks = {hostile, plain};
            gitbolt::models::BlameModel model;
            model.setBlameResult(result);
            QCOMPARE(model.rowCount(), 301);
            using gitbolt::models::BlameModel;
            for (int row : {0, 299})
                QCOMPARE(model.index(row, BlameModel::ColAuthor).data().toString(), cut);
            QCOMPARE(model.index(300, BlameModel::ColAuthor).data().toString(),
                     QStringLiteral("Ada Lovelace"));
        }
        // Cherry Pick's Author row.
        {
            gitbolt::git::CommitData commit;
            commit.id = gitbolt::git::ObjectId::fromHex(std::string(40, '1'));
            commit.summary = commit.message = "Fix the frobnicator";
            commit.author.name = huge;
            commit.author.email = huge + "@example.com";
            gitbolt::dialogs::CherryPickDialog dialog;
            dialog.setCommitDetails(commit);
            bool found = false;
            for (QLabel* label : dialog.findChildren<QLabel*>()) {
                if (label->text().startsWith(QStringLiteral("AAAA"))) {
                    QCOMPARE(label->text(), cut + QStringLiteral(" <") + cut + QLatin1Char('>'));
                    found = true;
                }
            }
            QVERIFY(found);
            commit.author.name = "Ada Lovelace";
            commit.author.email = "ada@example.com";
            dialog.setCommitDetails(commit);
            bool foundPlain = false;
            for (QLabel* label : dialog.findChildren<QLabel*>())
                foundPlain = foundPlain
                             || label->text() == QStringLiteral("Ada Lovelace <ada@example.com>");
            QVERIFY(foundPlain);
        }
        // An annotated tag's tagger, in its tooltip.
        {
            gitbolt::models::BranchModel model;
            gitbolt::git::TagInfo tag;
            tag.name = "v1";
            tag.fullRefName = "refs/tags/v1";
            tag.type = gitbolt::git::TagType::Annotated;
            tag.message = "Release 1\n";
            tag.tagger.name = huge;
            tag.tagger.email = huge + "@example.com";
            gitbolt::git::TagInfo plain = tag;
            plain.name = "v2";
            plain.fullRefName = "refs/tags/v2";
            plain.tagger.name = "Ada Lovelace";
            plain.tagger.email = "ada@example.com";
            model.setTags({tag, plain});
            const QModelIndex tags =
                model.index(static_cast<int>(gitbolt::models::BranchModel::RootCategory::Tags), 0);
            const QString tip = model.data(model.index(0, 0, tags), Qt::ToolTipRole).toString();
            QVERIFY2(tip.contains(QStringLiteral("Tagger: ") + cut + QStringLiteral(" <") + cut
                                  + QLatin1Char('>')),
                     qPrintable(tip.left(200)));
            QVERIFY(tip.size() < 3 * gitbolt::util::kMaxLineChars);
            QVERIFY(model.data(model.index(1, 0, tags), Qt::ToolTipRole)
                        .toString()
                        .contains(QStringLiteral("Tagger: Ada Lovelace <ada@example.com>")));
        }
        // The reflog's Committer column.
        {
            gitbolt::dialogs::ReflogDialog dialog;
            gitbolt::git::ReflogEntry entry;
            entry.committer.name = huge;
            entry.message = "commit: Fix the frobnicator";
            gitbolt::git::ReflogEntry other = entry;
            other.committer.name = "Ada Lovelace";
            dialog.setEntries({entry, other});
            auto* table = dialog.findChild<QTableWidget*>();
            QVERIFY(table);
            QCOMPARE(table->item(0, 2)->text(), cut);
            QCOMPARE(table->item(1, 2)->text(), QStringLiteral("Ada Lovelace"));
        }
    }

    // Copy (Cmd+C) in the log copies the current cell. The Message cell
    // has at most util::kMaxLineChars of a message, and an ordinary one
    // with its body shown is often longer: what Copy gave was cut, with
    // a "…". It copies the message whole, with its body or without.
    void copyingFromTheLogTakesTheWholeMessage() {
        // The offscreen platform keeps its clipboard in the process;
        // anywhere else this would overwrite the user's.
        const QString platform = QGuiApplication::platformName();
        if (platform != QLatin1String("offscreen") && platform != QLatin1String("minimal"))
            QSKIP("needs the offscreen platform (ctest sets it): the clipboard is the user's");

        gitbolt::git::CommitData commit;
        commit.id = gitbolt::git::ObjectId::fromHex(std::string(40, '1'));
        commit.summary = "Rework the parser";
        std::string body;
        for (int i = 0; i < 30; ++i)
            body += "Line " + std::to_string(i) + " of the body, which explains the change.\n";
        commit.message = commit.summary + "\n\n" + body;
        commit.author.name = "Ada Lovelace";
        QVERIFY(commit.message.size() > 1000);

        CommitLogModel model;
        model.setCommits({commit});
        model.setShowMessageBody(true);
        gitbolt::widgets::RevisionGraphWidget log;
        log.setModel(&model);
        log.resize(800, 400);
        log.show();
        QVERIFY(QTest::qWaitForWindowExposed(&log));
        auto* table = log.findChild<QTableView*>();
        QVERIFY(table);
        const int message = static_cast<int>(gitbolt::models::CommitLogColumn::Message);
        table->setCurrentIndex(table->model()->index(0, message));
        QVERIFY(table->model()->index(0, message).data().toString().endsWith(kEllipsis));

        QApplication::clipboard()->clear();
        QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(),
                 QString::fromStdString(commit.message).trimmed());

        model.setShowMessageBody(false);
        QApplication::clipboard()->clear();
        QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("Rework the parser"));

        // Any other cell, as it shows.
        table->setCurrentIndex(table->model()->index(
            0, static_cast<int>(gitbolt::models::CommitLogColumn::Author)));
        QApplication::clipboard()->clear();
        QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("Ada Lovelace"));
        table->setCurrentIndex(table->model()->index(
            0, static_cast<int>(gitbolt::models::CommitLogColumn::Hash)));
        QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("1111111"));
        // The graph has no text: the clipboard stays as it is.
        table->setCurrentIndex(table->model()->index(
            0, static_cast<int>(gitbolt::models::CommitLogColumn::Graph)));
        QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("1111111"));
    }

private:
    QTemporaryDir settings_;
};

QTEST_MAIN(TestCommitDetails)
#include "TestCommitDetails.moc"
