//
// TestInstallHelpers — the update helper scripts (update/InstallHelpers.h),
// run for real where possible. Unix helpers run against fake apps in
// scratch folders, under /bin/sh and dash; Windows' PowerShell parts are
// checked as text everywhere, and Write-Log runs for real on Windows. A
// real NSIS update runs only when GITBOLT_TEST_NSIS_INSTALLER and
// GITBOLT_TEST_NSIS_DIR are set, as CI's NSIS smoke test does.
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
#include "update/Installer.h"
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

    // Deb: the .deb (in a folder named with a ' and a $), and stand-ins
    // for pkexec and gitbolt.
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

    // The stand-in pkexec records its arguments, one a line, and exits
    // with `pkexecStatus`.
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

#if defined(Q_OS_WIN)
    // powershell.exe running `script` with powerShellArguments, as
    // launchWindowsUpdate starts the helper. CREATE_NO_WINDOW gives it a
    // console of its own: its -WindowStyle Hidden would otherwise hide
    // the console this test shares.
    static std::unique_ptr<QProcess> startPowerShell(const QString& script) {
        auto process = std::make_unique<QProcess>();
        process->setProgram(windowsPowerShell());
        process->setArguments(powerShellArguments(script));
        process->setProcessChannelMode(QProcess::MergedChannels);
        process->setStandardInputFile(QProcess::nullDevice());
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
            args->flags &= ~static_cast<unsigned long>(CREATE_NEW_CONSOLE | DETACHED_PROCESS);
            args->flags |= CREATE_NO_WINDOW;
        });
        process->start();
        return process;
    }

    // Opens `file` (creating it) as 5.1's Add-Content leaks a handle to a
    // busy log: write-only, sharing writing only, so neither Out-File nor
    // Add-Content can append to it meanwhile. INVALID_HANDLE_VALUE if it
    // can't be opened.
    static HANDLE holdOpen(const QString& file) {
        const std::wstring native = QDir::toNativeSeparators(file).toStdWString();
        return CreateFileW(native.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    }

    // A path as a PowerShell literal, in Windows form.
    static QString literal(const QString& file) { return powerShellQuote(QDir::toNativeSeparators(file)); }

    // windowsScriptPreamble (logging to log()), then `body`; with
    // `constrained`, all in Constrained Language mode, as under AppLocker
    // or WDAC.
    QString writeLogScript(const QString& body, bool constrained) const {
        HelperInputs in;
        in.logFile = log();
        in.newVersion = QStringLiteral("0.1.1");
        return (constrained ? QStringLiteral("$ExecutionContext.SessionState.LanguageMode = 'ConstrainedLanguage'\n")
                            : QString())
               + windowsScriptPreamble(in, false) + u'\n' + body;
    }

    // The log's lines, without their times, each checked as the parts
    // write it: UTF-8, the time first, CR LF last, and a BOM only at the
    // very start. False, and why, if not.
    bool logLines(QStringList* lines, QString* why) const {
        QFile file(log());
        if (!file.open(QIODevice::ReadOnly)) {
            *why = QStringLiteral("can't read the log: ") + file.errorString();
            return false;
        }
        QByteArray text = file.readAll();
        if (text.startsWith("\xEF\xBB\xBF"))
            text.remove(0, 3);
        if (text.contains("\xEF\xBB\xBF") || !text.endsWith("\r\n")) {
            *why = QStringLiteral("a BOM after the start, or no CR LF at the end");
            return false;
        }
        text.chop(2);
        static const QRegularExpression stamped(QStringLiteral("^\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d (.+)$"));
        for (const QString& line : QString::fromUtf8(text).split(QStringLiteral("\r\n"))) {
            const QRegularExpressionMatch match = stamped.match(line);
            if (!match.hasMatch() || line.contains(u'\r') || line.contains(u'\n')) {
                *why = QStringLiteral("not a line of the log: '%1'").arg(line);
                return false;
            }
            lines->append(match.captured(1));
        }
        return true;
    }

    // A powershell.exe a test runs, and what it has printed so far.
    struct Writer {
        std::unique_ptr<QProcess> process;
        QByteArray output;
    };

    // Ends every writer still running: nothing a test started outlives it.
    static void stopWriters(std::vector<Writer>& writers) {
        for (Writer& writer : writers) {
            if (writer.process && writer.process->state() != QProcess::NotRunning) {
                writer.process->kill();
                writer.process->waitForFinished(10000);
            }
        }
    }

    static bool allFinished(const std::vector<Writer>& writers) {
        for (const Writer& writer : writers) {
            if (writer.process->state() != QProcess::NotRunning)
                return false;
        }
        return true;
    }

    static bool anyFinished(const std::vector<Writer>& writers) {
        for (const Writer& writer : writers) {
            if (writer.process->state() == QProcess::NotRunning)
                return true;
        }
        return false;
    }

    // Each writer's state and output (both streams), then the log.
    QString writerDiagnostics(std::vector<Writer>& writers) const {
        QString text;
        int number = 0;
        for (Writer& writer : writers) {
            QProcess& process = *writer.process;
            writer.output += process.readAll();
            text += QStringLiteral("writer %1: state %2, exit status %3, code %4, output:\n%5\n")
                        .arg(QString::number(++number), QString::number(int(process.state())),
                             QString::number(int(process.exitStatus())), QString::number(process.exitCode()),
                             QString::fromLocal8Bit(writer.output));
        }
        return text + QStringLiteral("log:\n") + readFile(log());
    }

    // Each writer ended by itself, with exit code 0.
    static bool allSucceeded(const std::vector<Writer>& writers) {
        for (const Writer& writer : writers) {
            if (writer.process->exitStatus() != QProcess::NormalExit || writer.process->exitCode() != 0)
                return false;
        }
        return true;
    }
#endif

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
            {"macOS", macHelperScript(in)}, {"deb", debHelperScript(in)}};
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
        for (const QString& script : {macHelperScript(in), debHelperScript(in)}) {
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

        // The new copy in place, started with the helper's environment.
        QCOMPARE(copyOf(in.target), QStringLiteral("0.1.1"));
        QVERIFY(!QFileInfo::exists(in.target + QStringLiteral("/Contents/Resources/planted.txt")));
        const QString started = readFile(marker());
        QVERIFY2(started.startsWith(QLatin1String("0.1.1 ")), qPrintable(started));
        QVERIFY2(started.contains(QStringLiteral("instance=") + kInstance), qPrintable(started));
        QCOMPARE(started.count(u'\n'), 1);
        // The old one in the Trash, as GitBolt-<old>.app.
        const QString trashed = in.trashDir + QStringLiteral("/GitBolt-0.1.0.app");
        QCOMPARE(copyOf(trashed), QStringLiteral("0.1.0"));
        QVERIFY(QFileInfo::exists(trashed + QStringLiteral("/Contents/Resources/planted.txt")));
        QCOMPARE(entries(QStringLiteral("Applications")), QStringList{QStringLiteral("GitBolt.app")});
        QVERIFY2(readFile(log()).contains(QLatin1String("moved the old copy to")), qPrintable(diagnostics()));
        // The result: ok, no reason, written before the new copy started.
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
        // The result: failed, written before the old copy started again.
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

    // The installed bundle can't be moved aside (it is made immutable):
    // nothing changes, the result says failed, and the old copy starts.
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

    // A GitBolt its parent hasn't reaped yet is a zombie, which kill -0
    // still finds: the helper counts it as quit, or a launcher that never
    // calls wait() would hold every update for the full ten minutes.
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

        HelperInputs in = debInputs(child);
        in.waitSeconds = 60;
        QVERIFY(layOutDeb(in, 0));
        QElapsedTimer clock;
        clock.start();
        QVERIFY(startHelper(QStringLiteral("/bin/sh"), debHelperScript(in)));
        QVERIFY2(helperFinished(20000), qPrintable(diagnostics()));
        QVERIFY2(clock.elapsed() < 15000, qPrintable(QString::number(clock.elapsed())));
        QVERIFY(readFile(marker()).startsWith(QLatin1String("relaunched ")));
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
            // 126: pkexec's dialog was dismissed.
            QTest::newRow(qPrintable(name + QStringLiteral(" refused")))
                << shell << 126 << QStringLiteral("wasn't given permission (status 126)")
                << QStringLiteral("declined 0.1.1 the administrator password prompt was dismissed\n");
            // 127: pkexec couldn't authenticate (no polkit agent in the
            // session, or polkit doesn't allow it), or another error.
            QTest::newRow(qPrintable(name + QStringLiteral(" couldn't authenticate")))
                << shell << 127 << QStringLiteral("couldn't authenticate (status 127)")
                << QStringLiteral("failed 0.1.1 pkexec couldn't authenticate (no polkit authentication agent is "
                                  "running, or polkit isn't set up to allow it)\n");
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

    // Inputs as Installer::launchHelper gives them (forward slashes,
    // which the scripts convert), with the administrator part's pid.
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
        in.elevatedPid = 5678;
        return in;
    }

    // The literals of every line of `script`; false if one doesn't close
    // on its line.
    static bool literalsOf(const QString& script, QStringList* literals) {
        for (const QString& line : script.split(u'\n')) {
            if (!powerShellLiterals(line, literals))
                return false;
        }
        return true;
    }

    // What CreateProcess gets for the helper, run as launchWindowsUpdate
    // runs it: the program, then the arguments (none needs quoting).
    static qsizetype commandLineLength(const QString& program, const QStringList& arguments) {
        return program.size() + 1 + arguments.join(u' ').size();
    }
    // What ShellExecuteExW makes of lpFile and lpParameters for the
    // administrator part: "<file>" <parameters>.
    static qsizetype shellExecuteLength(const QString& file, const QString& parameters) {
        return 1 + file.size() + 2 + parameters.size();
    }

    void windowsScriptQuotesEveryPath() {
        HelperInputs in = windowsInputs();
        in.target = QStringLiteral("D:/Apps/Bob's \u2018Git\u2019Bolt $(x) #1");
        const QString script = windowsHelperScript(in);
        const QString elevated = windowsElevatedScript(in);

        // No double-quoted (interpolated) strings, so no path can expand.
        QVERIFY2(!script.contains(u'"'), qPrintable(script));
        QVERIFY2(!elevated.contains(u'"'), qPrintable(elevated));
        // Every literal closes on its line, and the paths come back as
        // given, in Windows form.
        QStringList literals;
        QVERIFY2(literalsOf(script, &literals), qPrintable(script));
        const QString dir = QStringLiteral("D:\\Apps\\Bob's \u2018Git\u2019Bolt $(x) #1");
        const QString cache = QStringLiteral("C:\\Users\\Bob\\AppData\\Local\\GitBolt\\cache\\updates\\");
        QVERIFY(literals.contains(cache + QStringLiteral("install.log")));
        QVERIFY(literals.contains(cache + QStringLiteral("install-result")));
        QVERIFY(literals.contains(dir + QStringLiteral("\\bin\\GitBolt.exe")));
        QVERIFY(literals.contains(QStringLiteral("0.1.1")));
        QVERIFY(!script.contains(u'/' + QStringLiteral("Users")));
        // Only the administrator part names the installer.
        QVERIFY(!literals.contains(cache + QStringLiteral("0.1.1\\GitBolt-0.1.1-Windows-AMD64.exe")));
        QVERIFY(!script.contains(QLatin1String("$installer")));

        // The administrator part's literals.
        QStringList inner;
        QVERIFY2(literalsOf(elevated, &inner), qPrintable(elevated));
        QVERIFY(inner.contains(cache + QStringLiteral("install.log")));
        QVERIFY(inner.contains(cache + QStringLiteral("install-result")));
        QVERIFY(inner.contains(dir + u'\\'));
        QVERIFY(inner.contains(dir + QStringLiteral("\\bin\\GitBolt.exe")));
        QVERIFY(inner.contains(cache + QStringLiteral("0.1.1\\GitBolt-0.1.1-Windows-AMD64.exe")));
        QVERIFY(inner.contains(QStringLiteral("/S /D=") + dir));
        QVERIFY(inner.contains(QStringLiteral("0.1.1")));
        QVERIFY(!elevated.contains(u'/' + QStringLiteral("Users")));
    }

    // NSIS takes everything after /D= as the folder, so /D= comes last,
    // unquoted, at the end of the one ArgumentList string. No RunAs in
    // either script: GitBolt's ShellExecuteExW already elevated the
    // administrator part, so UAC asks once, while GitBolt is in front.
    void windowsScriptPutsTheFolderLast() {
        HelperInputs in = windowsInputs();
        const QString elevated = windowsElevatedScript(in);
        QVERIFY2(elevated.contains(QLatin1String(
                     "\n            $setup = Start-Process -FilePath $installer -ArgumentList "
                     "'/S /D=C:\\Program Files\\GitBolt' -Wait -PassThru\n")),
                 qPrintable(elevated));
        QCOMPARE(elevated.count(QLatin1String("Start-Process")), 1);
        QCOMPARE(elevated.count(QLatin1String("/D=")), 1);
        QVERIFY(elevated.contains(QLatin1String("\n$installer = 'C:\\Users\\Bob\\AppData\\Local\\GitBolt\\cache\\updates\\"
                                                "0.1.1\\GitBolt-0.1.1-Windows-AMD64.exe'\n")));
        QVERIFY(elevated.contains(QLatin1String("\n$app = 'C:\\Program Files\\GitBolt\\bin\\GitBolt.exe'\n")));
        QVERIFY(elevated.contains(QLatin1String("\n$prefix = 'C:\\Program Files\\GitBolt\\'\n")));

        const QString script = windowsHelperScript(in);
        for (const QString& text : {script, elevated}) {
            QVERIFY(!text.contains(QLatin1String("-Verb"), Qt::CaseInsensitive));
            QVERIFY(!text.contains(QLatin1String("RunAs"), Qt::CaseInsensitive));
            QVERIFY(!text.contains(QLatin1String("-EncodedCommand"), Qt::CaseInsensitive));
        }
        // The helper starts GitBolt, and nothing else.
        QCOMPARE(script.count(QLatin1String("Start-Process")), 1);
        QVERIFY(script.contains(QLatin1String("\n$app = 'C:\\Program Files\\GitBolt\\bin\\GitBolt.exe'\n")));
        QVERIFY(script.contains(QLatin1String("\n    Start-Process -FilePath $app\n")));
        // A folder at a drive's root keeps one backslash.
        in.target = QStringLiteral("D:/");
        QVERIFY(windowsHelperScript(in).contains(QLatin1String("\n$app = 'D:\\bin\\GitBolt.exe'\n")));
        QVERIFY(windowsElevatedScript(in).contains(QLatin1String("\n$prefix = 'D:\\'\n")));
        QVERIFY(windowsElevatedScript(in).contains(QLatin1String("-ArgumentList '/S /D=D:\\' -Wait")));
    }

    // Both parts poll the process list for GitBolt: Wait-Process must
    // open the process, which an over-the-shoulder administrator may not
    // be allowed to. The administrator part gives up at waitSeconds; the
    // helper waits kWindowsHelperExtraSeconds longer.
    void windowsScriptWaitsForGitBoltFirst() {
        const HelperInputs in = windowsInputs();
        const QString script = windowsHelperScript(in);
        const QString elevated = windowsElevatedScript(in);
        // $pid is PowerShell's own (read-only): neither script may use it.
        const QRegularExpression pid(QStringLiteral("\\$pid\\b"), QRegularExpression::CaseInsensitiveOption);
        for (const QString& text : {script, elevated}) {
            QVERIFY(!text.contains(pid));
            QVERIFY(text.contains(QLatin1String("\n$appPid = 4321\n")));
            QVERIFY(text.contains(QLatin1String("\n$waitSeconds = 600\n")));
            QVERIFY(text.contains(QLatin1String("\n$ErrorActionPreference = 'Stop'\n"))
                    || text.startsWith(QLatin1String("$ErrorActionPreference = 'Stop'\n")));
            QVERIFY(!text.contains(QLatin1String("Wait-Process")));
        }
        QVERIFY(elevated.startsWith(QLatin1String("$ErrorActionPreference = 'Stop'\n")));
        QVERIFY(script.startsWith(QLatin1String("# GitBolt's update helper (Windows)")));

        // The administrator part: GitBolt, then the in-use check, then the
        // installer; giving up has its own code.
        const qsizetype wait = elevated.indexOf(QStringLiteral(
            "    $deadline = (Get-Date).AddSeconds($waitSeconds)\n"
            "    while (Get-Process -Id $appPid -ErrorAction SilentlyContinue) {\n"
            "        if ((Get-Date) -ge $deadline) {\n"
            "            $code = %1\n"
            "            break\n"
            "        }\n"
            "        Start-Sleep -Milliseconds 250\n"
            "    }\n"
            "    if ($code -eq 0) {\n").arg(kWindowsExitGaveUp));
        const qsizetype free = elevated.indexOf(QLatin1String("Get-Process -Name GitBolt"));
        const qsizetype install = elevated.indexOf(QLatin1String("Start-Process -FilePath $installer"));
        QVERIFY2(wait > 0, qPrintable(elevated));
        QVERIFY(free > wait && install > free);
        const qsizetype gaveUp = elevated.indexOf(QStringLiteral(
            "} elseif ($code -eq %1) {\n"
            "    $status = 'gave-up'\n"
            "    $reason = 'GitBolt did not quit within ' + $waitSeconds + ' seconds'\n"
            "    Write-Log ('GitBolt did not quit within ' + $waitSeconds + ' seconds: giving up, nothing was changed')\n"
            "    Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue\n").arg(kWindowsExitGaveUp));
        QVERIFY2(gaveUp > install, qPrintable(elevated));

        // The helper, if GitBolt is still there: gave-up (if there is no
        // result yet) and nothing started.
        QVERIFY(script.contains(QLatin1String("\n$elevatedPid = 5678\n")));
        QVERIFY(script.contains(QLatin1String("\n$installSeconds = 900\n")));
        const qsizetype helperWait = script.indexOf(QStringLiteral(
            "\n$deadline = (Get-Date).AddSeconds($waitSeconds + %1)\n"
            "while (Get-Process -Id $appPid -ErrorAction SilentlyContinue) {\n"
            "    if ((Get-Date) -ge $deadline) {\n"
            "        Write-Log ('GitBolt did not quit within ' + $waitSeconds + ' seconds: it is not started again')\n"
            "        Write-Result 'gave-up' ('GitBolt did not quit within ' + $waitSeconds + ' seconds')\n"
            "        Write-Log ''\n"
            "        exit 1\n"
            "    }\n").arg(kWindowsHelperExtraSeconds));
        QVERIFY2(helperWait > 0, qPrintable(script));
        QVERIFY(kWindowsHelperExtraSeconds > 0);
        QVERIFY(script.indexOf(QLatin1String("Start-Process -FilePath $app")) > helperWait);
    }

    // The administrator part waits until no other GitBolt runs from the
    // folder and GitBolt.exe opens with no sharing (else NSIS would
    // install a mix around the locked files; only a sharing or lock
    // violation counts as in use), then installs and checks the version.
    void windowsElevatedScriptWaitsUntilTheInstallIsFree() {
        const QString elevated = windowsElevatedScript(windowsInputs());
        const qsizetype loop =
            elevated.indexOf(QStringLiteral("        $deadline = (Get-Date).AddSeconds(%1)\n").arg(kWindowsInUseSeconds));
        const qsizetype others = elevated.indexOf(QLatin1String(
            "Get-Process -Name GitBolt -ErrorAction SilentlyContinue | Where-Object {\n"
            "                try { $_.Id -ne $appPid -and $_.Path -and $_.Path.Length -ge $prefix.Length -and "
            "$_.Path.Substring(0, $prefix.Length) -eq $prefix } catch { $false }\n"
            "            }).Count -gt 0\n"));
        const qsizetype exclusive = elevated.indexOf(QLatin1String(
            "                try {\n"
            "                    [IO.File]::Open($app, 'Open', 'ReadWrite', 'None').Dispose()\n"
            "                } catch {\n"));
        const qsizetype sharing = elevated.indexOf(QLatin1String(
            "                    while ($e.InnerException) {\n"
            "                        $e = $e.InnerException\n"
            "                    }\n"
            "                    $inUse = @(32, 33) -contains ($e.HResult -band 0xFFFF)\n"));
        const qsizetype busy = elevated.indexOf(QStringLiteral("$code = %1\n").arg(kWindowsExitInUse));
        const qsizetype pause = elevated.indexOf(QLatin1String("Start-Sleep -Seconds 1"));
        const qsizetype install = elevated.indexOf(QLatin1String("Start-Process -FilePath $installer"));
        const qsizetype noInstaller = elevated.indexOf(QStringLiteral("$code = %1\n").arg(kWindowsExitNoInstaller));
        const qsizetype itsCode = elevated.indexOf(QLatin1String(
            "        $setupCode = $setup.ExitCode\n"
            "        if ($null -ne $setupCode -and $setupCode -ne 0) {\n"
            "            $code = $setupCode\n"));
        const qsizetype version = elevated.indexOf(QLatin1String(
            "        $found = [string](Get-Item -LiteralPath $app).VersionInfo.ProductVersion\n"
            "        if ($found.Trim() -ne $version) {\n"));
        const qsizetype wrongVersion = elevated.indexOf(QStringLiteral("$code = %1\n").arg(kWindowsExitWrongVersion));
        const qsizetype failed = elevated.indexOf(QStringLiteral("} catch {\n    $code = %1\n").arg(kWindowsExitError));
        QVERIFY(elevated.contains(QLatin1String("\n$version = '0.1.1'\n")));
        for (const qsizetype at : {loop, others, exclusive, sharing, busy, pause, install, noInstaller, itsCode, version,
                                   wrongVersion, failed})
            QVERIFY2(at > 0, qPrintable(elevated));
        QVERIFY(loop < others && others < exclusive && exclusive < sharing && sharing < busy && busy < pause);
        QVERIFY(pause < install && install < noInstaller && noInstaller < itsCode && itsCode < version);
        QVERIFY(version < wrongVersion && wrongVersion < failed);
        // The codes it can end with are distinct, and none is NSIS's.
        const QList<int> codes = {kWindowsExitInUse, kWindowsExitNoInstaller, kWindowsExitWrongVersion,
                                  kWindowsExitError, kWindowsExitGaveUp};
        for (const int code : codes) {
            QVERIFY(code > 2);
            QCOMPARE(codes.count(code), 1);
        }
    }

    // The administrator part's result: ok only for 0, gave-up, or failed
    // with why. One ASCII line, no BOM, written once and followed only by
    // a log flush (the helper starts GitBolt once it is there). Neither
    // part says declined: UAC's No reaches GitBolt itself.
    void windowsScriptsReportEachOutcome() {
        const QString elevated = windowsElevatedScript(windowsInputs());
        const QString write = QStringLiteral(
            "        Set-Content -LiteralPath $result -Encoding Ascii -NoNewline -Value "
            "($status + ' ' + $version + ' ' + ($reason -replace '[\\r\\n]+', ' ') + [char]10)\n");
        QVERIFY2(elevated.contains(write), qPrintable(elevated));
        QVERIFY(elevated.contains(QLatin1String("\n$status = 'failed'\n$reason = ''\n")));
        const auto branch = [&](const QString& test, const QString& then) {
            return elevated.contains(QStringLiteral("if (%1) {\n%2").arg(test, then));
        };
        QVERIFY(branch(QStringLiteral("$code -eq 0"), QStringLiteral("    $status = 'ok'\n")));
        QVERIFY(branch(QStringLiteral("$code -eq %1").arg(kWindowsExitGaveUp), QStringLiteral("    $status = 'gave-up'\n")));
        QVERIFY(branch(QStringLiteral("$code -eq %1").arg(kWindowsExitInUse),
                       QStringLiteral("    $reason = 'another GitBolt kept running from the install folder'\n")));
        QVERIFY(branch(QStringLiteral("$code -eq %1").arg(kWindowsExitNoInstaller),
                       QStringLiteral("    $reason = 'the installer could not be started'\n")));
        QVERIFY(branch(QStringLiteral("$code -eq %1").arg(kWindowsExitWrongVersion),
                       QStringLiteral("    $reason = 'the installer ran, but GitBolt.exe is not version ' + $version\n")));
        QVERIFY(branch(QStringLiteral("$code -eq %1").arg(kWindowsExitError),
                       QStringLiteral("    $reason = 'the administrator part of the update failed'\n")));
        QVERIFY(elevated.contains(QLatin1String(
            "} else {\n    $reason = 'the installer exited with code ' + $code\n}\n")));
        QVERIFY(elevated.contains(QLatin1String(
            "    Write-Log ('the installer exited with code 0, and GitBolt.exe is now version ' + $version)\n")));
        QVERIFY(elevated.contains(QLatin1String("    Write-Log ('nothing was installed (code ' + $code + '): ' + $reason)\n")));
        QCOMPARE(elevated.count(QLatin1String("Write-Result ")), 1);
        QVERIFY(elevated.endsWith(QLatin1String("\nWrite-Log ''\nWrite-Result $status $reason\nWrite-Log ''\nexit $code")));

        const QString script = windowsHelperScript(windowsInputs());
        for (const QString& text : {script, elevated})
            QVERIFY(!text.contains(QLatin1String("declined")));
        // The helper's results never replace the administrator part's.
        QVERIFY(script.contains(write));
        QVERIFY(script.contains(QLatin1String(
            "function Write-Result([string]$status, [string]$reason) {\n"
            "    if ($result -eq '' -or (Test-Path -LiteralPath $result)) {\n"
            "        return\n")));
        QVERIFY(elevated.contains(QLatin1String(
            "function Write-Result([string]$status, [string]$reason) {\n"
            "    if ($result -eq '') {\n"
            "        return\n")));
    }

    // Once GitBolt has quit, the helper waits for the administrator
    // part's result, at most installSeconds or until that part has been
    // gone 5 s; with none, it writes failed itself. Then it starts
    // GitBolt either way.
    void windowsHelperWaitsForTheResultThenStartsGitBolt() {
        const QString script = windowsHelperScript(windowsInputs());
        const qsizetype quit = script.indexOf(QLatin1String(
            "\nWrite-Log ('GitBolt has quit: waiting for the administrator part (pid ' + $elevatedPid + ') to install "
            "GitBolt ' + $version)\n"));
        const qsizetype loop = script.indexOf(QLatin1String(
            "\n$deadline = (Get-Date).AddSeconds($installSeconds)\n"
            "$endedBy = $null\n"
            "while (-not ($result -ne '' -and (Test-Path -LiteralPath $result))) {\n"
            "    if ((Get-Date) -ge $deadline) {\n"
            "        Write-Log ('the administrator part did not finish within ' + $installSeconds + ' seconds')\n"
            "        Write-Result 'failed' ('the administrator part of the update did not finish within ' + "
            "$installSeconds + ' seconds')\n"
            "        break\n"
            "    }\n"
            "    if ($elevatedPid -gt 0 -and -not (Get-Process -Id $elevatedPid -ErrorAction SilentlyContinue)) {\n"
            "        if ($null -eq $endedBy) {\n"
            "            $endedBy = (Get-Date).AddSeconds(5)\n"
            "        } elseif ((Get-Date) -ge $endedBy) {\n"
            "            Write-Log ('the administrator part (pid ' + $elevatedPid + ') ended without an install result')\n"
            "            Write-Result 'failed' 'the administrator part of the update ended without a result'\n"
            "            break\n"
            "        }\n"
            "    }\n"
            "    Start-Sleep -Milliseconds 250\n"
            "}\n"));
        const qsizetype start = script.indexOf(QLatin1String(
            "\nWrite-Log ('starting ' + $app)\n"
            "try {\n"
            "    Start-Process -FilePath $app\n"
            "} catch {\n"
            "    Write-Log ('could not start GitBolt: ' + $_.Exception.Message)\n"
            "}\n"));
        QVERIFY2(quit > 0 && loop > quit && start > loop, qPrintable(script));
        QVERIFY(script.endsWith(
            QLatin1String("Write-Log ('could not start GitBolt: ' + $_.Exception.Message)\n}\nWrite-Log ''\n")));
        // No administrator part's pid known: only the timeout ends it.
        HelperInputs unknown = windowsInputs();
        unknown.elevatedPid = 0;
        unknown.installSeconds = 120;
        QVERIFY(windowsHelperScript(unknown).contains(QLatin1String("\n$elevatedPid = 0\n$installSeconds = 120\n")));
    }

    // Constrained Language mode (AppLocker or WDAC script rules) refuses
    // method calls on all but core types, and an unguarded one would end
    // a part before it reports or starts GitBolt again, so the one .NET
    // call ([IO.File]::Open, the in-use check) is in a try. With ASCII
    // inputs both scripts are ASCII, so every reason fits the ASCII result.
    void windowsScriptsRunInConstrainedLanguage() {
        const QString script = windowsHelperScript(windowsInputs());
        const QString elevated = windowsElevatedScript(windowsInputs());
        for (const QString& text : {script, elevated}) {
            for (const char* refused : {"[Convert]", "[Text.Encoding]", "[StringComparison]", "WriteAllText",
                                        "[Environment]", "New-Object", "Add-Type", "[Diagnostics."})
                QVERIFY2(!text.contains(QLatin1String(refused)), refused);
            for (const QChar c : text)
                QVERIFY2(c.unicode() < 0x80, qPrintable(QString(c)));
        }
        QCOMPARE(script.count(QLatin1String("]::")), 0);
        QCOMPARE(elevated.count(QLatin1String("]::")), 1);
        const QStringList lines = elevated.split(u'\n');
        for (qsizetype i = 0; i < lines.size(); ++i) {
            if (lines.at(i).contains(QLatin1String("]::"))) {
                QVERIFY(i > 0);
                QCOMPARE(lines.at(i - 1).trimmed(), QStringLiteral("try {"));
                QCOMPARE(lines.at(i + 1).trimmed(), QStringLiteral("} catch {"));
            }
        }
        // The rest is cmdlets; the log's is Out-File, never Add-Content.
        for (const QString& text : {script, elevated}) {
            QVERIFY(text.contains(QLatin1String(
                "Out-File -LiteralPath $log -Append -Encoding UTF8 -NoNewline -InputObject $script:logPending")));
            QVERIFY(!text.contains(QLatin1String("Add-Content")));
        }
    }

    // Both parts use windowsScriptPreamble's one Write-Log (run for real
    // by the windowsLog* tests): Out-File, never Add-Content
    // (PowerShell#27947). Each part flushes what is kept with
    // Write-Log '': the administrator part before and after its result,
    // the helper when it gives up and at its very end.
    void windowsScriptsShareOneWriteLog() {
        const HelperInputs in = windowsInputs();
        const QString elevated = windowsElevatedScript(in);
        const QString script = windowsHelperScript(in);
        QVERIFY(elevated.startsWith(windowsScriptPreamble(in, false) + u'\n'));
        QVERIFY(script.contains(u'\n' + windowsScriptPreamble(in, true) + u'\n'));
        QVERIFY(windowsScriptPreamble(in, true).startsWith(QLatin1String("$ErrorActionPreference = 'Stop'\n")));
        const QString writeLog = QStringLiteral(
            "\n$script:logPending = ''\n"
            "$script:logPatience = %1\n"
            "function Write-Log([string]$text) {\n"
            "    $patience = %1\n"
            "    if ($text -ne '') {\n"
            "        $script:logPending += (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + ' ' + $text + [char]13 + [char]10\n"
            "        $patience = $script:logPatience\n"
            "    }\n"
            "    $until = (Get-Date).AddSeconds($patience)\n"
            "    while ($script:logPending) {\n"
            "        try {\n"
            "            Out-File -LiteralPath $log -Append -Encoding UTF8 -NoNewline -InputObject $script:logPending\n"
            "            $script:logPending = ''\n"
            "            $script:logPatience = %1\n"
            "        } catch {\n"
            "            if ((Get-Date) -ge $until) {\n"
            "                $script:logPatience = 0\n"
            "                try {\n"
            "                    Write-Warning ('install.log: ' + $_.Exception.Message)\n"
            "                } catch {\n"
            "                }\n"
            "                return\n"
            "            }\n"
            "            Start-Sleep -Milliseconds (Get-Random -Minimum 10 -Maximum 100)\n"
            "        }\n"
            "    }\n"
            "}\n"
            "function Write-Result(").arg(kWindowsLogPatienceSeconds);
        QVERIFY(kWindowsLogPatienceSeconds > 0 && 3 * kWindowsLogPatienceSeconds < kWindowsHelperExtraSeconds);
        for (const QString& text : {script, elevated}) {
            QVERIFY2(text.contains(writeLog), qPrintable(text));
            QCOMPARE(text.count(QLatin1String("function Write-Log(")), 1);
            QCOMPARE(text.count(QLatin1String("Out-File")), 1);
            // Only the result uses Set-Content: it has one writer at a
            // time and is read only once it is there.
            QCOMPARE(text.count(QLatin1String("Set-Content")), 1);
            QVERIFY(text.contains(QLatin1String("        Set-Content -LiteralPath $result ")));
        }
        // Where each part ends: its one exit, or the helper's last line.
        QCOMPARE(elevated.count(QLatin1String("exit ")), 1);
        QCOMPARE(elevated.count(QLatin1String("Write-Log ''")), 2);
        QCOMPARE(script.count(QLatin1String("exit ")), 1);
        QCOMPARE(script.count(QLatin1String("Write-Log ''")), 2);
        QVERIFY(elevated.endsWith(QLatin1String("\nWrite-Log ''\nWrite-Result $status $reason\nWrite-Log ''\nexit $code")));
        QVERIFY(script.contains(QLatin1String(
            "        Write-Result 'gave-up' ('GitBolt did not quit within ' + $waitSeconds + ' seconds')\n"
            "        Write-Log ''\n"
            "        exit 1\n")));
        QVERIFY(script.endsWith(QLatin1String("\n}\nWrite-Log ''\n")));
    }

    // Both command lines (the helper's for CreateProcess, the
    // administrator part's for ShellExecuteExW) stay under the
    // 32767-character limit, the program included, even with long paths
    // full of (doubled) quotes.
    void windowsCommandLinesFitWithLongPaths() {
        const QString longName = QStringLiteral("Bob's \u2018quoted\u2019 folder ").repeated(8).left(220);
        HelperInputs in = windowsInputs();
        in.pid = 4294967295;
        in.elevatedPid = 4294967295;
        in.target = QStringLiteral("C:/") + longName + QStringLiteral("/GitBolt");
        in.staged = QStringLiteral("C:/Users/") + longName + QStringLiteral("/AppData/Local/GitBolt/cache/updates/"
                                                                          "0.1.10/GitBolt-0.1.10-Windows-AMD64.exe");
        in.logFile = QStringLiteral("C:/Users/") + longName + QStringLiteral("/AppData/Local/GitBolt/cache/updates/install.log");
        in.resultFile = QStringLiteral("C:/Users/") + longName
                        + QStringLiteral("/AppData/Local/GitBolt/cache/updates/install-result");
        in.newVersion = QStringLiteral("999999.999999.999999");
        const QString powershell = QStringLiteral("C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
        const qsizetype helper = commandLineLength(powershell, powerShellArguments(windowsHelperScript(in)));
        const qsizetype elevated = shellExecuteLength(powershell, powerShellParameters(windowsElevatedScript(in)));
        QVERIFY2(helper < kWindowsCommandLineLimit, qPrintable(QString::number(helper)));
        QVERIFY2(elevated < kWindowsCommandLineLimit, qPrintable(QString::number(elevated)));
        // With room to spare: the usual paths are a fraction of that.
        const HelperInputs usual = windowsInputs();
        const qsizetype usualHelper = commandLineLength(powershell, powerShellArguments(windowsHelperScript(usual)));
        const qsizetype usualElevated =
            shellExecuteLength(powershell, powerShellParameters(windowsElevatedScript(usual)));
        QVERIFY2(usualHelper < kWindowsCommandLineLimit / 2, qPrintable(QString::number(usualHelper)));
        QVERIFY2(usualElevated < kWindowsCommandLineLimit * 2 / 3, qPrintable(QString::number(usualElevated)));
        qInfo("command lines: helper %lld, administrator part %lld characters with long paths; usually %lld and %lld",
              qint64(helper), qint64(elevated), qint64(usualHelper), qint64(usualElevated));
    }

    // ShellExecuteExW gets the administrator part's arguments as one
    // string: exactly powerShellArguments, space-separated (none has a
    // space or a quote, so nothing needs quoting), the script last.
    void shellExecuteGetsTheArgumentsAsOneString() {
        const QString elevated = windowsElevatedScript(windowsInputs());
        const QString parameters = powerShellParameters(elevated);
        QCOMPARE(parameters, QStringLiteral("-NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand ")
                                 + QString::fromLatin1(powerShellEncodedCommand(elevated)));
        QCOMPARE(parameters.split(u' '), powerShellArguments(elevated));
        for (const QString& argument : powerShellArguments(elevated)) {
            QVERIFY(!argument.isEmpty());
            QVERIFY(!argument.contains(u' ') && !argument.contains(u'"') && !argument.contains(u'\t'));
        }
    }

    void encodedCommandIsUtf16LittleEndianBase64() {
        // Known answer: "dir" is d\0i\0r\0.
        QCOMPARE(powerShellEncodedCommand(QStringLiteral("dir")), QByteArray("ZABpAHIA"));
        HelperInputs in = windowsInputs();
        in.target = QStringLiteral("C:/Program Files/G\u00eftBolt \u2019\u4e2d");
        for (const QString& script : {windowsHelperScript(in), windowsElevatedScript(in)}) {
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
        }
    }

    // ---------------------------------------------------------------
    // Windows, for real: Write-Log (every run there)
    // ---------------------------------------------------------------

    // Three powershell.exe (one in Constrained Language mode) write to one
    // log, held busy for 1.5 s as the leaked Add-Content handle held it:
    // every line goes in once, whole and in order, and no Write-Log takes
    // over 1.5 s past its patience. Each test's waits add up to under
    // ctest's 60 s per function, so a hang fails a QVERIFY2 with the
    // writers' output and the guard stops the rest.
    void windowsLogKeepsEveryLineOfWritersAtOnce() {
#if !defined(Q_OS_WIN)
        QSKIP("Windows only: runs Windows PowerShell");
#else
        constexpr int kWriters = 3;
        constexpr int kLines = 100;
        QVERIFY2(QFileInfo(windowsPowerShell()).isFile(), qPrintable(windowsPowerShell()));
        const QString go = path(QStringLiteral("go"));
        const auto ready = [this](int writer) { return path(QStringLiteral("ready-%1").arg(writer)); };
        const auto stats = [this](int writer) { return path(QStringLiteral("stats-%1").arg(writer)); };
        HANDLE held = holdOpen(log());
        QVERIFY2(held != INVALID_HANDLE_VALUE, qPrintable(QString::number(GetLastError())));
        const auto release = [&held] {
            if (held != INVALID_HANDLE_VALUE)
                CloseHandle(held);
            held = INVALID_HANDLE_VALUE;
        };
        std::vector<Writer> writers;
        auto stopAll = qScopeGuard([&] {
            release();
            stopWriters(writers);
        });

        // Each writer warms up on a log of its own, so that its first real
        // line meets the held log, not its own slow start; then it says it
        // is ready, and in which language mode, and waits for the go. (Two
        // cold first lines meeting is the real-install test's case 1.)
        for (int i = 1; i <= kWriters; ++i) {
            const QString body =
                QStringLiteral("$ready = %1\n"
                               "$go = %2\n"
                               "$stats = %3\n"
                               "$real = $log\n"
                               "$log = %4\n"
                               "Write-Log 'warming up'\n"
                               "$log = $real\n"
                               "$mode = 'ConstrainedLanguage'\n"
                               "try {\n"
                               "    $null = [IO.File]::Exists($log)\n"
                               "    $mode = 'FullLanguage'\n"
                               "} catch {\n"
                               "}\n"
                               "Out-File -LiteralPath $ready -Encoding Ascii -InputObject $mode\n"
                               "$until = (Get-Date).AddSeconds(40)\n"
                               "while (-not (Test-Path -LiteralPath $go)) {\n"
                               "    if ((Get-Date) -ge $until) {\n"
                               "        exit 3\n"
                               "    }\n"
                               "    Start-Sleep -Milliseconds 10\n"
                               "}\n"
                               "$longest = 0\n"
                               "for ($n = 1; $n -le %5; $n++) {\n"
                               "    $start = Get-Date\n"
                               "    Write-Log ('writer %6 line ' + $n)\n"
                               "    $took = ((Get-Date) - $start).TotalMilliseconds\n"
                               "    if ($took -gt $longest) {\n"
                               "        $longest = $took\n"
                               "    }\n"
                               "    Start-Sleep -Milliseconds (Get-Random -Minimum 0 -Maximum 20)\n"
                               "}\n"
                               "Write-Log ''\n"
                               "Out-File -LiteralPath $stats -Encoding Ascii -InputObject ([string][int]$longest)\n"
                               "exit 0\n")
                    .arg(literal(ready(i)), literal(go), literal(stats(i)),
                         literal(path(QStringLiteral("warm-up-%1.log").arg(i))), QString::number(kLines),
                         QString::number(i));
            writers.push_back(Writer{startPowerShell(writeLogScript(body, i == kWriters)), {}});
            QVERIFY2(writers.back().process->waitForStarted(10000), qPrintable(writers.back().process->errorString()));
        }
        const auto allReady = [&] {
            for (int i = 1; i <= kWriters; ++i) {
                if (!QFileInfo::exists(ready(i)))
                    return false;
            }
            return true;
        };
        QVERIFY2(QTest::qWaitFor([&] { return allReady() || anyFinished(writers); }, 25000) && allReady(),
                 qPrintable(QStringLiteral("not every writer got ready\n") + writerDiagnostics(writers)));

        // Go, into a log that stays busy 1.5 s longer, then is free.
        QVERIFY(writeFile(go, "go\n"));
        QTest::qWait(1500);
        release();
        QVERIFY2(QTest::qWaitFor([&] { return allFinished(writers); }, 20000),
                 qPrintable(QStringLiteral("the writers didn't finish\n") + writerDiagnostics(writers)));
        const QString details = writerDiagnostics(writers);
        QVERIFY2(allSucceeded(writers), qPrintable(details));
        QCOMPARE(readFile(ready(1)).trimmed(), QStringLiteral("FullLanguage"));
        QCOMPARE(readFile(ready(kWriters)).trimmed(), QStringLiteral("ConstrainedLanguage"));

        QStringList lines;
        QString why;
        QVERIFY2(logLines(&lines, &why), qPrintable(why + u'\n' + details));
        QList<int> next(kWriters + 1, 1);
        static const QRegularExpression written(QStringLiteral("^writer (\\d+) line (\\d+)$"));
        for (const QString& line : std::as_const(lines)) {
            const QRegularExpressionMatch match = written.match(line);
            QVERIFY2(match.hasMatch(), qPrintable(line + u'\n' + details));
            const int writer = match.captured(1).toInt();
            QVERIFY2(writer >= 1 && writer <= kWriters, qPrintable(line));
            // Each writer's lines in its order, none twice, none missing.
            QVERIFY2(match.captured(2).toInt() == next[writer],
                     qPrintable(QStringLiteral("writer %1's line %2 was due, not '%3'\n")
                                    .arg(QString::number(writer), QString::number(next[writer]), line)
                                + details));
            ++next[writer];
        }
        for (int i = 1; i <= kWriters; ++i) {
            QVERIFY2(next[i] == kLines + 1,
                     qPrintable(QStringLiteral("writer %1's line %2 is missing\n").arg(i).arg(next[i]) + details));
            bool ok = false;
            const int longest = readFile(stats(i)).trimmed().toInt(&ok);
            QVERIFY2(ok, qPrintable(details));
            QVERIFY2(longest <= kWindowsLogPatienceSeconds * 1000 + 1500,
                     qPrintable(QStringLiteral("writer %1 waited %2 ms for the log\n").arg(i).arg(longest) + details));
            qInfo("writer %d: its longest Write-Log took %d ms", i, longest);
        }
        QCOMPARE(lines.size(), kWriters * kLines);
#endif
    }

    // Only the first Write-Log into a log that stays busy waits
    // kWindowsLogPatienceSeconds; the next tries once. Both lines are
    // kept and go in, in order, with the next one once the log is free.
    void windowsLogWaitsOnlySoLongAndKeepsWhatItCouldntWrite() {
#if !defined(Q_OS_WIN)
        QSKIP("Windows only: runs Windows PowerShell");
#else
        QVERIFY2(QFileInfo(windowsPowerShell()).isFile(), qPrintable(windowsPowerShell()));
        const QString go = path(QStringLiteral("go"));
        const QString stats = path(QStringLiteral("stats"));
        HANDLE held = holdOpen(log());
        QVERIFY2(held != INVALID_HANDLE_VALUE, qPrintable(QString::number(GetLastError())));
        const auto release = [&held] {
            if (held != INVALID_HANDLE_VALUE)
                CloseHandle(held);
            held = INVALID_HANDLE_VALUE;
        };
        std::vector<Writer> writers;
        auto stopAll = qScopeGuard([&] {
            release();
            stopWriters(writers);
        });

        const QString body = QStringLiteral(
                                 "$go = %1\n"
                                 "$stats = %2\n"
                                 "$real = $log\n"
                                 "$log = %3\n"
                                 "Write-Log 'warming up'\n"
                                 "$log = $real\n"
                                 "$start = Get-Date\n"
                                 "Write-Log 'one, while the log is busy'\n"
                                 "$first = ((Get-Date) - $start).TotalMilliseconds\n"
                                 "$start = Get-Date\n"
                                 "Write-Log 'two, while it still is'\n"
                                 "$second = ((Get-Date) - $start).TotalMilliseconds\n"
                                 "Out-File -LiteralPath $stats -Encoding Ascii -InputObject "
                                 "([string][int]$first + ' ' + [string][int]$second)\n"
                                 "$until = (Get-Date).AddSeconds(40)\n"
                                 "while (-not (Test-Path -LiteralPath $go)) {\n"
                                 "    if ((Get-Date) -ge $until) {\n"
                                 "        exit 3\n"
                                 "    }\n"
                                 "    Start-Sleep -Milliseconds 10\n"
                                 "}\n"
                                 "Write-Log 'three, once it is free'\n"
                                 "Write-Log ''\n"
                                 "exit 0\n")
                                 .arg(literal(go), literal(stats), literal(path(QStringLiteral("warm-up.log"))));
        writers.push_back(Writer{startPowerShell(writeLogScript(body, false)), {}});
        QVERIFY2(writers.back().process->waitForStarted(10000), qPrintable(writers.back().process->errorString()));
        // Both Write-Logs done (the log still busy), then it is free.
        QVERIFY2(QTest::qWaitFor([&] { return QFileInfo::exists(stats) || allFinished(writers); }, 20000)
                     && QFileInfo::exists(stats),
                 qPrintable(writerDiagnostics(writers)));
        release();
        QVERIFY(writeFile(go, "go\n"));
        QVERIFY2(QTest::qWaitFor([&] { return allFinished(writers); }, 20000),
                 qPrintable(QStringLiteral("the writer didn't finish\n") + writerDiagnostics(writers)));
        const QString details = writerDiagnostics(writers);
        QVERIFY2(allSucceeded(writers), qPrintable(details));
        // Its warnings, for the record: each wait in vain says why.
        qInfo("%s", qPrintable(details));

        const QStringList took = readFile(stats).trimmed().split(u' ');
        QCOMPARE(took.size(), 2);
        const int first = took.at(0).toInt();
        const int second = took.at(1).toInt();
        QVERIFY2(first >= kWindowsLogPatienceSeconds * 1000 - 500 && first <= kWindowsLogPatienceSeconds * 1000 + 1500,
                 qPrintable(QStringLiteral("the first Write-Log took %1 ms\n").arg(first) + details));
        QVERIFY2(second < 1000, qPrintable(QStringLiteral("the second Write-Log took %1 ms\n").arg(second) + details));
        QStringList lines;
        QString why;
        QVERIFY2(logLines(&lines, &why), qPrintable(why + u'\n' + details));
        QCOMPARE(lines, (QStringList{QStringLiteral("one, while the log is busy"), QStringLiteral("two, while it still is"),
                                     QStringLiteral("three, once it is free")}));
#endif
    }

    // ---------------------------------------------------------------
    // Windows, for real (opt-in)
    // ---------------------------------------------------------------

    // A whole Windows update of a real install. Opt-in: CI's NSIS smoke
    // test installs the installer it built, names both
    // (GITBOLT_TEST_NSIS_INSTALLER, GITBOLT_TEST_NSIS_DIR) and runs this
    // alone. launchWindowsUpdate starts both parts as GitBolt does (the
    // hosted runner has UAC off, so runas is granted with no prompt) for
    // a stand-in GitBolt; the test closes every GitBolt.exe it starts.
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
        // The updater's reader on the real installer: the minimum Windows
        // the build gave it (cmake/Packaging.cmake), which prepare() checks.
        QCOMPARE(windowsMinimumOf(installer), QStringLiteral(GITBOLT_WINDOWS_MINIMUM));

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

        // A copy of the installer where the Downloader leaves it,
        // <updates>/<version>/<asset name> (giving up deletes it).
        const QString fileName = QFileInfo(installer).fileName();
        static const QRegularExpression named(QStringLiteral("^GitBolt-([0-9.]+)-"));
        const std::optional<Version> version = Version::parse(named.match(fileName).captured(1));
        QVERIFY2(version.has_value(), qPrintable(fileName));
        QCOMPARE(fileName, assetNameFor(PackageKind::WindowsInstall, *version));
        const QString staged = path(QStringLiteral("updates/%1/%2").arg(version->toString(), fileName));
        QVERIFY(QDir().mkpath(QFileInfo(staged).path()));
        QVERIFY(QFile::copy(installer, staged));

        // A stand-in for GitBolt, ended when GitBolt would quit.
        const auto startStandIn = [this] {
            waited_ = std::make_unique<QProcess>();
            waited_->start(windowsPowerShell(), {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                                                 QStringLiteral("-Command"), QStringLiteral("Start-Sleep -Seconds 300")});
            return waited_->waitForStarted() ? waited_->processId() : qint64(0);
        };
        qint64 gitBoltPid = startStandIn();
        QVERIFY(gitBoltPid > 0);

        // What the helper hands to the GitBolt it starts: an instance name
        // of its own, no telemetry or update check, none of this run's Qt
        // settings (the install has no offscreen plugin), its own TEMP.
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
        QList<qint64> parts;
        auto stopAll = qScopeGuard([&] {
            for (const qint64 pid : parts)
                if (gitbolt::test::processAlive(pid))
                    endProcess(static_cast<DWORD>(pid));
            for (const QString& program : {app, staged})
                for (const DWORD running : processesOf(program))
                    endProcess(running);
        });
        // Starts both parts as Installer::launchHelper does, with a fresh
        // log and no result.
        const auto launch = [&](int waitSeconds) {
            HelperInputs in;
            in.pid = gitBoltPid;
            in.waitSeconds = waitSeconds;
            in.installSeconds = 5 * 60;
            in.logFile = log();
            in.resultFile = result();
            in.newVersion = version->toString();
            in.target = package.target;
            in.staged = staged;
            QFile::remove(log());
            QFile::remove(result());
            const WindowsLaunch launched = launchWindowsUpdate(in, 0, env);
            qInfo("launchWindowsUpdate: outcome %d, administrator part %lld, helper %lld, error '%s'",
                  int(launched.outcome), launched.elevatedPid, launched.helperPid, qPrintable(launched.error));
            if (launched.elevatedPid > 0)
                parts.append(launched.elevatedPid);
            if (launched.helperPid > 0)
                parts.append(launched.helperPid);
            return launched;
        };
        // Both started, each pid known (the helper watches the
        // administrator part's).
        const auto bothStarted = [](const WindowsLaunch& launched) {
            return launched.outcome == WindowsLaunch::Outcome::Started && launched.error.isEmpty()
                   && launched.elevatedPid > 0 && launched.helperPid > 0;
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
        // Waits for the helper's last line, "starting <app>" (or why that
        // failed), then for it to exit. False if it doesn't, in time.
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
        // Each part's first line: it is waiting for the stand-in.
        const auto adminWaiting = [&] {
            return QStringLiteral("administrator part: waiting for GitBolt (pid %1) to quit").arg(gitBoltPid);
        };
        const auto helperWaiting = [&] {
            return QStringLiteral("GitBolt update helper: waiting for GitBolt (pid %1) to quit").arg(gitBoltPid);
        };
        const auto bothWaiting = [&](int timeoutMs) {
            return QTest::qWaitFor(
                [&] {
                    const QString text = readFile(log());
                    return text.contains(adminWaiting()) && text.contains(helperWaiting());
                },
                timeoutMs);
        };

        // 1. GitBolt doesn't quit in time: the administrator part gives up
        // and deletes the download; the helper starts nothing.
        const WindowsLaunch givingUp = launch(2);
        QVERIFY2(bothStarted(givingUp), qPrintable(givingUp.error + QLatin1Char('\n') + diagnostics()));
        QVERIFY2(ended(givingUp.elevatedPid, 60000),
                 qPrintable(QStringLiteral("the administrator part didn't give up\n") + diagnostics()));
        // The administrator part's result: the helper writes none until
        // kWindowsHelperExtraSeconds after that part gave up.
        QVERIFY2(QFileInfo::exists(result()),
                 qPrintable(QStringLiteral("the administrator part wrote no result\n") + diagnostics()));
        QVERIFY2(ended(givingUp.helperPid, (2 + kWindowsHelperExtraSeconds + 60) * 1000),
                 qPrintable(QStringLiteral("the helper didn't give up\n") + diagnostics()));
        // Every line of both, the first ones written at the same moment.
        QVERIFY2(readFile(log()).contains(adminWaiting()), qPrintable(diagnostics()));
        QVERIFY2(readFile(log()).contains(helperWaiting()), qPrintable(diagnostics()));
        QVERIFY2(readFile(log()).contains(QLatin1String("did not quit within 2 seconds: giving up")),
                 qPrintable(diagnostics()));
        QVERIFY2(readFile(log()).contains(QLatin1String("did not quit within 2 seconds: it is not started again")),
                 qPrintable(diagnostics()));
        QVERIFY2(!readFile(log()).contains(QLatin1String(" running ")), qPrintable(diagnostics()));
        QVERIFY2(!readFile(log()).contains(QLatin1String(" starting ")), qPrintable(diagnostics()));
        QCOMPARE(readFile(result()),
                 QStringLiteral("gave-up %1 GitBolt did not quit within 2 seconds\n").arg(version->toString()));
        QVERIFY(!QFileInfo::exists(staged));
        QVERIFY(processesOf(app).isEmpty());
        QCOMPARE(waited_->state(), QProcess::Running);

        // 2. Another GitBolt runs from the install folder (another user's,
        // or one reopened meanwhile): nothing is installed, the result says
        // why, and the helper starts the old copy again.
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
        const WindowsLaunch blocked = launch(120);
        QVERIFY2(bothStarted(blocked), qPrintable(blocked.error + QLatin1Char('\n') + diagnostics()));
        QVERIFY2(bothWaiting(60000), qPrintable(diagnostics()));
        quitWaited();
        QVERIFY2(finishes(blocked.helperPid, (kWindowsInUseSeconds + 120) * 1000),
                 qPrintable(QStringLiteral("the blocked helper is still running\n") + diagnostics()));
        QVERIFY(ended(blocked.elevatedPid, 30000));
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

        // 3. Again, with the folder free. While GitBolt runs, both parts
        // only wait.
        gitBoltPid = startStandIn();
        QVERIFY(gitBoltPid > 0);
        const WindowsLaunch clean = launch(120);
        QVERIFY2(bothStarted(clean), qPrintable(clean.error + QLatin1Char('\n') + diagnostics()));
        QVERIFY2(bothWaiting(60000), qPrintable(diagnostics()));
        QTest::qWait(1000);
        QVERIFY2(!readFile(log()).contains(QLatin1String(" running ")), qPrintable(diagnostics()));
        QVERIFY(gitbolt::test::processAlive(clean.elevatedPid));
        QVERIFY(gitbolt::test::processAlive(clean.helperPid));
        QVERIFY(!QFileInfo::exists(result()));

        // GitBolt quits.
        quitWaited();
        QVERIFY2(finishes(clean.helperPid, 4 * 60 * 1000),
                 qPrintable(QStringLiteral("the helper is still running\n") + diagnostics()));
        QVERIFY(ended(clean.elevatedPid, 30000));
        const QString story = readFile(log());
        QVERIFY2(story.contains(QStringLiteral("GitBolt has quit: waiting for the administrator part (pid %1)")
                                    .arg(clean.elevatedPid)),
                 qPrintable(diagnostics()));
        QVERIFY2(story.contains(QStringLiteral(" running ") + QDir::toNativeSeparators(staged)),
                 qPrintable(diagnostics()));
        // Exit code 0, and GitBolt.exe's ProductVersion is the installer's.
        QVERIFY2(story.contains(QStringLiteral(" the installer exited with code 0, and GitBolt.exe is now version %1")
                                    .arg(version->toString())),
                 qPrintable(diagnostics()));
        QCOMPARE(readFile(result()), QStringLiteral("ok %1 \n").arg(version->toString()));
        QVERIFY2(lastLine().endsWith(startingApp), qPrintable(diagnostics()));

        // The relaunched GitBolt shows a window: it got past its DLLs, Qt's
        // platform plugin and the single-instance check (a copy that handed
        // off would exit without one).
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
