#pragma once
//
// TestRepoHelper — shared test fixture for building temporary Git
// repositories in unit tests. Uses libgit2 directly (via our wrapper
// where possible, and raw git2 where the wrapper doesn't expose the
// needed primitive yet) to avoid any dependency on a system git binary.
//
// Typical usage:
//
//     #include "../TestRepoHelper.h"
//
//     void MyTest::myTestCase() {
//         gitbolt::test::TestRepo repo;                    // makes an empty repo
//         repo.writeFile("README.md", "hello\n");
//         repo.stageAll();
//         auto firstCommit = repo.commit("initial commit");
//         QVERIFY(firstCommit.ok());
//         // ... the repo + its tempdir are cleaned up automatically
//     }
//
// TestRepo owns a QTemporaryDir and a gitbolt::git::Repository. It
// exposes a handful of building-block methods for common fixture setup:
//   writeFile / appendFile / deleteFile
//   stageAll / stageFile
//   commit (with optional author override)
//   path() to get the on-disk path
//
// All helpers return Result<T> from our core library, so tests use
// the usual QVERIFY(result.ok()) pattern.

#include "git/Repository.h"

#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include <git2.h>
#include <cstring>
#include <memory>

namespace gitbolt::test {

class TestRepo {
public:
    TestRepo() {
        isolateGitConfig();
        Q_ASSERT(tmpDir_.isValid());
        auto result = gitbolt::git::Repository::init(tmpDir_.path().toStdString());
        Q_ASSERT(result.ok());
        repo_ = std::make_unique<gitbolt::git::Repository>(std::move(*result));

        // Seed a deterministic identity so git_signature_default doesn't
        // blow up with "config value 'user.name' was not found".
        auto cfg = repo_->config();
        cfg.setString("user.name",  "Test User");
        cfg.setString("user.email", "test@gitbolt.local");
    }

    /// Path to the working tree (no trailing slash).
    QString path() const { return tmpDir_.path(); }

    /// Underlying Repository wrapper (non-owning).
    gitbolt::git::Repository& repo() { return *repo_; }

    /// Overwrite (or create) a file inside the work tree.
    void writeFile(const QString& relPath, const QByteArray& content) {
        QFile f(QDir(path()).absoluteFilePath(relPath));
        QDir().mkpath(QFileInfo(f).absolutePath());
        // Not Q_ASSERT: it compiles out in Release (QT_NO_DEBUG) and takes
        // the open() call with it, so every write silently went nowhere.
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            qFatal("TestRepo: cannot open %s", qPrintable(f.fileName()));
        f.write(content);
        f.close();
    }

    /// Append to a file (create if missing).
    void appendFile(const QString& relPath, const QByteArray& content) {
        QFile f(QDir(path()).absoluteFilePath(relPath));
        QDir().mkpath(QFileInfo(f).absolutePath());
        if (!f.open(QIODevice::WriteOnly | QIODevice::Append))
            qFatal("TestRepo: cannot open %s", qPrintable(f.fileName()));
        f.write(content);
        f.close();
    }

    /// Remove a file from the work tree.
    void deleteFile(const QString& relPath) {
        QFile::remove(QDir(path()).absoluteFilePath(relPath));
    }

    /// Stage every modification in the work tree.
    gitbolt::git::Result<void> stageAll() { return repo_->stageAll(); }

    /// Stage a single path.
    gitbolt::git::Result<void> stageFile(const QString& relPath) {
        return repo_->stageFile(relPath.toStdString());
    }

    /// Create a commit from the current index. Returns the new commit OID.
    gitbolt::git::Result<gitbolt::git::ObjectId> commit(const QString& message) {
        return repo_->commit(message.toStdString(), /*amend=*/false);
    }

    /// Convenience: write, stage, and commit in one call.
    gitbolt::git::Result<gitbolt::git::ObjectId>
    writeAndCommit(const QString& relPath, const QByteArray& content, const QString& message) {
        writeFile(relPath, content);
        auto staged = stageFile(relPath);
        if (!staged) return staged.error();
        return commit(message);
    }

private:
    /// Point libgit2's global/system/xdg config search paths at an
    /// empty directory ONCE per test process, so the developer's
    /// ~/.gitconfig (core.autocrlf, init.defaultBranch,
    /// commit.gpgsign, …) can't bleed into fixture behavior and
    /// flake tests across machines. Repo-local config written by
    /// the fixture (user.name/email) is unaffected.
    static void isolateGitConfig() {
        static const bool done = [] {
            static QTemporaryDir empty;  // lives for the process
            const QByteArray p = empty.path().toUtf8();
            git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH,
                             GIT_CONFIG_LEVEL_GLOBAL, p.constData());
            git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH,
                             GIT_CONFIG_LEVEL_SYSTEM, p.constData());
            git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH,
                             GIT_CONFIG_LEVEL_XDG, p.constData());
            git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH,
                             GIT_CONFIG_LEVEL_PROGRAMDATA, p.constData());
            return true;
        }();
        Q_UNUSED(done);
    }

    QTemporaryDir tmpDir_;
    std::unique_ptr<gitbolt::git::Repository> repo_;
};

} // namespace gitbolt::test
