#pragma once

#ifdef __APPLE__

#include <SDL3/SDL.h>

// Installs the macOS application menu (About / Preferences <Cmd-,> / Quit <Cmd-Q>).
// Call once after SDL_Init, on the main thread (NSApp must exist). Returns the SDL
// event type pushed when Preferences is chosen -- poll for it in the run loop and
// open the settings dialog; returns 0 if the menu/event could not be set up. Quit
// pushes SDL_EVENT_QUIT so shutdown goes through the normal graceful path.
Uint32 installAppMenu();

#endif // __APPLE__
