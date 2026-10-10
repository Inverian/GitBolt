#include <QFileInfo>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTest>
#include <QTemporaryDir>
#include "../TestRepoHelper.h"
#include "git/GitProcess.h"
#include "git/Repository.h"

#include <algorithm>

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
};

QTEST_MAIN(TestRepository)
#include "TestRepository.moc"
