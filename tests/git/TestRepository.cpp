#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTest>
#include <QTemporaryDir>
#include <QThread>
#include "../TestRepoHelper.h"
#include "git/GitProcess.h"
#include "git/Repository.h"
#include <git2.h>

#include <algorithm>
#include <cctype>
#include <clocale>
#include <string>
#ifndef Q_OS_WIN
#include <unistd.h>
#else
#include <QProcess>
#endif
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#if defined(Q_OS_MACOS)
#include <mach/mach.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {

// Tag names from tags(), sorted: libgit2's enumeration order is
// loose-then-packed, not alphabetical.
QStringList tagNames(gitbolt::git::Repository& repo) {
    QStringList names;
    auto tags = repo.tags();
    if (!tags.ok())
        return {QStringLiteral("<tags() failed>")};
    for (const auto& t : tags.value())
        names << QString::fromStdString(t.name);
    names.sort();
    return names;
}

bool succeeded(const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
    return r.ok() && r.value().success();
}

// Trimmed stdout, or "<failed>" when git failed.
QString output(const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
    return succeeded(r) ? QString::fromStdString(r.value().stdoutData).trimmed()
                        : QStringLiteral("<failed>");
}

// A work tree file's content, or "<missing>".
QByteArray readFile(const QString& workTree, const QString& relPath) {
    QFile file(QDir(workTree).absoluteFilePath(relPath));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}

// libgit2's status, sorted, a "<path> <what>" line per change:
// "staged" for one in the index, "changed" for one in the work tree,
// "untracked" for a new file. Not git's: git reads the index file,
// and libgit2's copy of the index is what these tests are about.
QStringList statusLines(gitbolt::git::Repository& repo) {
    auto status = repo.status();
    if (!status.ok())
        return {QStringLiteral("<status() failed>")};
    QStringList lines;
    for (const auto& entry : status.value()) {
        const QString path = QString::fromStdString(entry.path);
        if (entry.isStaged())
            lines << path + QStringLiteral(" staged");
        if (entry.isUntracked())
            lines << path + QStringLiteral(" untracked");
        else if (entry.isWorkingTree())
            lines << path + QStringLiteral(" changed");
    }
    lines.sort();
    return lines;
}

// Commits a.txt "main\n" and c.txt "shared\n", then gives `repo` a
// branch "feature" one commit further: a.txt "feature\n", and b.txt,
// which only it has. HEAD stays on the first branch. Returns
// feature's tip, or "" if git failed.
QString addFeatureBranch(gitbolt::test::TestRepo& repo) {
    if (!repo.writeAndCommit("a.txt", "main\n", "a").ok()
        || !repo.writeAndCommit("c.txt", "shared\n", "c").ok())
        return {};
    gitbolt::git::GitProcess git(repo.path().toStdString());
    // The tests compare file bytes, and Git for Windows' system config
    // has core.autocrlf on: the CLI checkouts here and the worktree
    // they add would write "main\r\n" where libgit2 wrote "main\n".
    if (!succeeded(git.run({"config", "core.autocrlf", "false"}))
        || !succeeded(git.run({"checkout", "-q", "-b", "feature"})))
        return {};
    repo.writeFile("a.txt", "feature\n");
    repo.writeFile("b.txt", "feature only\n");
    if (!succeeded(git.run({"add", "-A"}))
        || !succeeded(git.run({"commit", "-q", "-m", "feature"})))
        return {};
    const QString tip = output(git.run({"rev-parse", "HEAD"}));
    if (!succeeded(git.run({"checkout", "-q", "-"})))
        return {};
    return tip;
}

// Appends `text` to the repository's .git/config (a test checks
// QTest::currentTestFailed() after it).
void appendConfig(const QString& repoPath, const QByteArray& text) {
    QFile config(QDir(repoPath).filePath(QStringLiteral(".git/config")));
    QVERIFY(config.open(QIODevice::Append));
    config.write(text);
}

// Writes `data` to `path`, its folders made first.
bool writeFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(data) == data.size();
}

// A repository at `main` with a.txt committed, and a linked worktree
// "wt" of it at `worktree` on a new branch "side": added by git, or by
// libgit2 as GitBolt adds one. False if that failed.
bool addRepoWithWorktree(const QString& main, const QString& worktree, bool byGit = true) {
    gitbolt::git::GitProcess git(main.toStdString());
    if (!QDir().mkpath(main) || !writeFile(main + QStringLiteral("/a.txt"), "a\n")
        || !succeeded(git.run({"init", "-q"}))
        || !succeeded(git.run({"config", "core.autocrlf", "false"}))
        || !succeeded(git.run({"add", "a.txt"}))
        || !succeeded(git.run({"-c", "user.name=T", "-c", "user.email=t@gitbolt.local",
                               "-c", "commit.gpgsign=false", "commit", "-q", "-m", "base"})))
        return false;
    if (byGit)
        return succeeded(git.run({"worktree", "add", "-q", "-b", "side", worktree.toStdString()}));
    auto repo = gitbolt::git::Repository::open(main.toStdString());
    return repo.ok() && repo->addWorktree("wt", worktree.toStdString(), "side", true).ok();
}

#ifdef Q_OS_WIN
// A junction at `link` to the folder `target` (mklink /J), which takes
// no privilege, unlike a symbolic link. False if none was made.
bool makeJunction(const QString& link, const QString& target) {
    QProcess mklink;
    mklink.start(QStringLiteral("cmd"),
                 {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                  QDir::toNativeSeparators(link), QDir::toNativeSeparators(target)});
    return mklink.waitForFinished(30000) && mklink.exitStatus() == QProcess::NormalExit
           && mklink.exitCode() == 0;
}
#endif

#ifndef Q_OS_WIN
// A symbolic link at `link` to `target`, both given as bytes: a name
// that isn't UTF-8 too, where the file system takes one (macOS's
// doesn't).
bool symlinkBytes(const QByteArray& target, const QByteArray& link) {
    return ::symlink(target.constData(), link.constData()) == 0;
}
#endif

#ifndef Q_OS_WIN
// Writes `bytes` to the file `path`, outside any repository (a test
// checks QTest::currentTestFailed() after it).
void writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}
#endif

#if defined(GITBOLT_LIBGIT2_LIMITS_PATCHED)
// The memory the process holds (its resident set), in bytes, or 0
// where that can't be read.
qint64 residentBytes() {
#if defined(Q_OS_MACOS)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return static_cast<qint64>(info.resident_size);
#elif defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS counters;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, static_cast<DWORD>(sizeof(counters))))
        return 0;
    return static_cast<qint64>(counters.WorkingSetSize);
#else
    QFile statm(QStringLiteral("/proc/self/statm"));
    if (!statm.open(QIODevice::ReadOnly))
        return 0;
    const QList<QByteArray> pages = statm.readAll().split(' ');
    return pages.size() > 1 ? pages.at(1).toLongLong() * sysconf(_SC_PAGESIZE) : 0;
#endif
}

// Commits a tree that is `depth` nested single-entry directories named
// "d", with a blob "leaf.txt" at the bottom, and points HEAD at it. The
// objects are a few kilobytes whatever the depth. Returns the commit's
// id, or a zero id (and a test failure) if libgit2 refused a step.
gitbolt::git::ObjectId commitDeepTree(gitbolt::git::Repository& repo, int depth) {
    git_repository* raw = repo.raw();
    git_oid blob;
    if (git_blob_create_from_buffer(&blob, raw, "leaf\n", 5) < 0) {
        QTest::qVerify(false, "git_blob_create_from_buffer", "", __FILE__, __LINE__);
        return {};
    }
    git_oid cur;
    {
        git_treebuilder* tb = nullptr;
        if (git_treebuilder_new(&tb, raw, nullptr) < 0
            || git_treebuilder_insert(nullptr, tb, "leaf.txt", &blob, GIT_FILEMODE_BLOB) < 0
            || git_treebuilder_write(&cur, tb) < 0) {
            QTest::qVerify(false, "innermost tree", "", __FILE__, __LINE__);
            git_treebuilder_free(tb);
            return {};
        }
        git_treebuilder_free(tb);
    }
    for (int i = 0; i < depth; ++i) {
        git_treebuilder* tb = nullptr;
        if (git_treebuilder_new(&tb, raw, nullptr) < 0
            || git_treebuilder_insert(nullptr, tb, "d", &cur, GIT_FILEMODE_TREE) < 0
            || git_treebuilder_write(&cur, tb) < 0) {
            QTest::qVerify(false, "wrapping tree", "", __FILE__, __LINE__);
            git_treebuilder_free(tb);
            return {};
        }
        git_treebuilder_free(tb);
    }
    git_tree* tree = nullptr;
    if (git_tree_lookup(&tree, raw, &cur) < 0) {
        QTest::qVerify(false, "git_tree_lookup", "", __FILE__, __LINE__);
        return {};
    }
    git_signature* sig = nullptr;
    if (git_signature_new(&sig, "T", "t@gitbolt.local", 1700000000, 0) < 0) {
        QTest::qVerify(false, "git_signature_new", "", __FILE__, __LINE__);
        git_tree_free(tree);
        return {};
    }
    git_oid commitOid;
    int err = git_commit_create(&commitOid, raw, "HEAD", sig, sig, nullptr,
                                "deep", tree, 0, nullptr);
    git_signature_free(sig);
    git_tree_free(tree);
    if (err < 0) {
        QTest::qVerify(false, "git_commit_create", "", __FILE__, __LINE__);
        return {};
    }
    return gitbolt::git::ObjectId(&commitOid);
}
#endif

} // namespace

class TestRepository : public QObject {
    Q_OBJECT

private slots:
    void testInit() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        auto result = gitbolt::git::Repository::init(tmpDir.path().toStdString());
        QVERIFY(result.ok());
        QVERIFY(!result->isBare());
        QVERIFY(result->isHeadUnborn());
    }

    void testOpen() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        auto init = gitbolt::git::Repository::init(tmpDir.path().toStdString());
        QVERIFY(init.ok());

        auto open = gitbolt::git::Repository::open(tmpDir.path().toStdString());
        QVERIFY(open.ok());
    }

    void testOpenNonExistent() {
        auto result = gitbolt::git::Repository::open("/nonexistent/path");
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), gitbolt::git::GitErrorCode::NotFound);
    }

    // -----------------------------------------------------------------
    // openContaining(): a folder inside a repository opens that
    // repository, as git finds one from anywhere in its work tree
    // (Finder's Open in GitBolt, a Dock drop or File › Open on a
    // subfolder). `root` says what opened.
    // -----------------------------------------------------------------
    void openContainingKeepsARepositorysOwnPath() {
        gitbolt::test::TestRepo repo;
        std::string root;
        auto result = gitbolt::git::Repository::openContaining(repo.path().toStdString(), "", root);
        QVERIFY(result.ok());
        // As asked for, not as libgit2 resolves it: Recent and the
        // title keep the spelling the user's path had.
        QCOMPARE(QString::fromStdString(root), repo.path());
    }

    void openContainingFindsTheWorkTreeTop() {
        gitbolt::test::TestRepo repo;
        QVERIFY(QDir(repo.path()).mkpath(QStringLiteral("src/app")));
        const QString sub = QDir(repo.path()).filePath(QStringLiteral("src/app"));
        std::string root;
        auto result = gitbolt::git::Repository::openContaining(sub.toStdString(), "", root);
        QVERIFY2(result.ok(), result.ok() ? "" : result.error().message().c_str());
        // Spelled as asked for, though libgit2 resolves the links in
        // it (macOS's temp folder is /var/…, which is /private/var/…).
        QCOMPARE(QString::fromStdString(root), QDir::cleanPath(repo.path()));
        QCOMPARE(QFileInfo(QString::fromStdString(result->workdir())).canonicalFilePath(),
                 QFileInfo(repo.path()).canonicalFilePath());
    }

    // The same through a linked folder: the link's spelling, not its
    // target's, so Recent and the title don't get a second one.
    void openContainingKeepsALinkedFoldersSpelling() {
#ifdef Q_OS_WIN
        QSKIP("QFile::link makes a shortcut on Windows, not a link");
#endif
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString real = base.filePath(QStringLiteral("real/proj"));
        QVERIFY(gitbolt::git::Repository::init(real.toStdString()).ok());
        QVERIFY(QDir(real).mkpath(QStringLiteral("src")));
        QVERIFY(QFile::link(base.filePath(QStringLiteral("real")), base.filePath(QStringLiteral("link"))));
        std::string root;
        auto result = gitbolt::git::Repository::openContaining(
            base.filePath(QStringLiteral("link/proj/src")).toStdString(), "", root);
        QVERIFY(result.ok());
        QCOMPARE(QString::fromStdString(root), base.filePath(QStringLiteral("link/proj")));
    }

    // A repository inside another one (or a submodule): the innermost.
    void openContainingFindsTheInnermostRepository() {
        gitbolt::test::TestRepo outer;
        const QString inner = QDir(outer.path()).filePath(QStringLiteral("inner"));
        QVERIFY(gitbolt::git::Repository::init(inner.toStdString()).ok());
        QVERIFY(QDir(inner).mkpath(QStringLiteral("sub")));
        std::string root;
        auto result = gitbolt::git::Repository::openContaining(
            QDir(inner).filePath(QStringLiteral("sub")).toStdString(), "", root);
        QVERIFY(result.ok());
        QCOMPARE(QFileInfo(QString::fromStdString(root)).canonicalFilePath(),
                 QFileInfo(inner).canonicalFilePath());
    }

    void openContainingFindsALinkedWorktree() {
        gitbolt::test::TestRepo repo;
        QVERIFY(!addFeatureBranch(repo).isEmpty());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QTemporaryDir worktreeParent;
        QVERIFY(worktreeParent.isValid());
        const QString worktree = worktreeParent.filePath(QStringLiteral("wt"));
        QVERIFY(succeeded(git.run({"worktree", "add", "-q", worktree.toStdString(), "feature"})));
        QVERIFY(QDir(worktree).mkpath(QStringLiteral("sub")));
        std::string root;
        auto result = gitbolt::git::Repository::openContaining(
            QDir(worktree).filePath(QStringLiteral("sub")).toStdString(), "", root);
        QVERIFY(result.ok());
        QCOMPARE(QFileInfo(QString::fromStdString(root)).canonicalFilePath(),
                 QFileInfo(worktree).canonicalFilePath());
        QCOMPARE(QString::fromStdString(result->headBranchName().value()), QStringLiteral("feature"));
    }

    // A bare repository has no work tree: its git dir is what opens.
    void openContainingFindsABareRepository() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString bare = tmp.filePath(QStringLiteral("repo.git"));
        QVERIFY(gitbolt::git::Repository::init(bare.toStdString(), true).ok());
        std::string root;
        auto result = gitbolt::git::Repository::openContaining(
            QDir(bare).filePath(QStringLiteral("refs")).toStdString(), "", root);
        QVERIFY(result.ok());
        QVERIFY(result->isBare());
        const QString opened = QString::fromStdString(root);
        QVERIFY2(!opened.endsWith(QLatin1Char('/')), qPrintable(opened));
        QCOMPARE(QFileInfo(opened).canonicalFilePath(), QFileInfo(bare).canonicalFilePath());
    }

    // The ceiling ($HOME, in the app) is never searched: a dotfiles
    // repository there doesn't claim a folder below it that is in no
    // repository of its own. It still opens itself, and the search
    // stops there however the ceiling is spelled (macOS's /var is
    // /private/var).
    void openContainingStopsBelowTheCeiling_data() {
        QTest::addColumn<bool>("canonicalCeiling");
        QTest::newRow("ceiling as given") << false;
#ifndef Q_OS_WIN
        // Not on Windows, where the runners' %TEMP% is an 8.3 short
        // name that the canonical path spells out and libgit2 (like
        // git) compares as it is.
        QTest::newRow("ceiling canonical") << true;
#endif
    }

    void openContainingStopsBelowTheCeiling() {
        QFETCH(bool, canonicalCeiling);
        QTemporaryDir home;
        QVERIFY(home.isValid());
        QVERIFY(gitbolt::git::Repository::init(home.path().toStdString()).ok());
        QVERIFY(QDir(home.path()).mkpath(QStringLiteral("projects/plain")));
        const QString ceiling = canonicalCeiling ? QFileInfo(home.path()).canonicalFilePath()
                                                 : home.path();

        std::string root;
        auto below = gitbolt::git::Repository::openContaining(
            QDir(home.path()).filePath(QStringLiteral("projects/plain")).toStdString(),
            ceiling.toStdString(), root);
        QVERIFY(!below.ok());
        QCOMPARE(below.error().code(), gitbolt::git::GitErrorCode::NotFound);

        auto itself = gitbolt::git::Repository::openContaining(
            home.path().toStdString(), ceiling.toStdString(), root);
        QVERIFY(itself.ok());
        QCOMPARE(QString::fromStdString(root), home.path());
    }

    // The ceiling itself, or a folder above it, is no folder below it:
    // it opens if it is a repository, and nothing is searched for.
    // (libgit2 alone would search on up from the ceiling, into the
    // repository around it.)
    void openContainingNeverSearchesFromTheCeilingUp() {
        gitbolt::test::TestRepo outer;
        QVERIFY(QDir(outer.path()).mkpath(QStringLiteral("home/user")));
        const QString home = QDir(outer.path()).filePath(QStringLiteral("home/user"));
        std::string root;
        auto itself = gitbolt::git::Repository::openContaining(
            home.toStdString(), home.toStdString(), root);
        QVERIFY(!itself.ok());
        QCOMPARE(itself.error().code(), gitbolt::git::GitErrorCode::NotFound);
        auto above = gitbolt::git::Repository::openContaining(
            QDir(outer.path()).filePath(QStringLiteral("home")).toStdString(),
            home.toStdString(), root);
        QVERIFY(!above.ok());
        QCOMPARE(above.error().code(), gitbolt::git::GitErrorCode::NotFound);
        // A folder beside it is searched as any other.
        QVERIFY(QDir(outer.path()).mkpath(QStringLiteral("src")));
        auto beside = gitbolt::git::Repository::openContaining(
            QDir(outer.path()).filePath(QStringLiteral("src")).toStdString(),
            home.toStdString(), root);
        QVERIFY(beside.ok());
    }

#ifdef Q_OS_WIN
    // On Windows libgit2 compares a folder with the ceiling as they are
    // written, following no junction (nor subst drive), while the
    // system resolves them. A folder at or above the ceiling either way
    // is not searched from: the folder holding a home folder that is a
    // junction elsewhere (libgit2 went on up from it, into the
    // repository around it), and the home folder reached through a
    // junction (libgit2 took it for another folder, and searched on up
    // past the ceiling).
    void openContainingNeverSearchesFromTheCeilingUpThroughAJunction_data() {
        QTest::addColumn<bool>("ceilingIsTheJunction");
        QTest::newRow("the ceiling a junction elsewhere") << true;
        QTest::newRow("the ceiling reached through a junction") << false;
    }

    void openContainingNeverSearchesFromTheCeilingUpThroughAJunction() {
        QFETCH(bool, ceilingIsTheJunction);
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString outer = base.filePath(QStringLiteral("outer"));
        QVERIFY(QDir().mkpath(outer));
        QVERIFY(gitbolt::git::Repository::init(outer.toStdString()).ok());
        QString junction;
        QString start;
        QString ceiling;
        if (ceilingIsTheJunction) {
            // outer/users/me is the home folder elsewhere/me.
            const QString elsewhere = base.filePath(QStringLiteral("elsewhere/me"));
            QVERIFY(QDir().mkpath(elsewhere));
            QVERIFY(QDir(outer).mkpath(QStringLiteral("users")));
            junction = outer + QStringLiteral("/users/me");
            start = outer + QStringLiteral("/users");
            ceiling = junction;
            if (!makeJunction(junction, elsewhere))
                QSKIP("mklink /J made no junction here");
        } else {
            // j is outer: j/me is the home folder outer/me.
            QVERIFY(QDir(outer).mkpath(QStringLiteral("me")));
            junction = base.filePath(QStringLiteral("j"));
            start = junction + QStringLiteral("/me");
            ceiling = outer + QStringLiteral("/me");
            if (!makeJunction(junction, outer))
                QSKIP("mklink /J made no junction here");
        }
        // Before `base` goes: the junction alone, not what it leads to.
        const auto undo = qScopeGuard([&] { QDir().rmdir(junction); });

        std::string root = "untouched";
        auto result = gitbolt::git::Repository::openContaining(
            start.toStdString(), ceiling.toStdString(), root);
        QVERIFY2(!result.ok(), root.c_str());
        QCOMPARE(result.error().code(), gitbolt::git::GitErrorCode::NotFound);
        QCOMPARE(root, std::string("untouched"));
    }
#endif

    // A repository found broken is not "no repository": a linked
    // worktree whose main repository was moved away. libgit2 reports
    // it as not found too, and the window would say the folder is in
    // no repository instead of what is wrong with it.
    void openContainingReportsABrokenWorktree() {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString main = base.filePath(QStringLiteral("main"));
        const QString worktree = base.filePath(QStringLiteral("wt"));
        QVERIFY(QDir().mkpath(main));
        gitbolt::git::GitProcess git(main.toStdString());
        QVERIFY(succeeded(git.run({"init", "-q"})));
        QVERIFY(succeeded(git.run({"-c", "user.name=T", "-c", "user.email=t@gitbolt.local",
                                   "-c", "commit.gpgsign=false",
                                   "commit", "-q", "--allow-empty", "-m", "base"})));
        QVERIFY(succeeded(git.run({"worktree", "add", "-q", "-b", "topic", worktree.toStdString()})));
        QVERIFY(QDir(worktree).mkpath(QStringLiteral("sub")));
        QVERIFY(QDir().rename(main, base.filePath(QStringLiteral("moved"))));

        for (const QString& path : {worktree, QDir(worktree).filePath(QStringLiteral("sub"))}) {
            std::string root;
            auto result = gitbolt::git::Repository::openContaining(path.toStdString(), "", root);
            QVERIFY(!result.ok());
            QVERIFY2(result.error().code() != gitbolt::git::GitErrorCode::NotFound,
                     qPrintable(path));
            QVERIFY2(!result.error().message().empty(), qPrintable(path));
        }
        // Gone altogether, a folder is still not found.
        std::string root;
        auto gone = gitbolt::git::Repository::openContaining(
            base.filePath(QStringLiteral("nowhere")).toStdString(), "", root);
        QVERIFY(!gone.ok());
        QCOMPARE(gone.error().code(), gitbolt::git::GitErrorCode::NotFound);
    }

    void openContainingFailsOutsideAnyRepository() {
        QTemporaryDir plain;
        QVERIFY(plain.isValid());
        std::string root = "untouched";
        auto result = gitbolt::git::Repository::openContaining(
            plain.path().toStdString(), QDir::tempPath().toStdString(), root);
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), gitbolt::git::GitErrorCode::NotFound);
        QCOMPARE(root, std::string("untouched"));
    }

    // -----------------------------------------------------------------
    // tags(): `name` is the short form createTag/deleteTag take and
    // the UI shows; `fullRefName` is the ref. git_tag_foreach reports
    // the full ref, and storing that as `name` broke Delete tag.
    // -----------------------------------------------------------------
    void tagsReportShortAndFullNames() {
        gitbolt::test::TestRepo repo;
        auto head = repo.writeAndCommit("a.txt", "a\n", "initial");
        QVERIFY(head.ok());
        QVERIFY(repo.repo().createLightweightTag("v0.1", head.value()).ok());
        QVERIFY(repo.repo().createTag("v1.0", head.value(), "release").ok());
        // Slashes are part of the short name, not a prefix to strip.
        QVERIFY(repo.repo().createLightweightTag("release/2.0", head.value()).ok());

        auto tags = repo.repo().tags();
        QVERIFY(tags.ok());
        QCOMPARE(tags->size(), size_t(3));
        for (const auto& t : tags.value()) {
            QCOMPARE(QString::fromStdString(t.fullRefName),
                     QStringLiteral("refs/tags/") + QString::fromStdString(t.name));
        }
        QCOMPARE(tagNames(repo.repo()),
                 (QStringList{"release/2.0", "v0.1", "v1.0"}));

        // A lightweight tag's ref points straight at the commit.
        auto light = std::find_if(tags->begin(), tags->end(),
                                  [](const auto& t) { return t.name == "v0.1"; });
        QVERIFY(light != tags->end());
        QCOMPARE(light->targetId, head.value());
    }

    // -----------------------------------------------------------------
    // An annotated tag's ref points at the tag object, not the commit.
    // tags() used to report that object's id as `targetId` and every
    // tag as lightweight, so the sidebar tooltip showed a hash that
    // isn't in the log and never the tag's message.
    // -----------------------------------------------------------------
    void tagsReportAnnotatedDetails() {
        gitbolt::test::TestRepo repo;
        auto tagged = repo.writeAndCommit("a.txt", "a\n", "tagged");
        QVERIFY(tagged.ok());
        QVERIFY(repo.repo().createTag("v1.0", tagged.value(), "release notes").ok());
        QVERIFY(repo.repo().createLightweightTag("v0.1", tagged.value()).ok());
        // HEAD moves on, so a tag can't pass by pointing at HEAD.
        QVERIFY(repo.writeAndCommit("b.txt", "b\n", "later").ok());
        auto tagObject = repo.repo().resolveRef("refs/tags/v1.0");
        QVERIFY(tagObject.ok());
        QVERIFY(tagObject.value() != tagged.value());

        auto tags = repo.repo().tags();
        QVERIFY(tags.ok());
        QCOMPARE(tags->size(), size_t(2));
        auto byName = [&](const std::string& name) {
            return std::find_if(tags->begin(), tags->end(),
                                [&](const auto& t) { return t.name == name; });
        };

        auto annotated = byName("v1.0");
        QVERIFY(annotated != tags->end());
        QCOMPARE(annotated->type, gitbolt::git::TagType::Annotated);
        QCOMPARE(annotated->targetId, tagged.value());
        QCOMPARE(annotated->tagId, tagObject.value());
        QCOMPARE(QString::fromStdString(annotated->message).trimmed(),
                 QStringLiteral("release notes"));
        QCOMPARE(annotated->tagger.name, std::string("Test User"));
        QCOMPARE(annotated->tagger.email, std::string("test@gitbolt.local"));

        auto light = byName("v0.1");
        QVERIFY(light != tags->end());
        QCOMPARE(light->type, gitbolt::git::TagType::Lightweight);
        QCOMPARE(light->targetId, tagged.value());
        QVERIFY(light->tagId.isZero());
        QVERIFY(light->message.empty());
        QVERIFY(light->tagger.name.empty());
    }

    // -----------------------------------------------------------------
    // A tag has no signature header: `git tag -s` appends the armored
    // block to the message, and libgit2 returns that body whole, so
    // the sidebar tooltip showed the signature. createTag writes the
    // message verbatim, so these are the bytes a signed tag carries,
    // with no gpg or ssh-keygen needed.
    // -----------------------------------------------------------------
    void tagsDropTheSignatureBlock_data() {
        QTest::addColumn<QString>("message");
        QTest::addColumn<QString>("expected");
        QTest::newRow("pgp")
            << QStringLiteral("release notes\n"
                              "-----BEGIN PGP SIGNATURE-----\n\n"
                              "iQEzBAABCAAdFiEEabcdefghijklmnop\n=Ab12\n"
                              "-----END PGP SIGNATURE-----\n")
            << QStringLiteral("release notes\n");
        QTest::newRow("ssh")
            << QStringLiteral("Release v1\n\nSecond paragraph.\n"
                              "-----BEGIN SSH SIGNATURE-----\n"
                              "U1NIU0lHAAAAAQAAADMAAAALc3NoLWVkMjU1MTk=\n"
                              "-----END SSH SIGNATURE-----\n")
            << QStringLiteral("Release v1\n\nSecond paragraph.\n");
        QTest::newRow("x509")
            << QStringLiteral("release notes\n"
                              "-----BEGIN SIGNED MESSAGE-----\n"
                              "MIAGCSqGSIb3DQEHAqCAMIACAQExDTALBglghkgBZQME\n"
                              "-----END SIGNED MESSAGE-----\n")
            << QStringLiteral("release notes\n");
        QTest::newRow("signature only")
            << QStringLiteral("-----BEGIN PGP SIGNATURE-----\n\n"
                              "iQEzBAABCAAdFiEE\n-----END PGP SIGNATURE-----\n")
            << QString();
        // Only a line that STARTS with a marker begins a signature.
        QTest::newRow("marker mid-line")
            << QStringLiteral("Paste it after -----BEGIN PGP SIGNATURE----- here\n")
            << QStringLiteral("Paste it after -----BEGIN PGP SIGNATURE----- here\n");
        QTest::newRow("unsigned")
            << QStringLiteral("release notes\n")
            << QStringLiteral("release notes\n");
    }

    void tagsDropTheSignatureBlock() {
        QFETCH(QString, message);
        QFETCH(QString, expected);
        gitbolt::test::TestRepo repo;
        auto head = repo.writeAndCommit("a.txt", "a\n", "initial");
        QVERIFY(head.ok());
        QVERIFY(repo.repo().createTag("v1.0", head.value(), message.toStdString()).ok());

        auto tags = repo.repo().tags();
        QVERIFY(tags.ok());
        QCOMPARE(tags->size(), size_t(1));
        QCOMPARE(QString::fromStdString(tags->front().message), expected);
    }

    // -----------------------------------------------------------------
    // deleteTag accepts the short name and the full ref name, for
    // both tag kinds, and leaves every other tag alone.
    // -----------------------------------------------------------------
    void deleteTag_data() {
        QTest::addColumn<bool>("annotated");
        QTest::addColumn<QString>("spec");

        QTest::newRow("lightweight, short name") << false << "v1.0";
        QTest::newRow("lightweight, full ref")   << false << "refs/tags/v1.0";
        QTest::newRow("annotated, short name")   << true  << "v1.0";
        QTest::newRow("annotated, full ref")     << true  << "refs/tags/v1.0";
    }

    void deleteTag() {
        QFETCH(bool, annotated);
        QFETCH(QString, spec);

        gitbolt::test::TestRepo repo;
        auto head = repo.writeAndCommit("a.txt", "a\n", "initial");
        QVERIFY(head.ok());
        auto created = annotated
            ? repo.repo().createTag("v1.0", head.value(), "release")
            : repo.repo().createLightweightTag("v1.0", head.value());
        QVERIFY(created.ok());
        QVERIFY(repo.repo().createLightweightTag("keep", head.value()).ok());
        QCOMPARE(tagNames(repo.repo()), (QStringList{"keep", "v1.0"}));

        auto result = repo.repo().deleteTag(spec.toStdString());
        QVERIFY2(result.ok(), result.error().message().c_str());
        QCOMPARE(tagNames(repo.repo()), QStringList{"keep"});
        QVERIFY(!repo.repo().resolveRef("refs/tags/v1.0").ok());
    }

    // -----------------------------------------------------------------
    // Deleting a tag that doesn't exist is a NotFound error, not a
    // crash or a silent success, and touches nothing else.
    // -----------------------------------------------------------------
    void deleteMissingTagFailsCleanly() {
        gitbolt::test::TestRepo repo;
        auto head = repo.writeAndCommit("a.txt", "a\n", "initial");
        QVERIFY(head.ok());
        QVERIFY(repo.repo().createLightweightTag("keep", head.value()).ok());

        for (const char* spec : {"nope", "refs/tags/nope"}) {
            auto result = repo.repo().deleteTag(spec);
            QVERIFY2(!result.ok(), spec);
            QCOMPARE(result.error().code(), gitbolt::git::GitErrorCode::NotFound);
            QVERIFY(!result.error().message().empty());
        }
        QCOMPARE(tagNames(repo.repo()), QStringList{"keep"});
    }

    // -----------------------------------------------------------------
    // A tag and a local branch may share a name; deleting the tag
    // must leave the branch alone.
    // -----------------------------------------------------------------
    void deleteTagSparesSameNamedBranch() {
        gitbolt::test::TestRepo repo;
        auto head = repo.writeAndCommit("a.txt", "a\n", "initial");
        QVERIFY(head.ok());
        QVERIFY(repo.repo().createLightweightTag("v1", head.value()).ok());
        QVERIFY(repo.repo().createBranch("v1", head.value()).ok());

        QVERIFY(repo.repo().deleteTag("v1").ok());
        QVERIFY(tagNames(repo.repo()).isEmpty());
        QVERIFY(repo.repo().resolveRef("refs/heads/v1").ok());
    }

    // -----------------------------------------------------------------
    // checkout of a name that is both a local branch and a tag follows
    // git's `git checkout <name>`: the branch wins, for HEAD AND for
    // the files. It used to attach HEAD to the branch but check out
    // the tag's tree, leaving phantom changes. Full refs check out
    // what they name; like git, only a bare branch name attaches HEAD.
    // -----------------------------------------------------------------
    void checkoutNameSharedByBranchAndTag_data() {
        QTest::addColumn<bool>("annotated");
        QTest::addColumn<QString>("spec");
        QTest::addColumn<QString>("expectedBranch");   // empty: detached
        QTest::addColumn<QString>("expectedContent");

        for (const bool annotated : {true, false}) {
            const char* kind = annotated ? "annotated" : "lightweight";
            QTest::addRow("%s tag, bare name", kind)
                << annotated << "v1" << "v1" << "on branch\n";
            QTest::addRow("%s tag, refs/tags/", kind)
                << annotated << "refs/tags/v1" << "" << "at tag\n";
            QTest::addRow("%s tag, refs/heads/", kind)
                << annotated << "refs/heads/v1" << "" << "on branch\n";
            QTest::addRow("%s tag, no such branch", kind)
                << annotated << "only-tag" << "" << "at tag\n";
        }
    }

    void checkoutNameSharedByBranchAndTag() {
        QFETCH(bool, annotated);
        QFETCH(QString, spec);
        QFETCH(QString, expectedBranch);
        QFETCH(QString, expectedContent);

        gitbolt::test::TestRepo repo;
        auto atTag = repo.writeAndCommit("a.txt", "at tag\n", "tagged");
        QVERIFY(atTag.ok());
        for (const char* name : {"v1", "only-tag"}) {
            auto created = annotated
                ? repo.repo().createTag(name, atTag.value(), "release")
                : repo.repo().createLightweightTag(name, atTag.value());
            QVERIFY(created.ok());
        }
        auto onBranch = repo.writeAndCommit("a.txt", "on branch\n", "branch tip");
        QVERIFY(onBranch.ok());
        QVERIFY(repo.repo().createBranch("v1", onBranch.value()).ok());
        // HEAD's own branch moves on, so neither target is current.
        QVERIFY(repo.writeAndCommit("a.txt", "on main\n", "main tip").ok());

        auto result = repo.repo().checkout(spec.toStdString());
        QVERIFY2(result.ok(), result.ok() ? "" : result.error().message().c_str());

        if (expectedBranch.isEmpty()) {
            QVERIFY(repo.repo().isHeadDetached());
        } else {
            QVERIFY(!repo.repo().isHeadDetached());
            auto branch = repo.repo().headBranchName();
            QVERIFY(branch.ok());
            QCOMPARE(QString::fromStdString(branch.value()), expectedBranch);
        }
        auto head = repo.repo().head();
        QVERIFY(head.ok());
        QCOMPARE(head.value(), expectedContent == QStringLiteral("at tag\n")
                                   ? atTag.value() : onBranch.value());

        // The files match HEAD: nothing staged, nothing modified.
        QFile file(QDir(repo.path()).absoluteFilePath(QStringLiteral("a.txt")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(file.readAll()), expectedContent);
        auto status = repo.repo().status();
        QVERIFY(status.ok());
        QCOMPARE(status->size(), size_t(0));
    }

    // -----------------------------------------------------------------
    // git won't check out a branch that another worktree has checked
    // out. libgit2's set_head refused it too, but only after the
    // branch's files and index had been written: HEAD stayed on the
    // old branch, with the switch staged on it. From a detached HEAD
    // it didn't refuse at all. Now nothing is touched, and the error
    // says where the branch is checked out.
    // -----------------------------------------------------------------
    void checkoutRefusesABranchAnotherWorktreeHas_data() {
        QTest::addColumn<bool>("detached");
        QTest::newRow("from a branch") << false;
        QTest::newRow("from a detached HEAD") << true;
    }

    void checkoutRefusesABranchAnotherWorktreeHas() {
        QFETCH(bool, detached);
        gitbolt::test::TestRepo repo;
        QVERIFY(!addFeatureBranch(repo).isEmpty());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QTemporaryDir worktreeParent;
        QVERIFY(worktreeParent.isValid());
        const QString worktree = worktreeParent.filePath(QStringLiteral("wt"));
        QVERIFY(succeeded(git.run({"worktree", "add", "-q", worktree.toStdString(), "feature"})));
        if (detached)
            QVERIFY(succeeded(git.run({"checkout", "-q", "--detach"})));
        const QString headRef = output(git.run({"rev-parse", "--symbolic-full-name", "HEAD"}));
        const QString head = output(git.run({"rev-parse", "HEAD"}));

        auto result = repo.repo().checkout("feature");
        QVERIFY(!result.ok());

        // Nothing moved: not HEAD, not a file, not the index.
        QCOMPARE(output(git.run({"rev-parse", "--symbolic-full-name", "HEAD"})), headRef);
        QCOMPARE(output(git.run({"rev-parse", "HEAD"})), head);
        QCOMPARE(readFile(repo.path(), QStringLiteral("a.txt")), QByteArray("main\n"));
        QCOMPARE(readFile(repo.path(), QStringLiteral("b.txt")), QByteArray("<missing>"));
        QCOMPARE(statusLines(repo.repo()), QStringList());

        const QString message = QString::fromStdString(result.error().message());
        const auto at = QRegularExpression(
            QStringLiteral("^'feature' is already checked out at '(.+)'$")).match(message);
        QVERIFY2(at.hasMatch(), qPrintable(message));
        QCOMPARE(QFileInfo(at.captured(1)).canonicalFilePath(),
                 QFileInfo(worktree).canonicalFilePath());
    }

    // The same seen from a linked worktree: the main worktree's branch.
    void checkoutInAWorktreeRefusesTheMainWorktreesBranch() {
        gitbolt::test::TestRepo repo;
        QVERIFY(!addFeatureBranch(repo).isEmpty());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const QString mainBranch = output(git.run({"branch", "--show-current"}));
        QTemporaryDir worktreeParent;
        QVERIFY(worktreeParent.isValid());
        const QString worktree = worktreeParent.filePath(QStringLiteral("wt"));
        QVERIFY(succeeded(git.run({"worktree", "add", "-q", worktree.toStdString(), "feature"})));
        auto linked = gitbolt::git::Repository::open(worktree.toStdString());
        QVERIFY(linked.ok());

        auto result = linked->checkout(mainBranch.toStdString());
        QVERIFY(!result.ok());
        auto branch = linked->headBranchName();
        QVERIFY(branch.ok());
        QCOMPARE(branch.value(), std::string("feature"));
        QCOMPARE(readFile(worktree, QStringLiteral("a.txt")), QByteArray("feature\n"));
        QCOMPARE(statusLines(linked.value()), QStringList());

        const QString message = QString::fromStdString(result.error().message());
        const auto at = QRegularExpression(
            QStringLiteral("^'%1' is already checked out at '(.+)'$").arg(mainBranch)).match(message);
        QVERIFY2(at.hasMatch(), qPrintable(message));
        QCOMPARE(QFileInfo(at.captured(1)).canonicalFilePath(),
                 QFileInfo(repo.path()).canonicalFilePath());
    }

    // -----------------------------------------------------------------
    // HEAD can't always move: another git may hold its lock. The files
    // and the index were the target's by then, and stayed so: the
    // switch showed up as staged changes on the branch HEAD was still
    // on. They are put back now, and the user's own changes are kept.
    // -----------------------------------------------------------------
    void checkoutPutsTheFilesBackWhenHeadCannotMove_data() {
        QTest::addColumn<bool>("branch");
        QTest::newRow("a branch") << true;
        QTest::newRow("a commit") << false;
    }

    void checkoutPutsTheFilesBackWhenHeadCannotMove() {
        QFETCH(bool, branch);
        gitbolt::test::TestRepo repo;
        const QString featureTip = addFeatureBranch(repo);
        QVERIFY(!featureTip.isEmpty());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const QString headRef = output(git.run({"rev-parse", "--symbolic-full-name", "HEAD"}));
        repo.writeFile("c.txt", "the user's change\n");
        repo.writeFile("notes.txt", "untracked\n");
        QFile lock(QDir(repo.path()).filePath(QStringLiteral(".git/HEAD.lock")));
        QVERIFY(lock.open(QIODevice::WriteOnly));
        lock.close();

        const std::string spec = branch ? "feature" : featureTip.toStdString();
        auto result = repo.repo().checkout(spec);
        QVERIFY(!result.ok());
        QCOMPARE(output(git.run({"rev-parse", "--symbolic-full-name", "HEAD"})), headRef);
        QCOMPARE(readFile(repo.path(), QStringLiteral("a.txt")), QByteArray("main\n"));
        QCOMPARE(readFile(repo.path(), QStringLiteral("b.txt")), QByteArray("<missing>"));
        QCOMPARE(readFile(repo.path(), QStringLiteral("c.txt")), QByteArray("the user's change\n"));
        const QStringList userChanges{QStringLiteral("c.txt changed"),
                                      QStringLiteral("notes.txt untracked")};
        QCOMPARE(statusLines(repo.repo()), userChanges);

        // Without the lock the same checkout goes through, taking the
        // user's changes along.
        QVERIFY(lock.remove());
        result = repo.repo().checkout(spec);
        QVERIFY2(result.ok(), result.ok() ? "" : result.error().message().c_str());
        QCOMPARE(readFile(repo.path(), QStringLiteral("a.txt")), QByteArray("feature\n"));
        QCOMPARE(readFile(repo.path(), QStringLiteral("c.txt")), QByteArray("the user's change\n"));
        QCOMPARE(statusLines(repo.repo()), userChanges);
    }

    // -----------------------------------------------------------------
    // A checkout that fails partway (here another git holds index.lock,
    // so the index can't be written at the end) has written files, and
    // libgit2's copy of the index had them while the index file didn't.
    // Status listed them as staged, and the next commit took them
    // along. They are changes in the work tree, as git leaves them.
    // -----------------------------------------------------------------
    void failedCheckoutLeavesNothingStaged() {
        gitbolt::test::TestRepo repo;
        QVERIFY(!addFeatureBranch(repo).isEmpty());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const QString head = output(git.run({"rev-parse", "HEAD"}));
        QFile lock(QDir(repo.path()).filePath(QStringLiteral(".git/index.lock")));
        QVERIFY(lock.open(QIODevice::WriteOnly));
        lock.close();

        QVERIFY(!repo.repo().checkout("feature").ok());
        QVERIFY(lock.remove());
        QCOMPARE(output(git.run({"rev-parse", "HEAD"})), head);
        QCOMPARE(readFile(repo.path(), QStringLiteral("a.txt")), QByteArray("feature\n"));
        QCOMPARE(statusLines(repo.repo()),
                 (QStringList{QStringLiteral("a.txt changed"), QStringLiteral("b.txt untracked")}));

        repo.writeFile("d.txt", "d\n");
        QVERIFY(repo.stageFile("d.txt").ok());
        QVERIFY(repo.commit("d").ok());
        QCOMPARE(output(git.run({"diff", "--name-only", head.toStdString(), "HEAD"})),
                 QStringLiteral("d.txt"));
    }

    // -----------------------------------------------------------------
    // Staging and committing start from the index file, not from
    // changes libgit2's copy of the index holds that were never
    // written, which is what any libgit2 operation that fails partway
    // leaves there. Reloading the copy only when the file had changed
    // kept them, and a commit took them along.
    // -----------------------------------------------------------------
    void commitIgnoresUnwrittenIndexChanges() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const QString head = output(git.run({"rev-parse", "HEAD"}));

        // What a checkout that failed after writing a.txt leaves.
        repo.writeFile("a.txt", "half switched\n");
        git_index* index = nullptr;
        QCOMPARE(git_repository_index(&index, repo.repo().raw()), 0);
        QCOMPARE(git_index_add_bypath(index, "a.txt"), 0);
        git_index_free(index);

        repo.writeFile("d.txt", "d\n");
        QVERIFY(repo.stageFile("d.txt").ok());
        QVERIFY(repo.commit("d").ok());
        QCOMPARE(output(git.run({"diff", "--name-only", head.toStdString(), "HEAD"})),
                 QStringLiteral("d.txt"));
        QCOMPARE(statusLines(repo.repo()), QStringList{QStringLiteral("a.txt changed")});
    }

    // -----------------------------------------------------------------
    // The same for a merge or a cherry-pick whose checkout fails
    // partway (here a file can't be written): the files before it are
    // written, libgit2's copy of the index had them while the index
    // file didn't, and status listed them as staged. libgit2 drops the
    // operation's state files, so nothing is in progress either.
    // -----------------------------------------------------------------
    void failedMergeOrCherryPickLeavesNothingStaged_data() {
        QTest::addColumn<bool>("merge");
        QTest::newRow("merge") << true;
        QTest::newRow("cherry-pick") << false;
    }

    void failedMergeOrCherryPickLeavesNothingStaged() {
#if defined(Q_OS_WIN)
        // libgit2 deletes a file before writing it where case is
        // ignored, and on Windows clears its read-only flag to do so.
        QSKIP("needs Unix file permissions");
#else
        QFETCH(bool, merge);
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        QVERIFY(repo.writeAndCommit("z/z.txt", "z\n", "z").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        repo.writeFile("a.txt", "side\n");
        repo.writeFile("z/z.txt", "side\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "side"})));
        auto side = repo.repo().resolveRef("HEAD");
        QVERIFY(side.ok());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        QVERIFY(repo.writeAndCommit("m.txt", "m\n", "m").ok());
        const QString head = output(git.run({"rev-parse", "HEAD"}));

        // The checkout writes a.txt, then can't write z/z.txt: not the
        // file, which is where it fails on Linux, and not its directory
        // either, which is where it fails where case is ignored (macOS):
        // libgit2 deletes the file there first.
        const QString zDir = QDir(repo.path()).filePath(QStringLiteral("z"));
        const QString z = QDir(zDir).filePath(QStringLiteral("z.txt"));
        QVERIFY(QFile::setPermissions(z, QFile::ReadOwner | QFile::ReadUser));
        QVERIFY(QFile::setPermissions(zDir, QFile::ReadOwner | QFile::ExeOwner
                                                | QFile::ReadUser | QFile::ExeUser));
        const auto unlock = qScopeGuard([&] {
            QFile::setPermissions(zDir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                                            | QFile::ReadUser | QFile::WriteUser | QFile::ExeUser);
            QFile::setPermissions(z, QFile::ReadOwner | QFile::WriteOwner
                                         | QFile::ReadUser | QFile::WriteUser);
        });
        if (QFile(z).open(QIODevice::WriteOnly | QIODevice::Append))
            QSKIP("file permissions don't stop this user (root?)");

        if (merge)
            QVERIFY(!repo.repo().merge(side.value()).ok());
        else
            QVERIFY(!repo.repo().cherryPick(side.value()).ok());
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::None);
        QCOMPARE(output(git.run({"rev-parse", "HEAD"})), head);
        QCOMPARE(readFile(repo.path(), QStringLiteral("a.txt")), QByteArray("side\n"));
        QCOMPARE(readFile(repo.path(), QStringLiteral("z/z.txt")), QByteArray("z\n"));
        QCOMPARE(statusLines(repo.repo()), QStringList{QStringLiteral("a.txt changed")});
#endif
    }

    // -----------------------------------------------------------------
    // Applying a stash writes its files, stages the ones it added in
    // libgit2's copy of the index, and writes the index file last.
    // When that write failed (here another git holds index.lock), the
    // copy kept them staged and status listed them so. They are new
    // files in the work tree, and a stash that didn't apply stays.
    // -----------------------------------------------------------------
    void failedStashApplyLeavesNothingStaged_data() {
        QTest::addColumn<bool>("pop");
        QTest::newRow("apply") << false;
        QTest::newRow("pop") << true;
    }

    void failedStashApplyLeavesNothingStaged() {
        QFETCH(bool, pop);
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        repo.writeFile("a.txt", "stashed\n");
        repo.writeFile("n.txt", "new\n");
        QVERIFY(repo.stageFile("n.txt").ok());
        QVERIFY(repo.repo().stashSave("wip").ok());
        QCOMPARE(statusLines(repo.repo()), QStringList());
        QFile lock(QDir(repo.path()).filePath(QStringLiteral(".git/index.lock")));
        QVERIFY(lock.open(QIODevice::WriteOnly));
        lock.close();

        const auto result = pop ? repo.repo().stashPop() : repo.repo().stashApply();
        QVERIFY(!result.ok());
        QVERIFY(lock.remove());
        QCOMPARE(readFile(repo.path(), QStringLiteral("a.txt")), QByteArray("stashed\n"));
        QCOMPARE(readFile(repo.path(), QStringLiteral("n.txt")), QByteArray("new\n"));
        QCOMPARE(statusLines(repo.repo()),
                 (QStringList{QStringLiteral("a.txt changed"), QStringLiteral("n.txt untracked")}));
        auto stashes = repo.repo().stashes();
        QVERIFY(stashes.ok());
        QCOMPARE(stashes->size(), size_t(1));
    }

    // -----------------------------------------------------------------
    // Finishing git's own operations with GitBolt's staging and commit.
    // git (the CLI) merges and rebases; the conflict resolver stages
    // and the Commit dialog commits through libgit2, whose copy of the
    // index knew nothing of what git had written, and whose state
    // cleanup ended a rebase along with a merge.
    // -----------------------------------------------------------------
    void commitConcludesAMergeWithWhatGitStaged() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "base\n", "base").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        repo.writeFile("a.txt", "side\n");
        repo.writeFile("b.txt", "b\n");
        QVERIFY(succeeded(git.run({"add", "-A"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "-m", "side"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        repo.writeFile("a.txt", "main\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "main"})));
        // a.txt conflicts; git stages side's b.txt.
        QVERIFY(!succeeded(git.run({"merge", "side"})));
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::Merge);

        repo.writeFile("a.txt", "resolved\n");
        QVERIFY(repo.stageFile("a.txt").ok());
        QVERIFY(repo.commit("merge side").ok());

        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::None);
        QCOMPARE(output(git.run({"ls-tree", "--name-only", "HEAD"})),
                 QStringLiteral("a.txt\nb.txt"));
        QCOMPARE(output(git.run({"rev-list", "--parents", "-1", "HEAD"})).split(' ').size(), 3);
        QCOMPARE(output(git.run({"status", "--porcelain"})), QString());
    }

    // What a rebase step reports to the in-progress bar: the conflicted
    // paths, counted from the index alone — conflictEntries() reads
    // every side's blob, for the resolver. Both see what git (the CLI)
    // wrote, not libgit2's copy of the index from before.
    void conflictCountMatchesConflictEntries() {
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess git(repo.path().toStdString());
        repo.writeFile("a.txt", "a\n");
        repo.writeFile("b.txt", "b\n");
        repo.writeFile("c.txt", "c\n");
        QVERIFY(succeeded(git.run({"add", "-A"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "-m", "base"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        repo.writeFile("a.txt", "side a\n");
        repo.writeFile("b.txt", "side b\n");
        repo.deleteFile("c.txt");
        QVERIFY(succeeded(git.run({"add", "-A"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "-m", "side"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        repo.writeFile("a.txt", "main a\n");
        repo.writeFile("b.txt", "main b\n");
        repo.writeFile("c.txt", "main c\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "main"})));
        auto before = repo.repo().conflictCount();
        QVERIFY(before.ok());
        QCOMPARE(*before, 0);

        // a.txt and b.txt both changed, c.txt changed here and deleted there.
        QVERIFY(!succeeded(git.run({"merge", "side"})));
        auto count = repo.repo().conflictCount();
        auto entries = repo.repo().conflictEntries();
        QVERIFY(count.ok() && entries.ok());
        QCOMPARE(*count, 3);
        QCOMPARE(static_cast<int>(entries->size()), 3);

        repo.writeFile("a.txt", "resolved\n");
        QVERIFY(succeeded(git.run({"add", "a.txt"})));
        count = repo.repo().conflictCount();
        QVERIFY(count.ok());
        QCOMPARE(*count, 2);
    }

    // A conflict whose ours is a symbolic link to a file outside the
    // repository, in the work tree too, and whose theirs is a file.
    // Accepting theirs replaces the link with theirs' file: what it
    // pointed to stays as it was, and the index has theirs' file. With
    // core.ignoreCase off, as on Linux: libgit2 clears the way for a
    // file it writes on a case-insensitive file system alone, and
    // elsewhere wrote it through the link. (And a checkout against
    // HEAD, where the path is a link, left the file out once the link
    // was gone.)
    void resolvingALinkConflictToAFileReplacesTheLink() {
#ifdef Q_OS_WIN
        QSKIP("QFile::link makes a shortcut on Windows, not a link");
#else
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        const QString marker = outside.filePath("MARKER");
        writeBytes(marker, "original marker\n");
        if (QTest::currentTestFailed())
            return;
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        QVERIFY(QFile::link(marker, QDir(repo.path()).filePath("p")));
        QVERIFY(repo.stageFile("p").ok());
        QVERIFY(repo.commit("p links out").ok());
        // The conflict as git records one: the ancestor's file, ours (the
        // link HEAD has) and theirs' file, in stages 1 to 3.
        const auto blob = [&](const std::string& content) {
            return output(git.runWithInput({"hash-object", "-w", "--stdin"}, content))
                .toStdString();
        };
        const std::string base = blob("base\n");
        const std::string file = blob("theirs\n");
        const std::string link = output(git.run({"rev-parse", "HEAD:p"})).toStdString();
        QVERIFY(succeeded(git.runWithInput({"update-index", "--index-info"},
                                           "0 " + std::string(40, '0') + "\tp\n"
                                           "100644 " + base + " 1\tp\n"
                                           "120000 " + link + " 2\tp\n"
                                           "100644 " + file + " 3\tp\n")));
        const auto conflicts = repo.repo().conflictCount();
        QVERIFY(conflicts.ok());
        QCOMPARE(*conflicts, 1);
        QVERIFY(succeeded(git.run({"config", "core.ignorecase", "false"})));

        // Opened anew: libgit2 keeps the core.* settings it has read.
        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        auto resolved = opened->resolveConflict("p", "theirs\n");
        QVERIFY2(resolved.ok(), resolved.error().message().c_str());
        QCOMPARE(readFile(outside.path(), "MARKER"), QByteArray("original marker\n"));
        const QFileInfo p(QDir(repo.path()).filePath("p"));
        QVERIFY(!p.isSymLink());
        QCOMPARE(readFile(repo.path(), "p"), QByteArray("theirs\n"));
        QCOMPARE(output(git.run({"ls-files", "-s", "--", "p"})),
                 QString::fromStdString("100644 " + file + " 0\tp"));
        QCOMPARE(output(git.run({"status", "--porcelain"})), QStringLiteral("T  p"));
#endif
    }

    // A conflicted path below a directory that someone made a symbolic
    // link to a folder outside the repository since: git never leaves a
    // tracked path below a link, and the resolution would land in the
    // folder. Refused, and nothing changes: not the folder's file, not
    // the link, not the conflict. With core.ignoreCase off, as on Linux,
    // where libgit2 goes through a link on the way to a file it writes.
    void resolveConflictRefusesAPathBelowALink() {
#ifdef Q_OS_WIN
        QSKIP("QFile::link makes a shortcut on Windows, not a link");
#else
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        writeBytes(outside.filePath("f"), "original marker\n");
        if (QTest::currentTestFailed())
            return;
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("d/f", "base\n", "base").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        QVERIFY(repo.writeAndCommit("d/f", "side\n", "side").ok());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        QVERIFY(repo.writeAndCommit("d/f", "main\n", "main").ok());
        QVERIFY(!succeeded(git.run({"merge", "side"})));
        QVERIFY(QDir(QDir(repo.path()).filePath("d")).removeRecursively());
        QVERIFY(QFile::link(outside.path(), QDir(repo.path()).filePath("d")));
        QVERIFY(succeeded(git.run({"config", "core.ignorecase", "false"})));

        // Opened anew: libgit2 keeps the core.* settings it has read.
        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        auto resolved = opened->resolveConflict("d/f", "resolved\n");
        QCOMPARE(readFile(outside.path(), "f"), QByteArray("original marker\n"));
        QVERIFY(QFileInfo(QDir(repo.path()).filePath("d")).isSymLink());
        QVERIFY(!resolved.ok());
        QCOMPARE(QString::fromStdString(resolved.error().message()),
                 QStringLiteral("d/f is below d, a symbolic link: the resolution would be "
                                "written wherever it points."));
        const auto conflicts = repo.repo().conflictCount();
        QVERIFY(conflicts.ok());
        QCOMPARE(*conflicts, 1);
#endif
    }

    // GitBolt's cherry-pick (libgit2's) notes a rename it found in a
    // conflict in the index's NAME entries. The checkout that writes a
    // resolution checked each of them against the conflicts it read,
    // and failed on the first that named a path no longer conflicted
    // ("a NAME entry referenced ancestor entry 'a.txt' which does not
    // exist in the main index"): no file in such a pick could be
    // resolved, the plain text conflict next to it neither. The
    // checkout reads no conflicts now; the path it writes is resolved
    // by then.
    void resolveConflictAfterACherryPickThatFoundARename() {
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        QByteArray rest;
        for (int line = 2; line <= 30; ++line)
            rest += "line " + QByteArray::number(line) + '\n';
        repo.writeFile("a.txt", "line 1\n" + rest);
        repo.writeFile("c.txt", "base\n");
        QVERIFY(succeeded(git.run({"add", "-A"})));
        QVERIFY(repo.commit("base").ok());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        repo.writeFile("a.txt", "side 1\n" + rest);
        repo.writeFile("c.txt", "side\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-a", "-m", "side"})));
        const QString picked = output(git.run({"rev-parse", "HEAD"}));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        QVERIFY(succeeded(git.run({"mv", "a.txt", "b.txt"})));
        repo.writeFile("b.txt", "main 1\n" + rest);
        repo.writeFile("c.txt", "main\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-a", "-m", "main"})));

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        const auto id = opened->resolveRef(picked.toStdString());
        QVERIFY(id.ok());
        const auto pick = opened->cherryPick(*id);
        QVERIFY2(pick.ok(), pick.ok() ? "" : pick.error().message().c_str());
        QVERIFY(pick->hasConflicts);
        QCOMPARE(output(git.run({"diff", "--name-only", "--diff-filter=U"})),
                 QStringLiteral("a.txt\nb.txt\nc.txt"));

        for (const char* path : {"c.txt", "b.txt"}) {
            const auto resolved = opened->resolveConflict(path, std::string("resolved ") + path + '\n');
            QVERIFY2(resolved.ok(), resolved.ok() ? path : resolved.error().message().c_str());
            QCOMPARE(readFile(repo.path(), path), QByteArray("resolved ") + path + '\n');
        }
        QCOMPARE(output(git.run({"diff", "--name-only", "--diff-filter=U"})), QStringLiteral("a.txt"));
        QCOMPARE(output(git.run({"diff", "--name-only", "--", "b.txt", "c.txt"})), QString());
        QCOMPARE(output(git.run({"show", ":b.txt"})), QStringLiteral("resolved b.txt"));
        QCOMPARE(output(git.run({"show", ":c.txt"})), QStringLiteral("resolved c.txt"));
        // And the renamed file's old name, which only theirs still has.
        const auto resolved = opened->resolveConflict("a.txt", "resolved a.txt\n");
        QVERIFY2(resolved.ok(), resolved.ok() ? "" : resolved.error().message().c_str());
        QCOMPARE(readFile(repo.path(), "a.txt"), QByteArray("resolved a.txt\n"));
        QCOMPARE(output(git.run({"diff", "--name-only", "--diff-filter=U"})), QString());
        QCOMPARE(output(git.run({"diff", "--name-only"})), QString());
    }

    // A text conflict's result is staged with the mode git's merge gives
    // it: a side's change of the executable bit wins over the side that
    // left it alone, and ours' wins where both changed it. It went in
    // with ours' mode, and the checkout then took the bit off the work
    // tree's file: where only theirs had made the file executable (in a
    // rebase, the user's own commit), that was lost.
    void resolveConflictMergesTheExecutableBit() {
#ifdef Q_OS_WIN
        QSKIP("core.fileMode is off on Windows: the work tree has no executable bit");
#else
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        QVERIFY(succeeded(git.run({"config", "core.filemode", "true"})));
        // Each file's content as the commit has it, and its mode: x, or
        // - for none.
        const auto commitFiles = [&](const QByteArray& text, const char* modes,
                                     const char* message) {
            for (int i = 0; i < 4; ++i) {
                const QString name = QStringLiteral("f%1.sh").arg(i);
                repo.writeFile(name, "#!/bin/sh\necho " + text + '\n');
                const QString file = QDir(repo.path()).filePath(name);
                const auto exec = QFile::ExeOwner | QFile::ExeUser | QFile::ExeGroup | QFile::ExeOther;
                const auto plain = QFile::permissions(file) & ~exec;
                if (!QFile::setPermissions(file, modes[i] == 'x' ? plain | exec : plain)
                    || !succeeded(git.run({"add", name.toStdString()})))
                    return false;
            }
            return succeeded(git.run({"commit", "-q", "-m", message}));
        };
        // f0: theirs makes it executable; f1: ours does; f2: ours makes
        // it no longer executable; f3: theirs does.
        QVERIFY(commitFiles("base", "--xx", "base"));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        QVERIFY(commitFiles("side", "x-x-", "side"));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        QVERIFY(commitFiles("main", "-x-x", "main"));
        QVERIFY(!succeeded(git.run({"merge", "-q", "side"})));
        QCOMPARE(output(git.run({"diff", "--name-only", "--diff-filter=U"})),
                 QStringLiteral("f0.sh\nf1.sh\nf2.sh\nf3.sh"));

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        for (int i = 0; i < 4; ++i) {
            const std::string name = "f" + std::to_string(i) + ".sh";
            const auto resolved = opened->resolveConflict(name, "#!/bin/sh\necho resolved\n");
            QVERIFY2(resolved.ok(), resolved.ok() ? name.c_str() : resolved.error().message().c_str());
        }
        const auto modeOf = [&](const char* name) {
            return output(git.run({"ls-files", "-s", "--", name})).left(6);
        };
        QCOMPARE(modeOf("f0.sh"), QStringLiteral("100755"));
        QCOMPARE(modeOf("f1.sh"), QStringLiteral("100755"));
        QCOMPARE(modeOf("f2.sh"), QStringLiteral("100644"));
        QCOMPARE(modeOf("f3.sh"), QStringLiteral("100644"));
        // The work tree's the same: nothing left to stage.
        QCOMPARE(output(git.run({"diff", "--name-only"})), QString());
        QVERIFY(QFileInfo(QDir(repo.path()).filePath("f0.sh")).isExecutable());
        QVERIFY(!QFileInfo(QDir(repo.path()).filePath("f3.sh")).isExecutable());

        // A rebase replays the user's commit as theirs: its chmod +x is
        // staged, for the commit the rebase makes.
        QVERIFY(succeeded(git.run({"reset", "-q", "--hard"})));
        const QString upstream = output(git.run({"branch", "--show-current"}));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "topic", "HEAD~1"})));
        QVERIFY(commitFiles("topic", "x-xx", "make f0 executable, edit"));
        QVERIFY(!succeeded(git.run({"rebase", upstream.toStdString()})));
        QCOMPARE(opened->state(), gitbolt::git::RepoState::Rebase);
        QCOMPARE(output(git.run({"ls-files", "-u", "--", "f0.sh"})).count(QLatin1Char('\n')), 2);
        const auto resolved = opened->resolveConflict("f0.sh", "#!/bin/sh\necho resolved\n");
        QVERIFY2(resolved.ok(), resolved.ok() ? "" : resolved.error().message().c_str());
        QCOMPARE(modeOf("f0.sh"), QStringLiteral("100755"));
        QVERIFY(QFileInfo(QDir(repo.path()).filePath("f0.sh")).isExecutable());
#endif
    }

    // Paths whose names libgit2 reads as pattern syntax when they are
    // the paths of a checkout: a backslash (taken as escaping the next
    // character), a trailing space or carriage return (taken off), a
    // line feed (the end of the pattern), a leading # (a comment, which
    // matches every path) and the wildcards. The resolution was staged
    // and its file left with the conflict markers, or another file was
    // written as well. Each is written now, and only it: the work
    // tree's other files keep their changes. With core.ignoreCase on
    // (as on macOS and Windows) and off (as on Linux): libgit2 matches
    // patterns either way.
    void resolveConflictWritesAPathOfAnyName_data() {
        QTest::addColumn<bool>("ignoreCase");
        QTest::newRow("ignoreCase") << true;
        QTest::newRow("case-sensitive") << false;
    }

    void resolveConflictWritesAPathOfAnyName() {
#ifdef Q_OS_WIN
        QSKIP("Windows file names have none of these characters");
#else
        const QStringList names = {
            QStringLiteral("x\\y.txt"),   QStringLiteral("sub/x\\*.txt"),
            QStringLiteral("a*b.txt"),    QStringLiteral("[ab].txt"),
            QStringLiteral("?.txt"),      QStringLiteral("!bang.txt"),
            QStringLiteral("#hash.txt"),  QStringLiteral("trailing "),
            QStringLiteral("new\nline"),  QStringLiteral("Icon\r"),
            QStringLiteral("tab\tname"),  QStringLiteral(" lead.txt"),
            QStringLiteral("end\\\r"),
        };
        // What a name taken as a pattern would also, or instead, match.
        const QStringList others = {
            QStringLiteral("xy.txt"),    QStringLiteral("sub/x*.txt"),  QStringLiteral("sub/xa.txt"),
            QStringLiteral("aXb.txt"),   QStringLiteral("a.txt"),       QStringLiteral("b.txt"),
            QStringLiteral("1.txt"),     QStringLiteral("bang.txt"),    QStringLiteral("#hash.txt.orig"),
            QStringLiteral("trailing"),  QStringLiteral("new"),         QStringLiteral("Icon"),
            QStringLiteral("lead.txt"),  QStringLiteral("end\\"),
        };
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        const auto commitAll = [&](const QByteArray& text, bool withOthers, const char* message) {
            for (const QString& name : names)
                repo.writeFile(name, text);
            if (withOthers) {
                for (const QString& name : others)
                    repo.writeFile(name, "other\n");
            }
            return succeeded(git.run({"add", "-A"}))
                && succeeded(git.run({"commit", "-q", "-m", message}));
        };
        QVERIFY(commitAll("base\n", true, "base"));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        QVERIFY(commitAll("side\n", false, "side"));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        QVERIFY(commitAll("main\n", false, "main"));
        QVERIFY(!succeeded(git.run({"merge", "-q", "side"})));
        for (const QString& name : others)
            repo.writeFile(name, "changed\n");
        const auto list = [&](std::vector<std::string> args) {
            const auto out = git.run(args);
            QStringList paths = succeeded(out)
                ? QString::fromStdString(out.value().stdoutData).split(QChar(0), Qt::SkipEmptyParts)
                : QStringList{QStringLiteral("<failed>")};
            paths.sort();
            return paths;
        };
        QStringList sortedNames = names;
        sortedNames.sort();
        QCOMPARE(list({"diff", "-z", "--name-only", "--diff-filter=U"}), sortedNames);
        QFETCH(bool, ignoreCase);
        QVERIFY(succeeded(git.run({"config", "core.ignorecase", ignoreCase ? "true" : "false"})));

        // Opened anew: libgit2 keeps the core.* settings it has read.
        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        for (const QString& name : names) {
            const auto resolved = opened->resolveConflict(name.toStdString(), "resolved\n");
            QVERIFY2(resolved.ok(), qPrintable(name + QStringLiteral(": ")
                                               + QString::fromStdString(resolved.ok() ? std::string()
                                                                                      : resolved.error().message())));
        }
        for (const QString& name : names)
            QVERIFY2(readFile(repo.path(), name) == "resolved\n", qPrintable(name));
        for (const QString& name : others)
            QVERIFY2(readFile(repo.path(), name) == "changed\n", qPrintable(name));
        QCOMPARE(list({"diff", "-z", "--name-only", "--diff-filter=U"}), QStringList());
        QStringList sortedOthers = others;
        sortedOthers.sort();
        // In the work tree, only the other files' changes; the resolved
        // files are staged and as staged.
        QCOMPARE(list({"diff", "-z", "--name-only"}), sortedOthers);
        QCOMPARE(list({"diff", "-z", "--name-only", "--cached"}), sortedNames);
#endif
    }

    // libgit2 takes a path that is a pattern's literal spelling of
    // another path (a\\, two backslashes, for a\ with one) as that path
    // too. Resolving a\ would have written a\\ from the index as well,
    // throwing away its changes in the work tree, or, where a\\ has a
    // conflict of its own, deleted it, conflict markers and all, from
    // the work tree and the index; with its file gone from the work
    // tree already, its conflict went without a word. It is refused
    // instead, and nothing changes: neither file, nor either conflict.
    void resolveConflictWritesNoOtherPath_data() {
        QTest::addColumn<bool>("bothConflicted");
        QTest::addColumn<bool>("otherDeleted");
        QTest::newRow("changed") << false << false;
        QTest::newRow("conflicted") << true << false;
        QTest::newRow("conflicted, its file deleted") << true << true;
    }

    void resolveConflictWritesNoOtherPath() {
#ifdef Q_OS_WIN
        QSKIP("Windows file names have no backslash");
#else
        QFETCH(bool, bothConflicted);
        QFETCH(bool, otherDeleted);
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        const QString one = QStringLiteral("a\\");
        const QString two = QStringLiteral("a\\\\");
        const auto commitBoth = [&](const QByteArray& text, const QByteArray& other,
                                    const char* message) {
            repo.writeFile(one, text);
            repo.writeFile(two, other);
            return succeeded(git.run({"add", "-A"}))
                && succeeded(git.run({"commit", "-q", "-m", message}));
        };
        QVERIFY(commitBoth("base\n", "other\n", "base"));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        QVERIFY(commitBoth("side\n", bothConflicted ? "other side\n" : "other\n", "side"));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        QVERIFY(commitBoth("main\n", bothConflicted ? "other main\n" : "other\n", "main"));
        QVERIFY(!succeeded(git.run({"merge", "-q", "side"})));
        if (!bothConflicted)
            repo.writeFile(two, "changed\n");
        if (otherDeleted)
            repo.deleteFile(two);
        const QByteArray markers = readFile(repo.path(), one);
        const QByteArray otherBefore = readFile(repo.path(), two);
        const QString indexBefore = output(git.run({"ls-files", "-s"}));

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        const auto resolved = opened->resolveConflict(one.toStdString(), "resolved\n");
        QCOMPARE(readFile(repo.path(), two), otherBefore);
        QCOMPARE(output(git.run({"ls-files", "-s"})), indexBefore);
        QVERIFY(!resolved.ok());
        QCOMPARE(QString::fromStdString(resolved.error().message()),
                 QStringLiteral("a\\ was not resolved: writing it would also have written a\\\\."));
        QCOMPARE(readFile(repo.path(), one), markers);
        const auto conflicts = opened->conflictCount();
        QVERIFY(conflicts.ok());
        QCOMPARE(*conflicts, bothConflicted ? 2 : 1);
#endif
    }

    // merge.conflictstyle with no value, added while a conflict is open:
    // the checkout that writes a resolution read it, and a system
    // libgit2 crashed (the bundled one fails). Refused first, as the
    // other checkouts are, and the conflict stays as it was.
    void resolveConflictRefusesAValuelessConflictStyle() {
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        QVERIFY(repo.writeAndCommit("a.txt", "base\n", "base").ok());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        QVERIFY(repo.writeAndCommit("a.txt", "side\n", "side").ok());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        QVERIFY(repo.writeAndCommit("a.txt", "main\n", "main").ok());
        QVERIFY(!succeeded(git.run({"merge", "-q", "side"})));
        const QByteArray markers = readFile(repo.path(), "a.txt");
        appendConfig(repo.path(), "[merge]\n\tconflictstyle\n");
        if (QTest::currentTestFailed())
            return;

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        const auto resolved = opened->resolveConflict("a.txt", "resolved\n");
        QVERIFY(!resolved.ok());
        QCOMPARE(QString::fromStdString(resolved.error().message()),
                 QStringLiteral("missing value for 'merge.conflictstyle'"));
        QCOMPARE(readFile(repo.path(), "a.txt"), markers);
        const auto conflicts = opened->conflictCount();
        QVERIFY(conflicts.ok());
        QCOMPARE(*conflicts, 1);
    }

    // Accept Ours or Accept Theirs on a conflict on a symbolic link: the
    // resolver handed over its editor's text, and the side with that
    // text was staged. The editor gives a no-break space back as a space
    // and a line separator as a line feed, so a link whose target had
    // one matched neither side and could not be resolved. Bytes that
    // aren't UTF-8 were the same text on both sides, and so were a link
    // and a file holding its target: the wrong side could be staged. The
    // side accepted is staged as it is now, whatever text comes with it
    // (here the other side's). And as the resolver listed it: where the
    // conflict was staged since as the other side (from the file list,
    // say), Accept Theirs was refused, as if there were no theirs, and
    // Accept Ours kept theirs.
    void resolveConflictStagesTheSideAccepted_data() {
        QTest::addColumn<QByteArray>("ours");
        QTest::addColumn<QByteArray>("theirs");
        QTest::addColumn<QString>("theirsMode");
        QTest::newRow("no-break space") << QByteArray("z") << QByteArray("x\xc2\xa0y")
                                        << QStringLiteral("120000");
        QTest::newRow("line separator") << QByteArray("z") << QByteArray("x\xe2\x80\xa8y")
                                        << QStringLiteral("120000");
        QTest::newRow("not UTF-8") << QByteArray("x\xfe") << QByteArray("x\xff")
                                   << QStringLiteral("120000");
        QTest::newRow("a file holding the target") << QByteArray("t") << QByteArray("t")
                                                   << QStringLiteral("100644");
    }

    void resolveConflictStagesTheSideAccepted() {
        QFETCH(QByteArray, ours);
        QFETCH(QByteArray, theirs);
        QFETCH(QString, theirsMode);
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        const auto blob = [&](const QByteArray& content) {
            return output(git.runWithInput({"hash-object", "-w", "--stdin"},
                                            content.toStdString()))
                .toStdString();
        };
        const std::string base = blob("base");
        const std::string oursId = blob(ours);
        const std::string theirsId = blob(theirs);
        // The conflict as git records one, in stages 1 to 3: the
        // ancestor's link, ours' and theirs' (a link or a file). Without
        // theirs, as when theirs deleted it.
        const auto conflict = [&](bool withTheirs) {
            std::string info = "0 " + std::string(40, '0') + "\tp\n"
                               "120000 " + base + " 1\tp\n"
                               "120000 " + oursId + " 2\tp\n";
            if (withTheirs)
                info += theirsMode.toStdString() + " " + theirsId + " 3\tp\n";
            return succeeded(git.runWithInput({"update-index", "--index-info"}, info));
        };

        const std::string oursEntry = "120000 " + oursId + " 0\tp";
        const std::string theirsEntry = theirsMode.toStdString() + " " + theirsId + " 0\tp";
        const auto stage = [&](const std::string& entry) {
            return succeeded(git.runWithInput({"update-index", "--index-info"},
                                              "0 " + std::string(40, '0') + "\tp\n" + entry
                                                  + "\n"));
        };

        using gitbolt::git::AcceptedSide;
        using gitbolt::git::ConflictSide;
        using gitbolt::git::ObjectId;
        const AcceptedSide listedOurs{ConflictSide::Ours, ObjectId::fromHex(oursId), 0120000};
        const AcceptedSide listedTheirs{ConflictSide::Theirs, ObjectId::fromHex(theirsId),
                                        theirsMode.toUInt(nullptr, 8)};
        for (const bool stagedSince : {false, true}) {
            for (const AcceptedSide& side : {listedOurs, listedTheirs}) {
                const bool isOurs = side.side == ConflictSide::Ours;
                QVERIFY(stagedSince ? stage(isOurs ? theirsEntry : oursEntry) : conflict(true));
                const auto resolved = repo.repo().resolveConflict(
                    "p", (isOurs ? theirs : ours).toStdString(), side);
                QVERIFY2(resolved.ok(), resolved.ok() ? "" : resolved.error().message().c_str());
                QCOMPARE(output(git.run({"ls-files", "-s", "--", "p"})),
                         QString::fromStdString(isOurs ? oursEntry : theirsEntry));
                // And the work tree has it as staged.
                QCOMPARE(output(git.run({"diff", "--name-only", "--", "p"})), QString());
            }
        }

        // A side that isn't there is refused, and the conflict stays.
        QVERIFY(conflict(false));
        const QString before = output(git.run({"ls-files", "-s", "--", "p"}));
        const auto refused =
            repo.repo().resolveConflict("p", "", AcceptedSide{ConflictSide::Theirs, {}, 0});
        QVERIFY(!refused.ok());
        QCOMPARE(QString::fromStdString(refused.error().message()),
                 QStringLiteral("p has no Theirs to accept."));
        QCOMPARE(output(git.run({"ls-files", "-s", "--", "p"})), before);
    }

    // Discard and Unstage gave libgit2 the file's path as a pattern, as
    // the resolver's checkout once did (resolveConflictWritesAPathOfAnyName
    // above). Discarding a file named * checked out every file from HEAD,
    // their staged and unstaged changes gone, and unstaging it unstaged
    // every file; [ab].txt took a.txt and b.txt along, #x every file whose
    // name begins with #x, and x\y meant xy, Icon\r Icon. Each changes the
    // file named and no other now. Every file here has a staged change
    // and an unstaged one on top of it.
    void discardAndUnstageChangeOnlyTheFileNamed_data() {
        QTest::addColumn<bool>("discard");
        QTest::addColumn<bool>("ignoreCase");
        QTest::newRow("discard, ignoreCase") << true << true;
        QTest::newRow("discard, case-sensitive") << true << false;
        QTest::newRow("unstage, ignoreCase") << false << true;
        QTest::newRow("unstage, case-sensitive") << false << false;
    }

    void discardAndUnstageChangeOnlyTheFileNamed() {
        QFETCH(bool, discard);
        QFETCH(bool, ignoreCase);
        // Names a Windows file can have too, and what each, taken as a
        // pattern, would also or instead match.
        QStringList names = {
            QStringLiteral("[ab].txt"), QStringLiteral("sub/[ab].txt"),
            QStringLiteral("#x"),       QStringLiteral("!x"),
        };
        QStringList others = {
            QStringLiteral("a.txt"),     QStringLiteral("b.txt"), QStringLiteral("sub/a.txt"),
            QStringLiteral("sub/b.txt"), QStringLiteral("#xy"),   QStringLiteral("x"),
        };
#ifndef Q_OS_WIN
        names += {
            QStringLiteral("*"),         QStringLiteral("a*"),     QStringLiteral("[ab]"),
            QStringLiteral("?"),         QStringLiteral("x\\y"),   QStringLiteral("trailing "),
            QStringLiteral("end\\"),     QStringLiteral("Icon\r"), QStringLiteral("new\nline"),
        };
        others += {
            QStringLiteral("a"),         QStringLiteral("b"),      QStringLiteral("ab"),
            QStringLiteral("xy"),        QStringLiteral("trailing"), QStringLiteral("end"),
            QStringLiteral("Icon"),      QStringLiteral("new"),
        };
#endif
        const QStringList all = names + others;
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        const auto writeAll = [&](const QByteArray& text) {
            for (const QString& name : all)
                repo.writeFile(name, text);
        };
        writeAll("base\n");
        QVERIFY(succeeded(git.run({"add", "-A"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "-m", "base"})));
        QVERIFY(succeeded(git.run({"config", "core.ignorecase", ignoreCase ? "true" : "false"})));
        const auto list = [&](std::vector<std::string> args) {
            const auto out = git.run(args);
            QStringList paths = succeeded(out)
                ? QString::fromStdString(out.value().stdoutData).split(QChar(0), Qt::SkipEmptyParts)
                : QStringList{QStringLiteral("<failed>")};
            paths.sort();
            return paths;
        };
        QStringList sortedAll = all;
        sortedAll.sort();

        for (const QString& name : names) {
            writeAll("staged\n");
            QVERIFY(succeeded(git.run({"add", "-A"})));
            writeAll("unstaged\n");
            QCOMPARE(list({"diff", "-z", "--name-only", "--cached"}), sortedAll);

            // Opened anew, as git changed the index: libgit2 keeps the
            // core.* settings it has read.
            auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
            QVERIFY(opened.ok());
            const auto result = discard ? opened->discardWorkdirChanges(name.toStdString())
                                        : opened->unstageFile(name.toStdString());
            QVERIFY2(result.ok(), qPrintable(name + QStringLiteral(": ")
                                             + QString::fromStdString(result.ok() ? std::string()
                                                                                  : result.error().message())));
            QStringList rest = sortedAll;
            rest.removeOne(name);
            const QStringList staged = list({"diff", "-z", "--name-only", "--cached"});
            const QStringList changed = list({"diff", "-z", "--name-only"});
            QVERIFY2(staged == rest, qPrintable(name + QStringLiteral(": staged ")
                                                + staged.join(QLatin1Char('|'))));
            QVERIFY2(changed == (discard ? rest : sortedAll),
                     qPrintable(name + QStringLiteral(": changed ") + changed.join(QLatin1Char('|'))));
            QVERIFY2(readFile(repo.path(), name) == (discard ? "base\n" : "unstaged\n"),
                     qPrintable(name));
            for (const QString& other : rest)
                QVERIFY2(readFile(repo.path(), other) == "unstaged\n",
                         qPrintable(name + QStringLiteral(": ") + other));
        }
    }

    // libgit2 takes a path that is a pattern's literal spelling of
    // another path (a\\, two backslashes, for a\) as that path too (see
    // resolveConflictWritesNoOtherPath). Discarding a\ would also have
    // discarded a\\'s changes, or ended a\\'s conflict where its file
    // is HEAD's (a file the other side deleted), without a word. It is
    // refused, and nothing changes. Unstaging, which takes no pattern
    // now, unstages a\ alone.
    void discardChangesNoOtherPath_data() {
        QTest::addColumn<bool>("conflicted");
        QTest::newRow("changed") << false;
        QTest::newRow("conflicted") << true;
    }

    void discardChangesNoOtherPath() {
#ifdef Q_OS_WIN
        QSKIP("Windows file names have no backslash");
#else
        QFETCH(bool, conflicted);
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        const QString one = QStringLiteral("a\\");
        const QString two = QStringLiteral("a\\\\");
        repo.writeFile(one, "base\n");
        repo.writeFile(two, "base\n");
        QVERIFY(succeeded(git.run({"add", "-A"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "-m", "base"})));
        if (conflicted) {
            QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
            QVERIFY(succeeded(git.run({"--literal-pathspecs", "rm", "-q", "--", two.toStdString()})));
            QVERIFY(succeeded(git.run({"commit", "-q", "-m", "side"})));
            QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
            repo.writeFile(two, "main\n");
            QVERIFY(succeeded(git.run({"commit", "-q", "-a", "-m", "main"})));
            QVERIFY(!succeeded(git.run({"merge", "-q", "side"})));
            repo.writeFile(one, "changed\n");
        } else {
            repo.writeFile(one, "staged\n");
            repo.writeFile(two, "staged\n");
            QVERIFY(succeeded(git.run({"add", "-A"})));
            repo.writeFile(one, "changed\n");
            repo.writeFile(two, "changed\n");
        }
        const QByteArray otherBefore = readFile(repo.path(), two);
        const QString indexBefore = output(git.run({"ls-files", "-s"}));

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        const auto discarded = opened->discardWorkdirChanges(one.toStdString());
        QCOMPARE(readFile(repo.path(), two), otherBefore);
        QCOMPARE(readFile(repo.path(), one), QByteArray("changed\n"));
        QCOMPARE(output(git.run({"ls-files", "-s"})), indexBefore);
        QVERIFY(!discarded.ok());
        QCOMPARE(QString::fromStdString(discarded.error().message()),
                 QStringLiteral("a\\ was not discarded: that would also have discarded the "
                                "changes to a\\\\."));

        if (!conflicted) {
            QVERIFY(opened->unstageFile(one.toStdString()).ok());
            QCOMPARE(output(git.run({"diff", "-z", "--name-only", "--cached"})), two + QChar(0));
        }
#endif
    }

    // A file that is a directory now, everything staged: the index had
    // d deleted and d/x added. Unstaging d put HEAD's d back beside
    // d/x, an index git makes no tree from ("You have both d and d/x"),
    // and of which GitBolt's commit made a tree without d. libgit2 drops
    // d/x for the d it adds only where nothing sorts before d/x, and a
    // sorts before it. The other way round, a directory that is a file
    // now, unstaging d took the file out and left the directory's files
    // out too. What the index has below d goes back to HEAD's as well
    // now, as `git reset -- d` has it.
    void unstageFileReplacedByADirectory_data() {
        QTest::addColumn<bool>("wasFile");
        QTest::newRow("a file, a directory now") << true;
        QTest::newRow("a directory, a file now") << false;
    }

    void unstageFileReplacedByADirectory() {
        QFETCH(bool, wasFile);
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        repo.writeFile("a", "a\n");
        if (wasFile) {
            repo.writeFile("d", "d\n");
        } else {
            repo.writeFile("d/x", "x\n");
            repo.writeFile("d/y", "y\n");
        }
        QVERIFY(succeeded(git.run({"add", "-A"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "-m", "base"})));
        const QString d = QDir(repo.path()).filePath(QStringLiteral("d"));
        if (wasFile) {
            QVERIFY(QFile::remove(d));
            repo.writeFile("d/x", "x\n");
        } else {
            QVERIFY(QDir(d).removeRecursively());
            repo.writeFile("d", "d\n");
        }
        QVERIFY(succeeded(git.run({"add", "-A"})));

        QVERIFY(repo.repo().unstageFile("d").ok());
        // Nothing staged: the index is HEAD's tree, and one git writes.
        QCOMPARE(output(git.run({"write-tree"})), output(git.run({"rev-parse", "HEAD^{tree}"})));
        QCOMPARE(output(git.run({"ls-files", "--others"})),
                 wasFile ? QStringLiteral("d/x") : QStringLiteral("d"));
    }

    void commitWhileARebaseIsStoppedKeepsTheRebase() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "base").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const QString branch = output(git.run({"branch", "--show-current"}));
        repo.writeFile("b.txt", "b\n");
        QVERIFY(succeeded(git.run({"add", "b.txt"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "-m", "second"})));
        // Stops once "second" is replayed, as an `edit` would.
        QVERIFY(!succeeded(git.run({"rebase", "--exec", "false", "HEAD~1"})));
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::Rebase);

        repo.writeFile("c.txt", "c\n");
        QVERIFY(repo.stageFile("c.txt").ok());
        QVERIFY(repo.commit("added while stopped").ok());
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::Rebase);

        QVERIFY(succeeded(git.run({"rebase", "--continue"})));
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::None);
        QCOMPARE(output(git.run({"branch", "--show-current"})), branch);
        QCOMPARE(output(git.run({"log", "--format=%s"})),
                 QStringLiteral("added while stopped\nsecond\nbase"));
    }

    // libgit2's cherry-pick stages the picked commit's changes and
    // stops there, so every cherry-pick ends in this commit. It is
    // still the picked commit's work: like `git commit`, the commit
    // keeps that commit's author and author date, with the user as
    // committer. It used to go into the history as the user's own.
    void commitConcludingACherryPickKeepsItsAuthor() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "base").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        repo.writeFile("b.txt", "b\n");
        QVERIFY(succeeded(git.run({"add", "b.txt"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "-m", "their work",
                                   "--author=Team Mate <mate@example.com>",
                                   "--date=2001-02-03T04:05:06+01:00"})));
        auto picked = repo.repo().resolveRef("HEAD");
        QVERIFY(picked.ok());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));

        auto pick = repo.repo().cherryPick(picked.value());
        QVERIFY(pick.ok());
        QVERIFY(!pick->hasConflicts);
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::CherryPick);
        QVERIFY(repo.commit("their work").ok());
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::None);
        QCOMPARE(output(git.run({"log", "-1", "--date=iso-strict",
                                 "--format=%an <%ae> %ad, %cn <%ce>"})),
                 QStringLiteral("Team Mate <mate@example.com> 2001-02-03T04:05:06+01:00, "
                                "Test User <test@gitbolt.local>"));

        // The cherry-pick is over: the next commit is the user's.
        QVERIFY(repo.writeAndCommit("c.txt", "c\n", "mine").ok());
        QCOMPARE(output(git.run({"log", "-1", "--format=%an <%ae>"})),
                 QStringLiteral("Test User <test@gitbolt.local>"));
    }

    // Amending changes the commit, not who wrote it: like `git commit
    // --amend`, the author keeps their name, email and date, and the
    // user becomes the committer. The amend used to make the user its
    // author, dated now.
    void amendKeepsTheAuthor() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "base").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const QString base = output(git.run({"rev-parse", "HEAD"}));
        repo.writeFile("b.txt", "b\n");
        QVERIFY(succeeded(git.run({"add", "b.txt"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "-m", "their work",
                                   "--author=Team Mate <mate@example.com>",
                                   "--date=2001-02-03T04:05:06+01:00"})));

        auto amended = repo.repo().commit("reworded", /*amend=*/true);
        QVERIFY2(amended.ok(), amended.ok() ? "" : amended.error().message().c_str());
        QCOMPARE(output(git.run({"log", "-1", "--date=iso-strict",
                                 "--format=%an <%ae> %ad, %cn <%ce>"})),
                 QStringLiteral("Team Mate <mate@example.com> 2001-02-03T04:05:06+01:00, "
                                "Test User <test@gitbolt.local>"));
        QCOMPARE(output(git.run({"log", "-1", "--format=%s"})), QStringLiteral("reworded"));
        QCOMPARE(output(git.run({"rev-parse", "HEAD^"})), base);
    }

    // An amend can't conclude a merge, a cherry-pick or a revert: it
    // flattened the merge into a one-parent commit, or folded the pick
    // into the commit before it, and left the operation's state files
    // behind, so the repository still read as mid-operation. Refused,
    // as git refuses it for a merge and a cherry-pick (a revert too,
    // here: it reads "commit to finish it"). A rebase that stopped is
    // another matter: amending is what an `edit` stop is for.
    void amendIsRefusedMidOperation_data() {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<bool>("refused");
        QTest::newRow("merge") << QStringLiteral("merge") << true;
        QTest::newRow("cherry-pick") << QStringLiteral("cherry-pick") << true;
        QTest::newRow("revert") << QStringLiteral("revert") << true;
        QTest::newRow("rebase stop") << QStringLiteral("rebase") << false;
    }

    void amendIsRefusedMidOperation() {
        QFETCH(QString, operation);
        QFETCH(bool, refused);
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "base\n", "base").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        repo.writeFile("a.txt", "side\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "side"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        repo.writeFile("a.txt", "main\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "main"})));

        // A conflict in a.txt stops the first three, an exec that fails
        // the rebase; a.txt is then resolved (changed) and staged.
        if (operation == QLatin1String("merge"))
            QVERIFY(!succeeded(git.run({"merge", "side"})));
        else if (operation == QLatin1String("cherry-pick"))
            QVERIFY(!succeeded(git.run({"cherry-pick", "side"})));
        else if (operation == QLatin1String("revert"))
            QVERIFY(!succeeded(git.run({"revert", "--no-edit", "side"})));
        else
            QVERIFY(!succeeded(git.run({"rebase", "--exec", "false", "HEAD~1"})));
        repo.writeFile("a.txt", "resolved\n");
        QVERIFY(repo.stageFile("a.txt").ok());
        const auto state = repo.repo().state();
        QVERIFY(state != gitbolt::git::RepoState::None);
        const QString head = output(git.run({"rev-parse", "HEAD"}));
        const QString gitDir = QString::fromStdString(repo.repo().path());
        QStringList stateFiles;
        for (const char* name : {"MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "MERGE_MSG"})
            if (QFileInfo::exists(gitDir + QLatin1String(name)))
                stateFiles << QLatin1String(name);

        // What the Commit dialog asks before it offers Amend.
        const QString problem = refused
            ? QStringLiteral("You are in the middle of a %1 -- cannot amend.").arg(operation)
            : QString();
        QCOMPARE(QString::fromStdString(repo.repo().amendProblem()), problem);

        auto amended = repo.repo().commit("x", /*amend=*/true);
        QCOMPARE(repo.repo().state(), state);
        if (!refused) {
            QVERIFY2(amended.ok(), amended.ok() ? "" : amended.error().message().c_str());
            QCOMPARE(output(git.run({"log", "-1", "--format=%s"})), QStringLiteral("x"));
            return;
        }
        QVERIFY(!amended.ok());
        QCOMPARE(QString::fromStdString(amended.error().message()), problem);
        QCOMPARE(output(git.run({"rev-parse", "HEAD"})), head);
        for (const QString& name : stateFiles)
            QVERIFY2(QFileInfo::exists(gitDir + name), qPrintable(name));
    }

    // -----------------------------------------------------------------
    // Staging that fails partway (here another git holds index.lock, so
    // the index file can't be written) left its change in libgit2's
    // copy of the index, and status read that copy: a file that failed
    // to stage showed as staged, one that failed to unstage as
    // unstaged, though the index file had neither.
    // -----------------------------------------------------------------
    void failedStagingLeavesTheIndexAsItWas_data() {
        QTest::addColumn<QString>("operation");
        QTest::newRow("stage") << QStringLiteral("stage");
        QTest::newRow("stage all") << QStringLiteral("stage all");
        QTest::newRow("unstage") << QStringLiteral("unstage");
    }

    void failedStagingLeavesTheIndexAsItWas() {
        QFETCH(QString, operation);
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        QVERIFY(repo.writeAndCommit("b.txt", "b\n", "b").ok());
        repo.writeFile("a.txt", "changed\n");
        repo.writeFile("b.txt", "staged\n");
        QVERIFY(repo.stageFile("b.txt").ok());
        const QStringList before{QStringLiteral("a.txt changed"), QStringLiteral("b.txt staged")};
        QCOMPARE(statusLines(repo.repo()), before);
        QFile lock(QDir(repo.path()).filePath(QStringLiteral(".git/index.lock")));
        QVERIFY(lock.open(QIODevice::WriteOnly));
        lock.close();

        const auto result = operation == QLatin1String("stage") ? repo.stageFile("a.txt")
                          : operation == QLatin1String("stage all") ? repo.stageAll()
                          : repo.repo().unstageFile("b.txt");
        QVERIFY(!result.ok());
        QVERIFY(lock.remove());
        QCOMPARE(statusLines(repo.repo()), before);
    }

    // -----------------------------------------------------------------
    // A commit with nothing staged is refused, as `git commit` refuses
    // it: it made an empty commit. A merge may conclude with nothing
    // new (its second parent is the point), and an amend may keep the
    // commit's changes as they are; an amend that would leave the
    // commit with none at all is refused, as git refuses it.
    // -----------------------------------------------------------------
    void commitWithNothingStagedIsRefused_data() {
        QTest::addColumn<QString>("situation");
        QTest::addColumn<QString>("error");     // empty: the commit is made
        const QString nothing = QStringLiteral("Nothing to commit: no changes are staged.");
        QTest::newRow("unstaged changes only") << QStringLiteral("unstaged") << nothing;
        QTest::newRow("unborn HEAD, empty index") << QStringLiteral("unborn") << nothing;
        QTest::newRow("cherry-pick already applied") << QStringLiteral("cherry-pick")
            << QStringLiteral("The cherry-picked changes are already on this branch; "
                              "abort the cherry-pick.");
        QTest::newRow("merge -s ours") << QStringLiteral("merge") << QString();
        QTest::newRow("amend, nothing staged") << QStringLiteral("amend") << QString();
        QTest::newRow("amend that empties the commit") << QStringLiteral("amend empty")
            << QStringLiteral("Amending would leave the last commit empty.");
    }

    void commitWithNothingStagedIsRefused() {
        QFETCH(QString, situation);
        QFETCH(QString, error);
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        bool amend = false;
        if (situation != QLatin1String("unborn")) {
            QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
            QVERIFY(repo.writeAndCommit("b.txt", "b\n", "b").ok());
        }
        if (situation == QLatin1String("unstaged")) {
            repo.writeFile("a.txt", "changed\n");
            repo.writeFile("new.txt", "new\n");
        } else if (situation == QLatin1String("cherry-pick")) {
            // side adds c.txt; so does the branch picked onto.
            QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
            QVERIFY(repo.writeAndCommit("c.txt", "c\n", "c on side").ok());
            auto picked = repo.repo().head();
            QVERIFY(picked.ok());
            QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
            QVERIFY(repo.writeAndCommit("c.txt", "c\n", "c here too").ok());
            // GitBolt's own pick says so and leaves nothing in progress;
            // git's (a terminal's) stops with the pick in progress.
            const auto ours = repo.repo().cherryPick(*picked);
            QVERIFY(ours.ok() && ours->alreadyApplied && !ours->hasConflicts);
            QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::None);
            QVERIFY(!succeeded(git.run({"cherry-pick", picked->toHex()})));
            QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::CherryPick);
        } else if (situation == QLatin1String("merge")) {
            QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
            QVERIFY(repo.writeAndCommit("c.txt", "c\n", "c on side").ok());
            QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
            QVERIFY(succeeded(git.run({"merge", "-q", "-s", "ours", "--no-ff", "--no-commit",
                                       "side"})));
            QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::Merge);
        } else if (situation == QLatin1String("amend")) {
            amend = true;
        } else if (situation == QLatin1String("amend empty")) {
            // The index as it was before "b": amended, "b" adds nothing.
            QVERIFY(succeeded(git.run({"rm", "-q", "--cached", "b.txt"})));
            amend = true;
        }
        const QString head = output(git.run({"rev-parse", "--verify", "-q", "HEAD"}));
        const auto state = repo.repo().state();

        auto made = repo.repo().commit("x", amend);
        if (error.isEmpty()) {
            QVERIFY2(made.ok(), made.ok() ? "" : made.error().message().c_str());
            QCOMPARE(output(git.run({"log", "-1", "--format=%s"})), QStringLiteral("x"));
            if (situation == QLatin1String("merge"))
                QCOMPARE(output(git.run({"rev-list", "--parents", "-1", "HEAD"})).split(' ').size(),
                         3);
            return;
        }
        QVERIFY(!made.ok());
        QCOMPARE(QString::fromStdString(made.error().message()), error);
        QCOMPARE(output(git.run({"rev-parse", "--verify", "-q", "HEAD"})), head);
        QCOMPARE(repo.repo().state(), state);
    }

    // -----------------------------------------------------------------
    // A cherry-pick on top of an operation in progress. libgit2 writes
    // its CHERRY_PICK_HEAD and MERGE_MSG over the ones there, and when
    // the pick then fails (the earlier pick's changes are in the way)
    // deletes them: the merge, or the earlier pick, was gone, and its
    // commit went in as an ordinary one. Refused, before anything is
    // written, as git refuses it.
    // -----------------------------------------------------------------
    void cherryPickRefusedWhileAnOperationIsInProgress_data() {
        QTest::addColumn<QString>("operation");
        QTest::newRow("cherry-pick") << QStringLiteral("cherry-pick");
        QTest::newRow("merge") << QStringLiteral("merge");
    }

    void cherryPickRefusedWhileAnOperationIsInProgress() {
        QFETCH(QString, operation);
        const bool merge = operation == QLatin1String("merge");
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "base\n", "base").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        repo.writeFile("a.txt", "side\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "pick A",
                                   "--author=Team Mate <mate@example.com>"})));
        auto a = repo.repo().resolveRef("HEAD");
        repo.writeFile("b.txt", "b\n");
        QVERIFY(succeeded(git.run({"add", "b.txt"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "-m", "pick B"})));
        auto b = repo.repo().resolveRef("HEAD");
        QVERIFY(a.ok() && b.ok());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        if (merge) {
            // Up to A: B's b.txt stays out of it.
            repo.writeFile("a.txt", "main\n");
            QVERIFY(succeeded(git.run({"commit", "-q", "-am", "main"})));
            QVERIFY(!succeeded(git.run({"merge", "side~1"})));
        } else {
            QVERIFY(repo.writeAndCommit("c.txt", "c\n", "main").ok());
            auto picked = repo.repo().cherryPick(*a);
            QVERIFY(picked.ok() && !picked->hasConflicts);
        }
        const auto state = repo.repo().state();
        QCOMPARE(state, merge ? gitbolt::git::RepoState::Merge
                              : gitbolt::git::RepoState::CherryPick);
        const QString gitDir = QString::fromStdString(repo.repo().path());
        const auto stateFiles = [&] {
            QStringList files;
            for (const char* name : {"MERGE_HEAD", "CHERRY_PICK_HEAD", "MERGE_MSG"})
                files << QLatin1String(name) + QLatin1Char(' ')
                             + QString::fromUtf8(readFile(gitDir, QLatin1String(name)));
            return files;
        };
        const QStringList before = stateFiles();
        QVERIFY(!merge || before.at(0) != QStringLiteral("MERGE_HEAD <missing>"));
        QVERIFY(merge || before.at(1).startsWith(QStringLiteral("CHERRY_PICK_HEAD ")
                                                 + QString::fromStdString(a->toHex())));
        QVERIFY(merge || before.at(2).startsWith(QStringLiteral("MERGE_MSG pick A")));

        auto refused = repo.repo().cherryPick(*b);
        QVERIFY(!refused.ok());
        QCOMPARE(repo.repo().state(), state);
        QCOMPARE(stateFiles(), before);
        QCOMPARE(readFile(repo.path(), QStringLiteral("b.txt")), QByteArray("<missing>"));
        QCOMPARE(QString::fromStdString(refused.error().message()),
                 merge ? QStringLiteral("A merge is in progress. Finish or abort it first.")
                       : QStringLiteral("A cherry-pick is in progress. Finish or abort it first."));

        // The earlier pick still concludes as itself.
        if (!merge) {
            QVERIFY(repo.commit("pick A").ok());
            QCOMPARE(output(git.run({"log", "-1", "--format=%an <%ae>"})),
                     QStringLiteral("Team Mate <mate@example.com>"));
        }
    }

    // -----------------------------------------------------------------
    // `git cherry-pick A B C` (or a revert of several commits) that
    // stopped at B: once B is committed, the sequence is paused between
    // commits, with C still to come. libgit2 calls that no operation at
    // all, as it only looks for CHERRY_PICK_HEAD / REVERT_HEAD, so the
    // in-progress bar went away while `git status` still said
    // "cherry-pick in progress".
    // -----------------------------------------------------------------
    void pausedSequenceIsStillInProgress_data() {
        QTest::addColumn<bool>("revert");
        QTest::newRow("cherry-pick") << false;
        QTest::newRow("revert") << true;
    }

    void pausedSequenceIsStillInProgress() {
        QFETCH(bool, revert);
        const auto operation = revert ? gitbolt::git::RepoState::Revert
                                      : gitbolt::git::RepoState::CherryPick;
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "base\n", "base").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        if (revert) {
            // Reverting "q" clashes with "later".
            QVERIFY(repo.writeAndCommit("p.txt", "p\n", "p").ok());
            QVERIFY(repo.writeAndCommit("a.txt", "q\n", "q").ok());
            QVERIFY(repo.writeAndCommit("r.txt", "r\n", "r").ok());
            QVERIFY(repo.writeAndCommit("a.txt", "later\n", "later").ok());
            QVERIFY(!succeeded(git.run({"revert", "--no-edit", "HEAD~1", "HEAD~2", "HEAD~3"})));
        } else {
            QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
            QVERIFY(repo.writeAndCommit("x.txt", "x\n", "A").ok());
            QVERIFY(repo.writeAndCommit("a.txt", "side\n", "B").ok());
            QVERIFY(repo.writeAndCommit("c.txt", "c\n", "C").ok());
            QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
            QVERIFY(repo.writeAndCommit("a.txt", "main\n", "main").ok());
            QVERIFY(!succeeded(git.run({"cherry-pick", "side~2", "side~1", "side"})));
        }
        // Stopped at the second commit, on a conflict.
        QCOMPARE(repo.repo().state(), operation);
        QVERIFY(!repo.repo().sequencePaused());

        repo.writeFile("a.txt", "resolved\n");
        QVERIFY(repo.stageFile("a.txt").ok());
        QVERIFY(repo.commit("second").ok());
        QCOMPARE(repo.repo().state(), operation);
        QVERIFY(repo.repo().sequencePaused());

        // git carries on from there, and is done.
        QVERIFY(succeeded(git.run({revert ? "revert" : "cherry-pick", "--continue"})));
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::None);
        QVERIFY(!repo.repo().sequencePaused());
        QCOMPARE(output(git.run({"log", "-3", "--format=%s"})).split('\n').at(1),
                 QStringLiteral("second"));
    }

    // -----------------------------------------------------------------
    // sequenceSkippable() says what git's --skip will do. At the stop
    // (CHERRY_PICK_HEAD / REVERT_HEAD) it skips; paused after a reset
    // there, it skips; paused because the stopped commit was committed,
    // HEAD has moved past sequencer/abort-safety and git refuses
    // ("there is nothing to skip"), which the bar's Skip used to run
    // into every time.
    // -----------------------------------------------------------------
    void sequenceSkippableAsGitSkips_data() {
        QTest::addColumn<bool>("revert");
        QTest::addColumn<QString>("then");
        QTest::addColumn<bool>("skippable");
        for (const bool revert : {false, true}) {
            const char* op = revert ? "revert" : "cherry-pick";
            QTest::addRow("%s at the stop", op) << revert << "stop" << true;
            QTest::addRow("%s committed", op) << revert << "commit" << false;
            QTest::addRow("%s reset at the stop", op) << revert << "reset" << true;
        }
    }

    void sequenceSkippableAsGitSkips() {
        QFETCH(bool, revert);
        QFETCH(QString, then);
        QFETCH(bool, skippable);
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "base\n", "base").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        const char* op = revert ? "revert" : "cherry-pick";
        if (revert) {
            QVERIFY(repo.writeAndCommit("p.txt", "p\n", "p").ok());
            QVERIFY(repo.writeAndCommit("a.txt", "q\n", "q").ok());
            QVERIFY(repo.writeAndCommit("r.txt", "r\n", "r").ok());
            QVERIFY(repo.writeAndCommit("a.txt", "later\n", "later").ok());
            QVERIFY(!succeeded(git.run({"revert", "--no-edit", "HEAD~1", "HEAD~2", "HEAD~3"})));
        } else {
            QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
            QVERIFY(repo.writeAndCommit("x.txt", "x\n", "A").ok());
            QVERIFY(repo.writeAndCommit("a.txt", "side\n", "B").ok());
            QVERIFY(repo.writeAndCommit("c.txt", "c\n", "C").ok());
            QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
            QVERIFY(repo.writeAndCommit("a.txt", "main\n", "main").ok());
            QVERIFY(!succeeded(git.run({"cherry-pick", "side~2", "side~1", "side"})));
        }
        QVERIFY(!repo.repo().sequencePaused());
        QVERIFY(repo.repo().sequenceSkippable());
        if (then == QLatin1String("commit")) {
            repo.writeFile("a.txt", "resolved\n");
            QVERIFY(succeeded(git.run({"add", "a.txt"})));
            QVERIFY(succeeded(git.run({"commit", "-q", "--no-edit"})));
            QVERIFY(repo.repo().sequencePaused());
        } else if (then == QLatin1String("reset")) {
            QVERIFY(succeeded(git.run({"reset", "-q", "--hard"})));
            QVERIFY(repo.repo().sequencePaused());
        }
        QCOMPARE(repo.repo().sequenceSkippable(), skippable);

        const auto skipped = git.run({op, "--skip"});
        QVERIFY(skipped.ok());
        QVERIFY2(skipped->success() == skippable, skipped->stderrData.c_str());

        // Nothing to skip once the sequence is over, nor without one.
        if (skippable) {
            QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::None);
            QVERIFY(!repo.repo().sequenceSkippable());
        }
    }

    // The message the Commit dialog starts from where a merge, a
    // cherry-pick or a revert waits for its commit (MERGE_MSG):
    //   - a pick's is the picked commit's own, line for line, as a clean
    //     `git cherry-pick` commits it. Every line that started with
    //     '#' went: a "#42 Fix login" subject, a "# Notes" heading;
    //   - only git's (or libgit2's) note of the conflicts at the end
    //     goes, and a scissors line with it, with the comment prefix
    //     git wrote it with: core.commentChar, or core.commentString
    //     (git 2.45 and later take either, and a string), which was
    //     never read, so its "// Conflicts:" lines were committed;
    //   - a merge's and a revert's are git's own, and their comment
    //     lines (a signed tag's "gpg:" lines) are git's notes: they go,
    //     as a terminal's `git commit` drops them; where git's note of
    //     the conflicts is there, only the lines with its prefix, and
    //     where it isn't, the settings' (a '#' line stays with only
    //     core.commentString set, as git 2.45 and later keep it);
    //   - a message written with CRLF line ends comes without the CRs.
    void preparedMessageIsGitsWithoutItsNotes_data() {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<QStringList>("config");
        QTest::addColumn<QByteArray>("written");    // over git's MERGE_MSG, when set
        QTest::addColumn<QString>("expected");

        const QByteArray picked = "#42 Fix login\n\nDetails of the fix.\n\n"
                                  "# Notes\nKeep the session cookie.";
        const QString message = QString::fromUtf8(picked);
        const QStringList none;
        QTest::newRow("GitBolt's pick") << QStringLiteral("pick") << none << QByteArray()
                                        << message;
        QTest::newRow("GitBolt's pick, conflicted (#Conflicts:)")
            << QStringLiteral("pick conflict") << none << QByteArray() << message;
        QTest::newRow("git's pick, conflicted (# Conflicts:)")
            << QStringLiteral("git pick conflict") << none << QByteArray() << message;
        QTest::newRow("git's pick, conflicted, core.commentChar ;")
            << QStringLiteral("git pick conflict") << QStringList{QStringLiteral("core.commentChar=;")}
            << QByteArray() << message;
        QTest::newRow("git's pick, conflicted, the note after a scissors line")
            << QStringLiteral("git pick conflict") << none
            << picked + "\n\n# ------------------------ >8 ------------------------\n"
                        "# Do not modify or remove the line above.\n"
                        "# Everything below it will be ignored.\n#\n# Conflicts:\n#\ta.txt\n"
            << message;
        // What git 2.45 and later write (the git here may be older).
        QTest::newRow("core.commentString //")
            << QStringLiteral("git pick conflict")
            << QStringList{QStringLiteral("core.commentString=//")}
            << QByteArray("side change\n\n// Conflicts:\n//\ta.txt\n")
            << QStringLiteral("side change");
        QTest::newRow("core.commentChar ; and core.commentString //, as git's docs advise")
            << QStringLiteral("git pick conflict")
            << QStringList{QStringLiteral("core.commentChar=;"),
                           QStringLiteral("core.commentString=//")}
            << QByteArray("side change\n\n// Conflicts:\n//\ta.txt\n")
            << QStringLiteral("side change");
        QTest::newRow("CRLF line ends")
            << QStringLiteral("git pick conflict") << none
            << QByteArray("Fix\r\n\r\nBody line\r\n\r\n# Conflicts:\r\n#\ta.txt\r\n")
            << QStringLiteral("Fix\n\nBody line");
        QTest::newRow("git's merge, conflicted") << QStringLiteral("git merge conflict") << none
                                                 << QByteArray()
                                                 << QStringLiteral("Merge branch 'side'");
        QTest::newRow("a signed tag's merge: git's gpg lines")
            << QStringLiteral("git merge conflict") << none
            << QByteArray("Merge tag 'v1'\n\nRelease one\n\n# gpg: Signature made today\n"
                          "# gpg: Good signature from \"Someone\"\n\n# Conflicts:\n#\ta.txt\n")
            << QStringLiteral("Merge tag 'v1'\n\nRelease one");
        QTest::newRow("a merge, core.commentString //")
            << QStringLiteral("git merge conflict")
            << QStringList{QStringLiteral("core.commentString=//")}
            << QByteArray("Merge branch 'side'\n\n// Conflicts:\n//\ta.txt\n")
            << QStringLiteral("Merge branch 'side'");
        QTest::newRow("git's revert, conflicted")
            << QStringLiteral("git revert conflict") << none << QByteArray()
            << QStringLiteral("Revert \"main change\"\n\nThis reverts commit %1.");
        // '#' is what a git older than 2.45 writes with only
        // core.commentString set (it reads core.commentChar alone), and
        // with core.commentChar "auto" in any case (git's match of it
        // ignores case); "auto" keeps a character set before it.
        const QByteArray reverted = "# *** SAY WHY WE ARE REVERTING ON THE TITLE LINE ***\n\n"
                                    "This reverts commit %1.\n\n# Conflicts:\n#\ta.txt\n";
        QTest::newRow("revert.reference's placeholder, and the blank line under it")
            << QStringLiteral("git revert conflict") << none << reverted
            << QStringLiteral("This reverts commit %1.");
        QTest::newRow("revert.reference's placeholder, core.commentString // (git < 2.45)")
            << QStringLiteral("git revert conflict")
            << QStringList{QStringLiteral("core.commentString=//")} << reverted
            << QStringLiteral("This reverts commit %1.");
        QTest::newRow("revert.reference's placeholder, core.commentChar Auto")
            << QStringLiteral("git revert conflict")
            << QStringList{QStringLiteral("core.commentChar=Auto")} << reverted
            << QStringLiteral("This reverts commit %1.");
        QTest::newRow("a signed tag's merge, core.commentString // (git < 2.45)")
            << QStringLiteral("git merge conflict")
            << QStringList{QStringLiteral("core.commentString=//")}
            << QByteArray("Merge tag 'v1'\n\nRelease one\n\n# gpg: Signature made today\n"
                          "# gpg: Good signature from \"Someone\"\n\n# Conflicts:\n#\ta.txt\n")
            << QStringLiteral("Merge tag 'v1'\n\nRelease one");
        QTest::newRow("a merge, core.commentChar ; then auto: ; stays")
            << QStringLiteral("git merge conflict")
            << QStringList{QStringLiteral("core.commentChar=;"),
                           QStringLiteral("+core.commentChar=AUTO")}
            << QByteArray("Merge branch 'side'\n\n; Conflicts:\n;\ta.txt\n")
            << QStringLiteral("Merge branch 'side'");
        // git's note says which prefix it wrote its comments with: a
        // line with another is the message's own, as that git's
        // `git commit` keeps it.
        QTest::newRow("a merge, core.commentString ; and git's '; Conflicts:': #123 stays")
            << QStringLiteral("git merge conflict")
            << QStringList{QStringLiteral("core.commentString=;")}
            << QByteArray("Merge branch 'side'\n\n#123 closes the login bug\n\n"
                          "; Conflicts:\n;\ta.txt\n")
            << QStringLiteral("Merge branch 'side'\n\n#123 closes the login bug");
        QTest::newRow("a merge, core.commentChar ; then #, and git's '# Conflicts:': ;; stays")
            << QStringLiteral("git merge conflict")
            << QStringList{QStringLiteral("core.commentChar=;"),
                           QStringLiteral("+core.commentChar=#")}
            << QByteArray("Merge branch 'side'\n\n;; keep me\n\n# Conflicts:\n#\ta.txt\n")
            << QStringLiteral("Merge branch 'side'\n\n;; keep me");
        // With no note (a merge stopped by --no-commit or by a hook),
        // the settings' prefixes, '#' with none set. core.commentString
        // is a setting of git 2.45 and later, whose `git commit` keeps
        // a '#' line; an older git's '#' lines then show, to be deleted.
        const QByteArray noNote = "Merge branch 'side'\n\n#123 closes the login bug\n";
        QTest::newRow("a merge with no note: #123 goes, as git's `git commit` drops it")
            << QStringLiteral("git merge conflict") << none << noNote
            << QStringLiteral("Merge branch 'side'");
        QTest::newRow("a merge with no note, core.commentString ;: #123 stays")
            << QStringLiteral("git merge conflict")
            << QStringList{QStringLiteral("core.commentString=;")}
            << noNote + "\n; gpg: Signature made today\n"
            << QStringLiteral("Merge branch 'side'\n\n#123 closes the login bug");
        QTest::newRow("a merge with no note, core.commentString // (git < 2.45): # lines stay")
            << QStringLiteral("git merge conflict")
            << QStringList{QStringLiteral("core.commentString=//")}
            << QByteArray("Merge tag 'v1'\n\nRelease one\n\n# gpg: Signature made today\n")
            << QStringLiteral("Merge tag 'v1'\n\nRelease one\n\n# gpg: Signature made today");
    }

    void preparedMessageIsGitsWithoutItsNotes() {
        QFETCH(QString, operation);
        QFETCH(QStringList, config);
        QFETCH(QByteArray, written);
        QFETCH(QString, expected);
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "base\n", "base").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        const std::string picked = "#42 Fix login\n\nDetails of the fix.\n\n"
                                   "# Notes\nKeep the session cookie.\n";
        // side: a commit of b.txt alone, and one of a.txt that clashes
        // with main's, both with the message above, kept as it is.
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        repo.writeFile("b.txt", "b\n");
        QVERIFY(succeeded(git.run({"add", "b.txt"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "--cleanup=verbatim", "-m", picked})));
        const QString clean = output(git.run({"rev-parse", "HEAD"}));
        repo.writeFile("a.txt", "side\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-a", "--cleanup=verbatim", "-m", picked})));
        const QString clash = output(git.run({"rev-parse", "HEAD"}));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        repo.writeFile("a.txt", "main\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "main change"})));
        const QString mainChange = output(git.run({"rev-parse", "HEAD"}));
        // "+key=value" adds a value after the key's others.
        for (QString setting : config) {
            std::vector<std::string> args{"config"};
            if (setting.startsWith(QLatin1Char('+'))) {
                args.push_back("--add");
                setting.remove(0, 1);
            }
            args.push_back(setting.section(QLatin1Char('='), 0, 0).toStdString());
            args.push_back(setting.section(QLatin1Char('='), 1).toStdString());
            QVERIFY(succeeded(git.run(args)));
        }

        // A Repository of its own, as GitBolt opens one: the fixture's
        // copy of the index is from before git's checkouts.
        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        gitbolt::git::Repository& own = *opened;
        if (operation.startsWith(QLatin1String("pick"))) {
            const bool conflict = operation == QLatin1String("pick conflict");
            const auto pick = own.cherryPick(
                gitbolt::git::ObjectId::fromHex((conflict ? clash : clean).toStdString()));
            QVERIFY2(pick.ok(), pick.ok() ? "" : pick.error().message().c_str());
            QCOMPARE(pick->hasConflicts, conflict);
        } else if (operation == QLatin1String("git pick conflict")) {
            QVERIFY(!succeeded(git.run({"cherry-pick", clash.toStdString()})));
        } else if (operation == QLatin1String("git merge conflict")) {
            QVERIFY(!succeeded(git.run({"merge", "side"})));
        } else {
            repo.writeFile("a.txt", "later\n");
            QVERIFY(succeeded(git.run({"commit", "-q", "-am", "later change"})));
            QVERIFY(!succeeded(git.run({"revert", "--no-edit", mainChange.toStdString()})));
            expected = expected.arg(mainChange);
            written.replace("%1", mainChange.toUtf8());
        }
        QFile prepared(repo.path() + QStringLiteral("/.git/MERGE_MSG"));
        if (!written.isEmpty()) {
            QVERIFY(prepared.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QVERIFY(prepared.write(written) == written.size());
            prepared.close();
        } else if (operation.contains(QLatin1String("conflict"))) {
            // git's note of the conflicts is there to go.
            QVERIFY(prepared.open(QIODevice::ReadOnly));
            QVERIFY(prepared.readAll().contains("Conflicts:\n"));
            prepared.close();
        }
        QCOMPARE(QString::fromStdString(own.preparedMessage()), expected);
    }

    // -----------------------------------------------------------------
    // A config file that changed and can't be read again is no config
    // entry. libgit2's get_entry() took the backend's error for a found
    // entry and handed back an uninitialized pointer, which the lookups
    // that ignore errors (rename detection's, among others) then read
    // and freed through: test_git_service crashed so on Windows, where
    // .git/config can't be read while another handle replaces it. Here
    // the reread fails because the file is unreadable, and the first
    // lookups after the change are rename detection's. Once readable
    // again, the change must be seen: the failed reread used to leave
    // the handle on the old config for good. The fixes are patches to
    // the bundled libgit2 (cmake/PatchLibgit2.cmake).
    // -----------------------------------------------------------------
    void unreadableChangedConfigIsNoEntry() {
#if defined(Q_OS_WIN)
        QSKIP("needs Unix file permissions");
#elif !defined(GITBOLT_LIBGIT2_CONFIG_PATCHED)
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's config fixes");
#else
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        repo.writeFile("a.txt", "changed\n");
        // Read once, so the handle holds .git/config and its stamp.
        QVERIFY(repo.repo().status().ok());
        git_diff* diff = nullptr;
        QCOMPARE(git_diff_index_to_workdir(&diff, repo.repo().raw(), nullptr, nullptr), 0);
        const auto freeDiff = qScopeGuard([&] { git_diff_free(diff); });

        const QString config = repo.path() + QStringLiteral("/.git/config");
        {
            QFile file(config);
            QVERIFY(file.open(QIODevice::Append));
            QVERIFY(file.write("[diff]\n\trenames = copies\n\trenameLimit = 7\n") > 0);
        }
        const QFileDevice::Permissions readable = QFile::permissions(config);
        QVERIFY(QFile::setPermissions(config, QFileDevice::Permissions()));
        const auto unlock = qScopeGuard([&] { QFile::setPermissions(config, readable); });
        if (QFile(config).open(QIODevice::ReadOnly))
            QSKIP("file permissions don't stop this user (root?)");

        // diff.renames and diff.renameLimit, read ignoring errors: the
        // defaults, as if unset.
        QCOMPARE(git_diff_find_similar(diff, nullptr), 0);
        QVERIFY(QFile::setPermissions(config, readable));
        // Readable again, the same handle sees the change.
        git_config* live = nullptr;
        QCOMPARE(git_repository_config(&live, repo.repo().raw()), 0);
        const auto freeConfig = qScopeGuard([&] { git_config_free(live); });
        int32_t renameLimit = 0;
        QCOMPARE(git_config_get_int32(&renameLimit, live, "diff.renameLimit"), 0);
        QCOMPARE(renameLimit, 7);
        QVERIFY(repo.repo().status().ok());
        QCOMPARE(statusLines(repo.repo()), QStringList{QStringLiteral("a.txt changed")});
#endif
    }

    // The same with a file that .git/config includes: a failed read of
    // it left the handle on the old config for good, both when the
    // included file changed and when .git/config did (twice looked up
    // while the included file couldn't be read).
    void unreadableIncludedConfigIsReadAgain() {
#if defined(Q_OS_WIN)
        QSKIP("needs Unix file permissions");
#elif !defined(GITBOLT_LIBGIT2_CONFIG_PATCHED)
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's config fixes");
#else
        gitbolt::test::TestRepo repo;
        repo.writeFile(".git/extra.config", "[test]\n\tvalue = 3\n");
        repo.appendFile(".git/config", "[include]\n\tpath = extra.config\n");
        git_config* live = nullptr;
        QCOMPARE(git_repository_config(&live, repo.repo().raw()), 0);
        const auto freeConfig = qScopeGuard([&] { git_config_free(live); });
        int32_t value = 0;
        QCOMPARE(git_config_get_int32(&value, live, "test.value"), 0);
        QCOMPARE(value, 3);

        const QString included = repo.path() + QStringLiteral("/.git/extra.config");
        const QFileDevice::Permissions readable = QFile::permissions(included);
        const auto unlock = qScopeGuard([&] { QFile::setPermissions(included, readable); });
        // Another size, so the change shows in the stamp whatever the
        // file system's time resolution.
        repo.writeFile(".git/extra.config", "[test]\n\tvalue = 77\n");
        QVERIFY(QFile::setPermissions(included, QFileDevice::Permissions()));
        if (QFile(included).open(QIODevice::ReadOnly))
            QSKIP("file permissions don't stop this user (root?)");
        git_config_get_int32(&value, live, "test.value");
        QVERIFY(QFile::setPermissions(included, readable));
        QCOMPARE(git_config_get_int32(&value, live, "test.value"), 0);
        QCOMPARE(value, 77);

        QVERIFY(QFile::setPermissions(included, QFileDevice::Permissions()));
        repo.appendFile(".git/config", "[test]\n\tother = 9\n");
        git_config_get_int32(&value, live, "test.other");
        git_config_get_int32(&value, live, "test.other");
        QVERIFY(QFile::setPermissions(included, readable));
        QCOMPARE(git_config_get_int32(&value, live, "test.other"), 0);
        QCOMPARE(value, 9);
#endif
    }

    // -----------------------------------------------------------------
    // Edit URL changes the URL and nothing else, as `git remote set-url`
    // does. It used to remove the remote and add it back, which also
    // dropped every branch's upstream link to it (Pull then failed:
    // "did not specify a branch"), its remote-tracking branches, and its
    // push URL, options and extra refspecs.
    // -----------------------------------------------------------------
    void setRemoteUrlKeepsEverythingElse() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        auto& r = repo.repo();
        QVERIFY(r.addRemote("origin", "https://example.com/old/repo.git").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const std::string head = output(git.run({"rev-parse", "HEAD"})).toStdString();
        const std::string branch = output(git.run({"branch", "--show-current"})).toStdString();
        QVERIFY(succeeded(git.run({"update-ref", "refs/remotes/origin/" + branch, head})));
        QVERIFY(succeeded(git.run({"branch", "--set-upstream-to=origin/" + branch, branch})));
        QVERIFY(succeeded(git.run({"config", "remote.origin.pushurl",
                                   "https://example.com/push/repo.git"})));
        QVERIFY(succeeded(git.run({"config", "remote.origin.prune", "true"})));
        QVERIFY(succeeded(git.run({"config", "--add", "remote.origin.fetch",
                                   "+refs/notes/*:refs/notes/*"})));

        const auto ok = r.setRemoteUrl("origin", "https://example.com/new/repo.git");
        QVERIFY2(ok.ok(), ok.ok() ? "" : ok.error().message().c_str());

        QCOMPARE(output(git.run({"config", "remote.origin.url"})),
                 QStringLiteral("https://example.com/new/repo.git"));
        QCOMPARE(output(git.run({"config", "remote.origin.pushurl"})),
                 QStringLiteral("https://example.com/push/repo.git"));
        QCOMPARE(output(git.run({"config", "remote.origin.prune"})), QStringLiteral("true"));
        QCOMPARE(output(git.run({"config", "--get-all", "remote.origin.fetch"})),
                 QStringLiteral("+refs/heads/*:refs/remotes/origin/*\n+refs/notes/*:refs/notes/*"));
        QCOMPARE(output(git.run({"config", "branch." + branch + ".remote"})), QStringLiteral("origin"));
        QCOMPARE(output(git.run({"config", "branch." + branch + ".merge"})),
                 QString::fromStdString("refs/heads/" + branch));
        QCOMPARE(output(git.run({"rev-parse", "refs/remotes/origin/" + branch})),
                 QString::fromStdString(head));

        // An unknown remote is refused, and none is created.
        QVERIFY(!r.setRemoteUrl("nope", "https://example.com/x.git").ok());
        QVERIFY(!succeeded(git.run({"config", "remote.nope.url"})));
    }

    // A remote this repository doesn't define itself, from a file its
    // config includes or from the global config, is refused, as `git
    // remote set-url` refuses it: writing .git/config gave it a second
    // URL, and git then fetched from the old one and pushed to both.
    void setRemoteUrlRefusesRemotesDefinedElsewhere() {
        // From an included file.
        {
            gitbolt::test::TestRepo repo;
            QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
            repo.writeFile(".git/extra.config", "[remote \"inc\"]\n\turl = https://example.com/inc.git\n");
            gitbolt::git::GitProcess git(repo.path().toStdString());
            QVERIFY(succeeded(git.run({"config", "include.path", "extra.config"})));
            const QByteArray before = readFile(repo.path(), QStringLiteral(".git/config"));
            const auto res = repo.repo().setRemoteUrl("inc", "https://example.com/new.git");
            QVERIFY(!res.ok());
            QVERIFY2(QString::fromStdString(res.error().message()).contains(QLatin1String("includes")),
                     res.error().message().c_str());
            QCOMPARE(readFile(repo.path(), QStringLiteral(".git/config")), before);
        }
        // From the global config: libgit2's global search path points at
        // a config of the test's own for the length of this block.
        {
            git_buf saved = GIT_BUF_INIT;
            QCOMPARE(git_libgit2_opts(GIT_OPT_GET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, &saved), 0);
            const QByteArray savedPath(saved.ptr ? saved.ptr : "", static_cast<int>(saved.size));
            git_buf_dispose(&saved);
            const auto restore = qScopeGuard([&] {
                git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, savedPath.constData());
            });
            QTemporaryDir home;
            QVERIFY(home.isValid());
            QFile global(home.filePath(QStringLiteral(".gitconfig")));
            QVERIFY(global.open(QIODevice::WriteOnly));
            global.write("[remote \"glob\"]\n\turl = https://example.com/glob.git\n");
            global.close();
            QCOMPARE(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL,
                                      QFile::encodeName(home.path()).constData()), 0);

            gitbolt::test::TestRepo repo;
            QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
            auto remotes = repo.repo().remotes();
            QVERIFY(remotes.ok());
            QVERIFY(std::any_of(remotes->begin(), remotes->end(),
                                [](const auto& r) { return r.name == "glob"; }));
            const QByteArray before = readFile(repo.path(), QStringLiteral(".git/config"));
            const auto res = repo.repo().setRemoteUrl("glob", "https://example.com/new.git");
            QVERIFY(!res.ok());
            QVERIFY2(QString::fromStdString(res.error().message()).contains(QLatin1String("global")),
                     res.error().message().c_str());
            QCOMPARE(readFile(repo.path(), QStringLiteral(".git/config")), before);
        }
        // From a file the global config includes: the global config is
        // where to look, not the repository's (libgit2 gives the entry
        // the global level and an include depth).
        {
            git_buf saved = GIT_BUF_INIT;
            QCOMPARE(git_libgit2_opts(GIT_OPT_GET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, &saved), 0);
            const QByteArray savedPath(saved.ptr ? saved.ptr : "", static_cast<int>(saved.size));
            git_buf_dispose(&saved);
            const auto restore = qScopeGuard([&] {
                git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, savedPath.constData());
            });
            QTemporaryDir home;
            QVERIFY(home.isValid());
            QFile shared(home.filePath(QStringLiteral("shared.gitconfig")));
            QVERIFY(shared.open(QIODevice::WriteOnly));
            shared.write("[remote \"shared\"]\n\turl = https://example.com/shared.git\n");
            shared.close();
            QFile global(home.filePath(QStringLiteral(".gitconfig")));
            QVERIFY(global.open(QIODevice::WriteOnly));
            global.write("[include]\n\tpath = shared.gitconfig\n");
            global.close();
            QCOMPARE(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL,
                                      QFile::encodeName(home.path()).constData()), 0);

            gitbolt::test::TestRepo repo;
            QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
            const QByteArray before = readFile(repo.path(), QStringLiteral(".git/config"));
            const auto res = repo.repo().setRemoteUrl("shared", "https://example.com/new.git");
            QVERIFY(!res.ok());
            QVERIFY2(QString::fromStdString(res.error().message())
                         .contains(QLatin1String("a file that your global git config includes")),
                     res.error().message().c_str());
            QCOMPARE(readFile(repo.path(), QStringLiteral(".git/config")), before);
        }
    }

    // A remote in a linked worktree's own config.worktree: libgit2
    // writes the URL to the repository's config, where every other
    // worktree gets it, while the worktree's own entry wins on there.
    void setRemoteUrlRefusesAWorktreesOwnRemote() {
        gitbolt::test::TestRepo repo;
        QVERIFY(!addFeatureBranch(repo).isEmpty());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QTemporaryDir worktreeParent;
        QVERIFY(worktreeParent.isValid());
        const QString worktree = worktreeParent.filePath(QStringLiteral("wt"));
        QVERIFY(succeeded(git.run({"worktree", "add", "-q", worktree.toStdString(), "feature"})));
        QVERIFY(succeeded(git.run({"config", "extensions.worktreeConfig", "true"})));
        gitbolt::git::GitProcess wtGit(worktree.toStdString());
        QVERIFY(succeeded(wtGit.run({"config", "--worktree", "remote.wtremote.url",
                                     "https://example.com/wt-old.git"})));
        QVERIFY(succeeded(wtGit.run({"config", "--worktree", "remote.wtremote.fetch",
                                     "+refs/heads/*:refs/remotes/wtremote/*"})));

        auto opened = gitbolt::git::Repository::open(worktree.toStdString());
        QVERIFY(opened.ok());
        const QByteArray before = readFile(repo.path(), QStringLiteral(".git/config"));
        const auto res = opened->setRemoteUrl("wtremote", "https://example.com/wt-new.git");
        QVERIFY(!res.ok());
        QVERIFY2(QString::fromStdString(res.error().message()).contains(QLatin1String("config.worktree")),
                 res.error().message().c_str());
        QCOMPARE(readFile(repo.path(), QStringLiteral(".git/config")), before);
    }

    // -----------------------------------------------------------------
    // Config lines with a name and no "= value": libgit2 gives them a
    // NULL value, and crashed where it wanted a string (patched in the
    // bundled libgit2, cmake/PatchLibgit2.cmake). git refuses such a
    // config; GitBolt must not go down on it. A .gitmodules is committed,
    // so a cloned repository can bring one.
    // -----------------------------------------------------------------
    // .gitmodules with a `path` line and no value (or `path =`), and the
    // submodule's gitlink and folder there, as a clone has them: libgit2
    // took the path as "", the superproject's own folder, and status
    // (which every open runs) recursed into it until the stack ran out.
    void valuelessSubmodulePathIsIgnored_data() {
        QTest::addColumn<QByteArray>("pathLine");
        QTest::newRow("path with no value") << QByteArray("\tpath\n");
        QTest::newRow("path =") << QByteArray("\tpath =\n");
    }

    void valuelessSubmodulePathIsIgnored() {
#ifndef GITBOLT_LIBGIT2_NULL_VALUES_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's fixes");
#else
        QFETCH(QByteArray, pathLine);
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(".gitmodules",
                                    "[submodule \"x\"]\n" + pathLine
                                        + "\turl = https://example.com/x.git\n",
                                    "a .gitmodules with an empty path").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const QString head = output(git.run({"rev-parse", "HEAD"}));
        QVERIFY(!head.isEmpty());
        QVERIFY(succeeded(git.run({"update-index", "--add", "--cacheinfo",
                                   ("160000," + head + ",x").toStdString()})));
        QVERIFY(succeeded(git.run({"-c", "user.name=T", "-c", "user.email=t@gitbolt.local",
                                   "-c", "commit.gpgsign=false", "commit", "-q", "-m", "gitlink"})));
        QVERIFY(QDir(repo.path()).mkpath(QStringLiteral("x")));   // as a clone leaves it

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        const auto subs = opened->submodules();
        QVERIFY(subs.ok());
        QCOMPARE(subs->size(), std::size_t(1));
        QCOMPARE(QString::fromStdString(subs->front().path), QStringLiteral("x"));
        QVERIFY(opened->status().ok());
        QVERIFY(opened->diffIndexToWorkdir().ok());
#endif
    }

    // A bare repository has no submodules checked out, and libgit2
    // turns the listing down: no submodules, rather than a failure that
    // would be said at every open of one.
    void aBareRepositoryListsNoSubmodules() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto bare = gitbolt::git::Repository::init(dir.path().toStdString(), /*bare=*/true);
        QVERIFY(bare.ok());
        const auto subs = bare->submodules();
        QVERIFY2(subs.ok(), subs.ok() ? "" : subs.error().message().c_str());
        QVERIFY(subs->empty());
    }

    // A submodule whose folder is the superproject: `path = /`, or a
    // path through a committed symlink to `.`. Status opened the
    // superproject as its own submodule, over and over, until the stack
    // ran out; a clone of such a repository crashed GitBolt on opening.
    void submoduleThatIsItsSuperprojectIsNotOpened_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("path = /") << QStringLiteral("root");
#ifndef Q_OS_WIN
        QTest::newRow("a path through a symlink to .") << QStringLiteral("symlink");
#endif
    }

    void submoduleThatIsItsSuperprojectIsNotOpened() {
#ifndef GITBOLT_LIBGIT2_NULL_VALUES_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's fixes");
#else
        QFETCH(QString, kind);
        gitbolt::test::TestRepo repo;
        const QByteArray path = kind == QLatin1String("root") ? "/" : "l";
        repo.writeFile(".gitmodules", "[submodule \"x\"]\n\tpath = " + path
                                          + "\n\turl = https://example.com/x.git\n");
        if (kind == QLatin1String("symlink"))
            QVERIFY(QFile::link(QStringLiteral("."), QDir(repo.path()).filePath(QStringLiteral("l"))));
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"add", "-A"})));
        QVERIFY(succeeded(git.run({"-c", "user.name=T", "-c", "user.email=t@gitbolt.local",
                                   "-c", "commit.gpgsign=false", "commit", "-q", "-m", "modules"})));
        const QString head = output(git.run({"rev-parse", "HEAD"}));
        QVERIFY(succeeded(git.run({"update-index", "--add", "--cacheinfo",
                                   ("160000," + head + ",x").toStdString()})));
        QVERIFY(succeeded(git.run({"-c", "user.name=T", "-c", "user.email=t@gitbolt.local",
                                   "-c", "commit.gpgsign=false", "commit", "-q", "-m", "gitlink"})));
        QVERIFY(QDir(repo.path()).mkpath(QStringLiteral("x")));   // as a clone leaves it

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        QVERIFY(opened->status().ok());               // every open runs this
        QVERIFY(opened->diffIndexToWorkdir().ok());   // the Commit dialog's
#endif
    }

    // The same two levels down: submodule s (a clone of the repository
    // itself, url = ./) has a submodule x whose path is a committed
    // symlink to "..", which leads back to the superproject. s opened
    // the superproject, which opened s, and so on.
    void submoduleLoopingBackUpIsNotOpened() {
#if !defined(GITBOLT_LIBGIT2_NULL_VALUES_PATCHED)
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's fixes");
#elif defined(Q_OS_WIN)
        QSKIP("needs a symbolic link");
#else
        const char* const identity[] = {"-c", "user.name=T", "-c", "user.email=t@gitbolt.local",
                                        "-c", "commit.gpgsign=false"};
        auto commit = [&](gitbolt::git::GitProcess& git, const std::string& message) {
            std::vector<std::string> args(std::begin(identity), std::end(identity));
            for (const char* a : {"commit", "-q", "-m"})
                args.emplace_back(a);
            args.push_back(message);
            return succeeded(git.run(args));
        };
        // The commit s checks out: l -> "..", and x, a submodule at l.
        QTemporaryDir src;
        QVERIFY(src.isValid());
        gitbolt::git::GitProcess srcGit(src.path().toStdString());
        QVERIFY(succeeded(srcGit.run({"init", "-q"})));
        QFile first(QDir(src.path()).filePath(QStringLiteral("a.txt")));
        QVERIFY(first.open(QIODevice::WriteOnly));
        first.write("a\n");
        first.close();
        QVERIFY(succeeded(srcGit.run({"add", "a.txt"})));
        QVERIFY(commit(srcGit, "c0"));
        const QString c0 = output(srcGit.run({"rev-parse", "HEAD"}));
        QFile modules(QDir(src.path()).filePath(QStringLiteral(".gitmodules")));
        QVERIFY(modules.open(QIODevice::WriteOnly));
        modules.write("[submodule \"x\"]\n\tpath = l\n\turl = ./\n");
        modules.close();
        QVERIFY(QFile::link(QStringLiteral(".."), QDir(src.path()).filePath(QStringLiteral("l"))));
        QVERIFY(succeeded(srcGit.run({"add", "-A"})));
        QVERIFY(succeeded(srcGit.run({"update-index", "--add", "--cacheinfo",
                                      ("160000," + c0 + ",x").toStdString()})));
        QVERIFY(commit(srcGit, "loop"));
        const QString c1 = output(srcGit.run({"rev-parse", "HEAD"}));

        // The superproject, with s checked out at that commit.
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"clone", "-q", src.path().toStdString(), "s"})));
        repo.writeFile(".gitmodules", "[submodule \"s\"]\n\tpath = s\n\turl = ./\n");
        QVERIFY(succeeded(git.run({"add", ".gitmodules"})));
        QVERIFY(succeeded(git.run({"update-index", "--add", "--cacheinfo",
                                   ("160000," + c1 + ",s").toStdString()})));
        QVERIFY(commit(git, "s"));

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        QVERIFY(opened->status().ok());               // every open runs this
        QVERIFY(opened->diffIndexToWorkdir().ok());
#endif
    }

    // An absorbed submodule whose core.worktree names the superproject's
    // work tree (a hand edit gone wrong): the repository opened for it
    // has the superproject's work tree, and status read it again as
    // its own submodule, and so on.
    void submoduleWorkTreeElsewhereIsNotOpened() {
#ifndef GITBOLT_LIBGIT2_NULL_VALUES_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's fixes");
#else
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const QString c0 = output(git.run({"rev-parse", "HEAD"}));
        repo.writeFile(".gitmodules", "[submodule \"s\"]\n\tpath = s\n\turl = ./\n");
        QVERIFY(succeeded(git.run({"add", ".gitmodules"})));
        QVERIFY(succeeded(git.run({"update-index", "--add", "--cacheinfo",
                                   ("160000," + c0 + ",s").toStdString()})));
        QVERIFY(succeeded(git.run({"-c", "user.name=T", "-c", "user.email=t@gitbolt.local",
                                   "-c", "commit.gpgsign=false", "commit", "-q", "-m", "s"})));
        // s: a clone of the repository at that commit, its git dir
        // absorbed under .git/modules/s, its core.worktree the superproject's.
        QVERIFY(succeeded(git.run({"clone", "-q", repo.path().toStdString(), "s"})));
        const QDir top(repo.path());
        QVERIFY(top.mkpath(QStringLiteral(".git/modules")));
        QVERIFY(QDir().rename(top.filePath(QStringLiteral("s/.git")),
                              top.filePath(QStringLiteral(".git/modules/s"))));
        QFile gitFile(top.filePath(QStringLiteral("s/.git")));
        QVERIFY(gitFile.open(QIODevice::WriteOnly));
        gitFile.write("gitdir: ../.git/modules/s\n");
        gitFile.close();
        QVERIFY(succeeded(git.run({"--git-dir=.git/modules/s", "config", "core.worktree",
                                   "../../.."})));

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        QVERIFY(opened->status().ok());               // every open runs this
        QVERIFY(opened->diffIndexToWorkdir().ok());
#endif
    }

    // A submodule path through a symlink, even one to a folder inside
    // the repository: git won't follow it, and neither does GitBolt (on
    // Windows, where realpath doesn't resolve symlinks, it is what
    // catches a symlink to "." or "..").
    void submodulePathThroughASymlinkIsNotOpened() {
#if !defined(GITBOLT_LIBGIT2_NULL_VALUES_PATCHED)
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's fixes");
#elif defined(Q_OS_WIN)
        QSKIP("QFile::link makes a shortcut on Windows, not a link");
#else
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("inner/a.txt", "a\n", "a").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const QString c0 = output(git.run({"rev-parse", "HEAD"}));
        // inner/x: a real repository, with a commit of its own.
        QVERIFY(succeeded(git.run({"clone", "-q", repo.path().toStdString(), "inner/x"})));
        QVERIFY(QFile::link(QStringLiteral("inner"), QDir(repo.path()).filePath(QStringLiteral("l"))));
        repo.writeFile(".gitmodules", "[submodule \"x\"]\n\tpath = l/x\n\turl = ./\n");
        QVERIFY(succeeded(git.run({"add", ".gitmodules", "l"})));
        QVERIFY(succeeded(git.run({"update-index", "--add", "--cacheinfo",
                                   ("160000," + c0 + ",x").toStdString()})));
        QVERIFY(succeeded(git.run({"-c", "user.name=T", "-c", "user.email=t@gitbolt.local",
                                   "-c", "commit.gpgsign=false", "commit", "-q", "-m", "x"})));
        QVERIFY(QDir(repo.path()).mkpath(QStringLiteral("x")));

        git_repository* raw = nullptr;
        QCOMPARE(git_repository_open(&raw, repo.path().toUtf8().constData()), 0);
        git_submodule* sm = nullptr;
        QCOMPARE(git_submodule_lookup(&sm, raw, "x"), 0);
        git_repository* smRepo = nullptr;
        const int opened = git_submodule_open(&smRepo, sm);
        git_repository_free(smRepo);
        git_submodule_free(sm);
        git_repository_free(raw);
        QVERIFY2(opened < 0, "opened a submodule through a symlink");
#endif
    }

    // A linked worktree of the repository inside its work tree, tracked
    // as a submodule (a gh-pages checkout, say): a different working
    // tree, opened as any submodule. Clean, it is no change.
    void linkedWorktreeAsASubmoduleIsOpened() {
#ifndef GITBOLT_LIBGIT2_NULL_VALUES_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's fixes");
#else
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        // Git for Windows' system config has core.autocrlf on: the CLI
        // would check the worktree out with CRLF, which libgit2 (its
        // system config kept out, TestRepo) takes for a change.
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        QVERIFY(succeeded(git.run({"worktree", "add", "-q", "-b", "pages", "public"})));
        const QString head = output(git.run({"rev-parse", "HEAD"}));
        repo.writeFile(".gitmodules", "[submodule \"public\"]\n\tpath = public\n\turl = ./\n");
        QVERIFY(succeeded(git.run({"add", ".gitmodules"})));
        QVERIFY(succeeded(git.run({"update-index", "--add", "--cacheinfo",
                                   ("160000," + head + ",public").toStdString()})));
        QVERIFY(succeeded(git.run({"-c", "user.name=T", "-c", "user.email=t@gitbolt.local",
                                   "-c", "commit.gpgsign=false", "commit", "-q", "-m", "pages"})));

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        const auto status = opened->status();
        QVERIFY(status.ok());
        for (const auto& entry : *status)
            QVERIFY2(entry.path != "public", "the clean worktree shows as changed");
#endif
    }

    void valuelessInsteadOfIsIgnored() {
#ifndef GITBOLT_LIBGIT2_NULL_VALUES_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's fixes");
#else
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        appendConfig(repo.path(), "[remote \"origin\"]\n\turl = https://example.com/a.git\n"
                                  "[url \"https://z/\"]\n\tinsteadOf\n\tpushInsteadOf\n");
        if (QTest::currentTestFailed())
            return;
        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        const auto remotes = opened->remotes();   // Remote repositories' list
        QVERIFY(remotes.ok());
        QCOMPARE(remotes->size(), std::size_t(1));
        QCOMPARE(QString::fromStdString(remotes->front().url), QStringLiteral("https://example.com/a.git"));
        QVERIFY(opened->setRemoteUrl("origin", "https://example.com/b.git").ok());
#endif
    }

    void valuelessBranchRemoteLetsARemoteGo() {
#ifndef GITBOLT_LIBGIT2_NULL_VALUES_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's fixes");
#else
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        appendConfig(repo.path(), "[remote \"origin\"]\n\turl = https://example.com/a.git\n"
                                  "\tfetch = +refs/heads/*:refs/remotes/origin/*\n"
                                  "[branch \"main\"]\n\tremote\n");
        if (QTest::currentTestFailed())
            return;
        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        const auto removed = opened->removeRemote("origin");
        QVERIFY2(removed.ok(), removed.ok() ? "" : removed.error().message().c_str());
#endif
    }

    void valuelessObjectFormatFailsToOpen() {
#ifndef GITBOLT_LIBGIT2_NULL_VALUES_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's fixes");
#else
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        QVERIFY(QDir(repo.path()).mkpath(QStringLiteral("sub")));
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.repositoryformatversion", "1"})));
        appendConfig(repo.path(), "[extensions]\n\tobjectformat\n");
        if (QTest::currentTestFailed())
            return;
        const auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(!opened.ok());
        QVERIFY2(QString::fromStdString(opened.error().message()).contains(QLatin1String("objectformat")),
                 opened.error().message().c_str());
        std::string root;
        const auto fromSub = gitbolt::git::Repository::openContaining(
            QDir(repo.path()).filePath(QStringLiteral("sub")).toStdString(), "", root);
        QVERIFY(!fromSub.ok());
#endif
    }

    // A line with no value in a branch's or a remote's section (`rebase`,
    // `prune`, `mirror`: git booleans, true): rewriting the config
    // matched a pattern against the missing value, strlen(NULL). Delete
    // Branch, Remove Remote and Add Remote crashed, and left
    // .git/config.lock behind.
    void valuelessLinesLetBranchesAndRemotesChange() {
#ifndef GITBOLT_LIBGIT2_NULL_VALUES_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's fixes");
#else
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"branch", "feat"})));
        appendConfig(repo.path(), "[branch \"feat\"]\n\trebase\n"
                                  "[remote \"origin\"]\n\turl = https://example.com/a.git\n"
                                  "\tprune\n\tmirror\n"
                                  "[remote \"up\"]\n\tfetch\n");
        if (QTest::currentTestFailed())
            return;
        const QString lock = QDir(repo.path()).filePath(QStringLiteral(".git/config.lock"));

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        const auto deleted = opened->deleteBranch("feat");
        QVERIFY2(deleted.ok(), deleted.ok() ? "" : deleted.error().message().c_str());
        QVERIFY(!QFileInfo::exists(lock));
        QVERIFY(!succeeded(git.run({"config", "--get", "branch.feat.rebase"})));

        const auto removed = opened->removeRemote("origin");
        QVERIFY2(removed.ok(), removed.ok() ? "" : removed.error().message().c_str());
        QVERIFY(!QFileInfo::exists(lock));

        const auto added = opened->addRemote("up", "https://example.com/up.git");
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message().c_str());
        QVERIFY(!QFileInfo::exists(lock));
        QCOMPARE(output(git.run({"config", "--get", "remote.up.url"})),
                 QStringLiteral("https://example.com/up.git"));
#endif
    }

    // merge.conflictstyle with no value (in ~/.gitconfig: every
    // repository): every checkout crashed, a branch switch, a discard, a
    // cherry-pick. Each fails as git does now, and says why.
    void valuelessConflictStyleFailsCheckouts() {
#ifndef GITBOLT_LIBGIT2_NULL_VALUES_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's fixes");
#else
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        QVERIFY(repo.writeAndCommit("b.txt", "b\n", "b").ok());
        const QString picked = output(git.run({"rev-parse", "HEAD"}));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        repo.writeFile("a.txt", "changed\n");
        appendConfig(repo.path(), "[merge]\n\tconflictstyle\n");
        if (QTest::currentTestFailed())
            return;
        const auto says = [](const auto& result) {
            return !result.ok()
                && QString::fromStdString(result.error().message())
                       .contains(QLatin1String("merge.conflictstyle"));
        };

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        QVERIFY(says(opened->checkout("side")));
        QVERIFY(says(opened->discardWorkdirChanges("a.txt")));
        const auto id = opened->resolveRef(picked.toStdString());
        QVERIFY(id.ok());
        QVERIFY(says(opened->cherryPick(*id)));
        // Repository refuses these itself first; libgit2's own checkout
        // (anything else that calls it) fails too, rather than crash.
        git_checkout_options opts;
        git_checkout_options_init(&opts, GIT_CHECKOUT_OPTIONS_VERSION);
        opts.checkout_strategy = GIT_CHECKOUT_FORCE;
        QVERIFY(git_checkout_head(opened->raw(), &opts) < 0);
        QVERIFY(QString::fromUtf8(git_error_last()->message).contains(QLatin1String("merge.conflictstyle")));
#endif
    }

    // merge.default with no value: a cherry-pick (any libgit2 merge)
    // crashed. It fails as git does now.
    void valuelessMergeDefaultFailsACherryPick() {
#ifndef GITBOLT_LIBGIT2_NULL_VALUES_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's fixes");
#else
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        QVERIFY(repo.writeAndCommit("b.txt", "b\n", "b").ok());
        const QString picked = output(git.run({"rev-parse", "HEAD"}));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        appendConfig(repo.path(), "[merge]\n\tdefault\n");
        if (QTest::currentTestFailed())
            return;

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        const auto id = opened->resolveRef(picked.toStdString());
        QVERIFY(id.ok());
        const auto pick = opened->cherryPick(*id);
        QVERIFY(!pick.ok());
        QVERIFY2(QString::fromStdString(pick.error().message()).contains(QLatin1String("merge.default")),
                 pick.error().message().c_str());
        QVERIFY(!QFileInfo::exists(QDir(repo.path()).filePath(QStringLiteral(".git/index.lock"))));
        // And libgit2's own cherry-pick, behind Repository's refusal.
        git_commit* commit = nullptr;
        git_oid oid;
        QCOMPARE(git_oid_fromstr(&oid, picked.toLatin1().constData()), 0);
        QCOMPARE(git_commit_lookup(&commit, opened->raw(), &oid), 0);
        const int err = git_cherrypick(opened->raw(), commit, nullptr);
        git_commit_free(commit);
        QVERIFY(err < 0);
        QVERIFY(QString::fromUtf8(git_error_last()->message).contains(QLatin1String("merge.default")));
        QVERIFY(!QFileInfo::exists(QDir(repo.path()).filePath(QStringLiteral(".git/index.lock"))));
#endif
    }

    // With merge.conflictstyle given no value, stash save wrote the stash
    // and then failed to reset the work tree (it crashed, before
    // libgit2 was patched), and worktree add made the worktree's folder
    // and metadata, then failed to check it out. Both are refused now
    // before anything is written, with any libgit2.
    void valuelessConflictStyleWritesNothing() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        repo.writeFile("a.txt", "changed\n");
        appendConfig(repo.path(), "[merge]\n\tconflictstyle\n");
        if (QTest::currentTestFailed())
            return;
        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());

        const auto saved = opened->stashSave("wip");
        QVERIFY(!saved.ok());
        QVERIFY2(QString::fromStdString(saved.error().message()).contains(QLatin1String("merge.conflictstyle")),
                 saved.error().message().c_str());
        const auto stashes = opened->stashes();
        QVERIFY(stashes.ok());
        QVERIFY(stashes->empty());
        QFile file(QDir(repo.path()).filePath(QStringLiteral("a.txt")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("changed\n"));

        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString linked = scratch.filePath(QStringLiteral("linked"));
        const auto added = opened->addWorktree("linked", linked.toStdString(), "side", true);
        QVERIFY(!added.ok());
        QVERIFY(!QFileInfo::exists(linked));
        QVERIFY(!QFileInfo::exists(QDir(repo.path()).filePath(QStringLiteral(".git/worktrees/linked"))));
        QVERIFY(!succeeded(gitbolt::git::GitProcess(repo.path().toStdString())
                               .run({"rev-parse", "--verify", "-q", "refs/heads/side"})));
    }

    // A `url` line with no "= value" (a slip in a hand edit): libgit2
    // lists the remote with no URL, and gives the entry a null value.
    // Edit URL crashed on it; it now repairs the remote.
    void setRemoteUrlRepairsAValuelessUrl() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        QFile config(QDir(repo.path()).filePath(QStringLiteral(".git/config")));
        QVERIFY(config.open(QIODevice::Append));
        config.write("[remote \"origin\"]\n\turl\n");
        config.close();
        auto reopened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(reopened.ok());
        const auto res = reopened->setRemoteUrl("origin", "https://example.com/fixed.git");
        QVERIFY2(res.ok(), res.ok() ? "" : res.error().message().c_str());
        auto again = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(again.ok());
        auto remotes = again->remotes();
        QVERIFY(remotes.ok());
        const auto origin = std::find_if(remotes->begin(), remotes->end(),
                                         [](const auto& r) { return r.name == "origin"; });
        QVERIFY(origin != remotes->end());
        QCOMPARE(QString::fromStdString(origin->url), QStringLiteral("https://example.com/fixed.git"));
    }

    // A valueless `url` beside a real one: git's remote commands stop on
    // it ("missing value"), so the note says to delete the line.
    void setRemoteUrlNamesAValuelessUrlAmongOthers() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        appendConfig(repo.path(), "[remote \"origin\"]\n\turl = https://example.com/a.git\n\turl\n");
        if (QTest::currentTestFailed())
            return;
        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        const QByteArray before = readFile(repo.path(), QStringLiteral(".git/config"));
        const auto res = opened->setRemoteUrl("origin", "https://example.com/b.git");
        QVERIFY(!res.ok());
        QVERIFY2(QString::fromStdString(res.error().message())
                     .contains(QLatin1String("a `url` line with no value in the repository's config")),
                 res.error().message().c_str());
        QCOMPARE(readFile(repo.path(), QStringLiteral(".git/config")), before);
    }

    // Several URLs (`git remote set-url --add`): refused as git does,
    // saying so instead of libgit2's "not unique due to being a
    // multivar", and nothing changes.
    void setRemoteUrlRefusesARemoteWithSeveralUrls() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("a.txt", "a\n", "a").ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"remote", "add", "origin", "https://example.com/a.git"})));
        QVERIFY(succeeded(git.run({"remote", "set-url", "--add", "origin",
                                   "https://example.com/b.git"})));
        const QByteArray before = readFile(repo.path(), QStringLiteral(".git/config"));
        const auto res = repo.repo().setRemoteUrl("origin", "https://example.com/c.git");
        QVERIFY(!res.ok());
        const QString message = QString::fromStdString(res.error().message());
        QVERIFY2(message.contains(QLatin1String("2 URLs"))
                     && message.contains(QLatin1String("https://example.com/a.git"))
                     && message.contains(QLatin1String("https://example.com/b.git")),
                 qPrintable(message));
        QCOMPARE(readFile(repo.path(), QStringLiteral(".git/config")), before);

        // One URL in the global config, one in the repository's: `git
        // remote set-url --delete` couldn't remove the global one, so
        // the note says where it is instead.
        git_buf saved = GIT_BUF_INIT;
        QCOMPARE(git_libgit2_opts(GIT_OPT_GET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, &saved), 0);
        const QByteArray savedPath(saved.ptr ? saved.ptr : "", static_cast<int>(saved.size));
        git_buf_dispose(&saved);
        const auto restore = qScopeGuard([&] {
            git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, savedPath.constData());
        });
        QTemporaryDir home;
        QVERIFY(home.isValid());
        QFile global(home.filePath(QStringLiteral(".gitconfig")));
        QVERIFY(global.open(QIODevice::WriteOnly));
        global.write("[remote \"mixed\"]\n\turl = https://example.com/global.git\n");
        global.close();
        QCOMPARE(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL,
                                  QFile::encodeName(home.path()).constData()), 0);
        QVERIFY(succeeded(git.run({"config", "--local", "--add", "remote.mixed.url",
                                   "https://example.com/local.git"})));
        // Opened again: libgit2 read the global config with the first open.
        auto reopened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(reopened.ok());
        const QByteArray before2 = readFile(repo.path(), QStringLiteral(".git/config"));
        const auto mixed = reopened->setRemoteUrl("mixed", "https://example.com/c.git");
        QVERIFY(!mixed.ok());
        QVERIFY2(QString::fromStdString(mixed.error().message())
                     .contains(QLatin1String("some of them in your global git config")),
                 mixed.error().message().c_str());
        QCOMPARE(readFile(repo.path(), QStringLiteral(".git/config")), before2);

        // Both in the global config: listed, and where.
        QVERIFY(global.open(QIODevice::WriteOnly | QIODevice::Truncate));
        global.write("[remote \"both\"]\n\turl = https://example.com/g1.git\n"
                     "\turl = https://example.com/g2.git\n");
        global.close();
        auto reopened2 = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(reopened2.ok());
        const auto both = reopened2->setRemoteUrl("both", "https://example.com/c.git");
        QVERIFY(!both.ok());
        const QString bothMessage = QString::fromStdString(both.error().message());
        QVERIFY2(bothMessage.contains(QLatin1String("2 URLs"))
                     && bothMessage.contains(QLatin1String(", in your global git config;")),
                 qPrintable(bothMessage));
        QCOMPARE(readFile(repo.path(), QStringLiteral(".git/config")), before2);

        // One global, one in a file the repository's config includes:
        // both places named.
        QVERIFY(global.open(QIODevice::WriteOnly | QIODevice::Truncate));
        global.write("[remote \"spread\"]\n\turl = https://example.com/g.git\n");
        global.close();
        repo.writeFile(".git/spread.config", "[remote \"spread\"]\n\turl = https://example.com/i.git\n");
        QVERIFY(succeeded(git.run({"config", "include.path", "spread.config"})));
        auto reopened3 = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(reopened3.ok());
        const auto spread = reopened3->setRemoteUrl("spread", "https://example.com/c.git");
        QVERIFY(!spread.ok());
        const QString spreadMessage = QString::fromStdString(spread.error().message());
        QVERIFY2(spreadMessage.contains(QLatin1String(
                     "in your global git config and a file that the repository's config includes")),
                 qPrintable(spreadMessage));
    }

    // -----------------------------------------------------------------
    // removeWorktree(): libgit2's prune deletes the folder holding the
    // file that the worktree's gitdir file in .git/worktrees names, and
    // checks nothing. Anyone who could write to .git could have Remove
    // delete any folder, everything in it. A folder is deleted now only
    // if git would delete it: the worktree's own, whose .git file
    // points back.
    // -----------------------------------------------------------------
    void removeWorktreeDeletesItsFolderAndFiles_data() {
        QTest::addColumn<bool>("byGit");
        QTest::addColumn<bool>("changed");
        QTest::addColumn<QString>("folder");
        const QString wt = QStringLiteral("wt");
        QTest::newRow("added by git") << true << false << wt;
        QTest::newRow("added by git, uncommitted changes in it") << true << true << wt;
        QTest::newRow("added by GitBolt") << false << false << wt;
        // Its name in .git/worktrees ends in U+3000 too, and so does
        // the path in its .git file pointing back: git takes no more
        // than line ends off that file.
        QTest::newRow("added by git, its name ending in U+3000")
            << true << false << QStringLiteral("wt\u3000x\u3000");
    }

    void removeWorktreeDeletesItsFolderAndFiles() {
        QFETCH(bool, byGit);
        QFETCH(bool, changed);
        QFETCH(QString, folder);
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString main = base.filePath(QStringLiteral("main"));
        const QString worktree = base.filePath(folder);
        QVERIFY(addRepoWithWorktree(main, worktree, byGit));
        if (changed) {
            // Deleted all the same, as the question says.
            QVERIFY(writeFile(worktree + QStringLiteral("/a.txt"), "changed\n"));
            QVERIFY(writeFile(worktree + QStringLiteral("/new.txt"), "new\n"));
        }
        auto repo = gitbolt::git::Repository::open(main.toStdString());
        QVERIFY(repo.ok());
        const auto listed = repo->worktrees();
        QVERIFY(listed.ok());
        QCOMPARE(listed->size(), std::size_t(1));
        const std::string name = listed->front().name;
        QCOMPARE(QString::fromStdString(name), byGit ? folder : QStringLiteral("wt"));
        const auto removed = repo->removeWorktree(name, listed->front().path);
        QVERIFY2(removed.ok(), removed.error().message().c_str());
        QVERIFY(!QFileInfo::exists(worktree));
        QVERIFY(!QFileInfo::exists(main + QStringLiteral("/.git/worktrees/")
                                   + QString::fromStdString(name)));
        const auto after = repo->worktrees();
        QVERIFY(after.ok());
        QVERIFY(after->empty());
        QCOMPARE(readFile(main, QStringLiteral("a.txt")), QByteArray("a\n"));
    }

    // A locked worktree is refused, as it was: unlock it first.
    void removeWorktreeRefusesALockedOne() {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString main = base.filePath(QStringLiteral("main"));
        const QString worktree = base.filePath(QStringLiteral("wt"));
        QVERIFY(addRepoWithWorktree(main, worktree));
        QVERIFY(succeeded(gitbolt::git::GitProcess(main.toStdString())
                              .run({"worktree", "lock", "--reason", "on a stick",
                                    worktree.toStdString()})));
        auto repo = gitbolt::git::Repository::open(main.toStdString());
        QVERIFY(repo.ok());
        const auto removed = repo->removeWorktree("wt");
        QVERIFY(!removed.ok());
        QVERIFY2(QString::fromStdString(removed.error().message()).contains(QLatin1String("locked")),
                 removed.error().message().c_str());
        QCOMPARE(readFile(worktree, QStringLiteral("a.txt")), QByteArray("a\n"));
        QVERIFY(QFileInfo::exists(main + QStringLiteral("/.git/worktrees/wt/locked")));
    }

    // A worktree whose .git file is gone loses its files in
    // .git/worktrees, as `git worktree prune` takes them out, and
    // nothing else: a folder still there keeps what is in it.
    void removeWorktreeWhoseGitFileIsGone_data() {
        QTest::addColumn<bool>("folderGone");
        QTest::newRow("folder gone") << true;
        QTest::newRow(".git file gone") << false;
    }

    void removeWorktreeWhoseGitFileIsGone() {
        QFETCH(bool, folderGone);
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString main = base.filePath(QStringLiteral("main"));
        const QString worktree = base.filePath(QStringLiteral("wt"));
        QVERIFY(addRepoWithWorktree(main, worktree));
        if (folderGone)
            QVERIFY(QDir(worktree).removeRecursively());
        else
            QVERIFY(QFile::remove(worktree + QStringLiteral("/.git")));
        auto repo = gitbolt::git::Repository::open(main.toStdString());
        QVERIFY(repo.ok());
        const auto removed = repo->removeWorktree("wt");
        QVERIFY2(removed.ok(), removed.error().message().c_str());
        QVERIFY(!QFileInfo::exists(main + QStringLiteral("/.git/worktrees/wt")));
        QCOMPARE(readFile(main, QStringLiteral("a.txt")), QByteArray("a\n"));
        if (!folderGone)
            QCOMPARE(readFile(worktree, QStringLiteral("a.txt")), QByteArray("a\n"));
    }

    // Some keep a bare repository's worktrees in its own folder: one
    // there is removed as any other.
    void removeWorktreeInsideABareRepository() {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString main = base.filePath(QStringLiteral("main"));
        const QString bare = base.filePath(QStringLiteral("project.git"));
        const QString worktree = bare + QStringLiteral("/side");
        QVERIFY(addRepoWithWorktree(main, base.filePath(QStringLiteral("wt"))));
        QVERIFY(succeeded(gitbolt::git::GitProcess(base.path().toStdString())
                              .run({"clone", "-q", "--bare", main.toStdString(),
                                    bare.toStdString()})));
        QVERIFY(succeeded(gitbolt::git::GitProcess(bare.toStdString())
                              .run({"worktree", "add", "-q", worktree.toStdString(), "side"})));
        auto repo = gitbolt::git::Repository::open(bare.toStdString());
        QVERIFY(repo.ok());
        QVERIFY(repo->isBare());
        const auto listed = repo->worktrees();
        QVERIFY(listed.ok());
        QCOMPARE(listed->size(), std::size_t(1));
        const auto removed = repo->removeWorktree("side", listed->front().path);
        QVERIFY2(removed.ok(), removed.error().message().c_str());
        QVERIFY(!QFileInfo::exists(worktree));
        QVERIFY(!QFileInfo::exists(bare + QStringLiteral("/worktrees/side")));
        QVERIFY(QFileInfo::exists(bare + QStringLiteral("/HEAD")));
    }

    // The folder deleted is the one the user was asked about: a
    // worktree moved since is refused.
    void removeWorktreeRefusesAnotherFolderThanAsked() {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString main = base.filePath(QStringLiteral("main"));
        const QString worktree = base.filePath(QStringLiteral("wt"));
        const QString moved = base.filePath(QStringLiteral("moved"));
        QVERIFY(addRepoWithWorktree(main, worktree));
        auto repo = gitbolt::git::Repository::open(main.toStdString());
        QVERIFY(repo.ok());
        const auto listed = repo->worktrees();
        QVERIFY(listed.ok());
        QCOMPARE(listed->size(), std::size_t(1));
        QVERIFY(succeeded(gitbolt::git::GitProcess(main.toStdString())
                              .run({"worktree", "move", worktree.toStdString(),
                                    moved.toStdString()})));
        const auto removed = repo->removeWorktree("wt", listed->front().path);
        QVERIFY(!removed.ok());
        QVERIFY2(QString::fromStdString(removed.error().message())
                     .contains(QLatin1String("Nothing was deleted")),
                 removed.error().message().c_str());
        QCOMPARE(readFile(moved, QStringLiteral("a.txt")), QByteArray("a\n"));
        QVERIFY(QFileInfo::exists(main + QStringLiteral("/.git/worktrees/wt/gitdir")));
    }

    // What the worktree's files in .git/worktrees say, written there
    // by someone else: no folder but the worktree's own is deleted,
    // and nothing is when it fails. The victim folder holds files that
    // must stay, and so do the worktree and the repository.
    void removeWorktreeDeletesNothingElse_data() {
        QTest::addColumn<QString>("forgery");
        QTest::newRow("gitdir naming a folder's .git") << QStringLiteral("folder .git");
        QTest::newRow("gitdir naming a plain file") << QStringLiteral("plain file");
        QTest::newRow("relative gitdir climbing out of .git") << QStringLiteral("relative");
        QTest::newRow("gitdir naming another file of the worktree")
            << QStringLiteral("other file");
        QTest::newRow(".git file pointing elsewhere") << QStringLiteral("points elsewhere");
        QTest::newRow("gitdir naming the main work tree's .git") << QStringLiteral("main");
        QTest::newRow("a folder holding the repository, pointing back")
            << QStringLiteral("holds repository");
#ifdef Q_OS_WIN
        // The repository opened through a junction in the folder:
        // libgit2 keeps that spelling, and as written the folder holds
        // the repository, though not once the junction is resolved.
        QTest::newRow("a folder holding the repository through a junction")
            << QStringLiteral("holds junction");
#endif
#ifndef Q_OS_WIN
        // Windows' home folder isn't $HOME, and a link there takes a
        // privilege.
        QTest::newRow("the home folder, pointing back") << QStringLiteral("home");
        QTest::newRow("a .git link to a file pointing back") << QStringLiteral("link");
#endif
        QTest::newRow("commondir naming another folder") << QStringLiteral("commondir");
#ifndef Q_OS_WIN
        // The check reads commondir and gitdir as libgit2 does, or it
        // checks a path other than the one libgit2 deletes: each of
        // these was this repository's .git, or the worktree's folder,
        // to the check, and another folder to libgit2. (Links, so not
        // on Windows either.)
        QTest::newRow("commondir ending in U+3000") << QStringLiteral("wide space");
        QTest::newRow("commondir ending in a byte isspace() takes off")
            << QStringLiteral("isspace");
        QTest::newRow("commondir that isn't UTF-8") << QStringLiteral("commondir not UTF-8");
        QTest::newRow("commondir relative to the working directory")
            << QStringLiteral("working directory");
        QTest::newRow("gitdir with .. after a link") << QStringLiteral("dot dot");
        QTest::newRow("gitdir that isn't UTF-8") << QStringLiteral("gitdir not UTF-8");
#endif
    }

    void removeWorktreeDeletesNothingElse() {
        QFETCH(QString, forgery);
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString root = QFileInfo(base.path()).canonicalFilePath();
        const QString main = root + QStringLiteral("/main");
        const QString worktree = root + QStringLiteral("/wt");
        const QString victim = root + QStringLiteral("/victim");
        QVERIFY(addRepoWithWorktree(main, worktree));
        const QString admin = main + QStringLiteral("/.git/worktrees/wt");
        const QByteArray pointingBack = "gitdir: " + admin.toUtf8() + '\n';
        QStringList kept = {victim + QStringLiteral("/MARKER"),
                            victim + QStringLiteral("/sub/MARKER2"),
                            worktree + QStringLiteral("/a.txt"),
                            main + QStringLiteral("/a.txt"),
                            admin + QStringLiteral("/gitdir")};
        QVERIFY(writeFile(victim + QStringLiteral("/MARKER"), "keep\n"));
        QVERIFY(writeFile(victim + QStringLiteral("/sub/MARKER2"), "keep\n"));

        QByteArray gitdir;
        QString home;
        QString workingDir;
        std::string locale;
        QString opened = main;
        if (forgery == QLatin1String("folder .git")) {
            QVERIFY(QDir().mkpath(victim + QStringLiteral("/.git")));
            gitdir = (victim + QStringLiteral("/.git")).toUtf8();
        } else if (forgery == QLatin1String("plain file")) {
            gitdir = (victim + QStringLiteral("/MARKER")).toUtf8();
        } else if (forgery == QLatin1String("relative")) {
            // From main/.git/worktrees/wt.
            gitdir = "../../../../victim/MARKER";
        } else if (forgery == QLatin1String("other file")) {
            gitdir = (worktree + QStringLiteral("/a.txt")).toUtf8();
        } else if (forgery == QLatin1String("points elsewhere")) {
            QVERIFY(writeFile(victim + QStringLiteral("/.git"),
                              "gitdir: " + (root + QStringLiteral("/elsewhere")).toUtf8() + '\n'));
            gitdir = (victim + QStringLiteral("/.git")).toUtf8();
        } else if (forgery == QLatin1String("main")) {
            gitdir = (main + QStringLiteral("/.git")).toUtf8();
        } else if (forgery == QLatin1String("holds repository")) {
            QVERIFY(writeFile(root + QStringLiteral("/.git"), pointingBack));
            gitdir = (root + QStringLiteral("/.git")).toUtf8();
        } else if (forgery == QLatin1String("home")) {
            home = victim;
            QVERIFY(writeFile(victim + QStringLiteral("/.git"), pointingBack));
            gitdir = (victim + QStringLiteral("/.git")).toUtf8();
        } else if (forgery == QLatin1String("link")) {
            const QString gitFile = root + QStringLiteral("/elsewhere/gitfile");
            QVERIFY(writeFile(gitFile, pointingBack));
            QVERIFY(QFile::link(gitFile, victim + QStringLiteral("/.git")));
            gitdir = (victim + QStringLiteral("/.git")).toUtf8();
        } else if (forgery == QLatin1String("commondir")) {
            // The worktree's own gitdir, but prune would also delete
            // <commondir>/worktrees/wt.
            QVERIFY(writeFile(victim + QStringLiteral("/worktrees/wt/MARKER3"), "keep\n"));
            kept.prepend(victim + QStringLiteral("/worktrees/wt/MARKER3"));
            QVERIFY(writeFile(admin + QStringLiteral("/commondir"), victim.toUtf8() + '\n'));
        }
#ifndef Q_OS_WIN
        const QByteArray mainBytes = main.toUtf8();
        const QByteArray rootBytes = root.toUtf8();
        const QString victimAdmin = victim + QStringLiteral("/worktrees/wt/MARKER3");
        if (forgery == QLatin1String("wide space")) {
            // U+3000 is white space to Qt; libgit2 takes off only what
            // isspace() calls white space, a byte at a time, and keeps
            // its E3 80 80. main/l is .git, main/l<U+3000> the victim,
            // all in the repository.
            QVERIFY(symlinkBytes(".git", mainBytes + "/l"));
            QVERIFY(symlinkBytes("../victim", mainBytes + "/l\xE3\x80\x80"));
            QVERIFY(writeFile(victimAdmin, "keep\n"));
            kept.prepend(victimAdmin);
            QVERIFY(writeFile(admin + QStringLiteral("/commondir"),
                              "../../../l\xE3\x80\x80\n"));
        } else if (forgery == QLatin1String("isspace")) {
            // U+00A0 is C2 A0, and A0 is white space to isspace() in
            // some locales (UTF-8 on macOS, Latin-1 elsewhere): libgit2
            // takes it off there and reads main/l<C2>, the victim where
            // the file system takes such a name (macOS's doesn't).
            const std::string was = std::setlocale(LC_CTYPE, nullptr);
            const bool takesA0 = std::setlocale(LC_CTYPE, "en_US.UTF-8")
                                 && std::isspace(0xA0);
            std::setlocale(LC_CTYPE, was.c_str());
            if (!takesA0)
                QSKIP("isspace() takes A0 for white space in no locale here");
            locale = "en_US.UTF-8";
            QVERIFY(symlinkBytes(".git", mainBytes + "/l"));
            QVERIFY(symlinkBytes(".git", mainBytes + "/l\xC2\xA0"));
            symlinkBytes("../victim", mainBytes + "/l\xC2");
            QVERIFY(writeFile(victimAdmin, "keep\n"));
            kept.prepend(victimAdmin);
            QVERIFY(writeFile(admin + QStringLiteral("/commondir"), "../../../l\xC2\xA0\n"));
        } else if (forgery == QLatin1String("commondir not UTF-8")) {
            // Qt reads the byte FF as U+FFFD (EF BF BD), and libgit2
            // keeps it: main/l<U+FFFD> is .git, main/l<FF> the victim
            // where the file system takes such a name.
            QVERIFY(symlinkBytes(".git", mainBytes + "/l\xEF\xBF\xBD"));
            symlinkBytes("../victim", mainBytes + "/l\xFF");
            QVERIFY(writeFile(victimAdmin, "keep\n"));
            kept.prepend(victimAdmin);
            QVERIFY(writeFile(admin + QStringLiteral("/commondir"), "../../../l\xFF\n"));
        } else if (forgery == QLatin1String("working directory")) {
            // libgit2 takes a path from .git/worktrees/wt only when it
            // starts with ./ or ../: "l" is in GitBolt's working
            // directory. Its l in .git/worktrees/wt is .git.
            const QString cwdAdmin = root + QStringLiteral("/cwd/l/worktrees/wt/MARKER3");
            QVERIFY(writeFile(cwdAdmin, "keep\n"));
            kept.prepend(cwdAdmin);
            QVERIFY(symlinkBytes("../..", admin.toUtf8() + "/l"));
            QVERIFY(writeFile(admin + QStringLiteral("/commondir"), "l\n"));
            workingDir = root + QStringLiteral("/cwd");
        } else if (forgery == QLatin1String("dot dot")) {
            // libgit2 keeps the ".." of an absolute gitdir, and the
            // system takes it after the link before it: s/a is x, so
            // s/a/../victim is the victim. Taken off as text, it was
            // s/victim, a folder whose .git file points back.
            QVERIFY(QDir().mkpath(root + QStringLiteral("/x")));
            QVERIFY(QDir().mkpath(root + QStringLiteral("/s")));
            QVERIFY(symlinkBytes(rootBytes + "/x", rootBytes + "/s/a"));
            QVERIFY(QDir().mkpath(victim + QStringLiteral("/.git")));
            QVERIFY(writeFile(root + QStringLiteral("/s/victim/.git"), pointingBack));
            gitdir = rootBytes + "/s/a/../victim/.git";
        } else if (forgery == QLatin1String("gitdir not UTF-8")) {
            // The worktree's folder as Qt reads it has U+FFFD where
            // libgit2's has the byte FF: l<FF>/victim is the victim
            // where the file system takes such a name (l<FF> a link to
            // the folder it is in), l<U+FFFD>/victim a folder whose .git
            // file points back.
            symlinkBytes(rootBytes, rootBytes + "/l\xFF");
            QVERIFY(QDir().mkpath(victim + QStringLiteral("/.git")));
            QVERIFY(writeFile(root + QStringLiteral("/l\uFFFD/victim/.git"), pointingBack));
            gitdir = rootBytes + "/l\xFF/victim/.git";
        }
#endif
#ifdef Q_OS_WIN
        if (forgery == QLatin1String("holds junction")) {
            opened = victim + QStringLiteral("/j");
            if (!makeJunction(opened, main))
                QSKIP("mklink /J made no junction here");
            QVERIFY(writeFile(victim + QStringLiteral("/.git"), pointingBack));
            gitdir = (victim + QStringLiteral("/.git")).toUtf8();
        }
        // Before `base` goes: the junction alone, not what it leads to.
        const auto undo = qScopeGuard([&] {
            if (opened != main)
                QDir().rmdir(opened);
        });
#endif
        if (!gitdir.isEmpty())
            QVERIFY(writeFile(admin + QStringLiteral("/gitdir"), gitdir + '\n'));

        auto repo = gitbolt::git::Repository::open(opened.toStdString());
        QVERIFY(repo.ok());
        gitbolt::git::Result<void> removed;
        {
            const QByteArray savedHome = qgetenv("HOME");
            const auto restoreHome = qScopeGuard([&] { qputenv("HOME", savedHome); });
            if (!home.isEmpty())
                qputenv("HOME", home.toUtf8());
            const QString savedDir = QDir::currentPath();
            const auto restoreDir = qScopeGuard([&] { QDir::setCurrent(savedDir); });
            if (!workingDir.isEmpty())
                QVERIFY(QDir::setCurrent(workingDir));
            const std::string savedLocale = std::setlocale(LC_CTYPE, nullptr);
            const auto restoreLocale =
                qScopeGuard([&] { std::setlocale(LC_CTYPE, savedLocale.c_str()); });
            if (!locale.empty())
                QVERIFY(std::setlocale(LC_CTYPE, locale.c_str()));
            removed = repo->removeWorktree("wt");
        }
        for (const QString& file : kept)
            QVERIFY2(QFileInfo::exists(file), qPrintable(file + QStringLiteral(" is gone")));
        QVERIFY(!removed.ok());
        QVERIFY2(QString::fromStdString(removed.error().message())
                     .contains(QLatin1String("Nothing was deleted")),
                 removed.error().message().c_str());
    }

    // A worktree's name is a folder in .git/worktrees, not a path:
    // libgit2 looks up whatever folder a path names from there (".."
    // is .git itself), and prune deletes it.
    void removeWorktreeTakesOnlyAName() {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString main = base.filePath(QStringLiteral("main"));
        const QString worktree = base.filePath(QStringLiteral("wt"));
        QVERIFY(addRepoWithWorktree(main, worktree));
        auto repo = gitbolt::git::Repository::open(main.toStdString());
        QVERIFY(repo.ok());
        for (const char* name : {"wt/../wt", "..", ".", ""}) {
            const auto removed = repo->removeWorktree(name);
            QVERIFY2(!removed.ok(), name);
            QVERIFY2(QString::fromStdString(removed.error().message())
                         .contains(QLatin1String("not the name of a worktree")),
                     removed.error().message().c_str());
        }
        QCOMPARE(readFile(worktree, QStringLiteral("a.txt")), QByteArray("a\n"));
        QVERIFY(QFileInfo::exists(main + QStringLiteral("/.git/worktrees/wt/gitdir")));
    }

#ifdef Q_OS_WIN
    // git for Windows writes a worktree's .git file with the
    // repository's final path, as the system resolves it: a subst
    // drive's folder, a mapped drive's share, a junction's target.
    // libgit2 keeps the spelling the repository was opened by, and
    // canonicalFilePath() resolves none of them, so Remove refused
    // every worktree git made of a repository opened through one: its
    // .git file "does not point back".
    void removeWorktreeOfARepositoryReachedThroughADriveOrJunction_data() {
        QTest::addColumn<QString>("through");
        QTest::newRow("a junction") << QStringLiteral("junction");
        QTest::newRow("a subst drive") << QStringLiteral("subst");
    }

    void removeWorktreeOfARepositoryReachedThroughADriveOrJunction() {
        QFETCH(QString, through);
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString real = base.filePath(QStringLiteral("real"));
        QVERIFY(QDir().mkpath(real));
        const auto run = [](const QString& program, const QStringList& args) {
            QProcess process;
            process.start(program, args);
            return process.waitForFinished(30000) && process.exitStatus() == QProcess::NormalExit
                   && process.exitCode() == 0;
        };
        QString spelled;   // `real`, through the junction or the drive
        if (through == QLatin1String("junction")) {
            spelled = base.filePath(QStringLiteral("junction"));
            if (!makeJunction(spelled, real))
                QSKIP("mklink /J made no junction here");
        } else {
            QStringList taken;
            for (const QFileInfo& drive : QDir::drives())
                taken << drive.filePath().left(2).toUpper();
            for (char letter = 'Z'; letter >= 'G' && spelled.isEmpty(); --letter) {
                const QString drive = QString(QLatin1Char(letter)) + QLatin1Char(':');
                if (!taken.contains(drive))
                    spelled = drive;
            }
            if (spelled.isEmpty()
                || !run(QStringLiteral("subst"), {spelled, QDir::toNativeSeparators(real)}))
                QSKIP("subst made no drive here");
        }
        // Before `base` goes: the junction or the drive alone, not what
        // they lead to.
        const auto undo = qScopeGuard([&] {
            if (through == QLatin1String("junction"))
                QDir().rmdir(spelled);
            else
                run(QStringLiteral("subst"), {spelled, QStringLiteral("/D")});
        });
        const QString main = spelled + QStringLiteral("/main");
        QVERIFY(addRepoWithWorktree(main, spelled + QStringLiteral("/wt")));

        auto repo = gitbolt::git::Repository::open(main.toStdString());
        QVERIFY(repo.ok());
        const auto listed = repo->worktrees();
        QVERIFY(listed.ok());
        QCOMPARE(listed->size(), std::size_t(1));
        const auto removed = repo->removeWorktree(listed->front().name, listed->front().path);
        QVERIFY2(removed.ok(), removed.error().message().c_str());
        QVERIFY(!QFileInfo::exists(real + QStringLiteral("/wt")));
        QVERIFY(!QFileInfo::exists(real + QStringLiteral("/main/.git/worktrees/wt")));
        QCOMPARE(readFile(real + QStringLiteral("/main"), QStringLiteral("a.txt")),
                 QByteArray("a\n"));
    }
#endif

    // -----------------------------------------------------------------
    // Hostile repository content the bundled libgit2 is patched against
    // (cmake/PatchLibgit2.cmake): a .gitattributes or .gitignore pattern
    // that never finishes matching or recurses too deep, and a tree
    // nested so deep it exhausts memory or the stack. A system libgit2
    // lacks the patches, so these skip there.
    // -----------------------------------------------------------------

    // A committed .gitattributes pattern of a dozen stars and a literal
    // that never comes (`*a...*a*Q`) made libgit2's wildmatch backtrack
    // exponentially against a long path. Selecting such a commit froze
    // GitBolt for good at full CPU, computing its diff on the GUI
    // thread. The parent has files named with forty a's; HEAD installs
    // the pattern and deletes them, so diffCommit looks the "diff"
    // attribute up for each path. It must return promptly now, and so
    // must the same pattern with each star a run of ten thousand: a run
    // went by for one step of a match's budget, and every retry of a
    // star read it all again.
    void hostileGitattributesDiffsPromptly_data() {
        QTest::addColumn<QByteArray>("attributes");
        QTest::addColumn<int>("files");

        QByteArray stars;
        for (int i = 0; i < 12; ++i)
            stars += "*a";
        QTest::newRow("dozen stars") << stars + "*Q diff=x text\n" << 1;

        const QByteArray run(10000, '*');
        QByteArray runs;
        for (int i = 0; i < 12; ++i)
            runs += run + "a";
        runs += run + "Q diff=x\n";
        QTest::newRow("runs of stars") << runs + runs << 3;
    }

    void hostileGitattributesDiffsPromptly() {
#ifndef GITBOLT_LIBGIT2_LIMITS_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's limits");
#else
        QFETCH(QByteArray, attributes);
        QFETCH(int, files);

        gitbolt::test::TestRepo repo;
        QStringList names;
        for (int i = 0; i < files; ++i)
            names << QString(40, QLatin1Char('a')) + QString::number(i) + QStringLiteral(".txt");
        repo.writeFile(".gitattributes", "# benign\n");
        for (const QString& name : names)
            repo.writeFile(name, "hello\n");
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"add", "-A"})));
        QVERIFY(succeeded(git.run({"-c", "user.name=T", "-c", "user.email=t@gitbolt.local",
                                   "-c", "commit.gpgsign=false", "commit", "-q", "-m", "A"})));

        repo.writeFile(".gitattributes", attributes);
        for (const QString& name : names)
            repo.deleteFile(name);
        QVERIFY(succeeded(git.run({"add", "-A"})));
        QVERIFY(succeeded(git.run({"-c", "user.name=T", "-c", "user.email=t@gitbolt.local",
                                   "-c", "commit.gpgsign=false", "commit", "-q", "-m", "B"})));

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        const auto head = opened->head();
        QVERIFY(head.ok());

        QElapsedTimer timer;
        timer.start();
        const auto diff = opened->diffCommit(*head);
        const qint64 elapsed = timer.elapsed();
        QVERIFY2(diff.ok(), diff.ok() ? "" : diff.error().message().c_str());
        QCOMPARE(diff->files.size(), static_cast<size_t>(files + 1));
        // Microseconds a path now; without the patches this never
        // returns. Generous, for a loaded CI box.
        QVERIFY2(elapsed < 5000, qPrintable(QStringLiteral("diffCommit took %1 ms").arg(elapsed)));
#endif
    }

    // Thousands of hostile .gitattributes lines, each of a kind that
    // made wildmatch try again what could no longer match. Every rule is
    // tried against every path looked up, so a few microseconds a rule
    // froze a commit's diff for seconds a file (nine megabytes of the
    // dozen-stars line, three files: 25 s). Each kind now costs a step
    // or so per character, and the rule after them still applies.
    void hostileGitattributesLookupsStayCheap_data() {
        QTest::addColumn<QByteArray>("line");
        QTest::addColumn<QString>("pathPrefix");
        QTest::addColumn<int>("paths");

        // The literal after the last star isn't in the path: each star
        // before it tried all its other places.
        QTest::newRow("stars") << QByteArray("*a*a*a*a*a*a*a*a*a*a*a*a*Q x\n")
                               << QString(40, QLatin1Char('a')) << 450;
        // Each `**/` tried as no directories, its rest unable to match
        // anywhere, then again after every slash.
        QTest::newRow("double-star chain") << QByteArray("**/**/**/**/**/**/**/**/Q x\n")
                                           << QStringLiteral("a/").repeated(40) << 160;
        // A star-slash with no slash left in the path: each star before
        // it tried again, reading the rest of the path every time.
        QTest::newRow("star slash") << QByteArray("*a*/x x\n")
                                    << QString(4000, QLatin1Char('a')) << 300;
    }

    void hostileGitattributesLookupsStayCheap() {
#ifndef GITBOLT_LIBGIT2_LIMITS_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's limits");
#else
        QFETCH(QByteArray, line);
        QFETCH(QString, pathPrefix);
        QFETCH(int, paths);
#ifdef Q_OS_WIN
        if (pathPrefix.size() > 200)
            QSKIP("libgit2 refuses a work tree path past MAX_PATH on Windows");
#endif

        gitbolt::test::TestRepo repo;
        repo.writeFile(".gitattributes", line.repeated(5000) + "*.txt txt\n");
        git_repository* raw = repo.repo().raw();

        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < paths; ++i) {
            const QByteArray path =
                (pathPrefix + QString::number(i) + QStringLiteral(".txt")).toUtf8();
            const char* value = nullptr;
            QCOMPARE(git_attr_get(&value, raw, 0, path.constData(), "x"), 0);
            QCOMPARE(git_attr_value(value), GIT_ATTR_VALUE_UNSPECIFIED);
        }
        const qint64 elapsed = timer.elapsed();

        const QByteArray path = (pathPrefix + QStringLiteral("0.txt")).toUtf8();
        const char* value = nullptr;
        QCOMPARE(git_attr_get(&value, raw, 0, path.constData(), "txt"), 0);
        QCOMPARE(git_attr_value(value), GIT_ATTR_VALUE_TRUE);
        // Well under a second here; without the patches, a quarter of
        // a minute.
        QVERIFY2(elapsed < 5000, qPrintable(QStringLiteral("%1 lookups took %2 ms")
                                                .arg(paths).arg(elapsed)));
#endif
    }

    // Ordinary patterns match as before, through the same attribute
    // lookup the diff driver uses: a committed `*.lisp diff=lisp` still
    // gives a .lisp file that driver, a file it doesn't name has none,
    // and a `binary` glob still sets -diff. A hostile pattern earlier in
    // the file does not disturb the others. Nor do long paths stop
    // patterns from applying, which a match's budget of steps did.
    void ordinaryGitattributesStillApply() {
#ifndef GITBOLT_LIBGIT2_LIMITS_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's limits");
#else
        gitbolt::test::TestRepo repo;
        QByteArray attrs;
        for (int i = 0; i < 12; ++i)
            attrs += "*a";
        attrs += "*Q diff=x\n";          // the hostile pattern, up top
        attrs += "*.lisp diff=lisp\n";
        attrs += "*.bin binary\n";
        attrs += "**/*_*_*_*.bak bak\n";
        attrs += "**/*a* has_a\n";
        attrs += "**/*.scm scheme\n";
        QVERIFY(repo.writeAndCommit(".gitattributes", attrs, "attrs").ok());

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());

        const char* value = nullptr;
        QCOMPARE(git_attr_get(&value, opened->raw(), 0, "src/main.lisp", "diff"), 0);
        QCOMPARE(git_attr_value(value), GIT_ATTR_VALUE_STRING);
        QCOMPARE(QString::fromUtf8(value), QStringLiteral("lisp"));

        value = nullptr;
        QCOMPARE(git_attr_get(&value, opened->raw(), 0, "src/main.c", "diff"), 0);
        QCOMPARE(git_attr_value(value), GIT_ATTR_VALUE_UNSPECIFIED);

        // `binary` expands to -diff: the diff attribute reads as false.
        value = nullptr;
        QCOMPARE(git_attr_get(&value, opened->raw(), 0, "blob.bin", "diff"), 0);
        QCOMPARE(git_attr_value(value), GIT_ATTR_VALUE_FALSE);

#ifndef Q_OS_WIN
        // Not on Windows, where libgit2 refuses a work tree path past
        // MAX_PATH.
        auto isSet = [&](const QString& path, const char* attr) {
            const QByteArray utf8 = path.toUtf8();
            const char* v = nullptr;
            return git_attr_get(&v, opened->raw(), 0, utf8.constData(), attr) == 0
                && git_attr_value(v) == GIT_ATTR_VALUE_TRUE;
        };
        // Paths that take more than a match's first 1000 steps get 64 a
        // character on a second try.
        QVERIFY(isSet(QStringLiteral("dir/").repeated(300) + QStringLiteral("x.scm"), "scheme"));
        value = nullptr;
        const QByteArray longLisp =
            (QStringLiteral("src/") + QString(1500, QLatin1Char('l')) + QStringLiteral(".lisp")).toUtf8();
        QCOMPARE(git_attr_get(&value, opened->raw(), 0, longLisp.constData(), "diff"), 0);
        QCOMPARE(QString::fromUtf8(value), QStringLiteral("lisp"));
        // Snake_case directories with underscores aplenty: each star's
        // literal ran out at the end of a directory, and every star
        // before it tried its other places until the budget ran out.
        QVERIFY(isSet(QStringLiteral(
            "user_util_view_api_model_data_index/event_view_server_view_api_event_util_log_"
            "data_core_util_view/view_util_log_user_log_test_user_client_api_api_test_client_"
            "api_server_view_server/cache_client_view_log_parse_test/model_test_user_parse_"
            "build_event_api_cache_core_server_index_index_view/view_parse_data_parse_test_"
            "data.bak"), "bak"));
        // Directories of a's: a trailing star with a slash after it was
        // tried at each 'a', each try reading on to the slash.
        const QString as(255, QLatin1Char('a'));
        QVERIFY(isSet(as + QLatin1Char('/') + as + QLatin1Char('/') + as + QStringLiteral("/xa.txt"),
                      "has_a"));
#endif
#endif
    }

    // A committed .gitattributes of 8,000 rules naming one macro, built
    // up from four before it, 200 names a line (git reads them all and
    // says nothing). libgit2 copied the macro's thousand names into every
    // rule as it read the file, and kept them while the repository was
    // open: status and a commit's diff took 130 MB for these 87 KB (4 MB
    // now), and memory went as the square of the file. A file's rules
    // now take no more than its size allows, the first ones still
    // applying, and a rule of 2,000 names, a line git skips as too long,
    // is skipped too. The memory is the process's resident set, so the
    // bound is loose.
    void macroGitattributesStayBounded() {
#ifndef GITBOLT_LIBGIT2_LIMITS_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's limits");
#else
        QByteArray attrs = "long.txt";
        for (int i = 0; i < 2000; ++i)
            attrs += " c" + QByteArray::number(i);
        attrs += '\n';
        for (int m = 0, name = 0; m < 5; ++m) {
            attrs += "[attr]m" + QByteArray::number(m);
            if (m > 0)
                attrs += " m" + QByteArray::number(m - 1);
            for (int i = 0; i < 200; ++i, ++name)
                attrs += " a" + QByteArray::number(name);
            attrs += '\n';
        }
        for (int i = 0; i < 8000; ++i)
            attrs += "f" + QByteArray::number(i) + " m4\n";

        gitbolt::test::TestRepo repo;
        const QStringList files{QStringLiteral("f0"), QStringLiteral("f1"), QStringLiteral("f2")};
        for (const QString& file : files)
            repo.writeFile(file, "one\n");
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"add", "-A"})));
        QVERIFY(succeeded(git.run({"-c", "user.name=T", "-c", "user.email=t@gitbolt.local",
                                   "-c", "commit.gpgsign=false", "commit", "-q", "-m", "A"})));
        repo.writeFile(".gitattributes", attrs);
        for (const QString& file : files)
            repo.writeFile(file, "two\n");
        QVERIFY(succeeded(git.run({"add", "-A"})));
        QVERIFY(succeeded(git.run({"-c", "user.name=T", "-c", "user.email=t@gitbolt.local",
                                   "-c", "commit.gpgsign=false", "commit", "-q", "-m", "B"})));
        // A change of the same size: status reads the file, through the
        // attributes (its filters), to see it.
        repo.writeFile("f0", "TWO\n");

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        const auto head = opened->head();
        QVERIFY(head.ok());

        const qint64 before = residentBytes();
        QElapsedTimer timer;
        timer.start();
        const auto status = opened->status();
        const auto diff = opened->diffCommit(*head);
        const qint64 elapsed = timer.elapsed();
        const qint64 grown = residentBytes() - before;
        QVERIFY2(status.ok(), status.ok() ? "" : status.error().message().c_str());
        QVERIFY2(diff.ok(), diff.ok() ? "" : diff.error().message().c_str());
        QCOMPARE(diff->files.size(), static_cast<size_t>(files.size() + 1));
        QCOMPARE(statusLines(*opened), QStringList{QStringLiteral("f0 changed")});
        // A few megabytes now.
        QVERIFY2(grown < 64 * 1024 * 1024,
                 qPrintable(QStringLiteral("status and diffCommit took %1 MB").arg(grown >> 20)));
        QVERIFY2(elapsed < 5000,
                 qPrintable(QStringLiteral("status and diffCommit took %1 ms").arg(elapsed)));

        const char* value = nullptr;
        QCOMPARE(git_attr_get(&value, opened->raw(), 0, "f0", "a999"), 0);
        QCOMPARE(git_attr_value(value), GIT_ATTR_VALUE_TRUE);
        value = nullptr;
        QCOMPARE(git_attr_get(&value, opened->raw(), 0, "long.txt", "c0"), 0);
        QCOMPARE(git_attr_value(value), GIT_ATTR_VALUE_UNSPECIFIED);
        QCOMPARE(output(git.run({"check-attr", "c0", "--", "long.txt"})),
                 QStringLiteral("long.txt: c0: unspecified"));
#endif
    }

    // Short lines that each name 31 macros of 32 names, 116 bytes for
    // 1,023 assignments (under a rule's limit), repeated to 2 MB: git
    // reads every line, quickly and without a word. A file's rules could
    // take four assignments for each of its bytes, and status and a
    // commit's diff held 235 MB for these 2 MB (64 MB now), nearly four
    // times what as big a file of names written out costs. At one a
    // byte, naming macros costs about what spelling the names out does.
    // The first rules still apply.
    void manyMacrosGitattributesStayBounded() {
#ifndef GITBOLT_LIBGIT2_LIMITS_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's limits");
#else
        QByteArray attrs;
        QByteArray rule = "f";
        for (int m = 0, name = 0; m < 31; ++m) {
            attrs += "[attr]M" + QByteArray::number(m);
            for (int i = 0; i < 32; ++i, ++name)
                attrs += " n" + QByteArray::number(name);
            attrs += '\n';
            rule += " M" + QByteArray::number(m);
        }
        rule += '\n';
        while (attrs.size() < 2 * 1024 * 1024)
            attrs += rule;

        gitbolt::test::TestRepo repo;
        const QStringList files{QStringLiteral("f0"), QStringLiteral("f1"), QStringLiteral("f2")};
        for (const QString& file : files)
            repo.writeFile(file, "one\n");
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"add", "-A"})));
        QVERIFY(succeeded(git.run({"-c", "user.name=T", "-c", "user.email=t@gitbolt.local",
                                   "-c", "commit.gpgsign=false", "commit", "-q", "-m", "A"})));
        repo.writeFile(".gitattributes", attrs);
        for (const QString& file : files)
            repo.writeFile(file, "two\n");
        QVERIFY(succeeded(git.run({"add", "-A"})));
        QVERIFY(succeeded(git.run({"-c", "user.name=T", "-c", "user.email=t@gitbolt.local",
                                   "-c", "commit.gpgsign=false", "commit", "-q", "-m", "B"})));
        repo.writeFile("f0", "TWO\n");

        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());
        const auto head = opened->head();
        QVERIFY(head.ok());

        const qint64 before = residentBytes();
        QElapsedTimer timer;
        timer.start();
        const auto status = opened->status();
        const auto diff = opened->diffCommit(*head);
        const qint64 elapsed = timer.elapsed();
        const qint64 grown = residentBytes() - before;
        QVERIFY2(status.ok(), status.ok() ? "" : status.error().message().c_str());
        QVERIFY2(diff.ok(), diff.ok() ? "" : diff.error().message().c_str());
        QCOMPARE(diff->files.size(), static_cast<size_t>(files.size() + 1));
        QVERIFY2(grown < 128 * 1024 * 1024,
                 qPrintable(QStringLiteral("status and diffCommit took %1 MB").arg(grown >> 20)));
        QVERIFY2(elapsed < 5000,
                 qPrintable(QStringLiteral("status and diffCommit took %1 ms").arg(elapsed)));

        const char* value = nullptr;
        QCOMPARE(git_attr_get(&value, opened->raw(), 0, "f", "n0"), 0);
        QCOMPARE(git_attr_value(value), GIT_ATTR_VALUE_TRUE);
        value = nullptr;
        QCOMPARE(git_attr_get(&value, opened->raw(), 0, "f", "n991"), 0);
        QCOMPARE(git_attr_value(value), GIT_ATTR_VALUE_TRUE);
#endif
    }

    // The macros of an ordinary .gitattributes still apply: `binary`,
    // macros the file defines, one defined from another, and a name
    // after a macro on its line still wins over it.
    void ordinaryAttributeMacrosStillApply() {
#ifndef GITBOLT_LIBGIT2_LIMITS_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's limits");
#else
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(".gitattributes",
                                    "[attr]generated -diff -merge linguist-generated\n"
                                    "[attr]vendored generated linguist-vendored=yes\n"
                                    "*.png binary\n"
                                    "*.svg binary diff\n"
                                    "*.pb.go generated\n"
                                    "vendor/** vendored\n",
                                    "attrs").ok());
        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());

        // "set", "unset", "unspecified", or the attribute's value.
        auto attr = [&](const char* path, const char* name) {
            const char* value = nullptr;
            if (git_attr_get(&value, opened->raw(), 0, path, name) < 0)
                return QStringLiteral("<failed>");
            switch (git_attr_value(value)) {
            case GIT_ATTR_VALUE_TRUE:
                return QStringLiteral("set");
            case GIT_ATTR_VALUE_FALSE:
                return QStringLiteral("unset");
            case GIT_ATTR_VALUE_UNSPECIFIED:
                return QStringLiteral("unspecified");
            case GIT_ATTR_VALUE_STRING:
                return QString::fromUtf8(value);
            }
            return QStringLiteral("<unknown>");
        };
        QCOMPARE(attr("logo.png", "diff"), QStringLiteral("unset"));
        QCOMPARE(attr("logo.png", "text"), QStringLiteral("unset"));
        QCOMPARE(attr("logo.svg", "diff"), QStringLiteral("set"));
        QCOMPARE(attr("logo.svg", "merge"), QStringLiteral("unset"));
        QCOMPARE(attr("api/v1.pb.go", "diff"), QStringLiteral("unset"));
        QCOMPARE(attr("api/v1.pb.go", "linguist-generated"), QStringLiteral("set"));
        QCOMPARE(attr("vendor/lib/x.c", "merge"), QStringLiteral("unset"));
        QCOMPARE(attr("vendor/lib/x.c", "linguist-generated"), QStringLiteral("set"));
        QCOMPARE(attr("vendor/lib/x.c", "linguist-vendored"), QStringLiteral("yes"));
        QCOMPARE(attr("main.c", "diff"), QStringLiteral("unspecified"));
#endif
    }

    // A .gitignore line of 5,000 `**/` (15 KB): wildmatch recursed once
    // for each, and status, which opening a repository runs on a worker
    // thread (512 KB of stack on macOS), ran out of stack and crashed.
    // It stops at 256 now; the pattern doesn't apply, the one after it
    // does.
    void hostileGitignoreLeavesStatusTheStack() {
#ifndef GITBOLT_LIBGIT2_LIMITS_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's limits");
#else
        gitbolt::test::TestRepo repo;
        repo.writeFile(".gitignore", QByteArray("**/").repeated(5000) + "Q\n*.log\n");
        repo.writeFile("file.txt", "x\n");
        repo.writeFile("noise.log", "x\n");
        auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
        QVERIFY(opened.ok());

        std::optional<gitbolt::git::Result<std::vector<gitbolt::git::StatusEntry>>> status;
        std::unique_ptr<QThread> worker(QThread::create([&] { status = opened->status(); }));
        worker->setStackSize(512 * 1024);
        worker->start();
        QVERIFY(worker->wait(60000));
        QVERIFY(status.has_value());
        QVERIFY2(status->ok(), status->ok() ? "" : status->error().message().c_str());

        QStringList paths;
        for (const auto& entry : status->value())
            paths << QString::fromStdString(entry.path);
        paths.sort();
        QCOMPARE(paths, (QStringList{QStringLiteral(".gitignore"), QStringLiteral("file.txt")}));
#endif
    }

    // A file whose name reads as a negated pathspec, "!*b[ab]". Unstaging
    // passed its path as a pathspec; libgit2 took the '!' to negate
    // `*b[ab]`, then fell back to the name itself when that pattern
    // didn't match it, but only when the match said WM_NOMATCH. With
    // cmake/PatchLibgit2.cmake's limits, matches like this one stop
    // early, in an abort, and unstaging the file did nothing; wildmatch()
    // says WM_NOMATCH for any failure. Unstaging takes the name as
    // itself now, with any libgit2.
    void unstageFileNamedLikeANegatedPathspec() {
#ifdef Q_OS_WIN
        QSKIP("a Windows file name can't hold '*'");
#else
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit("base.txt", "base\n", "base").ok());
        const QString name = QStringLiteral("!*b[ab]");
        repo.writeFile(name, "x\n");
        QVERIFY(repo.stageFile(name).ok());

        auto entryFor = [&]() -> std::optional<gitbolt::git::StatusEntry> {
            const auto status = repo.repo().status();
            if (!status.ok())
                return std::nullopt;
            for (const auto& entry : status.value())
                if (entry.path == name.toStdString())
                    return entry;
            return std::nullopt;
        };
        auto before = entryFor();
        QVERIFY(before.has_value());
        QVERIFY(before->isStaged());

        QVERIFY(repo.repo().unstageFile(name.toStdString()).ok());
        auto after = entryFor();
        QVERIFY(after.has_value());
        QVERIFY2(!after->isStaged(), "the file is still staged");
        QVERIFY(after->isUntracked());
#endif
    }

    // A tree nested a few hundred levels past the depth limit: status,
    // diffCommit and the File Tree's walk grew without bound (memory as
    // the square of the depth; the walk's recursion ran the stack out).
    // They stop with a "tree too deep" error now, while a tree a little
    // under the limit still works. The submodules, which libgit2 reads
    // HEAD's tree for where there is a .gitmodules, say so too: their
    // failure went unseen, and the list was empty, as if there were none.
    void deepTreeIsBounded() {
#ifndef GITBOLT_LIBGIT2_LIMITS_PATCHED
        QSKIP("needs the bundled libgit2, with cmake/PatchLibgit2.cmake's limits");
#else
        // Just under the limit (2048): everything works.
        {
            gitbolt::test::TestRepo repo;
            const auto id = commitDeepTree(repo.repo(), 2000);
            if (QTest::currentTestFailed())
                return;
            auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
            QVERIFY(opened.ok());
            QVERIFY(opened->status().ok());
            QVERIFY(opened->diffCommit(id).ok());
            const auto walk = opened->walkTreeAtCommit(id);
            QVERIFY(walk.ok());
            QVERIFY(walk->size() >= 2000u);
        }
        // A few hundred levels past it: each call returns the error.
        {
            gitbolt::test::TestRepo repo;
            const auto id = commitDeepTree(repo.repo(), 2350);
            if (QTest::currentTestFailed())
                return;
            auto opened = gitbolt::git::Repository::open(repo.path().toStdString());
            QVERIFY(opened.ok());
            auto tooDeep = [](const auto& result) {
                return !result.ok()
                    && QString::fromStdString(result.error().message())
                           .contains(QLatin1String("tree too deep"));
            };
            QVERIFY2(tooDeep(opened->status()), "status() did not stop at the depth limit");
            QVERIFY2(tooDeep(opened->diffCommit(id)), "diffCommit did not stop at the depth limit");
            QVERIFY2(tooDeep(opened->walkTreeAtCommit(id)),
                     "walkTreeAtCommit did not stop at the depth limit");
            repo.writeFile(QStringLiteral(".gitmodules"),
                           "[submodule \"s\"]\n\tpath = s\n\turl = ./\n");
            QVERIFY2(tooDeep(opened->submodules()), "submodules() did not stop at the depth limit");
        }
#endif
    }

    // diffCommit and walkTreeAtCommit ask `cancelled` as they go, at each
    // entry (and the diff before each file's patch too), and stop, with
    // "Cancelled", the moment it says so, wherever they are: a read the
    // user had moved on from (GitService::readAsync) ran on to the end
    // of a huge tree. Never saying so, it changes nothing.
    void readsStopWhenCancelled() {
        gitbolt::test::TestRepo repo;
        const auto wide = repo.commitWideTree(1000, "1\n2\n3\n");
        // How many files the diff has, or entries the walk (10 folders
        // of 100).
        using Read = std::function<gitbolt::git::Result<size_t>(const std::function<bool()>&)>;
        const Read diff = [&](const std::function<bool()>& cancelled) -> gitbolt::git::Result<size_t> {
            auto read = repo.repo().diffCommit(wide, cancelled);
            if (!read)
                return read.error();
            return read->files.size();
        };
        const Read walk = [&](const std::function<bool()>& cancelled) -> gitbolt::git::Result<size_t> {
            auto read = repo.repo().walkTreeAtCommit(wide, cancelled);
            if (!read)
                return read.error();
            return read->size();
        };
        // And how often each asks at least: the diff at each file in the
        // trees and before its patch, the walk at each entry.
        for (const auto& [name, read, size, atLeast] :
             {std::tuple{"diffCommit", diff, size_t(1000), 2000},
              {"walkTreeAtCommit", walk, size_t(1010), 1010}}) {
            int asked = 0;
            const auto whole = read([&] { ++asked; return false; });
            QVERIFY2(whole.ok(), name);
            QCOMPARE(*whole, size);
            QVERIFY2(asked >= atLeast, qPrintable(QStringLiteral("%1: asked %2 times")
                                                      .arg(name).arg(asked)));
            for (const int stopAt : {1, asked / 2, asked}) {
                int askedNow = 0;
                const auto stopped = read([&] { return ++askedNow >= stopAt; });
                QVERIFY2(!stopped.ok()
                             && stopped.error().code() == gitbolt::git::GitErrorCode::User
                             && stopped.error().message() == "Cancelled",
                         qPrintable(QStringLiteral("%1 at %2").arg(name).arg(stopAt)));
                // Stopped right there (its own check of why libgit2
                // stopped asks once more).
                QVERIFY2(askedNow <= stopAt + 1,
                         qPrintable(QStringLiteral("%1 at %2: asked %3 times")
                                        .arg(name).arg(stopAt).arg(askedNow)));
            }
        }
    }
};

QTEST_MAIN(TestRepository)
#include "TestRepository.moc"
