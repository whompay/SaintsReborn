// Photo mode (Spider-Man / Uncharted style): Pause menu > OPTIONS > Photo Mode.
// The world stands still (game time and audio paused), the HUD is hidden, a free camera flies
// around the player within a range, FOV / roll / filters are set in a panel, and A saves the
// finished picture (with the filters, without the panel) as a PNG in Pictures\Saints Reborn.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sr::photo {

// Pause menu row picked: photo mode starts once the menu has closed.
void RequestStart();
// The pause menu has closed and its closing Start pulse is over (coop_menu.cpp).
void MenuGone();
bool Active();
bool Pending();

// The pad / mouse (kbm.cpp, while active; the game then gets an empty pad).
void Input(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry,
           double mouse_dx, double mouse_dy, int wheel);

// Game thread: start of a game frame (stubs.cpp, main loop) and at present.
void BeforeGameFrame(uint8_t* base);
void OnPresent(uint8_t* base);
// After the follow camera (kbm.cpp sub_8210D518): the photo camera again.
void AfterCamera(uint8_t* base);

// The panel, for fps_overlay.cpp.
struct PanelRow {
  std::string name, value;
  float fill = -1.0f;  // 0..1 slider position, -1 = none
  bool action = false;
};
struct Panel {
  bool show = false;
  int selected = 0;
  std::vector<PanelRow> rows;
  std::string notice;
  float notice_alpha = 0.0f;
  float flash = 0.0f;  // 1 right after a photo, fading
  bool keyboard = false;
};
bool GetPanel(Panel& out);
// Called with true when photo mode starts and false when it ends (main.cpp: keeps the overlay window that
// draws the panel up - it is otherwise only there while a mod shows text).
void SetPanelListener(std::function<void(bool)> listener);

}  // namespace sr::photo
