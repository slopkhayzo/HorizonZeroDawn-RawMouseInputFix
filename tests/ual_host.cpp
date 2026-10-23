// Minimal host that statically imports version.dll, to check offline that Ultimate ASI Loader
// (renamed to version.dll) loads HZDRawMouse.asi and calls InitializeASI.
// Copy ual_host.exe, version.dll (UAL), HZDRawMouse.asi/.ini into one folder and run it.
#include <windows.h>
#include <cstdio>

#pragma comment(lib, "version.lib")

int main()
{
    // Static import keeps version.dll in our import table.
    const DWORD n = GetFileVersionInfoSizeA("kernel32.dll", nullptr);
    std::printf("GetFileVersionInfoSizeA -> %lu\n", n);
    std::printf("HZDRawMouse.asi loaded: %s\n", GetModuleHandleW(L"HZDRawMouse.asi") ? "yes" : "no");
    Sleep(1500); // let the plugin's worker thread flush its log
    return 0;
}
