//
// TestInstallHelpers — the scripts that finish an update once GitBolt
// has quit (update/InstallHelpers.h), run for real where they can be:
//
//   - the macOS helper (macOS): against a fake old bundle and a staged
//     new one in a scratch "Applications" folder, with the Trash a
//     scratch folder too (HOME points at one for the whole run). It
//     waits while the pid lives and changes nothing; then swaps the
//     bundles, moves the old one to the Trash (with the time added if
//     its name is taken; deleted if there is no Trash), and starts the
//     new one, whose environment still has GITBOLT_INSTANCE_NAME. When
//     the swap fails (the staged bundle can't be moved: chflags uchg)
//     the old one is put back and started. After its timeout it gives
//     up, removes the staged copy and starts nothing;
//   - the AppImage helper (any Unix): renames the new file over the old
//     one and starts it (extracted again, APPIMAGE_EXTRACT_AND_RUN=1,
//     when this one ran extracted); keeps and starts the old one when
//     it can't; gives up after its timeout; and counts a zombie
//     (exited, not yet reaped) as gone instead of waiting for it;
//   - the deb helper (any Unix, with a stand-in pkexec that records
//     what it was asked to run and exits 0, 126, 127 or 1): pkexec
//     /usr/bin/apt-get install -y on the .deb's absolute path, then
//     /usr/bin/gitbolt's stand-in, whether apt-get worked or not;
//   - every Unix helper's install result: the line each outcome writes
//     (ok, declined, failed with why, gave-up), there before the
//     relaunched copy starts (the fake app copies what it finds);
//   - each Unix script under /bin/sh and, where it is installed, dash
//     (Debian's and Ubuntu's /bin/sh): sh -n on paths with spaces,
//     quotes, $ and newlines, a run for real, and none of the usual
//     bashisms in the text;
//   - shellQuote round-trips any text through sh;
//   - the Windows helper and its elevated part, as text: every path in
//     a single-quoted literal with each kind of quote PowerShell ends
//     one at doubled (twice where the helper carries the elevated
//     script), no double-quoted (interpolated) strings at all, one
//     elevation, the in-use wait before the installer, /D= last and
//     unquoted in the one ArgumentList string, the version check after
//     it, each exit code's install result, no use of $pid, both
//     command lines within CreateProcess's limit even with long paths,
//     and -EncodedCommand's UTF-16LE base64 decoding back to the script;
//   - and for real, on Windows, when GITBOLT_TEST_NSIS_INSTALLER and
//     GITBOLT_TEST_NSIS_DIR name a GitBolt installer and a folder it
//     installed into (CI's NSIS smoke test sets them; skipped
//     otherwise): started as Installer::launchHelper starts it, the
//     helper gives up on a stand-in GitBolt that outlives its timeout;
//     installs nothing while another GitBolt runs from the folder;
//     with time and the folder free, it waits for the stand-in, then
//     runs that installer elevated over the install, checks the new
//     version, writes "ok", and starts the installed GitBolt.exe,
//     which shows its window and is closed.
//

#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include "FakeApps.h"
#include "../TestProcessHelper.h"
#include "update/InstallHelpers.h"
#include "update/Package.h"
#include "update/Version.h"

#include <memory>
#include <optional>
#include <vector>

#if defined(Q_OS_UNIX)
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(Q_OS_WIN)
#include <tlhelp32.h>   // after <windows.h>, which TestProcessHelper.h includes
#endif

using namespace gitbolt::update;
using gitbolt::test::fakeAppScript;
using gitbolt::test::makeBundle;
using gitbolt::test::readFile;
using gitbolt::test::runTool;
using gitbolt::test::ScopedEnv;
using gitbolt::test::writeFile;

namespace {

const QString kInstance = QStringLiteral("gitbolt-update-helper-test");

// The shells a Unix helper must run under: /bin/sh, and dash where it
// is another program (macOS has it; on Debian it is /bin/sh already).
QStringList shells() {
    QStringList found{QStringLiteral("/bin/sh")};
    const QFileInfo dash(QStringLiteral("/bin/dash"));
    if (dash.isExecutable()
        && dash.canonicalFilePath() != QFileInfo(QStringLiteral("/bin/sh")).canonicalFilePath())
        found << dash.absoluteFilePath();
    return found;
}

void addShellRows() {
    QTest::addColumn<QString>("shell");
    for (const QString& shell : shells())
        QTest::newRow(qPrintable(QFileInfo(shell).fileName())) << shell;
}

bool isPowerShellQuote(QChar c) {
    const char16_t u = c.unicode();
    return u == u'\'' || u == u'\u2018' || u == u'\u2019' || u == u'\u201A' || u == u'\u201B';
}

// The single-quoted literals of a line of PowerShell, unquoted the way
// its tokenizer does: two quote characters in a row (any kind) are one
// of the first. False if a literal isn't closed on the line.
bool powerShellLiterals(const QString& line, QStringList* literals) {
    for (qsizetype i = 0; i < line.size(); ++i) {
        if (line[i] == u'#')
            return true;   // comments are whole lines here
        if (!isPowerShellQuote(line[i]))
            continue;
        QString value;
        for (++i;; ++i) {
            if (i >= line.size())
                return false;
            if (isPowerShellQuote(line[i])) {
                if (i + 1 < line.size() && isPowerShellQuote(line[i + 1])) {
                    value += line[i];
                    ++i;
                    continue;
                }
                break;
            }
            value += line[i];
        }
        literals->append(value);
    }
    return true;
}

#if defined(Q_OS_WIN)
// Windows PowerShell by absolute path, as Installer::launchHelper runs
// it (CreateProcess looks in the application's folder first).
QString windowsPowerShell() {
    QString root = qEnvironmentVariable("SystemRoot");
    if (root.isEmpty())
        root = QStringLiteral("C:\\Windows");
    return root + QStringLiteral("\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
}

// The ids of the running processes whose program is the file `image`.
QList<DWORD> processesOf(const QString& image) {
    QList<DWORD> found;
    const QString wanted = QFileInfo(image).canonicalFilePath();
    if (wanted.isEmpty())
        return found;
    const QString name = QFileInfo(image).fileName();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return found;
    PROCESSENTRY32W entry = {};
    entry.dwSize = static_cast<DWORD>(sizeof(entry));
    std::vector<wchar_t> program(32768);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (QString::fromWCharArray(entry.szExeFile).compare(name, Qt::CaseInsensitive) != 0)
            continue;
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process)
            continue;
        DWORD size = static_cast<DWORD>(program.size());
        if (QueryFullProcessImageNameW(process, 0, program.data(), &size)) {
            const QString file = QString::fromWCharArray(program.data(), static_cast<qsizetype>(size));
            if (QFileInfo(file).canonicalFilePath().compare(wanted, Qt::CaseInsensitive) == 0)
                found.append(entry.th32ProcessID);
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return found;
}

struct WindowSearch {
    DWORD pid = 0;
    bool askToClose = false;
    int visible = 0;
};

BOOL CALLBACK visitWindow(HWND window, LPARAM param) {
    auto* search = reinterpret_cast<WindowSearch*>(param);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == search->pid && IsWindowVisible(window)) {
        ++search->visible;
        if (search->askToClose)
            PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

// How many visible top-level windows `pid` has. With `askToClose`, each
// is sent WM_CLOSE, as its close button would.
int visibleWindows(DWORD pid, bool askToClose = false) {
    WindowSearch search{pid, askToClose, 0};
    EnumWindows(visitWindow, reinterpret_cast<LPARAM>(&search));
    return search.visible;
}

// Ends `pid` outright and waits (a while) for it to be gone.
void endProcess(DWORD pid) {
    const HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!process)
        return;
    TerminateProcess(process, 1);
    WaitForSingleObject(process, 10000);
    CloseHandle(process);
}

// Closes a GitBolt as a user would: WM_CLOSE to each of its windows
// every half second (a dialog, then the main window), until it quits.
// Ends it if it is still there after `timeoutMs`. True if it quit.
bool closeGitBolt(DWORD pid, int timeoutMs) {
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process)
        return true;
    const QDeadlineTimer deadline(timeoutMs);
    bool quit = false;
    for (;;) {
        quit = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        if (quit || deadline.hasExpired())
            break;
        visibleWindows(pid, true);
        WaitForSingleObject(process, 500);
    }
    CloseHandle(process);
    if (!quit)
        endProcess(pid);
    return quit;
}
#endif

} // namespace

class TestInstallHelpers : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> dir_;
    std::unique_ptr<ScopedEnv> home_;
    std::unique_ptr<QProcess> waited_;   // the "GitBolt" a helper waits for
    std::unique_ptr<QProcess> helper_;

    QString path(const QString& relative) const { return QDir::cleanPath(dir_->filePath(relative)); }
    QString marker() const { return path(QStringLiteral("marker.txt")); }
    QString log() const { return path(QStringLiteral("updates/install.log")); }
    QString result() const { return path(QStringLiteral("updates/install-result")); }
    // The install result as the relaunched GitBolt found it when it
    // started (the fake app copies it), so it was written before.
    QString resultAtRelaunch() const { return readFile(marker() + QStringLiteral(".result")); }

    // A process for the helper to wait for, as GitBolt would be.
    qint64 startWaited() {
        waited_ = std::make_unique<QProcess>();
        waited_->start(QStringLiteral("/bin/sleep"), {QStringLiteral("60")});
        if (!waited_->waitForStarted())
            return 0;
        return waited_->processId();
    }

    // GitBolt quits: the process is gone and reaped.
    void quitWaited() {
        waited_->kill();
        waited_->waitForFinished();
    }

    // Starts `script` under `shell`, as Installer::launchHelper would
    // (the same environment, plus where fake apps write their marker).
    bool startHelper(const QString& shell, const QString& script) {
        const QString file = path(QStringLiteral("updates/install-helper.sh"));
        if (!writeFile(file, script.toUtf8()))
            return false;
        helper_ = std::make_unique<QProcess>();
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GITBOLT_TEST_MARKER"), marker());
        env.insert(QStringLiteral("GITBOLT_TEST_RESULT"), result());
        env.insert(QStringLiteral("GITBOLT_INSTANCE_NAME"), kInstance);
        env.remove(QStringLiteral("APPIMAGE_EXTRACT_AND_RUN"));
        helper_->setProcessEnvironment(env);
        helper_->setWorkingDirectory(path(QStringLiteral("updates")));
        helper_->start(shell, {file});
        return helper_->waitForStarted();
    }

    // The helper has started waiting (its log says so: its first line,
    // then "waiting for GitBolt"), and a moment later still is.
    bool helperIsWaiting() {
        if (!gitbolt::test::waitForLines(log(), 2, 10000))
            return false;
        QTest::qWait(400);
        return helper_->state() == QProcess::Running && readFile(log()).contains(QLatin1String("waiting for GitBolt"));
    }

    bool helperFinished(int timeoutMs = 20000) {
        return helper_->state() == QProcess::NotRunning || helper_->waitForFinished(timeoutMs);
    }

    QString diagnostics() const {
        return QStringLiteral("log:\n%1\nmarker:\n%2\nresult:\n%3")
            .arg(readFile(log()), readFile(marker()), readFile(result()));
    }

    // What every helper is told about the result: the new version, and
    // where to write it.
    void reportTo(HelperInputs& in) const {
        in.newVersion = QStringLiteral("0.1.1");
        in.resultFile = result();
    }

    // Mac: <dir>/Applications/GitBolt.app (0.1.0, with a file planted
    // in it), its staged successor beside it (0.1.1), and a Trash.
    HelperInputs macInputs(qint64 pid) const {
        HelperInputs in;
        in.pid = pid;
        in.waitSeconds = 30;
        in.logFile = log();
        reportTo(in);
        in.target = path(QStringLiteral("Applications/GitBolt.app"));
        in.staged = path(QStringLiteral("Applications/.GitBolt-0.1.1.app"));
        in.oldVersion = QStringLiteral("0.1.0");
        in.trashDir = path(QStringLiteral("home/.Trash"));
        in.executable = QStringLiteral("Contents/MacOS/GitBolt");
        in.execDirectly = true;
        return in;
    }

    bool layOutMac(const HelperInputs& in) {
        return makeBundle(in.target, QStringLiteral("0.1.0"))
               && writeFile(in.target + QStringLiteral("/Contents/Resources/planted.txt"), "old\n")
               && makeBundle(in.staged, QStringLiteral("0.1.1")) && QDir().mkpath(in.trashDir);
    }

    static QString copyOf(const QString& bundle) {
        return readFile(bundle + QStringLiteral("/Contents/Resources/copy.txt")).trimmed();
    }

    QStringList entries(const QString& relative) const {
        return QDir(path(relative)).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                                              QDir::Name);
    }

    // AppImage: the old one and the new one, side by side.
    HelperInputs appImageInputs(qint64 pid) const {
        HelperInputs in;
        in.pid = pid;
        in.waitSeconds = 30;
        in.logFile = log();
        reportTo(in);
        in.target = path(QStringLiteral("My Apps/GitBolt-0.1.0-x86_64.AppImage"));
        in.staged = path(QStringLiteral("My Apps/GitBolt-0.1.1-x86_64.AppImage"));
        return in;
    }

    bool layOutAppImage(const HelperInputs& in) {
        return writeFile(in.target, fakeAppScript(QStringLiteral("0.1.0")), true)
               && writeFile(in.staged, fakeAppScript(QStringLiteral("0.1.1")), true);
    }

    // Deb: a stand-in pkexec that records its arguments, one a line,
    // and exits with $GITBOLT_TEST_PKEXEC_STATUS; a stand-in gitbolt.
    HelperInputs debInputs(qint64 pid) {
        HelperInputs in;
        in.pid = pid;
        in.waitSeconds = 30;
        in.logFile = log();
        reportTo(in);
        in.staged = path(QStringLiteral("cache dir/it's $HOME/gitbolt_0.1.1_amd64.deb"));
        in.pkexec = path(QStringLiteral("bin/pkexec"));
        in.executable = path(QStringLiteral("bin/gitbolt"));
        return in;
    }

    bool layOutDeb(const HelperInputs& in, int pkexecStatus) {
        const QByteArray pkexec = "#!/bin/sh\n"
                                  "for a in \"$@\"; do printf '%s\\n' \"$a\"; done >\"" + path(QStringLiteral("pkexec-args.txt")).toUtf8()
                                  + "\"\nexit " + QByteArray::number(pkexecStatus) + "\n";
        return writeFile(in.staged, "!<arch>\n") && writeFile(in.pkexec, pkexec, true)
               && writeFile(in.executable, fakeAppScript(QStringLiteral("relaunched")), true);
    }

    static HelperInputs nastyInputs() {
        HelperInputs in;
        in.pid = 12345;
        in.logFile = QStringLiteral("/tmp/it's a \"log\"/$HOME/`id`/install.log");
        in.resultFile = QStringLiteral("/tmp/it's a \"log\"/$HOME/`id`/install-result");
        in.newVersion = QStringLiteral("0.1.1");
        in.target = QStringLiteral("/Applications/Git'Bolt $(id).app");
        in.staged = QStringLiteral("/Applications/.GitBolt-0.1.1.app\nrm -rf ~");
        in.oldVersion = QStringLiteral("0.1.0");
        in.trashDir = QStringLiteral("/Users/o'brien/.Trash");
        in.executable = QStringLiteral("Contents/MacOS/GitBolt");
        in.pkexec = QStringLiteral("/usr/bin/pkexec");
        return in;
    }

private slots:
    void initTestCase() {
#if defined(Q_OS_UNIX)
        // No helper here touches the real Trash: each one is handed a
        // scratch folder, and HOME is one too, for anything that looks.
        static QTemporaryDir home;
        QVERIFY(home.isValid());
        QVERIFY(QDir().mkpath(home.filePath(QStringLiteral(".Trash"))));
        home_ = std::make_unique<ScopedEnv>("HOME", QFile::encodeName(home.path()));
#endif
    }

    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        QVERIFY(QDir().mkpath(path(QStringLiteral("updates"))));
    }

    void cleanup() {
        if (helper_ && helper_->state() != QProcess::NotRunning) {
            helper_->kill();
            helper_->waitForFinished();
        }
        if (waited_ && waited_->state() != QProcess::NotRunning) {
            waited_->kill();
            waited_->waitForFinished();
        }
        helper_.reset();
        waited_.reset();
#if defined(Q_OS_MACOS)
        // A test that made something immutable undoes it, but a failed
        // one may not have.
        runTool(QStringLiteral("/usr/bin/chflags"), {QStringLiteral("-R"), QStringLiteral("nouchg"), dir_->path()});
#endif
        dir_.reset();
    }

    // ---------------------------------------------------------------
    // Quoting and syntax
    // ---------------------------------------------------------------

    void shellQuoteRoundTrips_data() { addShellRows(); }

    void shellQuoteRoundTrips() {
#if !defined(Q_OS_UNIX)
        QSKIP("POSIX sh only");
#else
        QFETCH(QString, shell);
        const QStringList texts = {QString(), QStringLiteral("plain"), QStringLiteral("two words"),
                                   QStringLiteral("it's"), QStringLiteral("'"), QStringLiteral("''"),
                                   QStringLiteral("a\"b"), QStringLiteral("$HOME"), QStringLiteral("`id`"),
                                   QStringLiteral("$(id)"), QStringLiteral("back\\slash"),
                                   QStringLiteral("new\nline"), QStringLiteral("tab\there"),
                                   QStringLiteral("ünïcødé ’quotes’"), QStringLiteral("*?[a]~"),
                                   QStringLiteral("-n"), QStringLiteral("!event; rm -rf /")};
        // From a file, as the helpers are run: on macOS, QProcess hands
        // arguments over decomposed (NFD), a script file keeps its bytes.
        const QString file = path(QStringLiteral("quote.sh"));
        for (const QString& text : texts) {
            QVERIFY(writeFile(file, (QStringLiteral("printf '%s' ") + shellQuote(text) + u'\n').toUtf8()));
            QProcess sh;
            sh.start(shell, {file});
            QVERIFY(sh.waitForFinished());
            QCOMPARE(sh.exitCode(), 0);
            QCOMPARE(QString::fromUtf8(sh.readAllStandardOutput()), text);
        }
#endif
    }

    void unixScriptsParse_data() { addShellRows(); }

    // sh -n on each script, built from paths that break naive quoting.
    void unixScriptsParse() {
#if !defined(Q_OS_UNIX)
        QSKIP("POSIX sh only");
#else
        QFETCH(QString, shell);
        const HelperInputs in = nastyInputs();
        const QList<QPair<const char*, QString>> scripts = {
            {"macOS", macHelperScript(in)}, {"AppImage", appImageHelperScript(in)}, {"deb", debHelperScript(in)}};
        for (const auto& [name, script] : scripts) {
            const QString file = path(QStringLiteral("syntax-%1.sh").arg(QLatin1String(name)));
            QVERIFY(writeFile(file, script.toUtf8()));
            QString output;
            QVERIFY2(runTool(shell, {QStringLiteral("-n"), file}, &output) == 0,
                     qPrintable(QStringLiteral("%1 helper under %2: %3").arg(QLatin1String(name), shell, output)));
        }
#endif
    }

    // What checkbashisms would catch first, in case a later edit slips
    // one in (dash would choke on most of them only at run time).
    void unixScriptsHaveNoBashisms() {
        const HelperInputs in = nastyInputs();
        static const QRegularExpression bashism(QStringLiteral(
            R"((\[\[|\]\]|\s==\s|^\s*function\s|\blocal\s|\$'|<<<|&>|\bsource\s|\becho\s+-[en]|\bdeclare\s|\btypeset\s|)"
            R"(\$RANDOM|\$\{[A-Za-z_]+(:[0-9]|/|\^|,)|\bpushd\b|\bpopd\b|\bselect\s))"),
            QRegularExpression::MultilineOption);
        for (const QString& script : {macHelperScript(in), appImageHelperScript(in), debHelperScript(in)}) {
            QVERIFY(script.startsWith(QLatin1String("#!/bin/sh\n")));
            const QRegularExpressionMatch match = bashism.match(script);
            QVERIFY2(!match.hasMatch(), qPrintable(match.captured(0)));
        }
    }

    // ---------------------------------------------------------------
    // macOS
    // ---------------------------------------------------------------

    void macSwapsTheBundleAndTrashesTheOldOne_data() { addShellRows(); }

    void macSwapsTheBundleAndTrashesTheOldOne() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS only");
#else
        QFETCH(QString, shell);
        const HelperInputs in = macInputs(startWaited());
        QVERIFY(in.pid > 0);
        QVERIFY(layOutMac(in));
        QVERIFY(startHelper(shell, macHelperScript(in)));

        // While GitBolt runs, nothing is touched.
        QVERIFY2(helperIsWaiting(), qPrintable(diagnostics()));
        QCOMPARE(copyOf(in.target), QStringLiteral("0.1.0"));
        QVERIFY(QFileInfo(in.staged).isDir());
        QVERIFY(!QFileInfo::exists(marker()));

        quitWaited();
        QVERIFY2(helperFinished(), qPrintable(diagnostics()));
        QVERIFY2(QFileInfo::exists(marker()), qPrintable(diagnostics()));

        // The new copy in place, under the old one's name, and started
        // with the environment the helper had.
        QCOMPARE(copyOf(in.target), QStringLiteral("0.1.1"));
        QVERIFY(!QFileInfo::exists(in.target + QStringLiteral("/Contents/Resources/planted.txt")));
        const QString started = readFile(marker());
        QVERIFY2(started.startsWith(QLatin1String("0.1.1 ")), qPrintable(started));
        QVERIFY2(started.contains(QStringLiteral("instance=") + kInstance), qPrintable(started));
        QCOMPARE(started.count(u'\n'), 1);
        // The old one in the Trash, as GitBolt-<old>.app; nothing left
        // beside the new one.
        const QString trashed = in.trashDir + QStringLiteral("/GitBolt-0.1.0.app");
        QCOMPARE(copyOf(trashed), QStringLiteral("0.1.0"));
        QVERIFY(QFileInfo::exists(trashed + QStringLiteral("/Contents/Resources/planted.txt")));
        QCOMPARE(entries(QStringLiteral("Applications")), QStringList{QStringLiteral("GitBolt.app")});
        QVERIFY2(readFile(log()).contains(QLatin1String("moved the old copy to")), qPrintable(diagnostics()));
        // The result, written before the new copy started (which found
        // it): installed, no reason.
        QCOMPARE(readFile(result()), QStringLiteral("ok 0.1.1 \n"));
        QCOMPARE(resultAtRelaunch(), QStringLiteral("ok 0.1.1 \n"));
#endif
    }

    // A Trash that already has GitBolt-0.1.0.app gets the time added,
    // as Finder does.
    void macAddsTheTimeToATakenTrashName() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS only");
#else
        const HelperInputs in = macInputs(startWaited());
        QVERIFY(layOutMac(in));
        QVERIFY(makeBundle(in.trashDir + QStringLiteral("/GitBolt-0.1.0.app"), QStringLiteral("0.1.0"),
                           QStringLiteral("com.gitbolt.app"), QStringLiteral("earlier")));
        QVERIFY(startHelper(QStringLiteral("/bin/sh"), macHelperScript(in)));
        QVERIFY2(helperIsWaiting(), qPrintable(diagnostics()));
        quitWaited();
        QVERIFY2(helperFinished(), qPrintable(diagnostics()));

        QCOMPARE(copyOf(in.target), QStringLiteral("0.1.1"));
        QStringList trash = entries(QStringLiteral("home/.Trash"));
        QCOMPARE(trash.size(), 2);
        QVERIFY(trash.removeOne(QStringLiteral("GitBolt-0.1.0.app")));
        QVERIFY2(QRegularExpression(QStringLiteral("^GitBolt-0\\.1\\.0 \\d\\d\\.\\d\\d\\.\\d\\d\\.app$"))
                     .match(trash.at(0))
                     .hasMatch(),
                 qPrintable(trash.join(u',')));
        QCOMPARE(copyOf(in.trashDir + u'/' + trash.at(0)), QStringLiteral("0.1.0"));
        QCOMPARE(copyOf(in.trashDir + QStringLiteral("/GitBolt-0.1.0.app")), QStringLiteral("earlier"));
#endif
    }

    // No Trash to put it in: the old copy is deleted, not left beside
    // the new one.
    void macDeletesTheOldCopyWithoutATrash() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS only");
#else
        HelperInputs in = macInputs(startWaited());
        QVERIFY(layOutMac(in));
        in.trashDir = path(QStringLiteral("home/no-trash-here"));
        QVERIFY(startHelper(QStringLiteral("/bin/sh"), macHelperScript(in)));
        QVERIFY2(helperIsWaiting(), qPrintable(diagnostics()));
        quitWaited();
        QVERIFY2(helperFinished(), qPrintable(diagnostics()));

        QCOMPARE(copyOf(in.target), QStringLiteral("0.1.1"));
        QCOMPARE(entries(QStringLiteral("Applications")), QStringList{QStringLiteral("GitBolt.app")});
        QVERIFY(!QFileInfo::exists(in.trashDir));
        QVERIFY2(readFile(log()).contains(QLatin1String("deleted the old copy")), qPrintable(diagnostics()));
        QVERIFY(readFile(marker()).startsWith(QLatin1String("0.1.1 ")));
#endif
    }

    void macPutsTheOldOneBackWhenTheSwapFails_data() { addShellRows(); }

    // The staged bundle can't be renamed (immutable): the old one, by
    // then moved aside, goes back and is started.
    void macPutsTheOldOneBackWhenTheSwapFails() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS only");
#else
        QFETCH(QString, shell);
        const HelperInputs in = macInputs(startWaited());
        QVERIFY(layOutMac(in));
        QCOMPARE(runTool(QStringLiteral("/usr/bin/chflags"), {QStringLiteral("uchg"), in.staged}), 0);
        auto unlock = qScopeGuard([&] {
            runTool(QStringLiteral("/usr/bin/chflags"), {QStringLiteral("nouchg"), in.staged});
        });
        QVERIFY(startHelper(shell, macHelperScript(in)));
        QVERIFY2(helperIsWaiting(), qPrintable(diagnostics()));
        quitWaited();
        QVERIFY2(helperFinished(), qPrintable(diagnostics()));

        QCOMPARE(copyOf(in.target), QStringLiteral("0.1.0"));
        QVERIFY(QFileInfo::exists(in.target + QStringLiteral("/Contents/Resources/planted.txt")));
        QVERIFY(!QFileInfo::exists(path(QStringLiteral("Applications/.GitBolt-0.1.0.app.old"))));
        QVERIFY(entries(QStringLiteral("home/.Trash")).isEmpty());
        QVERIFY2(readFile(log()).contains(QLatin1String("putting the old one back")), qPrintable(diagnostics()));
        const QString started = readFile(marker());
        QVERIFY2(started.startsWith(QLatin1String("0.1.0 ")), qPrintable(started));
        // Rolled back: failed, and the old copy found that when it
        // started.
        const QString failed =
            QStringLiteral("failed 0.1.1 the new copy couldn't be moved into place, so the old one was put back\n");
        QCOMPARE(readFile(result()), failed);
        QCOMPARE(resultAtRelaunch(), failed);
#endif
    }

    // GitBolt never quits: after the timeout the helper removes the
    // staged copy, leaves the installed one alone, and starts nothing.
    void macGivesUpAfterTheTimeout() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS only");
#else
        HelperInputs in = macInputs(startWaited());
        in.waitSeconds = 1;
        QVERIFY(layOutMac(in));
        QVERIFY(startHelper(QStringLiteral("/bin/sh"), macHelperScript(in)));
        QVERIFY2(helperFinished(10000), qPrintable(diagnostics()));
        QCOMPARE(helper_->exitCode(), 1);
        QVERIFY(waited_->state() == QProcess::Running);
        QCOMPARE(copyOf(in.target), QStringLiteral("0.1.0"));
        QVERIFY(!QFileInfo::exists(in.staged));
        QVERIFY(!QFileInfo::exists(marker()));
        QVERIFY2(readFile(log()).contains(QLatin1String("giving up")), qPrintable(diagnostics()));
        QCOMPARE(readFile(result()), QStringLiteral("gave-up 0.1.1 GitBolt didn't quit within 1 seconds\n"));
#endif
    }

    // The staged copy went missing: nothing is swapped, and the old
    // GitBolt starts again.
    void macStartsTheOldOneWhenTheNewOneIsMissing() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS only");
#else
        const HelperInputs in = macInputs(startWaited());
        QVERIFY(layOutMac(in));
        QVERIFY(QDir(in.staged).removeRecursively());
        QVERIFY(startHelper(QStringLiteral("/bin/sh"), macHelperScript(in)));
        QVERIFY2(helperIsWaiting(), qPrintable(diagnostics()));
        quitWaited();
        QVERIFY2(helperFinished(), qPrintable(diagnostics()));
        QCOMPARE(copyOf(in.target), QStringLiteral("0.1.0"));
        QVERIFY(readFile(marker()).startsWith(QLatin1String("0.1.0 ")));
        QCOMPARE(entries(QStringLiteral("Applications")), QStringList{QStringLiteral("GitBolt.app")});
        QCOMPARE(resultAtRelaunch(), QStringLiteral("failed 0.1.1 the new copy was missing\n"));
#endif
    }

    // The installed bundle can't be moved aside (its folder is
    // read-only): nothing changes, the result says failed, and the old
    // copy starts.
    void macReportsAnOldCopyItCantMove() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS only");
#else
        const HelperInputs in = macInputs(startWaited());
        QVERIFY(layOutMac(in));
        QCOMPARE(runTool(QStringLiteral("/usr/bin/chflags"), {QStringLiteral("uchg"), in.target}), 0);
        auto unlock = qScopeGuard([&] {
            runTool(QStringLiteral("/usr/bin/chflags"), {QStringLiteral("nouchg"), in.target});
        });
        QVERIFY(startHelper(QStringLiteral("/bin/sh"), macHelperScript(in)));
        QVERIFY2(helperIsWaiting(), qPrintable(diagnostics()));
        quitWaited();
        QVERIFY2(helperFinished(), qPrintable(diagnostics()));
        QCOMPARE(copyOf(in.target), QStringLiteral("0.1.0"));
        QVERIFY(readFile(marker()).startsWith(QLatin1String("0.1.0 ")));
        QCOMPARE(resultAtRelaunch(), QStringLiteral("failed 0.1.1 the old copy couldn't be moved aside\n"));
#endif
    }

    // Outside test sessions the bundle is opened as Finder would (no
    // test runs that: it would really launch something).
    void macOpensTheBundleOutsideTests() {
        HelperInputs in = nastyInputs();
        in.execDirectly = false;
        const QString script = macHelperScript(in);
        QVERIFY(script.contains(QLatin1String("\nexec_directly=0\n")));
        QVERIFY(script.contains(QLatin1String("    exec /usr/bin/open \"$1\"\n")));
        in.execDirectly = true;
        QVERIFY(macHelperScript(in).contains(QLatin1String("\nexec_directly=1\n")));
        // The names it moves things to, from the target's folder and
        // the old version.
        QVERIFY(script.contains(QLatin1String("\naside='/Applications/.GitBolt-0.1.0.app.old'\n")));
        QVERIFY(script.contains(QLatin1String("\ntrash_name='GitBolt-0.1.0'\n")));
        QVERIFY(script.contains(QLatin1String("\ntrash='/Users/o'\\''brien/.Trash'\n")));
    }

    // ---------------------------------------------------------------
    // AppImage
    // ---------------------------------------------------------------

    void appImageReplacesTheFileAndStartsIt_data() { addShellRows(); }

    void appImageReplacesTheFileAndStartsIt() {
#if !defined(Q_OS_UNIX)
        QSKIP("POSIX sh only");
#else
        QFETCH(QString, shell);
        const HelperInputs in = appImageInputs(startWaited());
        QVERIFY(layOutAppImage(in));
        QVERIFY(startHelper(shell, appImageHelperScript(in)));
        QVERIFY2(helperIsWaiting(), qPrintable(diagnostics()));
        QVERIFY(readFile(in.target).contains(QLatin1String("'0.1.0'")));
        quitWaited();
        QVERIFY2(helperFinished(), qPrintable(diagnostics()));

        QVERIFY(readFile(in.target).contains(QLatin1String("'0.1.1'")));
        QVERIFY(QFileInfo(in.target).isExecutable());
        QVERIFY(!QFileInfo::exists(in.staged));
        const QString started = readFile(marker());
        QVERIFY2(started.startsWith(QLatin1String("0.1.1 ")), qPrintable(started));
        QVERIFY2(started.contains(QStringLiteral("instance=") + kInstance), qPrintable(started));
        QCOMPARE(readFile(result()), QStringLiteral("ok 0.1.1 \n"));
        QCOMPARE(resultAtRelaunch(), QStringLiteral("ok 0.1.1 \n"));
#endif
    }

    void appImageKeepsTheOldOneWhenItCantReplaceIt_data() { addShellRows(); }

    // Something other than a file where the new one should be: the old
    // AppImage stays and starts.
    void appImageKeepsTheOldOneWhenItCantReplaceIt() {
#if !defined(Q_OS_UNIX)
        QSKIP("POSIX sh only");
#else
        QFETCH(QString, shell);
        const HelperInputs in = appImageInputs(startWaited());
        QVERIFY(writeFile(in.target, fakeAppScript(QStringLiteral("0.1.0")), true));
        QVERIFY(QDir().mkpath(in.staged));
        QVERIFY(startHelper(shell, appImageHelperScript(in)));
        QVERIFY2(helperIsWaiting(), qPrintable(diagnostics()));
        quitWaited();
        QVERIFY2(helperFinished(), qPrintable(diagnostics()));
        QVERIFY(readFile(in.target).contains(QLatin1String("'0.1.0'")));
        QVERIFY(readFile(marker()).startsWith(QLatin1String("0.1.0 ")));
        QVERIFY2(readFile(log()).contains(QLatin1String("keeping the old one")), qPrintable(diagnostics()));
        QCOMPARE(resultAtRelaunch(),
                 QStringLiteral("failed 0.1.1 the new AppImage couldn't be moved over the old one\n"));
#endif
    }

    void appImageGivesUpAfterTheTimeout() {
#if !defined(Q_OS_UNIX)
        QSKIP("POSIX sh only");
#else
        HelperInputs in = appImageInputs(startWaited());
        in.waitSeconds = 1;
        QVERIFY(layOutAppImage(in));
        QVERIFY(startHelper(QStringLiteral("/bin/sh"), appImageHelperScript(in)));
        QVERIFY2(helperFinished(10000), qPrintable(diagnostics()));
        QCOMPARE(helper_->exitCode(), 1);
        QVERIFY(readFile(in.target).contains(QLatin1String("'0.1.0'")));
        QVERIFY(!QFileInfo::exists(in.staged));
        QVERIFY(!QFileInfo::exists(marker()));
        QVERIFY2(readFile(log()).contains(QLatin1String("giving up")), qPrintable(diagnostics()));
        QCOMPARE(readFile(result()), QStringLiteral("gave-up 0.1.1 GitBolt didn't quit within 1 seconds\n"));
#endif
    }

    // Started with --appimage-extract-and-run (no FUSE), which the
    // runtime consumes without setting APPIMAGE_EXTRACT_AND_RUN: the new
    // copy is started with it, or it would try FUSE and never come up.
    void appImageRestartsExtractedWhenThisOneWas_data() { addShellRows(); }

    void appImageRestartsExtractedWhenThisOneWas() {
#if !defined(Q_OS_UNIX)
        QSKIP("POSIX sh only");
#else
        QFETCH(QString, shell);
        HelperInputs in = appImageInputs(startWaited());
        in.extractAndRun = true;
        QVERIFY(writeFile(in.target, fakeAppScript(QStringLiteral("0.1.0")), true));
        const QByteArray reportsIt = "#!/bin/sh\n"
                                     "printf 'extract=%s\\n' \"${APPIMAGE_EXTRACT_AND_RUN-unset}\" >>\"$GITBOLT_TEST_MARKER\"\n";
        QVERIFY(writeFile(in.staged, reportsIt, true));
        QVERIFY(startHelper(shell, appImageHelperScript(in)));
        QVERIFY2(helperIsWaiting(), qPrintable(diagnostics()));
        quitWaited();
        QVERIFY2(helperFinished(), qPrintable(diagnostics()));
        QCOMPARE(readFile(marker()), QStringLiteral("extract=1\n"));

        // Not asked for, not set (startHelper's environment has none).
        in.extractAndRun = false;
        QVERIFY(!appImageHelperScript(in).contains(QLatin1String("APPIMAGE_EXTRACT_AND_RUN")));
        in.extractAndRun = true;
        const QString script = appImageHelperScript(in);
        QVERIFY(script.contains(QLatin1String("\nAPPIMAGE_EXTRACT_AND_RUN=1\nexport APPIMAGE_EXTRACT_AND_RUN\n")));
        QVERIFY(script.indexOf(QLatin1String("export APPIMAGE_EXTRACT_AND_RUN"))
                < script.indexOf(QLatin1String("exec \"$target\"")));
#endif
    }

    // Extracted: asked to (the variable, set to anything), or $APPDIR is
    // a plain folder (the runtime's appimage_extracted_<hash>). From
    // its FUSE mount, $APPDIR is a mount point, as / is.
    void tellsAnExtractedAppImageFromAMountedOne() {
        QVERIFY(appImageRunsExtracted(QString(), true));
        QVERIFY(appImageRunsExtracted(QStringLiteral("/"), true));
        QVERIFY(!appImageRunsExtracted(QString(), false));
        QVERIFY(!appImageRunsExtracted(QStringLiteral("/"), false));
        const QString extracted = path(QStringLiteral("tmp/appimage_extracted_0123456789abcdef"));
        QVERIFY(QDir().mkpath(extracted + QStringLiteral("/usr/bin")));
        QVERIFY(appImageRunsExtracted(extracted, false));
        QVERIFY(!appImageRunsExtracted(path(QStringLiteral("tmp/not-there")), false));
    }

    // A GitBolt whose parent hasn't reaped it yet is a zombie, which
    // kill -0 still finds: the helper must not wait its full timeout
    // for that (a launcher that never calls wait() would hold every
    // update for ten minutes).
    void aZombieCountsAsQuit() {
#if !defined(Q_OS_UNIX)
        QSKIP("POSIX only");
#else
        const pid_t child = ::fork();
        if (child == 0)
            ::_exit(0);
        QVERIFY(child > 0);
        auto reap = qScopeGuard([child] { ::waitpid(child, nullptr, 0); });
        QString state;
        for (int i = 0; i < 100 && !state.startsWith(u'Z'); ++i) {
            QTest::qWait(20);
            runTool(QStringLiteral("/bin/ps"), {QStringLiteral("-o"), QStringLiteral("stat="), QStringLiteral("-p"),
                                                QString::number(child)},
                    &state);
            state = state.trimmed();
        }
        QVERIFY2(state.startsWith(u'Z'), qPrintable(state));
        QVERIFY(gitbolt::test::processAlive(child));   // kill -0 alone says it lives

        HelperInputs in = appImageInputs(child);
        in.waitSeconds = 60;
        QVERIFY(layOutAppImage(in));
        QElapsedTimer clock;
        clock.start();
        QVERIFY(startHelper(QStringLiteral("/bin/sh"), appImageHelperScript(in)));
        QVERIFY2(helperFinished(20000), qPrintable(diagnostics()));
        QVERIFY2(clock.elapsed() < 15000, qPrintable(QString::number(clock.elapsed())));
        QVERIFY(readFile(marker()).startsWith(QLatin1String("0.1.1 ")));
#endif
    }

    // ---------------------------------------------------------------
    // deb
    // ---------------------------------------------------------------

    void debRunsAptThroughPkexecAndRestarts_data() {
        QTest::addColumn<QString>("shell");
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("logged");
        QTest::addColumn<QString>("outcome");
        for (const QString& shell : shells()) {
            const QString name = QFileInfo(shell).fileName();
            QTest::newRow(qPrintable(name + QStringLiteral(" installed")))
                << shell << 0 << QStringLiteral("installed ") << QStringLiteral("ok 0.1.1 \n");
            // 126: pkexec's "not authorized" (the dialog was dismissed).
            QTest::newRow(qPrintable(name + QStringLiteral(" refused")))
                << shell << 126 << QStringLiteral("wasn't given permission (status 126)")
                << QStringLiteral("declined 0.1.1 the administrator password prompt was dismissed\n");
            // 127: pkexec couldn't ask (no polkit agent in the session).
            QTest::newRow(qPrintable(name + QStringLiteral(" no agent")))
                << shell << 127 << QStringLiteral("couldn't ask for permission (status 127)")
                << QStringLiteral("failed 0.1.1 no polkit authentication agent is running\n");
            // apt-get's own failure, as pkexec passes it on.
            QTest::newRow(qPrintable(name + QStringLiteral(" apt failed")))
                << shell << 1 << QStringLiteral("failed with status 1")
                << QStringLiteral("failed 0.1.1 apt-get failed with status 1\n");
        }
    }

    void debRunsAptThroughPkexecAndRestarts() {
#if !defined(Q_OS_UNIX)
        QSKIP("POSIX sh only");
#else
        QFETCH(QString, shell);
        QFETCH(int, status);
        QFETCH(QString, logged);
        QFETCH(QString, outcome);
        const HelperInputs in = debInputs(startWaited());
        QVERIFY(layOutDeb(in, status));
        QVERIFY(startHelper(shell, debHelperScript(in)));
        QVERIFY2(helperIsWaiting(), qPrintable(diagnostics()));
        QVERIFY(!QFileInfo::exists(path(QStringLiteral("pkexec-args.txt"))));
        quitWaited();
        QVERIFY2(helperFinished(), qPrintable(diagnostics()));

        const QStringList args = readFile(path(QStringLiteral("pkexec-args.txt"))).split(u'\n', Qt::SkipEmptyParts);
        const QStringList expected = {QStringLiteral("/usr/bin/apt-get"), QStringLiteral("install"),
                                      QStringLiteral("-y"), QStringLiteral("-o"),
                                      QStringLiteral("DPkg::Lock::Timeout=60"), in.staged};
        QCOMPARE(args, expected);
        // Started either way: the new version, or the old one again.
        QVERIFY2(readFile(marker()).startsWith(QLatin1String("relaunched ")), qPrintable(diagnostics()));
        QVERIFY2(readFile(log()).contains(logged), qPrintable(diagnostics()));
        // And the result, before the relaunch.
        QCOMPARE(readFile(result()), outcome);
        QCOMPARE(resultAtRelaunch(), outcome);
#endif
    }

    void debGivesUpAfterTheTimeout() {
#if !defined(Q_OS_UNIX)
        QSKIP("POSIX sh only");
#else
        HelperInputs in = debInputs(startWaited());
        in.waitSeconds = 1;
        QVERIFY(layOutDeb(in, 0));
        QVERIFY(startHelper(QStringLiteral("/bin/sh"), debHelperScript(in)));
        QVERIFY2(helperFinished(10000), qPrintable(diagnostics()));
        QCOMPARE(helper_->exitCode(), 1);
        QVERIFY(!QFileInfo::exists(path(QStringLiteral("pkexec-args.txt"))));
        QVERIFY(!QFileInfo::exists(marker()));
        QVERIFY(!QFileInfo::exists(in.staged));
        QCOMPARE(readFile(result()), QStringLiteral("gave-up 0.1.1 GitBolt didn't quit within 1 seconds\n"));
#endif
    }

    void debScriptNamesTheRealCommands() {
        HelperInputs in = nastyInputs();
        in.staged = QStringLiteral("/home/o'brien/.cache/GitBolt/updates/0.1.1/gitbolt_0.1.1_amd64.deb");
        in.executable = QStringLiteral("/usr/bin/gitbolt");
        const QString script = debHelperScript(in);
        QVERIFY(script.contains(
            QLatin1String("\ndeb='/home/o'\\''brien/.cache/GitBolt/updates/0.1.1/gitbolt_0.1.1_amd64.deb'\n")));
        QVERIFY(script.contains(QLatin1String("\npkexec='/usr/bin/pkexec'\n")));
        QVERIFY(script.contains(QLatin1String("\nrelaunch='/usr/bin/gitbolt'\n")));
        QVERIFY(script.contains(
            QLatin1String("\n\"$pkexec\" /usr/bin/apt-get install -y -o DPkg::Lock::Timeout=60 \"$deb\"\n")));
        QVERIFY(script.endsWith(QLatin1String("exec \"$relaunch\"\n")));
    }

    // ---------------------------------------------------------------
    // Windows (text only)
    // ---------------------------------------------------------------

    void powerShellQuoteDoublesEveryKindOfQuote() {
        QCOMPARE(powerShellQuote(QString()), QStringLiteral("''"));
        QCOMPARE(powerShellQuote(QStringLiteral("C:\\Program Files\\GitBolt")),
                 QStringLiteral("'C:\\Program Files\\GitBolt'"));
        QCOMPARE(powerShellQuote(QStringLiteral("Bob's")), QStringLiteral("'Bob''s'"));
        QCOMPARE(powerShellQuote(QStringLiteral("\u2018a\u2019b\u201Ac\u201Bd")),
                 QStringLiteral("'\u2018\u2018a\u2019\u2019b\u201A\u201Ac\u201B\u201Bd'"));
        // Nothing else is special in a single-quoted string.
        QCOMPARE(powerShellQuote(QStringLiteral("$env:X `n \"q\" $(x)")),
                 QStringLiteral("'$env:X `n \"q\" $(x)'"));
        for (const QString& text : {QStringLiteral("it's ’here’"), QStringLiteral("''''"), QStringLiteral("’'‘")}) {
            QStringList literals;
            QVERIFY(powerShellLiterals(powerShellQuote(text), &literals));
            QCOMPARE(literals, QStringList{text});
        }
    }

    // The Windows helper's inputs, in the shape Installer::helperScript
    // gives them (forward slashes: the scripts turn them around).
    static HelperInputs windowsInputs() {
        HelperInputs in;
        in.pid = 4321;
        in.waitSeconds = 600;
        in.logFile = QStringLiteral("C:/Users/Bob/AppData/Local/GitBolt/cache/updates/install.log");
        in.resultFile = QStringLiteral("C:/Users/Bob/AppData/Local/GitBolt/cache/updates/install-result");
        in.newVersion = QStringLiteral("0.1.1");
        in.target = QStringLiteral("C:/Program Files/GitBolt");
        in.staged = QStringLiteral(
            "C:/Users/Bob/AppData/Local/GitBolt/cache/updates/0.1.1/GitBolt-0.1.1-Windows-AMD64.exe");
        return in;
    }

    // The elevated script as the outer one carries it: the lines between
    // "$elevated = (" and ") -join [char]10", each exactly one quoted
    // literal (and a comma but the last), unquoted and joined. Empty if
    // any line is anything else.
    static QString carriedElevatedScript(const QString& script) {
        const QStringList lines = script.split(u'\n');
        const qsizetype from = lines.indexOf(QStringLiteral("$elevated = ("));
        const qsizetype to = lines.indexOf(QStringLiteral(") -join [char]10"));
        if (from < 0 || to <= from + 1)
            return {};
        QStringList carried;
        for (qsizetype i = from + 1; i < to; ++i) {
            QString line = lines.at(i);
            if (!line.startsWith(QLatin1String("    ")))
                return {};
            line = line.mid(4);
            if (i + 1 < to) {
                if (!line.endsWith(u','))
                    return {};
                line.chop(1);
            }
            QStringList literals;
            if (!powerShellLiterals(line, &literals) || literals.size() != 1 || powerShellQuote(literals.at(0)) != line)
                return {};
            carried << literals.at(0);
        }
        return carried.join(u'\n');
    }

    // What CreateProcess gets for `script` run as Installer::launchHelper
    // runs it: the program, then the arguments (none needs quoting).
    static qsizetype commandLineLength(const QString& program, const QStringList& arguments) {
        return program.size() + 1 + arguments.join(u' ').size();
    }

    void windowsScriptQuotesEveryPath() {
        HelperInputs in = windowsInputs();
        in.target = QStringLiteral("D:/Apps/Bob's \u2018Git\u2019Bolt $(x) #1");
        const QString script = windowsHelperScript(in);
        const QString elevated = windowsElevatedScript(in);

        // No interpolated strings at all, in either script, so no path
        // can expand.
        QVERIFY2(!script.contains(u'"'), qPrintable(script));
        QVERIFY2(!elevated.contains(u'"'), qPrintable(elevated));
        // Every literal closes on its line, and the paths come back as
        // given, in Windows form.
        QStringList literals;
        for (const QString& line : script.split(u'\n'))
            QVERIFY2(powerShellLiterals(line, &literals), qPrintable(line));
        const QString dir = QStringLiteral("D:\\Apps\\Bob's \u2018Git\u2019Bolt $(x) #1");
        const QString cache = QStringLiteral("C:\\Users\\Bob\\AppData\\Local\\GitBolt\\cache\\updates\\");
        QVERIFY(literals.contains(cache + QStringLiteral("install.log")));
        QVERIFY(literals.contains(cache + QStringLiteral("install-result")));
        QVERIFY(literals.contains(cache + QStringLiteral("0.1.1\\GitBolt-0.1.1-Windows-AMD64.exe")));
        QVERIFY(literals.contains(dir + QStringLiteral("\\bin\\GitBolt.exe")));
        QVERIFY(literals.contains(QStringLiteral("0.1.1")));
        QVERIFY(!script.contains(u'/' + QStringLiteral("Users")));

        // The elevated script travels inside it intact (quoted twice:
        // each quote of it doubled again), and its own literals hold the
        // folder, the installer and the version.
        QCOMPARE(carriedElevatedScript(script), elevated);
        QStringList inner;
        for (const QString& line : elevated.split(u'\n'))
            QVERIFY2(powerShellLiterals(line, &inner), qPrintable(line));
        QVERIFY(inner.contains(dir + u'\\'));
        QVERIFY(inner.contains(dir + QStringLiteral("\\bin\\GitBolt.exe")));
        QVERIFY(inner.contains(cache + QStringLiteral("0.1.1\\GitBolt-0.1.1-Windows-AMD64.exe")));
        QVERIFY(inner.contains(QStringLiteral("/S /D=") + dir));
        QVERIFY(inner.contains(QStringLiteral("0.1.1")));
    }

    // NSIS takes everything after /D= as the folder: it must be the
    // last thing on the installer's command line, and not quoted. The
    // whole of it is one ArgumentList string, so PowerShell adds
    // nothing after it. The installer runs from the elevated script,
    // already an administrator: no second UAC prompt.
    void windowsScriptPutsTheFolderLast() {
        HelperInputs in = windowsInputs();
        const QString elevated = windowsElevatedScript(in);
        QVERIFY2(elevated.contains(QLatin1String(
                     "\n        $setup = Start-Process -FilePath $installer -ArgumentList "
                     "'/S /D=C:\\Program Files\\GitBolt' -Wait -PassThru\n")),
                 qPrintable(elevated));
        QCOMPARE(elevated.count(QLatin1String("Start-Process")), 1);
        QCOMPARE(elevated.count(QLatin1String("/D=")), 1);
        QVERIFY(!elevated.contains(QLatin1String("-Verb")));
        QVERIFY(elevated.contains(QLatin1String("\n$installer = 'C:\\Users\\Bob\\AppData\\Local\\GitBolt\\cache\\updates\\"
                                                "0.1.1\\GitBolt-0.1.1-Windows-AMD64.exe'\n")));
        QVERIFY(elevated.contains(QLatin1String("\n$app = 'C:\\Program Files\\GitBolt\\bin\\GitBolt.exe'\n")));
        QVERIFY(elevated.contains(QLatin1String("\n$prefix = 'C:\\Program Files\\GitBolt\\'\n")));

        // The one elevation: Windows PowerShell's own powershell.exe, the
        // elevated script by -EncodedCommand, hidden, waited for.
        const QString script = windowsHelperScript(in);
        QCOMPARE(script.count(QLatin1String("-Verb RunAs")), 1);
        QVERIFY2(script.contains(QLatin1String(
                     "\n    $setup = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList "
                     "('-NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand ' + $encoded) "
                     "-Verb RunAs -WindowStyle Hidden -Wait -PassThru\n")),
                 qPrintable(script));
        QVERIFY(script.contains(QLatin1String(
            "\n$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($elevated))\n")));
        QVERIFY(script.indexOf(QLatin1String(") -join [char]10\n$encoded = ")) > 0);
        QVERIFY(script.contains(QLatin1String("\n$app = 'C:\\Program Files\\GitBolt\\bin\\GitBolt.exe'\n")));
        QVERIFY(script.contains(QLatin1String("\n    Start-Process -FilePath $app\n")));
        // GitBolt is started after the elevated part, whatever it did.
        QVERIFY(script.indexOf(QLatin1String("$setup.ExitCode")) < script.indexOf(QLatin1String("-FilePath $app\n")));
        // A folder at a drive's root keeps one backslash.
        in.target = QStringLiteral("D:/");
        QVERIFY(windowsHelperScript(in).contains(QLatin1String("\n$app = 'D:\\bin\\GitBolt.exe'\n")));
        QVERIFY(windowsElevatedScript(in).contains(QLatin1String("\n$prefix = 'D:\\'\n")));
        QVERIFY(windowsElevatedScript(in).contains(QLatin1String("-ArgumentList '/S /D=D:\\' -Wait")));
    }

    void windowsScriptWaitsForGitBoltFirst() {
        const HelperInputs in = windowsInputs();
        const QString script = windowsHelperScript(in);
        const QString elevated = windowsElevatedScript(in);
        // $pid is PowerShell's own, read-only: neither script may
        // assign it, or read it meaning GitBolt's.
        const QRegularExpression pid(QStringLiteral("\\$pid\\b"), QRegularExpression::CaseInsensitiveOption);
        QVERIFY(!script.contains(pid));
        QVERIFY(!elevated.contains(pid));
        QVERIFY(script.contains(QLatin1String("\n$appPid = 4321\n")));
        QVERIFY(elevated.contains(QLatin1String("\n$appPid = 4321\n")));
        QVERIFY(script.contains(QLatin1String("\n$waitSeconds = 600\n")));
        const qsizetype wait = script.indexOf(QLatin1String("Wait-Process -Id $appPid -Timeout $waitSeconds"));
        const qsizetype stillThere = script.indexOf(QLatin1String("if (Get-Process -Id $appPid"));
        const qsizetype elevate = script.indexOf(QLatin1String("-Verb RunAs"));
        QVERIFY(wait > 0);
        QVERIFY(stillThere > wait);
        QVERIFY(elevate > stillThere);
        // Gives up without installing, and says so: the result and the
        // exit come before the elevation, inside the still-there branch.
        const qsizetype gaveUp = script.indexOf(
            QLatin1String("    Write-Result 'gave-up' ('GitBolt did not quit within ' + $waitSeconds + ' seconds')\n"
                          "    exit 1\n"),
            stillThere);
        QVERIFY(gaveUp > stillThere && gaveUp < elevate);
        QVERIFY(script.startsWith(QLatin1String("# GitBolt's update helper (Windows)")));
        QVERIFY(script.contains(QLatin1String("\n$ErrorActionPreference = 'Stop'\n")));
        QVERIFY(elevated.startsWith(QLatin1String("$ErrorActionPreference = 'Stop'\n")));
    }

    // Elevated, it waits (a while) until no other GitBolt runs from the
    // install folder and GitBolt.exe can be opened with no sharing: in
    // use, NSIS would uninstall around the locked files and install a
    // mix. Then the installer, then the version check.
    void windowsElevatedScriptWaitsUntilTheInstallIsFree() {
        const QString elevated = windowsElevatedScript(windowsInputs());
        const qsizetype loop = elevated.indexOf(QStringLiteral("$deadline = (Get-Date).AddSeconds(%1)").arg(kWindowsInUseSeconds));
        const qsizetype others = elevated.indexOf(QLatin1String(
            "Get-Process -Name GitBolt -ErrorAction SilentlyContinue | Where-Object {\n"
            "            try { $_.Id -ne $appPid -and $_.Path -and "
            "$_.Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) } catch { $false }\n"
            "        }).Count -gt 0\n"));
        const qsizetype exclusive =
            elevated.indexOf(QLatin1String("[IO.File]::Open($app, 'Open', 'ReadWrite', 'None').Dispose()"));
        const qsizetype busy = elevated.indexOf(QStringLiteral("exit %1\n").arg(kWindowsExitInUse));
        const qsizetype pause = elevated.indexOf(QLatin1String("Start-Sleep -Seconds 1"));
        const qsizetype install = elevated.indexOf(QLatin1String("Start-Process -FilePath $installer"));
        const qsizetype noInstaller = elevated.indexOf(QStringLiteral("exit %1\n").arg(kWindowsExitNoInstaller));
        const qsizetype itsCode = elevated.indexOf(QLatin1String(
            "    $code = $setup.ExitCode\n"
            "    if ($null -ne $code -and $code -ne 0) {\n"
            "        exit $code\n"));
        const qsizetype version = elevated.indexOf(QLatin1String(
            "    $found = [string](Get-Item -LiteralPath $app).VersionInfo.ProductVersion\n"
            "    if ($found.Trim() -ne $version) {\n"));
        const qsizetype wrongVersion = elevated.indexOf(QStringLiteral("exit %1\n").arg(kWindowsExitWrongVersion));
        const qsizetype done = elevated.indexOf(QLatin1String("    exit 0\n"));
        const qsizetype failed = elevated.indexOf(QStringLiteral("} catch {\n    exit %1\n}").arg(kWindowsExitError));
        QVERIFY(elevated.contains(QLatin1String("\n$version = '0.1.1'\n")));
        QVERIFY(elevated.contains(QLatin1String("\ntry {\n    $deadline")));
        for (const qsizetype at : {loop, others, exclusive, busy, pause, install, noInstaller, itsCode, version,
                                   wrongVersion, done, failed})
            QVERIFY2(at > 0, qPrintable(elevated));
        QVERIFY(loop < others && others < exclusive && exclusive < busy && busy < pause && pause < install);
        QVERIFY(install < noInstaller && noInstaller < itsCode && itsCode < version && version < wrongVersion);
        QVERIFY(wrongVersion < done && done < failed);
        QVERIFY(elevated.endsWith(QStringLiteral("} catch {\n    exit %1\n}").arg(kWindowsExitError)));
        // The codes it can end with are distinct, and none is NSIS's.
        const QList<int> codes = {kWindowsExitInUse, kWindowsExitNoInstaller, kWindowsExitWrongVersion,
                                  kWindowsExitError};
        for (const int code : codes) {
            QVERIFY(code > 2);
            QCOMPARE(codes.count(code), 1);
        }
    }

    // What the helper makes of each outcome, for the install result:
    // ok only for 0; declined for UAC's No (ERROR_CANCELLED, 1223);
    // failed, with why, for everything else; written before GitBolt
    // starts again.
    void windowsScriptReportsEachOutcome() {
        const QString script = windowsHelperScript(windowsInputs());
        QVERIFY(script.contains(QLatin1String(
            "\n        [IO.File]::WriteAllText($result, $status + ' ' + $version + ' ' + "
            "($reason -replace '[\\r\\n]+', ' ') + [char]10)\n")));
        QVERIFY(script.contains(QLatin1String("\n$status = 'failed'\n$reason = ''\n")));
        const auto branch = [&](const QString& test, const QString& then) {
            return script.contains(QStringLiteral("if (%1) {\n%2").arg(test, then));
        };
        QVERIFY(branch(QStringLiteral("$null -eq $code"),
                       QStringLiteral("        $reason = 'the installer finished, but its exit code is unknown'\n")));
        QVERIFY(branch(QStringLiteral("$code -eq 0"), QStringLiteral("        $status = 'ok'\n")));
        QVERIFY(branch(QStringLiteral("$code -eq %1").arg(kWindowsExitInUse),
                       QStringLiteral("        $reason = 'another GitBolt kept running from the install folder'\n")));
        QVERIFY(branch(QStringLiteral("$code -eq %1").arg(kWindowsExitNoInstaller),
                       QStringLiteral("        $reason = 'the installer could not be started'\n")));
        QVERIFY(branch(QStringLiteral("$code -eq %1").arg(kWindowsExitWrongVersion),
                       QStringLiteral("        $reason = 'the installer ran, but GitBolt.exe is not version ' + $version\n")));
        QVERIFY(branch(QStringLiteral("$code -eq %1").arg(kWindowsExitError),
                       QStringLiteral("        $reason = 'the administrator part of the update failed'\n")));
        QVERIFY(script.contains(QLatin1String(
            "    } else {\n        $reason = 'the installer exited with code ' + $code\n    }\n")));
        // UAC's No: the Win32Exception under Start-Process's, 1223.
        QVERIFY(script.contains(QLatin1String(
            "    if (($e -is [ComponentModel.Win32Exception] -and $e.NativeErrorCode -eq 1223) "
            "-or $_.Exception.Message -match 'canceled by the user') {\n"
            "        $status = 'declined'\n"
            "        $reason = 'the administrator prompt was declined'\n")));
        // Once, after the elevated part and before GitBolt starts.
        QCOMPARE(script.count(QLatin1String("\nWrite-Result $status $reason\n")), 1);
        const qsizetype written = script.indexOf(QLatin1String("\nWrite-Result $status $reason\n"));
        QVERIFY(written > script.indexOf(QLatin1String("-Verb RunAs")));
        QVERIFY(written < script.indexOf(QLatin1String("Write-Log ('starting ' + $app)")));
    }

    // CreateProcess takes 32767 characters, the program included, and
    // so do both powershell.exe command lines here: the helper's own
    // (the outer script, -EncodedCommand) and the elevated one that
    // Start-Process makes. Long paths, quotes in them (doubled in the
    // elevated script, doubled again where the helper carries it) and
    // all. (A path too long for this makes startDetached, or the
    // helper's Start-Process, fail; nothing would have been installed.)
    void windowsCommandLinesFitWithLongPaths() {
        const QString longName = QStringLiteral("Bob's \u2018quoted\u2019 folder ").repeated(8).left(220);
        HelperInputs in = windowsInputs();
        in.pid = 4294967295;
        in.target = QStringLiteral("C:/") + longName + QStringLiteral("/GitBolt");
        in.staged = QStringLiteral("C:/Users/") + longName + QStringLiteral("/AppData/Local/GitBolt/cache/updates/"
                                                                          "0.1.10/GitBolt-0.1.10-Windows-AMD64.exe");
        in.logFile = QStringLiteral("C:/Users/") + longName + QStringLiteral("/AppData/Local/GitBolt/cache/updates/install.log");
        in.resultFile = QStringLiteral("C:/Users/") + longName
                        + QStringLiteral("/AppData/Local/GitBolt/cache/updates/install-result");
        in.newVersion = QStringLiteral("999999.999999.999999");
        const QString powershell = QStringLiteral("C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
        const qsizetype outer = commandLineLength(powershell, powerShellArguments(windowsHelperScript(in)));
        const qsizetype elevated = commandLineLength(powershell, powerShellArguments(windowsElevatedScript(in)));
        QVERIFY2(outer < kWindowsCommandLineLimit, qPrintable(QString::number(outer)));
        QVERIFY2(elevated < kWindowsCommandLineLimit, qPrintable(QString::number(elevated)));
        // With room to spare: the usual paths are a fraction of that.
        const HelperInputs usual = windowsInputs();
        const qsizetype usualOuter = commandLineLength(powershell, powerShellArguments(windowsHelperScript(usual)));
        QVERIFY2(usualOuter < kWindowsCommandLineLimit * 2 / 3, qPrintable(QString::number(usualOuter)));
        qInfo("command lines: %lld and %lld characters with long paths, %lld usually", qint64(outer),
              qint64(elevated), qint64(usualOuter));
    }

    void encodedCommandIsUtf16LittleEndianBase64() {
        // Known answer: "dir" is d\0i\0r\0.
        QCOMPARE(powerShellEncodedCommand(QStringLiteral("dir")), QByteArray("ZABpAHIA"));
        HelperInputs in = windowsInputs();
        in.target = QStringLiteral("C:/Program Files/G\u00eftBolt \u2019\u4e2d");
        const QString script = windowsHelperScript(in);
        const QByteArray encoded = powerShellEncodedCommand(script);
        QVERIFY(QRegularExpression(QStringLiteral("^[A-Za-z0-9+/]+=*$")).match(QString::fromLatin1(encoded)).hasMatch());
        const QByteArray bytes = QByteArray::fromBase64(encoded);
        QCOMPARE(bytes.size(), script.size() * 2);
        QString decoded;
        for (qsizetype i = 0; i + 1 < bytes.size(); i += 2)
            decoded += QChar(static_cast<char16_t>(static_cast<unsigned char>(bytes[i])
                                                   | (static_cast<unsigned char>(bytes[i + 1]) << 8)));
        QCOMPARE(decoded, script);

        const QStringList args = powerShellArguments(script);
        QCOMPARE(args.mid(0, 5), (QStringList{QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                                              QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
                                              QStringLiteral("-EncodedCommand")}));
        QCOMPARE(args.size(), 6);
        QCOMPARE(args.at(5).toLatin1(), encoded);
        // The elevated script's arguments, as the helper builds them
        // there ([Text.Encoding]::Unicode is UTF-16LE): the same words.
        QVERIFY(windowsHelperScript(in).contains(
            QLatin1String("('-NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand ' + $encoded)")));
        QCOMPARE(powerShellArguments(windowsElevatedScript(in)).mid(0, 5), args.mid(0, 5));
    }

    // ---------------------------------------------------------------
    // Windows, for real (opt-in)
    // ---------------------------------------------------------------

    // A whole Windows update of a real install, which only CI's NSIS
    // smoke test has: it installs the installer it just built, then
    // names both (GITBOLT_TEST_NSIS_INSTALLER, GITBOLT_TEST_NSIS_DIR)
    // and runs this function alone. The folder must be what
    // detectPackage calls a WindowsInstall (the registry naming it, as
    // currentProbe reads it), and the installer must have the name the
    // updater downloads. The helper is built and started as
    // Installer::launchHelper does (powershell.exe with no window,
    // -EncodedCommand, detached), for a stand-in GitBolt it must wait
    // for. Given 2 seconds, it gives up while that still runs, and says
    // so in the install result. With another GitBolt running from the
    // install folder, the elevated part waits kWindowsInUseSeconds and
    // installs nothing: failed. With the folder free, it waits for the
    // stand-in; once that is gone, the installer runs elevated over the
    // folder and exits 0, GitBolt.exe has the new version, the result
    // is ok, and the installed GitBolt.exe starts again and shows its
    // window. Every GitBolt.exe started here is a test session (an
    // instance name of its own, so it sends and checks nothing and
    // can't hand off to another GitBolt); the test closes them.
    void windowsHelperUpdatesARealInstall() {
#if !defined(Q_OS_WIN)
        QSKIP("Windows only: runs a real NSIS installer");
#else
        const QString installer = qEnvironmentVariable("GITBOLT_TEST_NSIS_INSTALLER");
        const QString installDir = qEnvironmentVariable("GITBOLT_TEST_NSIS_DIR");
        if (installer.isEmpty() || installDir.isEmpty())
            QSKIP("Runs only when GITBOLT_TEST_NSIS_INSTALLER and GITBOLT_TEST_NSIS_DIR name a GitBolt installer "
                  "and a folder it installed into (CI's NSIS smoke test sets them)");
        QVERIFY2(QFileInfo(installer).isFile(), qPrintable(installer));

        // What the installed copy detects, from its own bin folder.
        PackageProbe probe;
        probe.appDirPath = QDir(installDir).filePath(QStringLiteral("bin"));
        probe.registeredUninstallers = registeredWindowsUninstallers();
        qInfo("registered uninstallers: %s", qPrintable(probe.registeredUninstallers.join(QStringLiteral(", "))));
        const PackageInfo package = detectPackage(probe);
        QVERIFY2(package.kind == PackageKind::WindowsInstall && package.canInstall,
                 qPrintable(installDir + QStringLiteral(" is no GitBolt install: ") + package.whyNot));
        const QString app = QDir::toNativeSeparators(package.target) + QStringLiteral("\\bin\\GitBolt.exe");
        QVERIFY2(QFileInfo(app).isFile(), qPrintable(app));
        QVERIFY2(processesOf(app).isEmpty(), qPrintable(app + QStringLiteral(" is running already")));

        // The installer where the Downloader leaves it,
        // <updates>/<version>/<asset name>: a copy, since a helper that
        // gives up deletes it.
        const QString fileName = QFileInfo(installer).fileName();
        static const QRegularExpression named(QStringLiteral("^GitBolt-([0-9.]+)-"));
        const std::optional<Version> version = Version::parse(named.match(fileName).captured(1));
        QVERIFY2(version.has_value(), qPrintable(fileName));
        QCOMPARE(fileName, assetNameFor(PackageKind::WindowsInstall, *version));
        const QString staged = path(QStringLiteral("updates/%1/%2").arg(version->toString(), fileName));
        QVERIFY(QDir().mkpath(QFileInfo(staged).path()));
        QVERIFY(QFile::copy(installer, staged));

        // GitBolt, still running when the helper starts: a stand-in the
        // test ends when GitBolt would quit.
        const auto startStandIn = [this] {
            waited_ = std::make_unique<QProcess>();
            waited_->start(windowsPowerShell(), {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                                                 QStringLiteral("-Command"), QStringLiteral("Start-Sleep -Seconds 300")});
            return waited_->waitForStarted() ? waited_->processId() : qint64(0);
        };
        qint64 gitBoltPid = startStandIn();
        QVERIFY(gitBoltPid > 0);

        // The helper passes its environment on to the GitBolt it starts:
        // a test session's, without this run's Qt settings (the install
        // has no offscreen plugin) and with a temporary folder of its own.
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const QString& name :
             {QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("QT_PLUGIN_PATH"), QStringLiteral("QML2_IMPORT_PATH"),
              QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"), QStringLiteral("GITBOLT_TEST_BRIDGE"),
              QStringLiteral("GITBOLT_TEST_UPDATE_FEED"), QStringLiteral("GITBOLT_TEST_APP_VERSION"),
              QStringLiteral("GITBOLT_TEST_NSIS_INSTALLER"), QStringLiteral("GITBOLT_TEST_NSIS_DIR")})
            env.remove(name);
        env.insert(QStringLiteral("GITBOLT_INSTANCE_NAME"), kInstance + QStringLiteral("-nsis"));
        env.insert(QStringLiteral("GITBOLT_TELEMETRY"), QStringLiteral("0"));
        env.insert(QStringLiteral("GITBOLT_UPDATE_CHECK"), QStringLiteral("0"));
        const QString temp = path(QStringLiteral("temp"));
        QVERIFY(QDir().mkpath(temp));
        env.insert(QStringLiteral("TEMP"), QDir::toNativeSeparators(temp));
        env.insert(QStringLiteral("TMP"), QDir::toNativeSeparators(temp));

        // Pass or fail, nothing this test started outlives it.
        QList<qint64> helpers;
        auto stopAll = qScopeGuard([&] {
            for (const qint64 pid : helpers)
                if (gitbolt::test::processAlive(pid))
                    endProcess(static_cast<DWORD>(pid));
            for (const QString& program : {app, staged})
                for (const DWORD running : processesOf(program))
                    endProcess(running);
        });
        // The helper, built and started as Installer::launchHelper does,
        // on a fresh log and with no result yet. Its pid, or 0.
        const auto launch = [&](int waitSeconds) {
            HelperInputs in;
            in.pid = gitBoltPid;
            in.waitSeconds = waitSeconds;
            in.logFile = log();
            in.resultFile = result();
            in.newVersion = version->toString();
            in.target = package.target;
            in.staged = staged;
            QFile::remove(log());
            QFile::remove(result());
            QProcess helper;
            helper.setProgram(windowsPowerShell());
            helper.setArguments(powerShellArguments(windowsHelperScript(in)));
            helper.setWorkingDirectory(path(QStringLiteral("updates")));
            helper.setStandardInputFile(QProcess::nullDevice());
            helper.setProcessEnvironment(env);
            helper.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
                args->flags &= ~static_cast<unsigned long>(CREATE_NEW_CONSOLE | DETACHED_PROCESS);
                args->flags |= CREATE_NO_WINDOW;
            });
            qint64 pid = 0;
            if (!helper.startDetached(&pid)) {
                qWarning("couldn't start the helper: %s", qPrintable(helper.errorString()));
                return qint64(0);
            }
            helpers.append(pid);
            return pid;
        };
        const auto ended = [](qint64 pid, int timeoutMs) {
            const QDeadlineTimer deadline(timeoutMs);
            while (gitbolt::test::processAlive(pid) && !deadline.hasExpired())
                QTest::qWait(250);
            return !gitbolt::test::processAlive(pid);
        };
        const auto lastLine = [this] {
            const QStringList lines = readFile(log()).split(u'\n', Qt::SkipEmptyParts);
            return lines.isEmpty() ? QString() : lines.constLast().trimmed();
        };
        const QString startingApp = QStringLiteral(" starting ") + app;
        // The helper's end: its last line "starting <app>" (or why that
        // failed), then the process gone. False if it isn't, in time.
        const auto finishes = [&](qint64 pid, int timeoutMs) {
            const QDeadlineTimer deadline(timeoutMs);
            while (!lastLine().endsWith(startingApp) && !lastLine().contains(QLatin1String("could not start"))
                   && gitbolt::test::processAlive(pid) && !deadline.hasExpired())
                QTest::qWait(250);
            return ended(pid, 30000);
        };
        // A GitBolt.exe from the install that shows a window, other than
        // `except`; 0 if none does within `timeoutMs`.
        const auto shownGitBolt = [&](DWORD except, int timeoutMs) {
            const QDeadlineTimer deadline(timeoutMs);
            for (;;) {
                for (const DWORD id : processesOf(app)) {
                    if (id != except && visibleWindows(id) > 0)
                        return id;
                }
                if (deadline.hasExpired())
                    return DWORD(0);
                QTest::qWait(250);
            }
        };
        const auto closeAll = [&] {
            for (const DWORD id : processesOf(app)) {
                if (!closeGitBolt(id, 30000))
                    qWarning("GitBolt (pid %lu) didn't close within 30 s, so it was ended", id);
            }
        };

        // 1. GitBolt doesn't quit in time: the helper gives up, deletes
        // the download, and neither installs nor starts anything. (So
        // Wait-Process's timeout is no terminating error.)
        const qint64 givingUp = launch(2);
        QVERIFY(givingUp > 0);
        QVERIFY2(ended(givingUp, 60000), qPrintable(QStringLiteral("the helper didn't give up\n") + diagnostics()));
        QVERIFY2(readFile(log()).contains(QLatin1String("did not quit within 2 seconds: giving up")),
                 qPrintable(diagnostics()));
        QVERIFY2(!readFile(log()).contains(QLatin1String(" running ")), qPrintable(diagnostics()));
        QCOMPARE(readFile(result()),
                 QStringLiteral("gave-up %1 GitBolt did not quit within 2 seconds\n").arg(version->toString()));
        QVERIFY(!QFileInfo::exists(staged));
        QVERIFY(processesOf(app).isEmpty());
        QCOMPARE(waited_->state(), QProcess::Running);

        // 2. Another GitBolt runs from the install folder (another
        // user's, or one reopened while UAC asked): once the stand-in
        // quits, the elevated part waits for it, gives up, and installs
        // nothing; the old copy starts again, and the result says why.
        QProcessEnvironment otherEnv = env;
        otherEnv.insert(QStringLiteral("GITBOLT_INSTANCE_NAME"), kInstance + QStringLiteral("-nsis-other"));
        QProcess other;
        other.setProgram(app);
        other.setProcessEnvironment(otherEnv);
        qint64 otherPid = 0;
        QVERIFY(other.startDetached(&otherPid));
        QVERIFY2(shownGitBolt(0, 60000) == DWORD(otherPid),
                 qPrintable(QStringLiteral("the other GitBolt shows no window\n") + diagnostics()));
        QVERIFY(QFile::copy(installer, staged));
        const qint64 blocked = launch(120);
        QVERIFY(blocked > 0);
        QVERIFY2(gitbolt::test::waitForLines(log(), 1, 60000), qPrintable(diagnostics()));
        quitWaited();
        QVERIFY2(finishes(blocked, (kWindowsInUseSeconds + 120) * 1000),
                 qPrintable(QStringLiteral("the blocked helper is still running\n") + diagnostics()));
        QVERIFY2(readFile(log()).contains(QStringLiteral("nothing was installed (code %1)").arg(kWindowsExitInUse)),
                 qPrintable(diagnostics()));
        QCOMPARE(readFile(result()),
                 QStringLiteral("failed %1 another GitBolt kept running from the install folder\n")
                     .arg(version->toString()));
        QVERIFY2(lastLine().endsWith(startingApp), qPrintable(diagnostics()));
        QVERIFY(gitbolt::test::processAlive(otherPid));
        // The old copy, started again beside the other one (it has an
        // instance name of its own), then both closed.
        QVERIFY2(shownGitBolt(DWORD(otherPid), 60000) != 0,
                 qPrintable(QStringLiteral("the old copy didn't start again\n") + diagnostics()));
        closeAll();
        QVERIFY(ended(otherPid, 30000));

        // 3. Again, with the folder free. While GitBolt runs, the helper
        // only waits.
        gitBoltPid = startStandIn();
        QVERIFY(gitBoltPid > 0);
        const qint64 helperPid = launch(120);
        QVERIFY(helperPid > 0);
        QVERIFY2(gitbolt::test::waitForLines(log(), 1, 60000), qPrintable(diagnostics()));
        QTest::qWait(1000);
        QVERIFY2(readFile(log()).contains(QStringLiteral("waiting for GitBolt (pid %1) to quit").arg(gitBoltPid)),
                 qPrintable(diagnostics()));
        QVERIFY2(!readFile(log()).contains(QLatin1String(" running ")), qPrintable(diagnostics()));
        QVERIFY(gitbolt::test::processAlive(helperPid));
        QVERIFY(!QFileInfo::exists(result()));

        // GitBolt quits. The log's last line, once the installer is done:
        // "starting <app>", or, after it, why that failed.
        quitWaited();
        QVERIFY2(finishes(helperPid, 4 * 60 * 1000),
                 qPrintable(QStringLiteral("the helper is still running\n") + diagnostics()));
        const QString story = readFile(log());
        QVERIFY2(story.contains(QStringLiteral(" running ") + QDir::toNativeSeparators(staged)),
                 qPrintable(diagnostics()));
        // 0, from the elevated part: the installer's exit code was 0,
        // and GitBolt.exe's ProductVersion is now the installer's.
        QVERIFY2(story.contains(QStringLiteral(" the installer exited with code 0, and GitBolt.exe is now version %1")
                                    .arg(version->toString())),
                 qPrintable(diagnostics()));
        QCOMPARE(readFile(result()), QStringLiteral("ok %1 \n").arg(version->toString()));
        QVERIFY2(lastLine().endsWith(startingApp), qPrintable(diagnostics()));

        // The installed GitBolt runs again and shows a window, so it got
        // past loading its DLLs and Qt's platform plugin, and past the
        // single-instance check (a copy that handed off would exit
        // without one).
        const DWORD relaunched = shownGitBolt(0, 60000);
        QVERIFY2(relaunched != 0,
                 qPrintable(QStringLiteral("after a minute, %1 copies of %2 run and none shows a window\n")
                                .arg(QString::number(processesOf(app).size()), app)
                            + diagnostics()));
        closeAll();
        // Anything else of it (a git askpass, say) goes with stopAll.
        QVERIFY(QFileInfo(app).isFile());
#endif
    }
};

QTEST_GUILESS_MAIN(TestInstallHelpers)
#include "TestInstallHelpers.moc"
