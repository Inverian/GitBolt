//
// TestChangelog — guards CHANGELOG.md, GitBolt's one changelog. The
// app embeds it for Help → Changelog and CI's release job publishes
// its "## [x.y.z]" section as the GitHub release notes, so users see
// the same text in both places.
//
// The file itself can still disagree with the version being built,
// and CI only notices after the fact: its "version" job skips pull
// requests, so a version bump without notes merges green and then
// fails the release on main. These checks run in every ctest run.
//
// Compiled against the app's own gitbolt.qrc, so the text under test
// is exactly what the viewer loads. QTEST_MAIN, not the GUILESS one:
// the markdown importer sets up fonts, which come out broken without
// a QGuiApplication (QFont::setPixelSize warnings).
//

#include <QFile>
#include <QFont>
#include <QRegularExpression>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>

namespace {

const QString kEmbedded = QStringLiteral(":/content/CHANGELOG.md");

QString readText(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

// One "## " entry: its heading, then its lines up to the next entry
// or the link-reference footer. That span is what CI's awk cuts out
// as release notes.
struct Entry {
    QString heading;
    QStringList body;
};

QList<Entry> parseEntries(const QString& markdown)
{
    static const QRegularExpression linkDefinition(
        QStringLiteral(R"(^\[.*\]: )"));
    QList<Entry> entries;
    bool inFooter = false;
    for (const QString& line : markdown.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("## "))) {
            entries.append(Entry{line.trimmed(), {}});
            inFooter = false;
        } else if (linkDefinition.match(line).hasMatch()) {
            inFooter = true;
        } else if (!entries.isEmpty() && !inFooter) {
            entries.last().body.append(line);
        }
    }
    return entries;
}

bool isUnreleased(const QString& heading)
{
    return heading == QLatin1String("## Unreleased")
        || heading == QLatin1String("## [Unreleased]");
}

// "## [1.2.3] — 2026-01-01" → "1.2.3". CI looks the section up by the
// literal prefix "## [<version>]", so no other spelling counts.
QString versionOf(const QString& heading)
{
    static const QRegularExpression re(
        QStringLiteral(R"(^## \[(\d+\.\d+\.\d+)\])"));
    const QRegularExpressionMatch m = re.match(heading);
    return m.hasMatch() ? m.captured(1) : QString();
}

} // anonymous namespace


class TestChangelog : public QObject {
    Q_OBJECT

private slots:
    // -----------------------------------------------------------------
    // The viewer shows the root CHANGELOG.md itself, not a copy that
    // can fall behind it (resources/CHANGELOG.md once did, for months).
    // -----------------------------------------------------------------
    void appShowsTheRootChangelog() {
        const QString embedded = readText(kEmbedded);
        QVERIFY2(!embedded.isEmpty(),
                 "Help > Changelog would be empty: no :/content/CHANGELOG.md");
        QCOMPARE(embedded,
                 readText(QStringLiteral(GITBOLT_CHANGELOG_PATH)));
    }

    // -----------------------------------------------------------------
    // Newest entry first: "Unreleased" (changes waiting for a release)
    // and/or the release being built. Anything else means the version
    // in CMakeLists.txt and the notes disagree, one way or the other.
    // -----------------------------------------------------------------
    void newestReleaseIsThisVersion() {
        const QList<Entry> entries = parseEntries(readText(kEmbedded));
        QVERIFY2(!entries.isEmpty(), "CHANGELOG.md has no \"## \" entries");

        for (qsizetype i = 1; i < entries.size(); ++i)
            QVERIFY2(!isUnreleased(entries.at(i).heading),
                     "\"Unreleased\" must be the first entry");

        const qsizetype newest = isUnreleased(entries.first().heading) ? 1 : 0;
        QVERIFY2(newest < entries.size(),
                 "no \"## [x.y.z] - date\" entry under Unreleased");

        const QString heading = entries.at(newest).heading;
        const QString version = versionOf(heading);
        QVERIFY2(!version.isEmpty(), qPrintable(QStringLiteral(
            "\"%1\": release entries must start \"## [x.y.z]\", the only "
            "form CI's release-notes step finds").arg(heading)));
        QVERIFY2(version == QLatin1String(GITBOLT_VERSION),
                 qPrintable(QStringLiteral(
            "the newest CHANGELOG.md release is %1 but project() says %2; "
            "a version bump and its \"## [x.y.z] - date\" entry (Unreleased, "
            "renamed) belong in the same change")
            .arg(version, QStringLiteral(GITBOLT_VERSION))));
    }

    // -----------------------------------------------------------------
    // This version's entry has text: an empty one fails the release
    // job's notes step, which refuses to publish without notes.
    // -----------------------------------------------------------------
    void thisVersionHasReleaseNotes() {
        const QString version = QStringLiteral(GITBOLT_VERSION);
        for (const Entry& e : parseEntries(readText(kEmbedded))) {
            if (versionOf(e.heading) != version)
                continue;
            for (const QString& line : e.body) {
                if (!line.trimmed().isEmpty())
                    return;
            }
            QFAIL(qPrintable(QStringLiteral(
                "\"%1\" has no notes").arg(e.heading)));
        }
        QFAIL(qPrintable(QStringLiteral(
            "CHANGELOG.md has no \"## [%1]\" entry").arg(version)));
    }

    // -----------------------------------------------------------------
    // Every heading still looks like one in the viewer. Qt's markdown
    // importer drops a heading's size and weight once it holds a link,
    // so Keep a Changelog's "[0.9.0]: <url>" footer, which links the
    // "## [0.9.0]" heading on GitHub, turns it into body text in-app.
    // -----------------------------------------------------------------
    void headingsRenderAsHeadings() {
        QTextDocument doc;
        doc.setMarkdown(readText(kEmbedded));   // what QTextBrowser does
        int headings = 0;
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            if (b.blockFormat().headingLevel() == 0)
                continue;
            ++headings;
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                QVERIFY2(it.fragment().charFormat().fontWeight() >= QFont::Bold,
                         qPrintable(QStringLiteral(
                    "heading \"%1\" renders as body text in Help > Changelog "
                    "(a link in it, e.g. from a \"[x.y.z]: <url>\" footer?)")
                    .arg(b.text())));
            }
        }
        QVERIFY(headings > 1);
    }
};

QTEST_MAIN(TestChangelog)
#include "TestChangelog.moc"
