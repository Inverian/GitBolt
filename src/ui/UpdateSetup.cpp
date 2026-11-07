#include "ui/UpdateSetup.h"

#include "telemetry/EventSchema.h"

#include <QDir>

namespace gitbolt::ui {

namespace {

constexpr char kTestFeedEnv[] = "GITBOLT_TEST_UPDATE_FEED";
constexpr char kTestVersionEnv[] = "GITBOLT_TEST_APP_VERSION";

update::Config configFor(const QUrl& feed, const update::Version& current, const QString& cacheDir) {
    update::Config config;
    config.feedUrl = feed;
    config.repo = QString::fromLatin1(update::kOfficialRepo);
    config.current = current;
    // The same User-Agent as usage statistics', so GitHub sees one
    // client: "GitBolt/<version> (<OS>)".
    config.userAgent = telemetry::userAgent({current.toString(), telemetry::Environment::current()});
    if (!cacheDir.isEmpty())
        config.downloadDir = QDir(QDir(cacheDir).absolutePath()).filePath(QStringLiteral("updates"));
    return config;
}

} // namespace

QString updateGateName(UpdateGate gate) {
    switch (gate) {
    case UpdateGate::CompiledOut: return QStringLiteral("compiled-out");
    case UpdateGate::TestSession: return QStringLiteral("off:test-session");
    case UpdateGate::TestFeedRefused: return QStringLiteral("off:test-feed-refused");
    case UpdateGate::TestFeed: return QStringLiteral("test-feed");
    case UpdateGate::Active: return QStringLiteral("on");
    }
    return {};
}

UpdateSetup setUpUpdates(const UpdateSetupInputs& inputs) {
    UpdateSetup setup;
    setup.envOptOut = update::updateCheckOptedOut(inputs.env);
    const bool testFeedSet = inputs.env.contains(QString::fromLatin1(kTestFeedEnv));
    const bool testVersionSet = inputs.env.contains(QString::fromLatin1(kTestVersionEnv));

    if (util::isTestSession(inputs.testSession)) {
        if (!testFeedSet) {
            setup.gate = UpdateGate::TestSession;
            return setup;
        }
        setup.gate = UpdateGate::TestFeedRefused;
        const QString feedText = inputs.env.value(QString::fromLatin1(kTestFeedEnv)).trimmed();
        const QUrl feed(feedText, QUrl::StrictMode);
        if (!update::isLoopbackHttpUrl(feed)) {
            setup.problem = QStringLiteral("%1 must be http://127.0.0.1:<port>/… or http://[::1]:<port>/…, "
                                           "not '%2'; in-app updates are off")
                                .arg(QLatin1String(kTestFeedEnv), feedText);
            return setup;
        }
        const QString versionText = testVersionSet ? inputs.env.value(QString::fromLatin1(kTestVersionEnv))
                                                   : inputs.appVersion;
        const std::optional<update::Version> current = update::Version::parse(versionText.trimmed());
        if (!current || *current == update::Version{}) {
            setup.problem = QStringLiteral("%1 must be a version such as 0.1.0, not '%2'; in-app updates are off")
                                .arg(testVersionSet ? QLatin1String(kTestVersionEnv)
                                                    : QLatin1String("the app version"),
                                     versionText);
            return setup;
        }
        setup.config = configFor(feed, *current, {});
        if (!inputs.tempDir.isEmpty())
            setup.config.downloadDir =
                QDir(QDir(inputs.tempDir).absolutePath()).filePath(QStringLiteral("gitbolt-updates"));
        setup.config.allowLoopback = true;
        if (!setup.config.isUsable()) {
            setup.problem = QStringLiteral("the update settings aren't usable (no temporary folder?); "
                                           "in-app updates are off");
            return setup;
        }
        setup.gate = UpdateGate::TestFeed;
        return setup;
    }

    if (testFeedSet || testVersionSet)
        setup.problem = QStringLiteral("%1 and %2 are read in test sessions only; ignored")
                            .arg(QLatin1String(kTestFeedEnv), QLatin1String(kTestVersionEnv));
    if (inputs.officialFeed.isEmpty())
        return setup;   // CompiledOut
    const std::optional<update::Version> current = update::Version::parse(inputs.appVersion);
    if (current)
        setup.config = configFor(inputs.officialFeed, *current, inputs.cacheDir);
    if (!current || !setup.config.isUsable()) {
        setup.problem = QStringLiteral("the update settings aren't usable (version '%1', no cache folder?); "
                                       "in-app updates are off")
                            .arg(inputs.appVersion);
        return setup;
    }
    setup.gate = UpdateGate::Active;
    return setup;
}

} // namespace gitbolt::ui
