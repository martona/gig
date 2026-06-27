#include "ui/app_menu.h"

#ifdef __APPLE__

#include <SDL3/SDL.h>

#import <AppKit/AppKit.h>

// Target for the Preferences/Quit menu items. Both just push an SDL event so the
// existing run loop handles them on its own thread, exactly like F2 (settings) and a
// window close (graceful shutdown) already do -- no cross-thread UI from the menu.
@interface GigMenuTarget : NSObject
@property (nonatomic) Uint32 prefsEventType;
@end

@implementation GigMenuTarget
- (void)openPreferences:(id)sender
{
    (void)sender;
    if (self.prefsEventType == 0) {
        return;
    }
    SDL_Event event;
    SDL_zero(event);
    event.type = self.prefsEventType;
    SDL_PushEvent(&event);
}

- (void)quit:(id)sender
{
    (void)sender;
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
}
@end

Uint32 installAppMenu()
{
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        if (!app) {
            return 0;
        }

        Uint32 prefsEvent = SDL_RegisterEvents(1);
        if (prefsEvent == (Uint32)-1) {
            prefsEvent = 0;
        }

        // Retained for the app's lifetime: NSMenuItem.target is a weak reference, so
        // the target must outlive the menu on its own.
        static GigMenuTarget* target = nil;
        target = [[GigMenuTarget alloc] init];
        target.prefsEventType = prefsEvent;

        NSMenu* menubar = [[NSMenu alloc] init];
        NSMenuItem* appItem = [[NSMenuItem alloc] init];
        [menubar addItem:appItem];

        NSMenu* appMenu = [[NSMenu alloc] init];
        NSString* name = @"gig";

        // About -> the standard panel (nil target routes up the responder chain to NSApp).
        [appMenu addItemWithTitle:[@"About " stringByAppendingString:name]
                           action:@selector(orderFrontStandardAboutPanel:)
                    keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];

        NSMenuItem* prefs = [[NSMenuItem alloc] initWithTitle:@"Preferences…"
                                                       action:@selector(openPreferences:)
                                                keyEquivalent:@","];
        prefs.target = target;
        [appMenu addItem:prefs];
        [appMenu addItem:[NSMenuItem separatorItem]];

        NSMenuItem* quit = [[NSMenuItem alloc] initWithTitle:[@"Quit " stringByAppendingString:name]
                                                      action:@selector(quit:)
                                               keyEquivalent:@"q"];
        quit.target = target;
        [appMenu addItem:quit];

        appItem.submenu = appMenu;
        [app setMainMenu:menubar];

        return prefsEvent;
    }
}

#endif // __APPLE__
