#include "log.h"
#include "config.h"

#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

static FILE* s_log = nullptr;
static FILE* s_csv = nullptr;
static std::mutex s_logMtx;
static int64_t s_qpcFreq = 1;
static int64_t s_qpcStart = 0;

int64_t QpcNow()
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double QpcToMs(int64_t ticks) { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(s_qpcFreq); }

void LogInit()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s_qpcFreq = f.QuadPart;
    s_qpcStart = QpcNow();

    // Prefer the plugin's folder; fall back to %TEMP% if it is not writable.
    std::wstring dir = g_moduleDir;
    if (g_cfg.logEnable) {
        s_log = _wfsopen((dir + L"HZDRawMouse.log").c_str(), L"w", _SH_DENYWR);
        if (!s_log) {
            wchar_t tmp[MAX_PATH];
            if (GetTempPathW(MAX_PATH, tmp)) {
                dir = tmp;
                s_log = _wfsopen((dir + L"HZDRawMouse.log").c_str(), L"w", _SH_DENYWR);
            }
        }
    }
    if (g_cfg.eventCsv) {
        s_csv = _wfsopen((dir + L"HZDRawMouse_events.csv").c_str(), L"w", _SH_DENYWR);
        if (s_csv) {
            setvbuf(s_csv, nullptr, _IOFBF, 1 << 20);
            fputs("t_ms,tid,type,a,b,c,d,caller\n", s_csv);
        }
    }
}

void LogShutdown()
{
    EvDrain();
    std::lock_guard lk(s_logMtx);
    if (s_csv) { fclose(s_csv); s_csv = nullptr; }
    if (s_log) { fclose(s_log); s_log = nullptr; }
}

void Log(const char* fmt, ...)
{
    if (!s_log) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard lk(s_logMtx);
    fprintf(s_log, "[%10.3f] %s\n", QpcToMs(QpcNow() - s_qpcStart), buf);
    fflush(s_log);
}

// ---------------------------------------------------------------------------------------------
// MPSC ring: producers claim a slot with fetch_add, publish with seq = idx + 1.
namespace {
constexpr size_t kRingSize = 1u << 18; // 262144 events
struct Slot {
    std::atomic<uint64_t> seq{0};
    EvRec rec;
};
Slot* s_ring = new Slot[kRingSize];
std::atomic<uint64_t> s_head{0};
uint64_t s_tail = 0;
std::atomic<uint64_t> s_dropped{0};
}

void EvPush(Ev type, int32_t a, int32_t b, float c, float d, uintptr_t ret)
{
    if (!s_csv) return;
    uint64_t idx = s_head.fetch_add(1, std::memory_order_relaxed);
    Slot& s = s_ring[idx & (kRingSize - 1)];
    s.rec = EvRec{QpcNow(), GetCurrentThreadId(), type, a, b, c, d, ret};
    s.seq.store(idx + 1, std::memory_order_release);
}

static const char* EvName(Ev t)
{
    switch (t) {
    case Ev::RawInput: return "RI";
    case Ev::Update: return "UPD";
    case Ev::GetState: return "GS";
    case Ev::Present: return "PRS";
    case Ev::Mark: return "MARK";
    case Ev::Throttle: return "THR";
    }
    return "?";
}

size_t EvDrain()
{
    if (!s_csv) return 0;
    size_t n = 0;
    const uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    for (;;) {
        Slot& s = s_ring[s_tail & (kRingSize - 1)];
        uint64_t seq = s.seq.load(std::memory_order_acquire);
        if (seq == s_tail + 1) {
            const EvRec& r = s.rec;
            // Caller printed as Ghidra VA (preferred base 0x140000000) for direct lookup.
            uintptr_t caller = r.ret ? (r.ret - exeBase + 0x140000000ull) : 0;
            fprintf(s_csv, "%.4f,%u,%s,%d,%d,%.6g,%.6g,%llx\n", QpcToMs(r.qpc - s_qpcStart), r.tid, EvName(r.type),
                    r.a, r.b, r.c, r.d, static_cast<unsigned long long>(caller));
            ++s_tail;
            ++n;
        } else if (seq > s_tail + 1) {
            // Producer lapped us; skip what was overwritten.
            uint64_t newTail = seq - 1;
            s_dropped.fetch_add(newTail - s_tail, std::memory_order_relaxed);
            s_tail = newTail;
        } else {
            break; // empty, or slot claimed but not yet published
        }
    }
    if (n) fflush(s_csv);
    return n;
}

uint64_t EvDropped() { return s_dropped.load(std::memory_order_relaxed); }
