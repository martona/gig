#include "ui/settings_dialog.h"

#include "ui/resource.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <optional>
#include <string>

#include <windows.h>
#include <shobjidl.h> // IFileOpenDialog (Common Item Dialog)
#include <umbra.h>

namespace gig {
namespace {

std::wstring utf8ToWide(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
    return wide;
}

std::string wideToUtf8(const wchar_t* text, std::size_t length)
{
    if (length == 0) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

void setDlgTextUtf8(HWND dlg, int id, const std::string& text)
{
    SetDlgItemTextW(dlg, id, utf8ToWide(text).c_str());
}

std::string getDlgTextUtf8(HWND dlg, int id)
{
    HWND control = GetDlgItem(dlg, id);
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, wide.data(), length + 1);
    return wideToUtf8(wide.c_str(), static_cast<std::size_t>(copied));
}

// The Common Item Dialog (IFileOpenDialog), owned to the settings dialog.
std::optional<std::wstring> browseForFile(HWND owner)
{
    const HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool weInitialized = SUCCEEDED(initResult); // S_OK / S_FALSE; not RPC_E_CHANGED_MODE

    std::optional<std::wstring> chosen;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        const COMDLG_FILTERSPEC filters[] = {
            { L"Certificates / keys", L"*.pem;*.crt;*.cer;*.key" },
            { L"All files", L"*.*" },
        };
        dialog->SetFileTypes(2, filters);
        if (SUCCEEDED(dialog->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    chosen = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (weInitialized) {
        CoUninitialize();
    }
    return chosen;
}

struct DialogState {
    AppConfig* config;
    bool* showOverlay;
    int* labelMode; // 0 hide / 1 show-on-error-only / 2 always
    int* dimLevel;  // idle-dim luminance percent (10..100)
    int* dimDelay;  // idle-dim delay seconds (0 = never)
    std::string status;
};

// Idle-dim delay choices (seconds; 0 = Never), shared by both dialogs.
struct DimDelayChoice { int seconds; const wchar_t* label; };
constexpr DimDelayChoice kDimDelays[] = {
    { 0, L"Never" },        { 300, L"5 minutes" },  { 600, L"10 minutes" },
    { 900, L"15 minutes" }, { 1800, L"30 minutes" }, { 3600, L"1 hour" },
    { 7200, L"2 hours" },   { 14400, L"4 hours" },   { 28800, L"8 hours" },
};

// The choice index nearest a delay value (snaps arbitrary stored values to a
// menu entry so the dropdown always shows something sensible).
int dimDelayIndex(int seconds)
{
    int best = 0;
    int bestDiff = std::numeric_limits<int>::max();
    for (int i = 0; i < static_cast<int>(std::size(kDimDelays)); ++i) {
        const int diff = std::abs(kDimDelays[i].seconds - seconds);
        if (diff < bestDiff) {
            bestDiff = diff;
            best = i;
        }
    }
    return best;
}

// Custom dialog result: the user confirmed "Forget..." (wipe settings + restart
// onboarding). TODO(onboarding-project): temporary; remove with IDC_FORGET.
constexpr INT_PTR kDialogResultForget = 100;

// Primary dialog: the base URL + credentials most users touch.
void populatePrimary(HWND dlg, const DialogState& state)
{
    const AppConfig& c = *state.config;
    setDlgTextUtf8(dlg, IDC_BASE, c.baseUrl);
    setDlgTextUtf8(dlg, IDC_USER, c.user);
    setDlgTextUtf8(dlg, IDC_PASSWORD, c.password);
}

void readBackPrimary(HWND dlg, const DialogState& state)
{
    AppConfig& c = *state.config;
    c.baseUrl = getDlgTextUtf8(dlg, IDC_BASE);
    c.user = getDlgTextUtf8(dlg, IDC_USER);
    c.password = getDlgTextUtf8(dlg, IDC_PASSWORD);
}

// Advanced dialog: TLS material, tuning, and the niche connection fields.
void populateAdvanced(HWND dlg, const DialogState& state)
{
    const AppConfig& c = *state.config;
    setDlgTextUtf8(dlg, IDC_CA, c.tls.caFile);
    setDlgTextUtf8(dlg, IDC_CERT, c.tls.certFile);
    setDlgTextUtf8(dlg, IDC_KEY, c.tls.keyFile);
    CheckDlgButton(dlg, IDC_INSECURE, c.tls.verifyServer ? BST_UNCHECKED : BST_CHECKED);
    SetDlgItemInt(dlg, IDC_LOGIN_REFRESH, static_cast<UINT>(c.loginRefreshSeconds), FALSE);
    SetDlgItemInt(dlg, IDC_POLL, static_cast<UINT>(c.pollIntervalSeconds), FALSE);
    CheckDlgButton(dlg, IDC_SOFTWARE, c.softwareDecode ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_OVERLAY, *state.showOverlay ? BST_CHECKED : BST_UNCHECKED);

    HWND labelCombo = GetDlgItem(dlg, IDC_LABELMODE);
    SendMessageW(labelCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(labelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Hide"));
    SendMessageW(labelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Show on error only"));
    SendMessageW(labelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Always show"));
    int sel = state.labelMode ? *state.labelMode : 1;
    if (sel < 0 || sel > 2) {
        sel = 1;
    }
    SendMessageW(labelCombo, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);

    // Screen protection: dim level (%) + delay dropdown.
    SetDlgItemInt(dlg, IDC_DIM_LEVEL, static_cast<UINT>(state.dimLevel ? *state.dimLevel : 60), FALSE);
    HWND dimCombo = GetDlgItem(dlg, IDC_DIM_DELAY);
    SendMessageW(dimCombo, CB_RESETCONTENT, 0, 0);
    for (const DimDelayChoice& choice : kDimDelays) {
        SendMessageW(dimCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label));
    }
    SendMessageW(dimCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(dimDelayIndex(state.dimDelay ? *state.dimDelay : 600)), 0);

    setDlgTextUtf8(dlg, IDC_STREAM_URL, c.streamUrlTemplate);
}

void readBackAdvanced(HWND dlg, const DialogState& state)
{
    AppConfig& c = *state.config;
    c.tls.caFile = getDlgTextUtf8(dlg, IDC_CA);
    c.tls.certFile = getDlgTextUtf8(dlg, IDC_CERT);
    c.tls.keyFile = getDlgTextUtf8(dlg, IDC_KEY);
    c.tls.verifyServer = IsDlgButtonChecked(dlg, IDC_INSECURE) != BST_CHECKED;
    c.loginRefreshSeconds = static_cast<int>(GetDlgItemInt(dlg, IDC_LOGIN_REFRESH, nullptr, FALSE));
    c.pollIntervalSeconds = static_cast<int>(GetDlgItemInt(dlg, IDC_POLL, nullptr, FALSE));
    c.softwareDecode = IsDlgButtonChecked(dlg, IDC_SOFTWARE) == BST_CHECKED;
    *state.showOverlay = IsDlgButtonChecked(dlg, IDC_OVERLAY) == BST_CHECKED;
    if (state.labelMode) {
        const LRESULT sel = SendMessageW(GetDlgItem(dlg, IDC_LABELMODE), CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) {
            *state.labelMode = static_cast<int>(sel);
        }
    }
    if (state.dimLevel) {
        *state.dimLevel = std::clamp(static_cast<int>(GetDlgItemInt(dlg, IDC_DIM_LEVEL, nullptr, FALSE)), 10, 100);
    }
    if (state.dimDelay) {
        const LRESULT sel = SendMessageW(GetDlgItem(dlg, IDC_DIM_DELAY), CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR && sel < static_cast<LRESULT>(std::size(kDimDelays))) {
            *state.dimDelay = kDimDelays[sel].seconds;
        }
    }
    c.streamUrlTemplate = getDlgTextUtf8(dlg, IDC_STREAM_URL);
    // tls.useSystemStore is re-derived from ca/cert/key on reload; rwTimeoutUs
    // is left as-is (not exposed in the dialog).
}

INT_PTR CALLBACK advancedDlgProc(HWND dlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG: {
        auto* state = reinterpret_cast<DialogState*>(lParam);
        SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        umbra::setDarkWndNotifySafe(dlg); // dark title bar + themed controls
        populateAdvanced(dlg, *state);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BROWSE_CA:
            if (const auto path = browseForFile(dlg)) {
                SetDlgItemTextW(dlg, IDC_CA, path->c_str());
            }
            return TRUE;
        case IDC_BROWSE_CERT:
            if (const auto path = browseForFile(dlg)) {
                SetDlgItemTextW(dlg, IDC_CERT, path->c_str());
            }
            return TRUE;
        case IDC_BROWSE_KEY:
            if (const auto path = browseForFile(dlg)) {
                SetDlgItemTextW(dlg, IDC_KEY, path->c_str());
            }
            return TRUE;
        case IDOK: {
            auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
            if (state) {
                readBackAdvanced(dlg, *state);
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        return FALSE;
    default:
        return FALSE;
    }
}

INT_PTR CALLBACK primaryDlgProc(HWND dlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG: {
        auto* state = reinterpret_cast<DialogState*>(lParam);
        SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        umbra::setDarkWndNotifySafe(dlg); // dark title bar + themed controls
        populatePrimary(dlg, *state);
        if (!state->status.empty()) {
            setDlgTextUtf8(dlg, IDC_STATUS, state->status);
        }
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_ADVANCED: {
            auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
            if (state) {
                // Nested modal editing the same working config: its OK writes the
                // advanced fields back, its Cancel leaves them as they were. The
                // primary fields stay live in their controls across the trip.
                DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS_ADVANCED),
                    dlg, advancedDlgProc, reinterpret_cast<LPARAM>(state));
            }
            return TRUE;
        }
        case IDC_FORGET: {
            // TODO(onboarding-project): temporary. Confirm, then close with the
            // forget result; the caller wipes the store and restarts onboarding.
            const int answer = umbra::DarkMessageBox(dlg,
                L"Forget ALL settings?\n\nThis erases the server, credentials, certificate pins "
                L"and window state, and restarts first-run setup.",
                L"gig", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer == IDYES) {
                EndDialog(dlg, kDialogResultForget);
            }
            return TRUE;
        }
        case IDOK: {
            auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
            if (state) {
                readBackPrimary(dlg, *state);
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        return FALSE;
    default:
        return FALSE;
    }
}

} // namespace

bool showSettingsDialog(void* parent, AppConfig& config, bool& showOverlay, int& labelMode,
                        int& dimLevelPercent, int& dimDelaySeconds,
                        bool& forgetRequested, const std::string& statusMessage)
{
    // Edit a working copy so a Cancel in either the primary or the nested advanced
    // dialog leaves the caller's config untouched; commit only on primary OK.
    forgetRequested = false;
    AppConfig working = config;
    bool workingOverlay = showOverlay;
    int workingLabelMode = labelMode;
    int workingDimLevel = dimLevelPercent;
    int workingDimDelay = dimDelaySeconds;
    DialogState state { &working, &workingOverlay, &workingLabelMode,
                        &workingDimLevel, &workingDimDelay, statusMessage };
    const INT_PTR result = DialogBoxParamW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), static_cast<HWND>(parent),
        primaryDlgProc, reinterpret_cast<LPARAM>(&state));
    if (result == kDialogResultForget) {
        forgetRequested = true;
        return false;
    }
    if (result != IDOK) {
        return false;
    }
    config = working;
    showOverlay = workingOverlay;
    labelMode = workingLabelMode;
    dimLevelPercent = workingDimLevel;
    dimDelaySeconds = workingDimDelay;
    return true;
}

} // namespace gig
