// Tests for git::Config::identity() following the global config while
// GitBolt runs: deleted (libgit2 cached it), created again, edited.

#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include "../TestRepoHelper.h"
#include "git/Config.h"

#include <git2.h>

namespace {

bool writeConfig(const QString& path, const QByteArray& text) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(text) == text.size();
}

QByteArray identityConfig(const char* name, const char* email) {
    return QByteArray("[user]\n\tname = ") + name + "\n\temail = " + email + "\n";
}

} // namespace

class TestConfig : public QObject {
    Q_OBJECT

private slots:
    void identityFollowsTheGlobalFile() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.repo().config().remove("user.name").ok());
        QVERIFY(repo.repo().config().remove("user.email").ok());

        git_buf saved = GIT_BUF_INIT;
        QCOMPARE(git_libgit2_opts(GIT_OPT_GET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, &saved), 0);
        const QByteArray savedPath(saved.ptr ? saved.ptr : "", static_cast<int>(saved.size));
        git_buf_dispose(&saved);
        const auto restore = qScopeGuard([&] {
            git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, savedPath.constData());
        });
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QString global = home.filePath(QStringLiteral(".gitconfig"));
        QVERIFY(writeConfig(global, identityConfig("Ada One", "ada@example.invalid")));
        QCOMPARE(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL,
                                  QFile::encodeName(home.path()).constData()), 0);

        // Opened now, its cached config reads that file, as the window's does.
        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        auto config = opened->config();
        const auto cached = config.getString("user.name");
        QVERIFY(cached.ok());
        QCOMPARE(cached.value(), std::string("Ada One"));
        auto id = config.identity();
        QCOMPARE(id.name.value_or("<unset>"), std::string("Ada One"));
        QCOMPARE(id.email.value_or("<unset>"), std::string("ada@example.invalid"));

        // Deleted.
        QVERIFY(QFile::remove(global));
        id = config.identity();
        QVERIFY(!id.name);
        QVERIFY(!id.email);

        // Created again, with someone else.
        QVERIFY(writeConfig(global, identityConfig("Bea Two", "bea@example.invalid")));
        id = config.identity();
        QCOMPARE(id.name.value_or("<unset>"), std::string("Bea Two"));
        QCOMPARE(id.email.value_or("<unset>"), std::string("bea@example.invalid"));

        // Edited in place, the size kept (a timestamp check alone may miss it).
        QVERIFY(writeConfig(global, identityConfig("Cal Six", "cal@example.invalid")));
        id = config.identity();
        QCOMPARE(id.name.value_or("<unset>"), std::string("Cal Six"));
        QCOMPARE(id.email.value_or("<unset>"), std::string("cal@example.invalid"));
        QCOMPARE(config.userName().value_or("<unset>"), std::string("Cal Six"));
        QCOMPARE(config.userEmail().value_or("<unset>"), std::string("cal@example.invalid"));

        // The repository's own name wins, as in git.
        QVERIFY(config.setString("user.name", "Local Name").ok());
        id = config.identity();
        QCOMPARE(id.name.value_or("<unset>"), std::string("Local Name"));
        QCOMPARE(id.email.value_or("<unset>"), std::string("cal@example.invalid"));
    }

    // A linked worktree reads the repository's config too.
    void identityInALinkedWorktree() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString path = parent.filePath(QStringLiteral("wt"));
        git_worktree* worktree = nullptr;
        git_repository* main = nullptr;
        QCOMPARE(git_repository_open(&main, QFile::encodeName(repo.path()).constData()), 0);
        const auto freeMain = qScopeGuard([&] {
            git_worktree_free(worktree);
            git_repository_free(main);
        });
        QCOMPARE(git_worktree_add(&worktree, main, "wt", QFile::encodeName(path).constData(), nullptr), 0);
        auto opened = gitbolt::git::Repository::open(path.toStdString());
        QVERIFY(opened.ok());
        const auto id = opened->config().identity();
        QCOMPARE(id.name.value_or("<unset>"), std::string("Test User"));
        QCOMPARE(id.email.value_or("<unset>"), std::string("test@gitbolt.local"));
    }
};

QTEST_GUILESS_MAIN(TestConfig)
#include "TestConfig.moc"
