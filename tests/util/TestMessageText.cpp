//
// TestMessageText — how much of a message the views lay out
// (util/MessageText.h):
//
//   - a message that fits comes back exactly as it was, its lines,
//     blank lines and trailing newline included;
//   - a longer one is cut at the cap and ends in "…": one line at the
//     line cap, each line of a text at the paragraph cap (the lines
//     after a line of megabytes stay in), the whole text at the text
//     cap;
//   - a cut never splits a surrogate pair (an emoji stays whole or
//     goes);
//   - from UTF-8, only the start of a message of megabytes is decoded,
//     and it comes out as the same text a full decode would have cut.
//

#include <QElapsedTimer>
#include <QTest>

#include "util/MessageText.h"

#include <string>

using gitbolt::util::elided;
using gitbolt::util::elidedText;
using gitbolt::util::kMaxLineChars;
using gitbolt::util::kMaxParagraphChars;
using gitbolt::util::kMaxTextChars;

namespace {
const QChar kEllipsis(0x2026);
}

class TestMessageText : public QObject {
    Q_OBJECT

private slots:
    void aMessageThatFitsIsUnchanged() {
        const QString subject = QStringLiteral("Fix the frobnicator <b>&amp;</b> its % args");
        QCOMPARE(elided(subject), subject);
        const QString line(kMaxLineChars, QLatin1Char('x'));
        QCOMPARE(elided(line), line);

        // A long, ordinary message: 300 lines, a blank one between
        // paragraphs, a line of exactly the paragraph cap, and the
        // trailing newline git ends it with.
        QString message = QStringLiteral("Subject line\n\n");
        for (int i = 0; i < 300; ++i)
            message += QStringLiteral("Line %1 of the body, with some words in it.\n").arg(i);
        message += QString(kMaxParagraphChars, QLatin1Char('y')) + QLatin1Char('\n');
        QVERIFY(message.size() < kMaxTextChars);
        QCOMPARE(elidedText(message), message);
        QCOMPARE(elidedText(message.toStdString()), message);
        QCOMPARE(elided(subject.toStdString()), subject);
        QCOMPARE(elided(QString()), QString());
        QCOMPARE(elidedText(QString()), QString());
    }

    void aLongLineIsCut() {
        const QString line(4 * 1024 * 1024, QLatin1Char('a'));
        const QString shown = elided(line);
        QCOMPARE(shown.size(), kMaxLineChars + 1);
        QCOMPARE(shown, QString(kMaxLineChars, QLatin1Char('a')) + kEllipsis);
        QCOMPARE(elided(line, 10), QStringLiteral("aaaaaaaaaa") + kEllipsis);
        // One past the cap is cut too.
        QCOMPARE(elided(QString(kMaxLineChars + 1, QLatin1Char('a'))), shown);
    }

    void aTextIsCutByLineAndInAll() {
        // A long line amid short ones: only that line is cut.
        const QString text = QStringLiteral("subject\n\n")
                             + QString(kMaxParagraphChars + 5, QLatin1Char('b'))
                             + QStringLiteral("\nlast line\n");
        QCOMPARE(elidedText(text),
                 QStringLiteral("subject\n\n") + QString(kMaxParagraphChars, QLatin1Char('b'))
                     + kEllipsis + QStringLiteral("\nlast line\n"));

        // A line of megabytes amid ordinary ones: the lines after it
        // stay in.
        const QString hugeLine = QStringLiteral("first\n") + QString(4 * 1024 * 1024, QLatin1Char('c'))
                                 + QStringLiteral("\nlast\n");
        const QString expected = QStringLiteral("first\n")
                                 + QString(kMaxParagraphChars, QLatin1Char('c')) + kEllipsis
                                 + QStringLiteral("\nlast\n");
        QCOMPARE(elidedText(hugeLine), expected);
        QCOMPARE(elidedText(hugeLine.toStdString()), expected);

        // Many short lines: the whole is cut at the text cap.
        QString lines;
        while (lines.size() <= 2 * kMaxTextChars)
            lines += QStringLiteral("0123456789\n");
        const QString shown = elidedText(lines);
        QVERIFY(shown.size() <= kMaxTextChars + 1);
        QVERIFY(shown.endsWith(kEllipsis));
        QVERIFY(lines.startsWith(shown.chopped(1)));

        // Caps of the caller's own.
        QCOMPARE(elidedText(QStringLiteral("abcdef\ngh"), 3, 100),
                 QStringLiteral("abc") + kEllipsis + QStringLiteral("\ngh"));
        QCOMPARE(elidedText(QStringLiteral("ab\ncd\nef"), 10, 4),
                 QStringLiteral("ab\nc") + kEllipsis);
    }

    // A cut that fell between the two halves of a surrogate pair left a
    // lone high surrogate, which renders as a box.
    void aCutKeepsSurrogatePairsWhole() {
        const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");   // U+1F600, two units
        QCOMPARE(emoji.size(), 2);
        QString text = QStringLiteral("abc");
        for (int i = 0; i < 10; ++i)
            text += emoji;
        // 3 + 2k units: a cap of 4 lands inside the first emoji.
        QCOMPARE(elided(text, 4), QStringLiteral("abc") + kEllipsis);
        QCOMPARE(elided(text, 5), QStringLiteral("abc") + emoji + kEllipsis);
        QCOMPARE(elidedText(text, 6, 100), QStringLiteral("abc") + emoji + kEllipsis);
        QCOMPARE(elided(text.toStdString(), 4), QStringLiteral("abc") + kEllipsis);
    }

    // CommitLogModel calls this on every paint of a row: decoding the
    // whole of a message of megabytes each time, to show its start, was
    // the cost. The window decoded is a few times the cap, and gives the
    // same text a full decode would: multi-byte characters, a sequence
    // cut at the window's edge, and bytes that aren't UTF-8 included.
    void utf8IsDecodedOnlyAsFarAsShown() {
        std::string huge;
        for (int i = 0; i < 300000; ++i)
            huge += "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80" "a\xFF";   // é € 😀 a, a stray byte
        QVERIFY(huge.size() > 3u * 1024 * 1024);
        const QString full = QString::fromStdString(huge);
        QCOMPARE(elided(std::string_view(huge)), elided(full));
        QCOMPARE(elidedText(std::string_view(huge)), elidedText(full));
        for (qsizetype cap : {1, 2, 3, 4, 5, 6, 7, 100, 1001})
            QCOMPARE(elided(std::string_view(huge), cap), elided(full, cap));

        // And quickly: 2,000 times takes some 10 ms here, where decoding
        // all of it each time took 4 s.
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 2000; ++i)
            QVERIFY(elided(std::string_view(huge)).size() <= kMaxLineChars + 1);
        QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
    }
};

QTEST_GUILESS_MAIN(TestMessageText)
#include "TestMessageText.moc"
