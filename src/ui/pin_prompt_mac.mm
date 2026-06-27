#include "ui/pin_prompt.h"

#include <string>

#import <Cocoa/Cocoa.h>

namespace gig {
namespace {

NSString* toNs(const std::string& s)
{
    NSString* r = [NSString stringWithUTF8String:s.c_str()];
    return r ? r : @"";
}

} // namespace

bool promptPinDecision(void* parent, const PendingPinDecision& decision)
{
    (void)parent; // the alert is app-modal; no owner window needed

    @autoreleasepool {
        NSAlert* alert = [[NSAlert alloc] init];
        if (decision.changed) {
            alert.alertStyle = NSAlertStyleCritical;
            alert.messageText = [NSString stringWithFormat:@"The certificate for %@ has CHANGED", toNs(decision.host)];
            alert.informativeText = [NSString stringWithFormat:
                @"Previously pinned (SPKI-SHA256):\n  %@\nNow presented:\n  %@\n\n"
                 "Subject: %@\nExpires: %@\nReason: %@\n\n"
                 "This can be a normal renewal — or an interception attempt. "
                 "Pin the new certificate and trust it?",
                toNs(decision.previousSpki), toNs(decision.spki), toNs(decision.subject),
                toNs(decision.notAfter), toNs(decision.errorText)];
        } else {
            alert.alertStyle = NSAlertStyleWarning;
            alert.messageText = [NSString stringWithFormat:@"The certificate for %@ is not trusted", toNs(decision.host)];
            alert.informativeText = [NSString stringWithFormat:
                @"Reason: %@\nSubject: %@\nExpires: %@\nSPKI-SHA256:\n  %@\n\n"
                 "Pin this certificate and trust it from now on?",
                toNs(decision.errorText), toNs(decision.subject), toNs(decision.notAfter), toNs(decision.spki)];
        }
        // First button is the default (Enter) -- make it the safe choice.
        [alert addButtonWithTitle:@"Don't Trust"];
        [alert addButtonWithTitle:@"Trust"];
        [NSApp activateIgnoringOtherApps:YES];
        const NSModalResponse response = [alert runModal];
        return response == NSAlertSecondButtonReturn; // "Trust"
    }
}

} // namespace gig
