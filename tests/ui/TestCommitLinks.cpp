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
//     second href above loaded a local file into the pane.
//
// The URLs QDesktopServices would hand the desktop are caught. The
// browsers are found by what they show, so this builds unchanged
// against the code before the fix.
//

#include <QApplication>
#include <QDesktopServices>
#include <QFile>
#include <QMainWindow>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
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
};

QTEST_MAIN(TestCommitLinks)
#include "TestCommitLinks.moc"
