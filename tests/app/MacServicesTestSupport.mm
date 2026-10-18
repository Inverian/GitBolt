#import "MacServicesTestSupport.h"

#import "app/platform/macos/MacServices.h"

namespace gitbolt::test {

namespace {

ServiceCall call(NSPasteboard* pboard)
{
    ServiceCall result;
    GitBoltServicesProvider* provider = [[GitBoltServicesProvider alloc]
        initWithHandler:[&result](const QStringList& folders) {
            result.handlerCalled = true;
            result.folders = folders;
        }];
    NSString* error = nil;
    [provider openInGitBolt:pboard userData:nil error:&error];
    if (error)
        result.error = QString::fromNSString(error);
    return result;
}

NSArray<NSURL*>* fileURLs(const QStringList& paths)
{
    NSMutableArray<NSURL*>* urls = [NSMutableArray array];
    for (const QString& path : paths)
        [urls addObject:[NSURL fileURLWithPath:path.toNSString()]];
    return urls;
}

} // namespace

bool pasteboardAvailable()
{
    NSPasteboard* pboard = [NSPasteboard pasteboardWithUniqueName];
    [pboard clearContents];
    const bool ok = [pboard writeObjects:@[ @"probe" ]];
    [pboard releaseGlobally];
    return ok;
}

ServiceCall callProvider(const QStringList& paths, const QStringList& texts)
{
    NSPasteboard* pboard = [NSPasteboard pasteboardWithUniqueName];
    [pboard clearContents];
    NSMutableArray* items = [NSMutableArray arrayWithArray:fileURLs(paths)];
    for (const QString& text : texts)
        [items addObject:text.toNSString()];
    [pboard writeObjects:items];
    ServiceCall result = call(pboard);
    [pboard releaseGlobally];
    return result;
}

ServiceCall callProviderWithFilenames(const QStringList& paths)
{
    NSPasteboard* pboard = [NSPasteboard pasteboardWithUniqueName];
    NSMutableArray<NSString*>* names = [NSMutableArray array];
    for (const QString& path : paths)
        [names addObject:path.toNSString()];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [pboard declareTypes:@[ NSFilenamesPboardType ] owner:nil];
    [pboard setPropertyList:names forType:NSFilenamesPboardType];
#pragma clang diagnostic pop
    ServiceCall result = call(pboard);
    [pboard releaseGlobally];
    return result;
}

bool providerAnswers(const QString& nsMessage)
{
    NSString* selector = [nsMessage.toNSString() stringByAppendingString:@":userData:error:"];
    return [GitBoltServicesProvider instancesRespondToSelector:NSSelectorFromString(selector)];
}

} // namespace gitbolt::test
