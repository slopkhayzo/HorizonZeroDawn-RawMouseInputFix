#include "mem.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>

namespace mem {

static uint8_t* s_baseOverride = nullptr;

void SetImageBaseOverride(uint8_t* base) { s_baseOverride = base; }

const Module& Exe()
{
    static Module m = [] {
        Module r;
        r.base = s_baseOverride ? s_baseOverride : reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(r.base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(r.base + dos->e_lfanew);
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            char name[9] = {};
            std::memcpy(name, sec->Name, 8);
            // Only the file-backed part matters for scans (skips .data's large BSS tail).
            const DWORD size = (std::min)(sec->Misc.VirtualSize, sec->SizeOfRawData);
            Section s{r.base + sec->VirtualAddress, r.base + sec->VirtualAddress + size};
            if (!std::strcmp(name, ".text")) r.text = s;
            else if (!std::strcmp(name, ".rdata")) r.rdata = s;
            else if (!std::strcmp(name, ".data")) r.data = s;
        }
        return r;
    }();
    return m;
}

uint8_t* FindPattern(uint8_t* from, size_t len, const char* pattern)
{
    std::vector<int> pat;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { pat.push_back(-1); p += (p[1] == '?') ? 2 : 1; continue; }
        pat.push_back(static_cast<int>(strtoul(p, nullptr, 16)));
        p += 2;
    }
    if (pat.empty() || len < pat.size()) return nullptr;
    for (size_t i = 0; i + pat.size() <= len; ++i) {
        size_t j = 0;
        while (j < pat.size() && (pat[j] < 0 || from[i + j] == pat[j])) ++j;
        if (j == pat.size()) return from + i;
    }
    return nullptr;
}

void** FindVTableByRtti(const char* mangledName)
{
    const Module& m = Exe();
    const size_t nameLen = std::strlen(mangledName) + 1;

    // TypeDescriptor: { vftable*, spare*, char name[] } — name lives at +0x10, usually in .data.
    const auto* needle = reinterpret_cast<const uint8_t*>(mangledName);
    const std::boyer_moore_horspool_searcher searcher(needle, needle + nameLen);
    for (const Section* s : {&m.data, &m.rdata}) {
        for (uint8_t* p = s->begin;; ++p) {
            p = std::search(p, s->end, searcher);
            if (p == s->end) break;
            const uint32_t tdRva = static_cast<uint32_t>((p - 0x10) - m.base);

            // CompleteObjectLocator (x64): sig=1, offset, cdOffset, pTypeDescriptor(rva), pClassHierarchy(rva), pSelf(rva)
            for (uint8_t* c = m.rdata.begin; c + 24 <= m.rdata.end; c += 4) {
                auto* col = reinterpret_cast<uint32_t*>(c);
                if (col[0] != 1 || col[3] != tdRva || col[1] != 0) continue;
                if (col[5] != static_cast<uint32_t>(c - m.base)) continue;

                // vtable[-1] == &COL
                const uintptr_t colVa = reinterpret_cast<uintptr_t>(c);
                for (uint8_t* v = m.rdata.begin; v + 16 <= m.rdata.end; v += 8) {
                    if (*reinterpret_cast<uintptr_t*>(v) == colVa) {
                        auto** vt = reinterpret_cast<void**>(v + 8);
                        if (m.text.contains(vt[0])) return vt;
                    }
                }
            }
        }
    }
    return nullptr;
}

bool WriteProtected(void* dst, const void* src, size_t len)
{
    DWORD old;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(dst, src, len);
    VirtualProtect(dst, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    return true;
}

void* PatchPointer(void** slot, void* value)
{
    void* prev = *slot;
    WriteProtected(slot, &value, sizeof(value));
    return prev;
}

uint8_t* FindUniqueCallTo(const void* fn)
{
    const Module& m = Exe();
    uint8_t* found = nullptr;
    for (uint8_t* p = m.text.begin; p + 5 <= m.text.end; ++p) {
        if (*p != 0xE8) continue;
        if (p + 5 + *reinterpret_cast<int32_t*>(p + 1) != fn) continue;
        if (found) return nullptr; // ambiguous
        found = p;
    }
    return found;
}

// Allocate a small executable block within ±2 GB of `anchor` (64 KB allocation granularity).
static uint8_t* AllocNear(const void* anchor, size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t origin = reinterpret_cast<uintptr_t>(anchor) & ~(gran - 1);
    const uintptr_t range = 0x7FF00000;
    for (uintptr_t delta = gran; delta < range; delta += gran) {
        for (int dir = -1; dir <= 1; dir += 2) {
            const uintptr_t addr = dir < 0 ? origin - delta : origin + delta;
            if (dir < 0 && delta > origin) continue;
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) || mbi.State != MEM_FREE) continue;
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                return static_cast<uint8_t*>(p);
        }
    }
    return nullptr;
}

void* RedirectCall(uint8_t* site, void* hook)
{
    if (!site || site[0] != 0xE8) return nullptr;
    uint8_t* orig = site + 5 + *reinterpret_cast<int32_t*>(site + 1);

    // Stubs are packed into one near block per module region; 16 bytes each.
    static uint8_t* block = nullptr;
    static size_t used = 0;
    if (!block || used + 16 > 4096) {
        block = AllocNear(site, 4096);
        used = 0;
        if (!block) return nullptr;
    }
    uint8_t* stub = block + used;
    const intptr_t reach = stub - (site + 5);
    if (reach > INT32_MAX || reach < INT32_MIN) return nullptr;
    used += 16;

    // jmp qword ptr [rip+0] ; dq hook
    static const uint8_t jmp[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(stub, jmp, 6);
    std::memcpy(stub + 6, &hook, 8);
    FlushInstructionCache(GetCurrentProcess(), stub, 16);

    const int32_t rel = static_cast<int32_t>(reach);
    if (!WriteProtected(site + 1, &rel, 4)) return nullptr;
    return orig;
}

void** FindIatSlot(const char* dll, const char* func)
{
    const Module& m = Exe();
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m.base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;

    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(m.base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char*>(m.base + imp->Name), dll) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(m.base + imp->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(m.base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(m.base + names->u1.AddressOfData);
            if (!std::strcmp(reinterpret_cast<char*>(ibn->Name), func))
                return reinterpret_cast<void**>(&iat->u1.Function);
        }
    }
    return nullptr;
}

} // namespace mem
