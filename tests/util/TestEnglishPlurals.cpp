//
// TestEnglishPlurals — GitBolt's English plural forms
// (util::installEnglishPlurals, translations/gitbolt_en.ts):
//
//   - with no translation loaded, Qt puts the count into a %n text's
//     source as it stands: "1 file(s)", which the app used to show;
//   - once installEnglishPlurals() has run, the same tr() call reads
//     "1 file" and "2 files", in a German locale too (the UI is English
//     in every locale), and a second call adds nothing;
//   - every %n text in the .ts file has a singular and a plural that
//     differ, each with its %n and no "(s)", finished (no
//     type="unfinished"), and the compiled .qm gives exactly them.
//     CI's Linux leg re-runs update_translations, so every %n text in
//     the sources is in the file.
//
// A build without Qt LinguistTools (or Qt before 6.7) has no forms: it
// keeps "(s)", and the tests that need the forms are skipped.
//

#include <QCoreApplication>
#include <QFile>
#include <QLocale>
#include <QTest>
#include <QTranslator>
#include <QXmlStreamReader>

#include "util/Translations.h"

namespace {

// A text from the app, in its class's translation context.
constexpr const char* kContext = "gitbolt::widgets::RepoOperationBar";
constexpr const char* kSource = "Merge stopped on conflicts in %n file(s). Resolve them, then commit.";

QString conflictText(int n) {
    return QCoreApplication::translate(kContext, kSource, nullptr, n);
}

struct NumerusMessage {
    QString context;
    QString source;
    QStringList forms;
    QString type; // the <translation>'s type attribute: "unfinished", "vanished", ...
};

std::vector<NumerusMessage> readNumerusMessages(const QString& path, QString* error) {
    std::vector<NumerusMessage> messages;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return messages;
    }
    // <TS><context><name/><message numerus="yes"><source/>
    // <translation><numerusform/>...</translation></message>...
    QXmlStreamReader xml(&file);
    QString context;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;
        if (xml.name() == QLatin1String("name")) {
            context = xml.readElementText();
        } else if (xml.name() == QLatin1String("message")) {
            NumerusMessage m;
            m.context = context;
            const bool numerus = xml.attributes().value(QLatin1String("numerus")) == QLatin1String("yes");
            while (xml.readNextStartElement()) {        // false at </message>
                if (xml.name() == QLatin1String("source")) {
                    m.source = xml.readElementText();
                } else if (xml.name() == QLatin1String("translation")) {
                    m.type = xml.attributes().value(QLatin1String("type")).toString();
                    while (xml.readNextStartElement()) { // false at </translation>
                        if (xml.name() == QLatin1String("numerusform"))
                            m.forms << xml.readElementText();
                        else
                            xml.skipCurrentElement();
                    }
                } else {
                    xml.skipCurrentElement();
                }
            }
            if (numerus)
                messages.push_back(m);
        }
    }
    if (xml.hasError())
        *error = xml.errorString();
    return messages;
}

} // namespace

class TestEnglishPlurals : public QObject {
    Q_OBJECT

private slots:
    // Runs first: nothing is installed yet.
    void withoutTheFormsTheSourceTextShows() {
        QCOMPARE(conflictText(1),
                 QStringLiteral("Merge stopped on conflicts in 1 file(s). Resolve them, then commit."));
    }

    // Loaded by path, not by locale: a German locale gets the English
    // forms too.
    void installedFormsGiveSingularAndPlural() {
        QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
        QCOMPARE(gitbolt::util::installEnglishPlurals(), gitbolt::util::hasEnglishPlurals());
        if (!gitbolt::util::hasEnglishPlurals())
            QSKIP("built without Qt LinguistTools (or with Qt before 6.7): no English plural forms");

        QCOMPARE(conflictText(1),
                 QStringLiteral("Merge stopped on conflicts in 1 file. Resolve them, then commit."));
        QCOMPARE(conflictText(2),
                 QStringLiteral("Merge stopped on conflicts in 2 files. Resolve them, then commit."));
        QCOMPARE(QCoreApplication::translate("gitbolt::widgets::FileTreeWidget", "%n file(s)", nullptr, 1),
                 QStringLiteral("1 file"));
        QCOMPARE(QCoreApplication::translate("gitbolt::widgets::FileTreeWidget", "%n file(s)", nullptr, 2),
                 QStringLiteral("2 files"));
        // English: every count but one takes the plural.
        QCOMPARE(QCoreApplication::translate("gitbolt::widgets::FileTreeWidget", "%n file(s)", nullptr, 0),
                 QStringLiteral("0 files"));

        // Once only: the second call finds the first translator.
        const auto translators = QCoreApplication::instance()->findChildren<QTranslator*>().size();
        QVERIFY(gitbolt::util::installEnglishPlurals());
        QCOMPARE(QCoreApplication::instance()->findChildren<QTranslator*>().size(), translators);
    }

    // Every %n text in the file has both forms, filled in and finished,
    // and the .qm built into gitbolt_util gives exactly them.
    void everyNumerusTextHasBothForms() {
        if (!gitbolt::util::hasEnglishPlurals())
            QSKIP("built without Qt LinguistTools (or with Qt before 6.7): no English plural forms");
        QVERIFY(gitbolt::util::installEnglishPlurals());

        QString error;
        const auto messages = readNumerusMessages(QStringLiteral(GITBOLT_PLURALS_TS), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(messages.size() >= 10);
        for (const auto& m : messages) {
            const QString where = m.context + QStringLiteral(": ") + m.source;
            QVERIFY2(m.source.contains(QLatin1String("%n")), qPrintable(where));
            QVERIFY2(m.type.isEmpty(), qPrintable(m.type + QStringLiteral(" ") + where));
            QCOMPARE(m.forms.size(), 2);
            const QString& one = m.forms[0];
            const QString& many = m.forms[1];
            QVERIFY2(!one.isEmpty() && !many.isEmpty() && one != many, qPrintable(where));
            for (const auto& form : m.forms) {
                QVERIFY2(form.contains(QLatin1String("%n")), qPrintable(form));
                QVERIFY2(!form.contains(QLatin1String("(s)")) && !form.contains(QLatin1String("(es)")),
                         qPrintable(form));
            }
            const QByteArray context = m.context.toUtf8();
            const QByteArray source = m.source.toUtf8();
            QCOMPARE(QCoreApplication::translate(context.constData(), source.constData(), nullptr, 1),
                     QString(one).replace(QLatin1String("%n"), QLatin1String("1")));
            QCOMPARE(QCoreApplication::translate(context.constData(), source.constData(), nullptr, 3),
                     QString(many).replace(QLatin1String("%n"), QLatin1String("3")));
        }
    }
};

QTEST_GUILESS_MAIN(TestEnglishPlurals)
#include "TestEnglishPlurals.moc"
