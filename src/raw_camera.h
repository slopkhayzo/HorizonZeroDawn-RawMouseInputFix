#pragma once
#include <cstdint>

// "Raw camera" (mouse mode 5): makes the third-person camera follow raw mouse counts 1:1 every
// rendered frame, independent of the game's 29.97 Hz camera tick.
//
//  Tick side   - the camera mode's angular-acceleration limiter is disabled and its look response
//                curve is linearised (|look| * curve(1.0), the scale the game already uses when
//                aiming), so each tick turns by exactly C * (counts consumed by that tick).
//  Render side - the per-frame view (built from the camera entity's 30 Hz transform) is rotated
//                about the orbit pivot by C * (counts not yet applied by a tick).
//
// All entry points except Install are called on the game thread unless noted.
namespace rawcam {

void Install();
bool Available();

void SetEnabled(bool on);
bool Enabled();

// Input side (game thread): the 30 Hz tick read consumed (ax, ay) look units; live = counts
// accumulated since that read (not yet consumed by any tick).
void OnTickRead(float ax, float ay);
void SetLiveAccum(float ax, float ay);

// Camera tick (job thread): called after the game's look-input processing for that tick.
void OnLookInput(uint8_t* comp, float dt);

void StatsTick(double elapsedMs);

// Frame timing (render thread that presents): QPC before/after the game's Present call.
void OnPresent(int64_t qpcBefore, int64_t qpcAfter, unsigned syncInterval);

// Diagnostic: apply a constant 20-degree yaw through one path only, to identify which path the
// renderer uses. 0 = off, 1 = makeView, 2 = camera vtable slot 44, 3 = entity transform write.
void CycleDebugPath();

} // namespace rawcam
