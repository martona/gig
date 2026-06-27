#include "ui/pin_prompt.h"

#include <string>

#include <windows.h>
#include <umbra.h>

namespace gig {
namespace {

// UTF-8 -> UTF-16 for the wide-string Win32/umbra APIs.
std::wstring widen(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
    return wide;
}

} // namespace

bool promptPinDecision(void* parent, const PendingPinDecision& decision)
{
    std::string message;
    if (decision.changed) {
        message = "WARNING: the TLS certificate for " + decision.host + " has CHANGED.\n\n"
            "Previously pinned (SPKI-SHA256):\n  " + decision.previousSpki + "\n"
            "Now presented:\n  " + decision.spki + "\n\n"
            "Subject: " + decision.subject + "\n"
            "Expires: " + decision.notAfter + "\n"
            "Reason:  " + decision.errorText + "\n\n"
            "This can be a normal renewal -- or an interception attempt. "
            "Pin the new certificate and trust it?";
    } else {
        message = "The TLS certificate for " + decision.host + " is not trusted.\n\n"
            "Reason:  " + decision.errorText + "\n"
            "Subject: " + decision.subject + "\n"
            "Expires: " + decision.notAfter + "\n"
            "SPKI-SHA256:\n  " + decision.spki + "\n\n"
            "Pin this certificate and trust it from now on?";
    }
    const UINT icon = decision.changed ? MB_ICONWARNING : MB_ICONQUESTION;
    const int result = umbra::DarkMessageBox(static_cast<HWND>(parent), widen(message).c_str(),
        L"gig - certificate", MB_YESNO | icon | MB_DEFBUTTON2);
    return result == IDYES;
}

} // namespace gig
