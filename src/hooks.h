#pragma once

// Each installer validates its targets once and logs the outcome; a failed check disables only
// that feature.
void InstallInputHooks();
void InstallDxgiHooks();

// Mouse handling experiments (instrumentation build). Safe to call from any thread.
enum class MouseMode : int {
    Original = 0,    // game's 33.3 ms throttled drain
    Unthrottled = 1, // drain every poll; per-frame delta passed through unscaled
    Compensated = 2, // drain every poll; delta scaled by throttle interval / drain interval
    FrameFeed = 3,   // Compensated, and the per-frame delta is also written into the player's
                     // 30 Hz input buffer every frame (camera sees fresh input each frame)
    TickExact = 4,   // each 30 Hz tick gets exactly the raw counts since the previous tick, and
                     // the camera's look smoothing (N-tick average + slew limiter) is bypassed
    RawCamera = 5,   // TickExact + linear camera response, no angular acceleration limit, and the
                     // rendered view rotated every frame by the counts not yet applied by a tick
    Count
};
bool ModesAvailable();
MouseMode GetMouseMode();
void SetMouseMode(MouseMode m);

// Hardware watchpoint on the mouse delta in the player's 30 Hz input buffer (falls back to the
// input manager's per-frame copy). Returns false if no address is known yet (not in gameplay).
bool ToggleStateWatch();

// World clock step divider (game sim step = divider × base 59.94 Hz step; game default 2 → 29.97 Hz).
bool SimDividerAvailable();
int GetSimDivider();
void SetSimDivider(int divider);

// Periodic diagnostics from the worker thread (once per second).
void InputStatsTick(double elapsedMs);
