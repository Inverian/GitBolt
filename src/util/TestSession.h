#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>

#include <functional>
#include <optional>

namespace gitbolt::util {

/// Reads one environment variable: std::nullopt when it is unset, its
/// value (possibly empty) when it is set. Unset and empty differ for
/// GITBOLT_TEST_BRIDGE: main.cpp starts the bridge whenever the
/// variable is SET, and an empty value picks the default socket.
/// Injected so the telemetry gate's table test never reads the real
/// environment, which ctest and CI set some of these in.
using EnvLookup = std::function<std::optional<QString>(const char* name)>;

/// The process environment as an EnvLookup (qEnvironmentVariableIsSet,
/// then qEnvironmentVariable).
std::optional<QString> processEnvironment(const char* name);

/// Why a process counts as a test session, strongest reason first.
///
/// The ONE predicate behind both the crash handler's "Session: test"
/// stamp and the telemetry gate's inert states. They must agree: a
/// session the gate keeps quiet that still stamped "Session: user"
/// would have its crash uploaded by the user's next launch (e2e and
/// bridge runs share the real ~/.gitbolt/crash.log).
///
///   - Bridge: GITBOLT_TEST_BRIDGE is set, even to nothing — exactly
///     main.cpp's condition for creating the TestBridge.
///   - Instance: GITBOLT_INSTANCE_NAME is non-empty. The e2e harness
///     and every CI smoke launch set it, bridge or not.
///   - Headless: the platform is offscreen or minimal, from the
///     resolved platform name, QT_QPA_PLATFORM, or a -platform
///     argument (the crash handler runs before QApplication, so it can
///     only see the last two).
///   - Ci: CI or GITHUB_ACTIONS is set to anything but empty, "0" or
///     "false". Catches CI's xvfb and cocoa smoke launches, which are
///     not headless by platform name.
enum class TestSessionKind { None, Bridge, Instance, Headless, Ci };

/// `platformName`: QGuiApplication::platformName(), or empty when
/// there is no application yet. `arguments`: argv, program name first.
TestSessionKind detectTestSession(const QString& platformName,
                                  const QStringList& arguments,
                                  const EnvLookup& env = processEnvironment);

/// The same, before QApplication exists: no resolved platform name, so
/// argv is scanned for -platform (QApplication strips it later).
TestSessionKind detectTestSession(int argc, const char* const* argv,
                                  const EnvLookup& env = processEnvironment);

inline bool isTestSession(TestSessionKind kind) {
    return kind != TestSessionKind::None;
}

/// True for an offscreen or minimal platform spec, as QT_QPA_PLATFORM
/// or -platform take it: case-insensitive, ":options" ignored, and any
/// entry of a ";" fallback list counts (erring towards "test").
bool isHeadlessPlatform(QStringView spec);

/// Why the user turned telemetry off through the environment, if they
/// did. Shared for the same reason as detectTestSession: the crash
/// header's "Opt-out: env" and the gate's off states can't drift.
///
///   - GitBoltTelemetry: GITBOLT_TELEMETRY is 0, false, off or no,
///     case-insensitive.
///
/// DO_NOT_TRACK isn't one: GitBolt's own switch (Settings) and
/// GITBOLT_TELEMETRY are the ways to turn it off.
enum class EnvOptOut { None, GitBoltTelemetry };

EnvOptOut detectEnvOptOut(const EnvLookup& env = processEnvironment);

inline bool isEnvOptOut(EnvOptOut reason) {
    return reason != EnvOptOut::None;
}

} // namespace gitbolt::util
