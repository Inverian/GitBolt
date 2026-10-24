#include "telemetry/TelemetryGate.h"

namespace gitbolt::telemetry {

Gate evaluateGate(const GateInputs& inputs) {
    if (!inputs.configured)
        return Gate::CompiledOut;

    switch (util::detectTestSession(inputs.platformName, inputs.arguments, inputs.env)) {
    case util::TestSessionKind::Bridge:   return Gate::TestBridge;
    case util::TestSessionKind::Instance: return Gate::TestInstance;
    case util::TestSessionKind::Headless: return Gate::Headless;
    case util::TestSessionKind::Ci:       return Gate::Ci;
    case util::TestSessionKind::None:     break;
    }

    switch (util::detectEnvOptOut(inputs.env)) {
    case util::EnvOptOut::GitBoltTelemetry: return Gate::EnvOff;
    case util::EnvOptOut::None:             break;
    }

    return inputs.settingOn ? Gate::Active : Gate::SettingOff;
}

QString gateName(Gate gate) {
    switch (gate) {
    case Gate::CompiledOut:  return QStringLiteral("compiled-out");
    case Gate::TestBridge:   return QStringLiteral("inert:test-bridge");
    case Gate::TestInstance: return QStringLiteral("inert:instance-name");
    case Gate::Headless:     return QStringLiteral("inert:headless");
    case Gate::Ci:           return QStringLiteral("inert:ci");
    case Gate::EnvOff:       return QStringLiteral("off:env");
    case Gate::SettingOff:   return QStringLiteral("off:setting");
    case Gate::Active:       return QStringLiteral("on");
    }
    return QStringLiteral("compiled-out");
}

bool createsService(Gate gate) {
    return gate == Gate::Active || gate == Gate::SettingOff;
}

bool discardsQueue(Gate gate) {
    return gate == Gate::EnvOff || gate == Gate::SettingOff;
}

} // namespace gitbolt::telemetry
