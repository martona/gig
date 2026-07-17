#include "ui/settings_dialog.h"

#include "app/app_session.h"

#include <algorithm>
#include <functional>
#include <string>

#import <Cocoa/Cocoa.h>

// Native AppKit settings, styled after the System Settings app: rounded "card"
// sections (NSBox), rows with a title (and secondary subtitle) on the left and
// the control on the right, switches (NSSwitch) for booleans, large-size text
// fields. The primary window carries the connection fields + the View group;
// "Advanced..." opens a second window (security / display / screen protection).
// Everything edits working copies and commits ONLY on the primary OK; the
// password is persisted through the SettingsStore by main.cpp, so a signed gig
// owns the keychain item (no prompt -- see settings_store_mac.mm).

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

constexpr CGFloat kWinW = 560;   // both windows
constexpr CGFloat kMargin = 20;  // window edge -> card
constexpr CGFloat kCardW = kWinW - 2 * kMargin;
constexpr CGFloat kInset = 14;   // card edge -> row content
constexpr CGFloat kSwitchW = 40; // NSSwitch intrinsic width (approx; right-aligned)

NSString* toNs(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }

std::string fromField(NSTextField* field)
{
    const char* s = field.stringValue.UTF8String;
    return s ? std::string(s) : std::string();
}

// Idle-dim delay choices (seconds; 0 = Never), matching the Windows dropdown.
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

// --- System Settings-style building blocks -----------------------------------

// Small secondary header above a card ("View", "Screen protection", ...).
void addCardHeader(NSView* parent, CGFloat& top, NSString* text)
{
    NSTextField* l = [NSTextField labelWithString:text];
    l.font = [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
    l.textColor = [NSColor secondaryLabelColor];
    l.frame = NSMakeRect(kMargin + 4, top - 16, kCardW - 8, 14);
    [parent addSubview:l];
    top -= 22;
}

// A rounded card; rows go into its contentView (local coordinates). `top` is
// the card's top edge and advances past the card + gap.
NSView* addCard(NSView* parent, CGFloat& top, CGFloat height)
{
    NSBox* box = [[NSBox alloc] initWithFrame:NSMakeRect(kMargin, top - height, kCardW, height)];
    box.boxType = NSBoxCustom;
    box.cornerRadius = 10.0;
    box.borderWidth = 1.0;
    box.borderColor = [NSColor separatorColor];
    box.fillColor = [NSColor controlBackgroundColor];
    box.titlePosition = NSNoTitle;
    box.contentViewMargins = NSMakeSize(0, 0);
    [parent addSubview:box];
    top -= height + 14;
    return box.contentView;
}

// Hairline row separator inside a card, at local y.
void addSeparator(NSView* card, CGFloat y)
{
    NSBox* line = [[NSBox alloc] initWithFrame:NSMakeRect(kInset, y, kCardW - 2 * kInset, 1)];
    line.boxType = NSBoxSeparator;
    [card addSubview:line];
}

// Row title (13pt) with optional secondary subtitle beneath; centered on rowCenterY.
void addRowText(NSView* card, CGFloat rowCenterY, NSString* title, NSString* subtitle, CGFloat rightReserve)
{
    const CGFloat width = kCardW - 2 * kInset - rightReserve;
    if (subtitle) {
        NSTextField* t = [NSTextField labelWithString:title];
        t.font = [NSFont systemFontOfSize:13];
        t.frame = NSMakeRect(kInset, rowCenterY + 1, width, 17);
        [card addSubview:t];
        NSTextField* s = [NSTextField labelWithString:subtitle];
        s.font = [NSFont systemFontOfSize:11];
        s.textColor = [NSColor secondaryLabelColor];
        s.frame = NSMakeRect(kInset, rowCenterY - 15, width, 14);
        [card addSubview:s];
    } else {
        NSTextField* t = [NSTextField labelWithString:title];
        t.font = [NSFont systemFontOfSize:13];
        t.frame = NSMakeRect(kInset, rowCenterY - 8, width, 17);
        [card addSubview:t];
    }
}

// Right-aligned switch on a row.
NSSwitch* addSwitch(NSView* card, CGFloat rowCenterY, BOOL on)
{
    NSSwitch* sw = [[NSSwitch alloc] init];
    sw.frame = NSMakeRect(kCardW - kInset - kSwitchW, rowCenterY - 11, kSwitchW, 22);
    sw.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    [card addSubview:sw];
    return sw;
}

// Right-aligned popup on a row.
NSPopUpButton* addPopup(NSView* card, CGFloat rowCenterY, CGFloat width, NSArray<NSString*>* titles, NSInteger selected)
{
    NSPopUpButton* popup = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(kCardW - kInset - width, rowCenterY - 14, width, 28)
            pullsDown:NO];
    popup.controlSize = NSControlSizeLarge;
    [popup addItemsWithTitles:titles];
    [popup selectItemAtIndex:selected];
    [card addSubview:popup];
    return popup;
}

// Right-aligned text field on a row (label-left layout).
NSTextField* addField(NSView* card, CGFloat rowCenterY, CGFloat width, const std::string& value, BOOL secure)
{
    const NSRect frame = NSMakeRect(kCardW - kInset - width, rowCenterY - 14, width, 28);
    NSTextField* f = secure ? [[NSSecureTextField alloc] initWithFrame:frame]
                            : [[NSTextField alloc] initWithFrame:frame];
    f.controlSize = NSControlSizeLarge;
    f.font = [NSFont systemFontOfSize:13];
    f.stringValue = toNs(value);
    [card addSubview:f];
    return f;
}

// --- Advanced window ----------------------------------------------------------
// Security / Display / Screen protection. Edits the working values in place on
// its own OK only (its Cancel leaves them untouched, like the Windows dialog).
// PEM CA/cert/key, login-refresh, poll-interval and software-decode have no UI
// anymore -- the settings-store keys are still honored (registry/defaults-level
// escape hatches); they ride through the working config unchanged.
void showAdvancedDialog(AppConfig& config, int& labelMode,
                        int& dimLevelPercent, int& dimDelaySeconds, int& orbitStepSeconds,
                        const std::function<void(int)>& onDimPreview)
{
    @autoreleasepool {
        const CGFloat height = 420;
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kWinW, height)
                                                       styleMask:NSWindowStyleMaskTitled
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.title = @"Advanced Settings";
        window.releasedWhenClosed = NO;
        NSView* content = window.contentView;
        GigSettingsController* controller = [[GigSettingsController alloc] init];
        NSMutableArray* helpers = [NSMutableArray array]; // keep target objects alive

        CGFloat top = height - kMargin;

        addCardHeader(content, top, @"Security");
        NSView* security = addCard(content, top, 56);
        addRowText(security, 28, @"Skip server certificate verification",
                   @"Insecure — disables pinning. For testing only.", kSwitchW + 8);
        NSSwitch* insecureSwitch = addSwitch(security, 28, !config.tls.verifyServer);

        addCardHeader(content, top, @"Display");
        NSView* display = addCard(content, top, 48);
        addRowText(display, 24, @"Camera labels", nil, 220);
        NSPopUpButton* labelPopup = addPopup(display, 24, 210,
            @[ @"Hide", @"Show on error only", @"Always show" ],
            std::clamp(labelMode, 0, 2));

        addCardHeader(content, top, @"Screen protection");
        NSView* burnin = addCard(content, top, 152);
        // Dim slider row (top local y 152..108, center 130).
        addRowText(burnin, 130, @"Dim to", nil, 320);
        NSTextField* dimValueLabel =
            [NSTextField labelWithString:[NSString stringWithFormat:@"%d%%", std::clamp(dimLevelPercent, 10, 100)]];
        dimValueLabel.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
        dimValueLabel.alignment = NSTextAlignmentRight;
        dimValueLabel.frame = NSMakeRect(kCardW - kInset - 44, 122, 44, 16);
        [burnin addSubview:dimValueLabel];
        NSSlider* dimSlider = [NSSlider sliderWithValue:std::clamp(dimLevelPercent, 10, 100)
                                               minValue:10 maxValue:100 target:nil action:nil];
        dimSlider.frame = NSMakeRect(110, 118, kCardW - 110 - kInset - 52, 24);
        dimSlider.continuous = YES;
        [burnin addSubview:dimSlider];
        GigDimSliderHelper* dimHelper = [[GigDimSliderHelper alloc] init];
        dimHelper.valueLabel = dimValueLabel;
        dimHelper.onChange = ^(int pct) { if (onDimPreview) onDimPreview(pct); };
        dimSlider.target = dimHelper;
        dimSlider.action = @selector(changed:);
        [helpers addObject:dimHelper];
        addSeparator(burnin, 108);
        // Dim delay row (108..64, center 86).
        addRowText(burnin, 86, @"Dim after", nil, 220);
        NSPopUpButton* dimDelayPopup = addPopup(burnin, 86, 210, dimDelayTitles(),
                                                dimDelayIndexFor(dimDelaySeconds));
        addSeparator(burnin, 64);
        // Pixel-shift row (64..0, center 32) with subtitle.
        addRowText(burnin, 32, @"Pixel-shift step (seconds)",
                   @"~1px per step spreads OLED wear; lower = more motion.", 110);
        NSTextField* orbitField = addField(burnin, 32, 90, std::to_string(orbitStepSeconds), NO);

        NSButton* okButton = [NSButton buttonWithTitle:@"OK" target:controller action:@selector(ok:)];
        okButton.frame = NSMakeRect(kWinW - 110, 16, 94, 30);
        okButton.keyEquivalent = @"\r";
        [content addSubview:okButton];
        NSButton* cancelButton = [NSButton buttonWithTitle:@"Cancel" target:controller action:@selector(cancel:)];
        cancelButton.frame = NSMakeRect(kWinW - 214, 16, 94, 30);
        cancelButton.keyEquivalent = @"\033";
        [content addSubview:cancelButton];

        [window center];
        [window makeKeyAndOrderFront:nil];
        const NSModalResponse response = [NSApp runModalForWindow:window];
        [window orderOut:nil];
        (void)helpers; // keep-alive through the modal
        if (response != NSModalResponseOK) {
            return;
        }

        config.tls.verifyServer = (insecureSwitch.state != NSControlStateValueOn);
        labelMode = static_cast<int>(labelPopup.indexOfSelectedItem);
        dimLevelPercent = std::clamp(static_cast<int>(std::lround(dimSlider.doubleValue)), 10, 100);
        {
            const NSInteger i = dimDelayPopup.indexOfSelectedItem;
            if (i >= 0 && i < static_cast<NSInteger>(sizeof(kDimDelaySeconds) / sizeof(int))) {
                dimDelaySeconds = kDimDelaySeconds[i];
            }
        }
        orbitStepSeconds = std::clamp(static_cast<int>(orbitField.intValue), 1, 600);
    }
}

} // namespace

bool showSettingsDialog(void* parent, AppConfig& config, int& labelMode,
                        int& dimLevelPercent, int& dimDelaySeconds, int& orbitStepSeconds,
                        int& viewMode, bool& motionActivity, bool& activeOnly,
                        bool& showBoxes, bool& keepHiddenStreams,
                        bool& forgetRequested, const std::string& statusMessage,
                        const std::function<void(int)>& onDimPreview)
{
    (void)parent; // macOS modal has no owner window to thread through
    forgetRequested = false;

    @autoreleasepool {
        // Edit working copies so Cancel (in either window) leaves the caller's
        // values untouched; commit only on the primary OK.
        AppConfig working = config;
        int workingLabelMode = labelMode;
        int workingDimLevel = dimLevelPercent;
        int workingDimDelay = dimDelaySeconds;
        int workingOrbitStep = orbitStepSeconds;

        const CGFloat height = 518;
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kWinW, height)
                                                       styleMask:NSWindowStyleMaskTitled
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.title = @"gig Settings";
        window.releasedWhenClosed = NO;
        NSView* content = window.contentView;
        GigSettingsController* controller = [[GigSettingsController alloc] init];

        CGFloat top = height - kMargin;

        // Connection card: label-left / field-right rows.
        NSView* connection = addCard(content, top, 128);
        addRowText(connection, 106, @"Frigate URL", nil, 380);
        NSTextField* baseField = addField(connection, 106, 370, working.baseUrl, NO);
        addSeparator(connection, 84);
        addRowText(connection, 63, @"User", nil, 380);
        NSTextField* userField = addField(connection, 63, 370, working.user, NO);
        addSeparator(connection, 42);
        addRowText(connection, 21, @"Password", nil, 380);
        NSTextField* passField = addField(connection, 21, 370, working.password, YES);

        // The View card lives HERE, not in Advanced: what the wall shows
        // day-to-day belongs where the user can reach it.
        addCardHeader(content, top, @"View");
        NSView* view = addCard(content, top, 240);
        // Show row (240..196, center 218).
        addRowText(view, 218, @"Show", nil, 220);
        NSPopUpButton* viewPopup = addPopup(view, 218, 210,
            @[ @"All cameras", @"Active cameras only" ], viewMode == 1 ? 1 : 0);
        addSeparator(view, 196);
        // Switch rows (48 each, centers 172 / 124 / 76 / 28).
        addRowText(view, 172, @"Raw motion counts as activity",
                   @"Noisy on windy days — moving shadows and foliage count too.", kSwitchW + 8);
        NSSwitch* motionSwitch = addSwitch(view, 172, motionActivity);
        addSeparator(view, 148);
        addRowText(view, 124, @"Ignore stationary objects",
                   @"Parked cars and settled packages stop counting once they stop moving.", kSwitchW + 8);
        NSSwitch* activeOnlySwitch = addSwitch(view, 124, activeOnly);
        addSeparator(view, 100);
        addRowText(view, 76, @"Draw detection boxes",
                   @"Red pulses around a live detection; blue lingers where one just ended.", kSwitchW + 8);
        NSSwitch* boxesSwitch = addSwitch(view, 76, showBoxes);
        addSeparator(view, 52);
        addRowText(view, 28, @"Keep hidden cameras streaming",
                   @"Off saves power; a hidden camera reconnects in a second or two.", kSwitchW + 8);
        NSSwitch* keepStreamsSwitch = addSwitch(view, 28, keepHiddenStreams);

        if (!statusMessage.empty()) {
            NSTextField* status = [NSTextField labelWithString:toNs(statusMessage)];
            status.font = [NSFont systemFontOfSize:12];
            status.textColor = [NSColor systemRedColor];
            status.frame = NSMakeRect(kMargin, 58, kCardW, 16);
            [content addSubview:status];
        }

        NSButton* advancedButton = [NSButton buttonWithTitle:@"Advanced…" target:controller action:@selector(advanced:)];
        advancedButton.frame = NSMakeRect(kMargin - 4, 16, 116, 30);
        [content addSubview:advancedButton];
        // TODO(onboarding-project): temporary Forget Settings affordance.
        NSButton* forgetButton = [NSButton buttonWithTitle:@"Forget…" target:controller action:@selector(forget:)];
        forgetButton.frame = NSMakeRect(kMargin + 114, 16, 100, 30);
        [content addSubview:forgetButton];
        NSButton* okButton = [NSButton buttonWithTitle:@"OK" target:controller action:@selector(ok:)];
        okButton.frame = NSMakeRect(kWinW - 110, 16, 94, 30);
        okButton.keyEquivalent = @"\r";
        [content addSubview:okButton];
        NSButton* cancelButton = [NSButton buttonWithTitle:@"Cancel" target:controller action:@selector(cancel:)];
        cancelButton.frame = NSMakeRect(kWinW - 214, 16, 94, 30);
        cancelButton.keyEquivalent = @"\033";
        [content addSubview:cancelButton];

        AppConfig* workingPtr = &working;
        int* labelPtr = &workingLabelMode;
        int* dimLevelPtr = &workingDimLevel;
        int* dimDelayPtr = &workingDimDelay;
        int* orbitStepPtr = &workingOrbitStep;
        controller.onAdvanced = ^{
            showAdvancedDialog(*workingPtr, *labelPtr, *dimLevelPtr, *dimDelayPtr,
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
        labelMode = workingLabelMode;
        dimLevelPercent = workingDimLevel;
        dimDelaySeconds = workingDimDelay;
        orbitStepSeconds = workingOrbitStep;
        viewMode = viewPopup.indexOfSelectedItem == 1 ? 1 : 0;
        motionActivity = (motionSwitch.state == NSControlStateValueOn);
        activeOnly = (activeOnlySwitch.state == NSControlStateValueOn);
        showBoxes = (boxesSwitch.state == NSControlStateValueOn);
        keepHiddenStreams = (keepStreamsSwitch.state == NSControlStateValueOn);
        return true;
    }
}

} // namespace gig
