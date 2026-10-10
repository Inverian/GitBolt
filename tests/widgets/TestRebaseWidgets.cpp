//
// TestRebaseWidgets — the Rebase dialog, its plan list and the
// repository view's in-progress bar, without a repository:
//
//   - the plan lists commits newest first, all picks, each keeping its
//     whole message, and leaves merges out, as git does;
//   - every operation can be chosen for the selected commit, a reword
//     takes its new message from the prompt (and the commit's own
//     message undoes it; an empty one isn't taken), and commits move up
//     and down;
//   - planProblem() names a squash or fixup with no kept commit below
//     it, which git would refuse;
//   - a rebase turned down keeps the dialog open with its plan, and
//     picking the same target again lists its commits again;
//   - the bar shows only while a rebase / merge / cherry-pick / revert
//     is in progress, offers Resolve while files are conflicted, holds
//     Continue / Commit back until they're not, goes quiet while a step
//     runs, and follows a theme switch.
//

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QListView>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "dialogs/RebaseDialog.h"
#include "widgets/InteractiveRebaseWidget.h"
#include "widgets/RepoOperationBar.h"

using gitbolt::dialogs::RebaseDialog;
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
    // The Rebase dialog keeps its size in GitBolt's settings: here, in
    // a folder of the test's own, not the user's.
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    }

    void planListsCommitsNewestFirstAsPicks() {
        InteractiveRebaseWidget widget;
        widget.setCommits(commits(3), gitbolt::git::ObjectId(), gitbolt::git::ObjectId(), {});
        QCOMPARE(plan(widget), (QStringList{"pick c3", "pick c2", "pick c1"}));
        // The whole message, for Reword to start from.
        QCOMPARE(widget.rebasePlan().operations.front().message, std::string("c3\n\nbody of c3\n"));
        QVERIFY(widget.planProblem().isEmpty());
    }

    // git can't pick a merge ("is a merge but no -m option was given")
    // and puts it back at the top of its list each time, so Continue
    // and Skip both failed on it and only Abort got out. The plan
    // leaves merges out, as git's own todo list does. It also keeps the
    // HEAD it was made for, commit and branch.
    void planLeavesMergesOut() {
        auto list = commits(3);
        list[1].parentIds = {list[2].id, gitbolt::git::ObjectId::fromHex(std::string(40, 'a'))};
        QVERIFY(list[1].isMerge());
        InteractiveRebaseWidget widget;
        widget.setCommits(list, gitbolt::git::ObjectId(), list.front().id, "feature");
        QCOMPARE(plan(widget), (QStringList{"pick c3", "pick c1"}));
        QCOMPARE(widget.rebasePlan().head, list.front().id);
        QCOMPARE(widget.rebasePlan().branch, std::string("feature"));
    }

    void operationsApplyToTheSelectedCommit() {
        InteractiveRebaseWidget widget;
        widget.setCommits(commits(4), gitbolt::git::ObjectId(), gitbolt::git::ObjectId(), {});
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
        widget.setCommits(commits(2), gitbolt::git::ObjectId(), gitbolt::git::ObjectId(), {});

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

    // git makes no commit with an empty message; it stops the rebase.
    // The prompt holds OK back while the message is blank, and a blank
    // one changes nothing.
    void rewordWantsAMessage() {
        InteractiveRebaseWidget widget;
        widget.setCommits(commits(1), gitbolt::git::ObjectId(), gitbolt::git::ObjectId(), {});
        bool okWhenBlank = true;
        bool okWithText = false;
        QTimer::singleShot(0, [&] {
            auto* prompt = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            QVERIFY(prompt);
            auto* buttons = prompt->findChild<QDialogButtonBox*>();
            QVERIFY(buttons);
            prompt->setTextValue(QStringLiteral(" \n\n  "));
            okWhenBlank = buttons->button(QDialogButtonBox::Ok)->isEnabled();
            prompt->setTextValue(QStringLiteral("x"));
            okWithText = buttons->button(QDialogButtonBox::Ok)->isEnabled();
            prompt->setTextValue(QStringLiteral(" \n\n  "));
            prompt->accept();
        });
        widget.applyToSelected(RebaseOperationType::Reword);
        QVERIFY(!okWhenBlank);
        QVERIFY(okWithText);
        QCOMPARE(plan(widget), (QStringList{"pick c1"}));
    }

    void squashNeedsAKeptCommitBelow() {
        InteractiveRebaseWidget widget;
        widget.setCommits(commits(3), gitbolt::git::ObjectId(), gitbolt::git::ObjectId(), {});
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

    // A rebaseRequested handler that can't start the rebase (a step
    // still running, HEAD moved since the plan was made) turned it down
    // after the dialog had closed: the reason came with the plan gone.
    // It now says why with the dialog still open, the plan as it was.
    void refusedRebaseKeepsTheDialogAndItsPlan() {
        RebaseDialog dialog;
        dialog.setCommitsToRebase(commits(2), gitbolt::git::ObjectId(), commits(2).front().id,
                                  "feature");
        auto* widget = dialog.findChild<InteractiveRebaseWidget*>();
        QVERIFY(widget);
        widget->applyToSelected(RebaseOperationType::Drop);
        const QStringList edited{"drop c2", "pick c1"};
        QCOMPARE(plan(*widget), edited);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));

        const QString reason = QStringLiteral("feature has moved since the rebase was planned.");
        bool refuse = true;
        int requests = 0;
        connect(&dialog, &RebaseDialog::rebaseRequested, &dialog,
                [&](const gitbolt::git::RebasePlan& requested) {
            ++requests;
            QCOMPARE(requested.branch, std::string("feature"));
            if (refuse)
                dialog.refuse(reason);
        });
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        QVERIFY(buttons);
        QString shown;
        QTimer::singleShot(0, &dialog, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(box);
            shown = box->text();
            box->accept();
        });
        buttons->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(requests, 1);
        QCOMPARE(shown, reason);
        QVERIFY(dialog.isVisible());
        QCOMPARE(plan(*widget), edited);

        // Then it goes, and the dialog closes.
        refuse = false;
        buttons->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(requests, 2);
        QVERIFY(!dialog.isVisible());
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }

    // A refusal for a moved HEAD asks for the target to be picked
    // again; the branch picked already is the likely one. The combo
    // listed commits on an index change only, so picking it again did
    // nothing.
    void pickingTheSameTargetAgainListsItsCommitsAgain() {
        RebaseDialog dialog;
        gitbolt::git::BranchInfo main;
        main.name = "main";
        gitbolt::git::BranchInfo dev;
        dev.name = "dev";
        dialog.setBranches({main, dev});
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* combo = dialog.findChild<QComboBox*>();
        QVERIFY(combo);
        QSignalSpy targets(&dialog, &RebaseDialog::targetRefChanged);

        // A pick from the list: the popup, an entry, Return.
        const auto pick = [combo](int index) {
            combo->showPopup();
            QTRY_VERIFY(combo->view()->isVisible());
            combo->view()->setCurrentIndex(combo->model()->index(index, 0));
            QTest::keyClick(combo->view(), Qt::Key_Return);
            QTRY_VERIFY(!combo->view()->isVisible());
        };
        pick(1);
        QCOMPARE(targets.size(), 1);
        pick(1);
        QCOMPARE(targets.size(), 2);
        QCOMPARE(targets.at(1).at(0).toString(), QStringLiteral("main"));
    }

    // A theme switch sets the application palette. The bar waited for
    // ApplicationPaletteChange in changeEvent(), where Qt never delivers
    // it, and kept the old theme's amber until GitBolt was restarted.
    void barFollowsTheTheme() {
        const QPalette before = QApplication::palette();
        const auto restore = qScopeGuard([&] { QApplication::setPalette(before); });
        // A whole theme's worth, as ThemeService sets them.
        const auto palette = [](const QColor& background, const QColor& text) {
            QPalette p;
            for (const auto role : {QPalette::Window, QPalette::Base, QPalette::Button})
                p.setColor(role, background);
            for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
                p.setColor(role, text);
            return p;
        };
        // In a window on screen, as in the repository view: Qt tells
        // the window, which passes the change down.
        QWidget window;
        auto* bar = new RepoOperationBar(&window);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto barIsDark = [bar] {
            const QPalette p = bar->palette();
            return p.color(QPalette::Window).lightness() < p.color(QPalette::WindowText).lightness();
        };

        QApplication::setPalette(palette(Qt::white, Qt::black));
        QTRY_VERIFY(!barIsDark());
        QApplication::setPalette(palette(QColor(0x20, 0x20, 0x20), Qt::white));
        QTRY_VERIFY(barIsDark());
        QApplication::setPalette(palette(Qt::white, Qt::black));
        QTRY_VERIFY(!barIsDark());
    }

private:
    QTemporaryDir settings_;
};

QTEST_MAIN(TestRebaseWidgets)
#include "TestRebaseWidgets.moc"
