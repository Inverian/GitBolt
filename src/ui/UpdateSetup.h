#pragma once

#include "update/Package.h"
#include "update/UpdateConfig.h"
#include "util/TestSession.h"

#include <QProcessEnvironment>
#include <QString>
#include <QUrl>

namespace gitbolt::ui {

/// Whether this session offers in-app updates, and if not, why. main()
/// evaluates it once (setUpUpdates); nothing else reads these
/// conditions. In precedence order:
///   - TestSession: a test session (util::detectTestSession: the
///     bridge, an instance name, a headless platform, CI) without
///     GITBOLT_TEST_UPDATE_FEED. Off, official build or not: a test
///     session never asks the official feed, so no ctest, e2e, CI smoke
///     or dogfood run reaches api.github.com.
///   - TestFeedRefused: a test session whose GITBOLT_TEST_UPDATE_FEED
///     isn't plain http to 127.0.0.1 or [::1], or whose
///     GITBOLT_TEST_APP_VERSION isn't a version. Off, with a warning.
///   - TestFeed: a test session with a loopback test feed. On, against
///     it, with loopback downloads allowed and GITBOLT_TEST_APP_VERSION
///     as the running version; nothing is scheduled (the e2e tests fire
///     updateCheckTimer through the bridge).
///   - CompiledOut: not an official build (GITBOLT_UPDATES off, which
///     source, fork, pull request and distribution builds are). Help ›
///     Check for Updates opens the releases page, as it always did, and
///     Settings has no Updates page.
///   - Active: an official build outside a test session.
/// The test variables are read in a test session only.
enum class UpdateGate { CompiledOut, TestSession, TestFeedRefused, TestFeed, Active };

/// "compiled-out", "off:test-session", "off:test-feed-refused",
/// "test-feed" or "on": for the test bridge's dump-state.
QString updateGateName(UpdateGate gate);

struct UpdateSetupInputs {
    /// update::kOfficialFeed in an official build (GITBOLT_UPDATES_ENABLED,
    /// which only main() sees), empty otherwise: no library names it.
    QUrl officialFeed;
    util::TestSessionKind testSession = util::TestSessionKind::None;
    /// GITBOLT_TEST_UPDATE_FEED, GITBOLT_TEST_APP_VERSION and
    /// GITBOLT_UPDATE_CHECK are read from here; injected, so the table
    /// test never reads the process's.
    QProcessEnvironment env;
    /// QCoreApplication::applicationVersion().
    QString appVersion;
    /// QStandardPaths::CacheLocation; downloads go in its "updates".
    QString cacheDir;
    /// QDir::tempPath(): a test session's downloads go in its
    /// "gitbolt-updates" instead, never in the user's cache. macOS's
    /// CacheLocation ignores HOME, so a test's private HOME wouldn't
    /// keep them out of ~/Library/Caches; the e2e harness gives each app
    /// a TMPDIR of its own, and removes it afterwards.
    QString tempDir;
};

/// What the updater runs with this session: main() builds it and hands
/// it to MainWindow::setUpdates.
struct UpdateSetup {
    UpdateGate gate = UpdateGate::CompiledOut;
    /// Usable (Config::isUsable) whenever on().
    update::Config config;
    /// How this copy was installed: main() fills it in when on(), from
    /// update::detectPackage(update::currentProbe()).
    update::PackageInfo package;
    /// GITBOLT_UPDATE_CHECK turned the automatic check off: Settings
    /// shows its switch disabled, with the reason. A manual check still
    /// asks.
    bool envOptOut = false;
    /// Why a test variable was refused or ignored, for main()'s log;
    /// empty when there is nothing to say.
    QString problem;

    /// Help › Check for Updates asks the feed, and Settings › Updates
    /// is shown.
    bool on() const { return gate == UpdateGate::TestFeed || gate == UpdateGate::Active; }
    /// The daily check is scheduled: an official build outside a test
    /// session, not opted out through the environment. (The Settings
    /// switch is read at each tick, so it needs no restart.)
    bool schedulesChecks() const { return gate == UpdateGate::Active && !envOptOut; }
};

/// The gate and the Config, by the rules above. The User-Agent is
/// telemetry's (telemetry::userAgent), with the running version.
UpdateSetup setUpUpdates(const UpdateSetupInputs& inputs);

} // namespace gitbolt::ui
