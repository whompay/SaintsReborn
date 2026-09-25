// Keyboard and mouse controls (kbm.cpp).
#pragma once

namespace sr {

// Mouse look speed (1.0 = default).
void SetMouseSensitivity(double sensitivity);

// Mouse wheel input from the host window (positive = wheel up).
void AddMouseWheel(int delta);

// Suspends the mouselook cursor capture (used while an interactive overlay
// such as the display settings menu is open).
void SetMouseCaptureSuspended(bool suspended);

}  // namespace sr
