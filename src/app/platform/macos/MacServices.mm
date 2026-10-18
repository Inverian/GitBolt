#import "app/platform/macos/MacServices.h"

#include <QGuiApplication>
#include <QTimer>

#include <servers/bootstrap.h>

#include <utility>

namespace gitbolt::app::macos {

QStringList foldersFromPasteboard(NSPasteboard* pboard)
{
    QStringList folders;
    // File URLs, whichever way the sender wrote them: Finder's
    // public.file-url, or the old NSFilenamesPboardType list.
    NSArray<NSURL*>* urls =
        [pboard readObjectsForClasses:@[ NSURL.class ]
                              options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
    for (NSURL* url in urls) {
        NSNumber* isDir = nil;
        [url getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
        if (isDir.boolValue)
            folders << QString::fromNSString(url.URLByStandardizingPath.path);
    }
    return folders;
}

} // namespace gitbolt::app::macos

@implementation GitBoltServicesProvider {
    gitbolt::app::macos::FolderHandler _handler;
}

- (instancetype)initWithHandler:(gitbolt::app::macos::FolderHandler)handler
{
    if ((self = [super init]))
        _handler = std::move(handler);
    return self;
}

- (void)openInGitBolt:(NSPasteboard *)pboard
             userData:(NSString *)userData
                error:(NSString **)error
{
    (void)userData;
    // The pasteboard is only good for the length of this call: read
    // it now, and leave the opening to the handler's queue.
    const QStringList folders = gitbolt::app::macos::foldersFromPasteboard(pboard);
    if (folders.isEmpty()) {
        if (error)
            *error = @"GitBolt opens folders only.";
        return;
    }
    if (_handler)
        _handler(folders);
}
@end

namespace gitbolt::app::macos {

namespace {

// What sendToServicesProvider() calls, whether or not Finder's
// requests come to it too.
GitBoltServicesProvider* s_provider = nil;
// Bumped by each install: a retry for an earlier one stops.
quint64 s_install = 0;

// While another process has the service: every 250 ms for 30 s (a
// GitBolt still quitting), then every 5 s for as long as this one runs.
constexpr int kRetryMs = 250;
constexpr int kFastRetries = 120;
constexpr int kSlowRetryMs = 5000;

bool onCocoa()
{
    return QGuiApplication::platformName() == QLatin1String("cocoa");
}

// Another process has the service: AppKit registers it with launchd as
// "<bundle id>.ServiceProvider", and a second process asking for it
// gets nothing, silently and for good.
bool serviceTakenElsewhere()
{
    NSString* bundleId = NSBundle.mainBundle.bundleIdentifier;
    if (!bundleId)
        return false;
    NSString* name = [bundleId stringByAppendingString:@".ServiceProvider"];
    mach_port_t port = MACH_PORT_NULL;
    if (bootstrap_look_up(bootstrap_port, name.UTF8String, &port) != KERN_SUCCESS)
        return false;
    mach_port_deallocate(mach_task_self(), port);
    return true;
}

void registerWhenFree(quint64 install, int attempt)
{
    if (install != s_install || !s_provider)
        return;
    // Never ask while another process has it: the ask would fail, and
    // AppKit doesn't ask again, so this GitBolt would never get it.
    if (serviceTakenElsewhere()) {
        QTimer::singleShot(attempt < kFastRetries ? kRetryMs : kSlowRetryMs, qApp,
                           [install, attempt] { registerWhenFree(install, attempt + 1); });
        return;
    }
    // NSApp keeps it: servicesProvider is a strong property.
    NSApp.servicesProvider = s_provider;
}

} // namespace

void installServicesProvider(FolderHandler onFolders, bool registerWithSystem)
{
    // QApplication made NSApp (the cocoa platform plugin); the
    // offscreen and minimal platforms that tests and CI use have none.
    if (!onCocoa())
        return;
    ++s_install;
    if (NSApp.servicesProvider == s_provider)
        NSApp.servicesProvider = nil;
    s_provider = onFolders
                     ? [[GitBoltServicesProvider alloc] initWithHandler:std::move(onFolders)]
                     : nil;
    if (s_provider && registerWithSystem)
        registerWhenFree(s_install, 0);
}

void activateApp()
{
    if (!onCocoa())
        return;
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
        [NSApp activateIgnoringOtherApps:YES];
}

bool sendToServicesProvider(const QStringList& folders, QString& error)
{
    if (!onCocoa()) {
        error = QStringLiteral("no services provider off the cocoa platform");
        return false;
    }
    GitBoltServicesProvider* provider = s_provider;
    if (!provider) {
        error = QStringLiteral("no services provider installed");
        return false;
    }
    NSPasteboard* pboard = [NSPasteboard pasteboardWithUniqueName];
    NSMutableArray<NSURL*>* urls = [NSMutableArray array];
    for (const QString& folder : folders)
        [urls addObject:[NSURL fileURLWithPath:folder.toNSString()]];
    [pboard clearContents];
    const bool written = [pboard writeObjects:urls];
    NSString* refusal = nil;
    if (written)
        [provider openInGitBolt:pboard userData:nil error:&refusal];
    [pboard releaseGlobally];
    if (!written) {
        error = QStringLiteral("could not write the pasteboard");
        return false;
    }
    if (refusal) {
        error = QString::fromNSString(refusal);
        return false;
    }
    return true;
}

} // namespace gitbolt::app::macos
