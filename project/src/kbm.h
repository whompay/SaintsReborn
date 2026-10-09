// Keyboard and mouse controls (kbm.cpp).
#pragma once
#include <cstdint>

namespace sr {

// Mouse look speed (1.0 = default).
void SetMouseSensitivity(double sensitivity);
double GetMouseSensitivity();

// Mouse wheel input from the host window (positive = wheel up).
void AddMouseWheel(int delta);
// Game state for the Discord status: the player drives a vehicle / the pause
// menu is open.
bool PlayerDriving(uint8_t* base);
bool PauseMenuOpen();

// Suspends the mouselook cursor capture (used while an interactive overlay
// such as the display settings menu is open).
void SetMouseCaptureSuspended(bool suspended);

}  // namespace sr
