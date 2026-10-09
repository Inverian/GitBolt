//
// TestRebaseWidgets — the Rebase dialog's plan list and the repository
// view's in-progress bar, without a repository:
//
//   - the plan lists commits newest first, all picks, each keeping its
//     whole message;
//   - every operation can be chosen for the selected commit, a reword
//     takes its new message from the prompt (and the commit's own
//     message undoes it), and commits move up and down;
//   - planProblem() names a squash or fixup with no kept commit below
//     it, which git would refuse;
//   - the bar shows only while a rebase / merge / cherry-pick / revert
//     is in progress, offers Resolve while files are conflicted, holds
//     Continue / Commit back until they're not, and goes quiet while a
//     step runs.
//

#include <QApplication>
#include <QInputDialog>
#include <QListView>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include "widgets/InteractiveRebaseWidget.h"
#include "widgets/RepoOperationBar.h"

using gitbolt::git::RebaseOperationType;
using gitbolt::widgets::InteractiveRebaseWidget;
using gitbolt::widgets::RepoOperationBar;

namespace {

// Commits "c<n>" … "c1", newest first, as the log hands them over.
std::vector<gitbolt::git::CommitData> commits(int n) {
    std::vector<gitbolt::git::CommitData> list;
    for (int i = n; i >= 1; --i) {
        gitbolt::git::CommitData c;
        c.id = gitbolt::git::ObjectId::fromHex(std::string(40, static_cast<char>('0' + i)));
        c.summary = "c" + std::to_string(i);
        c.message = c.summary + "\n\nbody of c" + std::to_string(i) + "\n";
        list.push_back(c);
    }
    return list;
}

QStringList plan(const InteractiveRebaseWidget& widget) {
    static const char* const names[] = {"pick", "reword", "edit", "squash", "fixup", "drop"};
    QStringList rows;
    for (const auto& op : widget.rebasePlan().operations) {
        QString row = QStringLiteral("%1 %2")
                          .arg(QLatin1String(names[static_cast<int>(op.type)]),
                               QString::fromStdString(op.message).section(QLatin1Char('\n'), 0, 0));
        if (!op.newMessage.empty())
            row += QStringLiteral(" -> ") + QString::fromStdString(op.newMessage).trimmed();
        rows << row;
    }
    return rows;
}

// Answers the reword prompt the next time one opens.
void answerRewordPrompt(const QString& text, bool accept = true) {
    QTimer::singleShot(0, [text, accept] {
        auto* prompt = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        QVERIFY(prompt);
        prompt->setTextValue(text);
        accept ? prompt->accept() : prompt->reject();
    });
}

void select(InteractiveRebaseWidget& widget, int row) {
    auto* list = widget.findChild<QListView*>();
    QVERIFY(list);
    list->setCurrentIndex(list->model()->index(row, 0));
}

QPushButton* button(const QWidget& parent, const QString& text) {
    for (auto* b : parent.findChildren<QPushButton*>())
        if (b->text() == text)
            return b;
    return nullptr;
}

// The bar's button texts, as it translates them.
QString label(const char* text) {
    return RepoOperationBar::tr(text);
}

} // namespace

class TestRebaseWidgets : public QObject {
    Q_OBJECT

private slots:
    void planListsCommitsNewestFirstAsPicks() {
        InteractiveRebaseWidget widget;
        widget.setCommits(commits(3), gitbolt::git::ObjectId());
        QCOMPARE(plan(widget), (QStringList{"pick c3", "pick c2", "pick c1"}));
        // The whole message, for Reword to start from.
        QCOMPARE(widget.rebasePlan().operations.front().message, std::string("c3\n\nbody of c3\n"));
        QVERIFY(widget.planProblem().isEmpty());
    }

    void operationsApplyToTheSelectedCommit() {
        InteractiveRebaseWidget widget;
        widget.setCommits(commits(4), gitbolt::git::ObjectId());
        // The newest commit starts out selected.
        widget.applyToSelected(RebaseOperationType::Squash);
        select(widget, 1);
        widget.applyToSelected(RebaseOperationType::Edit);
        select(widget, 2);
        widget.applyToSelected(RebaseOperationType::Drop);
        QCOMPARE(plan(widget), (QStringList{"squash c4", "edit c3", "drop c2", "pick c1"}));

        // A move takes the commit's operation along, and keeps it
        // selected.
        widget.moveSelected(1);
        QCOMPARE(plan(widget), (QStringList{"squash c4", "edit c3", "pick c1", "drop c2"}));
        widget.moveSelected(1);                 // already at the bottom
        widget.moveSelected(-3);
        QCOMPARE(plan(widget), (QStringList{"drop c2", "squash c4", "edit c3", "pick c1"}));
        widget.moveSelected(-1);                // already at the top
        widget.applyToSelected(RebaseOperationType::Pick);
        QCOMPARE(plan(widget), (QStringList{"pick c2", "squash c4", "edit c3", "pick c1"}));
    }

    void rewordTakesTheMessageFromThePrompt() {
        InteractiveRebaseWidget widget;
        widget.setCommits(commits(2), gitbolt::git::ObjectId());

        answerRewordPrompt(QStringLiteral("new c2\n\nnew body"));
        widget.applyToSelected(RebaseOperationType::Reword);
        QCOMPARE(plan(widget), (QStringList{"reword c2 -> new c2\n\nnew body", "pick c1"}));

        // Cancelled: nothing changes.
        answerRewordPrompt(QStringLiteral("ignored"), false);
        widget.applyToSelected(RebaseOperationType::Reword);
        QCOMPARE(plan(widget).at(0), QStringLiteral("reword c2 -> new c2\n\nnew body"));

        // The commit's own message back: no reword after all.
        answerRewordPrompt(QStringLiteral("c2\n\nbody of c2"));
        widget.applyToSelected(RebaseOperationType::Reword);
        QCOMPARE(plan(widget).at(0), QStringLiteral("pick c2"));
    }

    void squashNeedsAKeptCommitBelow() {
        InteractiveRebaseWidget widget;
        widget.setCommits(commits(3), gitbolt::git::ObjectId());
        select(widget, 2);
        widget.applyToSelected(RebaseOperationType::Fixup);
        QVERIFY(widget.planProblem().contains(QLatin1String("c1")));   // the oldest

        widget.applyToSelected(RebaseOperationType::Pick);
        select(widget, 0);
        widget.applyToSelected(RebaseOperationType::Squash);
        select(widget, 1);
        widget.applyToSelected(RebaseOperationType::Fixup);
        QVERIFY2(widget.planProblem().isEmpty(), qPrintable(widget.planProblem()));

        // With c1 dropped, c2 has nothing left to go into.
        select(widget, 2);
        widget.applyToSelected(RebaseOperationType::Drop);
        QCOMPARE(plan(widget), (QStringList{"squash c3", "fixup c2", "drop c1"}));
        QVERIFY(widget.planProblem().contains(QLatin1String("c2")));
    }

    void barShowsOnlyWhileAnOperationIsInProgress() {
        RepoOperationBar bar;
        QVERIFY(bar.isHidden());
        bar.setState(gitbolt::git::RepoState::Other, 0);
        QVERIFY(bar.isHidden());

        bar.setState(gitbolt::git::RepoState::Rebase, 2);
        QVERIFY(!bar.isHidden());
        QVERIFY(!button(bar, label("Resolve Conflicts…"))->isHidden());
        QVERIFY(!button(bar, label("Continue"))->isEnabled());
        QVERIFY(!button(bar, label("Skip"))->isHidden());
        QVERIFY(button(bar, label("Abort"))->isEnabled());

        bar.setState(gitbolt::git::RepoState::Rebase, 0);
        QVERIFY(button(bar, label("Resolve Conflicts…"))->isHidden());
        QVERIFY(button(bar, label("Continue"))->isEnabled());

        bar.setState(gitbolt::git::RepoState::Merge, 0);
        QVERIFY(button(bar, label("Skip"))->isHidden());
        QVERIFY(!button(bar, label("Continue")));
        QVERIFY(button(bar, label("Commit…"))->isEnabled());

        bar.setState(gitbolt::git::RepoState::None, 0);
        QVERIFY(bar.isHidden());
    }

    void barButtonsAskForTheirStepUntilBusy() {
        RepoOperationBar bar;
        bar.setState(gitbolt::git::RepoState::Rebase, 0);
        QSignalSpy continued(&bar, &RepoOperationBar::continueRequested);
        QSignalSpy skipped(&bar, &RepoOperationBar::skipRequested);
        QSignalSpy aborted(&bar, &RepoOperationBar::abortRequested);
        button(bar, label("Continue"))->click();
        button(bar, label("Skip"))->click();
        button(bar, label("Abort"))->click();
        QCOMPARE(continued.count(), 1);
        QCOMPARE(skipped.count(), 1);
        QCOMPARE(aborted.count(), 1);

        bar.setBusy(true);
        for (auto* b : bar.findChildren<QPushButton*>())
            QVERIFY2(!b->isEnabled(), qPrintable(b->text()));
        // A refresh while the step runs doesn't turn them back on.
        bar.setState(gitbolt::git::RepoState::Rebase, 0);
        QVERIFY(!button(bar, label("Continue"))->isEnabled());
        bar.setBusy(false);
        QVERIFY(button(bar, label("Continue"))->isEnabled());
    }

};

QTEST_MAIN(TestRebaseWidgets)
#include "TestRebaseWidgets.moc"
