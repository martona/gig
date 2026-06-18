#pragma once

#include "app/app_session.h" // AppConfig

#include <windows.h>

namespace gig {

// Modal native settings dialog (Win32, dark via umbra). Pre-fills from `config`
// + `showOverlay`; on OK writes the edited values back into them and returns
// true. Cancel returns false and leaves both untouched. The caller persists to
// the settings store and applies the result (AppSession::applyConfig).
bool showSettingsDialog(HWND parent, AppConfig& config, bool& showOverlay);

} // namespace gig
