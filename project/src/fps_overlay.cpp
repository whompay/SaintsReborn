// On-screen frame rate counter (F1): plain text in the top-left corner.

#include "fps_overlay.h"
#include "wml/mod_loader.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

namespace sr {

std::atomic<uint64_t> g_game_frames{0};

namespace {

ImFont* g_font = nullptr;

void SetupFont(ImFontAtlas* atlas) {
  // A bold system font if there is one; ImGui's built-in font otherwise.
  const char* candidates[] = {
      "C:\\Windows\\Fonts\\segoeuib.ttf",
      "C:\\Windows\\Fonts\\arialbd.ttf",
  };
  for (const char* path : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      g_font = atlas->AddFontFromFileTTF(path, 28.0f);
      if (g_font) return;
    }
  }
}

class FpsDialog : public rex::ui::ImGuiDialog {
 public:
  explicit FpsDialog(rex::ui::ImGuiDrawer* drawer)
      : ImGuiDialog(drawer),
        last_time_(std::chrono::steady_clock::now()),
        last_frames_(g_game_frames.load(std::memory_order_relaxed)) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - last_time_).count();
    if (elapsed >= 0.5) {
      uint64_t frames = g_game_frames.load(std::memory_order_relaxed);
      fps_ = double(frames - last_frames_) / elapsed;
      last_frames_ = frames;
      last_time_ = now;
    }

    char text[32];
    if (fps_ < 0) {
      std::snprintf(text, sizeof(text), "-- FPS");
    } else {
      std::snprintf(text, sizeof(text), "%d FPS", int(fps_ + 0.5));
    }

    float scale = std::max(0.75f, io.DisplaySize.y / 1080.0f);
    float size = 28.0f * scale;
    ImVec2 pos(16.0f * scale, 12.0f * scale);
    ImFont* font = g_font ? g_font : ImGui::GetFont();
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    float shadow = std::max(1.0f, 2.0f * scale);
    draw_list->AddText(font, size, ImVec2(pos.x + shadow, pos.y + shadow),
                       IM_COL32(0, 0, 0, 200), text);
    draw_list->AddText(font, size, pos, IM_COL32(255, 255, 255, 255), text);
  }

 private:
  std::chrono::steady_clock::time_point last_time_;
  uint64_t last_frames_;
  double fps_ = -1.0;
};

// Shows "FPS cap: N" for two seconds after F10.
class NoticeDialog : public rex::ui::ImGuiDialog {
 public:
  explicit NoticeDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  void Show(int cap) {
    cap_ = cap;
    until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (std::chrono::steady_clock::now() >= until_) return;
    char text[32];
    std::snprintf(text, sizeof(text), "FPS cap: %d", cap_);
    float scale = std::max(0.75f, io.DisplaySize.y / 1080.0f);
    float size = 28.0f * scale;
    ImVec2 pos(16.0f * scale, 48.0f * scale);
    ImFont* font = g_font ? g_font : ImGui::GetFont();
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    float shadow = std::max(1.0f, 2.0f * scale);
    draw_list->AddText(font, size, ImVec2(pos.x + shadow, pos.y + shadow),
                       IM_COL32(0, 0, 0, 200), text);
    draw_list->AddText(font, size, pos, IM_COL32(255, 220, 80, 255), text);
  }

 private:
  int cap_ = 0;
  std::chrono::steady_clock::time_point until_{};
};

// Text native mods show over the game (WML overlay_text), always drawn.
class ModTextDialog : public rex::ui::ImGuiDialog {
 public:
  explicit ModTextDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const std::string text = wml::OverlayText();
    if (text.empty()) return;
    if (!logged_) {
      logged_ = true;
      REXLOG_INFO("Drawing mod text ({} characters)", text.size());
    }
    float scale = std::max(0.75f, io.DisplaySize.y / 1080.0f);
    float size = 26.0f * scale;
    ImVec2 pos(40.0f * scale, 120.0f * scale);
    ImFont* font = g_font ? g_font : ImGui::GetFont();
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    float shadow = std::max(1.0f, 2.0f * scale);
    draw_list->AddText(font, size, ImVec2(pos.x + shadow, pos.y + shadow),
                       IM_COL32(0, 0, 0, 220), text.c_str());
    draw_list->AddText(font, size, pos, IM_COL32(255, 255, 255, 255), text.c_str());
  }

 private:
  bool logged_ = false;
};

}  // namespace

struct FpsOverlay::Impl {
  std::unique_ptr<rex::ui::ImmediateDrawer> immediate_drawer;
  std::unique_ptr<rex::ui::ImGuiDrawer> imgui_drawer;
  std::unique_ptr<FpsDialog> dialog;
  std::unique_ptr<NoticeDialog> notice;
  std::unique_ptr<ModTextDialog> mod_text;
};

FpsOverlay::FpsOverlay() = default;
FpsOverlay::~FpsOverlay() { Shutdown(); }

bool FpsOverlay::Initialize(rex::ui::Window* window, rex::ui::GraphicsProvider* provider,
                            rex::ui::Presenter* presenter) {
  if (!window || !provider || !presenter) return false;
  auto impl = std::make_unique<Impl>();
  impl->immediate_drawer = provider->CreateImmediateDrawer();
  if (!impl->immediate_drawer) return false;
  impl->immediate_drawer->SetPresenter(presenter);
  // Above the game, below anything else.
  impl->imgui_drawer = std::make_unique<rex::ui::ImGuiDrawer>(window, 64, SetupFont);
  impl->imgui_drawer->SetPresenterAndImmediateDrawer(presenter, impl->immediate_drawer.get());
  impl_ = std::move(impl);
  return true;
}

void FpsOverlay::Toggle() {
  if (!impl_) return;
  if (impl_->dialog) {
    impl_->dialog.reset();
  } else {
    impl_->dialog = std::make_unique<FpsDialog>(impl_->imgui_drawer.get());
  }
}

void FpsOverlay::SetVisible(bool visible) {
  if (!impl_) return;
  if (visible && !impl_->dialog) {
    impl_->dialog = std::make_unique<FpsDialog>(impl_->imgui_drawer.get());
  } else if (!visible) {
    impl_->dialog.reset();
  }
}

bool FpsOverlay::IsVisible() const {
  return impl_ && impl_->dialog != nullptr;
}

void FpsOverlay::ShowNotice(int cap) {
  if (!impl_) return;
  if (!impl_->notice) {
    impl_->notice = std::make_unique<NoticeDialog>(impl_->imgui_drawer.get());
  }
  impl_->notice->Show(cap);
}

// Created on demand on the UI thread: a dialog made at start-up, before the
// presenter is connected to the window, never gets drawn.
void FpsOverlay::SetModTextVisible(bool visible) {
  if (!impl_) return;
  if (visible && !impl_->mod_text) {
    impl_->mod_text = std::make_unique<ModTextDialog>(impl_->imgui_drawer.get());
  } else if (!visible) {
    impl_->mod_text.reset();
  }
}

void FpsOverlay::Shutdown() {
  if (!impl_) return;
  impl_->mod_text.reset();
  impl_->notice.reset();
  impl_->dialog.reset();
  impl_->imgui_drawer.reset();
  if (impl_->immediate_drawer) impl_->immediate_drawer->SetPresenter(nullptr);
  impl_->immediate_drawer.reset();
  impl_.reset();
}

}  // namespace sr
