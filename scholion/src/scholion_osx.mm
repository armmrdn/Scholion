// scholion_osx.mm — macOS file-open integration
//
// Two-phase registration to beat NSApplication's own kAEOpenDocuments handler:
//
//  Phase 1 (before glfwInit): register an NSApplicationWillFinishLaunching
//    observer. When glfwInit() calls [NSApp finishLaunching], this fires
//    synchronously BEFORE any Apple Events are dispatched. We use it to:
//      a) Inject application:openFile: / openFiles: into GLFW's delegate so
//         that the delegate-based path queues paths instead of silently failing.
//      b) Register (or re-register) our NSAppleEventManager handler, overriding
//         NSApplication's default kAEOpenDocuments handler.
//
//  Phase 2 (after glfwInit): call scholion_register_file_handler() again to
//    ensure the NSAppleEventManager entry is ours for any later warm launches.
//
//  Main loop drains s_pending each frame via scholion_pop_pending_open().

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#include <string>
#include <vector>

static std::vector<std::string> s_pending;

// ---------------------------------------------------------------------------
// Replacement IMP injected into GLFW's NSApplicationDelegate at runtime.
// Both the single-file and multi-file variants queue into s_pending.

static BOOL scholion_openFile_imp(id, SEL, NSApplication*, NSString* filename) {
    if (filename) s_pending.push_back([filename UTF8String]);
    return YES;
}

static void scholion_openFiles_imp(id, SEL, NSApplication*, NSArray<NSString*>* filenames) {
    for (NSString* f in filenames)
        if (f) s_pending.push_back([f UTF8String]);
}

static void inject_into_app_delegate(void) {
    id delegate = [NSApp delegate];
    if (!delegate) return;
    Class cls = object_getClass(delegate);

    auto inject = [&](SEL sel, IMP imp, const char* types) {
        Method m = class_getInstanceMethod(cls, sel);
        if (m) method_setImplementation(m, imp);
        else   class_addMethod(cls, sel, imp, types);
    };

    inject(@selector(application:openFile:),  (IMP)scholion_openFile_imp,  "c@:@@");
    inject(@selector(application:openFiles:), (IMP)scholion_openFiles_imp, "v@:@@");
}

// ---------------------------------------------------------------------------
// NSAppleEventManager handler — intercepts kAEOpenDocuments at the AE layer.

@interface _ScholionAEHandler : NSObject
- (void)handleOpen:(NSAppleEventDescriptor*)event
             reply:(NSAppleEventDescriptor*)reply;
@end
@implementation _ScholionAEHandler
- (void)handleOpen:(NSAppleEventDescriptor*)event
             reply:(NSAppleEventDescriptor*)reply {
    NSAppleEventDescriptor* list = [event paramDescriptorForKeyword:keyDirectObject];
    NSInteger n = list ? [list numberOfItems] : 0;
    for (NSInteger i = 1; i <= n; ++i) {
        NSAppleEventDescriptor* urlDesc =
            [[list descriptorAtIndex:i] coerceToDescriptorType:typeFileURL];
        if (!urlDesc) continue;
        NSString* urlStr = [[NSString alloc] initWithData:[urlDesc data]
                                                 encoding:NSUTF8StringEncoding];
        if (!urlStr) continue;
        NSURL*    url  = [NSURL URLWithString:urlStr];
        NSString* path = url.path;
        if (path) s_pending.push_back(path.UTF8String);
    }
}
@end

static _ScholionAEHandler* s_aeHandler = nil;

static void register_ae_handler(void) {
    if (!s_aeHandler) s_aeHandler = [[_ScholionAEHandler alloc] init];
    [[NSAppleEventManager sharedAppleEventManager]
        setEventHandler:s_aeHandler
            andSelector:@selector(handleOpen:reply:)
          forEventClass:kCoreEventClass
             andEventID:kAEOpenDocuments];
}

// ---------------------------------------------------------------------------
extern "C" {

// Call BEFORE glfwInit(). Registers a WillFinishLaunching observer that fires
// during glfwInit() (when NSApp calls finishLaunching) to inject our handlers
// before Apple Events are dispatched that frame.
void scholion_register_early(void) {
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSApplicationWillFinishLaunchingNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
            inject_into_app_delegate();
            register_ae_handler();
        }];
}

// Call AFTER glfwInit(). Re-registers the NSAppleEventManager entry (overrides
// any handler NSApplication installed during its own initialization) and
// injects into the delegate for completeness.
void scholion_register_file_handler(void) {
    inject_into_app_delegate();
    register_ae_handler();
}

// Returns 1 and copies the next queued path into buf (null-terminated),
// or returns 0 if the queue is empty. buf_len includes the null terminator.
int scholion_pop_pending_open(char* buf, int buf_len) {
    if (s_pending.empty()) return 0;
    const std::string& p = s_pending.front();
    if ((int)p.size() + 1 > buf_len) return 0;
    memcpy(buf, p.c_str(), p.size() + 1);
    s_pending.erase(s_pending.begin());
    return 1;
}

// Call before any tinyfd dialog so the dialog appears in front of the app.
void scholion_activate_app(void) {
    [NSApp activateIgnoringOtherApps:YES];
}

} // extern "C"
