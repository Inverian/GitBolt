// Tests for a repository another user owns (GIT_EOWNER, git's "dubious
// ownership"): its own error code, the folder, the quoted safe.directory
// command, and with the bundled libgit2 a real refusal.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <git2.h>
#include <git2/sys/errors.h>

#include "../TestRepoHelper.h"
#include "git/Error.h"
#include "git/Repository.h"

using gitbolt::git::GitError;
using gitbolt::git::GitErrorCode;
using gitbolt::git::Repository;

#ifdef GITBOLT_LIBGIT2_BUNDLED
// libgit2's test hook (src/util/fs_path.h), in the static library: every
// file is owned by `owner` (a git_fs_path_owner_t) until it is 0 again.
extern "C" void git_fs_path__set_owner(int owner);
#endif

class TestOwnerError : public QObject {
    Q_OBJECT

private slots:
    void ownerKeepsItsCode() {
        git_error_set_str(GIT_ERROR_CONFIG, "repository path '/x/repo' is not owned by current user");
        const GitError error = GitError::fromLibgit2(GIT_EOWNER);
        QCOMPARE(error.code(), GitErrorCode::Owner);
        QCOMPARE(QString::fromStdString(error.message()),
                 QStringLiteral("repository path '/x/repo' is not owned by current user"));
    }

    void safeDirectoryFromTheMessage_data() {
        QTest::addColumn<QString>("message");
        QTest::addColumn<QString>("folder");
        const QString text = QStringLiteral("repository path '%1' is not owned by current user");
        QTest::newRow("work tree") << text.arg(QStringLiteral("/x/first repo"))
                                   << QStringLiteral("/x/first repo");
        // Its work tree is the user's, its .git isn't: safe.directory
        // still names the work tree.
        QTest::newRow(".git folder") << text.arg(QStringLiteral("/x/repo/.git"))
                                     << QStringLiteral("/x/repo");
        QTest::newRow("bare") << text.arg(QStringLiteral("/x/b.git")) << QStringLiteral("/x/b.git");
        QTest::newRow("windows") << text.arg(QStringLiteral("C:/Users/gitbolt/Documents/demo-repo"))
                                 << QStringLiteral("C:/Users/gitbolt/Documents/demo-repo");
        QTest::newRow("quote in a name") << text.arg(QStringLiteral("/x/it's/repo"))
                                         << QStringLiteral("/x/it's/repo");
        // A linked worktree's or submodule's git dir: its work tree is
        // elsewhere, so the caller falls back to the folder it opened.
        QTest::newRow("worktree's git dir")
            << text.arg(QStringLiteral("/x/main/.git/worktrees/wt")) << QString();
        QTest::newRow("submodule's git dir")
            << text.arg(QStringLiteral("/x/main/.git/modules/sub")) << QString();
        QTest::newRow("none") << QStringLiteral("could not find repository") << QString();
        QTest::newRow("empty quotes") << text.arg(QString()) << QString();
    }

    void safeDirectoryFromTheMessage() {
        QFETCH(QString, message);
        QFETCH(QString, folder);
        QCOMPARE(QString::fromStdString(gitbolt::git::ownerSafeDirectory(message.toStdString())),
                 folder);
    }

    void safeDirectoryCommand_data() {
        QTest::addColumn<QString>("folder");
        QTest::addColumn<bool>("windows");
        QTest::addColumn<QString>("argument");
        QTest::newRow("posix") << QStringLiteral("/x/repo") << false << QStringLiteral("/x/repo");
        QTest::newRow("posix, space") << QStringLiteral("/x/first repo") << false
                                      << QStringLiteral("'/x/first repo'");
        QTest::newRow("posix, quote") << QStringLiteral("/x/it's") << false
                                      << QStringLiteral("'/x/it'\\''s'");
        QTest::newRow("posix, dollar") << QStringLiteral("/x/$HOME") << false
                                       << QStringLiteral("'/x/$HOME'");
        QTest::newRow("posix, accents") << QStringLiteral("/x/réseau") << false
                                        << QStringLiteral("/x/réseau");
        QTest::newRow("windows") << QStringLiteral("C:/Users/gitbolt/demo-repo") << true
                                 << QStringLiteral("C:/Users/gitbolt/demo-repo");
        QTest::newRow("windows, space") << QStringLiteral("C:/Users/gitbolt/first repo") << true
                                        << QStringLiteral("\"C:/Users/gitbolt/first repo\"");
        QTest::newRow("windows, ampersand") << QStringLiteral("C:/x/R&D") << true
                                            << QStringLiteral("\"C:/x/R&D\"");
        // Git for Windows takes a leading '/' as its own folder's without
        // %(prefix)/ (libgit2 drops it), and cmd and PowerShell want the
        // parentheses quoted.
        QTest::newRow("windows, share") << QStringLiteral("//server/share/repo") << true
                                        << QStringLiteral("\"%(prefix)///server/share/repo\"");
        QTest::newRow("posix, no prefix") << QStringLiteral("//server/share/repo") << false
                                          << QStringLiteral("//server/share/repo");
    }

    void safeDirectoryCommand() {
        QFETCH(QString, folder);
        QFETCH(bool, windows);
        QFETCH(QString, argument);
        QCOMPARE(QString::fromStdString(
                     gitbolt::git::safeDirectoryCommand(folder.toStdString(), windows)),
                 QStringLiteral("git config --global --add safe.directory ") + argument);
    }

    // A real refusal, and the folder it names is the one to trust.
    void refusedUntilTrusted() {
#ifndef GITBOLT_LIBGIT2_BUNDLED
        QSKIP("needs the bundled libgit2's test hook");
#else
        gitbolt::test::TestRepo repo;
        const QString sub = repo.path() + QStringLiteral("/src/app");
        QVERIFY(QDir().mkpath(sub));
        QTemporaryDir globalDir;
        QVERIFY(globalDir.isValid());
        const QByteArray global = QFile::encodeName(globalDir.path());
        QVERIFY(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL,
                                 global.constData()) == 0);

        constexpr int kOtherUser = 1 << 4;   // GIT_FS_PATH_OWNER_OTHER
        git_fs_path__set_owner(kOtherUser);
        const auto restore = qScopeGuard([] { git_fs_path__set_owner(0); });

        const auto asked = Repository::open(repo.path().toStdString());
        QVERIFY(!asked.ok());
        QCOMPARE(asked.error().code(), GitErrorCode::Owner);

        // From a folder inside it, the folder named is the repository's.
        std::string root;
        const auto inside = Repository::openContaining(sub.toStdString(), {}, root);
        QVERIFY(!inside.ok());
        QCOMPARE(inside.error().code(), GitErrorCode::Owner);
        const std::string folder = gitbolt::git::ownerSafeDirectory(inside.error().message());
        QVERIFY2(!folder.empty(), inside.error().message().c_str());
        QCOMPARE(QFileInfo(QString::fromStdString(folder)).canonicalFilePath(),
                 QFileInfo(repo.path()).canonicalFilePath());

        // What the command adds, in the global config libgit2 reads.
        QFile config(globalDir.filePath(QStringLiteral(".gitconfig")));
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write("[safe]\n\tdirectory = " + QByteArray::fromStdString(folder) + "\n");
        config.close();
        const auto trusted = Repository::openContaining(sub.toStdString(), {}, root);
        QVERIFY2(trusted.ok(),
                 trusted.ok() ? "" : trusted.error().message().c_str());

        // The value Windows' command gives a folder starting with '/'
        // (a share there), as libgit2 reads it.
        const std::string command = gitbolt::git::safeDirectoryCommand(folder, true);
        const std::string flag = "safe.directory ";
        std::string value = command.substr(command.find(flag) + flag.size());
        if (value.size() > 1 && value.front() == '"')
            value = value.substr(1, value.size() - 2);
        if (folder.front() == '/')
            QCOMPARE(QString::fromStdString(value).left(10), QStringLiteral("%(prefix)/"));
        QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Truncate));
        config.write("[safe]\n\tdirectory = " + QByteArray::fromStdString(value) + "\n");
        config.close();
        const auto prefixed = Repository::openContaining(sub.toStdString(), {}, root);
        QVERIFY2(prefixed.ok(),
                 prefixed.ok() ? "" : prefixed.error().message().c_str());
#endif
    }
};

QTEST_MAIN(TestOwnerError)
#include "TestOwnerError.moc"
