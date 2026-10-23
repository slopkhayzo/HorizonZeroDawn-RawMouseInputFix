#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace mem {

struct Section {
    uint8_t* begin = nullptr;
    uint8_t* end = nullptr;
    bool contains(const void* p) const { return p >= begin && p < end; }
};

struct Module {
    uint8_t* base = nullptr;
    Section text, rdata, data;
};

// Main executable image layout. Initialised once.
const Module& Exe();

// Test hook: analyse an image mapped elsewhere instead of the host process. Call before Exe().
void SetImageBaseOverride(uint8_t* base);

// Byte pattern with wildcards, e.g. "0F 31 F3 0F 10 05 ?? ?? ?? ??".
// Returns first match within [from, from+len) or nullptr.
uint8_t* FindPattern(uint8_t* from, size_t len, const char* pattern);

// Resolve a rip-relative operand: `insn` points at the instruction, `dispOffset` is the offset of
// the disp32 inside it and `insnLen` its total length.
inline uint8_t* RipTarget(uint8_t* insn, size_t dispOffset, size_t insnLen)
{
    return insn + insnLen + *reinterpret_cast<int32_t*>(insn + dispOffset);
}

// Locate the primary vtable (COL offset 0) of an MSVC class by its RTTI name, e.g. ".?AVFoo@@".
void** FindVTableByRtti(const char* mangledName);

// Swap a pointer-sized slot (vtable entry / IAT thunk). Returns the previous value.
void* PatchPointer(void** slot, void* value);

// Find the IAT slot of `dll!func` in the main executable's import table.
void** FindIatSlot(const char* dll, const char* func);

// Write bytes into a (possibly read-only) code/data page.
bool WriteProtected(void* dst, const void* src, size_t len);

// Find the single `call rel32` in .text whose target is `fn`; nullptr if none or ambiguous.
uint8_t* FindUniqueCallTo(const void* fn);

// Redirect a `call rel32` at `site` to `hook` through an absolute-jump stub allocated within
// ±2 GB of the site. Returns the original call target, or nullptr on failure.
void* RedirectCall(uint8_t* site, void* hook);

} // namespace mem
