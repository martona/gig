#pragma once

#include "app/app_session.h" // AppConfig

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
bool showSettingsDialog(void* parent, AppConfig& config, bool& showOverlay, int& labelMode,
                        int& dimLevelPercent, int& dimDelaySeconds,
                        bool& forgetRequested, const std::string& statusMessage = {});

} // namespace gig
