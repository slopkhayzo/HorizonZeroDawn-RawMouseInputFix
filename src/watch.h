#pragma once
#include <cstddef>
#include <cstdint>

// Data watchpoints via debug registers DR0..DR3 on every thread of the process (read/write,
// 1/2/4/8 bytes each). Hits are recorded per instruction address (and call stack) by a VEH.
namespace watch {

struct Region {
    const void* addr;
    size_t len;
    const char* name;
};

bool Arm(const Region* regions, int count); // count <= 4
void Disarm();
bool Armed();

// Logs hit counts per instruction and per call stack (since the previous report) and clears them.
void Report(const char* label);

} // namespace watch
