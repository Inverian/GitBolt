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

    /// HEAD made again with other idents ("Name <email>"), its tree,
    /// parents and message the same, and the branch moved to it. The
    /// object is written as it is: git puts no limit on an ident's
    /// length (an author name of megabytes passes fsck), and a clone
    /// brings whatever one a commit has.
    gitbolt::git::ObjectId rewriteHeadIdents(const std::string& author,
                                              const std::string& committer) {
        git_repository* raw = repo_->raw();
        git_reference* head = nullptr;
        git_commit* old = nullptr;
        if (git_repository_head(&head, raw) != 0
            || git_commit_lookup(&old, raw, git_reference_target(head)) != 0)
            qFatal("TestRepo: no HEAD commit to rewrite");
        const auto hex = [](const git_oid* oid) {
            char buf[GIT_OID_MAX_HEXSIZE + 1];
            return std::string(git_oid_tostr(buf, sizeof buf, oid));
        };
        std::string text = "tree " + hex(git_commit_tree_id(old)) + "\n";
        for (unsigned i = 0; i < git_commit_parentcount(old); ++i)
            text += "parent " + hex(git_commit_parent_id(old, i)) + "\n";
        text += "author " + author + " 1700000000 +0000\n";
        text += "committer " + committer + " 1700000000 +0000\n\n";
        text += git_commit_message(old);
        git_commit_free(old);

        git_odb* odb = nullptr;
        git_oid id;
        git_reference* moved = nullptr;
        if (git_repository_odb(&odb, raw) != 0
            || git_odb_write(&id, odb, text.data(), text.size(), GIT_OBJECT_COMMIT) != 0
            || git_reference_set_target(&moved, head, &id, "rewrite idents") != 0)
            qFatal("TestRepo: cannot write the rewritten commit");
        git_reference_free(moved);
        git_odb_free(odb);
        git_reference_free(head);
        return gitbolt::git::ObjectId(&id);
    }

    /// A commit on HEAD, after the one there if any, whose tree is
    /// `depth` nested folders named "d" with a blob "leaf.txt" at the
    /// bottom: a few kilobytes whatever the depth. The bundled libgit2
    /// turns down a tree nested deeper than 2048 levels ("tree too
    /// deep"; cmake/PatchLibgit2.cmake).
    gitbolt::git::ObjectId commitDeepTree(int depth) {
        git_repository* raw = repo_->raw();
        git_oid cur;
        if (git_blob_create_from_buffer(&cur, raw, "leaf\n", 5) != 0)
            qFatal("TestRepo: cannot write the deep tree's blob");
        for (int level = 0; level <= depth; ++level) {
            const bool leaf = level == 0;
            git_treebuilder* builder = nullptr;
            if (git_treebuilder_new(&builder, raw, nullptr) != 0
                || git_treebuilder_insert(nullptr, builder, leaf ? "leaf.txt" : "d", &cur,
                                          leaf ? GIT_FILEMODE_BLOB : GIT_FILEMODE_TREE) != 0
                || git_treebuilder_write(&cur, builder) != 0)
                qFatal("TestRepo: cannot write the deep tree");
            git_treebuilder_free(builder);
        }
        return commitTree(cur, "HEAD", "deep");
    }

    /// A commit with no parent, on no branch, of `files` files in folders
    /// of a hundred, each `content` (the same blob): written in a moment
    /// however many, and as slow to read as wanted. Its diff has every
    /// line of each added, and libgit2 caches no blob, so a walk of the
    /// tree reads `content` again at each file (its size).
    gitbolt::git::ObjectId commitWideTree(int files, const QByteArray& content) {
        git_repository* raw = repo_->raw();
        git_oid blob;
        if (git_blob_create_from_buffer(&blob, raw, content.constData(),
                                        static_cast<size_t>(content.size())) != 0)
            qFatal("TestRepo: cannot write the wide tree's blob");
        git_treebuilder* root = nullptr;
        if (git_treebuilder_new(&root, raw, nullptr) != 0)
            qFatal("TestRepo: cannot write the wide tree");
        for (int first = 0; first < files; first += 100) {
            git_treebuilder* folder = nullptr;
            git_oid folderId;
            if (git_treebuilder_new(&folder, raw, nullptr) != 0)
                qFatal("TestRepo: cannot write the wide tree");
            for (int file = first; file < files && file < first + 100; ++file) {
                const QByteArray name = "f" + QByteArray::number(file) + ".txt";
                if (git_treebuilder_insert(nullptr, folder, name.constData(), &blob,
                                           GIT_FILEMODE_BLOB) != 0)
                    qFatal("TestRepo: cannot write the wide tree");
            }
            const QByteArray name = "d" + QByteArray::number(first / 100);
            if (git_treebuilder_write(&folderId, folder) != 0
                || git_treebuilder_insert(nullptr, root, name.constData(), &folderId,
                                          GIT_FILEMODE_TREE) != 0)
                qFatal("TestRepo: cannot write the wide tree");
            git_treebuilder_free(folder);
        }
        git_oid tree;
        if (git_treebuilder_write(&tree, root) != 0)
            qFatal("TestRepo: cannot write the wide tree");
        git_treebuilder_free(root);
        return commitTree(tree, nullptr, "wide");
    }

private:
    /// Commits `treeId` with `message`: after HEAD's commit and moving
    /// it when `ref` is "HEAD", with no parent and moving nothing when
    /// it is null.
    gitbolt::git::ObjectId commitTree(const git_oid& treeId, const char* ref,
                                      const char* message) {
        git_repository* raw = repo_->raw();
        git_tree* tree = nullptr;
        git_signature* sig = nullptr;
        if (git_tree_lookup(&tree, raw, &treeId) != 0
            || git_signature_new(&sig, "Test User", "test@gitbolt.local", 1700000000, 0) != 0)
            qFatal("TestRepo: cannot commit the tree");
        git_commit* parent = nullptr;
        git_oid parentId;
        if (ref && git_reference_name_to_id(&parentId, raw, "HEAD") == 0
            && git_commit_lookup(&parent, raw, &parentId) != 0)
            qFatal("TestRepo: cannot read HEAD's commit");
        const git_commit* parents[] = {parent};
        git_oid id;
        const int made = git_commit_create(&id, raw, ref, sig, sig, nullptr, message, tree,
                                           parent ? 1 : 0, parents);
        git_commit_free(parent);
        git_signature_free(sig);
        git_tree_free(tree);
        if (made != 0)
            qFatal("TestRepo: cannot commit the tree");
        return gitbolt::git::ObjectId(&id);
    }

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
