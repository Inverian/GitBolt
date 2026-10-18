//
// TestCommitLinks — the links in the repository view's Commit tab.
//
//   - an author's or committer's email is the text of one mailto: link
//     to that address and adds nothing to it. The email went into a
//     single-quoted href escaped with toHtmlEscaped(), which leaves '
//     alone, so one such as  x@y' style='…' title='…'  closed the href
//     and gave the link its own styling, a tooltip or a second href
//     (git and libgit2 take an email with quotes, '=' and spaces);
//   - a Parent or Child hash selects that commit, and one the log
//     hasn't loaded says so on the status bar. A click on any link had
//     QTextBrowser look for a document at it, and blank the pane;
//   - a click on an email hands its mailto: URL to the desktop and does
//     nothing else, and a link to anything else isn't followed: the
//     second href above loaded a local file into the pane;
//   - a long author or committer, which wraps, leaves every row in view
//     (or, past the pane's tallest, a scroll bar to reach them). The
//     pane's height came from its count of rows, with no scroll bar, so
//     the rows under a wrapped one went out of sight: the Committer row
//     too, which is how the pane says someone else committed it.
//
// The URLs QDesktopServices would hand the desktop are caught. The
// browsers are found by what they show, so this builds unchanged
// against the code before the fix.
//

#include <QApplication>
#include <QDesktopServices>
#include <QFile>
#include <QMainWindow>
#include <QScrollBar>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QUrl>

#include <algorithm>

#include "git/Commit.h"
#include "models/CommitLogModel.h"
#include "ui/RepositoryView.h"
#include "widgets/RevisionGraphWidget.h"

using gitbolt::git::CommitData;
using gitbolt::git::ObjectId;
using gitbolt::models::CommitLogModel;
using gitbolt::ui::RepositoryView;

namespace {

const QString kChild     = QStringLiteral("2222222222222222222222222222222222222222");
const QString kParent    = QStringLiteral("1111111111111111111111111111111111111111");
// The parent's parent: on a page of the log not loaded yet.
const QString kNotLoaded = QStringLiteral("3333333333333333333333333333333333333333");

CommitData makeCommit(const QString& hex, const QStringList& parents,
                      const std::string& email, const std::string& committerEmail = {}) {
    CommitData c;
    c.id = ObjectId::fromHex(hex.toStdString());
    for (const QString& parent : parents)
        c.parentIds.push_back(ObjectId::fromHex(parent.toStdString()));
    c.author.name = c.committer.name = "Ada";
    c.author.email = email;
    c.committer.email = committerEmail.empty() ? email : committerEmail;
    c.summary = c.message = "A commit";
    return c;
}

// The mailto: URL of an address, every character of it in the path.
QUrl mailto(const QString& address) {
    QUrl url;
    url.setScheme(QStringLiteral("mailto"));
    url.setPath(address, QUrl::DecodedMode);
    return url;
}

// A link as a browser shows it: its text, where that starts, its href
// and how it's drawn.
struct Link {
    QString text;
    int position = -1;
    QString href;
    QTextCharFormat format;
};

QList<Link> linksIn(QTextBrowser* browser) {
    QList<Link> links;
    for (QTextBlock block = browser->document()->begin(); block.isValid();
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isAnchor())
                continue;
            links << Link{fragment.text(), fragment.position(),
                          fragment.charFormat().anchorHref(), fragment.charFormat()};
        }
    }
    return links;
}

// How the plain text containing `text` is drawn.
QTextCharFormat textFormat(QTextBrowser* browser, const QString& text) {
    for (QTextBlock block = browser->document()->begin(); block.isValid();
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && !fragment.charFormat().isAnchor()
                && fragment.text().contains(text))
                return fragment.charFormat();
        }
    }
    return {};
}

// The labels of the details pane's rows.
const QStringList kRowLabels = {
    QStringLiteral("Author:"), QStringLiteral("Date:"), QStringLiteral("Committer:"),
    QStringLiteral("Commit hash:"), QStringLiteral("Parent:"), QStringLiteral("Child:")};

// The rows, of those labelled `labels`, that the details pane has laid
// out where the user can't see them, or doesn't show.
QString rowsOutOfView(QTextBrowser* details, const QStringList& labels = kRowLabels) {
    QStringList hidden;
    for (const QString& label : labels) {
        const QTextCursor found = details->document()->find(label);
        if (found.isNull())
            hidden << label + QStringLiteral(" (not shown at all)");
        else if (!details->viewport()->rect().contains(details->cursorRect(found)))
            hidden << label;
    }
    return hidden.join(QStringLiteral(", "));
}

// The details pane at its tallest: past it, its rows scroll.
constexpr int kTallest = 180;

// Every row of the details pane in view, or the pane at its tallest
// with a scroll bar to reach them. Empty if so, else what isn't.
QString rowsUnreachable(QTextBrowser* details) {
    const bool scrolls = details->verticalScrollBar()->isVisible();
    if (scrolls && details->height() == kTallest)
        return {};
    const QString hidden = rowsOutOfView(details);
    if (hidden.isEmpty() && !scrolls)
        return {};
    return QStringLiteral("out of view: [%1], scroll bar %2, pane %3 px tall")
        .arg(hidden, scrolls ? QStringLiteral("shown") : QStringLiteral("hidden"))
        .arg(details->height());
}

// " word word …": a value that wraps over a line or more.
QString words(int count) {
    QString text;
    for (int i = 0; i < count; ++i)
        text += QStringLiteral(" word");
    return text;
}

QTextBrowser* browserShowing(QWidget& view, const QString& text) {
    for (QTextBrowser* browser : view.findChildren<QTextBrowser*>()) {
        if (browser->toPlainText().contains(text))
            return browser;
    }
    return nullptr;
}

// Clicks the middle of the link whose text is `text`, as the user
// would. False if there's no such link, or it isn't under the mouse.
bool clickLink(QTextBrowser* browser, const QString& text) {
    for (const Link& link : linksIn(browser)) {
        if (link.text != text)
            continue;
        QTextCursor cursor(browser->document());
        cursor.setPosition(link.position + static_cast<int>(link.text.size()) / 2);
        const QPoint at = browser->cursorRect(cursor).center();
        if (browser->anchorAt(at) != link.href)
            return false;
        QTest::mouseClick(browser->viewport(), Qt::LeftButton, {}, at);
        // The next click isn't taken for the second of a double click.
        QTest::qWait(QApplication::doubleClickInterval() + 50);
        return true;
    }
    return false;
}

// Takes the URLs QDesktopServices would hand the desktop, for every
// scheme a link here could name.
class UrlCatcher : public QObject {
    Q_OBJECT
public:
    UrlCatcher() {
        for (const QString& scheme : schemes())
            QDesktopServices::setUrlHandler(scheme, this, "open");
    }
    ~UrlCatcher() override {
        for (const QString& scheme : schemes())
            QDesktopServices::unsetUrlHandler(scheme);
    }
    QList<QUrl> urls;

public slots:
    void open(const QUrl& url) { urls << url; }

private:
    static QStringList schemes() {
        return {QStringLiteral("mailto"), QStringLiteral("file"), QStringLiteral("http"),
                QStringLiteral("https"), QStringLiteral("gitbolt")};
    }
};

} // namespace

class TestCommitLinks : public QObject {
    Q_OBJECT

private slots:
    // An email with attributes in it: the author's styles the link and
    // gives it a tooltip, the committer's adds an href and a style.
    void emailIsOneMailtoLink() {
        const std::string author =
            "x@y.test' style='color:#00aa00;font-size:28px;background-color:#ffee00'"
            " title='TITLE-MARKER";
        const std::string committer =
            "c@y.test' href='file:///nowhere/MARKER.html' style='font-weight:bold";
        CommitLogModel model;
        model.setCommits({makeCommit(kChild, {kParent}, author, committer),
                          makeCommit(kParent, {}, "ada@example.com")});
        UrlCatcher desktop;
        RepositoryView view;
        view.setCommitLogModel(&model);
        view.resize(1200, 900);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QVERIFY(view.revisionGraph()->selectCommit(kChild));
        QTextBrowser* details = browserShowing(view, QStringLiteral("Author:"));
        QVERIFY(details);

        const QList<Link> links = linksIn(details);
        // A link with nothing added, and the text around the emails.
        const auto hash = std::find_if(links.begin(), links.end(),
                                       [](const Link& l) { return l.text == kParent; });
        QVERIFY(hash != links.end());
        const QTextCharFormat plain = textFormat(details, QStringLiteral("Ada"));
        QVERIFY(plain.hasProperty(QTextFormat::FontPixelSize));

        for (const std::string& email : {author, committer}) {
            const QString address = QString::fromStdString(email);
            const auto isIt = [&](const Link& l) { return l.text == address; };
            QCOMPARE(static_cast<int>(std::count_if(links.begin(), links.end(), isIt)), 1);
            const Link link = *std::find_if(links.begin(), links.end(), isIt);
            // One href, the address's own, percent-encoded.
            QCOMPARE(link.href, QString::fromLatin1(mailto(address).toEncoded()));
            QCOMPARE(QUrl(link.href).path(), address);
            QVERIFY(!link.href.contains(QLatin1Char(' ')));
            // Drawn as the hash link and the text around it are.
            QCOMPARE(link.format.foreground().color(), hash->format.foreground().color());
            QCOMPARE(link.format.background().style(), Qt::NoBrush);
            QCOMPARE(link.format.intProperty(QTextFormat::FontPixelSize),
                     plain.intProperty(QTextFormat::FontPixelSize));
            QCOMPARE(link.format.fontWeight(), plain.fontWeight());
            QVERIFY2(link.format.toolTip().isEmpty(), qPrintable(link.format.toolTip()));
        }
        // The two emails and the parent's hash are the only links (the
        // commit is the newest, with no Child to link to).
        QStringList hrefs;
        for (const Link& link : links)
            hrefs << link.href;
        QCOMPARE(hrefs.size(), 3);
        QCOMPARE(hrefs.filter(QStringLiteral("mailto:")).size(), 2);
        QCOMPARE(hrefs.filter(QStringLiteral("gitbolt://commit/") + kParent).size(), 1);
        QVERIFY(desktop.urls.isEmpty());
    }

    // A Parent or Child hash selects that commit, and the pane shows it.
    void hashLinkSelectsItsCommit() {
        CommitLogModel model;
        model.setCommits({makeCommit(kChild, {kParent}, "ada@example.com"),
                          makeCommit(kParent, {kNotLoaded}, "ada@example.com")});
        UrlCatcher desktop;
        QMainWindow window;
        auto* view = new RepositoryView(&window);
        window.setCentralWidget(view);
        view->setCommitLogModel(&model);
        window.resize(1200, 900);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* graph = view->revisionGraph();
        QVERIFY(graph->selectCommit(kChild));
        QTextBrowser* details = browserShowing(*view, QStringLiteral("Author:"));
        QVERIFY(details);

        // Down to the parent, which shows its own parent's hash...
        QVERIFY(clickLink(details, kParent));
        QCOMPARE(graph->currentCommitHash(), kParent);
        QVERIFY(details->source().isEmpty());
        QVERIFY(details->toPlainText().contains(QStringLiteral("Author:")));
        QVERIFY(details->toPlainText().contains(kNotLoaded));

        // ...and back up through its Child row.
        QVERIFY(clickLink(details, kChild));
        QCOMPARE(graph->currentCommitHash(), kChild);
        QVERIFY(details->toPlainText().contains(QStringLiteral("Author:")));
        QVERIFY(!details->toPlainText().contains(kNotLoaded));

        // A parent the log hasn't loaded: said so, and nothing moves.
        QVERIFY(clickLink(details, kParent));
        QVERIFY(clickLink(details, kNotLoaded));
        QCOMPARE(graph->currentCommitHash(), kParent);
        QVERIFY(details->toPlainText().contains(kNotLoaded));
        QVERIFY2(window.statusBar()->currentMessage().contains(kNotLoaded.left(8)),
                 qPrintable(window.statusBar()->currentMessage()));
        QVERIFY(desktop.urls.isEmpty());
    }

    // An email whose second href names a local file: the click hands
    // the mail app the address, and the pane keeps the commit.
    void emailClickOpensOnlyMail() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString markerPath = dir.filePath(QStringLiteral("MARKER.html"));
        {
            QFile marker(markerPath);
            QVERIFY(marker.open(QIODevice::WriteOnly));
            marker.write("<p>MARKER-FILE-CONTENT</p>");
        }
        const QUrl fileUrl = QUrl::fromLocalFile(markerPath);
        const std::string email = "x@y.test' href='" + fileUrl.toString().toStdString();
        CommitLogModel model;
        model.setCommits({makeCommit(kChild, {kParent}, email),
                          makeCommit(kParent, {}, "ada@example.com")});
        UrlCatcher desktop;
        RepositoryView view;
        view.setCommitLogModel(&model);
        view.resize(1200, 900);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QVERIFY(view.revisionGraph()->selectCommit(kChild));
        QTextBrowser* details = browserShowing(view, QStringLiteral("Author:"));
        QTextBrowser* message = browserShowing(view, QStringLiteral("Contained in"));
        QVERIFY(details);
        QVERIFY(message);

        QVERIFY(clickLink(details, QString::fromStdString(email)));
        QCOMPARE(desktop.urls.size(), 1);
        QCOMPARE(desktop.urls.first().scheme(), QStringLiteral("mailto"));
        QCOMPARE(desktop.urls.first().path(), QString::fromStdString(email));
        QVERIFY(details->source().isEmpty());
        QVERIFY(!details->toPlainText().contains(QStringLiteral("MARKER-FILE-CONTENT")));
        QVERIFY(details->toPlainText().contains(QStringLiteral("Author:")));
        QCOMPARE(view.revisionGraph()->currentCommitHash(), kChild);

        // Nor is a link to anything else followed, from either browser.
        desktop.urls.clear();
        const QList<QUrl> others = {
            fileUrl,
            QUrl(QStringLiteral("https://example.invalid/")),
            QUrl(QStringLiteral("gitbolt://commit/not-a-hash")),
            QUrl(QStringLiteral("gitbolt://elsewhere/") + kParent),
        };
        for (QTextBrowser* browser : {details, message}) {
            for (const QUrl& url : others)
                emit browser->anchorClicked(url);
            QVERIFY(browser->source().isEmpty());
        }
        QVERIFY2(desktop.urls.isEmpty(), qPrintable(desktop.urls.value(0).toString()));
        QCOMPARE(view.revisionGraph()->currentCommitHash(), kChild);
        QVERIFY(details->toPlainText().contains(QStringLiteral("Author:")));
        QVERIFY(message->toPlainText().contains(QStringLiteral("Contained in")));

        // The message pane shows no link of its own; one put there
        // isn't followed either when clicked.
        message->setHtml(QStringLiteral("<p><a href=\"%1\">MESSAGE-LINK</a></p>")
                             .arg(fileUrl.toString(QUrl::FullyEncoded).toHtmlEscaped()));
        QVERIFY(clickLink(message, QStringLiteral("MESSAGE-LINK")));
        QVERIFY(message->source().isEmpty());
        QVERIFY(message->toPlainText().contains(QStringLiteral("MESSAGE-LINK")));
        QVERIFY(desktop.urls.isEmpty());
    }

    // A long author name or email wraps; the rows under it, the
    // Committer row among them, stay in view. As the window narrows
    // it wraps over more lines, and past the pane's tallest the pane
    // scrolls instead, with a scroll bar to say so.
    void longAuthorKeepsRowsInView_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("email");
        QTest::newRow("email") << QStringLiteral("Ada")
                               << QStringLiteral("ada@example.com") + words(20);
        QTest::newRow("name") << QStringLiteral("Ada") + words(20)
                              << QStringLiteral("ada@example.com");
    }
    void longAuthorKeepsRowsInView() {
        QFETCH(QString, name);
        QFETCH(QString, email);
        CommitData commit = makeCommit(kParent, {kNotLoaded}, email.toStdString(),
                                       "mallory@example.com");
        commit.author.name = name.toStdString();
        commit.committer.name = "Mallory";
        CommitLogModel model;
        model.setCommits({makeCommit(kChild, {kParent}, "ada@example.com"), commit});
        RepositoryView view;
        view.setCommitLogModel(&model);
        view.resize(1200, 900);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QVERIFY(view.revisionGraph()->selectCommit(kParent));
        QTextBrowser* details = browserShowing(view, QStringLiteral("Author:"));
        QVERIFY(details);
        QVERIFY(details->toPlainText().contains(QStringLiteral("mallory@example.com")));

        QTRY_COMPARE(rowsOutOfView(details), QString());
        QVERIFY(!details->verticalScrollBar()->isVisible());
        // The value wrapped, or this shows nothing.
        const QTextCursor value = details->document()->find(words(20).trimmed());
        QVERIFY(!value.isNull());
        const int lines = value.block().layout()->lineCount();
        QVERIFY(lines > 1);

        for (int width = 1150; width >= 700; width -= 50) {
            view.resize(width, 900);
            // Lays the pane out at the new width before it's looked at.
            QTest::qWait(50);
            QTRY_COMPARE(rowsUnreachable(details), QString());
        }
        // It wrapped further as the window narrowed.
        QVERIFY(value.block().layout()->lineCount() > lines);
    }

    // A value too long for the pane at its tallest: the pane scrolls,
    // so the rows under it can still be reached.
    void tooLongAuthorScrolls() {
        CommitData commit = makeCommit(kParent, {kNotLoaded},
                                       ("ada@example.com" + words(400)).toStdString(),
                                       "mallory@example.com");
        commit.committer.name = "Mallory";
        CommitData child = makeCommit(kChild, {kParent}, "ada@example.com",
                                      "mallory@example.com");
        child.committer.name = "Mallory";
        CommitLogModel model;
        model.setCommits({child, commit});
        RepositoryView view;
        view.setCommitLogModel(&model);
        view.resize(1200, 900);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QVERIFY(view.revisionGraph()->selectCommit(kParent));
        QTextBrowser* details = browserShowing(view, QStringLiteral("Author:"));
        QVERIFY(details);

        QTRY_VERIFY(details->verticalScrollBar()->isVisible());
        QCOMPARE(details->height(), kTallest);
        QVERIFY(!rowsOutOfView(details, {QStringLiteral("Committer:")}).isEmpty());
        // Scrolled to its end (again, while the layout settles), the
        // rows under the value are in view.
        const auto rowsAtEnd = [details] {
            QScrollBar* bar = details->verticalScrollBar();
            bar->setValue(bar->maximum());
            return rowsOutOfView(details, {QStringLiteral("Date:"), QStringLiteral("Committer:")});
        };
        QTRY_COMPARE(rowsAtEnd(), QString());

        // A commit that fits again takes the scroll bar away.
        QVERIFY(view.revisionGraph()->selectCommit(kChild));
        QTRY_VERIFY(!details->verticalScrollBar()->isVisible());
        QTRY_COMPARE(rowsOutOfView(details), QString());
    }
};

QTEST_MAIN(TestCommitLinks)
#include "TestCommitLinks.moc"
