// On-screen frame rate counter (F1): plain text in the top-left corner.

#include "fps_overlay.h"
#include "chat.h"
#include "wml/mod_loader.h"
#include "world_studio_bridge.h"

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

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
    if (cap_) std::snprintf(text, sizeof(text), "FPS cap: %d", cap_);
    else std::snprintf(text, sizeof(text), "FPS cap: off");
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

// In-game chat (chat.cpp): the last lines at the bottom left, name in the
// game's yellow; while typing, a "Say:" line under them.
void DrawChat(ImGuiIO& io) {
  std::vector<sr::ChatLine> lines;
  std::string input;
  const bool typing = sr::ChatSnapshot(lines, input);
  if (lines.empty() && !typing) return;
  const float scale = std::max(0.75f, io.DisplaySize.y / 1080.0f);
  const float size = 24.0f * scale;
  const float line_h = size * 1.2f;
  ImFont* font = g_font ? g_font : ImGui::GetFont();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float shadow = std::max(1.0f, 2.0f * scale);
  const float x = 40.0f * scale;
  float y = io.DisplaySize.y * 0.72f - line_h * float(lines.size() + (typing ? 1 : 0));
  auto put = [&](float px, float py, const std::string& s, ImU32 color, float alpha) {
    const int a = int(alpha * 255.0f);
    dl->AddText(font, size, ImVec2(px + shadow, py + shadow), IM_COL32(0, 0, 0, a * 220 / 255), s.c_str());
    dl->AddText(font, size, ImVec2(px, py), (color & 0x00FFFFFFu) | (ImU32(a) << 24), s.c_str());
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, s.c_str()).x;
  };
  for (const auto& l : lines) {
    if (l.system) {
      put(x, y, l.text, IM_COL32(190, 190, 190, 255), l.alpha);
    } else {
      const float w = put(x, y, l.name + ": ", IM_COL32(255, 210, 60, 255), l.alpha);
      put(x + w, y, l.text, IM_COL32(255, 255, 255, 255), l.alpha);
    }
    y += line_h;
  }
  if (typing) {
    const float w = put(x, y, "Say: ", IM_COL32(255, 210, 60, 255), 1.0f);
    const bool caret = (std::chrono::steady_clock::now().time_since_epoch() / std::chrono::milliseconds(500)) % 2 == 0;
    put(x + w, y, input + (caret ? "_" : ""), IM_COL32(255, 255, 255, 255), 1.0f);
  }
}

// Online notices (fair play, invites, friends): a panel in the style of the
// game's own help boxes - dark purple, Saints purple edge, white text, a
// small header and a bar that runs down while it shows. Slides in from the
// left and fades out.
void DrawOnlineNotice(ImGuiIO& io) {
  static std::string shown_text;
  static std::chrono::steady_clock::time_point since;
  const std::string text = wml::HostOverlayText();
  const auto now = std::chrono::steady_clock::now();
  if (text.empty()) { shown_text.clear(); return; }
  if (text != shown_text) { shown_text = text; since = now; }
  constexpr float kShow = 12.0f, kIn = 0.28f, kOut = 0.45f;
  const float t = std::chrono::duration<float>(now - since).count();
  if (t >= kShow) return;
  const float in = std::min(1.0f, t / kIn);
  const float ease = 1.0f - (1.0f - in) * (1.0f - in) * (1.0f - in);
  const float alpha = std::min(ease, std::clamp((kShow - t) / kOut, 0.0f, 1.0f));
  auto col = [&](int r, int g, int b, int a) { return IM_COL32(r, g, b, int(a * alpha)); };

  const float scale = std::max(0.6f, io.DisplaySize.y / 1080.0f);
  ImFont* font = g_font ? g_font : ImGui::GetFont();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const char* title = "ONLINE";
  if (text.find("invited") != std::string::npos) title = "INVITE";
  else if (text.find("friend") != std::string::npos && text.find("Private Party") == std::string::npos) title = "FRIENDS";

  const float body_size = 24.0f * scale, title_size = 19.0f * scale;
  const float pad = 18.0f * scale, edge = 7.0f * scale;
  const float width = std::min(600.0f * scale, io.DisplaySize.x * 0.42f);
  const float wrap = width - edge - pad * 2.0f;
  const ImVec2 body_ext = font->CalcTextSizeA(body_size, FLT_MAX, wrap, text.c_str());
  const float head_h = title_size + 10.0f * scale;
  const float height = pad + head_h + body_ext.y + pad + 4.0f * scale;
  const float x = io.DisplaySize.x * 0.045f - (1.0f - ease) * (width * 0.35f);
  const float y = io.DisplaySize.y * 0.075f;
  const ImVec2 a(x, y), b(x + width, y + height);

  // Drop shadow, body (black to deep purple), purple frame and left edge.
  dl->AddRectFilled(ImVec2(a.x + 5 * scale, a.y + 6 * scale), ImVec2(b.x + 5 * scale, b.y + 6 * scale),
                    col(0, 0, 0, 110));
  dl->AddRectFilledMultiColor(a, b, col(14, 6, 20, 232), col(30, 10, 44, 232), col(46, 14, 66, 236),
                              col(18, 6, 26, 236));
  dl->AddRect(a, b, col(126, 58, 176, 200), 0.0f, 0, std::max(1.0f, 1.5f * scale));
  dl->AddRectFilled(a, ImVec2(a.x + edge, b.y), col(150, 70, 205, 255));
  dl->AddRectFilled(ImVec2(a.x + edge, a.y), ImVec2(a.x + edge + 2 * scale, b.y), col(215, 170, 255, 120));

  // Header: small spaced capitals and a line that fades out to the right.
  float tx = a.x + edge + pad;
  const float ty = a.y + pad * 0.8f;
  for (const char* c = title; *c; ++c) {
    const char ch[2] = {*c, 0};
    dl->AddText(font, title_size, ImVec2(tx + scale, ty + scale), col(0, 0, 0, 200), ch);
    dl->AddText(font, title_size, ImVec2(tx, ty), col(205, 160, 255, 255), ch);
    tx += font->CalcTextSizeA(title_size, FLT_MAX, 0.0f, ch).x + 3.0f * scale;
  }
  const float line_y = ty + title_size + 4.0f * scale;
  const float lx = a.x + edge + pad, rx = b.x - pad;
  dl->AddRectFilledMultiColor(ImVec2(lx, line_y), ImVec2(rx, line_y + std::max(1.0f, 2.0f * scale)),
                              col(160, 80, 220, 230), col(160, 80, 220, 0), col(160, 80, 220, 0),
                              col(160, 80, 220, 230));

  // Body: white with a black outline, like the game's HUD text.
  const ImVec2 bp(lx, a.y + pad + head_h);
  const float o = std::max(1.0f, 1.6f * scale);
  const ImVec2 offs[] = {{-o, 0}, {o, 0}, {0, -o}, {0, o}, {o, o}};
  for (const ImVec2& d : offs)
    dl->AddText(font, body_size, ImVec2(bp.x + d.x, bp.y + d.y), col(0, 0, 0, 210), text.c_str(), nullptr, wrap);
  dl->AddText(font, body_size, bp, col(255, 255, 255, 255), text.c_str(), nullptr, wrap);

  // Time left.
  const float left = std::clamp(1.0f - t / kShow, 0.0f, 1.0f);
  const float bar_y = b.y - 4.0f * scale;
  dl->AddRectFilled(ImVec2(a.x + edge, bar_y), ImVec2(b.x, b.y), col(0, 0, 0, 120));
  dl->AddRectFilled(ImVec2(a.x + edge, bar_y), ImVec2(a.x + edge + (b.x - a.x - edge) * left, b.y),
                    col(150, 70, 205, 255));
}

// Text native mods show over the game (WML overlay_text), always drawn.
class ModTextDialog : public rex::ui::ImGuiDialog {
 public:
  explicit ModTextDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    DrawChat(io);
    DrawOnlineNotice(io);
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

// World Studio editor host: the move gizmo of the selected object and a
// "WORLD PAUSED" label, drawn over the game view.
class GizmoDialog : public rex::ui::ImGuiDialog {
 public:
  explicit GizmoDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    sr::world_studio::GizmoDraw g;
    if (!sr::world_studio::GetGizmoDraw(g)) return;
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    if (g.paused) {
      const char* text = "WORLD PAUSED  (Ctrl+Shift+P)";
      ImFont* font = g_font ? g_font : ImGui::GetFont();
      const float size = std::max(16.0f, io.DisplaySize.y / 40.0f);
      const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
      const ImVec2 pos((io.DisplaySize.x - extent.x) * 0.5f, size * 0.6f);
      draw_list->AddText(font, size, ImVec2(pos.x + 2, pos.y + 2), IM_COL32(0, 0, 0, 200), text);
      draw_list->AddText(font, size, pos, IM_COL32(255, 210, 60, 255), text);
    }
    if (!g.visible || g.view_width <= 0 || g.view_height <= 0) return;
    const float kx = io.DisplaySize.x / g.view_width, ky = io.DisplaySize.y / g.view_height;
    const ImVec2 o(g.ox * kx, g.oy * ky);
    const ImU32 colors[3] = {IM_COL32(230, 60, 60, 255), IM_COL32(110, 210, 60, 255),
                             IM_COL32(60, 130, 240, 255)};
    const ImU32 highlight = IM_COL32(255, 225, 40, 255);
    for (int a = 0; a < 3; ++a) {
      if (!g.axis_visible[a]) continue;
      const ImVec2 tip(g.ex[a] * kx, g.ey[a] * ky);
      const ImU32 color = (a == g.active || (g.active < 0 && a == g.hot)) ? highlight : colors[a];
      draw_list->AddLine(o, tip, IM_COL32(0, 0, 0, 160), 5.0f);
      draw_list->AddLine(o, tip, color, 3.0f);
      // Arrow head.
      float dx = tip.x - o.x, dy = tip.y - o.y;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len > 1.0f) {
        dx /= len; dy /= len;
        const float h = 14.0f, w = 6.0f;
        const ImVec2 base(tip.x - dx * h, tip.y - dy * h);
        draw_list->AddTriangleFilled(tip, ImVec2(base.x - dy * w, base.y + dx * w),
                                     ImVec2(base.x + dy * w, base.y - dx * w), color);
      }
    }
    draw_list->AddRectFilled(ImVec2(o.x - 4, o.y - 4), ImVec2(o.x + 4, o.y + 4), IM_COL32(255, 255, 255, 230));
  }
};

// Beams from mods (WML overlay_beams, e.g. the Superpowers laser eyes):
// world-space lines projected with the game camera and drawn as a glow with
// a bright core. Drawn over the game (no depth test).
class BeamDialog : public rex::ui::ImGuiDialog {
 public:
  explicit BeamDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!wml::OverlayBeams(beams_)) return;
    float eye[3], R[3], U[3], F[3], fov;
    if (!wml::GameCamera(eye, R, U, F, fov)) return;
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    if (w < 8 || h < 8) return;
    const float focal = (h * 0.5f) / std::tan(fov * 3.14159265f / 360.0f);
    const float scale = h / 1080.0f;
    const float t = float(ImGui::GetTime());
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    auto cam = [&](const float* p, float c[3]) {
      const float d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
      c[0] = d[0] * R[0] + d[1] * R[1] + d[2] * R[2];
      c[1] = d[0] * U[0] + d[1] * U[1] + d[2] * U[2];
      c[2] = d[0] * F[0] + d[1] * F[1] + d[2] * F[2];
    };
    for (size_t i = 0; i + 10 <= beams_.size(); i += 10) {
      const float* b = &beams_[i];
      float a[3], c[3];
      cam(b, a); cam(b + 3, c);
      // Clip against a near plane in front of the camera.
      const float near_z = 0.1f;
      if (a[2] < near_z && c[2] < near_z) continue;
      if (a[2] < near_z || c[2] < near_z) {
        const float k = (near_z - a[2]) / (c[2] - a[2]);
        float m[3] = {a[0] + (c[0] - a[0]) * k, a[1] + (c[1] - a[1]) * k, near_z};
        if (a[2] < near_z) std::memcpy(a, m, sizeof(m)); else std::memcpy(c, m, sizeof(m));
      }
      const ImVec2 p0(w * 0.5f + a[0] / a[2] * focal, h * 0.5f - a[1] / a[2] * focal);
      const ImVec2 p1(w * 0.5f + c[0] / c[2] * focal, h * 0.5f - c[1] / c[2] * focal);
      const float width = std::max(1.0f, b[6] * scale) * (0.9f + 0.1f * std::sin(t * 60.0f + float(i)));
      const int r = int(std::clamp(b[7], 0.0f, 1.0f) * 255), g = int(std::clamp(b[8], 0.0f, 1.0f) * 255),
                bl = int(std::clamp(b[9], 0.0f, 1.0f) * 255);
      // Glow: wide and faint -> narrow and strong, then a hot white core.
      dl->AddLine(p0, p1, IM_COL32(r, g, bl, 40), width * 4.0f);
      dl->AddLine(p0, p1, IM_COL32(r, g, bl, 80), width * 2.4f);
      dl->AddLine(p0, p1, IM_COL32(r, g, bl, 200), width * 1.3f);
      dl->AddLine(p0, p1, IM_COL32(255, std::min(255, g + 170), std::min(255, bl + 170), 255), std::max(1.0f, width * 0.45f));
      // Hit glow at the far end.
      dl->AddCircleFilled(p1, width * 2.2f, IM_COL32(r, g, bl, 90), 16);
      dl->AddCircleFilled(p1, width * 1.1f, IM_COL32(255, std::min(255, g + 150), std::min(255, bl + 150), 220), 12);
    }
  }

 private:
  std::vector<float> beams_;
};

}  // namespace

struct FpsOverlay::Impl {
  std::unique_ptr<rex::ui::ImmediateDrawer> immediate_drawer;
  std::unique_ptr<rex::ui::ImGuiDrawer> imgui_drawer;
  std::unique_ptr<FpsDialog> dialog;
  std::unique_ptr<NoticeDialog> notice;
  std::unique_ptr<ModTextDialog> mod_text;
  std::unique_ptr<GizmoDialog> gizmo;
  std::unique_ptr<BeamDialog> beams;
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

bool FpsOverlay::IsVisible() const { return impl_ && impl_->dialog != nullptr; }

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

void FpsOverlay::SetStudioGizmoVisible(bool visible) {
  if (!impl_) return;
  if (visible && !impl_->gizmo) {
    impl_->gizmo = std::make_unique<GizmoDialog>(impl_->imgui_drawer.get());
  } else if (!visible) {
    impl_->gizmo.reset();
  }
}

void FpsOverlay::SetBeamsVisible(bool visible) {
  if (!impl_) return;
  if (visible && !impl_->beams) {
    impl_->beams = std::make_unique<BeamDialog>(impl_->imgui_drawer.get());
  } else if (!visible) {
    impl_->beams.reset();
  }
}

void FpsOverlay::Shutdown() {
  if (!impl_) return;
  impl_->beams.reset();
  impl_->gizmo.reset();
  impl_->mod_text.reset();
  impl_->notice.reset();
  impl_->dialog.reset();
  impl_->imgui_drawer.reset();
  if (impl_->immediate_drawer) impl_->immediate_drawer->SetPresenter(nullptr);
  impl_->immediate_drawer.reset();
  impl_.reset();
}

}  // namespace sr
