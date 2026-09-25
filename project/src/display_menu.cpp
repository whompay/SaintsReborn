// Display settings overlay: lists the monitor's display modes and applies
// them live with a 30-second confirm / auto-revert countdown.
//
// Uses the Win32 display settings API directly: the SDL window lives in
// rexruntime.dll's own statically-linked SDL instance, so exe-side SDL calls
// cannot see it. Window state changes still go through rex::ui::Window, and
// the resulting WM_SIZE / WM_DISPLAYCHANGE events reach SDL and the presenter
// through the normal event path.

#include "display_menu.h"

#include <algorithm>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "kbm.h"
#include "fps_overlay.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Persisted window mode: "windowed", "borderless" or "fullscreen".
REXCVAR_DEFINE_STRING(window_mode, "fullscreen", "UI/Window",
                      "Startup window mode: windowed, borderless or fullscreen");

// Show the FPS counter at startup (the F1 overlay).
REXCVAR_DEFINE_BOOL(fps_counter, false, "UI/Window", "Show the FPS counter overlay at startup");

namespace {
constexpr double kConfirmSeconds = 30.0;

const char* kWindowModeNames[] = {"Windowed", "Borderless Fullscreen", "Fullscreen"};
const char* kWindowModeValues[] = {"windowed", "borderless", "fullscreen"};

// Win32 display device name (e.g. \\.\DISPLAY1) for the monitor showing hwnd.
std::string DeviceNameForWindow(HWND hwnd) {
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXA info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoA(monitor, &info)) {
        return {};
    }
    return info.szDevice;
}

DEVMODEA DesktopModeForWindow(HWND hwnd) {
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    std::string device = DeviceNameForWindow(hwnd);
    if (!device.empty()) {
        EnumDisplaySettingsA(device.c_str(), ENUM_CURRENT_SETTINGS, &dm);
    }
    return dm;
}

// Temporarily switches the desktop display mode. Reverted by Windows when the
// process exits (CDS_FULLSCREEN semantics).
bool SetDesktopMode(const std::string& device, int width, int height, int refresh) {
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsA(device.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
        return false;
    }
    if (static_cast<int>(dm.dmPelsWidth) == width &&
        static_cast<int>(dm.dmPelsHeight) == height) {
        return true;  // already there
    }
    dm.dmPelsWidth = static_cast<DWORD>(width);
    dm.dmPelsHeight = static_cast<DWORD>(height);
    if (refresh > 1) {  // 0/1 mean "default"
        dm.dmDisplayFrequency = static_cast<DWORD>(refresh);
    }
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | (refresh > 1 ? DM_DISPLAYFREQUENCY : 0);
    LONG result = ChangeDisplaySettingsExA(device.c_str(), &dm, nullptr, CDS_FULLSCREEN, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        REXLOG_ERROR("Display menu: ChangeDisplaySettingsEx failed ({}x{}@{}): {}", width, height,
                     refresh, static_cast<int>(result));
        return false;
    }
    return true;
}

void RestoreDesktopMode(const std::string& device) {
    // Passing a null devmode restores the registry (persistent) mode.
    ChangeDisplaySettingsExA(device.c_str(), nullptr, nullptr, 0, nullptr);
}

void ResizeWindow(HWND hwnd, int width, int height) {
    RECT rect{0, 0, width, height};
    AdjustWindowRect(&rect, static_cast<DWORD>(GetWindowLongPtrA(hwnd, GWL_STYLE)), FALSE);
    SetWindowPos(hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Display mode changes can wedge XInput 1.4's per-process focus tracking,
// which mutes controller input until restart. XInputEnable(TRUE) re-arms it;
// SetForegroundWindow repairs the activation state it tracks.
void RestoreControllerInput(HWND hwnd) {
    HMODULE xinput = GetModuleHandleA("xinput1_4.dll");
    if (!xinput) {
        xinput = LoadLibraryA("xinput1_4.dll");
    }
    if (xinput) {
        auto enable = reinterpret_cast<void(WINAPI*)(BOOL)>(GetProcAddress(xinput, "XInputEnable"));
        if (enable) {
            enable(TRUE);
        }
    }
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
}
}  // namespace

void ApplyFullscreenDisplayMode(rex::ui::Window* window, int width, int height) {
    HWND hwnd = static_cast<HWND>(window->GetNativeWindowHandle());
    if (!hwnd || width <= 0 || height <= 0) {
        return;
    }
    std::string device = DeviceNameForWindow(hwnd);
    if (!device.empty()) {
        SetDesktopMode(device, width, height, 0);
    }
}

DisplaySettingsDialog::DisplaySettingsDialog(rex::ui::ImGuiDrawer* imgui_drawer,
                                             rex::ui::Window* window,
                                             std::filesystem::path config_path,
                                             sr::FpsOverlay* fps_overlay,
                                             std::function<void(bool)> set_fps_visible)
    : ImGuiDialog(imgui_drawer),
      window_(window),
      config_path_(std::move(config_path)),
      fps_overlay_(fps_overlay),
      set_fps_visible_(std::move(set_fps_visible)) {
    std::string wm = REXCVAR_GET(window_mode);
    for (int i = 0; i < 3; ++i) {
        if (wm == kWindowModeValues[i]) {
            window_mode_ = i;
            break;
        }
    }
    aspect_mode_ = rex::cvar::GetFlagByName("present_aspect_mode");
    aspect_ratio_ = rex::cvar::GetFlagByName("present_aspect_ratio");
    try {
        render_scale_ = std::clamp(std::stoi(rex::cvar::GetFlagByName("draw_resolution_scale_x")) - 1,
                                   0, 3);
    } catch (...) {
        render_scale_ = 1;  // 2x
    }
    if (HWND hwnd = static_cast<HWND>(window_->GetNativeWindowHandle())) {
        device_name_ = DeviceNameForWindow(hwnd);
    }
    // The game hides the cursor and its mouselook pins it to the window
    // center; show it and free it while the menu is open.
    window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    sr::SetMouseCaptureSuspended(true);
    EnumerateModes();
}

DisplaySettingsDialog::~DisplaySettingsDialog() {
    window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    sr::SetMouseCaptureSuspended(false);
}

void DisplaySettingsDialog::EnumerateModes() {
    modes_.clear();
    selected_ = -1;

    if (device_name_.empty()) {
        REXLOG_ERROR("Display menu: no display device found for window");
        return;
    }

    DEVMODEA current = DesktopModeForWindow(
        static_cast<HWND>(window_->GetNativeWindowHandle()));

    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(device_name_.c_str(), i, &dm); ++i) {
        if (dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) {
            continue;
        }
        // Dedupe by WxH, keeping the highest refresh rate for each size.
        auto it = std::find_if(modes_.begin(), modes_.end(), [&](const Mode& e) {
            return e.width == static_cast<int>(dm.dmPelsWidth) &&
                   e.height == static_cast<int>(dm.dmPelsHeight);
        });
        if (it != modes_.end()) {
            it->refresh = std::max(it->refresh, static_cast<int>(dm.dmDisplayFrequency));
            continue;
        }
        Mode entry;
        entry.width = static_cast<int>(dm.dmPelsWidth);
        entry.height = static_cast<int>(dm.dmPelsHeight);
        entry.refresh = static_cast<int>(dm.dmDisplayFrequency);
        entry.label = std::to_string(entry.width) + " x " + std::to_string(entry.height);
        if (entry.refresh > 1) {  // 0/1 mean "default"
            entry.label += " @ " + std::to_string(entry.refresh) + " Hz";
        }
        modes_.push_back(std::move(entry));
    }

    std::sort(modes_.begin(), modes_.end(), [](const Mode& a, const Mode& b) {
        return a.width * a.height > b.width * b.height;
    });

    // Pre-select the mode matching the current desktop resolution.
    for (size_t i = 0; i < modes_.size(); ++i) {
        if (modes_[i].width == static_cast<int>(current.dmPelsWidth) &&
            modes_[i].height == static_cast<int>(current.dmPelsHeight)) {
            selected_ = static_cast<int>(i);
            break;
        }
    }
    if (modes_.empty()) {
        REXLOG_ERROR("Display menu: EnumDisplaySettings returned no modes");
    }
}

void DisplaySettingsDialog::RestoreDesktopModeForSnapshot(const Snapshot& snapshot) {
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = static_cast<DWORD>(snapshot.desktop_width);
    dm.dmPelsHeight = static_cast<DWORD>(snapshot.desktop_height);
    dm.dmDisplayFrequency = static_cast<DWORD>(snapshot.desktop_refresh);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    ChangeDisplaySettingsExA(device_name_.c_str(), &dm, nullptr, CDS_FULLSCREEN, nullptr);
    desktop_mode_changed_ = false;
}

void DisplaySettingsDialog::SetMode(int width, int height, int refresh, WindowMode mode) {
    HWND hwnd = static_cast<HWND>(window_->GetNativeWindowHandle());
    if (!hwnd) {
        REXLOG_ERROR("Display menu: no native window handle");
        return;
    }
    switch (mode) {
        case WindowMode::kFullscreen:
            if (SetDesktopMode(device_name_, width, height, refresh)) {
                desktop_mode_changed_ = true;
            }
            window_->SetFullscreen(true);
            break;
        case WindowMode::kBorderless:
            if (desktop_mode_changed_) {
                RestoreDesktopMode(device_name_);
                desktop_mode_changed_ = false;
            }
            window_->SetFullscreen(true);
            break;
        case WindowMode::kWindowed:
            if (desktop_mode_changed_) {
                RestoreDesktopMode(device_name_);
                desktop_mode_changed_ = false;
            }
            window_->SetFullscreen(false);
            ResizeWindow(hwnd, width, height);
            break;
    }
    RestoreControllerInput(hwnd);
}

void DisplaySettingsDialog::ApplySelection() {
    if (selected_ < 0 || selected_ >= static_cast<int>(modes_.size())) {
        return;
    }
    if (!pending_) {
        snapshot_.window_mode = window_->IsFullscreen() ? WindowMode::kFullscreen
                                                        : WindowMode::kWindowed;
        // Distinguish borderless from fullscreen in the snapshot: if we never
        // changed the desktop mode, fullscreen meant borderless.
        if (snapshot_.window_mode == WindowMode::kFullscreen && !desktop_mode_changed_) {
            snapshot_.window_mode = WindowMode::kBorderless;
        }
        if (HWND hwnd = static_cast<HWND>(window_->GetNativeWindowHandle())) {
            RECT rect{};
            GetWindowRect(hwnd, &rect);
            snapshot_.width = rect.right - rect.left;
            snapshot_.height = rect.bottom - rect.top;
            DEVMODEA desktop = DesktopModeForWindow(hwnd);
            snapshot_.desktop_width = static_cast<int>(desktop.dmPelsWidth);
            snapshot_.desktop_height = static_cast<int>(desktop.dmPelsHeight);
            snapshot_.desktop_refresh = static_cast<int>(desktop.dmDisplayFrequency);
        }
    }
    const Mode& m = modes_[selected_];
    const WindowMode mode = static_cast<WindowMode>(window_mode_);
    REXLOG_INFO("Display menu: applying {}x{}@{} mode={}", m.width, m.height, m.refresh,
                kWindowModeValues[window_mode_]);
    SetMode(m.width, m.height, m.refresh, mode);
    pending_ = true;
    deadline_ = ImGui::GetTime() + kConfirmSeconds;
}

void DisplaySettingsDialog::RevertPending() {
    REXLOG_INFO("Display menu: reverting to {}x{} mode={}", snapshot_.desktop_width,
                snapshot_.desktop_height, static_cast<int>(snapshot_.window_mode));
    HWND hwnd = static_cast<HWND>(window_->GetNativeWindowHandle());
    if (snapshot_.window_mode == WindowMode::kFullscreen) {
        RestoreDesktopModeForSnapshot(snapshot_);
        window_->SetFullscreen(true);
    } else {
        if (desktop_mode_changed_) {
            RestoreDesktopMode(device_name_);
            desktop_mode_changed_ = false;
        }
        if (snapshot_.window_mode == WindowMode::kBorderless) {
            window_->SetFullscreen(true);
        } else {
            window_->SetFullscreen(false);
            if (hwnd && snapshot_.width > 0) {
                ResizeWindow(hwnd, snapshot_.width, snapshot_.height);
            }
        }
    }
    if (hwnd) {
        RestoreControllerInput(hwnd);
    }
    pending_ = false;
}

void DisplaySettingsDialog::CommitPending() {
    pending_ = false;
    rex::cvar::SetFlagByName("window_mode", kWindowModeValues[window_mode_]);
    rex::cvar::SetFlagByName("fullscreen", window_mode_ != 0 ? "true" : "false");
    if (selected_ >= 0) {
        rex::cvar::SetFlagByName("window_width", std::to_string(modes_[selected_].width));
        rex::cvar::SetFlagByName("window_height", std::to_string(modes_[selected_].height));
    }
    rex::cvar::SaveConfig(config_path_);
    REXLOG_INFO("Display menu: settings saved to {}", config_path_.string());
}

void DisplaySettingsDialog::OnDraw(ImGuiIO& io) {
    (void)io;

    // Re-enumerate if the window moved to another display.
    if (HWND hwnd = static_cast<HWND>(window_->GetNativeWindowHandle())) {
        std::string device = DeviceNameForWindow(hwnd);
        if (!device.empty() && device != device_name_) {
            device_name_ = std::move(device);
            EnumerateModes();
        }
    }

    ImGui::SetNextWindowSize(ImVec2(420, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Display Settings##sr", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (modes_.empty()) {
        ImGui::TextUnformatted("No display modes found.");
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Window mode:");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##windowmode", &window_mode_, kWindowModeNames, 3);
    if (window_mode_ == static_cast<int>(WindowMode::kBorderless)) {
        ImGui::TextDisabled("Borderless uses the desktop resolution.");
    }

    ImGui::Text("Resolution (%zu modes):", modes_.size());
    if (window_mode_ == static_cast<int>(WindowMode::kBorderless)) {
        ImGui::BeginDisabled(true);
    }
    if (ImGui::BeginListBox("##modes", ImVec2(-FLT_MIN, 260))) {
        for (int i = 0; i < static_cast<int>(modes_.size()); ++i) {
            const bool is_selected = (selected_ == i);
            if (ImGui::Selectable(modes_[i].label.c_str(), is_selected)) {
                selected_ = i;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndListBox();
    }
    if (window_mode_ == static_cast<int>(WindowMode::kBorderless)) {
        ImGui::EndDisabled();
    }

    // Aspect mode applies immediately; it is cheap to toggle back.
    const char* aspect_names[] = {"Keep aspect (black bars)", "Fill screen (crop edges)",
                                  "Stretch to fill (distorts)"};
    const char* aspect_values[] = {"letterbox", "crop", "stretch"};
    int aspect = 0;
    for (int i = 0; i < 3; ++i) {
        if (aspect_mode_ == aspect_values[i]) {
            aspect = i;
            break;
        }
    }
    ImGui::TextUnformatted("Widescreen:");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##aspect", &aspect, aspect_names, 3)) {
        aspect_mode_ = aspect_values[aspect];
        bool ok = rex::cvar::SetFlagByName("present_aspect_mode", aspect_mode_);
        REXLOG_INFO("Display menu: aspect mode -> {} (set={}, now={})", aspect_mode_, ok,
                    rex::cvar::GetFlagByName("present_aspect_mode"));
        rex::cvar::SaveConfig(config_path_);
    }

    // Presentation aspect ratio; applies immediately. Irrelevant in stretch
    // mode, which always fills the surface.
    const char* ratio_names[] = {"Auto (game)", "4:3", "16:9", "16:10", "21:9"};
    const char* ratio_values[] = {"auto", "4:3", "16:9", "16:10", "21:9"};
    int ratio = 0;
    for (int i = 0; i < 5; ++i) {
        if (aspect_ratio_ == ratio_values[i]) {
            ratio = i;
            break;
        }
    }
    ImGui::TextUnformatted("Aspect ratio:");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (aspect_mode_ == "stretch") {
        ImGui::BeginDisabled(true);
    }
    if (ImGui::Combo("##aspectratio", &ratio, ratio_names, 5)) {
        aspect_ratio_ = ratio_values[ratio];
        bool ok = rex::cvar::SetFlagByName("present_aspect_ratio", aspect_ratio_);
        REXLOG_INFO("Display menu: aspect ratio -> {} (set={}, now={})", aspect_ratio_, ok,
                    rex::cvar::GetFlagByName("present_aspect_ratio"));
        rex::cvar::SaveConfig(config_path_);
    }
    if (aspect_mode_ == "stretch") {
        ImGui::EndDisabled();
        ImGui::TextDisabled("Stretch mode fills the screen at any ratio.");
    }

    // Internal render scale: written to the config, applied at next launch
    // (the cvars are kRequiresRestart and guest render targets are fixed).
    const char* scale_names[] = {"1x (1280x720)", "2x (2560x1440)", "3x (3840x2160)",
                                 "4x (5120x2880)"};
    ImGui::TextUnformatted("Internal resolution:");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##scale", &render_scale_, scale_names, 4)) {
        const std::string sv = std::to_string(render_scale_ + 1);
        rex::cvar::SetFlagByName("draw_resolution_scale_x", sv);
        rex::cvar::SetFlagByName("draw_resolution_scale_y", sv);
        rex::cvar::SaveConfig(config_path_);
    }
    ImGui::TextDisabled("Internal resolution applies after a restart.");

    if (pending_) {
        double remaining = deadline_ - ImGui::GetTime();
        if (remaining <= 0.0) {
            RevertPending();
        } else {
            ImGui::Separator();
            ImGui::Text("Keep these settings? Reverting in %ds...",
                        static_cast<int>(remaining + 0.5));
            if (ImGui::Button("Keep")) {
                CommitPending();
            }
            ImGui::SameLine();
            if (ImGui::Button("Revert")) {
                RevertPending();
            }
        }
    } else {
        ImGui::BeginDisabled(selected_ < 0);
        if (ImGui::Button("Apply")) {
            ApplySelection();
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();
    {
        // Read the live state so F1 toggles are reflected here too.
        bool fps = fps_overlay_ && fps_overlay_->IsVisible();
        if (ImGui::Checkbox("Show FPS counter (F1)", &fps)) {
            // Deferred: destroying the overlay's dialog mid-draw would make
            // the presenter mutate its drawer list while iterating it.
            if (set_fps_visible_) {
                set_fps_visible_(fps);
            }
            rex::cvar::SetFlagByName("fps_counter", fps ? "true" : "false");
            rex::cvar::SaveConfig(config_path_);
        }
    }
    ImGui::TextDisabled("Internal resolution replaces res_scale.txt.");
    ImGui::TextDisabled("Press F5 to close this menu.");

    ImGui::End();
}
