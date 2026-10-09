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
};

QTEST_MAIN(TestRepository)
#include "TestRepository.moc"
