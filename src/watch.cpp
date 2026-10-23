#include "watch.h"
#include "log.h"

#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace watch {
namespace {

constexpr int kMaxRegions = 4;
constexpr int kMaxSites = 96;
constexpr int kDepth = 6;
constexpr int kMaxStacks = 48;

struct Site {
    std::atomic<uint64_t> key{0}; // rip | region << 60
    std::atomic<uint32_t> hits{0};
    std::atomic<uint32_t> tid{0};
};
struct Stack {
    std::atomic<uint64_t> hash{0};
    int region = 0;
    uintptr_t frames[kDepth];
    std::atomic<uint32_t> hits{0};
};

Site s_sites[kMaxSites];
Stack s_stacks[kMaxStacks];
std::atomic<uint32_t> s_overflow{0};
std::atomic<bool> s_armed{false};
PVOID s_veh = nullptr;
Region s_regions[kMaxRegions];
int s_count = 0;

void RecordSite(uintptr_t rip, int region, uint32_t tid)
{
    const uint64_t key = rip | (static_cast<uint64_t>(region) << 60);
    for (auto& s : s_sites) {
        uint64_t cur = s.key.load(std::memory_order_relaxed);
        if (cur == 0) {
            uint64_t expected = 0;
            if (!s.key.compare_exchange_strong(expected, key) && expected != key) continue;
            cur = key;
        }
        if (cur == key) {
            s.hits.fetch_add(1, std::memory_order_relaxed);
            s.tid.store(tid, std::memory_order_relaxed);
            return;
        }
    }
    s_overflow.fetch_add(1, std::memory_order_relaxed);
}

void RecordStack(const CONTEXT* fault, int region)
{
    CONTEXT ctx = *fault;
    uintptr_t frames[kDepth] = {};
    uint64_t h = 1469598103934665603ull ^ static_cast<uint64_t>(region);
    for (int i = 0; i < kDepth; ++i) {
        frames[i] = ctx.Rip;
        h = (h ^ ctx.Rip) * 1099511628211ull;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (fn) {
            PVOID handlerData;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
        } else {
            // Leaf function (no unwind data): the return address is at [rsp].
            if (IsBadReadPtr(reinterpret_cast<void*>(ctx.Rsp), 8)) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        }
        if (!ctx.Rip) break;
    }
    if (h == 0 || h == ~0ull) h = 1;
    for (auto& s : s_stacks) {
        uint64_t cur = s.hash.load(std::memory_order_acquire);
        if (cur == 0) {
            uint64_t expected = 0;
            if (s.hash.compare_exchange_strong(expected, ~0ull)) {
                std::memcpy(s.frames, frames, sizeof(frames));
                s.region = region;
                s.hash.store(h, std::memory_order_release);
                cur = h;
            } else {
                cur = expected;
            }
        }
        if (cur == h) {
            s.hits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = e->ContextRecord;
    const DWORD64 status = c->Dr6 & 0xF;
    if (!status) return EXCEPTION_CONTINUE_SEARCH; // not ours (e.g. a debugger's single step)
    // Data breakpoints trap after the access: Rip is the instruction following the accessor.
    for (int i = 0; i < s_count; ++i) {
        if (!(status & (1ull << i))) continue;
        RecordSite(c->Rip, i, GetCurrentThreadId());
        RecordStack(c, i);
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD64 LenBits(size_t len) { return len >= 8 ? 2 : len >= 4 ? 3 : len >= 2 ? 1 : 0; }

// Program DR0..DR(count-1) on every thread except the caller (which never touches the addresses).
int ApplyToAllThreads(bool enable)
{
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    int applied = 0;
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &ctx)) {
                DWORD64* dr[kMaxRegions] = {&ctx.Dr0, &ctx.Dr1, &ctx.Dr2, &ctx.Dr3};
                for (int i = 0; i < kMaxRegions; ++i) {
                    ctx.Dr7 &= ~((3ull << (2 * i)) | (0xFull << (16 + 4 * i)));
                    *dr[i] = 0;
                    if (enable && i < s_count) {
                        *dr[i] = reinterpret_cast<DWORD64>(s_regions[i].addr);
                        ctx.Dr7 |= (1ull << (2 * i)) | (3ull << (16 + 4 * i)) | (LenBits(s_regions[i].len) << (18 + 4 * i));
                    }
                }
                if (SetThreadContext(t, &ctx)) ++applied;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return applied;
}

const char* RegionName(int i) { return i < s_count && s_regions[i].name ? s_regions[i].name : "?"; }

} // namespace

bool Arm(const Region* regions, int count)
{
    if (s_armed) return true;
    if (count < 1 || count > kMaxRegions) return false;
    if (!s_veh) s_veh = AddVectoredExceptionHandler(1, Handler);
    if (!s_veh) return false;
    for (auto& s : s_sites) { s.key = 0; s.hits = 0; s.tid = 0; }
    for (auto& s : s_stacks) { s.hash = 0; s.hits = 0; }
    s_overflow = 0;
    for (int i = 0; i < count; ++i) s_regions[i] = regions[i];
    s_count = count;
    const int n = ApplyToAllThreads(true);
    s_armed = n > 0;
    for (int i = 0; i < count; ++i)
        Log("watch: DR%d = %s at %p (%zu bytes)", i, RegionName(i), regions[i].addr, regions[i].len);
    Log("watch: armed on %d threads", n);
    return s_armed;
}

void Disarm()
{
    if (!s_armed) return;
    const int n = ApplyToAllThreads(false);
    s_armed = false;
    Log("watch: disarmed (%d threads)", n);
}

bool Armed() { return s_armed; }

void Report(const char* label)
{
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto fmtAddr = [exe](char* out, size_t cap, uintptr_t f) {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(f), &m);
        if (reinterpret_cast<uintptr_t>(m) == exe)
            return snprintf(out, cap, " %llx", static_cast<unsigned long long>(f - exe + 0x140000000ull));
        return snprintf(out, cap, " [%p+%llx]", m, static_cast<unsigned long long>(f - reinterpret_cast<uintptr_t>(m)));
    };

    bool any = false;
    for (auto& s : s_sites) {
        const uint64_t key = s.key.load();
        if (!key) break;
        const uint32_t hits = s.hits.exchange(0);
        if (!hits) continue;
        any = true;
        char a[64];
        fmtAddr(a, sizeof(a), static_cast<uintptr_t>(key & 0x0FFFFFFFFFFFFFFFull));
        Log("  %s[%s]: after-insn%s hits=%u tid=%u", label, RegionName(static_cast<int>(key >> 60)), a, hits, s.tid.load());
    }
    if (const uint32_t o = s_overflow.exchange(0)) Log("  %s: %u hits from untracked sites", label, o);
    if (!any) Log("  %s: no hits", label);

    for (auto& s : s_stacks) {
        const uint64_t h = s.hash.load(std::memory_order_acquire);
        if (h == 0 || h == ~0ull) continue;
        const uint32_t hits = s.hits.exchange(0);
        if (!hits) continue;
        char line[600];
        int n = snprintf(line, sizeof(line), "  %s[%s] stack x%u:", label, RegionName(s.region), hits);
        for (int i = 0; i < kDepth && s.frames[i] && n < static_cast<int>(sizeof(line)) - 40; ++i)
            n += fmtAddr(line + n, sizeof(line) - n, s.frames[i]);
        Log("%s", line);
    }
}

} // namespace watch
