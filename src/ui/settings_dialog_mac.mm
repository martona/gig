#include "ui/settings_dialog.h"

#include "app/app_session.h"

#include <algorithm>
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
void showAdvancedDialog(AppConfig& config, bool& showOverlay, int& labelMode)
{
    @autoreleasepool {
        constexpr CGFloat kWidth = 560;
        constexpr CGFloat kRow = 30;
        const CGFloat height = 470;

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
        NSTextField* caField = field(config.tls.caFile, kWidth - 190);
        row();
        label(@"Client cert:");
        NSTextField* certField = field(config.tls.certFile, kWidth - 190);
        row();
        label(@"Client key:");
        NSTextField* keyField = field(config.tls.keyFile, kWidth - 190);
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
        config.streamUrlTemplate = fromField(streamField);
    }
}

} // namespace

bool showSettingsDialog(void* parent, AppConfig& config, bool& showOverlay, int& labelMode,
                        const std::string& statusMessage)
{
    (void)parent; // macOS modal has no owner window to thread through

    @autoreleasepool {
        // Edit a working copy so Cancel (in either window) leaves the caller's config
        // untouched; commit only on the primary OK.
        AppConfig working = config;
        bool workingOverlay = showOverlay;
        int workingLabelMode = labelMode;

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
        controller.onAdvanced = ^{ showAdvancedDialog(*workingPtr, *overlayPtr, *labelPtr); };

        [window center];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        const NSModalResponse response = [NSApp runModalForWindow:window];
        [window orderOut:nil];
        if (response != NSModalResponseOK) {
            return false;
        }

        working.baseUrl = fromField(baseField);
        working.user = fromField(userField);
        working.password = fromField(passField);
        config = working;
        showOverlay = workingOverlay;
        labelMode = workingLabelMode;
        return true;
    }
}

} // namespace gig
