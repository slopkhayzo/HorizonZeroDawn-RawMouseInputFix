// Frame counter: IAT hook on CreateDXGIFactory1/2 -> vtable hooks on CreateSwapChain(ForHwnd)
// -> vtable hooks on Present/Present1. Only used to correlate input polls with frames.
#include "hooks.h"
#include "config.h"
#include "log.h"
#include "mem.h"
#include "raw_camera.h"

#include <windows.h>
#include <dxgi1_2.h>
#include <atomic>

namespace {

constexpr int kFactoryCreateSwapChain = 10;
constexpr int kFactory2CreateSwapChainForHwnd = 15;
constexpr int kSwapChainPresent = 8;
constexpr int kSwapChain1Present1 = 22;

using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

CreateFactoryFn s_origCreateFactory1 = nullptr;
CreateFactory2Fn s_origCreateFactory2 = nullptr;
CreateSwapChainFn s_origCreateSwapChain = nullptr;
CreateSwapChainForHwndFn s_origCreateSwapChainForHwnd = nullptr;
PresentFn s_origPresent = nullptr;
Present1Fn s_origPresent1 = nullptr;

std::atomic<bool> s_factoryHooked{false};
std::atomic<bool> s_swapChainHooked{false};

HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    EvPush(Ev::Present, static_cast<int32_t>(sync), static_cast<int32_t>(flags));
    const int64_t t0 = QpcNow();
    const HRESULT hr = s_origPresent(sc, sync, flags);
    rawcam::OnPresent(t0, QpcNow(), sync);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    EvPush(Ev::Present, static_cast<int32_t>(sync), static_cast<int32_t>(flags | 0x80000000u));
    const int64_t t0 = QpcNow();
    const HRESULT hr = s_origPresent1(sc, sync, flags, p);
    rawcam::OnPresent(t0, QpcNow(), sync);
    return hr;
}

void HookSwapChain(IUnknown* obj)
{
    if (!obj || s_swapChainHooked.exchange(true)) return;
    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(obj->QueryInterface(IID_PPV_ARGS(&sc1)))) {
        s_swapChainHooked = false;
        return;
    }
    void** vt = *reinterpret_cast<void***>(sc1);
    s_origPresent = static_cast<PresentFn>(mem::PatchPointer(&vt[kSwapChainPresent], reinterpret_cast<void*>(&HkPresent)));
    s_origPresent1 = static_cast<Present1Fn>(mem::PatchPointer(&vt[kSwapChain1Present1], reinterpret_cast<void*>(&HkPresent1)));
    sc1->Release();
    Log("Hooked IDXGISwapChain::Present / Present1");
}

HRESULT STDMETHODCALLTYPE HkCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out)
{
    HRESULT hr = s_origCreateSwapChain(f, dev, d, out);
    if (SUCCEEDED(hr) && out) HookSwapChain(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o,
                                                   IDXGISwapChain1** out)
{
    HRESULT hr = s_origCreateSwapChainForHwnd(f, dev, w, d, fs, o, out);
    if (SUCCEEDED(hr) && out) HookSwapChain(*out);
    return hr;
}

void HookFactory(void* obj)
{
    if (!obj || s_factoryHooked.exchange(true)) return;
    auto* unk = static_cast<IUnknown*>(obj);
    IDXGIFactory2* f2 = nullptr;
    if (FAILED(unk->QueryInterface(IID_PPV_ARGS(&f2)))) {
        s_factoryHooked = false;
        return;
    }
    void** vt = *reinterpret_cast<void***>(f2);
    s_origCreateSwapChain = static_cast<CreateSwapChainFn>(
        mem::PatchPointer(&vt[kFactoryCreateSwapChain], reinterpret_cast<void*>(&HkCreateSwapChain)));
    s_origCreateSwapChainForHwnd = static_cast<CreateSwapChainForHwndFn>(
        mem::PatchPointer(&vt[kFactory2CreateSwapChainForHwnd], reinterpret_cast<void*>(&HkCreateSwapChainForHwnd)));
    f2->Release();
    Log("Hooked IDXGIFactory2::CreateSwapChain / CreateSwapChainForHwnd");
}

HRESULT WINAPI HkCreateDXGIFactory1(REFIID riid, void** pp)
{
    HRESULT hr = s_origCreateFactory1(riid, pp);
    if (SUCCEEDED(hr) && pp) HookFactory(*pp);
    return hr;
}

HRESULT WINAPI HkCreateDXGIFactory2(UINT flags, REFIID riid, void** pp)
{
    HRESULT hr = s_origCreateFactory2(flags, riid, pp);
    if (SUCCEEDED(hr) && pp) HookFactory(*pp);
    return hr;
}

} // namespace

void InstallDxgiHooks()
{
    if (!g_cfg.hookPresent) return;
    void** s1 = mem::FindIatSlot("dxgi.dll", "CreateDXGIFactory1");
    void** s2 = mem::FindIatSlot("dxgi.dll", "CreateDXGIFactory2");
    if (s1) s_origCreateFactory1 = static_cast<CreateFactoryFn>(mem::PatchPointer(s1, reinterpret_cast<void*>(&HkCreateDXGIFactory1)));
    if (s2) s_origCreateFactory2 = static_cast<CreateFactory2Fn>(mem::PatchPointer(s2, reinterpret_cast<void*>(&HkCreateDXGIFactory2)));
    Log("DXGI factory IAT hooks: CreateDXGIFactory1=%s CreateDXGIFactory2=%s", s1 ? "ok" : "missing", s2 ? "ok" : "missing");
}
