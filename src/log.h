#pragma once
#include <cstdint>

// Human-readable log (HZDRawMouse.log). Thread-safe, printf-style.
void LogInit();
void LogShutdown();
void Log(const char* fmt, ...);

// High-rate event ring (lock-free MPSC), drained to HZDRawMouse_events.csv by the worker thread.
enum class Ev : uint32_t {
    RawInput = 1,  // a=lLastX b=lLastY c=usFlags d=usButtonFlags
    Update,        // a=drained(0/1) b=queued events before call c=dx d=dy (KeyBoardController accum after call)
    GetState,      // c=out dx d=out dy (floats as written to the controller state), ret=caller
    Present,       // a=sync interval b=flags
    Mark,          // user marker hotkey
    Throttle,      // a=1 removed / 0 restored
};

struct EvRec {
    int64_t qpc;
    uint32_t tid;
    Ev type;
    int32_t a, b;
    float c, d;
    uintptr_t ret;
};

void EvPush(Ev type, int32_t a = 0, int32_t b = 0, float c = 0.f, float d = 0.f, uintptr_t ret = 0);

// Drains pending events to the CSV; returns number written. Called from the worker thread only.
size_t EvDrain();
uint64_t EvDropped();

int64_t QpcNow();
double QpcToMs(int64_t ticks);
