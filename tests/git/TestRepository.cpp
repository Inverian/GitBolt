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
};

QTEST_MAIN(TestRepository)
#include "TestRepository.moc"
