// HZDRawMouse — ASI plugin (Ultimate ASI Loader) for Horizon Zero Dawn (PC).
// Instrumentation build: observes the mouse path and offers an opt-in throttle-removal experiment.
#include "config.h"
#include "hooks.h"
#include "log.h"
#include "mem.h"
#include "raw_camera.h"
#include "version.h"
#include "watch.h"

#include <windows.h>
#include <atomic>

namespace {

std::atomic<bool> s_stop{false};

bool GameIsForeground()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

bool KeyPressedEdge(int vk, bool& wasDown)
{
    const bool down = vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = down && !wasDown;
    wasDown = down;
    return edge;
}

void Tone(DWORD freq, DWORD ms)
{
    if (g_cfg.beep) Beep(freq, ms);
}

DWORD WINAPI Worker(LPVOID)
{
    bool modeDown = false, markDown = false, watchDown = false, divDown = false, dbgDown = false;
    int64_t lastStats = QpcNow();
    while (!s_stop.load()) {
        Sleep(10);
        if (GameIsForeground()) {
            if (KeyPressedEdge(g_cfg.cycleModeKey, modeDown) && ModesAvailable()) {
                const int cur = static_cast<int>(GetMouseMode());
                const int next = g_cfg.cycleAllModes
                                     ? (cur + 1) % static_cast<int>(MouseMode::Count)
                                     : (cur == static_cast<int>(MouseMode::RawCamera) ? static_cast<int>(MouseMode::Original)
                                                                                      : static_cast<int>(MouseMode::RawCamera));
                SetMouseMode(static_cast<MouseMode>(next));
                static const DWORD kModeTone[] = {440, 660, 990, 1320, 1760, 2350};
                Tone(kModeTone[next], 120);
            }
            if (KeyPressedEdge(g_cfg.markerKey, markDown)) {
                EvPush(Ev::Mark);
                Log("---- MARK ----");
                Tone(1500, 40);
            }
            if (KeyPressedEdge(g_cfg.simDividerKey, divDown) && SimDividerAvailable()) {
                const int next = GetSimDivider() == 1 ? 2 : 1;
                SetSimDivider(next);
                Tone(next == 1 ? 2000 : 300, 150);
            }
            if (KeyPressedEdge(g_cfg.debugPathKey, dbgDown)) {
                rawcam::CycleDebugPath();
                Tone(800, 60);
            }
            if (KeyPressedEdge(g_cfg.watchKey, watchDown)) {
                const bool wasArmed = watch::Armed();
                if (ToggleStateWatch()) {
                    Tone(wasArmed ? 900 : 600, 80);
                    Tone(wasArmed ? 600 : 900, 80);
                } else {
                    Tone(200, 300); // not available yet
                }
            }
        }
        EvDrain();
        const int64_t now = QpcNow();
        const double ms = QpcToMs(now - lastStats);
        if (ms >= 1000.0) {
            if (g_cfg.stats) InputStatsTick(ms);
            lastStats = now;
        }
    }
    return 0;
}

void Init(HMODULE self)
{
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    g_moduleDir.assign(path, n);
    g_moduleDir.resize(g_moduleDir.find_last_of(L"\\/") + 1);

    LoadConfig();
    LogInit();

    const auto& exe = mem::Exe();
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(exe.base + reinterpret_cast<IMAGE_DOS_HEADER*>(exe.base)->e_lfanew);
    // Informational only — never used to gate behaviour.
    Log("HZDRawMouse " HZDRM_VERSION_STRING " | exe base %p | PE timestamp %lu | .text %zu KB",
        exe.base, nt->FileHeader.TimeDateStamp, static_cast<size_t>(exe.text.end - exe.text.begin) / 1024);

    InstallInputHooks();
    InstallDxgiHooks();
    Log("Hotkeys: cycle mode = VK 0x%02X, marker = VK 0x%02X, watch = VK 0x%02X", g_cfg.cycleModeKey, g_cfg.markerKey,
        g_cfg.watchKey);

    if (HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(t);
}

HMODULE s_self = nullptr;
INIT_ONCE s_initOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK InitOnceCb(PINIT_ONCE, PVOID, PVOID*)
{
    Init(s_self);
    return TRUE;
}

void InitOnce() { InitOnceExecuteOnce(&s_initOnce, InitOnceCb, nullptr, nullptr); }

// Ultimate ASI Loader exports IsUltimateASILoader; if it is on our load stack it will call
// InitializeASI() for us outside the loader lock.
bool LoadedByUltimateAsiLoader()
{
    void* frames[64];
    const USHORT n = RtlCaptureStackBackTrace(0, 64, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        HMODULE m = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(frames[i]), &m) &&
            GetProcAddress(m, "IsUltimateASILoader"))
            return true;
    }
    return false;
}

} // namespace

extern "C" __declspec(dllexport) void InitializeASI() { InitOnce(); }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        s_self = module;
        DisableThreadLibraryCalls(module);
        // Other loaders only LoadLibrary us: initialise here (under the loader lock).
        if (!LoadedByUltimateAsiLoader()) InitOnce();
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        // Explicit unload only; on process exit other threads are already gone.
        s_stop = true;
        LogShutdown();
    }
    return TRUE;
}
