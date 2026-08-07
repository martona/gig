#include "ui/settings_dialog.h"

#include <algorithm>
#include <functional>
#include <string>

#import <Cocoa/Cocoa.h>

// Native AppKit settings, styled after the System Settings app: rounded "card"
// sections (NSBox), rows with a title (and secondary subtitle) on the left and
// the control on the right, switches (NSSwitch) for booleans, large-size text
// fields. The primary window carries the connection registry (popup +
// Add/Edit/Delete; per-connection fields live in a nested editor window) + the
// View group; "Advanced..." opens a second window (display / screen
// protection). Everything edits working copies and commits ONLY on the primary
// OK; the password is persisted through the SettingsStore by main.cpp, so a
// signed gig owns the keychain item (no prompt -- see settings_store_mac.mm).

@interface GigSettingsController : NSObject
@property (nonatomic, copy) void (^onAdvanced)(void);
@property (nonatomic, copy) void (^onAddConnection)(void);
@property (nonatomic, copy) void (^onEditConnection)(void);
@property (nonatomic, copy) void (^onDeleteConnection)(void);
@end

@implementation GigSettingsController
- (void)ok:(id)sender { (void)sender; [NSApp stopModalWithCode:NSModalResponseOK]; }
- (void)cancel:(id)sender { (void)sender; [NSApp stopModalWithCode:NSModalResponseCancel]; }
- (void)advanced:(id)sender { (void)sender; if (self.onAdvanced) self.onAdvanced(); }
- (void)addConnection:(id)sender { (void)sender; if (self.onAddConnection) self.onAddConnection(); }
- (void)editConnection:(id)sender { (void)sender; if (self.onEditConnection) self.onEditConnection(); }
- (void)deleteConnection:(id)sender { (void)sender; if (self.onDeleteConnection) self.onDeleteConnection(); }
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

// --- Connection registry (popup + editor) -------------------------------------

// (Re)fill the connections popup from the staged list. NSMenuItems are added
// to the menu directly: addItemWithTitle: DEDUPS same-title items, and two
// servers can legitimately share a label (same host:port over http vs https).
void reloadConnectionPopup(NSPopUpButton* popup, const std::vector<ConnectionInfo>& items, int select)
{
    [popup removeAllItems];
    for (const ConnectionInfo& item : items) {
        NSMenuItem* row = [[NSMenuItem alloc] initWithTitle:toNs(item.listLabel())
                                                     action:nil
                                              keyEquivalent:@""];
        [popup.menu addItem:row];
    }
    if (!items.empty()) {
        [popup selectItemAtIndex:std::clamp(select, 0, static_cast<int>(items.size()) - 1)];
    }
    popup.enabled = items.empty() ? NO : YES;
}

// Index of another staged entry with the same identity (URL) hash, or -1;
// `exceptIndex` skips the entry being edited so it can keep its own URL.
int duplicateConnectionIndex(const std::vector<ConnectionInfo>& items, const ConnectionInfo& item,
                             int exceptIndex)
{
    const std::string id = item.id();
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        if (i != exceptIndex && items[static_cast<std::size_t>(i)].id() == id) {
            return i;
        }
    }
    return -1;
}

// Per-connection editor (a nested modal, like the Advanced window): URL, user,
// password + the insecure switch -- certificate verification is a property of
// the server you connect to. Validates a non-empty URL and rejects a duplicate
// of another entry's URL (identity is URL-only); the window re-runs until
// valid or cancelled, keeping the typed field contents. True = `info` updated.
bool runConnectionEditor(ConnectionInfo& info, const std::vector<ConnectionInfo>& existing,
                         int exceptIndex)
{
    @autoreleasepool {
        const CGFloat height = 328;
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kWinW, height)
                                                       styleMask:NSWindowStyleMaskTitled
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.title = @"Connection";
        window.releasedWhenClosed = NO;
        NSView* content = window.contentView;
        GigSettingsController* controller = [[GigSettingsController alloc] init];

        CGFloat top = height - kMargin;
        NSView* card = addCard(content, top, 128);
        addRowText(card, 106, @"Frigate URL", nil, 380);
        NSTextField* baseField = addField(card, 106, 370, info.baseUrl, NO);
        addSeparator(card, 84);
        addRowText(card, 63, @"User", nil, 380);
        NSTextField* userField = addField(card, 63, 370, info.user, NO);
        addSeparator(card, 42);
        addRowText(card, 21, @"Password", nil, 380);
        NSTextField* passField = addField(card, 21, 370, info.password, YES);

        addCardHeader(content, top, @"Security");
        NSView* security = addCard(content, top, 56);
        addRowText(security, 28, @"Skip server certificate verification",
                   @"Insecure — disables pinning. For testing only.", kSwitchW + 8);
        NSSwitch* insecureSwitch = addSwitch(security, 28, info.insecure);

        NSButton* okButton = [NSButton buttonWithTitle:@"OK" target:controller action:@selector(ok:)];
        okButton.frame = NSMakeRect(kWinW - 110, 16, 94, 30);
        okButton.keyEquivalent = @"\r";
        [content addSubview:okButton];
        NSButton* cancelButton = [NSButton buttonWithTitle:@"Cancel" target:controller action:@selector(cancel:)];
        cancelButton.frame = NSMakeRect(kWinW - 214, 16, 94, 30);
        cancelButton.keyEquivalent = @"\033";
        [content addSubview:cancelButton];

        [window center];
        for (;;) {
            [window makeKeyAndOrderFront:nil];
            const NSModalResponse response = [NSApp runModalForWindow:window];
            [window orderOut:nil];
            if (response != NSModalResponseOK) {
                return false;
            }
            ConnectionInfo edited = info; // keeps the no-UI ride-alongs
            edited.baseUrl = fromField(baseField);
            edited.user = fromField(userField);
            edited.password = fromField(passField);
            edited.insecure = (insecureSwitch.state == NSControlStateValueOn);
            NSString* problem = nil;
            if (edited.identityUrl().empty()) {
                problem = @"Enter the Frigate URL.";
            } else if (duplicateConnectionIndex(existing, edited, exceptIndex) >= 0) {
                problem = @"A connection with this URL already exists.";
            }
            if (problem) {
                NSAlert* alert = [[NSAlert alloc] init];
                alert.messageText = problem;
                alert.alertStyle = NSAlertStyleWarning;
                [alert runModal];
                continue; // fields keep their content; try again
            }
            info = edited;
            return true;
        }
    }
}

// --- Advanced window ----------------------------------------------------------
// Display / Screen protection. Edits the working values in place on its own OK
// only (its Cancel leaves them untouched, like the Windows dialog). Insecure
// moved into the per-connection editor -- certificate verification is a
// property of the server. PEM CA/cert/key, login-refresh, poll-interval and
// software-decode have no UI anymore -- the settings-store keys are still
// honored (registry/defaults-level escape hatches); they ride through
// unchanged.
void showAdvancedDialog(int& labelMode, int& labelSize,
                        int& dimLevelPercent, int& dimDelaySeconds, int& orbitStepSeconds,
                        const std::function<void(int)>& onDimPreview)
{
    @autoreleasepool {
        const CGFloat height = 376;
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

        addCardHeader(content, top, @"Display");
        NSView* display = addCard(content, top, 96);
        addRowText(display, 72, @"Camera labels", nil, 220);
        NSPopUpButton* labelPopup = addPopup(display, 72, 210,
            @[ @"Hide", @"Show on error only", @"Always show" ],
            std::clamp(labelMode, 0, 2));
        addSeparator(display, 48);
        addRowText(display, 24, @"Label size",
                   @"Applies to the tile labels and the all-quiet line.", 220);
        NSPopUpButton* sizePopup = addPopup(display, 24, 210,
            @[ @"Normal", @"Large", @"Larger" ], std::clamp(labelSize, 0, 2));

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

        labelMode = static_cast<int>(labelPopup.indexOfSelectedItem);
        labelSize = std::clamp(static_cast<int>(sizePopup.indexOfSelectedItem), 0, 2);
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

bool showSettingsDialog(void* parent, std::vector<ConnectionInfo>& connections, int& activeIndex,
                        int& labelMode, int& labelSize,
                        int& dimLevelPercent, int& dimDelaySeconds, int& orbitStepSeconds,
                        int& viewMode, bool& motionActivity, bool& activeOnly,
                        bool& showBoxes, bool& keepHiddenStreams, bool& hideOffline,
                        bool& forgetRequested, const std::string& statusMessage,
                        const std::function<void(int)>& onDimPreview)
{
    (void)parent; // macOS modal has no owner window to thread through
    forgetRequested = false;

    @autoreleasepool {
        // Edit working copies so Cancel (in either window) leaves the caller's
        // values untouched (connection adds/edits/deletes included); commit
        // only on the primary OK. The popup selection is the entry the app
        // connects to after OK.
        std::vector<ConnectionInfo> working = connections;
        int workingLabelMode = labelMode;
        int workingLabelSize = labelSize;
        int workingDimLevel = dimLevelPercent;
        int workingDimDelay = dimDelaySeconds;
        int workingOrbitStep = orbitStepSeconds;

        const CGFloat height = 560;
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kWinW, height)
                                                       styleMask:NSWindowStyleMaskTitled
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.title = @"gig Settings";
        window.releasedWhenClosed = NO;
        NSView* content = window.contentView;
        GigSettingsController* controller = [[GigSettingsController alloc] init];

        CGFloat top = height - kMargin;

        // Connections card: the multi-server registry -- a popup of the saved
        // entries (selection = the server the app connects to on OK) with
        // Add/Edit/Delete beneath; per-connection fields live in the nested
        // editor (runConnectionEditor).
        addCardHeader(content, top, @"Connections");
        NSView* connCard = addCard(content, top, 100);
        addRowText(connCard, 76, @"Server", nil, 340);
        NSPopUpButton* connPopup =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(kCardW - kInset - 330, 76 - 14, 330, 28)
                                       pullsDown:NO];
        connPopup.controlSize = NSControlSizeLarge;
        [connCard addSubview:connPopup];
        reloadConnectionPopup(connPopup, working, activeIndex);
        addSeparator(connCard, 52);
        NSButton* addButton =
            [NSButton buttonWithTitle:@"Add…" target:controller action:@selector(addConnection:)];
        addButton.frame = NSMakeRect(kCardW - kInset - 296, 11, 96, 30);
        [connCard addSubview:addButton];
        NSButton* editButton =
            [NSButton buttonWithTitle:@"Edit…" target:controller action:@selector(editConnection:)];
        editButton.frame = NSMakeRect(kCardW - kInset - 196, 11, 96, 30);
        [connCard addSubview:editButton];
        NSButton* deleteButton =
            [NSButton buttonWithTitle:@"Delete" target:controller action:@selector(deleteConnection:)];
        deleteButton.frame = NSMakeRect(kCardW - kInset - 96, 11, 96, 30);
        [connCard addSubview:deleteButton];

        // The View card lives HERE, not in Advanced: what the wall shows
        // day-to-day belongs where the user can reach it.
        addCardHeader(content, top, @"View");
        NSView* view = addCard(content, top, 288);
        // Show row (288..244, center 266).
        addRowText(view, 266, @"Show", nil, 220);
        NSPopUpButton* viewPopup = addPopup(view, 266, 210,
            @[ @"All cameras", @"Active cameras only" ], viewMode == 1 ? 1 : 0);
        addSeparator(view, 244);
        // Switch rows (48 each, centers 220 / 172 / 124 / 76 / 28).
        addRowText(view, 220, @"Raw motion counts as activity",
                   @"Noisy on windy days — moving shadows and foliage count too.", kSwitchW + 8);
        NSSwitch* motionSwitch = addSwitch(view, 220, motionActivity);
        addSeparator(view, 196);
        addRowText(view, 172, @"Ignore stationary objects",
                   @"Parked cars and settled packages stop counting once they stop moving.", kSwitchW + 8);
        NSSwitch* activeOnlySwitch = addSwitch(view, 172, activeOnly);
        addSeparator(view, 148);
        addRowText(view, 124, @"Draw detection boxes",
                   @"Red pulses around a live detection; blue lingers where one just ended.", kSwitchW + 8);
        NSSwitch* boxesSwitch = addSwitch(view, 124, showBoxes);
        addSeparator(view, 100);
        addRowText(view, 76, @"Keep hidden cameras streaming",
                   @"Off saves power; a hidden camera reconnects in a second or two.", kSwitchW + 8);
        NSSwitch* keepStreamsSwitch = addSwitch(view, 76, keepHiddenStreams);
        addSeparator(view, 52);
        addRowText(view, 28, @"Hide offline cameras",
                   @"A camera with no video disappears; a status line appears if all are down.", kSwitchW + 8);
        NSSwitch* hideOfflineSwitch = addSwitch(view, 28, hideOffline);

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

        // The registry buttons run while the primary modal spins (nested
        // modals, like Advanced); plain pointer captures into the stack-local
        // working state, alive for the whole modal.
        std::vector<ConnectionInfo>* workingPtr = &working;
        NSPopUpButton* popupRef = connPopup;
        controller.onAddConnection = ^{
            ConnectionInfo fresh;
            if (runConnectionEditor(fresh, *workingPtr, -1)) {
                workingPtr->push_back(std::move(fresh));
                reloadConnectionPopup(popupRef, *workingPtr,
                                      static_cast<int>(workingPtr->size()) - 1);
            }
        };
        controller.onEditConnection = ^{
            const int sel = static_cast<int>(popupRef.indexOfSelectedItem);
            if (sel < 0 || sel >= static_cast<int>(workingPtr->size())) {
                return;
            }
            ConnectionInfo edited = (*workingPtr)[static_cast<std::size_t>(sel)];
            if (runConnectionEditor(edited, *workingPtr, sel)) {
                (*workingPtr)[static_cast<std::size_t>(sel)] = std::move(edited);
                reloadConnectionPopup(popupRef, *workingPtr, sel);
            }
        };
        controller.onDeleteConnection = ^{
            const int sel = static_cast<int>(popupRef.indexOfSelectedItem);
            if (sel < 0 || sel >= static_cast<int>(workingPtr->size())) {
                return;
            }
            // Confirm even though the edit is staged (Cancel would undo it):
            // one habitual OK after a stray Delete would drop saved credentials.
            NSAlert* alert = [[NSAlert alloc] init];
            alert.messageText = [NSString stringWithFormat:@"Delete “%@”?",
                toNs((*workingPtr)[static_cast<std::size_t>(sel)].listLabel())];
            alert.informativeText = @"Its saved credentials are removed.";
            alert.alertStyle = NSAlertStyleWarning;
            [alert addButtonWithTitle:@"Cancel"];
            [alert addButtonWithTitle:@"Delete"];
            if ([alert runModal] == NSAlertSecondButtonReturn) {
                workingPtr->erase(workingPtr->begin() + sel);
                reloadConnectionPopup(popupRef, *workingPtr, sel);
            }
        };
        int* labelPtr = &workingLabelMode;
        int* labelSizePtr = &workingLabelSize;
        int* dimLevelPtr = &workingDimLevel;
        int* dimDelayPtr = &workingDimDelay;
        int* orbitStepPtr = &workingOrbitStep;
        controller.onAdvanced = ^{
            showAdvancedDialog(*labelPtr, *labelSizePtr, *dimLevelPtr, *dimDelayPtr,
                               *orbitStepPtr, onDimPreview);
        };

        if (working.empty()) {
            // First run (or everything deleted): drop straight into the Add
            // editor once the modal loop is spinning (queued into the modal
            // run-loop mode so the primary window appears first).
            [controller performSelector:@selector(addConnection:)
                             withObject:nil
                             afterDelay:0.0
                                inModes:@[ NSModalPanelRunLoopMode ]];
        }
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

        {
            const int sel = static_cast<int>(connPopup.indexOfSelectedItem);
            activeIndex = (sel >= 0 && sel < static_cast<int>(working.size())) ? sel : -1;
        }
        connections = std::move(working);
        labelMode = workingLabelMode;
        labelSize = workingLabelSize;
        dimLevelPercent = workingDimLevel;
        dimDelaySeconds = workingDimDelay;
        orbitStepSeconds = workingOrbitStep;
        viewMode = viewPopup.indexOfSelectedItem == 1 ? 1 : 0;
        motionActivity = (motionSwitch.state == NSControlStateValueOn);
        activeOnly = (activeOnlySwitch.state == NSControlStateValueOn);
        showBoxes = (boxesSwitch.state == NSControlStateValueOn);
        keepHiddenStreams = (keepStreamsSwitch.state == NSControlStateValueOn);
        hideOffline = (hideOfflineSwitch.state == NSControlStateValueOn);
        return true;
    }
}

} // namespace gig
