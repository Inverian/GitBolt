//
// TestCommitDialog — the Commit dialog against a real repository:
//
//   - Amend is off, with git's reason as its tooltip, while a merge, a
//     cherry-pick or a revert waits for its commit (an amend there
//     flattened the merge, or folded the pick into the commit before
//     it), and on again once the operation is over;
//   - a commit that a hook turns down shows what the hook said, and the
//     dialog stays open with its message.
//
// Settings live in a folder of the test's own (QSettings::setPath), and
// git's global configuration is an empty file there: a developer's
// core.hooksPath or commit.gpgSign stays out of it.
//

#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include "../TestRepoHelper.h"
#include "dialogs/CommitDialog.h"
#include "git/GitProcess.h"
#include "services/GitService.h"

using gitbolt::dialogs::CommitDialog;
using gitbolt::services::GitService;

namespace {

bool succeeded(const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
    return r.ok() && r.value().success();
}

// Trimmed stdout, or "<failed>" when git failed.
QString output(const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
    return succeeded(r) ? QString::fromStdString(r.value().stdoutData).trimmed()
                        : QStringLiteral("<failed>");
}

template <typename T>
T* child(const QWidget& parent, const char* name) {
    return parent.findChild<T*>(QString::fromLatin1(name));
}

} // namespace

class TestCommitDialog : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        const QString global = settingsDir_.filePath(QStringLiteral("gitconfig"));
        QFile empty(global);
        QVERIFY(empty.open(QIODevice::WriteOnly));
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(global));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
    }

    void amendIsOffWhileAnOperationWaitsForItsCommit_data() {
        QTest::addColumn<QString>("operation");
        QTest::newRow("merge") << QStringLiteral("merge");
        QTest::newRow("cherry-pick") << QStringLiteral("cherry-pick");
        QTest::newRow("revert") << QStringLiteral("revert");
    }

    void amendIsOffWhileAnOperationWaitsForItsCommit() {
        QFETCH(QString, operation);
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "base\n", QStringLiteral("base")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        repo.writeFile(QStringLiteral("a.txt"), "side\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "side"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        repo.writeFile(QStringLiteral("a.txt"), "main\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "main"})));

        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        CommitDialog dialog(&svc, nullptr);
        auto* amend = child<QCheckBox>(dialog, "commit.amendCheck");
        QVERIFY(amend);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QVERIFY(amend->isEnabled());
        amend->setChecked(true);

        // git, in a terminal say, stops on the conflict in a.txt.
        const std::string command = operation.toStdString();
        if (operation == QLatin1String("revert"))
            QVERIFY(!succeeded(git.run({"revert", "--no-edit", "side"})));
        else
            QVERIFY(!succeeded(git.run({command, "side"})));
        svc.refreshStatus();
        QTRY_COMPARE(amend->toolTip(),
                     QStringLiteral("You are in the middle of a %1 -- cannot amend.").arg(operation));
        QVERIFY(!amend->isEnabled());
        QVERIFY(!amend->isChecked());

        QVERIFY(succeeded(git.run({command, "--abort"})));
        svc.refreshStatus();
        QTRY_VERIFY(amend->isEnabled());
        QCOMPARE(amend->toolTip(), QString());
    }

    // The commit goes through git, which runs the repository's hooks;
    // a pre-commit's refusal is what the dialog shows.
    void hookRefusalShowsInTheDialog() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("a")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const QString head = output(git.run({"rev-parse", "HEAD"}));
        QDir().mkpath(repo.path() + QStringLiteral("/.git/hooks"));
        QFile hook(repo.path() + QStringLiteral("/.git/hooks/pre-commit"));
        QVERIFY(hook.open(QIODevice::WriteOnly));
        hook.write("#!/bin/sh\necho 'pre-commit: no secrets, please' >&2\nexit 1\n");
        hook.close();
        QVERIFY(hook.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        repo.writeFile(QStringLiteral("b.txt"), "b\n");
        QVERIFY(repo.stageFile(QStringLiteral("b.txt")).ok());

        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        CommitDialog dialog(&svc, nullptr);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* message = child<QPlainTextEdit>(dialog, "commit.message");
        auto* commit = child<QPushButton>(dialog, "commit.commitBtn");
        auto* error = child<QLabel>(dialog, "commit.error");
        QVERIFY(message && commit && error);
        message->setPlainText(QStringLiteral("refused"));
        commit->click();

        QTRY_VERIFY_WITH_TIMEOUT(error->isVisible(), 15000);
        QVERIFY2(error->text().contains(QStringLiteral("pre-commit: no secrets, please")),
                 qPrintable(error->text()));
        QVERIFY(dialog.isVisible());
        QVERIFY(commit->isEnabled());
        QCOMPARE(message->toPlainText(), QStringLiteral("refused"));
        QCOMPARE(output(git.run({"rev-parse", "HEAD"})), head);
    }

private:
    QTemporaryDir settingsDir_;
};

QTEST_MAIN(TestCommitDialog)
#include "TestCommitDialog.moc"
