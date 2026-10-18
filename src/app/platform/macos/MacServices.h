#pragma once

#include <QString>
#include <QStringList>

#include <functional>

// Finder's "Open in GitBolt": a macOS service (NSServices in
// Info.plist.in) that GitBolt provides itself, with no extension
// bundle, sandbox or entitlements. Finder sends the selected folders
// on a pasteboard to the object set as NSApp.servicesProvider, a
// running GitBolt or one that macOS launches for it.

namespace gitbolt::app::macos {

using FolderHandler = std::function<void(const QStringList& folders)>;

/// Makes GitBolt the provider of its "Open in GitBolt" service, with
/// `onFolders` called with the folders of each request (never none),
/// from inside the service call: it should only queue them. An empty
/// handler takes the provider away again. Does nothing off the cocoa
/// platform (offscreen, minimal), where there is no NSApp.
void installServicesProvider(FolderHandler onFolders);

/// Brings GitBolt in front of the app that asked it to open something.
void activateApp();

/// Sends `folders` to the installed provider as Finder would, on a
/// pasteboard of its own: the test bridge's service-open. False, with
/// `error` set, if there is no provider or it refused them.
bool sendToServicesProvider(const QStringList& folders, QString& error);

} // namespace gitbolt::app::macos

#ifdef __OBJC__
#import <AppKit/AppKit.h>

/// The provider. NSMessage "openInGitBolt" in Info.plist.in names its
/// method, -openInGitBolt:userData:error:. Declared here for the tests,
/// which call it with a pasteboard of their own.
@interface GitBoltServicesProvider : NSObject
- (instancetype)initWithHandler:(gitbolt::app::macos::FolderHandler)handler;
- (void)openInGitBolt:(NSPasteboard *)pboard
             userData:(NSString *)userData
                error:(NSString **)error;
@end

namespace gitbolt::app::macos {
/// The local folders a service pasteboard names, in order. Files and
/// anything else are left out.
QStringList foldersFromPasteboard(NSPasteboard* pboard);
} // namespace gitbolt::app::macos
#endif
