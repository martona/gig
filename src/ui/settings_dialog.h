#pragma once

#include "app/connections.h" // ConnectionInfo

#include <functional>
#include <string>
#include <vector>

namespace gig {

// Modal native settings dialog (Win32 dark dialog on Windows; AppKit window on
// macOS). `parent` is the owner window (HWND on Windows; ignored on macOS).
// `statusMessage` (e.g. a prior login/connection error) is shown to the user;
// pass empty for none.
//
// The Connection group edits a STAGED copy of the whole multi-server registry:
// `connections` + `activeIndex` (which entry the app should connect to; the
// list selection on OK). Add/Edit/Delete only mutate the staged list; OK
// returns true with the edited list/index written back -- the caller commits
// via connections::applyStaged, persists the globals, and reconnects. Cancel
// returns false and leaves everything untouched, adds/edits/deletes included.
// Per-connection fields (URL, user, password, insecure) are edited in a
// sub-dialog; an entry without a URL, or a duplicate of another entry's URL,
// is rejected there.
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
// an activity trigger (tracked objects always count); `activeOnly` ignores
// STATIONARY objects (a parked car stops counting ~10s after it parks);
// `keepHiddenStreams` keeps off-screen cameras' streams connected (off = tear
// down + reconnect on demand, saving power at the cost of a 1-2s wake);
// `hideOffline` drops cameras with no incoming video from the wall entirely
// (a wandering status line appears when every camera is down).
bool showSettingsDialog(void* parent, std::vector<ConnectionInfo>& connections, int& activeIndex,
                        int& labelMode, int& labelSize,
                        int& dimLevelPercent, int& dimDelaySeconds, int& orbitStepSeconds,
                        int& viewMode, bool& motionActivity, bool& activeOnly,
                        bool& showBoxes, bool& keepHiddenStreams, bool& hideOffline,
                        bool& forgetRequested, const std::string& statusMessage = {},
                        const std::function<void(int dimPercent)>& onDimPreview = {});

} // namespace gig
