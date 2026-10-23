// Offline check of the plugin's runtime discovery against a game executable on disk:
// maps the PE at its preferred base (so absolute pointers in .rdata need no relocation) and runs
// the same installers the plugin runs in-game, printing the resulting log.
//
//   scan_test.exe "<path>\HorizonZeroDawn.exe" <output dir>
#include "../src/config.h"
#include "../src/hooks.h"
#include "../src/log.h"
#include "../src/mem.h"
#include "../src/raw_camera.h"

#include <windows.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static uint8_t* MapImage(const wchar_t* path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return nullptr;
    std::vector<char> file((std::istreambuf_iterator<char>(f)), {});
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    auto* want = reinterpret_cast<void*>(nt->OptionalHeader.ImageBase);
    auto* base = static_cast<uint8_t*>(
        VirtualAlloc(want, nt->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (base != want) {
        std::cerr << "could not map at preferred base\n";
        return nullptr;
    }
    memcpy(base, file.data(), nt->OptionalHeader.SizeOfHeaders);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (sec->SizeOfRawData)
            memcpy(base + sec->VirtualAddress, file.data() + sec->PointerToRawData, sec->SizeOfRawData);
    return base;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        std::wcerr << L"usage: scan_test <HorizonZeroDawn.exe> <outdir>\n";
        return 2;
    }
    uint8_t* base = MapImage(argv[1]);
    if (!base) return 1;
    mem::SetImageBaseOverride(base);

    g_moduleDir = argv[2];
    if (g_moduleDir.back() != L'\\') g_moduleDir += L'\\';
    g_cfg.eventCsv = false;
    LogInit();

    const DWORD t0 = GetTickCount();
    InstallInputHooks();
    Log("discovery took %lu ms", GetTickCount() - t0);
    std::printf("IAT dxgi.dll!CreateDXGIFactory2 slot: %p\n", static_cast<void*>(mem::FindIatSlot("dxgi.dll", "CreateDXGIFactory2")));
    std::printf("modes available: %d, raw camera available: %d\n", ModesAvailable(), rawcam::Available());
    LogShutdown();

    std::ifstream log(g_moduleDir + L"HZDRawMouse.log");
    std::cout << log.rdbuf();
    return (ModesAvailable() && rawcam::Available()) ? 0 : 1;
}
