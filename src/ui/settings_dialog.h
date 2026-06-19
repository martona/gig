#pragma once

#include "app/app_session.h" // AppConfig

#include <string>
#include <windows.h>

namespace gig {

// Modal native settings dialog (Win32, dark via umbra). Pre-fills from `config`
// + `showOverlay`; on OK writes the edited values back into them and returns
// true. Cancel returns false and leaves both untouched. `statusMessage` (e.g. a
// prior login/connection error) is shown at the bottom; pass empty for none.
// The caller persists to the settings store and applies the result.
bool showSettingsDialog(HWND parent, AppConfig& config, bool& showOverlay,
                        const std::string& statusMessage = {});

} // namespace gig
