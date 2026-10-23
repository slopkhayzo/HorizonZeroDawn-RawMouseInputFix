// Raw camera (mouse mode 5). See raw_camera.h for the overall design and research/docs/PLAN.md
// ("Test 8–11") for the reverse-engineering notes behind the offsets used here.
//
// The renderer draws the camera entity at interpolate(entity+0xE0, entity+0x120, alpha), alpha
// running 0..1 across each 29.97 Hz camera tick, so stock mouse look is always ~1 tick behind and
// moves in straight segments between tick poses. In this mode both endpoints are written:
//   - the camera tick's own SetWorldTransform call is redirected: it stores the new tick pose and
//     the previous one advanced by the look this tick applied, both rotated by the counts not yet
//     applied,
//   - once per frame (game thread, after the mouse drain) both endpoints are refreshed with the new
//     offset (entity lock; +0x120 through the game's SetWorldTransform for its change flag).
// Mouse rotation is then exact at any alpha; the pivot's movement and the game's own extra rotation
// keep the game's interpolation. If anything else moved the camera (cutscene, camera switch,
// teleport), we stop until the next camera tick. While the game ignores mouse look (UI over the
// running world, e.g. looting) no offset is applied.
#include "raw_camera.h"
#include "log.h"
#include "mem.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace rawcam {
namespace {

// --- layout (validated once in Install) ----------------------------------------------------
// Camera look-input component (`comp`, owner of the per-tick look processing):
constexpr size_t kCompEntity = 0x48;   // camera entity
constexpr size_t kCompMode = 0x70;     // active camera mode
constexpr size_t kCompLook = 0x120;    // processed look vector for this tick
constexpr size_t kCompSensX = 0x140;   // sensitivity factors applied by the camera mode
constexpr size_t kCompSensY = 0x144;
// Camera mode:
constexpr size_t kModeClamp = 0x50;    // pitch limiter object, vfunc +0xB0: float(obj, pitch)
constexpr size_t kModePivot = 0x70;    // orbit pivot world position (3 doubles)
constexpr size_t kClampVfunc = 0xB0;
// Entity: CRITICAL_SECTION guarding the transform at +0x50; world transform at +0x120
// (position 3 doubles, rotation rows right/forward/up as floats from +0x138).
constexpr size_t kEntityLock = 0x50;
constexpr size_t kEntityPrev = 0xE0;  // interpolation start pose (same layout as +0x120)
constexpr size_t kEntityPos = 0x120;
constexpr size_t kEntityRot = 0x138;
constexpr int kGetTransformSlot = 44;

struct WorldTransform {
    double pos[3];
    float rot[9]; // rows: right, forward, up
};
constexpr size_t kTransformBytes = 0x3C; // what the game copies (no tail padding)

using CurveEvalFn = float(__fastcall*)(void* curve, float x);
using AccelFn = float(__fastcall*)(void* settings);
using MakeViewFn = uintptr_t(__fastcall*)(void* out, const double* pos, const float* a, const float* b, const float* c,
                                         float f6, float f7, float f8, float f9, int i10);
using GetTransformFn = WorldTransform*(__fastcall*)(uint8_t* entity, WorldTransform* out);
using SetTransformFn = void(__fastcall*)(uint8_t* entity, const WorldTransform* xform);
using ClampFn = float(__fastcall*)(void* obj, float pitch);

CurveEvalFn s_origCurve = nullptr;
AccelFn s_origAccelYaw = nullptr;
AccelFn s_origAccelPitch = nullptr;
MakeViewFn s_origMakeView = nullptr;
GetTransformFn s_origGetTransform = nullptr;
uint8_t* s_getTransformFn = nullptr;
SetTransformFn s_setTransform = nullptr; // the game's Entity::SetWorldTransform
const float* s_deg2rad = nullptr;

std::atomic<bool> s_enabled{false};
std::atomic<int> s_debugPath{0};
constexpr double kDebugYaw = 0.349066; // 20 degrees

// --- tick bookkeeping (game thread) --------------------------------------------------------
// Each 30 Hz tick read consumes the accumulated look; that amount stays "pending" until the
// camera tick that uses it has written the camera transform.
struct Pending {
    uint32_t seq;
    float ax, ay;
};
constexpr int kMaxPending = 6;
Pending s_pending[kMaxPending];
int s_pendingCount = 0;
uint32_t s_readSeq = 0;
float s_liveX = 0.f, s_liveY = 0.f;

// Published by the game thread for the camera job.
std::atomic<uint32_t> s_lastReadSeq{0};
std::atomic<float> s_lastReadAx{0.f}, s_lastReadAy{0.f};

// Published by the camera job.
std::atomic<uint8_t*> s_comp{nullptr};
std::atomic<uint32_t> s_tickSeq{0};       // tick read consumed by the camera tick in flight
std::atomic<uint32_t> s_committedSeq{0};  // tick read included in the stored transform
std::atomic<float> s_sensX{0.f}, s_sensY{0.f}, s_dt{0.f};
std::atomic<float> s_signX{1.f}, s_signY{1.f};
std::atomic<float> s_slope{0.f};
// Look the camera tick actually applied (processed look x sign = consumed units), and whether the
// game uses mouse look at all right now: in UI over a running world (looting, dialogue choices)
// it consumes the counts but the look vector stays zero.
std::atomic<float> s_tickAppliedX{0.f}, s_tickAppliedY{0.f};
std::atomic<bool> s_lookActive{true};

// Published by the game thread once per frame for the camera job (seqlock).
struct Published {
    float liveX, liveY;
    int count;
    Pending pending[kMaxPending];
};
std::atomic<uint32_t> s_pubSeq{0};
Published s_pub{};

// The renderer draws the camera at interpolate(entity+0xE0 "from", entity+0x120 "to", alpha) with
// alpha running 0..1 over each tick (scene node update, FUN_140bd6dc0 / FUN_1402c7fa0). We keep
//   to   = latest tick pose                                   (about the current pivot)
//   from = previous tick pose advanced by the look this tick applied (about the previous pivot)
// and rotate both by the counts not yet applied, so mouse rotation is exact at any alpha while the
// game's own extra rotation (follow alignment) and the pivot's translation stay interpolated.
// Guarded by the entity's own lock (entity+0x50).
WorldTransform s_to{}, s_from{};
double s_toPivot[3] = {}, s_fromPivot[3] = {};
double s_pitchMin = -1.55, s_pitchMax = 1.55; // camera mode limits sampled at each tick
// Camera entity of the last commit, guarded by s_trackLock. The camera entity destructor hook
// clears it (under the lock) before the object is torn down, so while the lock is held a non-null
// value is a live entity. Lock order: entity lock -> s_trackLock; the per-frame writer holds
// s_trackLock and only *tries* the entity lock, so it can never deadlock.
SRWLOCK s_trackLock = SRWLOCK_INIT;
uint8_t* s_trackedEntity = nullptr;
bool s_dtorHooked = false;

using DtorFn = void*(__fastcall*)(uint8_t* self, uint32_t flags);
struct DtorEntry {
    void** vtable;
    DtorFn orig;
};
constexpr int kMaxCameraClasses = 16;
DtorEntry s_dtors[kMaxCameraClasses];
int s_dtorCount = 0;
WorldTransform s_written{};  // what we last stored at entity+0x120
uint8_t* s_baseEntity = nullptr;
bool s_baseValid = false;

std::atomic<uint32_t> st_frames{0}, st_frameWrites{0}, st_tickWrites{0}, st_foreign{0}, st_calls{0};
std::atomic<float> st_maxYaw{0.f}, st_maxPitch{0.f}, st_pivotDist{0.f};

// Commit diagnostics (camera job): game's per-tick turn vs. the turn predicted from the look.
// Guarded by the entity lock; read racily by the stats line.
float st_errYawSum = 0.f, st_errYawMax = 0.f, st_errPitchSum = 0.f, st_errPitchMax = 0.f;
float st_ratioSum = 0.f;
uint32_t st_errN = 0, st_ratioN = 0;

// Frame timing: last per-frame pose write (game thread) -> next Present (render thread).
std::atomic<int64_t> s_lastWriteQpc{0};
std::atomic<int64_t> st_w2pSumUs{0}, st_blockSumUs{0};
std::atomic<int64_t> st_w2pMaxUs{0}, st_blockMaxUs{0};
std::atomic<uint32_t> st_presents{0}, st_w2pN{0};
std::atomic<unsigned> st_sync{0};

template <class T> T& Field(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

bool InText(const void* p) { return mem::Exe().text.contains(p); }

void AtomicMax(std::atomic<float>& a, float v)
{
    float cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

LPCRITICAL_SECTION EntityLock(uint8_t* entity) { return reinterpret_cast<LPCRITICAL_SECTION>(entity + kEntityLock); }

void ReadEntityTransform(uint8_t* entity, WorldTransform& t) { std::memcpy(&t, entity + kEntityPos, kTransformBytes); }

bool SameTransform(const WorldTransform& a, const WorldTransform& b) { return std::memcmp(&a, &b, kTransformBytes) == 0; }

// --- tick-side hooks -------------------------------------------------------------------------
float __fastcall HkLookCurve(void* curve, float x)
{
    if (!s_enabled.load(std::memory_order_relaxed)) return s_origCurve(curve, x);
    // Linear response with the scale the game uses while aiming: |look| * curve(1.0).
    const float slope = s_origCurve(curve, 1.0f);
    s_slope.store(slope, std::memory_order_relaxed);
    return x * slope;
}

float __fastcall HkAccelYaw(void* settings)
{
    return s_enabled.load(std::memory_order_relaxed) ? 0.f : s_origAccelYaw(settings);
}

float __fastcall HkAccelPitch(void* settings)
{
    return s_enabled.load(std::memory_order_relaxed) ? 0.f : s_origAccelPitch(settings);
}

// --- math ------------------------------------------------------------------------------------
struct V3 {
    double x, y, z;
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double Len(V3 a) { return std::sqrt(Dot(a, a)); }

// Rodrigues rotation of v about unit axis k by angle t.
V3 Rotate(V3 v, V3 k, double t)
{
    const double c = std::cos(t), s = std::sin(t);
    return v * c + Cross(k, v) * s + k * (Dot(k, v) * (1.0 - c));
}

V3 FromF(const float* p) { return {p[0], p[1], p[2]}; }
void ToF(V3 v, float* p)
{
    p[0] = static_cast<float>(v.x);
    p[1] = static_cast<float>(v.y);
    p[2] = static_cast<float>(v.z);
}

// Rotate a camera transform by the look amount (lx, ly) with the same expression the camera
// mode integrates per tick (linear curve, no acceleration limit):
//   heading -= sensX*slope*deg2rad*dt*lookX,  pitch += sensY*slope*deg2rad*dt*lookY
// about `pivot` (pitch about the horizontal right axis with the game's own pitch limiter, then
// yaw about world up +Z). Any thread.
bool ApplyLook(WorldTransform& t, float lx, float ly, const double* pivot, bool stats = true)
{
    if (lx == 0.f && ly == 0.f) return false;
    const float sensX = s_sensX.load(std::memory_order_relaxed), sensY = s_sensY.load(std::memory_order_relaxed);
    const float dt = s_dt.load(std::memory_order_relaxed), slope = s_slope.load(std::memory_order_relaxed);
    if (dt <= 0.f || slope == 0.f || !s_deg2rad) return false;

    const double k = static_cast<double>(slope) * *s_deg2rad * dt;
    const double dYaw = -k * sensX * s_signX.load(std::memory_order_relaxed) * lx;
    double dPitch = k * sensY * s_signY.load(std::memory_order_relaxed) * ly;

    const V3 C{t.pos[0], t.pos[1], t.pos[2]};
    V3 axes[3] = {FromF(t.rot), FromF(t.rot + 3), FromF(t.rot + 6)};
    const V3 f = axes[1];

    V3 P = C;
    if (pivot) {
        const V3 pv{pivot[0], pivot[1], pivot[2]};
        const double dist = Len(pv - C);
        if (stats) st_pivotDist.store(static_cast<float>(dist), std::memory_order_relaxed);
        if (dist > 0.05 && dist < 30.0) P = pv;
    }

    // Pitch limits of the camera mode, sampled at the last tick (no game calls on this path).
    const double pitch0 = std::atan2(f.z, std::sqrt(f.x * f.x + f.y * f.y));
    if (dPitch > 0.0 && pitch0 + dPitch > s_pitchMax) dPitch = (std::max)(0.0, s_pitchMax - pitch0);
    if (dPitch < 0.0 && pitch0 + dPitch < s_pitchMin) dPitch = (std::min)(0.0, s_pitchMin - pitch0);

    const V3 up{0, 0, 1};
    V3 right = Cross(f, up);
    const double rl = Len(right);
    if (rl < 1e-6) dPitch = 0.0;
    else right = right * (1.0 / rl);

    V3 rel = C - P;
    for (V3* v : {&rel, &axes[0], &axes[1], &axes[2]}) {
        if (dPitch != 0.0) *v = Rotate(*v, right, dPitch); // +angle tilts forward towards +Z
        *v = Rotate(*v, up, dYaw);
    }
    const V3 Cn = P + rel;
    t.pos[0] = Cn.x;
    t.pos[1] = Cn.y;
    t.pos[2] = Cn.z;
    ToF(axes[0], t.rot);
    ToF(axes[1], t.rot + 3);
    ToF(axes[2], t.rot + 6);

    if (stats) {
        AtomicMax(st_maxYaw, static_cast<float>(std::fabs(dYaw)));
        AtomicMax(st_maxPitch, static_cast<float>(std::fabs(dPitch)));
    }
    return true;
}

double Heading(const WorldTransform& t) { return std::atan2(-static_cast<double>(t.rot[3]), static_cast<double>(t.rot[4])); }
double Pitch(const WorldTransform& t)
{
    const double fx = t.rot[3], fy = t.rot[4], fz = t.rot[5];
    return std::atan2(fz, std::sqrt(fx * fx + fy * fy));
}
double WrapPi(double a)
{
    while (a > 3.14159265358979) a -= 6.28318530717959;
    while (a < -3.14159265358979) a += 6.28318530717959;
    return a;
}

bool ReadPublished(Published& out)
{
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t s0 = s_pubSeq.load(std::memory_order_acquire);
        if (s0 & 1) { YieldProcessor(); continue; }
        std::memcpy(&out, &s_pub, sizeof(out));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s_pubSeq.load(std::memory_order_relaxed) == s0) return true;
    }
    return false;
}

// The camera component is only dereferenced outside its own tick while camera ticks are running:
// in menus, loading screens and at quit they stop, and the component may be freed afterwards.
constexpr double kLiveMs = 100.0;
std::atomic<int64_t> s_lastLookQpc{0};

bool CameraLive()
{
    const int64_t t = s_lastLookQpc.load(std::memory_order_acquire);
    return t && QpcToMs(QpcNow() - t) < kLiveMs;
}

uint8_t* CameraEntity()
{
    if (!CameraLive()) return nullptr;
    uint8_t* comp = s_comp.load(std::memory_order_acquire);
    return comp ? Field<uint8_t*>(comp, kCompEntity) : nullptr;
}

// Write both interpolation endpoints rotated by (lx, ly). Caller holds the entity lock.
void WriteEndpoints(uint8_t* entity, float lx, float ly, bool stats)
{
    WorldTransform to = s_to, from = s_from;
    ApplyLook(to, lx, ly, s_toPivot, stats);
    ApplyLook(from, lx, ly, s_fromPivot, false);
    s_setTransform(entity, &to); // +0x120, sets the "transform changed" flag
    std::memcpy(entity + kEntityPrev, &from, kTransformBytes);
    ReadEntityTransform(entity, s_written);
}

// --- camera tick commit (job thread) ---------------------------------------------------------
// Redirected call `SetWorldTransform(cameraEntity, comp+0xA8)` at the end of the camera tick.
void __fastcall HkSetCameraTransform(uint8_t* entity, const WorldTransform* xform)
{
    if (!s_enabled.load(std::memory_order_relaxed) || s_debugPath.load(std::memory_order_relaxed) || entity != CameraEntity()) {
        s_setTransform(entity, xform);
        return;
    }
    // This tick consumed reads <= s_tickSeq; what is left is live + newer reads.
    const uint32_t committed = s_tickSeq.load(std::memory_order_acquire);
    float lx = 0.f, ly = 0.f;
    Published pub;
    if (s_lookActive.load(std::memory_order_relaxed) && ReadPublished(pub)) {
        lx = pub.liveX;
        ly = pub.liveY;
        for (int i = 0; i < pub.count; ++i)
            if (static_cast<int32_t>(pub.pending[i].seq - committed) > 0) {
                lx += pub.pending[i].ax;
                ly += pub.pending[i].ay;
            }
    }

    // Pivot of this tick (the camera mode has already run for this tick).
    // Also sample the mode's pitch limits here, while the camera is certainly alive.
    double pivotNew[3] = {xform->pos[0], xform->pos[1], xform->pos[2]};
    double pitchMin = -1.55, pitchMax = 1.55;
    if (uint8_t* mode = Field<uint8_t*>(s_comp.load(std::memory_order_acquire), kCompMode)) {
        std::memcpy(pivotNew, &Field<double>(mode, kModePivot), sizeof(pivotNew));
        void* clampObj = Field<void*>(mode, kModeClamp);
        void** vt = clampObj ? *static_cast<void***>(clampObj) : nullptr;
        if (vt && InText(vt[kClampVfunc / 8])) {
            const auto clamp = reinterpret_cast<ClampFn>(vt[kClampVfunc / 8]);
            pitchMin = clamp(clampObj, -1.55f);
            pitchMax = clamp(clampObj, 1.55f);
            if (!(pitchMin < pitchMax)) { pitchMin = -1.55; pitchMax = 1.55; }
        }
    }

    EnterCriticalSection(EntityLock(entity));
    WorldTransform actual;
    std::memcpy(&actual, xform, kTransformBytes);
    bool interpolate = false;
    if (s_baseValid && s_baseEntity == entity) {
        // Look this tick actually applied (from the game's processed look vector, so input the
        // game ignored - UI over a running world - is not assumed to have turned the camera).
        const float cx = s_tickAppliedX.load(std::memory_order_relaxed);
        const float cy = s_tickAppliedY.load(std::memory_order_relaxed);
        // "from" = previous tick pose advanced by exactly that look: what remains between from
        // and to is the game's own rotation plus the pivot's movement.
        WorldTransform predicted = s_to;
        ApplyLook(predicted, cx, cy, s_toPivot, false);

        // Diagnostics: the game's own rotation per tick.
        const float ey = static_cast<float>(std::fabs(WrapPi(Heading(actual) - Heading(predicted))));
        const float ep = static_cast<float>(std::fabs(Pitch(actual) - Pitch(predicted)));
        st_errYawSum += ey;
        st_errPitchSum += ep;
        if (ey > st_errYawMax) st_errYawMax = ey;
        if (ep > st_errPitchMax) st_errPitchMax = ep;
        ++st_errN;
        const double dPred = WrapPi(Heading(predicted) - Heading(s_to));
        if (std::fabs(dPred) > 0.02) {
            st_ratioSum += static_cast<float>(WrapPi(Heading(actual) - Heading(s_to)) / dPred);
            ++st_ratioN;
        }

        // Do not interpolate across cuts / teleports.
        const V3 dp{pivotNew[0] - s_toPivot[0], pivotNew[1] - s_toPivot[1], pivotNew[2] - s_toPivot[2]};
        if (Len(dp) < 3.0 && ey < 0.35f && ep < 0.35f) {
            s_from = predicted;
            std::memcpy(s_fromPivot, s_toPivot, sizeof(s_fromPivot));
            interpolate = true;
        }
    }
    if (!interpolate) {
        s_from = actual;
        std::memcpy(s_fromPivot, pivotNew, sizeof(s_fromPivot));
    }
    s_to = actual;
    std::memcpy(s_toPivot, pivotNew, sizeof(s_toPivot));
    s_pitchMin = pitchMin;
    s_pitchMax = pitchMax;
    WriteEndpoints(entity, lx, ly, true);
    s_baseEntity = entity;
    s_baseValid = true;
    AcquireSRWLockExclusive(&s_trackLock);
    s_trackedEntity = entity;
    ReleaseSRWLockExclusive(&s_trackLock);
    s_committedSeq.store(committed, std::memory_order_release);
    LeaveCriticalSection(EntityLock(entity));
    st_tickWrites.fetch_add(1, std::memory_order_relaxed);
}

// --- render-side diagnostics (F12) -----------------------------------------------------------
void DebugYaw(float rot[9])
{
    const V3 up{0, 0, 1};
    for (int i = 0; i < 3; ++i) ToF(Rotate(FromF(rot + 3 * i), up, kDebugYaw), rot + 3 * i);
}

// Camera entity vtable slot 44 (virtual GetWorldTransform). Not on the render path (test 11);
// kept for the F12 diagnostic only.
WorldTransform* __fastcall HkGetTransform(uint8_t* entity, WorldTransform* out)
{
    WorldTransform* r = s_origGetTransform(entity, out);
    st_calls.fetch_add(1, std::memory_order_relaxed);
    if (s_debugPath.load(std::memory_order_relaxed) == 2 && entity == CameraEntity()) DebugYaw(out->rot);
    return r;
}

// Camera entity scalar deleting destructor (vtable slot 1) of every camera entity class: forget the
// tracked camera before it is destroyed.
void* __fastcall HkCamDtor(uint8_t* self, uint32_t flags)
{
    void** vt = *reinterpret_cast<void***>(self); // most-derived vtable at entry
    DtorFn orig = nullptr;
    for (int i = 0; i < s_dtorCount; ++i)
        if (s_dtors[i].vtable == vt) orig = s_dtors[i].orig;
    AcquireSRWLockExclusive(&s_trackLock);
    if (self == s_trackedEntity) s_trackedEntity = nullptr;
    ReleaseSRWLockExclusive(&s_trackLock);
    return orig(self, flags);
}

// Refresh both interpolation endpoints with the latest offset. Caller holds s_trackLock and passes
// the tracked (live) entity; the entity lock is only tried, never waited for.
void FrameWrite(uint8_t* entity)
{
    Published pub;
    if (!ReadPublished(pub)) return;
    const uint32_t committed = s_committedSeq.load(std::memory_order_acquire);
    float lx = 0.f, ly = 0.f;
    if (s_lookActive.load(std::memory_order_relaxed)) { // else the game ignores the mouse (UI)
        lx = pub.liveX;
        ly = pub.liveY;
        for (int i = 0; i < pub.count; ++i)
            if (static_cast<int32_t>(pub.pending[i].seq - committed) > 0) {
                lx += pub.pending[i].ax;
                ly += pub.pending[i].ay;
            }
    }
    if (!TryEnterCriticalSection(EntityLock(entity))) return; // busy (tick commit): skip this frame
    if (s_baseValid && s_baseEntity == entity) {
        WorldTransform cur;
        ReadEntityTransform(entity, cur);
        if (!SameTransform(cur, s_written)) {
            s_baseValid = false; // moved by someone else (cutscene, switch, teleport)
            st_foreign.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Always rewrite both endpoints: the game resets the "from" pose at each tick.
            WriteEndpoints(entity, lx, ly, true);
            st_frameWrites.fetch_add(1, std::memory_order_relaxed);
            s_lastWriteQpc.store(QpcNow(), std::memory_order_release);
        }
    }
    LeaveCriticalSection(EntityLock(entity));
}

// Per-frame view build for the active camera (FUN(cameraEntity, out) -> makeView(out, &entity.pos,
// ...)). Not the render path, and too late in the frame for the pose refresh (test 16); F12 only.
uintptr_t __fastcall HkMakeView(void* out, const double* pos, const float* a, const float* b, const float* c, float f6,
                                float f7, float f8, float f9, int i10)
{
    if (s_debugPath.load(std::memory_order_relaxed) != 1) return s_origMakeView(out, pos, a, b, c, f6, f7, f8, f9, i10);
    uint8_t* entity = CameraEntity();
    if (!entity || reinterpret_cast<const uint8_t*>(pos) != entity + kEntityPos)
        return s_origMakeView(out, pos, a, b, c, f6, f7, f8, f9, i10);

    float rot[9];
    std::memcpy(rot, entity + kEntityRot, sizeof(rot));
    DebugYaw(rot);
    alignas(16) float na[4] = {0, 0, 0, a[3]};
    alignas(16) float nb[4] = {rot[3], rot[4], rot[5], b[3]};
    alignas(16) float nc[4] = {rot[6], rot[7], rot[8], c[3]};
    ToF(Cross(FromF(nb), FromF(nc)), na); // the game passes cross(forward, up) first
    return s_origMakeView(out, pos, na, nb, nc, f6, f7, f8, f9, i10);
}

// Is `vt` the start of an MSVC vtable (vt[-1] = complete object locator with a matching self RVA)?
bool IsVtableStart(void** vt)
{
    const auto& m = mem::Exe();
    auto* col = static_cast<uint8_t*>(vt[-1]);
    if (!m.rdata.contains(col)) return false;
    const auto* c = reinterpret_cast<const uint32_t*>(col);
    return c[0] == 1 && c[5] == static_cast<uint32_t>(col - m.base);
}

void PatchCameraVtables()
{
    // Every camera entity class shares the slot-44 GetWorldTransform; that identifies their vtables.
    const auto& m = mem::Exe();
    int patched = 0;
    for (uint8_t* p = m.rdata.begin; p + 8 <= m.rdata.end; p += 8) {
        if (*reinterpret_cast<uint8_t**>(p) != s_getTransformFn) continue;
        mem::PatchPointer(reinterpret_cast<void**>(p), reinterpret_cast<void*>(&HkGetTransform));
        ++patched;
        void** vt = reinterpret_cast<void**>(p) - kGetTransformSlot;
        if (!IsVtableStart(vt) || !InText(vt[1]) || s_dtorCount == kMaxCameraClasses) continue;
        s_dtors[s_dtorCount++] = {vt, static_cast<DtorFn>(vt[1])};
        mem::PatchPointer(&vt[1], reinterpret_cast<void*>(&HkCamDtor));
    }
    s_dtorHooked = s_dtorCount > 0 && s_dtorCount == patched;
    Log("RawCam: camera entity classes: %d (GetWorldTransform slot 44 = diagnostic, destructor slot 1 hooked: %d) -> "
        "per-frame refresh %s",
        patched, s_dtorCount, s_dtorHooked ? "enabled" : "DISABLED (tick-only)");
}

// Publish live + pending for the camera tick commit (game thread). Called after every change, so
// a commit never counts a read that was already consumed as "live".
void Publish()
{
    s_pubSeq.fetch_add(1, std::memory_order_acq_rel); // odd: writing
    s_pub.liveX = s_liveX;
    s_pub.liveY = s_liveY;
    s_pub.count = s_pendingCount;
    std::memcpy(s_pub.pending, s_pending, sizeof(Pending) * s_pendingCount);
    s_pubSeq.fetch_add(1, std::memory_order_release); // even: stable
}

} // namespace

void Install()
{
    const auto& m = mem::Exe();
    const size_t textLen = m.text.end - m.text.begin;

    // Settings getters (inheritance chain via +0x28, override flag, value).
    uint8_t* getAccelYaw = mem::FindPattern(m.text.begin, textLen, "80 B9 E0 03 00 00 00 75 ?? 0F 1F 80 00 00 00 00 48 8B 41 28");
    uint8_t* getAccelPitch = mem::FindPattern(m.text.begin, textLen, "80 B9 F8 03 00 00 00 75 ?? 0F 1F 80 00 00 00 00 48 8B 41 28");
    uint8_t* getLookCurve = mem::FindPattern(m.text.begin, textLen, "48 83 B9 D0 03 00 00 00 75 ?? 66 0F 1F 44 00 00 48 8B 41 28");
    uint8_t* accelYawSite = getAccelYaw ? mem::FindUniqueCallTo(getAccelYaw) : nullptr;
    uint8_t* accelPitchSite = getAccelPitch ? mem::FindUniqueCallTo(getAccelPitch) : nullptr;

    // Look curve evaluation: call getLookCurve ; movss xmm1,[rbp+70h] ; lea rcx,[rax+28h] ; call curveEval
    uint8_t* curveSite = nullptr;
    for (uint8_t* p = m.text.begin; getLookCurve && p;) {
        p = mem::FindPattern(p, m.text.end - p, "E8 ?? ?? ?? ?? F3 0F 10 4D 70 48 8D 48 28 E8");
        if (!p) break;
        if (mem::RipTarget(p, 1, 5) == getLookCurve) {
            curveSite = p + 14;
            // deg->rad constant: mulss xmm0,[rip+X] right after the call.
            if (uint8_t* mul = mem::FindPattern(curveSite + 5, 0x20, "F3 0F 59 05"))
                s_deg2rad = reinterpret_cast<const float*>(mem::RipTarget(mul, 4, 8));
            break;
        }
        ++p;
    }

    // Camera tick commit: inside the camera component tick (which also calls LookInput):
    //   mov rsi,[rdi+48h] ; lea rdx,[rdi+0A8h] ; mov rcx,rsi ; movups xmm0,[rsi+120h] ... call SetWorldTransform
    uint8_t* commitSite = nullptr;
    if (uint8_t* p = mem::FindPattern(m.text.begin, textLen, "48 8B 77 48 48 8D 97 A8 00 00 00 48 8B CE 0F 10 86 20 01 00 00")) {
        uint8_t* call = mem::FindPattern(p, 0x40, "E8");
        // SetWorldTransform: ... mov rdi,rcx ; mov rsi,rdx ; add rcx,50h ; call [TryEnterCriticalSection]
        uint8_t* target = call ? mem::RipTarget(call, 1, 5) : nullptr;
        if (target && InText(target) && mem::FindPattern(target, 0x20, "48 8B F9 48 8B F2 48 83 C1 50 FF 15")) {
            commitSite = call;
            s_setTransform = reinterpret_cast<SetTransformFn>(target);
        }
    }

    // Per-frame view build (pose refresh + F12 makeView diagnostic) and camera vtable slot 44 (F12 only).
    uint8_t* camView = mem::FindPattern(m.text.begin, textLen,
                                        "48 8B C4 48 89 58 20 55 56 57 48 8D 6C 24 90 48 81 EC 70 01 00 00 0F 29 70 D8 "
                                        "48 8D 99 20 01 00 00 F3 0F 10 B1 44 03 00 00");
    uint8_t* viewSite = camView ? mem::FindPattern(camView, 0x200, "F3 44 0F 11 54 24 28 48 89 44 24 20 E8") : nullptr;
    if (viewSite) viewSite += 12;
    void** camVt = mem::FindVTableByRtti(".?AVCameraEntity@@");
    if (camVt && InText(camVt[kGetTransformSlot]) &&
        mem::FindPattern(static_cast<uint8_t*>(camVt[kGetTransformSlot]), 0x10, "0F 10 81 20 01 00 00"))
        s_getTransformFn = static_cast<uint8_t*>(camVt[kGetTransformSlot]);

    const bool degOk = s_deg2rad && mem::Exe().rdata.contains(s_deg2rad) && std::fabs(*s_deg2rad - 0.0174533f) < 1e-4f;
    Log("RawCam: accelYaw=%p accelPitch=%p curveSite=%p deg2rad=%s commitSite=%p setTransform=%p | diag: viewSite=%p getTransform=%p",
        static_cast<void*>(accelYawSite), static_cast<void*>(accelPitchSite), static_cast<void*>(curveSite),
        degOk ? "ok" : "MISSING", static_cast<void*>(commitSite), reinterpret_cast<void*>(s_setTransform),
        static_cast<void*>(viewSite), static_cast<void*>(s_getTransformFn));
    if (!accelYawSite || !accelPitchSite || !curveSite || !degOk || !commitSite || !viewSite) {
        Log("RawCam: a required site was not found -> raw camera disabled");
        return;
    }
    s_origAccelYaw = static_cast<AccelFn>(mem::RedirectCall(accelYawSite, reinterpret_cast<void*>(&HkAccelYaw)));
    s_origAccelPitch = static_cast<AccelFn>(mem::RedirectCall(accelPitchSite, reinterpret_cast<void*>(&HkAccelPitch)));
    s_origCurve = static_cast<CurveEvalFn>(mem::RedirectCall(curveSite, reinterpret_cast<void*>(&HkLookCurve)));
    void* origCommit = mem::RedirectCall(commitSite, reinterpret_cast<void*>(&HkSetCameraTransform));
    if (origCommit != reinterpret_cast<void*>(s_setTransform)) s_setTransform = nullptr;
    if (viewSite) s_origMakeView = static_cast<MakeViewFn>(mem::RedirectCall(viewSite, reinterpret_cast<void*>(&HkMakeView)));
    if (s_getTransformFn) {
        s_origGetTransform = reinterpret_cast<GetTransformFn>(s_getTransformFn);
        PatchCameraVtables();
    }
    Log("RawCam: hooks %s", Available() ? "installed" : "FAILED");
}

bool Available() { return s_origAccelYaw && s_origAccelPitch && s_origCurve && s_setTransform && s_origMakeView; }

void SetEnabled(bool on)
{
    s_enabled = on && Available();
    s_pendingCount = 0;
    s_liveX = s_liveY = 0.f;
}

bool Enabled() { return s_enabled.load(); }

void CycleDebugPath()
{
    const int next = (s_debugPath.load() + 1) % 4;
    s_debugPath = next;
    static const char* kNames[] = {"off", "makeView", "vtable slot 44", "entity transform write"};
    Log("RawCam DEBUG: constant 20 deg yaw via %s", kNames[next]);
}

void OnTickRead(float ax, float ay)
{
    ++s_readSeq;
    if (s_pendingCount == kMaxPending) { // camera ticks stopped consuming: keep the newest
        for (int i = 1; i < kMaxPending; ++i) s_pending[i - 1] = s_pending[i];
        --s_pendingCount;
    }
    s_pending[s_pendingCount++] = {s_readSeq, ax, ay};
    s_lastReadAx.store(ax, std::memory_order_relaxed);
    s_lastReadAy.store(ay, std::memory_order_relaxed);
    s_lastReadSeq.store(s_readSeq, std::memory_order_release);
    s_liveX = s_liveY = 0.f;
    if (s_enabled.load(std::memory_order_relaxed)) Publish();
}

void SetLiveAccum(float ax, float ay)
{
    s_liveX = ax;
    s_liveY = ay;

    if (s_debugPath.load(std::memory_order_relaxed) == 3) {
        // Diagnostic: overwrite the stored camera rotation every frame (the next tick rewrites it).
        if (uint8_t* entity = CameraEntity()) {
            static float base[9];
            static float written[9];
            float* rot = reinterpret_cast<float*>(entity + kEntityRot);
            if (std::memcmp(rot, written, sizeof(written)) != 0) std::memcpy(base, rot, sizeof(base));
            float r[9];
            std::memcpy(r, base, sizeof(r));
            DebugYaw(r);
            std::memcpy(rot, r, sizeof(r));
            std::memcpy(written, r, sizeof(r));
        }
        return;
    }
    if (!s_enabled.load(std::memory_order_relaxed)) return;
    if (!CameraLive()) {
        // No camera ticks (menu, loading, quitting): do not touch the camera, drop the offset.
        s_pendingCount = 0;
        s_liveX = s_liveY = 0.f;
        Publish();
        return;
    }
    st_frames.fetch_add(1, std::memory_order_relaxed);

    // Reads already in the stored transform are no longer pending.
    const uint32_t committed = s_committedSeq.load(std::memory_order_acquire);
    int w = 0;
    for (int i = 0; i < s_pendingCount; ++i)
        if (static_cast<int32_t>(s_pending[i].seq - committed) > 0) s_pending[w++] = s_pending[i];
    s_pendingCount = w;

    Publish();

    // Refresh the stored pose early in the frame (before the render interpolation runs), on the
    // tracked camera only — which the destructor hook keeps valid while we hold the lock.
    if (!s_dtorHooked || !s_setTransform) return;
    AcquireSRWLockExclusive(&s_trackLock);
    if (s_trackedEntity) FrameWrite(s_trackedEntity);
    ReleaseSRWLockExclusive(&s_trackLock);
}

void OnLookInput(uint8_t* comp, float dt)
{
    const uint32_t seq = s_lastReadSeq.load(std::memory_order_acquire);
    s_sensX.store(Field<float>(comp, kCompSensX), std::memory_order_relaxed);
    s_sensY.store(Field<float>(comp, kCompSensY), std::memory_order_relaxed);
    s_dt.store(dt, std::memory_order_relaxed);
    // Sign of look / consumed (captures the invert-X/Y options); the pipeline is otherwise 1:1.
    const float ax = s_lastReadAx.load(std::memory_order_relaxed), ay = s_lastReadAy.load(std::memory_order_relaxed);
    const float lx = Field<float>(comp, kCompLook), ly = Field<float>(comp, kCompLook + 4);
    if (std::fabs(ax) > 1e-4f && std::fabs(lx) > 1e-6f) s_signX.store(lx / ax < 0.f ? -1.f : 1.f, std::memory_order_relaxed);
    if (std::fabs(ay) > 1e-4f && std::fabs(ly) > 1e-6f) s_signY.store(ly / ay < 0.f ? -1.f : 1.f, std::memory_order_relaxed);
    // Mouse look in use? Counts consumed but no look -> suppressed (UI); any look -> active;
    // no counts and no look -> unchanged.
    const bool consumed = std::fabs(ax) > 2e-3f || std::fabs(ay) > 2e-3f;
    const bool looked = std::fabs(lx) > 1e-7f || std::fabs(ly) > 1e-7f;
    if (looked || consumed) {
        const bool was = s_lookActive.exchange(looked, std::memory_order_relaxed);
        static int logged = 0;
        if (was != looked && logged < 40) {
            ++logged;
            Log("RawCam: mouse look %s", looked ? "resumed" : "ignored by the game (UI): raw offset off");
        }
    }
    s_tickAppliedX.store(lx * s_signX.load(std::memory_order_relaxed), std::memory_order_relaxed);
    s_tickAppliedY.store(ly * s_signY.load(std::memory_order_relaxed), std::memory_order_relaxed);
    s_tickSeq.store(seq, std::memory_order_release);
    s_comp.store(comp, std::memory_order_release);
    s_lastLookQpc.store(QpcNow(), std::memory_order_release);
}

void OnPresent(int64_t qpcBefore, int64_t qpcAfter, unsigned syncInterval)
{
    if (!s_enabled.load(std::memory_order_relaxed)) return;
    st_presents.fetch_add(1, std::memory_order_relaxed);
    st_sync.store(syncInterval, std::memory_order_relaxed);
    const int64_t blockUs = static_cast<int64_t>(QpcToMs(qpcAfter - qpcBefore) * 1000.0);
    st_blockSumUs.fetch_add(blockUs, std::memory_order_relaxed);
    if (blockUs > st_blockMaxUs.load(std::memory_order_relaxed)) st_blockMaxUs.store(blockUs, std::memory_order_relaxed);
    const int64_t w = s_lastWriteQpc.exchange(0, std::memory_order_acq_rel);
    if (w && qpcBefore > w) {
        const int64_t us = static_cast<int64_t>(QpcToMs(qpcBefore - w) * 1000.0);
        st_w2pSumUs.fetch_add(us, std::memory_order_relaxed);
        if (us > st_w2pMaxUs.load(std::memory_order_relaxed)) st_w2pMaxUs.store(us, std::memory_order_relaxed);
        st_w2pN.fetch_add(1, std::memory_order_relaxed);
    }
}

void StatsTick(double elapsedMs)
{
    if (!s_enabled.load()) return;
    const double s = elapsedMs / 1000.0;
    {
        const uint32_t n = st_errN, rn = st_ratioN;
        const uint32_t w2pN = st_w2pN.exchange(0), pres = st_presents.exchange(0);
        Log("    RawCam timing: presents=%.0f/s sync=%u presentBlock avg=%.2f max=%.2f ms | poseWrite->Present avg=%.2f max=%.2f ms (n=%u)"
            " | tick turn error vs counts: yaw avg=%.5f max=%.5f pitch avg=%.5f max=%.5f rad (n=%u), turn ratio game/pred=%.3f (n=%u)",
            pres / s, st_sync.load(), pres ? st_blockSumUs.exchange(0) / 1000.0 / pres : 0.0, st_blockMaxUs.exchange(0) / 1000.0,
            w2pN ? st_w2pSumUs.exchange(0) / 1000.0 / w2pN : 0.0, st_w2pMaxUs.exchange(0) / 1000.0, w2pN,
            n ? st_errYawSum / n : 0.f, st_errYawMax, n ? st_errPitchSum / n : 0.f, st_errPitchMax, n,
            rn ? st_ratioSum / rn : 0.f, rn);
        st_errYawSum = st_errYawMax = st_errPitchSum = st_errPitchMax = st_ratioSum = 0.f;
        st_errN = st_ratioN = 0;
    }
    Log("    RawCam: frames=%.0f/s frameWrites=%.0f/s tickWrites=%.1f/s foreignMoves=%u maxYaw=%.4f maxPitch=%.4f rad | "
        "sens=%.3f/%.3f slope=%.4f dt=%.5f sign=%+.0f/%+.0f | pivotDist=%.2f pending=%d committedSeq=%u readSeq=%u",
        st_frames.exchange(0) / s, st_frameWrites.exchange(0) / s, st_tickWrites.exchange(0) / s, st_foreign.exchange(0),
        st_maxYaw.exchange(0.f), st_maxPitch.exchange(0.f), s_sensX.load(), s_sensY.load(), s_slope.load(), s_dt.load(),
        s_signX.load(), s_signY.load(), st_pivotDist.load(), s_pendingCount, s_committedSeq.load(), s_readSeq);
}

} // namespace rawcam
