#pragma once

#include "util/TestSession.h"

#include <QString>
#include <QStringList>

namespace gitbolt::telemetry {

/// Whether this session sends usage statistics, and if not, why. The
/// one place that decides; main() evaluates it once, right after the
/// SettingsService exists, and nothing else reads these conditions.
///
/// In precedence order, first match wins:
///   - CompiledOut: no endpoints in this build (source builds, forks,
///     PR and dry-run CI builds). No service, no Settings group.
///   - TestBridge, TestInstance, Headless, Ci: a test session, by
///     util::detectTestSession — the same predicate behind the crash
///     report's "Session: test". Inert: no service, no install ID,
///     and the queue is never opened, created or deleted, so a test
///     run can't touch a real user's queue even outside XDG isolation.
///   - DoNotTrack, EnvOff: the user opted out through the environment
///     (util::detectEnvOptOut). The queue is discarded; no service.
///   - SettingOff: the Settings switch is off. The queue is discarded,
///     and a disabled service is created so the switch can be turned
///     back on without a restart.
///   - Active.
enum class Gate : int {
    CompiledOut,
    TestBridge,
    TestInstance,
    Headless,
    Ci,
    DoNotTrack,
    EnvOff,
    SettingOff,
    Active,
};

struct GateInputs {
    /// Config::isComplete() for the compiled-in endpoints.
    bool configured = false;
    /// QGuiApplication::platformName().
    QString platformName;
    /// argv, program first. QApplication has stripped -platform by the
    /// time main() evaluates the gate (platformName covers it then).
    QStringList arguments;
    /// SettingsService::usageStatisticsEnabled().
    bool settingOn = true;
    /// The environment; injected so the table test never reads the
    /// process's, which ctest and CI set some of these variables in.
    util::EnvLookup env = util::processEnvironment;
};

Gate evaluateGate(const GateInputs& inputs);

/// "compiled-out", "inert:test-bridge", "inert:instance-name",
/// "inert:headless", "inert:ci", "off:do-not-track", "off:env",
/// "off:setting" or "on". For the test bridge's dump-state.
QString gateName(Gate gate);

/// Active and SettingOff: main() creates a TelemetryService.
bool createsService(Gate gate);

/// The three off gates: main() discards the queue at startup.
bool discardsQueue(Gate gate);

} // namespace gitbolt::telemetry
