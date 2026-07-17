#include "ui/settings_dialog.h"

#include "ui/resource.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <string>

#include <windows.h>
#include <commctrl.h> // trackbar messages (TBM_*)
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

struct DialogState {
    AppConfig* config;
    int* labelMode; // 0 hide / 1 show-on-error-only / 2 always
    int* labelSize; // 0 normal / 1 large (1.5x) / 2 larger (2x)
    int* dimLevel;  // idle-dim luminance percent (10..100)
    int* dimDelay;  // idle-dim delay seconds (0 = never)
    int* orbitStep; // pixel-orbit step seconds (>= 1)
    int* viewMode;  // 0 all cameras / 1 active cameras only
    bool* motionActivity; // raw motion counts as activity (opt-in)
    bool* activeOnly;     // ignore stationary objects (opt-in)
    bool* showBoxes;      // detection-box overlay (default on)
    bool* keepHiddenStreams; // keep off-screen cameras streaming (default on)
    std::string status;
    std::function<void(int)> onDimPreview; // live dim preview while the slider moves
};

// Update the "NN%" label next to the dim slider.
void setDimValueLabel(HWND dlg, int percent)
{
    wchar_t text[16];
    wsprintfW(text, L"%d%%", percent);
    SetDlgItemTextW(dlg, IDC_DIM_LEVEL_VAL, text);
}

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

// Primary dialog: the base URL + credentials, plus the View group (what the
// wall shows day-to-day belongs where the user can reach it).
void populatePrimary(HWND dlg, const DialogState& state)
{
    const AppConfig& c = *state.config;
    setDlgTextUtf8(dlg, IDC_BASE, c.baseUrl);
    setDlgTextUtf8(dlg, IDC_USER, c.user);
    setDlgTextUtf8(dlg, IDC_PASSWORD, c.password);

    HWND viewCombo = GetDlgItem(dlg, IDC_VIEW_MODE);
    SendMessageW(viewCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(viewCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All cameras"));
    SendMessageW(viewCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Active cameras only"));
    const int viewSel = (state.viewMode && *state.viewMode == 1) ? 1 : 0;
    SendMessageW(viewCombo, CB_SETCURSEL, static_cast<WPARAM>(viewSel), 0);
    CheckDlgButton(dlg, IDC_MOTION_ACTIVITY,
                   (state.motionActivity && *state.motionActivity) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_ACTIVE_ONLY,
                   (state.activeOnly && *state.activeOnly) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_SHOW_BOXES,
                   (!state.showBoxes || *state.showBoxes) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_STREAM_HIDDEN,
                   (!state.keepHiddenStreams || *state.keepHiddenStreams) ? BST_CHECKED : BST_UNCHECKED);
}

void readBackPrimary(HWND dlg, const DialogState& state)
{
    AppConfig& c = *state.config;
    c.baseUrl = getDlgTextUtf8(dlg, IDC_BASE);
    c.user = getDlgTextUtf8(dlg, IDC_USER);
    c.password = getDlgTextUtf8(dlg, IDC_PASSWORD);

    if (state.viewMode) {
        const LRESULT sel = SendMessageW(GetDlgItem(dlg, IDC_VIEW_MODE), CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) {
            *state.viewMode = sel == 1 ? 1 : 0;
        }
    }
    if (state.motionActivity) {
        *state.motionActivity = IsDlgButtonChecked(dlg, IDC_MOTION_ACTIVITY) == BST_CHECKED;
    }
    if (state.activeOnly) {
        *state.activeOnly = IsDlgButtonChecked(dlg, IDC_ACTIVE_ONLY) == BST_CHECKED;
    }
    if (state.showBoxes) {
        *state.showBoxes = IsDlgButtonChecked(dlg, IDC_SHOW_BOXES) == BST_CHECKED;
    }
    if (state.keepHiddenStreams) {
        *state.keepHiddenStreams = IsDlgButtonChecked(dlg, IDC_STREAM_HIDDEN) == BST_CHECKED;
    }
}

// Advanced dialog: the security escape hatch, label mode, and burn-in tuning.
// (PEM CA/cert/key, login-refresh, poll-interval, software-decode and the
// stream template lost their UI -- the settings-store keys are still honored,
// they're just registry/defaults-level escape hatches now.)
void populateAdvanced(HWND dlg, const DialogState& state)
{
    const AppConfig& c = *state.config;
    CheckDlgButton(dlg, IDC_INSECURE, c.tls.verifyServer ? BST_UNCHECKED : BST_CHECKED);

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

    HWND sizeCombo = GetDlgItem(dlg, IDC_LABELSIZE);
    SendMessageW(sizeCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(sizeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Normal"));
    SendMessageW(sizeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Large"));
    SendMessageW(sizeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Larger"));
    int sizeSel = state.labelSize ? *state.labelSize : 0;
    if (sizeSel < 0 || sizeSel > 2) {
        sizeSel = 0;
    }
    SendMessageW(sizeCombo, CB_SETCURSEL, static_cast<WPARAM>(sizeSel), 0);

    // Screen protection: dim level (%) slider + delay dropdown + orbit step.
    const int dimLevel = state.dimLevel ? std::clamp(*state.dimLevel, 10, 100) : 60;
    SendDlgItemMessageW(dlg, IDC_DIM_LEVEL, TBM_SETRANGE, TRUE, MAKELPARAM(10, 100));
    SendDlgItemMessageW(dlg, IDC_DIM_LEVEL, TBM_SETPOS, TRUE, dimLevel);
    setDimValueLabel(dlg, dimLevel);
    HWND dimCombo = GetDlgItem(dlg, IDC_DIM_DELAY);
    SendMessageW(dimCombo, CB_RESETCONTENT, 0, 0);
    for (const DimDelayChoice& choice : kDimDelays) {
        SendMessageW(dimCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label));
    }
    SendMessageW(dimCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(dimDelayIndex(state.dimDelay ? *state.dimDelay : 600)), 0);
    SetDlgItemInt(dlg, IDC_ORBIT_STEP, static_cast<UINT>(state.orbitStep ? *state.orbitStep : 40), FALSE);
}

void readBackAdvanced(HWND dlg, const DialogState& state)
{
    AppConfig& c = *state.config;
    c.tls.verifyServer = IsDlgButtonChecked(dlg, IDC_INSECURE) != BST_CHECKED;
    if (state.labelMode) {
        const LRESULT sel = SendMessageW(GetDlgItem(dlg, IDC_LABELMODE), CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) {
            *state.labelMode = static_cast<int>(sel);
        }
    }
    if (state.labelSize) {
        const LRESULT sel = SendMessageW(GetDlgItem(dlg, IDC_LABELSIZE), CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) {
            *state.labelSize = static_cast<int>(sel);
        }
    }
    if (state.dimLevel) {
        *state.dimLevel = std::clamp(
            static_cast<int>(SendDlgItemMessageW(dlg, IDC_DIM_LEVEL, TBM_GETPOS, 0, 0)), 10, 100);
    }
    if (state.dimDelay) {
        const LRESULT sel = SendMessageW(GetDlgItem(dlg, IDC_DIM_DELAY), CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR && sel < static_cast<LRESULT>(std::size(kDimDelays))) {
            *state.dimDelay = kDimDelays[sel].seconds;
        }
    }
    if (state.orbitStep) {
        *state.orbitStep = std::clamp(static_cast<int>(GetDlgItemInt(dlg, IDC_ORBIT_STEP, nullptr, FALSE)), 1, 600);
    }
    // tls.useSystemStore is re-derived on reload; everything without a control
    // here (PEM material, timeouts, tuning) rides through the working config
    // untouched and persists unchanged.
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
    case WM_HSCROLL: {
        // Dim-level trackbar moved: update the "NN%" label and live-preview the
        // dim on the main view behind the dialog.
        if (reinterpret_cast<HWND>(lParam) == GetDlgItem(dlg, IDC_DIM_LEVEL)) {
            const int pos = static_cast<int>(SendDlgItemMessageW(dlg, IDC_DIM_LEVEL, TBM_GETPOS, 0, 0));
            setDimValueLabel(dlg, pos);
            auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
            if (state && state->onDimPreview) {
                state->onDimPreview(pos);
            }
        }
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
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

bool showSettingsDialog(void* parent, AppConfig& config, int& labelMode, int& labelSize,
                        int& dimLevelPercent, int& dimDelaySeconds, int& orbitStepSeconds,
                        int& viewMode, bool& motionActivity, bool& activeOnly,
                        bool& showBoxes, bool& keepHiddenStreams,
                        bool& forgetRequested, const std::string& statusMessage,
                        const std::function<void(int)>& onDimPreview)
{
    // Register the trackbar (slider) window class for the Advanced dialog's dim
    // control. Idempotent + process-wide; comctl32 v6 comes from the app manifest.
    const INITCOMMONCONTROLSEX icc { sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    // Edit a working copy so a Cancel in either the primary or the nested advanced
    // dialog leaves the caller's config untouched; commit only on primary OK.
    forgetRequested = false;
    AppConfig working = config;
    int workingLabelMode = labelMode;
    int workingLabelSize = labelSize;
    int workingDimLevel = dimLevelPercent;
    int workingDimDelay = dimDelaySeconds;
    int workingOrbitStep = orbitStepSeconds;
    int workingViewMode = viewMode;
    bool workingMotionActivity = motionActivity;
    bool workingActiveOnly = activeOnly;
    bool workingShowBoxes = showBoxes;
    bool workingKeepHidden = keepHiddenStreams;
    DialogState state { &working, &workingLabelMode, &workingLabelSize,
                        &workingDimLevel, &workingDimDelay, &workingOrbitStep,
                        &workingViewMode, &workingMotionActivity, &workingActiveOnly,
                        &workingShowBoxes, &workingKeepHidden,
                        statusMessage, onDimPreview };
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
    labelMode = workingLabelMode;
    labelSize = workingLabelSize;
    dimLevelPercent = workingDimLevel;
    dimDelaySeconds = workingDimDelay;
    orbitStepSeconds = workingOrbitStep;
    viewMode = workingViewMode;
    motionActivity = workingMotionActivity;
    activeOnly = workingActiveOnly;
    showBoxes = workingShowBoxes;
    keepHiddenStreams = workingKeepHidden;
    return true;
}

} // namespace gig
