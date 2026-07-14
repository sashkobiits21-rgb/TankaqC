#include "GpuDetect.h"
#include "Log.h"
#include <d3d12.h>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace tankaq
{

static std::string Narrow(const wchar_t* w)
{
    char buf[256]{};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    return buf;
}

static bool ProbeD3D12(IDXGIAdapter1* adapter)
{
    // d3d12.dll is delay-loaded; make sure it exists before touching its imports.
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!d3d12)
        return false;
    // Null out-pointer = capability probe, no device actually created.
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0,
                                   __uuidof(ID3D12Device), nullptr);
    return SUCCEEDED(hr);
}

GpuInfo DetectGpu(const std::string& forced)
{
    GpuInfo info;

    ComPtr<IDXGIFactory1> factory1;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1))))
    {
        Log("GPU: CreateDXGIFactory1 failed");
        return info;
    }

    // Prefer the high-performance GPU on hybrid systems when IDXGIFactory6 exists.
    ComPtr<IDXGIFactory6> factory6;
    factory1.As(&factory6);

    std::vector<ComPtr<IDXGIAdapter1>> adapters;
    if (factory6)
    {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0;
             factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                  IID_PPV_ARGS(&a)) == S_OK;
             ++i)
            adapters.push_back(a);
    }
    else
    {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; factory1->EnumAdapters1(i, &a) == S_OK; ++i)
            adapters.push_back(a);
    }

    for (auto& a : adapters)
    {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        Log("GPU: adapter '%s'  VRAM %zu MB%s", Narrow(d.Description).c_str(),
            size_t(d.DedicatedVideoMemory >> 20),
            (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "  (software)" : "");
    }

    for (auto& a : adapters)
    {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        info.adapter = a;
        info.name = Narrow(d.Description);
        info.dedicatedVideoMB = size_t(d.DedicatedVideoMemory >> 20);
        break;
    }
    if (!info.adapter && !adapters.empty())
    {
        info.adapter = adapters[0];
        DXGI_ADAPTER_DESC1 d{};
        adapters[0]->GetDesc1(&d);
        info.name = Narrow(d.Description);
    }
    if (!info.adapter)
    {
        Log("GPU: no adapters found");
        return info;
    }

    info.supportsD3D12 = ProbeD3D12(info.adapter.Get());

    if (forced == "d3d11")
        info.chosen = Backend::D3D11;
    else if (forced == "d3d12")
        info.chosen = Backend::D3D12;
    else
        info.chosen = info.supportsD3D12 ? Backend::D3D12 : Backend::D3D11;

    Log("GPU: selected '%s' (%zu MB), D3D12 support: %s, backend: %s%s",
        info.name.c_str(), info.dedicatedVideoMB,
        info.supportsD3D12 ? "yes" : "no",
        info.chosen == Backend::D3D12 ? "D3D12" : "D3D11",
        forced.empty() ? " (auto)" : " (forced)");
    return info;
}

} // namespace tankaq
