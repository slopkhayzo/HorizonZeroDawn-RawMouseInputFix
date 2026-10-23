// Instrumentation of the game's mouse path:
//   WM_INPUT -> GetRawInputData (IAT hook)          : every raw packet
//   KeyBoardController vtable[6] "Update"            : 33.3 ms throttled drain of the Win32Mouse queue
//   KeyBoardController vtable[1] "GetState"          : delta -> controller state (+0x08 / +0x0C floats)
//   Input manager per-frame loop (caller of Update)  : stores the state copy other systems read
// See research/docs/PLAN.md §2 for the reverse-engineering notes behind the offsets below.
#include "hooks.h"
#include "config.h"
#include "log.h"
#include "mem.h"
#include "raw_camera.h"
#include "watch.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstring>

namespace {

// --- KeyBoardController layout (validated once in InstallInputHooks) -----------------------
constexpr size_t kKbcMouse = 0x20;    // Win32Mouse*
constexpr size_t kKbcDx = 0x48;       // int32 accumulated dx since last drain (dy at +0x4C)
constexpr size_t kKbcLastPoll = 0x58; // uint64 rdtsc of last drain
constexpr size_t kMouseQueueCount = 0x10;
constexpr size_t kStateDx = 0x08;     // float in controller state (dy at +0x0C)

constexpr int kSlotGetState = 1;
constexpr int kSlotUpdate = 6;

using UpdateFn = void(__fastcall*)(void* self);
using GetStateFn = uint64_t(__fastcall*)(void* self, uintptr_t a2, uintptr_t a3, uint8_t* out, uintptr_t a5,
                                         uintptr_t a6);
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);

UpdateFn s_origUpdate = nullptr;
GetStateFn s_origGetState = nullptr;
GetRawInputDataFn s_origGetRawInputData = nullptr;

bool s_layoutOk = false;
float* s_throttleConst = nullptr; // 33.333f in .rdata
float s_throttleOrig = 0.f;
float* s_tscPerMs = nullptr;      // game's TSC calibration
std::atomic<int> s_mode{0};

// Drain interval (ms) of the most recent drain, for compensation.
std::atomic<int64_t> s_lastDrainQpc{0};
std::atomic<float> s_drainIntervalMs{0.f};

// --- input manager stored-state discovery (once) -------------------------------------------
std::atomic<uintptr_t> s_mgrRetChecked{0}; // return site already examined
std::atomic<bool> s_mgrRetOk{false};
int32_t s_stateOff = 0;                    // controller entry -> stored 0x160-byte state
std::atomic<uintptr_t> s_entry{0};         // input manager controller entry for the KB+M slot

// Player input buffer filled by the 30 Hz gameplay tick (a GetState whose output is not on the
// caller's stack), and the latest per-frame delta in camera "rate" units for FrameFeed.
std::atomic<uint8_t*> s_tickOut{nullptr};
std::atomic<int64_t> s_tickQpc{0};
float s_feedDx = 0.f, s_feedDy = 0.f; // game thread only

// TickExact: per-frame deltas accumulated until the next 30 Hz tick read (game thread only).
float s_tickAccDx = 0.f, s_tickAccDy = 0.f;

// --- camera look-input processing (FUN_1413b02f0-equivalent, per 30 Hz tick) --------------
// void LookInput(CameraInputComponent* comp, float dt):
//   comp+0x30  -> settings (int N at +0x38 = averaging window in ticks, curve at +0x40 = slew limit)
//   comp+0x100 -> byte: when set, the N-tick moving average and the slew limiter are skipped
//   comp+0x120 -> filtered look vector consumed by the camera mode
constexpr size_t kCamSettings = 0x30;
constexpr size_t kCamSettingsWindow = 0x38;
constexpr size_t kCamNoSmooth = 0x100;
constexpr size_t kCamLook = 0x120;
constexpr size_t kCamEntity = 0x48;   // owning (camera) entity
constexpr size_t kEntityPos = 0x120;  // entity world position: 3 doubles, rotation matrix follows
using LookInputFn = void(__fastcall*)(uint8_t* comp, float dt);
LookInputFn s_origLookInput = nullptr;
std::atomic<uint8_t*> s_camComp{nullptr};
std::atomic<uint32_t> st_look{0};
std::atomic<float> st_lookDt{0.f};
std::atomic<int> st_lookWindow{0};
std::atomic<int> st_lookFlag{0};

bool OnCurrentStack(const void* p)
{
    ULONG_PTR lo, hi;
    GetCurrentThreadStackLimits(&lo, &hi);
    const auto a = reinterpret_cast<ULONG_PTR>(p);
    return a >= lo && a < hi;
}

// --- per-second statistics -----------------------------------------------------------------
std::atomic<uint32_t> st_raw{0}, st_upd{0}, st_drain{0}, st_gs{0}, st_gsNonZero{0};
std::atomic<int64_t> st_rawSumX{0}, st_rawSumY{0};
std::atomic<uint32_t> st_updThread{0}, st_gsThread{0};

constexpr int kMaxCallers = 8;
std::atomic<uintptr_t> s_gsCallers[kMaxCallers];
std::atomic<uintptr_t> s_updCallers[kMaxCallers];

void NoteCaller(std::atomic<uintptr_t>* table, uintptr_t ret)
{
    for (int i = 0; i < kMaxCallers; ++i) {
        uintptr_t cur = table[i].load(std::memory_order_relaxed);
        if (cur == ret) return;
        if (cur == 0) {
            uintptr_t expected = 0;
            if (table[i].compare_exchange_strong(expected, ret) || expected == ret) return;
        }
    }
}

template <class T> T& Field(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

unsigned long long GhidraVa(const void* p)
{
    return static_cast<unsigned long long>(static_cast<const uint8_t*>(p) - mem::Exe().base + 0x140000000ull);
}

// The manager loop calls Update as:
//   lea rbx,[rdi+X] ; lea rbp,[rdi+Y] ; ... call thunk ; mov rcx,[rbx] ; lea rdx,[rsp+..] ; call GetState-wrapper
//   ... lea rax,[rbp+Z]  (copy destination)
// so rbx = controller entry and the stored state is at entry + (Y + Z - X). Checked once per site.
bool CheckManagerReturnSite(uint8_t* ret)
{
    if (!mem::Exe().text.contains(ret)) return false;
    if (!mem::FindPattern(ret, 3, "48 8B 0B")) return false;
    uint8_t* leaRbx = mem::FindPattern(ret - 0x50, 0x50, "48 8D 9F ?? ?? ?? ??");
    uint8_t* leaRbp = mem::FindPattern(ret - 0x50, 0x50, "48 8D AF ?? ?? ?? ??");
    uint8_t* leaRax = mem::FindPattern(ret, 0x30, "48 8D 85 ?? ?? ?? ??");
    if (!leaRbx || !leaRbp || !leaRax) return false;
    const int32_t x = *reinterpret_cast<int32_t*>(leaRbx + 3);
    const int32_t y = *reinterpret_cast<int32_t*>(leaRbp + 3);
    const int32_t z = *reinterpret_cast<int32_t*>(leaRax + 3);
    s_stateOff = y + z - x;
    Log("Input manager loop at 0x%llx: entry=rdi+0x%x, state copy at entry+0x%x", GhidraVa(ret), x, s_stateOff);
    return s_stateOff > 0 && s_stateOff < 0x100;
}

// Recover the caller's rbx (callee-saved, untouched by the jmp thunk) by unwinding our own frame.
__declspec(noinline) uintptr_t CallerRbx(uintptr_t expectedRet)
{
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    for (int frame = 0; frame < 3; ++frame) { // CallerRbx -> HkUpdate -> game
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn) return 0;
        PVOID handlerData;
        DWORD64 establisher;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
        if (ctx.Rip == expectedRet) return ctx.Rbx;
    }
    return 0;
}

// --- hooks ---------------------------------------------------------------------------------
void __fastcall HkUpdate(void* self)
{
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    int32_t queued = -1;
    uint64_t lastBefore = 0;
    if (s_layoutOk) {
        void* mouse = Field<void*>(self, kKbcMouse);
        if (mouse) queued = Field<int32_t>(mouse, kMouseQueueCount);
        lastBefore = Field<uint64_t>(self, kKbcLastPoll);
    }

    // One-time discovery of where the manager stores this controller's state.
    if (!s_entry.load(std::memory_order_relaxed)) {
        uintptr_t expected = 0;
        if (s_mgrRetChecked.compare_exchange_strong(expected, ret))
            s_mgrRetOk = CheckManagerReturnSite(reinterpret_cast<uint8_t*>(ret));
        if (s_mgrRetOk && s_mgrRetChecked.load() == ret) {
            const uintptr_t entry = CallerRbx(ret);
            if (entry && *reinterpret_cast<void**>(entry)) {
                s_entry = entry;
                Log("Input manager KB+M entry at %p -> stored state %p", reinterpret_cast<void*>(entry),
                    reinterpret_cast<void*>(entry + s_stateOff));
            }
        }
    }

    s_origUpdate(self);

    st_upd.fetch_add(1, std::memory_order_relaxed);
    st_updThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    NoteCaller(s_updCallers, ret);
    if (s_layoutOk) {
        const bool drained = Field<uint64_t>(self, kKbcLastPoll) != lastBefore;
        if (drained) {
            st_drain.fetch_add(1, std::memory_order_relaxed);
            const int64_t now = QpcNow();
            const int64_t prev = s_lastDrainQpc.exchange(now);
            if (prev) s_drainIntervalMs.store(static_cast<float>(QpcToMs(now - prev)), std::memory_order_relaxed);
        }
        EvPush(Ev::Update, drained, queued, static_cast<float>(Field<int32_t>(self, kKbcDx)),
               static_cast<float>(Field<int32_t>(self, kKbcDx + 4)), ret);
    } else {
        EvPush(Ev::Update, -1, -1, 0.f, 0.f, ret);
    }
}

uint64_t __fastcall HkGetState(void* self, uintptr_t a2, uintptr_t a3, uint8_t* out, uintptr_t a5, uintptr_t a6)
{
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uint64_t r = s_origGetState(self, a2, a3, out, a5, a6);

    st_gs.fetch_add(1, std::memory_order_relaxed);
    st_gsThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    NoteCaller(s_gsCallers, ret);
    if (s_layoutOk && out) {
        float& dx = Field<float>(out, kStateDx);
        float& dy = Field<float>(out, kStateDx + 4);
        const int mode = s_mode.load(std::memory_order_relaxed);
        const bool tickRead = !OnCurrentStack(out); // 30 Hz gameplay tick writes a persistent buffer
        if (tickRead) {
            if (s_tickOut.exchange(out) != out)
                Log("Player input buffer (30 Hz tick) at %p, caller 0x%llx", static_cast<void*>(out), GhidraVa(reinterpret_cast<void*>(ret)));
            s_tickQpc = QpcNow();
        }
        if (mode >= static_cast<int>(MouseMode::TickExact)) {
            // Every per-frame drain is summed; each tick takes the whole sum exactly once.
            if (tickRead) {
                dx = s_tickAccDx;
                dy = s_tickAccDy;
                s_tickAccDx = s_tickAccDy = 0.f;
                rawcam::OnTickRead(dx, dy);
            } else {
                // Only while gameplay ticks consume input; in menus / loading the movement is
                // dropped instead of piling up for the first tick after resuming.
                if (QpcToMs(QpcNow() - s_tickQpc.load(std::memory_order_relaxed)) < 100.0) {
                    s_tickAccDx += dx;
                    s_tickAccDy += dy;
                } else {
                    s_tickAccDx = s_tickAccDy = 0.f;
                }
                rawcam::SetLiveAccum(s_tickAccDx, s_tickAccDy);
            }
        } else if (mode >= static_cast<int>(MouseMode::Compensated)) {
            if (tickRead && mode == static_cast<int>(MouseMode::FrameFeed)) {
                // Same value the per-frame feed wrote this frame.
                dx = s_feedDx;
                dy = s_feedDy;
            } else {
                // The camera scales the delta by (frame time / 33.3 ms); undo that so the per-frame
                // delta lands 1:1. Clamp guards against stalls (alt-tab, loading).
                float interval = s_drainIntervalMs.load(std::memory_order_relaxed);
                if (interval < 0.25f) interval = 0.25f;
                if (interval > s_throttleOrig) interval = s_throttleOrig;
                const float scale = s_throttleOrig / interval;
                dx *= scale;
                dy *= scale;
            }
            if (!tickRead && mode == static_cast<int>(MouseMode::FrameFeed)) {
                s_feedDx = dx;
                s_feedDy = dy;
                // Refresh the player's input buffer every frame while gameplay ticks are running.
                uint8_t* tick = s_tickOut.load(std::memory_order_relaxed);
                if (tick && QpcToMs(QpcNow() - s_tickQpc.load(std::memory_order_relaxed)) < 100.0) {
                    Field<float>(tick, kStateDx) = dx;
                    Field<float>(tick, kStateDx + 4) = dy;
                }
            }
        }
        if (dx != 0.f || dy != 0.f) st_gsNonZero.fetch_add(1, std::memory_order_relaxed);
        EvPush(Ev::GetState, static_cast<int32_t>(r & 0xFF), s_mode.load(std::memory_order_relaxed), dx, dy, ret);
    }
    return r;
}

void __fastcall HkLookInput(uint8_t* comp, float dt)
{
    if (s_camComp.exchange(comp) != comp) Log("Camera look-input component at %p", static_cast<void*>(comp));
    st_look.fetch_add(1, std::memory_order_relaxed);
    st_lookDt.store(dt, std::memory_order_relaxed);
    if (uint8_t* settings = Field<uint8_t*>(comp, kCamSettings))
        st_lookWindow.store(Field<int32_t>(settings, kCamSettingsWindow), std::memory_order_relaxed);
    st_lookFlag.store(Field<uint8_t>(comp, kCamNoSmooth), std::memory_order_relaxed);

    if (s_mode.load(std::memory_order_relaxed) >= static_cast<int>(MouseMode::TickExact)) {
        uint8_t& noSmooth = Field<uint8_t>(comp, kCamNoSmooth);
        const uint8_t saved = noSmooth;
        noSmooth = 1;
        s_origLookInput(comp, dt);
        noSmooth = saved;
    } else {
        s_origLookInput(comp, dt);
    }
    rawcam::OnLookInput(comp, dt);
}

void InstallLookInputHook()
{
    // Prologue: mov rax,rsp; push rdi; sub rsp,120h; movaps [rax-68h],xmm9; mov rdi,rcx; movaps [rax-88h],xmm11
    const auto& m = mem::Exe();
    uint8_t* fn = mem::FindPattern(m.text.begin, m.text.end - m.text.begin,
                                   "48 8B C4 57 48 81 EC 20 01 00 00 44 0F 29 48 98 48 8B F9 44 0F 29 98 78");
    if (!fn) {
        Log("LookInput: function pattern not found -> TickExact smoothing bypass disabled");
        return;
    }
    // Field-layout checks: mov r13,[rcx+30h] / movups xmm6,[rcx+120h] / cmp byte [rdi+100h],0 / movsxd r15,[r13+38h]
    const bool ok = mem::FindPattern(fn, 0x60, "4C 8B 69 30") && mem::FindPattern(fn, 0x60, "0F 10 B1 20 01 00 00") &&
                    mem::FindPattern(fn, 0x300, "80 BF 00 01 00 00 00") && mem::FindPattern(fn, 0x300, "4D 63 7D 38");
    uint8_t* site = ok ? mem::FindUniqueCallTo(fn) : nullptr;
    if (!site) {
        Log("LookInput: layout check %s, call site %s -> hook disabled", ok ? "ok" : "FAILED", site ? "ok" : "not unique");
        return;
    }
    s_origLookInput = static_cast<LookInputFn>(mem::RedirectCall(site, reinterpret_cast<void*>(&HkLookInput)));
    Log("LookInput at 0x%llx, call site 0x%llx -> %s", GhidraVa(fn), GhidraVa(site), s_origLookInput ? "hooked" : "redirect FAILED");
}

UINT WINAPI HkGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdrSize)
{
    const UINT r = s_origGetRawInputData(h, cmd, data, size, hdrSize);
    if (cmd == RID_INPUT && data && r != static_cast<UINT>(-1) && r >= sizeof(RAWINPUTHEADER)) {
        const auto* ri = static_cast<const RAWINPUT*>(data);
        if (ri->header.dwType == RIM_TYPEMOUSE) {
            const RAWMOUSE& m = ri->data.mouse;
            st_raw.fetch_add(1, std::memory_order_relaxed);
            if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                st_rawSumX.fetch_add(m.lLastX, std::memory_order_relaxed);
                st_rawSumY.fetch_add(m.lLastY, std::memory_order_relaxed);
            }
            EvPush(Ev::RawInput, m.lLastX, m.lLastY, static_cast<float>(m.usFlags), static_cast<float>(m.usButtonFlags));
        }
    }
    return r;
}

// --- validation helpers --------------------------------------------------------------------
uint8_t* Code(void* fn) { return static_cast<uint8_t*>(fn); }

bool InText(void* p) { return mem::Exe().text.contains(p); }
bool InReadable(void* p)
{
    const auto& m = mem::Exe();
    return m.rdata.contains(p) || m.data.contains(p) ||
           (p >= m.data.begin && p < m.data.begin + 0x8000000); // .data incl. BSS
}

void InstallKeyboardControllerHooks()
{
    void** vt = mem::FindVTableByRtti(".?AVKeyBoardController@@");
    if (!vt) {
        Log("KeyBoardController: RTTI vtable NOT found -> controller hooks disabled");
        return;
    }
    Log("KeyBoardController vtable at exe+0x%llx", static_cast<unsigned long long>(Code(vt) - mem::Exe().base));

    void* upd = vt[kSlotUpdate];
    void* gs = vt[kSlotGetState];
    if (!InText(upd) || !InText(gs)) {
        Log("KeyBoardController: vtable slots outside .text -> controller hooks disabled");
        return;
    }

    // Update (slot 6): rdtsc; movss xmm0,[tscPerMs]; xor ecx,ecx; mulss xmm0,[33.333f]
    uint8_t* thr = mem::FindPattern(Code(upd), 0x80, "0F 31 F3 0F 10 05 ?? ?? ?? ?? 33 C9 F3 0F 59 05 ?? ?? ?? ??");
    if (thr) {
        auto* tsc = reinterpret_cast<float*>(mem::RipTarget(thr + 2, 4, 8));
        auto* c = reinterpret_cast<float*>(mem::RipTarget(thr + 12, 4, 8));
        if (InReadable(tsc) && mem::Exe().rdata.contains(c) && *c > 1.f && *c < 1000.f) {
            s_tscPerMs = tsc;
            s_throttleConst = c;
            s_throttleOrig = *c;
            Log("Update: throttle constant %.4f ms at exe+0x%llx, TSC/ms global at exe+0x%llx", *c,
                static_cast<unsigned long long>(Code(c) - mem::Exe().base),
                static_cast<unsigned long long>(Code(tsc) - mem::Exe().base));
        }
    }
    if (!s_throttleConst) Log("Update: throttle pattern not matched -> mouse modes disabled");

    // Field layout: mov r9,[rdi+20h] / add rax,[rdi+58h] / add [rdi+48h],eax
    const bool fMouse = mem::FindPattern(Code(upd), 0x40, "4C 8B 4F 20") != nullptr;
    const bool fPoll = mem::FindPattern(Code(upd), 0x180, "48 03 47 58") != nullptr;
    const bool fDelta = mem::FindPattern(Code(upd), 0x180, "01 47 48") != nullptr;
    // GetState: movd xmm0,[r13+48h]; cvtdq2ps; mulss xmm0,[k]; movss [r14+8],xmm0
    uint8_t* gsConv = mem::FindPattern(Code(gs), 0x400, "66 41 0F 6E 45 48 0F 5B C0 F3 0F 59 05 ?? ?? ?? ?? F3 41 0F 11 46 08");
    s_layoutOk = fMouse && fPoll && fDelta && gsConv;
    if (gsConv) {
        auto* k = reinterpret_cast<float*>(mem::RipTarget(gsConv + 9, 4, 8));
        if (InReadable(k)) Log("GetState: mouse scale k = %g at exe+0x%llx", *k, static_cast<unsigned long long>(Code(k) - mem::Exe().base));
    }
    Log("Layout checks: mouse=%d lastPoll=%d delta=%d stateConv=%d -> field logging %s", fMouse, fPoll, fDelta,
        gsConv != nullptr, s_layoutOk ? "ON" : "OFF");

    s_origUpdate = static_cast<UpdateFn>(mem::PatchPointer(&vt[kSlotUpdate], reinterpret_cast<void*>(&HkUpdate)));
    s_origGetState = static_cast<GetStateFn>(mem::PatchPointer(&vt[kSlotGetState], reinterpret_cast<void*>(&HkGetState)));
    Log("Hooked KeyBoardController::Update (slot %d) and ::GetState (slot %d)", kSlotUpdate, kSlotGetState);

}

// World clock: cmp [rdi+93Ah],sil ; mov ecx,[divider] ; movss xmm1,[rdi+0C0h] ; cmovne ecx,r15d ; imul ecx,[baseStep]
int32_t* s_simDivider = nullptr;
int32_t s_simDividerOrig = 0;

void InstallSimDivider()
{
    const auto& m = mem::Exe();
    uint8_t* p = mem::FindPattern(m.text.begin, m.text.end - m.text.begin,
                                  "40 38 B7 3A 09 00 00 8B 0D ?? ?? ?? ?? F3 0F 10 8F C0 00 00 00 41 0F 45 CF 0F AF 0D");
    if (!p) {
        Log("SimDivider: pattern not found -> experiment disabled");
        return;
    }
    auto* div = reinterpret_cast<int32_t*>(mem::RipTarget(p + 7, 2, 6));
    if (!InReadable(div) || *div < 1 || *div > 8) {
        Log("SimDivider: unexpected value -> experiment disabled");
        return;
    }
    s_simDivider = div;
    s_simDividerOrig = *div;
    Log("SimDivider at exe+0x%llx = %d", static_cast<unsigned long long>(Code(div) - mem::Exe().base), *div);
}

void InstallRawInputHook()
{
    void** slot = mem::FindIatSlot("USER32.dll", "GetRawInputData");
    if (!slot) {
        Log("GetRawInputData: IAT slot not found -> raw packet logging disabled");
        return;
    }
    s_origGetRawInputData = static_cast<GetRawInputDataFn>(mem::PatchPointer(slot, reinterpret_cast<void*>(&HkGetRawInputData)));
    Log("Hooked USER32!GetRawInputData (IAT)");
}

const char* ModeName(int m)
{
    switch (m) {
    case 0: return "original";
    case 1: return "unthrottled";
    case 2: return "compensated";
    case 3: return "frame-feed";
    case 4: return "tick-exact";
    case 5: return "raw-camera";
    }
    return "?";
}

} // namespace

void InstallInputHooks()
{
    InstallRawInputHook();
    InstallKeyboardControllerHooks();
    InstallLookInputHook();
    InstallSimDivider();
    if (s_origLookInput) rawcam::Install();
    else Log("RawCam: needs the LookInput hook -> disabled");

    // Applied after every installer, so modes that need later hooks (5) can start enabled.
    if (g_cfg.startMode > 0 && g_cfg.startMode < static_cast<int>(MouseMode::Count))
        SetMouseMode(static_cast<MouseMode>(g_cfg.startMode));
}

bool SimDividerAvailable() { return s_simDivider != nullptr; }
int GetSimDivider() { return s_simDivider ? *s_simDivider : 0; }

void SetSimDivider(int divider)
{
    if (!s_simDivider) return;
    const int32_t v = divider;
    if (mem::WriteProtected(s_simDivider, &v, sizeof(v))) Log("SimDivider -> %d (game default %d)", v, s_simDividerOrig);
}

bool ModesAvailable() { return s_throttleConst != nullptr && s_layoutOk; }
MouseMode GetMouseMode() { return static_cast<MouseMode>(s_mode.load()); }

void SetMouseMode(MouseMode m)
{
    if (!ModesAvailable()) return;
    const float v = m == MouseMode::Original ? s_throttleOrig : 0.f;
    if (m == MouseMode::RawCamera && !rawcam::Available()) {
        Log("Mouse mode 5 unavailable (raw camera hooks missing)");
        return;
    }
    if (mem::WriteProtected(s_throttleConst, &v, sizeof(v))) {
        s_tickAccDx = s_tickAccDy = 0.f;
        rawcam::SetEnabled(m == MouseMode::RawCamera);
        s_mode = static_cast<int>(m);
        EvPush(Ev::Throttle, static_cast<int32_t>(m));
        Log("Mouse mode -> %d (%s), throttle constant = %.4f", static_cast<int>(m), ModeName(static_cast<int>(m)), v);
    }
}

bool ToggleStateWatch()
{
    if (watch::Armed()) {
        watch::Report("final");
        watch::Disarm();
        return true;
    }
    // Camera entity world transform: position (3 doubles at +0x120) and the rotation matrix that
    // follows it. Per-frame readers of these are the render-side camera interpolation.
    uint8_t* cam = s_camComp.load();
    uint8_t* entity = cam ? Field<uint8_t*>(cam, kCamEntity) : nullptr;
    if (!entity) {
        Log("watch: camera entity not discovered yet (load into gameplay first)");
        return false;
    }
    Log("watch: camera entity %p (from look component %p)", static_cast<void*>(entity), static_cast<void*>(cam));
    const watch::Region regions[] = {
        {entity + kEntityPos, 8, "entity.pos.x"},
        {entity + kEntityPos + 0x18, 8, "entity.rot[0..1]"},
        {entity + kEntityPos + 0x28, 8, "entity.rot[4..5]"},
        {cam + kCamLook, 8, "cam.look"},
    };
    return watch::Arm(regions, 4);
}

void InputStatsTick(double elapsedMs)
{
    // Measured TSC rate vs the game's registry-derived calibration.
    static int64_t lastQpc = 0;
    static uint64_t lastTsc = 0;
    const int64_t q = QpcNow();
    const uint64_t t = __rdtsc();
    double measuredTscPerMs = 0;
    if (lastQpc) measuredTscPerMs = static_cast<double>(t - lastTsc) / QpcToMs(q - lastQpc);
    lastQpc = q;
    lastTsc = t;

    if (watch::Armed()) watch::Report("watch");

    const double s = elapsedMs / 1000.0;
    const uint32_t raw = st_raw.exchange(0), upd = st_upd.exchange(0), drain = st_drain.exchange(0);
    const uint32_t gs = st_gs.exchange(0), gsNz = st_gsNonZero.exchange(0);
    const int64_t sx = st_rawSumX.exchange(0), sy = st_rawSumY.exchange(0);
    if (!raw && !upd && !gs) return; // idle (menus without mouse movement, loading, etc.)

    Log("1s: raw=%.0f/s (sum %lld,%lld) | Update=%.0f/s drains=%.1f/s (last %.2f ms) tid=%u | GetState=%.0f/s "
        "nonzero=%.0f/s tid=%u | mode=%s | TSC/ms game=%.1f measured=%.1f | dropped=%llu",
        raw / s, static_cast<long long>(sx), static_cast<long long>(sy), upd / s, drain / s,
        s_drainIntervalMs.load(), st_updThread.load(), gs / s, gsNz / s, st_gsThread.load(), ModeName(s_mode.load()),
        s_tscPerMs ? *s_tscPerMs : 0.f, measuredTscPerMs, static_cast<unsigned long long>(EvDropped()));
    rawcam::StatsTick(elapsedMs);
    if (const uint32_t look = st_look.exchange(0))
        Log("    LookInput=%.1f/s dt=%.5f window=%d noSmoothFlag=%d", look / s, st_lookDt.load(), st_lookWindow.load(),
            st_lookFlag.load());

    // Report newly seen callers once.
    static uintptr_t reported[2][kMaxCallers] = {};
    for (int which = 0; which < 2; ++which) {
        auto* table = which ? s_gsCallers : s_updCallers;
        for (int i = 0; i < kMaxCallers; ++i) {
            const uintptr_t c = table[i].load();
            if (c && reported[which][i] != c) {
                reported[which][i] = c;
                Log("  new %s caller: 0x%llx (Ghidra VA)", which ? "GetState" : "Update", GhidraVa(reinterpret_cast<void*>(c)));
            }
        }
    }
}
