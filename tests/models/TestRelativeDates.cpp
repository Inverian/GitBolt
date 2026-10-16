//
// TestRelativeDates — the commit log's and the blame view's relative
// dates count in English: "1 day ago", "2 days ago". The log said
// "1 day(s) ago" and "1 yr(s) ago", the blame view "1 hours ago" and
// "1 days ago"; they are %n texts now, with their forms in
// translations/gitbolt_en.ts (util::installEnglishPlurals).
//

#include <QTest>

#include <chrono>

#include "git/Blame.h"
#include "git/Commit.h"
#include "models/BlameModel.h"
#include "models/CommitLogModel.h"
#include "util/Translations.h"

using gitbolt::models::BlameModel;
using gitbolt::models::CommitLogColumn;
using gitbolt::models::CommitLogModel;

namespace {

// Ago, plus a minute, so the bucket can't tip over while the test runs.
std::chrono::system_clock::time_point ago(std::chrono::hours hours) {
    return std::chrono::system_clock::now() - hours - std::chrono::minutes(1);
}

QString logDate(std::chrono::hours hours) {
    gitbolt::git::CommitData commit;
    commit.id = gitbolt::git::ObjectId::fromHex(std::string(40, 'a'));
    commit.summary = "c";
    commit.author.when = ago(hours);
    commit.committer.when = commit.author.when;
    CommitLogModel model;
    model.setRelativeDate(true);
    model.setCommits({commit});
    return model.index(0, static_cast<int>(CommitLogColumn::Date)).data().toString();
}

QString blameDate(std::chrono::hours hours) {
    gitbolt::git::BlameResult result;
    result.path = "file.txt";
    result.lines = {"line"};
    gitbolt::git::BlameHunk hunk;
    hunk.commitId = gitbolt::git::ObjectId::fromHex(std::string(40, 'b'));
    hunk.signature.name = "A U Thor";
    hunk.signature.when = ago(hours);
    hunk.startLine = 1;
    hunk.lineCount = 1;
    result.hunks = {hunk};
    BlameModel model;
    model.setBlameResult(result);
    return model.index(0, BlameModel::ColDate).data().toString();
}

constexpr std::chrono::hours kDay{24};

} // namespace

class TestRelativeDates : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QCOMPARE(gitbolt::util::installEnglishPlurals(), gitbolt::util::hasEnglishPlurals());
        if (!gitbolt::util::hasEnglishPlurals())
            QSKIP("built without Qt LinguistTools (or with Qt before 6.7): no English plural forms");
    }

    void commitLogCountsDaysAndYears_data() {
        QTest::addColumn<int>("days");
        QTest::addColumn<QString>("text");
        QTest::newRow("1 day") << 1 << QStringLiteral("1 day ago");
        QTest::newRow("2 days") << 2 << QStringLiteral("2 days ago");
        QTest::newRow("1 yr") << 400 << QStringLiteral("1 yr ago");
        QTest::newRow("2 yrs") << 800 << QStringLiteral("2 yrs ago");
    }

    void commitLogCountsDaysAndYears() {
        QFETCH(int, days);
        QFETCH(QString, text);
        QCOMPARE(logDate(kDay * days), text);
    }

    void blameCountsHoursToYears_data() {
        QTest::addColumn<int>("hours");
        QTest::addColumn<QString>("text");
        QTest::newRow("1 hour") << 1 << QStringLiteral("1 hour ago");
        QTest::newRow("5 hours") << 5 << QStringLiteral("5 hours ago");
        QTest::newRow("1 day") << 24 << QStringLiteral("1 day ago");
        QTest::newRow("2 days") << 48 << QStringLiteral("2 days ago");
        QTest::newRow("1 month") << 24 * 31 << QStringLiteral("1 month ago");
        QTest::newRow("2 months") << 24 * 61 << QStringLiteral("2 months ago");
        QTest::newRow("1 year") << 24 * 366 << QStringLiteral("1 year ago");
        QTest::newRow("2 years") << 24 * 731 << QStringLiteral("2 years ago");
    }

    void blameCountsHoursToYears() {
        QFETCH(int, hours);
        QFETCH(QString, text);
        QCOMPARE(blameDate(std::chrono::hours(hours)), text);
    }
};

QTEST_GUILESS_MAIN(TestRelativeDates)
#include "TestRelativeDates.moc"
