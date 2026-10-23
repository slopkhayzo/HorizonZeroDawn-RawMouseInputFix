#pragma once
#include <string>

// Defaults are the release behaviour; config\HZDRawMouse.dev.ini turns the diagnostics back on.
struct Config {
    bool logEnable = true;        // HZDRawMouse.log: startup / hook status
    bool stats = false;           // once-per-second statistics in the log
    bool eventCsv = false;        // per-event CSV (large)
    bool hookPresent = false;     // IDXGISwapChain::Present hook (frame timing diagnostics)
    int startMode = 5;            // see MouseMode (5 = raw camera)
    int cycleModeKey = 0x78;      // F9: toggle raw camera <-> original
    int markerKey = 0;            // diagnostics (0 = disabled)
    int watchKey = 0;
    int simDividerKey = 0;
    int debugPathKey = 0;
    bool beep = true;             // audible feedback for hotkeys
    bool cycleAllModes = false;   // F9: false = toggle 5 <-> 0, true = cycle 0..5 (research)
};

extern Config g_cfg;

// Directory containing the plugin module, with trailing backslash.
extern std::wstring g_moduleDir;

void LoadConfig();
