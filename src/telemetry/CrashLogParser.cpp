#include "telemetry/CrashLogParser.h"

#include <QCryptographicHash>
#include <QHash>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>
#include <cstdlib>
#include <memory>

#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define GITBOLT_HAVE_CXA_DEMANGLE 1
#endif

namespace gitbolt::telemetry {

namespace {

constexpr qsizetype kMaxFrames = 64;
constexpr qsizetype kMaxFunctionLength = 200;

// One "#<n> …" line from CrashHandler (format in CrashHandler.h).
struct RawFrame {
    int index = 0;
    QString module;           // as written; empty when the line had "?"
    quint64 offset = 0;
    QString symbol;           // empty when the line had "?"
};

QRegularExpression anchored(const char* pattern,
                            QRegularExpression::PatternOptions options = {}) {
    return QRegularExpression(QStringLiteral("^%1$").arg(QLatin1String(pattern)), options);
}

// Modules whose name may be sent; first entry is GitBolt itself. File
// names only (a module token is cut at its last '/' or '\' first).
// Windows file names are case-insensitive. PRIVACY.md ("Crash
// reports") names every group below: change it with this list.
struct ModuleRule {
    QRegularExpression pattern;
    bool inApp;
};

const QList<ModuleRule>& moduleRules() {
    constexpr auto ci = QRegularExpression::CaseInsensitiveOption;
    static const QList<ModuleRule> rules = {
        {anchored("GitBolt|gitbolt"), true},
        {anchored("GitBolt\\.exe", ci), true},
        // Qt: macOS framework binaries, Linux and Windows libraries,
        // and the platform/style plugins a crash in painting runs through.
        {anchored("Qt[A-Z][A-Za-z0-9]*"), false},
        {anchored("libQt6[A-Za-z0-9]+\\.so(\\.[0-9]+)*"), false},
        {anchored("Qt6[A-Za-z0-9]+d?\\.dll", ci), false},
        {anchored("libq(cocoa|macstyle|offscreen|minimal|xcb|wayland-generic|wayland-egl)"
                  "\\.(dylib|so)"), false},
        {anchored("q(windows|modernwindowsstyle|windowsvistastyle)d?\\.dll", ci), false},
        // libgit2
        {anchored("libgit2(\\.[0-9]+)*\\.dylib"), false},
        {anchored("libgit2\\.so(\\.[0-9]+)*"), false},
        {anchored("git2\\.dll", ci), false},
        // macOS
        {anchored("libsystem_[a-z_]+\\.dylib"), false},
        {anchored("libc\\+\\+(abi)?(\\.[0-9]+)*\\.dylib"), false},
        {anchored("libdyld\\.dylib|dyld"), false},
        {anchored("libobjc(\\.A)?\\.dylib"), false},
        {anchored("CoreFoundation|AppKit|Foundation|HIToolbox"), false},
        // Linux
        {anchored("libc\\.so(\\.[0-9]+)*"), false},
        {anchored("libstdc\\+\\+\\.so(\\.[0-9]+)*"), false},
        {anchored("libgcc_s\\.so(\\.[0-9]+)*"), false},
        {anchored("libpthread\\.so(\\.[0-9]+)*"), false},
        {anchored("ld-linux[-a-z0-9_]*\\.so(\\.[0-9]+)*"), false},
        // Windows
        {anchored("ntdll\\.dll|KERNELBASE\\.dll|kernel32\\.dll|ucrtbase\\.dll", ci), false},
        {anchored("msvcp140[_a-z0-9]*\\.dll|vcruntime140[_a-z0-9]*\\.dll", ci), false},
    };
    return rules;
}

const ModuleRule* matchModule(const QString& name) {
    for (const ModuleRule& rule : moduleRules()) {
        if (rule.pattern.match(name).hasMatch())
            return &rule;
    }
    return nullptr;
}

QString fileName(QStringView module) {
    const qsizetype cut = std::max(module.lastIndexOf(QLatin1Char('/')),
                                   module.lastIndexOf(QLatin1Char('\\')));
    return module.mid(cut + 1).toString();
}

// The demangled name; a "_Z" name that doesn't demangle is dropped
// (empty) rather than sent raw. So is every "_Z" name where there is
// no demangler (MSVC): Windows reports carry undecorated names anyway.
QString demangle(const QString& symbol) {
    // dladdr hands back the linker's name; on macOS that can keep one
    // extra leading underscore, so try with it stripped too.
    bool mangled = false;
    for (const QString& candidate : {symbol, symbol.mid(1)}) {
        if (!candidate.startsWith(QLatin1String("_Z")))
            continue;
        mangled = true;
#ifdef GITBOLT_HAVE_CXA_DEMANGLE
        int status = -1;
        const std::unique_ptr<char, decltype(&std::free)> demangled(
            abi::__cxa_demangle(candidate.toLatin1().constData(), nullptr, nullptr, &status),
            &std::free);
        if (status == 0 && demangled)
            return QString::fromLatin1(demangled.get());
#endif
    }
    if (mangled)
        return {};
    return symbol;
}

// Cuts a demangled name at its parameter list: the first '(' outside
// template arguments, other than "(anonymous namespace)" and an
// operator's own symbols ("operator()", "operator<<").
QString dropParameters(const QString& name) {
    static const QString anonymous = QStringLiteral("(anonymous namespace)");
    static const QString op = QStringLiteral("operator");
    int depth = 0;
    for (qsizetype i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        if (QStringView(name).mid(i).startsWith(op)
            && (i == 0 || !(name.at(i - 1).isLetterOrNumber() || name.at(i - 1) == u'_'))) {
            qsizetype j = i + op.size();
            if (QStringView(name).mid(j).startsWith(QLatin1String("()"))) {
                j += 2;
            } else {
                while (j < name.size() && !name.at(j).isLetterOrNumber()
                       && name.at(j) != u'_' && name.at(j) != u'(' && name.at(j) != u' ')
                    ++j;
            }
            i = j - 1;
            continue;
        }
        if (c == u'<') {
            ++depth;
        } else if (c == u'>') {
            if (depth > 0)
                --depth;
        } else if (c == u'(' && depth == 0) {
            if (QStringView(name).mid(i).startsWith(anonymous)) {
                i += anonymous.size() - 1;
                continue;
            }
            return name.left(i);
        }
    }
    return name;
}

// The function name a non-GitBolt frame may carry, or empty.
QString reduceFunction(const QString& symbol) {
    static const QRegularExpression cxx(
        QStringLiteral("^[A-Za-z_~(][A-Za-z0-9_:<>~,*&\\[\\]. ()-]*$"));
    static const QRegularExpression objc(
        QStringLiteral("^[-+]\\[[A-Za-z_][A-Za-z0-9_]*(\\([A-Za-z0-9_]+\\))? "
                       "[A-Za-z_][A-Za-z0-9_:]*\\]$"));
    if (symbol.isEmpty())
        return {};
    const QString name = dropParameters(demangle(symbol)).trimmed();
    if (name.isEmpty() || name.size() > kMaxFunctionLength)
        return {};
    if (cxx.match(name).hasMatch() || objc.match(name).hasMatch())
        return name;
    return {};
}

std::optional<RawFrame> parseFrameLine(QStringView line) {
    static const QRegularExpression resolved(
        QStringLiteral("^#(\\d{1,3}) (\\S+) \\+0x([0-9a-fA-F]{1,16}) (.*)$"));
    static const QRegularExpression unresolved(
        QStringLiteral("^#(\\d{1,3}) \\? 0x[0-9a-fA-F]{1,16}$"));
    static const QRegularExpression symbolPart(
        QStringLiteral("^(.+)\\+0x[0-9a-fA-F]{1,16}$"));

    RawFrame frame;
    if (const auto m = resolved.matchView(line); m.hasMatch()) {
        frame.index = m.capturedView(1).toInt();
        frame.module = m.captured(2);
        frame.offset = m.capturedView(3).toULongLong(nullptr, 16);
        if (const auto s = symbolPart.matchView(m.capturedView(4)); s.hasMatch())
            frame.symbol = s.captured(1);
        return frame;
    }
    if (const auto m = unresolved.matchView(line); m.hasMatch()) {
        frame.index = m.capturedView(1).toInt();
        return frame;
    }
    return std::nullopt;
}

bool isWindowsDispatchModule(const QString& module) {
    static const QRegularExpression dispatch(
        QStringLiteral("^(ntdll|KERNELBASE|kernel32|ucrtbase|vcruntime140[_a-z0-9]*)\\.dll$"),
        QRegularExpression::CaseInsensitiveOption);
    return dispatch.match(fileName(module)).hasMatch();
}

// Where a Windows exception entered the stack. A fault enters user
// mode at ntdll's KiUserExceptionDispatcher; a throw raises it in
// place: _CxxThrowException calls RaiseException, which calls
// RtlRaiseException. Symbols are the export names dbghelp finds
// without PDBs.
bool isWindowsRaisePoint(const RawFrame& f) {
    static const QRegularExpression vcruntime(QStringLiteral("^vcruntime140[_a-z0-9]*\\.dll$"),
                                              QRegularExpression::CaseInsensitiveOption);
    const QString module = fileName(f.module);
    const auto is = [&module](const char* name) {
        return module.compare(QLatin1String(name), Qt::CaseInsensitive) == 0;
    };
    if (is("ntdll.dll"))
        return f.symbol == QLatin1String("KiUserExceptionDispatcher")
               || f.symbol == QLatin1String("RtlRaiseException");
    if (is("KERNELBASE.dll") || is("kernel32.dll"))
        return f.symbol == QLatin1String("RaiseException");
    return vcruntime.match(module).hasMatch() && f.symbol == QLatin1String("_CxxThrowException");
}

// Drops the frames the handler itself added, by position: names can
// be wrong, positions can't. POSIX: #0 is the handler (backtrace()
// runs in it) and #1 the kernel's trampoline (_sigtramp, __restore_rt).
// Windows: #0 is signalHandler (CaptureStackBackTrace skips the
// writer), then everything up to where the exception was raised: for
// a fault, the CRT and loader frames that dispatched it up to
// KiUserExceptionDispatcher; for an unhandled C++ exception, abort()
// from terminate(), the CRT's unhandled-exception filter (inside
// GitBolt.exe, so it would pass for the crash point), the dispatch and
// the throw's own frames. For raise()/abort(), the CRT frames on top
// of the caller.
QList<RawFrame> dropHandlerFrames(QList<RawFrame> frames, bool windows) {
    if (!windows) {
        frames.removeIf([](const RawFrame& f) { return f.index < 2; });
        return frames;
    }
    frames.removeIf([](const RawFrame& f) { return f.index < 1; });
    auto raised = std::find_if(frames.cbegin(), frames.cend(), isWindowsRaisePoint);
    if (raised != frames.cend()) {
        // The newest raise frame, then its callers that raise too.
        while (raised + 1 != frames.cend() && isWindowsRaisePoint(*(raised + 1)))
            ++raised;
        frames.erase(frames.cbegin(), raised + 1);
        return frames;
    }
    while (!frames.isEmpty() && isWindowsDispatchModule(frames.first().module))
        frames.removeFirst();
    return frames;
}

CrashFrame reduceFrame(const RawFrame& raw) {
    CrashFrame frame;
    const QString name = fileName(raw.module);
    const ModuleRule* rule = name.isEmpty() ? nullptr : matchModule(name);
    if (!rule) {
        frame.package = QStringLiteral("<other>");
        return frame;
    }
    frame.package = name;
    frame.offset = raw.offset;
    frame.inApp = rule->inApp;
    frame.function = rule->inApp ? QStringLiteral("<unknown>") : reduceFunction(raw.symbol);
    return frame;
}

QString osName(QStringView productType) {
    if (productType == QLatin1String("macos") || productType == QLatin1String("osx"))
        return QStringLiteral("macOS");
    if (productType == QLatin1String("windows"))
        return QStringLiteral("Windows");
    return QStringLiteral("Linux");
}

} // namespace

std::optional<CrashEvent> CrashLogParser::parse(QStringView log) {
    static const QRegularExpression release(
        QStringLiteral("^\\d+\\.\\d+\\.\\d+([-+][0-9A-Za-z.-]+)?$"));
    static const QRegularExpression signalLine(
        QStringLiteral("^(SIGSEGV|SIGABRT|SIGBUS|SIGFPE|UNKNOWN) \\((\\d{1,3})\\)$"));
    static const QRegularExpression epoch(QStringLiteral("^\\d{1,12}$"));
    static const QRegularExpression qtLine(QStringLiteral("^(\\d+\\.\\d+\\.\\d+) \\(runtime\\)"));
    static const QRegularExpression osLine(
        QStringLiteral("^([a-z0-9_.-]{1,32}) ([0-9A-Za-z._-]{1,32})$"));
    static const QRegularExpression arch(QStringLiteral("^[a-z0-9_]{1,16}$"));

    const QList<QStringView> lines = log.split(QLatin1Char('\n'));
    if (lines.isEmpty() || lines.first().trimmed() != QLatin1String("=== GitBolt Crash Report ==="))
        return std::nullopt;

    // Header: "Key: value" lines up to the stack marker. A key seen
    // twice means a garbled report; refuse rather than pick one.
    QHash<QString, QString> header;
    qsizetype i = 1;
    for (; i < lines.size(); ++i) {
        const QStringView line = lines.at(i).trimmed();
        if (line == QLatin1String("--- Stack Trace ---"))
            break;
        const qsizetype colon = line.indexOf(QLatin1String(": "));
        if (colon <= 0)
            continue;
        const QString key = line.left(colon).toString();
        if (header.contains(key))
            return std::nullopt;
        header.insert(key, line.mid(colon + 2).toString());
    }
    if (i == lines.size())
        return std::nullopt;

    if (header.value(QStringLiteral("Format")) != QLatin1String("2")
        || header.value(QStringLiteral("Build")) != QLatin1String("official")
        || header.value(QStringLiteral("Opt-out")) != QLatin1String("none")
        || header.value(QStringLiteral("Session")) != QLatin1String("user"))
        return std::nullopt;

    CrashEvent event;
    event.release = header.value(QStringLiteral("App version"));
    if (!release.match(event.release).hasMatch())
        return std::nullopt;

    const auto sig = signalLine.match(header.value(QStringLiteral("Signal")));
    if (!sig.hasMatch())
        return std::nullopt;
    event.signal = sig.captured(1);
    event.signalNumber = sig.capturedView(2).toInt();

    const QString seconds = header.value(QStringLiteral("Epoch"));
    if (!epoch.match(seconds).hasMatch())
        return std::nullopt;
    event.at = QDateTime::fromSecsSinceEpoch(seconds.toLongLong(), QTimeZone::UTC);

    if (const auto qt = qtLine.match(header.value(QStringLiteral("Qt version"))); qt.hasMatch())
        event.qtVersion = qt.captured(1);
    const auto os = osLine.match(header.value(QStringLiteral("OS")));
    const QString productType = os.hasMatch() ? os.captured(1) : QString();
    event.os = osName(productType);
    if (os.hasMatch())
        event.osVersion = event.os == QLatin1String("Linux")
                              ? productType + QLatin1Char(' ') + os.captured(2)
                              : os.captured(2);
    if (const QString a = header.value(QStringLiteral("CPU Arch")); arch.match(a).hasMatch())
        event.arch = a;

    // Frames, newest first as written, up to the end marker.
    QList<RawFrame> raw;
    for (++i; i < lines.size(); ++i) {
        const QStringView line = lines.at(i).trimmed();
        if (line == QLatin1String("=== End of Report ==="))
            break;
        if (auto frame = parseFrameLine(line))
            raw.append(*frame);
    }
    raw = dropHandlerFrames(std::move(raw), event.os == QLatin1String("Windows"));

    // Fingerprint from the crash point down: the first three GitBolt
    // frames' offsets.
    QByteArray offsets;
    int inAppSeen = 0;
    for (const RawFrame& f : std::as_const(raw)) {
        const QString name = fileName(f.module);
        const ModuleRule* rule = name.isEmpty() ? nullptr : matchModule(name);
        if (!rule || !rule->inApp)
            continue;
        offsets += QByteArray::number(f.offset, 16) + ',';
        if (++inAppSeen == 3)
            break;
    }
    const QString location =
        inAppSeen == 0
            ? QStringLiteral("none")
            : QString::fromLatin1(
                  QCryptographicHash::hash(offsets, QCryptographicHash::Sha1).toHex().left(16));
    event.fingerprint = {QStringLiteral("crash"), event.signal,
                         QStringLiteral("gitbolt@%1:%2").arg(event.release, location)};

    // Sentry wants the oldest frame first; keep the 64 nearest the crash.
    for (auto it = raw.crbegin(); it != raw.crend(); ++it)
        event.frames.append(reduceFrame(*it));
    if (event.frames.size() > kMaxFrames)
        event.frames.remove(0, event.frames.size() - kMaxFrames);
    return event;
}

} // namespace gitbolt::telemetry
