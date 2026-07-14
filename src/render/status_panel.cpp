#include "render/status_panel.h"

#include "render/quiet_status.h"

#include <algorithm>
#include <ctime>
#include <string>

#include <imgui.h>

namespace gig {
namespace {

// A centered, size-to-content column inside the full-window panel.
void centerNextText(float panelWidth, const char* text)
{
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX((panelWidth - width) * 0.5f);
    ImGui::TextUnformatted(text);
}

// A centered row of up-to-three buttons; returns which was pressed (0 none).
int centeredButtons(float panelWidth, const char* primary, const char* secondary, const char* tertiary)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    auto widthOf = [&](const char* label) {
        return label ? ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f + 16.0f : 0.0f;
    };
    float total = widthOf(primary);
    if (secondary) {
        total += style.ItemSpacing.x + widthOf(secondary);
    }
    if (tertiary) {
        total += style.ItemSpacing.x + widthOf(tertiary);
    }
    ImGui::SetCursorPosX((panelWidth - total) * 0.5f);

    int pressed = 0;
    if (ImGui::Button(primary, ImVec2(widthOf(primary), 0.0f))) {
        pressed = 1;
    }
    if (secondary) {
        ImGui::SameLine();
        if (ImGui::Button(secondary, ImVec2(widthOf(secondary), 0.0f))) {
            pressed = 2;
        }
    }
    if (tertiary) {
        ImGui::SameLine();
        if (ImGui::Button(tertiary, ImVec2(widthOf(tertiary), 0.0f))) {
            pressed = 3;
        }
    }
    return pressed;
}

} // namespace

StatusPanelAction buildStatusPanel(const OverlayStats& stats, float topOffsetLogical)
{
    StatusPanelAction action;
    if (stats.screen == OverlayStats::StatusScreen::None) {
        // Activity view with an empty grid: draw the wandering "all quiet"
        // liveness line over the bare background (there are no tiles under
        // it). Placement is deterministic per wall-clock minute so it moves
        // once a minute, everywhere, in the same spot on every platform.
        if (!stats.quietStatus.empty()) {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            float fx = 0.0f;
            float fy = 0.0f;
            quietStatusPlacement(static_cast<long long>(std::time(nullptr) / 60), fx, fy);
            const ImVec2 textSize = ImGui::CalcTextSize(stats.quietStatus.c_str());
            const float x = viewport->WorkPos.x
                + std::clamp(viewport->WorkSize.x * fx, 8.0f,
                             std::max(8.0f, viewport->WorkSize.x - textSize.x - 8.0f));
            const float y = viewport->WorkPos.y + topOffsetLogical
                + std::clamp((viewport->WorkSize.y - topOffsetLogical) * fy, 8.0f,
                             std::max(8.0f, viewport->WorkSize.y - topOffsetLogical - textSize.y - 8.0f));
            ImGui::GetBackgroundDrawList()->AddText(
                ImVec2(x, y), IM_COL32(150, 158, 170, 200), stats.quietStatus.c_str());
        }
        return action;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 pos(viewport->WorkPos.x, viewport->WorkPos.y + topOffsetLogical);
    const ImVec2 size(viewport->WorkSize.x, viewport->WorkSize.y - topOffsetLogical);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.016f, 0.020f, 0.028f, 1.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##statuspanel", nullptr, flags)) {
        const float panelWidth = ImGui::GetWindowSize().x;
        const float panelHeight = ImGui::GetWindowSize().y;
        const float lineHeight = ImGui::GetTextLineHeightWithSpacing();

        switch (stats.screen) {
        case OverlayStats::StatusScreen::Welcome: {
            ImGui::SetCursorPosY(panelHeight * 0.32f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.95f, 1.0f, 1.0f));
            centerNextText(panelWidth, "gig");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0.0f, lineHeight * 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.70f, 0.78f, 1.0f));
            centerNextText(panelWidth, "Live multi-camera viewer for Frigate.");
            centerNextText(panelWidth, "Point it at your Frigate server to get started.");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0.0f, lineHeight * 1.5f));
            if (centeredButtons(panelWidth, "Set Up Connection", nullptr, nullptr) == 1) {
                action.openSettings = true;
            }
            break;
        }

        case OverlayStats::StatusScreen::Connecting: {
            ImGui::SetCursorPosY(panelHeight * 0.42f);
            const std::string line = stats.statusHost.empty()
                ? std::string("Connecting...")
                : "Connecting to " + stats.statusHost + "...";
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.70f, 0.78f, 1.0f));
            centerNextText(panelWidth, line.c_str());
            ImGui::PopStyleColor();
            break;
        }

        case OverlayStats::StatusScreen::Error: {
            ImGui::SetCursorPosY(panelHeight * 0.28f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.62f, 0.55f, 1.0f));
            centerNextText(panelWidth, stats.errorIsAuth
                ? "Frigate rejected the login."
                : (stats.errorIsConfig
                    ? "The connection settings need attention."
                    : "Can't reach the Frigate server."));
            ImGui::PopStyleColor();
            if (!stats.statusHost.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.70f, 0.78f, 1.0f));
                centerNextText(panelWidth, stats.statusHost.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0.0f, lineHeight * 0.75f));

            if (!stats.statusDetail.empty()) {
                // Wrapped error detail, centered as a column.
                const float column = panelWidth * 0.7f;
                ImGui::SetCursorPosX((panelWidth - column) * 0.5f);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + column);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.83f, 0.88f, 1.0f));
                ImGui::TextWrapped("%s", stats.statusDetail.c_str());
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
            }

#if defined(__APPLE__)
            // macOS 15+ Local Network privacy: a denied/stale grant surfaces as
            // "No route to host" while ping/ssh to the same host work. Cost an
            // hour once; say it here so it never costs another.
            if (stats.statusDetail.find("No route to host") != std::string::npos) {
                ImGui::Dummy(ImVec2(0.0f, lineHeight * 0.5f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.78f, 0.45f, 1.0f));
                centerNextText(panelWidth,
                    "This can be macOS Local Network privacy: check System Settings ->");
                centerNextText(panelWidth,
                    "Privacy & Security -> Local Network, and toggle gig off/on if already enabled.");
                ImGui::PopStyleColor();
            }
#endif

            if (stats.autoRetryPending) {
                ImGui::Dummy(ImVec2(0.0f, lineHeight * 0.5f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.62f, 0.72f, 1.0f));
                centerNextText(panelWidth, "Retrying automatically...");
                ImGui::PopStyleColor();
            }

            ImGui::Dummy(ImVec2(0.0f, lineHeight * 1.5f));
            // Settings is the fix for config AND auth failures; retry for network.
            const bool settingsPrimary = stats.errorIsConfig || stats.errorIsAuth;
            const int pressed = settingsPrimary
                ? centeredButtons(panelWidth, "Open Settings", "Try Again", "View Log")
                : centeredButtons(panelWidth, "Try Again", "Open Settings", "View Log");
            if (pressed == 1) {
                (settingsPrimary ? action.openSettings : action.retry) = true;
            } else if (pressed == 2) {
                (settingsPrimary ? action.retry : action.openSettings) = true;
            } else if (pressed == 3) {
                action.viewLog = true;
            }
            break;
        }

        case OverlayStats::StatusScreen::None:
            break;
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    return action;
}

} // namespace gig
