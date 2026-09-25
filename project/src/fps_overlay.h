// On-screen frame rate counter (F1).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace rex::ui {
class GraphicsProvider;
class Presenter;
class Window;
}  // namespace rex::ui

namespace sr {

// Counts frames the game presents (incremented by the present hook).
extern std::atomic<uint64_t> g_game_frames;

class FpsOverlay {
 public:
  FpsOverlay();
  ~FpsOverlay();

  // Call on the UI thread once the presenter exists.
  bool Initialize(rex::ui::Window* window, rex::ui::GraphicsProvider* provider,
                  rex::ui::Presenter* presenter);
  // Shows or hides the counter. UI thread only.
  void Toggle();
  // Shows or hides the counter explicitly. UI thread only.
  void SetVisible(bool visible);
  bool IsVisible() const;
  // Briefly shows "FPS cap: N". UI thread only.
  void ShowNotice(int cap);
  // Shows or hides text from native mods (WML overlay_text). UI thread only.
  void SetModTextVisible(bool visible);
  // Call on the UI thread before the presenter goes away.
  void Shutdown();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sr
