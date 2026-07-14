#pragma once
#include <string>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace tankaq
{

enum class Backend { D3D11, D3D12 };

struct GpuInfo
{
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    std::string name;
    size_t dedicatedVideoMB = 0;
    bool supportsD3D12 = false;     // feature level 12_0 device creation succeeded
    Backend chosen = Backend::D3D11;
};

// Enumerates GPUs, picks the high-performance adapter, probes D3D12 support and
// decides the backend. `forced` may be "d3d11", "d3d12" or "" for auto.
GpuInfo DetectGpu(const std::string& forced);

} // namespace tankaq
