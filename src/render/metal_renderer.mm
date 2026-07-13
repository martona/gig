#include "render/grid_layout.h"
#include "render/metal_scene.h"
#include "render/status_panel.h"
#include "render/video_renderer.h"

#include "log.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <imgui.h>
#include <imgui_impl_metal.h>
#include <imgui_impl_sdl3.h>

// macOS renderer shell: the video scene itself (tile upload/draw, signal scope,
// resolve fade, hover, click-to-zoom, grid layout) lives in the shared
// gig::MetalScene (metal_scene.mm, also used by the iOS host); this file owns the
// SDL window/Metal-layer plumbing and all the desktop chrome -- dear imgui (Metal
// backend) toolbar / status banner / log view, camera labels + the diagnostics
// tile via imgui draw lists, SF Symbol toolbar glyphs, HiDPI font rebake, and
// on-demand rendering via isAnimating().

namespace {

constexpr float kToolbarLogicalHeight = 32.0f;
constexpr float kBannerLogicalHeight = 22.0f;
constexpr float kToolbarHideDelay = 2.5f;   // focus view: quick immersive hide
constexpr float kChromeHideDelay = 60.0f;   // grid view: burn-in chromeless mode

class MetalRenderer final : public VideoRenderer {
public:
    bool initialize(SDL_Window* window) override
    {
        window_ = window;

        metalView_ = SDL_Metal_CreateView(window);
        if (!metalView_) {
            gig::logError() << "SDL_Metal_CreateView failed: " << SDL_GetError();
            return false;
        }
        layer_ = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(metalView_);
        device_ = MTLCreateSystemDefaultDevice();
        if (!layer_ || !device_) {
            gig::logError() << "Metal layer/device unavailable";
            return false;
        }
        layer_.device = device_;
        layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
        queue_ = [device_ newCommandQueue];

        scene_ = std::make_unique<gig::MetalScene>();
        if (!scene_->initialize(device_, layer_.pixelFormat)) {
            return false;
        }

        // Initial display scale (pixels per point) for the HiDPI font bake below and
        // the video viewports. Recomputed each render; applyDpiScale() rebakes on a
        // mid-session display-scale change.
        {
            int pointW = 0, pointH = 0, pixelW = 0, pixelH = 0;
            SDL_GetWindowSize(window_, &pointW, &pointH);
            SDL_GetWindowSizeInPixels(window_, &pixelW, &pixelH);
            scale_ = (pointH > 0) ? static_cast<float>(pixelH) / static_cast<float>(pointH) : 1.0f;
        }

        // Toolbar glyphs (SF Symbols -> textures); nil falls back to text labels.
        iconSettings_ = makeSymbolTexture(@"gearshape");
        iconReconnect_ = makeSymbolTexture(@"arrow.clockwise");
        iconLog_ = makeSymbolTexture(@"list.bullet");
        iconFullscreen_ = makeSymbolTexture(@"arrow.up.left.and.arrow.down.right");

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        loadImguiFont();
        applyImguiStyle();
        if (ImGui_ImplSDL3_InitForMetal(window_) && ImGui_ImplMetal_Init(device_)) {
            imguiReady_ = true;
        } else {
            gig::logWarning() << "ImGui init failed; toolbar/log disabled.";
            ImGui::DestroyContext();
        }

        bakedScale_ = scale_; // the font + glyphs above were baked at this scale
        gig::logInfo() << "metal renderer ready: " << device_.name.UTF8String;
        return true;
    }

    ~MetalRenderer() override
    {
        if (imguiReady_) {
            ImGui_ImplMetal_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
        }
        if (metalView_) {
            SDL_Metal_DestroyView(metalView_);
            metalView_ = nullptr;
        }
        // scene_ (tile textures + CV wrappers) releases in its own destructor.
    }

    void resize() override {}

    void render(const std::vector<std::shared_ptr<VideoFrame>>& frames) override
    {
        if (!layer_ || !queue_ || !scene_) {
            return;
        }

        @autoreleasepool {
            int pointWidth = 0, pointHeight = 0, pixelWidth = 0, pixelHeight = 0;
            SDL_GetWindowSize(window_, &pointWidth, &pointHeight);
            SDL_GetWindowSizeInPixels(window_, &pixelWidth, &pixelHeight);
            if (pixelWidth <= 0 || pixelHeight <= 0 || pointWidth <= 0 || pointHeight <= 0) {
                return;
            }
            scale_ = static_cast<float>(pixelHeight) / static_cast<float>(pointHeight);
            layer_.drawableSize = CGSizeMake(pixelWidth, pixelHeight);
            applyDpiScale(); // rebake the font + glyphs if the display scale changed

            id<CAMetalDrawable> drawable = [layer_ nextDrawable];
            if (!drawable) {
                return;
            }
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0.01, 0.01, 0.012, 1.0);
            id<MTLCommandBuffer> commandBuffer = [queue_ commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];

            gig::MetalScene::Params params;
            params.pointWidth = static_cast<float>(pointWidth);
            params.pointHeight = static_cast<float>(pointHeight);
            params.scale = scale_;
            // Chromeless: the grid is full-bleed and the toolbar is an auto-hiding
            // translucent overlay (burn-in) -- no reserved strip anymore.
            params.reservedTopPoints = 0.0f;
            params.extraCell = overlayStats_.showDiagnostics; // trailing diagnostics cell
            // Idle-dim is applied by the imgui pass here (over video + labels +
            // diagnostics, under the toolbar), NOT inside the scene -- otherwise
            // the imgui-drawn labels/diagnostics would stay bright over a dimmed
            // video. So the scene renders undimmed; the shell dims.
            params.dimFactor = 1.0f;
            const gig::MetalScene::Frame scene = scene_->render(encoder, frames, params);

            // Focused-view label anchors to the rect the scene actually drew into
            // (which carries the burn-in orbit offset), not the raw window bounds.
            renderImGui(commandBuffer, encoder, pass, frames, *scene.layout, scene.fullyFocused,
                        scene.contentRect);

            const bool toolbarAnimating = scene_->focusedTile() >= 0 && toolbarIdle_ < kToolbarHideDelay;
            animating_ = scene.animating || toolbarAnimating;

            [encoder endEncoding];
            [commandBuffer presentDrawable:drawable];
            [commandBuffer commit];
        }
    }

    bool isAnimating() const override { return animating_; }

    void setFocusedTile(int index) override { scene_->setFocusedTile(index); }
    int focusedTile() const override { return scene_->focusedTile(); }

    void setCameraLabels(const std::vector<std::string>& labels) override { cameraLabels_ = labels; }
    void setDiagnostics(const OverlayStats& stats) override { overlayStats_ = stats; }
    void setLabelMode(LabelMode mode) override { labelMode_ = mode; }
    void setHoveredTile(int index) override { scene_->setHoveredTile(index); }
    void setTileActivity(const std::vector<std::uint64_t>& byteCounts) override
    {
        scene_->setTileActivity(byteCounts);
    }

    bool handleEvent(const SDL_Event& event) override
    {
        if (!imguiReady_) {
            return false;
        }
        ImGui_ImplSDL3_ProcessEvent(&event);
        const ImGuiIO& io = ImGui::GetIO();
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
            return io.WantCaptureMouse;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
            return io.WantCaptureKeyboard;
        default:
            return false;
        }
    }

    void setLogViewVisible(bool visible) override { logViewVisible_ = visible; }
    bool logViewVisible() const override { return logViewVisible_; }

    ToolbarAction takeToolbarAction() override
    {
        const ToolbarAction action = pendingToolbarAction_;
        pendingToolbarAction_ = ToolbarAction::None;
        return action;
    }

    float reservedTopLogical() const override
    {
        return 0.0f; // chromeless: the toolbar overlays, nothing is reserved
    }

    int hitTestCell(float x, float y) const override
    {
        return scene_->cellAt(x, y); // scene rects include the burn-in orbit
    }

    void setDimFactor(float factor) override { dimFactor_ = factor; }

    bool wantsRepaint() const override { return scene_->wantsOrbitRepaint(); }

private:
    // Rasterize an SF Symbol to a white-on-transparent RGBA texture for the toolbar
    // (the macOS analog of the Windows Segoe MDL2 glyphs). Returns nil if the symbol
    // is unavailable, in which case buildToolbar falls back to text labels.
    id<MTLTexture> makeSymbolTexture(NSString* symbolName)
    {
        if (!device_) {
            return nil;
        }
        NSImage* image = [NSImage imageWithSystemSymbolName:symbolName accessibilityDescription:nil];
        if (!image) {
            return nil;
        }
        NSImageSymbolConfiguration* sizeCfg =
            [NSImageSymbolConfiguration configurationWithPointSize:15.0 weight:NSFontWeightRegular];
        NSImage* configured = [image imageWithSymbolConfiguration:sizeCfg];
        if (!configured) {
            configured = image;
        }
        const NSSize ptSize = configured.size;
        const int w = std::max(1, static_cast<int>(std::ceil(ptSize.width * scale_)));
        const int h = std::max(1, static_cast<int>(std::ceil(ptSize.height * scale_)));

        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:nullptr pixelsWide:w pixelsHigh:h
                        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
                       colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:w * 4 bitsPerPixel:32];
        if (!rep) {
            return nil;
        }
        NSGraphicsContext* gctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = gctx;
        [[NSColor clearColor] set];
        NSRectFill(NSMakeRect(0, 0, w, h));
        [configured drawInRect:NSMakeRect(0, 0, w, h)
                      fromRect:NSZeroRect
                     operation:NSCompositingOperationSourceOver
                      fraction:1.0];
        [gctx flushGraphics];
        [NSGraphicsContext restoreGraphicsState];

        // Force white RGB and keep the rendered alpha as coverage -- straight alpha
        // for ImGui's blend (it tints by the white vertex color), independent of
        // whatever color/premultiplication the symbol drew with.
        unsigned char* bytes = static_cast<unsigned char*>(rep.bitmapData);
        if (bytes) {
            for (int i = 0; i < w * h; ++i) {
                bytes[i * 4 + 0] = 255;
                bytes[i * 4 + 1] = 255;
                bytes[i * 4 + 2] = 255;
            }
        }

        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:static_cast<NSUInteger>(w)
                                                              height:static_cast<NSUInteger>(h)
                                                           mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModeShared;
        id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor];
        [texture replaceRegion:MTLRegionMake2D(0, 0, w, h)
                   mipmapLevel:0
                     withBytes:rep.bitmapData
                   bytesPerRow:static_cast<NSUInteger>(rep.bytesPerRow)];
        return texture;
    }

    std::string labelFor(std::size_t index) const
    {
        return index < cameraLabels_.size() ? cameraLabels_[index] : std::string();
    }

    bool labelVisible(std::size_t index) const
    {
        switch (labelMode_) {
        case LabelMode::Hide: return false;
        case LabelMode::Always: return true;
        case LabelMode::ErrorOnly: return scene_->tileShowingSignal(index);
        }
        return false;
    }

    // ---- ImGui (toolbar / banner / log) + camera labels + diagnostics ----------

    void renderImGui(id<MTLCommandBuffer> commandBuffer, id<MTLRenderCommandEncoder> encoder,
                     MTLRenderPassDescriptor* pass, const std::vector<std::shared_ptr<VideoFrame>>& frames,
                     const gig::GridLayout& layout, bool fullyFocused, const gig::TileRect& fullPts)
    {
        if (!imguiReady_) {
            return;
        }
        ImGui_ImplMetal_NewFrame(pass);
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Camera labels + diagnostics tile via the background draw list (points; over
        // the video, under the toolbar window).
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        const int focusedTile = scene_->focusedTile();
        if (fullyFocused) {
            if (labelVisible(static_cast<std::size_t>(focusedTile))) {
                drawLabel(bg, fullPts, labelFor(static_cast<std::size_t>(focusedTile)), 2.0f);
            }
        } else {
            for (std::size_t i = 0; i < frames.size() && i < layout.tiles.size(); ++i) {
                if (labelVisible(i)) {
                    drawLabel(bg, layout.tiles[i], labelFor(i), 1.0f);
                }
            }
            if (overlayStats_.showDiagnostics && frames.size() < layout.tiles.size()) {
                drawDiagnostics(bg, layout.tiles[frames.size()]);
            }
        }

        // Idle-dim wash on the BACKGROUND draw list: covers video + labels +
        // diagnostics but sits under the toolbar/banner/log/status windows (they
        // are separate imgui windows, always above the background list). alpha =
        // 1 - dim is an exact luminance multiply. (The scene renders undimmed;
        // dimming happens here so imgui text isn't left bright over dimmed video.)
        if (dimFactor_ < 0.999f) {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            bg->AddRectFilled(vp->WorkPos,
                              ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y),
                              IM_COL32(0, 0, 0, static_cast<int>((1.0f - dimFactor_) * 255.0f + 0.5f)));
        }

        if (overlayStats_.screen != OverlayStats::StatusScreen::None) {
            // Full-window welcome/connecting/error panel (no running session). It
            // carries the status message, so the slim banner is suppressed.
            const gig::StatusPanelAction panel =
                gig::buildStatusPanel(overlayStats_, kToolbarLogicalHeight);
            if (panel.openSettings) {
                pendingToolbarAction_ = ToolbarAction::Settings;
            } else if (panel.retry) {
                pendingToolbarAction_ = ToolbarAction::Reconnect;
            } else if (panel.viewLog) {
                logViewVisible_ = true;
            }
        }
        buildToolbar();
        if (overlayStats_.screen == OverlayStats::StatusScreen::None) {
            buildStatusBanner();
        }
        if (logViewVisible_) {
            buildLogWindow();
        }

        ImGui::Render();
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer, encoder);
    }

    void drawLabel(ImDrawList* dl, const gig::TileRect& cellPts, const std::string& label, float sizeScale)
    {
        if (label.empty() || cellPts.width <= 0.0f) {
            return;
        }
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize() * sizeScale;
        const float pad = 4.0f;
        std::string shown = label;
        while (!shown.empty()
               && font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, shown.c_str()).x + 2.0f * pad > cellPts.width) {
            shown.pop_back();
        }
        if (shown.empty()) {
            return;
        }
        const float textW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, shown.c_str()).x;
        const float stripW = std::min(cellPts.width, textW + 2.0f * pad);
        const float stripH = fontSize + 2.0f * pad;
        dl->AddRectFilled(ImVec2(cellPts.x, cellPts.y), ImVec2(cellPts.x + stripW, cellPts.y + stripH),
                          IM_COL32(0, 0, 0, 140));
        dl->AddText(font, fontSize, ImVec2(cellPts.x + pad, cellPts.y + pad), IM_COL32(230, 242, 255, 255),
                    shown.c_str());
    }

    void drawDiagnostics(ImDrawList* dl, const gig::TileRect& cellPts)
    {
        if (cellPts.width <= 0.0f || cellPts.height <= 0.0f) {
            return;
        }
        dl->AddRectFilled(ImVec2(cellPts.x, cellPts.y), ImVec2(cellPts.x + cellPts.width, cellPts.y + cellPts.height),
                          IM_COL32(10, 13, 18, 235));
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize();
        const float pad = 8.0f;
        float y = cellPts.y + pad;
        const float x = cellPts.x + pad;
        const ImU32 heading = IM_COL32(166, 199, 255, 255);
        const ImU32 body = IM_COL32(217, 230, 255, 255);
        const float lh = fontSize + 4.0f;
        char line[128] = {};

        dl->AddText(font, fontSize, ImVec2(x, y), heading, "diagnostics");
        y += lh * 1.4f;
        std::snprintf(line, sizeof(line), "cams good: %d  bad: %d", overlayStats_.camerasOnline, overlayStats_.camerasOffline);
        dl->AddText(font, fontSize, ImVec2(x, y), body, line);
        y += lh;
        std::snprintf(line, sizeof(line), "frames: %d/s, %llu total", static_cast<int>(overlayStats_.fps + 0.5),
                      static_cast<unsigned long long>(overlayStats_.framesTotal));
        dl->AddText(font, fontSize, ImVec2(x, y), body, line);
        y += lh;
        std::snprintf(line, sizeof(line), "bandwidth: %d kbps", overlayStats_.kbps);
        dl->AddText(font, fontSize, ImVec2(x, y), body, line);
        y += lh;
        std::snprintf(line, sizeof(line), "cpu: %.2f%%", overlayStats_.cpuPercent);
        dl->AddText(font, fontSize, ImVec2(x, y), body, line);
    }

    void buildToolbar()
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool active = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || ImGui::IsMouseDown(ImGuiMouseButton_Left);
        toolbarIdle_ = active ? 0.0f : toolbarIdle_ + io.DeltaTime;
        const bool focusView = scene_->focusedTile() >= 0;
        // Chromeless: the grid toolbar also auto-hides (burn-in), just slower than
        // the focus view's immersive hide -- except while a status screen needs
        // its buttons reachable.
        const bool statusScreenUp = overlayStats_.screen != OverlayStats::StatusScreen::None;
        const float hideDelay = focusView ? kToolbarHideDelay : kChromeHideDelay;
        if (!statusScreenUp && toolbarIdle_ >= hideDelay) {
            return;
        }

        const float height = kToolbarLogicalHeight;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
        // Always a translucent overlay now (nothing reserves space beneath it).
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.07f, 0.08f, 0.82f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 0.0f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##toolbar", nullptr, flags)) {
            const float rowHeight = ImGui::GetFrameHeight();
            ImGui::SetCursorPosY((height - rowHeight) * 0.5f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("gig");
            ImGui::SameLine();

            const int online = overlayStats_.camerasOnline;
            const int total = overlayStats_.camerasOnline + overlayStats_.camerasOffline;
            const ImVec4 green(0.40f, 0.85f, 0.40f, 1.0f), amber(0.90f, 0.70f, 0.20f, 1.0f), red(0.90f, 0.35f, 0.35f, 1.0f);
            ImGui::AlignTextToFramePadding();
            if (overlayStats_.screen == OverlayStats::StatusScreen::Welcome) {
                ImGui::TextDisabled("not configured"); // first run: nothing scary
            } else if (overlayStats_.screen == OverlayStats::StatusScreen::Connecting) {
                ImGui::TextDisabled("connecting...");
            } else {
                switch (overlayStats_.link) {
                case OverlayStats::LinkState::Disconnected: ImGui::TextColored(red, "disconnected"); break;
                case OverlayStats::LinkState::Reconnecting: ImGui::TextColored(amber, "reconnecting"); break;
                case OverlayStats::LinkState::Ok: {
                    const ImVec4 c = (total > 0 && online == total) ? green : (online == 0 ? red : amber);
                    ImGui::TextColored(c, "%d/%d live", online, total);
                    break;
                }
                }
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%d fps   %.1f Mbps   cpu %.0f%%", static_cast<int>(overlayStats_.fps + 0.5),
                                overlayStats_.kbps / 1000.0, overlayStats_.cpuPercent);

            // Right-aligned buttons: SF Symbol glyphs when available, else text.
            const ImGuiStyle& style = ImGui::GetStyle();
            const bool icons = iconSettings_ && iconReconnect_ && iconLog_ && iconFullscreen_;
            if (icons) {
                const float ext = ImGui::GetFontSize();
                const ImVec2 isz(ext, ext);
                const float btnW = ext + style.FramePadding.x * 2.0f;
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - (btnW * 4.0f + style.ItemSpacing.x * 3.0f));
                if (ImGui::ImageButton("##settings", (ImTextureID)(uintptr_t)(__bridge void*)iconSettings_, isz)) {
                    pendingToolbarAction_ = ToolbarAction::Settings;
                }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Settings (F2)"); }
                ImGui::SameLine();
                if (ImGui::ImageButton("##reconnect", (ImTextureID)(uintptr_t)(__bridge void*)iconReconnect_, isz)) {
                    pendingToolbarAction_ = ToolbarAction::Reconnect;
                }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Reconnect (F5)"); }
                ImGui::SameLine();
                if (ImGui::ImageButton("##log", (ImTextureID)(uintptr_t)(__bridge void*)iconLog_, isz)) {
                    logViewVisible_ = !logViewVisible_;
                }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Log"); }
                ImGui::SameLine();
                if (ImGui::ImageButton("##fullscreen", (ImTextureID)(uintptr_t)(__bridge void*)iconFullscreen_, isz)) {
                    pendingToolbarAction_ = ToolbarAction::ToggleFullscreen;
                }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Fullscreen (F11)"); }
            } else {
                const auto buttonWidth = [&](const char* label) { return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f; };
                const float buttonsWidth = buttonWidth("Settings") + buttonWidth("Reconnect") + buttonWidth("Log")
                    + buttonWidth("Fullscreen") + style.ItemSpacing.x * 3.0f;
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - buttonsWidth);
                if (ImGui::Button("Settings")) { pendingToolbarAction_ = ToolbarAction::Settings; }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Settings (F2)"); }
                ImGui::SameLine();
                if (ImGui::Button("Reconnect")) { pendingToolbarAction_ = ToolbarAction::Reconnect; }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Reconnect (F5)"); }
                ImGui::SameLine();
                if (ImGui::Button("Log")) { logViewVisible_ = !logViewVisible_; }
                ImGui::SameLine();
                if (ImGui::Button("Fullscreen")) { pendingToolbarAction_ = ToolbarAction::ToggleFullscreen; }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Fullscreen (F11)"); }
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }

    void buildStatusBanner()
    {
        if (scene_->focusedTile() >= 0) {
            return;
        }
        const OverlayStats& s = overlayStats_;
        if (s.link == OverlayStats::LinkState::Ok && !s.healthDegraded) {
            return;
        }
        const std::string host = s.statusHost.empty() ? std::string("Frigate") : s.statusHost;
        ImVec4 background;
        std::string message;
        if (s.link == OverlayStats::LinkState::Disconnected) {
            background = ImVec4(0.42f, 0.10f, 0.10f, 0.94f);
            message = "Not connected to " + host + "  --  press Reconnect (F5)";
            if (!s.statusDetail.empty()) {
                message += "   [" + s.statusDetail + "]";
            }
        } else if (s.link == OverlayStats::LinkState::Reconnecting) {
            background = ImVec4(0.42f, 0.28f, 0.05f, 0.94f);
            message = "Lost contact with " + host + "  --  reconnecting";
            if (s.secondsSinceData > 0) {
                message += " (last data " + std::to_string(s.secondsSinceData) + "s ago)";
            }
        } else {
            background = ImVec4(0.42f, 0.28f, 0.05f, 0.94f);
            message = "Camera health unreadable  --  go2rtc stream schema may have changed";
        }

        const float height = kBannerLogicalHeight;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + kToolbarLogicalHeight));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, background);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.95f, 0.92f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0.0f, 0.0f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus
            | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##statusbanner", nullptr, flags)) {
            ImGui::SetCursorPosY((height - ImGui::GetFrameHeight()) * 0.5f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(message.c_str());
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    void buildLogWindow()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        bool open = true;
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("Log", &open, flags)) {
            gig::LogBuffer::instance().snapshot(logScratch_);
            if (ImGui::Button("Copy")) {
                std::string joined;
                for (const std::string& entry : logScratch_) { joined += entry; joined.push_back('\n'); }
                ImGui::SetClipboardText(joined.c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear")) { gig::LogBuffer::instance().clear(); }
            ImGui::SameLine();
            ImGui::TextDisabled("wheel / drag to scroll, Esc or X to close");
            ImGui::Separator();
            ImGui::BeginChild("log_scroll", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(logScratch_.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    ImGui::TextUnformatted(logScratch_[static_cast<std::size_t>(i)].c_str());
                }
            }
            clipper.End();
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
        }
        ImGui::End();
        if (!open) {
            logViewVisible_ = false;
        }
    }

    void loadImguiFont()
    {
        // HiDPI-sharp: bake the atlas at device pixels (14pt * scale) and divide back
        // with FontGlobalScale, so the displayed size stays 14pt but glyphs are crisp
        // on Retina. (imgui_impl_sdl3 sets DisplayFramebufferScale; ImGui works in points.)
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        const float pixelSize = 14.0f * scale_;
        if (io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Menlo.ttc", pixelSize) == nullptr) {
            ImFontConfig cfg;
            cfg.SizePixels = 13.0f * scale_;
            io.Fonts->AddFontDefault(&cfg);
        }
        io.FontGlobalScale = (scale_ > 0.0f) ? 1.0f / scale_ : 1.0f;
    }

    void applyImguiStyle()
    {
        // No ScaleAllSizes here (unlike the D3D11 path): on macOS ImGui is in points and
        // imgui_impl_sdl3's DisplayFramebufferScale already handles the pixel scaling.
        ImGui::GetStyle() = ImGuiStyle();
        ImGui::StyleColorsDark();
    }

    // Rebake the imgui font atlas + SF Symbol glyphs when the display scale changes
    // (e.g. the window moved to a different-DPI monitor). scale_ is recomputed each
    // render() from the backing size; bakedScale_ tracks what those were baked at. The
    // Windows analog runs from SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED; here render()
    // already recomputes scale_, so a self-detecting compare is enough.
    void applyDpiScale()
    {
        if (scale_ == bakedScale_) {
            return;
        }
        bakedScale_ = scale_;
        gig::logInfo() << "display scale changed: " << scale_;

        // SF Symbol toolbar glyphs are rasterized at device px; re-rasterize them.
        iconSettings_ = makeSymbolTexture(@"gearshape");
        iconReconnect_ = makeSymbolTexture(@"arrow.clockwise");
        iconLog_ = makeSymbolTexture(@"list.bullet");

        if (imguiReady_) {
            // imgui (1.92+) owns font-texture lifecycle: rebuilding the atlas
            // (loadImguiFont) marks it dirty and ImGui_ImplMetal_RenderDrawData
            // re-uploads it on the next frame -- no manual texture create/destroy.
            loadImguiFont();
            applyImguiStyle();
        }
    }

    SDL_Window* window_ = nullptr;
    SDL_MetalView metalView_ = nullptr;
    CAMetalLayer* layer_ = nil;
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLTexture> iconSettings_ = nil;
    id<MTLTexture> iconReconnect_ = nil;
    id<MTLTexture> iconLog_ = nil;
    id<MTLTexture> iconFullscreen_ = nil;
    float dimFactor_ = 1.0f;

    std::unique_ptr<gig::MetalScene> scene_;

    std::vector<std::string> cameraLabels_;
    OverlayStats overlayStats_;
    LabelMode labelMode_ = LabelMode::ErrorOnly;

    float scale_ = 1.0f;
    float bakedScale_ = 0.0f; // display scale the imgui font + SF Symbol glyphs were baked at
    bool animating_ = true;

    bool imguiReady_ = false;
    bool logViewVisible_ = false;
    std::vector<std::string> logScratch_;
    ToolbarAction pendingToolbarAction_ = ToolbarAction::None;
    float toolbarIdle_ = 0.0f;
};

} // namespace

std::unique_ptr<VideoRenderer> createRenderer()
{
    return std::make_unique<MetalRenderer>();
}
