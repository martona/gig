#pragma once

#include "app/app_session.h" // AppConfig

#include <string>
#include <windows.h>

namespace gig {

// Modal native settings dialog (Win32, dark via umbra). Pre-fills from `config`,
// `showOverlay`, and `labelMode` (0 hide / 1 show-on-error-only / 2 always); on
// OK writes the edited values back into them and returns true. Cancel returns
// false and leaves them untouched. `statusMessage` (e.g. a prior login/connection
// error) is shown at the bottom; pass empty for none. The caller persists to the
// settings store and applies the result.
bool showSettingsDialog(HWND parent, AppConfig& config, bool& showOverlay, int& labelMode,
                        const std::string& statusMessage = {});

} // namespace gig
