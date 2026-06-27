#pragma once

#include "net/cert_pin.hpp" // PendingPinDecision

namespace gig {

// Modal trust prompt for an untrusted / changed server certificate (Win32 dark
// message box on Windows; AppKit NSAlert on macOS). Returns true to pin + trust it.
// `parent` is the owner window (HWND on Windows; ignored on macOS). The safe option
// ("Don't Trust" / No) is the default. Loud about a *changed* pin.
bool promptPinDecision(void* parent, const PendingPinDecision& decision);

} // namespace gig
