#include "util/TestSession.h"

#include <QtGlobal>

namespace gitbolt::util {

namespace {

// Empty counts as unset, and so do the usual spellings of "no".
bool isSetAndNotFalse(const std::optional<QString>& value) {
    if (!value)
        return false;
    const QString v = value->trimmed();
    return !v.isEmpty() && v != QLatin1String("0")
           && v.compare(QLatin1String("false"), Qt::CaseInsensitive) != 0;
}

bool isHeadlessArgument(const QStringList& arguments) {
    // Qt takes "-platform <spec>" and "--platform <spec>" alike
    // (QGuiApplicationPrivate::init); index 0 is the program.
    for (qsizetype i = 1; i + 1 < arguments.size(); ++i) {
        QStringView arg = arguments.at(i);
        if (arg.startsWith(QLatin1String("--")))
            arg = arg.mid(1);
        if (arg == QLatin1String("-platform")
            && isHeadlessPlatform(arguments.at(i + 1)))
            return true;
    }
    return false;
}

} // namespace

std::optional<QString> processEnvironment(const char* name) {
    if (!qEnvironmentVariableIsSet(name))
        return std::nullopt;
    return qEnvironmentVariable(name);
}

bool isHeadlessPlatform(QStringView spec) {
    for (QStringView entry : spec.split(QLatin1Char(';'))) {
        const QStringView name = entry.left(entry.indexOf(QLatin1Char(':'))).trimmed();
        if (name.compare(QLatin1String("offscreen"), Qt::CaseInsensitive) == 0
            || name.compare(QLatin1String("minimal"), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

TestSessionKind detectTestSession(const QString& platformName,
                                  const QStringList& arguments,
                                  const EnvLookup& env) {
    if (env("GITBOLT_TEST_BRIDGE"))
        return TestSessionKind::Bridge;
    if (!env("GITBOLT_INSTANCE_NAME").value_or(QString()).isEmpty())
        return TestSessionKind::Instance;
    if (isHeadlessPlatform(platformName)
        || isHeadlessPlatform(env("QT_QPA_PLATFORM").value_or(QString()))
        || isHeadlessArgument(arguments))
        return TestSessionKind::Headless;
    if (isSetAndNotFalse(env("CI")) || isSetAndNotFalse(env("GITHUB_ACTIONS")))
        return TestSessionKind::Ci;
    return TestSessionKind::None;
}

TestSessionKind detectTestSession(int argc, const char* const* argv,
                                  const EnvLookup& env) {
    QStringList arguments;
    for (int i = 0; i < argc; ++i)
        arguments << QString::fromLocal8Bit(argv[i]);
    return detectTestSession(QString(), arguments, env);
}

EnvOptOut detectEnvOptOut(const EnvLookup& env) {
    const QString telemetry = env("GITBOLT_TELEMETRY").value_or(QString()).trimmed();
    for (const char* off : {"0", "false", "off", "no"}) {
        if (telemetry.compare(QLatin1String(off), Qt::CaseInsensitive) == 0)
            return EnvOptOut::GitBoltTelemetry;
    }
    return EnvOptOut::None;
}

} // namespace gitbolt::util
