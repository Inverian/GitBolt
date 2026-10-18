#pragma once
//
// MacServicesTestSupport — the Objective-C++ half of TestMacServices:
// it builds the pasteboards Finder sends and calls the "Open in
// GitBolt" provider (src/app/platform/macos/MacServices.h) with them,
// so the test itself stays plain C++ for AUTOMOC.
//

#include <QString>
#include <QStringList>

namespace gitbolt::test {

/// What a call of the provider did.
struct ServiceCall {
    bool handlerCalled = false;
    QStringList folders;   // what the handler was given
    QString error;         // what the provider answered Finder with
};

/// A pasteboard of the test's own can be made and written; false where
/// there is no pasteboard server to talk to.
bool pasteboardAvailable();

/// `paths` as file URLs, as Finder writes them, then `texts` as plain
/// strings, on a pasteboard of its own, sent to a new provider.
ServiceCall callProvider(const QStringList& paths, const QStringList& texts = {});

/// The same with the old NSFilenamesPboardType list some senders use.
ServiceCall callProviderWithFilenames(const QStringList& paths);

/// The provider has the method an NSServices entry's NSMessage names
/// (`<message>:userData:error:`).
bool providerAnswers(const QString& nsMessage);

} // namespace gitbolt::test
