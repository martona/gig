#pragma once

#include "app/app_session.h" // AppConfig

#include <functional>
#include <string>

namespace gig {

// Modal native settings dialog (Win32 dark dialog on Windows; AppKit window on
// macOS). Pre-fills from `config`, `showOverlay`, and `labelMode` (0 hide / 1
// show-on-error-only / 2 always); on OK writes the edited values back into them and
// returns true. Cancel returns false and leaves them untouched. `parent` is the
// owner window (HWND on Windows; ignored on macOS). `statusMessage` (e.g. a prior
// login/connection error) is shown to the user; pass empty for none. The caller
// persists to the settings store and applies the result.
//
// `forgetRequested` (TODO(onboarding-project): temporary) is set when the user
// confirmed the "Forget..." button: the dialog closes returning false (no values
// to save) and the caller wipes the settings store and restarts onboarding.
// `onDimPreview` (optional) is invoked live while the idle-dim slider moves, with
// the previewed luminance percent, so the caller can apply it to the main view
// behind the modal dialog. It is transient -- the caller restores/re-derives the
// dim state after the dialog closes (Cancel discards it like any other edit).
// `viewMode` is 0 = show all cameras, 1 = show active cameras only (tiles
// driven by Frigate's activity feed); `motionActivity` opts raw motion in as
// an activity trigger (tracked objects always count); `keepHiddenStreams`
// keeps off-screen cameras' streams connected (off = tear down + reconnect on
// demand, saving power at the cost of a 1-2s wake).
bool showSettingsDialog(void* parent, AppConfig& config, bool& showOverlay, int& labelMode,
                        int& dimLevelPercent, int& dimDelaySeconds, int& orbitStepSeconds,
                        int& viewMode, bool& motionActivity, bool& keepHiddenStreams,
                        bool& forgetRequested, const std::string& statusMessage = {},
                        const std::function<void(int dimPercent)>& onDimPreview = {});

} // namespace gig
