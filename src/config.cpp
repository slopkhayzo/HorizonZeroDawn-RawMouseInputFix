#include "config.h"

#include <windows.h>
#include <cwchar>

Config g_cfg;
std::wstring g_moduleDir;

static int ReadInt(const wchar_t* path, const wchar_t* sec, const wchar_t* key, int def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, 64, path);
    if (!buf[0]) return def;
    return static_cast<int>(wcstol(buf, nullptr, 0)); // base 0: accepts 0x..
}

void LoadConfig()
{
    std::wstring ini = g_moduleDir + L"HZDRawMouse.ini";
    const wchar_t* p = ini.c_str();
    const Config d; // defaults
    g_cfg.logEnable = ReadInt(p, L"Log", L"Enable", d.logEnable) != 0;
    g_cfg.stats = ReadInt(p, L"Log", L"Stats", d.stats) != 0;
    g_cfg.eventCsv = ReadInt(p, L"Log", L"EventCsv", d.eventCsv) != 0;
    g_cfg.hookPresent = ReadInt(p, L"Hooks", L"Present", d.hookPresent) != 0;
    g_cfg.startMode = ReadInt(p, L"Mouse", L"StartMode", ReadInt(p, L"Experimental", L"StartMode", d.startMode));
    g_cfg.cycleModeKey = ReadInt(p, L"Mouse", L"ToggleKey", ReadInt(p, L"Experimental", L"CycleModeKey", d.cycleModeKey));
    g_cfg.beep = ReadInt(p, L"Mouse", L"Beep", ReadInt(p, L"Experimental", L"Beep", d.beep)) != 0;
    g_cfg.cycleAllModes = ReadInt(p, L"Experimental", L"CycleAllModes", d.cycleAllModes) != 0;
    g_cfg.markerKey = ReadInt(p, L"Experimental", L"MarkerKey", d.markerKey);
    g_cfg.watchKey = ReadInt(p, L"Experimental", L"WatchKey", d.watchKey);
    g_cfg.simDividerKey = ReadInt(p, L"Experimental", L"SimDividerKey", d.simDividerKey);
    g_cfg.debugPathKey = ReadInt(p, L"Experimental", L"DebugPathKey", d.debugPathKey);
}
