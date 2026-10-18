#import "app/platform/macos/MacServices.h"

#include <QGuiApplication>

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
bool onCocoa()
{
    return QGuiApplication::platformName() == QLatin1String("cocoa");
}
} // namespace

void installServicesProvider(FolderHandler onFolders)
{
    // QApplication made NSApp (the cocoa platform plugin); the
    // offscreen and minimal platforms that tests and CI use have none.
    if (!onCocoa())
        return;
    // NSApp keeps it: servicesProvider is a strong property.
    NSApp.servicesProvider =
        onFolders ? [[GitBoltServicesProvider alloc] initWithHandler:std::move(onFolders)]
                  : nil;
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
    id provider = NSApp.servicesProvider;
    if (![provider isKindOfClass:GitBoltServicesProvider.class]) {
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
