#include "ui/settings_dialog.h"

#include "app/app_session.h"

#include <algorithm>
#include <functional>
#include <string>

#import <Cocoa/Cocoa.h>

// Native AppKit settings, mirroring the Windows dialog: a 3-field primary window
// (Frigate URL / User / Password) with an "Advanced..." button that opens a second
// "Advanced settings" window (TLS/security + Tuning + Stream template). Built
// programmatically and run modally; Save writes the edited values back. The password
// is persisted through the SettingsStore by main.cpp, so a signed gig owns the
// keychain item (no prompt -- see settings_store_mac.mm).

@interface GigSettingsController : NSObject
@property (nonatomic, copy) void (^onAdvanced)(void);
@end

@implementation GigSettingsController
- (void)ok:(id)sender { (void)sender; [NSApp stopModalWithCode:NSModalResponseOK]; }
- (void)cancel:(id)sender { (void)sender; [NSApp stopModalWithCode:NSModalResponseCancel]; }
- (void)advanced:(id)sender { (void)sender; if (self.onAdvanced) self.onAdvanced(); }
// TODO(onboarding-project): temporary Forget Settings affordance; remove when done.
// NSModalResponseAbort = the confirmed "forget" outcome (distinct from OK/Cancel).
- (void)forget:(id)sender
{
    (void)sender;
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Forget ALL settings?";
    alert.informativeText = @"This erases the server, credentials, certificate pins and "
                            @"window state, and restarts first-run setup.";
    alert.alertStyle = NSAlertStyleWarning;
    [alert addButtonWithTitle:@"Cancel"];
    [alert addButtonWithTitle:@"Forget Settings"];
    if ([alert runModal] == NSAlertSecondButtonReturn) {
        [NSApp stopModalWithCode:NSModalResponseAbort];
    }
}
@end

// Opens an NSOpenPanel and writes the chosen file path into its target field.
@interface GigBrowseHelper : NSObject
@property (nonatomic, weak) NSTextField* target;
@end

@implementation GigBrowseHelper
- (void)browse:(id)sender
{
    (void)sender;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    if ([panel runModal] == NSModalResponseOK && panel.URL) {
        self.target.stringValue = panel.URL.path;
    }
}
@end

// Idle-dim slider: updates its "NN%" label and live-previews the dim on the main
// view (the block hops to the C++ preview callback) as the slider moves.
@interface GigDimSliderHelper : NSObject
@property (nonatomic, weak) NSTextField* valueLabel;
@property (nonatomic, copy) void (^onChange)(int percent);
@end

@implementation GigDimSliderHelper
- (void)changed:(NSSlider*)sender
{
    const int percent = static_cast<int>(std::lround(sender.doubleValue));
    self.valueLabel.stringValue = [NSString stringWithFormat:@"%d%%", percent];
    if (self.onChange) {
        self.onChange(percent);
    }
}
@end

namespace gig {
namespace {

NSString* toNs(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }

std::string fromField(NSTextField* field)
{
    const char* s = field.stringValue.UTF8String;
    return s ? std::string(s) : std::string();
}

// Advanced window: edits the working config's TLS / tuning / stream-template fields
// in place, but only on its own OK (Cancel leaves the working copy untouched, same
// as the Windows advanced dialog).
// Idle-dim delay choices (seconds; 0 = Never), matching the Windows dropdown.
struct DimDelayChoice { int seconds; NSString* label; };
static NSArray<NSString*>* dimDelayTitles()
{
    return @[ @"Never", @"5 minutes", @"10 minutes", @"15 minutes", @"30 minutes",
              @"1 hour", @"2 hours", @"4 hours", @"8 hours" ];
}
static const int kDimDelaySeconds[] = { 0, 300, 600, 900, 1800, 3600, 7200, 14400, 28800 };
static int dimDelayIndexFor(int seconds)
{
    int best = 0, bestDiff = INT_MAX;
    for (int i = 0; i < static_cast<int>(sizeof(kDimDelaySeconds) / sizeof(int)); ++i) {
        const int diff = std::abs(kDimDelaySeconds[i] - seconds);
        if (diff < bestDiff) { bestDiff = diff; best = i; }
    }
    return best;
}

void showAdvancedDialog(AppConfig& config, bool& showOverlay, int& labelMode,
                        int& dimLevelPercent, int& dimDelaySeconds, int& orbitStepSeconds,
                        const std::function<void(int)>& onDimPreview)
{
    @autoreleasepool {
        constexpr CGFloat kWidth = 560;
        constexpr CGFloat kRow = 30;
        const CGFloat height = 620;

        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kWidth, height)
                                                       styleMask:NSWindowStyleMaskTitled
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.title = @"Advanced settings";
        window.releasedWhenClosed = NO;
        NSView* content = window.contentView;
        GigSettingsController* controller = [[GigSettingsController alloc] init];

        CGFloat y = height - 24;
        auto section = [&](NSString* text) {
            NSTextField* l = [NSTextField labelWithString:text];
            l.frame = NSMakeRect(16, y, kWidth - 32, 18);
            l.font = [NSFont boldSystemFontOfSize:12];
            [content addSubview:l];
            y -= 24;
        };
        auto label = [&](NSString* text) {
            NSTextField* l = [NSTextField labelWithString:text];
            l.frame = NSMakeRect(16, y, 150, 20);
            l.alignment = NSTextAlignmentRight;
            [content addSubview:l];
        };
        auto field = [&](const std::string& value, CGFloat width) -> NSTextField* {
            NSTextField* f = [[NSTextField alloc] initWithFrame:NSMakeRect(174, y - 2, width, 24)];
            f.stringValue = toNs(value);
            [content addSubview:f];
            return f;
        };
        // A path field with a trailing "…" button that opens an NSOpenPanel.
        NSMutableArray* browseHelpers = [NSMutableArray array];
        auto fieldWithBrowse = [&](const std::string& value) -> NSTextField* {
            constexpr CGFloat browseW = 40;
            const CGFloat fieldW = kWidth - 190 - browseW - 6;
            NSTextField* f = [[NSTextField alloc] initWithFrame:NSMakeRect(174, y - 2, fieldW, 24)];
            f.stringValue = toNs(value);
            [content addSubview:f];
            GigBrowseHelper* helper = [[GigBrowseHelper alloc] init];
            helper.target = f;
            [browseHelpers addObject:helper]; // keep alive for the modal's lifetime
            NSButton* browse = [NSButton buttonWithTitle:@"…" target:helper action:@selector(browse:)];
            browse.frame = NSMakeRect(174 + fieldW + 6, y - 2, browseW, 24);
            [content addSubview:browse];
            return f;
        };
        auto check = [&](NSString* title, BOOL on) -> NSButton* {
            NSButton* b = [NSButton checkboxWithTitle:title target:nil action:nil];
            b.frame = NSMakeRect(16, y, kWidth - 32, 22);
            b.state = on ? NSControlStateValueOn : NSControlStateValueOff;
            [content addSubview:b];
            return b;
        };
        auto row = [&]() { y -= kRow; };

        section(@"TLS / security");
        label(@"CA file:");
        NSTextField* caField = fieldWithBrowse(config.tls.caFile);
        row();
        label(@"Client cert:");
        NSTextField* certField = fieldWithBrowse(config.tls.certFile);
        row();
        label(@"Client key:");
        NSTextField* keyField = fieldWithBrowse(config.tls.keyFile);
        row();
        NSTextField* note = [NSTextField labelWithString:@"Leave CA / cert / key blank to use the macOS keychain."];
        note.frame = NSMakeRect(16, y, kWidth - 32, 16);
        note.textColor = [NSColor secondaryLabelColor];
        [content addSubview:note];
        y -= 22;
        NSButton* insecureCheck = check(@"Skip server certificate verification (insecure; disables pinning)",
                                        !config.tls.verifyServer);
        row();

        section(@"Tuning");
        label(@"Login refresh (s):");
        NSTextField* loginRefreshField = field(std::to_string(config.loginRefreshSeconds), 80);
        row();
        label(@"Poll interval (s):");
        NSTextField* pollField = field(std::to_string(config.pollIntervalSeconds), 80);
        row();
        NSButton* softwareCheck = check(@"Force software decode", config.softwareDecode);
        row();
        NSButton* overlayCheck = check(@"Show diagnostics overlay", showOverlay);
        row();
        label(@"Camera labels:");
        NSPopUpButton* labelPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(174, y - 2, 220, 26) pullsDown:NO];
        [labelPopup addItemsWithTitles:@[ @"Hide", @"Show on error only", @"Always show" ]];
        [labelPopup selectItemAtIndex:std::clamp(labelMode, 0, 2)];
        [content addSubview:labelPopup];
        row();

        section(@"Screen protection");
        label(@"Dim to:");
        NSSlider* dimSlider = [NSSlider sliderWithValue:std::clamp(dimLevelPercent, 10, 100)
                                               minValue:10 maxValue:100 target:nil action:nil];
        dimSlider.frame = NSMakeRect(174, y - 2, kWidth - 174 - 60, 24);
        dimSlider.continuous = YES;
        [content addSubview:dimSlider];
        NSTextField* dimValueLabel =
            [NSTextField labelWithString:[NSString stringWithFormat:@"%d%%", std::clamp(dimLevelPercent, 10, 100)]];
        dimValueLabel.frame = NSMakeRect(kWidth - 52, y, 44, 20);
        [content addSubview:dimValueLabel];
        GigDimSliderHelper* dimHelper = [[GigDimSliderHelper alloc] init];
        dimHelper.valueLabel = dimValueLabel;
        dimHelper.onChange = ^(int pct) { if (onDimPreview) onDimPreview(pct); };
        dimSlider.target = dimHelper;
        dimSlider.action = @selector(changed:);
        [browseHelpers addObject:dimHelper]; // keep alive for the modal's lifetime
        row();
        label(@"Dim after:");
        NSPopUpButton* dimDelayPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(174, y - 2, 220, 26) pullsDown:NO];
        [dimDelayPopup addItemsWithTitles:dimDelayTitles()];
        [dimDelayPopup selectItemAtIndex:dimDelayIndexFor(dimDelaySeconds)];
        [content addSubview:dimDelayPopup];
        row();
        label(@"Pixel-shift step (s):");
        NSTextField* orbitField = field(std::to_string(orbitStepSeconds), 80);
        row();
        NSTextField* orbitNote = [NSTextField labelWithString:@"The image nudges ~1px every N s to spread OLED wear (lower = more motion)."];
        orbitNote.frame = NSMakeRect(16, y, kWidth - 32, 16);
        orbitNote.textColor = [NSColor secondaryLabelColor];
        [content addSubview:orbitNote];
        y -= 22;

        section(@"Advanced connection");
        label(@"Stream template:");
        NSTextField* streamField = field(config.streamUrlTemplate, kWidth - 190);
        row();

        NSButton* okButton = [NSButton buttonWithTitle:@"OK" target:controller action:@selector(ok:)];
        okButton.frame = NSMakeRect(kWidth - 110, 16, 94, 30);
        okButton.keyEquivalent = @"\r";
        [content addSubview:okButton];
        NSButton* cancelButton = [NSButton buttonWithTitle:@"Cancel" target:controller action:@selector(cancel:)];
        cancelButton.frame = NSMakeRect(kWidth - 214, 16, 94, 30);
        cancelButton.keyEquivalent = @"\033";
        [content addSubview:cancelButton];

        [window center];
        [window makeKeyAndOrderFront:nil];
        const NSModalResponse response = [NSApp runModalForWindow:window];
        [window orderOut:nil];
        if (response != NSModalResponseOK) {
            return;
        }

        config.tls.caFile = fromField(caField);
        config.tls.certFile = fromField(certField);
        config.tls.keyFile = fromField(keyField);
        config.tls.verifyServer = (insecureCheck.state != NSControlStateValueOn);
        config.loginRefreshSeconds = loginRefreshField.intValue;
        config.pollIntervalSeconds = pollField.intValue;
        config.softwareDecode = (softwareCheck.state == NSControlStateValueOn);
        showOverlay = (overlayCheck.state == NSControlStateValueOn);
        labelMode = static_cast<int>(labelPopup.indexOfSelectedItem);
        dimLevelPercent = std::clamp(static_cast<int>(std::lround(dimSlider.doubleValue)), 10, 100);
        {
            const NSInteger i = dimDelayPopup.indexOfSelectedItem;
            if (i >= 0 && i < static_cast<NSInteger>(sizeof(kDimDelaySeconds) / sizeof(int))) {
                dimDelaySeconds = kDimDelaySeconds[i];
            }
        }
        orbitStepSeconds = std::clamp(static_cast<int>(orbitField.intValue), 1, 600);
        config.streamUrlTemplate = fromField(streamField);
    }
}

} // namespace

bool showSettingsDialog(void* parent, AppConfig& config, bool& showOverlay, int& labelMode,
                        int& dimLevelPercent, int& dimDelaySeconds, int& orbitStepSeconds,
                        bool& forgetRequested, const std::string& statusMessage,
                        const std::function<void(int)>& onDimPreview)
{
    (void)parent; // macOS modal has no owner window to thread through
    forgetRequested = false;

    @autoreleasepool {
        // Edit a working copy so Cancel (in either window) leaves the caller's config
        // untouched; commit only on the primary OK.
        AppConfig working = config;
        bool workingOverlay = showOverlay;
        int workingLabelMode = labelMode;
        int workingDimLevel = dimLevelPercent;
        int workingDimDelay = dimDelaySeconds;
        int workingOrbitStep = orbitStepSeconds;

        constexpr CGFloat kWidth = 520;
        const CGFloat height = 196;

        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kWidth, height)
                                                       styleMask:NSWindowStyleMaskTitled
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.title = @"gig settings";
        window.releasedWhenClosed = NO;
        NSView* content = window.contentView;
        GigSettingsController* controller = [[GigSettingsController alloc] init];

        CGFloat y = height - 36;
        auto label = [&](NSString* text) {
            NSTextField* l = [NSTextField labelWithString:text];
            l.frame = NSMakeRect(16, y, 90, 20);
            l.alignment = NSTextAlignmentRight;
            [content addSubview:l];
        };
        auto field = [&](const std::string& value, BOOL secure) -> NSTextField* {
            const NSRect frame = NSMakeRect(114, y - 2, kWidth - 130, 24);
            NSTextField* f = secure ? [[NSSecureTextField alloc] initWithFrame:frame]
                                    : [[NSTextField alloc] initWithFrame:frame];
            f.stringValue = toNs(value);
            [content addSubview:f];
            return f;
        };

        label(@"Frigate URL:");
        NSTextField* baseField = field(working.baseUrl, NO);
        y -= 32;
        label(@"User:");
        NSTextField* userField = field(working.user, NO);
        y -= 32;
        label(@"Password:");
        NSTextField* passField = field(working.password, YES);
        y -= 32;

        if (!statusMessage.empty()) {
            NSTextField* status = [NSTextField labelWithString:toNs(statusMessage)];
            status.frame = NSMakeRect(16, 52, kWidth - 32, 16);
            status.textColor = [NSColor systemRedColor];
            [content addSubview:status];
        }

        NSButton* advancedButton = [NSButton buttonWithTitle:@"Advanced..." target:controller action:@selector(advanced:)];
        advancedButton.frame = NSMakeRect(16, 16, 110, 30);
        [content addSubview:advancedButton];
        // TODO(onboarding-project): temporary Forget Settings affordance.
        NSButton* forgetButton = [NSButton buttonWithTitle:@"Forget..." target:controller action:@selector(forget:)];
        forgetButton.frame = NSMakeRect(130, 16, 96, 30);
        [content addSubview:forgetButton];
        NSButton* okButton = [NSButton buttonWithTitle:@"OK" target:controller action:@selector(ok:)];
        okButton.frame = NSMakeRect(kWidth - 110, 16, 94, 30);
        okButton.keyEquivalent = @"\r";
        [content addSubview:okButton];
        NSButton* cancelButton = [NSButton buttonWithTitle:@"Cancel" target:controller action:@selector(cancel:)];
        cancelButton.frame = NSMakeRect(kWidth - 214, 16, 94, 30);
        cancelButton.keyEquivalent = @"\033";
        [content addSubview:cancelButton];

        AppConfig* workingPtr = &working;
        bool* overlayPtr = &workingOverlay;
        int* labelPtr = &workingLabelMode;
        int* dimLevelPtr = &workingDimLevel;
        int* dimDelayPtr = &workingDimDelay;
        int* orbitStepPtr = &workingOrbitStep;
        controller.onAdvanced = ^{
            showAdvancedDialog(*workingPtr, *overlayPtr, *labelPtr, *dimLevelPtr, *dimDelayPtr,
                               *orbitStepPtr, onDimPreview);
        };

        [window center];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        const NSModalResponse response = [NSApp runModalForWindow:window];
        [window orderOut:nil];
        if (response == NSModalResponseAbort) {
            forgetRequested = true; // confirmed "Forget..." -- caller wipes the store
            return false;
        }
        if (response != NSModalResponseOK) {
            return false;
        }

        working.baseUrl = fromField(baseField);
        working.user = fromField(userField);
        working.password = fromField(passField);
        config = working;
        showOverlay = workingOverlay;
        labelMode = workingLabelMode;
        dimLevelPercent = workingDimLevel;
        dimDelaySeconds = workingDimDelay;
        orbitStepSeconds = workingOrbitStep;
        return true;
    }
}

} // namespace gig
