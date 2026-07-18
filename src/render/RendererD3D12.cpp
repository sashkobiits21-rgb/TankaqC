#include "render/IRenderer.h"
#include "AssetLoad.h"
#include "Log.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <fstream>
#include <sstream>

#include "stb_image_write.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace tankaq
{

namespace
{

constexpr int FrameCount = 3;          // swapchain buffers
constexpr int FramesInFlight = 2;
constexpr UINT CbAlign = 256;
constexpr UINT MaxObjectsPerFrame = 512;   // culled objects cost no slot;
                                           // this is the VISIBLE budget
constexpr UINT MaxSkinnedPerFrame = 40;   // soldiers + skulls + rigtest
constexpr UINT PaletteBytes = 64 * 64;   // MaxBones float4x4, 256-aligned

// CB ring slot map (256-byte slots). PerFrameCB outgrew a single slot when
// the STEALTH LOS boxes landed (560 bytes = 3 slots), which used to make the
// shadow-pass copy bleed into the VfxCB slot right after it -- killing every
// VFX draw on this backend. Every tenant now has an explicit, size-asserted
// home (asserts follow the struct definitions below):
//   [0..2]              main-pass PerFrameCB
//   [3..255]            per-object CBs
//   [256..257]          PostCB
//   [258..260]          shadow-pass PerFrameCB
//   [261..263]          VfxCB
//   [264..272]          UiBurnCB
//   [273..]             bone palettes
constexpr UINT FrameCbSlots = 3;
constexpr UINT ShadowCbSlot = MaxObjectsPerFrame + 2;
constexpr UINT VfxCbSlot    = MaxObjectsPerFrame + 5;
constexpr UINT BurnCbSlot   = MaxObjectsPerFrame + 8;
constexpr UINT PaletteBase  = (MaxObjectsPerFrame + 17) * CbAlign;
constexpr UINT UiVbBytes = 4 * 1024 * 1024;
constexpr UINT MaxTextures = 64;
constexpr UINT PostSrvBase = MaxTextures;      // 9 groups spaced 8 descriptors apart
constexpr UINT SrvHeapSize = MaxTextures + 80;

struct PerFrameCB
{
    XMFLOAT4X4 viewProj;
    XMFLOAT4X4 lightViewProj;
    XMFLOAT4 sunDirAmbient;
    XMFLOAT4 camPosFog;
    XMFLOAT4 screen;        // xy = viewport, z = shadow texel, w = shadows on
    XMFLOAT4 viewer;        // xy = local tank xz, w = LOS box count
    XMFLOAT4 losBoxes[24];  // STEALTH occluders
};

constexpr UINT ShadowSrvSlot = MaxTextures - 1;   // last material slot is reserved

struct VfxCB
{
    XMFLOAT4X4 viewProj;
    XMFLOAT4X4 invViewProj;
    XMFLOAT4 camRight;
    XMFLOAT4 camUp;
    XMFLOAT4 camPos;
    XMFLOAT4 screenTime;    // w, h, time, unused
    XMFLOAT4 counts;        // bursts, scorches
    XMFLOAT4 bursts[16];
    XMFLOAT4 scorches[16];
};

constexpr int VfxParticlesPerBurst = 16;
constexpr int MaxUiBurnQuads = 32;

struct UiBurnCB
{
    XMFLOAT4 rect[MaxUiBurnQuads];
    XMFLOAT4 color[MaxUiBurnQuads];
    XMFLOAT4 param[MaxUiBurnQuads];
    XMFLOAT4 uv[MaxUiBurnQuads];
    XMFLOAT4 misc;   // count, time, cell size, unused
};

struct PerObjectCB
{
    XMFLOAT4X4 world;
    XMFLOAT4 tint;
    XMFLOAT4 misc;   // x = dynamic-object flag
    XMFLOAT4 misc2;  // x = STEALTH LOS-clip flag
};

struct PostCB
{
    XMFLOAT4X4 viewProj;
    XMFLOAT4X4 invViewProj;
    XMFLOAT4X4 prevViewProj;
    XMFLOAT4 camPos;        // w = frame index
    XMFLOAT4 prevCamPos;
    XMFLOAT4 screen;        // w, h, 1/w, 1/h
    XMFLOAT4 params;        // giRays, temporalSamples, giOn, aoOn
    XMFLOAT4 params2;       // giIntensity, aoRadius, aoStrength, skyGi
    XMFLOAT4 params3;       // giW, giH, 1/giW, 1/giH
};

// the slot map above only holds while these stay inside their reservations
static_assert(sizeof(PerFrameCB) <= FrameCbSlots * CbAlign,
              "PerFrameCB outgrew its slots: update the CB ring map");
static_assert(sizeof(PostCB) <= (ShadowCbSlot - MaxObjectsPerFrame) * CbAlign,
              "PostCB outgrew its slots: update the CB ring map");
static_assert(sizeof(PerFrameCB) <= (VfxCbSlot - ShadowCbSlot) * CbAlign,
              "shadow PerFrameCB outgrew its slots: update the CB ring map");
static_assert(sizeof(VfxCB) <= (BurnCbSlot - VfxCbSlot) * CbAlign,
              "VfxCB outgrew its slots: update the CB ring map");
static_assert(sizeof(UiBurnCB) <= PaletteBase - BurnCbSlot * CbAlign,
              "UiBurnCB outgrew its slots: update the CB ring map");

struct GpuMesh
{
    ComPtr<ID3D12Resource> vb;
    ComPtr<ID3D12Resource> ib;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW ibv{};
    UINT indexCount = 0;
    bool skinned = false;
    BoundingSphere bounds;   // mesh space; culling per pass
};

// Offscreen target with tracked resource state.
struct Target
{
    ComPtr<ID3D12Resource> res;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    int rtvIndex = -1;
};

std::string ReadTextFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

ComPtr<ID3DBlob> Compile(const std::string& src, const char* file, const char* entry,
                         const char* target, std::string& error)
{
    ComPtr<ID3DBlob> blob, errs;
    HRESULT hr = D3DCompile(src.data(), src.size(), file, nullptr, nullptr,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &blob, &errs);
    if (FAILED(hr))
    {
        error = errs ? std::string(static_cast<const char*>(errs->GetBufferPointer()),
                                   errs->GetBufferSize())
                     : "unknown shader compile error";
        return nullptr;
    }
    return blob;
}

D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    return p;
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 size)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

class RendererD3D12 final : public IRenderer
{
public:
    ~RendererD3D12() override
    {
        if (m_device)
            WaitForGpuIdle();
        if (m_fenceEvent)
            CloseHandle(m_fenceEvent);
    }

    bool Init(void* hwnd, int width, int height, std::string& error) override
    {
        m_hwnd = static_cast<HWND>(hwnd);
        m_width = width;
        m_height = height;

#ifdef _DEBUG
        {
            ComPtr<ID3D12Debug> dbg;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
                dbg->EnableDebugLayer();
        }
#endif
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        { error = "CreateDXGIFactory1 failed"; return false; }

        ComPtr<IDXGIFactory6> factory6;
        factory.As(&factory6);
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = E_FAIL;
        if (factory6)
        {
            for (UINT i = 0;
                 factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                      IID_PPV_ARGS(&adapter)) == S_OK; ++i)
            {
                DXGI_ADAPTER_DESC1 d{};
                adapter->GetDesc1(&d);
                if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                    continue;
                hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                       IID_PPV_ARGS(&m_device));
                if (SUCCEEDED(hr))
                    break;
            }
        }
        if (FAILED(hr))
            hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
        if (FAILED(hr)) { error = "D3D12CreateDevice failed"; return false; }

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue))))
        { error = "CreateCommandQueue failed"; return false; }

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width;
        sd.Height = height;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = FrameCount;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_NONE;   // size mismatch = cut, never stretch
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(factory->CreateSwapChainForHwnd(m_queue.Get(), m_hwnd, &sd,
                                                   nullptr, nullptr, &sc1)))
        { error = "CreateSwapChainForHwnd failed"; return false; }
        sc1.As(&m_swapChain);
        factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);

        D3D12_DESCRIPTOR_HEAP_DESC rtvd{};
        rtvd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvd.NumDescriptors = 12;
        m_device->CreateDescriptorHeap(&rtvd, IID_PPV_ARGS(&m_rtvHeap));
        m_rtvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC dsvd{};
        dsvd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvd.NumDescriptors = 3;   // scene depth + shadow map + read-only depth
        m_device->CreateDescriptorHeap(&dsvd, IID_PPV_ARGS(&m_dsvHeap));
        m_dsvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        D3D12_DESCRIPTOR_HEAP_DESC srvd{};
        srvd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvd.NumDescriptors = SrvHeapSize;
        srvd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        m_device->CreateDescriptorHeap(&srvd, IID_PPV_ARGS(&m_srvHeap));
        m_srvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        for (int i = 0; i < FramesInFlight; ++i)
        {
            if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&m_alloc[i]))))
            { error = "CreateCommandAllocator failed"; return false; }
        }
        if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               m_alloc[0].Get(), nullptr,
                                               IID_PPV_ARGS(&m_cmd))))
        { error = "CreateCommandList failed"; return false; }
        m_cmd->Close();

        if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
        { error = "CreateFence failed"; return false; }
        m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        for (int i = 0; i < FramesInFlight; ++i)
        {
            auto hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
            auto bd = BufferDesc(PaletteBase + MaxSkinnedPerFrame * PaletteBytes);
            if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_cbUpload[i]))))
            { error = "cb upload failed"; return false; }
            m_cbUpload[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_cbMapped[i]));

            bd = BufferDesc(UiVbBytes);
            if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_uiVb[i]))))
            { error = "ui vb failed"; return false; }
            m_uiVb[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_uiMapped[i]));

            if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_uiTexVb[i]))))
            { error = "ui tex vb failed"; return false; }
            m_uiTexVb[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_uiTexMapped[i]));
        }

        if (!CreateSizedResources(error))
            return false;
        if (!CreatePipeline(error))
            return false;
        Log("D3D12: initialized %dx%d", width, height);
        return true;
    }

    void Resize(int width, int height) override
    {
        if (!m_swapChain || width <= 0 || height <= 0)
            return;
        WaitForGpuIdle();
        for (int i = 0; i < FrameCount; ++i)
            m_backbuffers[i].Reset();
        m_depth.Reset();
        m_width = width;
        m_height = height;
        m_swapChain->ResizeBuffers(FrameCount, width, height, DXGI_FORMAT_UNKNOWN, 0);
        std::string err;
        CreateSizedResources(err);
        m_historyValid = false;
    }

    int CreateMesh(const Vertex* verts, size_t vertexCount,
                   const uint32_t* indices, size_t indexCount) override
    {
        GpuMesh mesh;
        UINT64 vbSize = vertexCount * sizeof(Vertex);
        UINT64 ibSize = indexCount * sizeof(uint32_t);
        mesh.vb = CreateStaticBuffer(verts, vbSize);
        mesh.ib = CreateStaticBuffer(indices, ibSize);
        if (!mesh.vb || !mesh.ib)
            return -1;
        mesh.vbv = { mesh.vb->GetGPUVirtualAddress(), UINT(vbSize), sizeof(Vertex) };
        mesh.ibv = { mesh.ib->GetGPUVirtualAddress(), UINT(ibSize), DXGI_FORMAT_R32_UINT };
        mesh.indexCount = UINT(indexCount);
        mesh.bounds = ComputeMeshBounds(verts, vertexCount);
        m_meshes.push_back(std::move(mesh));
        return int(m_meshes.size()) - 1;
    }

    int CreateSkinnedMesh(const SkinnedVertex* verts, size_t vertexCount,
                          const uint32_t* indices, size_t indexCount) override
    {
        GpuMesh mesh;
        UINT64 vbSize = vertexCount * sizeof(SkinnedVertex);
        UINT64 ibSize = indexCount * sizeof(uint32_t);
        mesh.vb = CreateStaticBuffer(verts, vbSize);
        mesh.ib = CreateStaticBuffer(indices, ibSize);
        if (!mesh.vb || !mesh.ib)
            return -1;
        mesh.vbv = { mesh.vb->GetGPUVirtualAddress(), UINT(vbSize),
                     sizeof(SkinnedVertex) };
        mesh.ibv = { mesh.ib->GetGPUVirtualAddress(), UINT(ibSize),
                     DXGI_FORMAT_R32_UINT };
        mesh.indexCount = UINT(indexCount);
        mesh.skinned = true;
        mesh.bounds = ComputeMeshBounds(verts, vertexCount);
        // animation reaches beyond the rest pose: inflate generously
        mesh.bounds.radius = mesh.bounds.radius * 1.6f + 0.5f;
        m_meshes.push_back(std::move(mesh));
        return int(m_meshes.size()) - 1;
    }

    int CreateTexture(const uint8_t* rgba, int width, int height) override
    {
        if (m_textureCount >= int(ShadowSrvSlot))   // last slot holds the shadow SRV
            return -1;
        ImageData src;
        src.width = width;
        src.height = height;
        src.rgba.assign(rgba, rgba + size_t(width) * height * 4);
        auto chain = BuildMipChain(src);

        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = width;
        td.Height = height;
        td.DepthOrArraySize = 1;
        td.MipLevels = UINT16(chain.size());
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;

        ComPtr<ID3D12Resource> tex;
        auto hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))))
            return -1;

        UINT64 uploadSize = 0;
        std::vector<UINT> rowPitch(chain.size());
        std::vector<UINT64> mipOffset(chain.size());
        for (size_t m = 0; m < chain.size(); ++m)
        {
            rowPitch[m] = (UINT(chain[m].width) * 4 + 255) & ~255u;
            mipOffset[m] = uploadSize;
            uploadSize += UINT64(rowPitch[m]) * chain[m].height;
            uploadSize = (uploadSize + 511) & ~511ull;
        }

        ComPtr<ID3D12Resource> upload;
        auto hpUp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = BufferDesc(uploadSize);
        if (FAILED(m_device->CreateCommittedResource(&hpUp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
            return -1;
        uint8_t* mapped = nullptr;
        upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        for (size_t m = 0; m < chain.size(); ++m)
            for (int y = 0; y < chain[m].height; ++y)
                memcpy(mapped + mipOffset[m] + UINT64(y) * rowPitch[m],
                       chain[m].rgba.data() + size_t(y) * chain[m].width * 4,
                       size_t(chain[m].width) * 4);
        upload->Unmap(0, nullptr);

        m_alloc[0]->Reset();
        m_cmd->Reset(m_alloc[0].Get(), nullptr);
        for (size_t m = 0; m < chain.size(); ++m)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = tex.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = UINT(m);
            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = upload.Get();
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.PlacedFootprint.Offset = mipOffset[m];
            srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srcLoc.PlacedFootprint.Footprint.Width = UINT(chain[m].width);
            srcLoc.PlacedFootprint.Footprint.Height = UINT(chain[m].height);
            srcLoc.PlacedFootprint.Footprint.Depth = 1;
            srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch[m];
            m_cmd->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
        }
        BarrierRaw(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_cmd->Close();
        ID3D12CommandList* lists[] = { m_cmd.Get() };
        m_queue->ExecuteCommandLists(1, lists);
        WaitForGpuIdle();

        D3D12_CPU_DESCRIPTOR_HANDLE handle = CpuSrv(m_textureCount);
        m_device->CreateShaderResourceView(tex.Get(), nullptr, handle);
        m_textures.push_back(tex);
        return m_textureCount++;
    }

    void RenderFrame(const FrameData& frame) override
    {
        int fi = m_frameIndex % FramesInFlight;
        if (m_fenceValues[fi] != 0 && m_fence->GetCompletedValue() < m_fenceValues[fi])
        {
            m_fence->SetEventOnCompletion(m_fenceValues[fi], m_fenceEvent);
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }

        if (frame.post.giHalfRes != m_giHalf)
        {
            WaitForGpuIdle();
            m_giHalf = frame.post.giHalfRes;
            std::string err;
            CreateGiTargets(err);
            RefreshPostDescriptors();
            m_historyValid = false;
        }
        if (frame.post.shadowMapSize != m_shadowSize)
        {
            WaitForGpuIdle();
            m_shadowSize = frame.post.shadowMapSize;
            m_shadowMap.Reset();
            std::string err;
            CreateShadowMap(err);
        }

        UINT back = m_swapChain->GetCurrentBackBufferIndex();
        m_alloc[fi]->Reset();
        m_cmd->Reset(m_alloc[fi].Get(), m_psoMesh.Get());

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        m_cmd->SetDescriptorHeaps(1, heaps);

        D3D12_VIEWPORT vp{ 0, 0, float(m_width), float(m_height), 0, 1 };
        D3D12_RECT sc{ 0, 0, LONG(m_width), LONG(m_height) };
        m_cmd->RSSetViewports(1, &vp);
        m_cmd->RSSetScissorRects(1, &sc);

        // ---------------- constants ----------------
        PerFrameCB pf{};
        pf.viewProj = frame.viewProj;
        pf.lightViewProj = frame.lightViewProj;
        pf.sunDirAmbient = XMFLOAT4(frame.sunDir.x, frame.sunDir.y, frame.sunDir.z, frame.ambient);
        pf.camPosFog = XMFLOAT4(frame.camPos.x, frame.camPos.y, frame.camPos.z, frame.fogDensity);
        pf.viewer = XMFLOAT4(frame.losViewer.x, frame.losViewer.y, 0.0f,
                             float(frame.losBoxCount));
        memcpy(pf.losBoxes, frame.losBoxes, sizeof(pf.losBoxes));
        pf.screen = XMFLOAT4(float(m_width), float(m_height), 1.0f / m_shadowSize,
                             frame.post.shadowsEnabled ? float(1 + frame.post.shadowFilter) : 0.0f);
        memcpy(m_cbMapped[fi], &pf, sizeof(pf));
        D3D12_GPU_VIRTUAL_ADDRESS cbBase = m_cbUpload[fi]->GetGPUVirtualAddress();

        VfxCB vc{};
        size_t numBursts = std::min<size_t>(frame.bursts.size(), 16);
        size_t numScorches = std::min<size_t>(frame.scorches.size(), 16);
        vc.viewProj = frame.viewProj;
        vc.invViewProj = frame.invViewProj;
        vc.camRight = XMFLOAT4(frame.camRight.x, frame.camRight.y, frame.camRight.z, 0);
        vc.camUp = XMFLOAT4(frame.camUp.x, frame.camUp.y, frame.camUp.z, 0);
        vc.camPos = XMFLOAT4(frame.camPos.x, frame.camPos.y, frame.camPos.z, 0);
        vc.screenTime = XMFLOAT4(float(m_width), float(m_height), frame.time, 0);
        vc.counts = XMFLOAT4(float(numBursts), float(numScorches), 0, 0);
        for (size_t i = 0; i < numBursts; ++i)
            vc.bursts[i] = XMFLOAT4(frame.bursts[i].pos.x, frame.bursts[i].pos.y,
                                    frame.bursts[i].pos.z, frame.bursts[i].age);
        for (size_t i = 0; i < numScorches; ++i)
            vc.scorches[i] = XMFLOAT4(frame.scorches[i].pos.x, frame.scorches[i].pos.y,
                                      frame.scorches[i].pos.z, frame.scorches[i].age);
        UINT64 vfxCbOffset = UINT64(VfxCbSlot) * CbAlign;
        memcpy(m_cbMapped[fi] + vfxCbOffset, &vc, sizeof(vc));

        PerFrameCB sf = pf;
        sf.viewProj = frame.lightViewProj;   // shadow pass renders from the sun
        UINT64 shadowCbOffset = UINT64(ShadowCbSlot) * CbAlign;
        memcpy(m_cbMapped[fi] + shadowCbOffset, &sf, sizeof(sf));

        // Bone palettes for skinned draws (own ring region, root CBV b2).
        UINT numPalettes = UINT(std::min<size_t>(frame.palettes.size(),
                                                 MaxSkinnedPerFrame));
        for (UINT p = 0; p < numPalettes; ++p)
            memcpy(m_cbMapped[fi] + PaletteBase + size_t(p) * PaletteBytes,
                   frame.palettes[p].m, sizeof(frame.palettes[p].m));
        auto paletteAddr = [&](const RenderObject* obj) -> D3D12_GPU_VIRTUAL_ADDRESS
        {
            UINT pi = (obj->paletteIndex >= 0
                       && obj->paletteIndex < int(numPalettes))
                          ? UINT(obj->paletteIndex) : 0;
            return cbBase + PaletteBase + UINT64(pi) * PaletteBytes;
        };

        // Per-object constants are written once and shared by both passes.
        // FRUSTUM CULLING: each pass tests the mesh's world-space sphere
        // against its own frustum; an object visible in neither pass costs
        // nothing -- not even a CB slot.
        Frustum viewF = FrustumFromViewProj(frame.viewProj);
        Frustum lightF = FrustumFromViewProj(frame.lightViewProj);
        struct Drawable { const RenderObject* obj; UINT slot;
                          bool inView; bool inLight; };
        std::vector<Drawable> drawables;
        drawables.reserve(frame.objects.size());
        {
            UINT objIndex = FrameCbSlots;   // frame CB owns slots 0..2
            for (const RenderObject& obj : frame.objects)
            {
                if (obj.mesh < 0 || obj.mesh >= int(m_meshes.size())
                    || objIndex >= MaxObjectsPerFrame)
                    continue;
                XMFLOAT3 wc;
                float wr;
                TransformSphere(m_meshes[obj.mesh].bounds, obj.world, wc, wr);
                bool inView = SphereInFrustum(viewF, wc, wr);
                bool inLight = frame.post.shadowsEnabled && !obj.noShadow
                            && SphereInFrustum(lightF, wc, wr);
                if (!inView && !inLight)
                    continue;
                PerObjectCB po{};
                po.world = obj.world;
                po.tint = obj.tint;
                po.misc = XMFLOAT4(obj.isDynamic ? 1.0f : 0.0f,
                                   obj.deformDist, obj.deformAge,
                                   obj.deformDist >= 0.0f ? 1.0f : 0.0f);
                po.misc2 = XMFLOAT4(obj.losClip, 0.0f, 0.0f, 0.0f);
                memcpy(m_cbMapped[fi] + size_t(objIndex) * CbAlign, &po, sizeof(po));
                drawables.push_back({ &obj, objIndex, inView, inLight });
                ++objIndex;
            }
        }

        if (!m_historyValid)
        {
            m_prevViewProj = frame.viewProj;
            m_prevCamPos = frame.camPos;
        }
        PostCB pc{};
        pc.viewProj = frame.viewProj;
        pc.invViewProj = frame.invViewProj;
        pc.prevViewProj = m_prevViewProj;
        pc.camPos = XMFLOAT4(frame.camPos.x, frame.camPos.y, frame.camPos.z,
                             float(m_frameIndex % 1024));
        pc.prevCamPos = XMFLOAT4(m_prevCamPos.x, m_prevCamPos.y, m_prevCamPos.z, 0);
        pc.screen = XMFLOAT4(float(m_width), float(m_height),
                             1.0f / m_width, 1.0f / m_height);
        pc.params = XMFLOAT4(float(frame.post.giRays), float(frame.post.temporalSamples),
                             frame.post.giEnabled ? 1.0f : 0.0f,
                             frame.post.aoEnabled ? 1.0f : 0.0f);
        pc.params2 = XMFLOAT4(frame.post.giIntensity, 0.9f, 1.0f, 0.35f);
        pc.params3 = XMFLOAT4(float(m_giW), float(m_giH), 1.0f / m_giW, 1.0f / m_giH);
        UINT64 postCbOffset = UINT64(MaxObjectsPerFrame) * CbAlign;
        memcpy(m_cbMapped[fi] + postCbOffset, &pc, sizeof(pc));

        // ---------------- shadow pass (depth only, sun's ortho camera) ----------------
        if (frame.post.shadowsEnabled)
        {
            if (m_shadowState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
            {
                BarrierRaw(m_shadowMap.Get(), m_shadowState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                m_shadowState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
            shadowDsv.ptr += m_dsvSize;
            m_cmd->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            m_cmd->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
            D3D12_VIEWPORT svp{ 0, 0, float(m_shadowSize), float(m_shadowSize), 0, 1 };
            D3D12_RECT ssc{ 0, 0, LONG(m_shadowSize), LONG(m_shadowSize) };
            m_cmd->RSSetViewports(1, &svp);
            m_cmd->RSSetScissorRects(1, &ssc);

            m_cmd->SetGraphicsRootSignature(m_rootSig.Get());
            m_cmd->SetGraphicsRootConstantBufferView(0, cbBase + shadowCbOffset);
            m_cmd->SetGraphicsRootDescriptorTable(2, GpuSrv(0));
            m_cmd->SetGraphicsRootDescriptorTable(3, GpuSrv(ShadowSrvSlot));
            m_cmd->SetGraphicsRootDescriptorTable(4, GpuSrv(0));   // unused (no PS)
            m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_cmd->SetPipelineState(m_psoShadow.Get());
            bool shSkinned = false;
            for (const auto& d : drawables)
            {
                if (!d.inLight)
                    continue;   // culled from the sun's box (or noShadow)
                const GpuMesh& mesh = m_meshes[d.obj->mesh];
                if (mesh.skinned != shSkinned)
                {
                    shSkinned = mesh.skinned;
                    m_cmd->SetPipelineState(shSkinned ? m_psoShadowSkinned.Get()
                                                      : m_psoShadow.Get());
                }
                if (mesh.skinned)
                    m_cmd->SetGraphicsRootConstantBufferView(5, paletteAddr(d.obj));
                m_cmd->SetGraphicsRootConstantBufferView(1, cbBase + d.slot * CbAlign);
                m_cmd->IASetVertexBuffers(0, 1, &mesh.vbv);
                m_cmd->IASetIndexBuffer(&mesh.ibv);
                m_cmd->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
            }
            BarrierRaw(m_shadowMap.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_shadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

            m_cmd->RSSetViewports(1, &vp);
            m_cmd->RSSetScissorRects(1, &sc);
        }
        else if (m_shadowState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        {
            BarrierRaw(m_shadowMap.Get(), m_shadowState,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_shadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        // ---------------- scene pass (G-buffer MRT) ----------------
        Ensure(m_sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Ensure(m_sceneNormal, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Ensure(m_sceneAlbedo, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (m_depthState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
        {
            BarrierRaw(m_depth.Get(), m_depthState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            m_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE mrt[3] = { Rtv(m_sceneColor.rtvIndex),
                                               Rtv(m_sceneNormal.rtvIndex),
                                               Rtv(m_sceneAlbedo.rtvIndex) };
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        float clearColor[4] = { 0.62f, 0.72f, 0.83f, 1.0f };
        float clearNormal[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
        float clearBlack[4] = { 0, 0, 0, 0 };
        m_cmd->ClearRenderTargetView(mrt[0], clearColor, 0, nullptr);
        m_cmd->ClearRenderTargetView(mrt[1], clearNormal, 0, nullptr);
        m_cmd->ClearRenderTargetView(mrt[2], clearBlack, 0, nullptr);
        m_cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        m_cmd->OMSetRenderTargets(3, mrt, FALSE, &dsv);

        m_cmd->SetGraphicsRootSignature(m_rootSig.Get());
        m_cmd->SetGraphicsRootConstantBufferView(0, cbBase);
        m_cmd->SetGraphicsRootDescriptorTable(3, GpuSrv(ShadowSrvSlot));
        m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_cmd->SetPipelineState(m_psoMesh.Get());

        bool sceneSkinned = false;
        for (const auto& d : drawables)
        {
            if (!d.inView)
                continue;       // culled from the camera frustum
            const GpuMesh& mesh = m_meshes[d.obj->mesh];
            if (mesh.skinned != sceneSkinned)
            {
                sceneSkinned = mesh.skinned;
                m_cmd->SetPipelineState(sceneSkinned ? m_psoMeshSkinned.Get()
                                                     : m_psoMesh.Get());
            }
            if (mesh.skinned)
                m_cmd->SetGraphicsRootConstantBufferView(5, paletteAddr(d.obj));
            m_cmd->SetGraphicsRootConstantBufferView(1, cbBase + d.slot * CbAlign);
            int texIdx = (d.obj->texture >= 0 && d.obj->texture < m_textureCount)
                             ? d.obj->texture : 0;
            m_cmd->SetGraphicsRootDescriptorTable(2, GpuSrv(texIdx));
            int nraIdx = d.obj->texNormal >= 0 ? d.obj->texNormal
                                               : frame.defaultNormalTex;
            if (nraIdx < 0 || nraIdx >= m_textureCount)
                nraIdx = texIdx;
            m_cmd->SetGraphicsRootDescriptorTable(4, GpuSrv(nraIdx));
            m_cmd->IASetVertexBuffers(0, 1, &mesh.vbv);
            m_cmd->IASetIndexBuffer(&mesh.ibv);
            m_cmd->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
        }

        // Normals + depth become shader inputs; color/albedo stay writable for VFX.
        Ensure(m_sceneNormal, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        BarrierRaw(m_depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                   D3D12_RESOURCE_STATE_DEPTH_READ
                   | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_depthState = D3D12_RESOURCE_STATE_DEPTH_READ
                     | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        // ---------------- VFX: scorch decals + smoke billboards ----------------
        if (numScorches > 0 || numBursts > 0)
        {
            m_cmd->SetGraphicsRootSignature(m_rootSigPost.Get());
            m_cmd->SetGraphicsRootConstantBufferView(0, cbBase + vfxCbOffset);
            m_cmd->SetGraphicsRootDescriptorTable(1, GpuSrv(PostSrvBase + 64));

            if (numScorches > 0)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE rts[2] = { Rtv(m_sceneColor.rtvIndex),
                                                       Rtv(m_sceneAlbedo.rtvIndex) };
                m_cmd->OMSetRenderTargets(2, rts, FALSE, nullptr);
                m_cmd->SetPipelineState(m_psoScorch.Get());
                m_cmd->DrawInstanced(3, 1, 0, 0);
            }
            if (numBursts > 0)
            {
                // READ-ONLY depth view (slot 2): matches the resource's
                // DEPTH_READ | PIXEL_SHADER_RESOURCE state at this point
                D3D12_CPU_DESCRIPTOR_HANDLE colorRtv = Rtv(m_sceneColor.rtvIndex);
                D3D12_CPU_DESCRIPTOR_HANDLE sceneDsvRO =
                    m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
                sceneDsvRO.ptr += size_t(m_dsvSize) * 2;
                m_cmd->OMSetRenderTargets(1, &colorRtv, FALSE, &sceneDsvRO);
                m_cmd->SetPipelineState(m_psoVfx.Get());
                m_cmd->DrawInstanced(6, UINT(numBursts) * VfxParticlesPerBurst, 0, 0);
            }
        }

        Ensure(m_sceneColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Ensure(m_sceneAlbedo, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        m_cmd->SetGraphicsRootSignature(m_rootSigPost.Get());
        m_cmd->SetGraphicsRootConstantBufferView(0, cbBase + postCbOffset);

        // ---------------- SSAO + blur ----------------
        if (frame.post.aoEnabled)
        {
            RunPost(m_psoSsao.Get(), m_ao, PostSrvBase + 0);
            RunPost(m_psoAoBlurH.Get(), m_aoTmp, PostSrvBase + 48);
            RunPost(m_psoAoBlurV.Get(), m_ao, PostSrvBase + 56);
        }

        // ---------------- SSGI + temporal (at GI resolution) ----------------
        int written = m_accumIndex;
        if (frame.post.giEnabled)
        {
            D3D12_VIEWPORT giVp{ 0, 0, float(m_giW), float(m_giH), 0, 1 };
            D3D12_RECT giSc{ 0, 0, LONG(m_giW), LONG(m_giH) };
            m_cmd->RSSetViewports(1, &giVp);
            m_cmd->RSSetScissorRects(1, &giSc);

            RunPost(m_psoSsgi.Get(), m_gi, PostSrvBase + 8);
            int cur = m_accumIndex;
            RunPost(m_psoTemporal.Get(), m_accum[cur],
                    PostSrvBase + (cur == 0 ? 16 : 24));
            written = cur;
            m_accumIndex = 1 - cur;

            m_cmd->RSSetViewports(1, &vp);
            m_cmd->RSSetScissorRects(1, &sc);
        }
        // Everything the composite reads must be in PSR even when passes are off.
        Ensure(m_ao, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Ensure(m_gi, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Ensure(m_accum[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Ensure(m_accum[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Ensure(m_aoTmp, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // ---------------- composite (+ optional edge AA) ----------------
        BarrierRaw(m_backbuffers[back].Get(), D3D12_RESOURCE_STATE_PRESENT,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE backRtv = Rtv(int(back));
        if (frame.post.aaEnabled)
        {
            RunPost(m_psoComposite.Get(), m_final,
                    PostSrvBase + (written == 0 ? 32 : 40));
            m_cmd->OMSetRenderTargets(1, &backRtv, FALSE, nullptr);
            m_cmd->SetPipelineState(m_psoAA.Get());
            m_cmd->SetGraphicsRootDescriptorTable(1, GpuSrv(PostSrvBase + 72));
            m_cmd->DrawInstanced(3, 1, 0, 0);
        }
        else
        {
            m_cmd->OMSetRenderTargets(1, &backRtv, FALSE, nullptr);
            m_cmd->SetPipelineState(m_psoComposite.Get());
            m_cmd->SetGraphicsRootDescriptorTable(1, GpuSrv(PostSrvBase + (written == 0 ? 32 : 40)));
            m_cmd->DrawInstanced(3, 1, 0, 0);
        }

        // ---------------- UI ----------------
        size_t uiBytes = frame.ui.size() * sizeof(UiVertex);
        if (!frame.ui.empty() && uiBytes <= UiVbBytes)
        {
            memcpy(m_uiMapped[fi], frame.ui.data(), uiBytes);
            m_cmd->SetGraphicsRootSignature(m_rootSig.Get());
            m_cmd->SetGraphicsRootConstantBufferView(0, cbBase);
            m_cmd->SetGraphicsRootConstantBufferView(1, cbBase);
            m_cmd->SetGraphicsRootDescriptorTable(2, GpuSrv(0));
            m_cmd->SetGraphicsRootDescriptorTable(3, GpuSrv(ShadowSrvSlot));
            m_cmd->SetPipelineState(m_psoUi.Get());
            D3D12_VERTEX_BUFFER_VIEW uivbv{ m_uiVb[fi]->GetGPUVirtualAddress(),
                                            UINT(uiBytes), sizeof(UiVertex) };
            m_cmd->IASetVertexBuffers(0, 1, &uivbv);
            m_cmd->DrawInstanced(UINT(frame.ui.size()), 1, 0, 0);

            bool haveAtlas = frame.uiTexTexture >= 0
                          && frame.uiTexTexture < m_textureCount;

            // textured UI (icon atlas quads)
            size_t texBytes = frame.uiTex.size() * sizeof(UiTexVertex);
            if (!frame.uiTex.empty() && texBytes <= UiVbBytes && haveAtlas)
            {
                memcpy(m_uiTexMapped[fi], frame.uiTex.data(), texBytes);
                m_cmd->SetGraphicsRootDescriptorTable(2, GpuSrv(frame.uiTexTexture));
                m_cmd->SetPipelineState(m_psoUiTex.Get());
                D3D12_VERTEX_BUFFER_VIEW tvbv{ m_uiTexVb[fi]->GetGPUVirtualAddress(),
                                               UINT(texBytes), sizeof(UiTexVertex) };
                m_cmd->IASetVertexBuffers(0, 1, &tvbv);
                m_cmd->DrawInstanced(UINT(frame.uiTex.size()), 1, 0, 0);
            }

            // burning shop cards: vertex-pulled quads, dissolve shader
            size_t burnCount = std::min<size_t>(frame.uiBurn.size(), MaxUiBurnQuads);
            if (burnCount > 0)
            {
                UiBurnCB bc{};
                for (size_t q = 0; q < burnCount; ++q)
                {
                    const UiBurnQuad& b = frame.uiBurn[q];
                    bc.rect[q] = XMFLOAT4(b.x, b.y, b.w, b.h);
                    bc.color[q] = XMFLOAT4(b.r, b.g, b.b, b.a);
                    bc.param[q] = XMFLOAT4(b.originX, b.originY, b.progress, b.maxRadius);
                    bc.uv[q] = XMFLOAT4(b.u0, b.v0, b.u1, b.v1);
                }
                bc.misc = XMFLOAT4(float(burnCount), frame.time, 5.0f, 0);
                UINT64 burnCbOffset = UINT64(BurnCbSlot) * CbAlign;
                memcpy(m_cbMapped[fi] + burnCbOffset, &bc, sizeof(bc));
                m_cmd->SetGraphicsRootConstantBufferView(1, cbBase + burnCbOffset);
                if (haveAtlas)
                    m_cmd->SetGraphicsRootDescriptorTable(2, GpuSrv(frame.uiTexTexture));
                m_cmd->SetPipelineState(m_psoUiBurn.Get());
                m_cmd->DrawInstanced(6, UINT(burnCount), 0, 0);
            }
        }

        BarrierRaw(m_backbuffers[back].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PRESENT);
        m_cmd->Close();
        ID3D12CommandList* lists[] = { m_cmd.Get() };
        m_queue->ExecuteCommandLists(1, lists);
        m_swapChain->Present(frame.vsync ? 1 : 0, 0);

        m_fenceValues[fi] = ++m_nextFence;
        m_queue->Signal(m_fence.Get(), m_fenceValues[fi]);

        m_prevViewProj = frame.viewProj;
        m_prevCamPos = frame.camPos;
        m_historyValid = true;
        ++m_frameIndex;
    }

    bool SaveBackbufferPNG(const std::string& path) override
    {
        WaitForGpuIdle();
        UINT back = m_swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* src = m_backbuffers[back].Get();

        UINT rowPitch = (UINT(m_width) * 4 + 255) & ~255u;
        UINT64 size = UINT64(rowPitch) * m_height;
        ComPtr<ID3D12Resource> readback;
        auto hp = HeapProps(D3D12_HEAP_TYPE_READBACK);
        auto bd = BufferDesc(size);
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
            return false;

        m_alloc[0]->Reset();
        m_cmd->Reset(m_alloc[0].Get(), nullptr);
        BarrierRaw(src, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = UINT(m_width);
        dst.PlacedFootprint.Footprint.Height = UINT(m_height);
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = src;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        m_cmd->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
        BarrierRaw(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        m_cmd->Close();
        ID3D12CommandList* lists[] = { m_cmd.Get() };
        m_queue->ExecuteCommandLists(1, lists);
        WaitForGpuIdle();

        uint8_t* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
            return false;
        std::vector<uint8_t> pixels(size_t(m_width) * m_height * 4);
        for (int y = 0; y < m_height; ++y)
            memcpy(pixels.data() + size_t(y) * m_width * 4,
                   mapped + size_t(y) * rowPitch, size_t(m_width) * 4);
        readback->Unmap(0, nullptr);
        int ok = stbi_write_png(path.c_str(), m_width, m_height, 4,
                                pixels.data(), m_width * 4);
        Log("D3D12: screenshot %s -> %s", path.c_str(), ok ? "ok" : "FAILED");
        return ok != 0;
    }

    const char* Name() const override { return "D3D12"; }

private:
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(int index) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(index) * m_rtvSize;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE CpuSrv(int index) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(index) * m_srvSize;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE GpuSrv(int index) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += UINT64(index) * m_srvSize;
        return h;
    }

    void BarrierRaw(ID3D12Resource* res, D3D12_RESOURCE_STATES from,
                    D3D12_RESOURCE_STATES to)
    {
        if (from == to)
            return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_cmd->ResourceBarrier(1, &b);
    }

    void Ensure(Target& t, D3D12_RESOURCE_STATES desired)
    {
        if (t.state != desired)
        {
            BarrierRaw(t.res.Get(), t.state, desired);
            t.state = desired;
        }
    }

    void RunPost(ID3D12PipelineState* pso, Target& target, UINT srvGroup)
    {
        Ensure(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(target.rtvIndex);
        m_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        m_cmd->SetPipelineState(pso);
        m_cmd->SetGraphicsRootDescriptorTable(1, GpuSrv(srvGroup));
        m_cmd->DrawInstanced(3, 1, 0, 0);
        Ensure(target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    bool CreateTarget(Target& t, DXGI_FORMAT fmt, int rtvIndex, std::string& error,
                      int width, int height)
    {
        t.res.Reset();
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = width;
        td.Height = height;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = fmt;
        auto hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&t.res))))
        { error = "offscreen target failed"; return false; }
        t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        t.format = fmt;
        t.rtvIndex = rtvIndex;
        m_device->CreateRenderTargetView(t.res.Get(), nullptr, Rtv(rtvIndex));
        return true;
    }

    void WriteSrv(int slot, ID3D12Resource* res, DXGI_FORMAT fmt)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = fmt;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(res, &sd, CpuSrv(slot));
    }

    bool CreateSizedResources(std::string& error)
    {
        for (int i = 0; i < FrameCount; ++i)
        {
            if (FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backbuffers[i]))))
            { error = "GetBuffer failed"; return false; }
            m_device->CreateRenderTargetView(m_backbuffers[i].Get(), nullptr, Rtv(i));
        }

        // Depth: typeless resource, D32 DSV, R32 SRV.
        D3D12_RESOURCE_DESC dd{};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = m_width;
        dd.Height = m_height;
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        dd.Format = DXGI_FORMAT_R32_TYPELESS;
        dd.SampleDesc.Count = 1;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 1.0f;
        auto hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&m_depth))))
        { error = "depth failed"; return false; }
        m_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        D3D12_DEPTH_STENCIL_VIEW_DESC dvd{};
        dvd.Format = DXGI_FORMAT_D32_FLOAT;
        dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(m_depth.Get(), &dvd,
                                         m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
        // Read-only DSV (slot 2): the smoke pass depth-tests while the SAME
        // depth is bound as a pixel-shader SRV (soft particles). The resource
        // sits in DEPTH_READ | PIXEL_SHADER_RESOURCE then, and D3D12 only
        // permits that with a READ_ONLY_DEPTH view -- binding the writable
        // DSV was undefined behavior and made the smoke flicker (D3D11 was
        // immune: its VFX pass already used a read-only DSV).
        D3D12_DEPTH_STENCIL_VIEW_DESC dvr = dvd;
        dvr.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        D3D12_CPU_DESCRIPTOR_HANDLE dsvRO =
            m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsvRO.ptr += size_t(m_dsvSize) * 2;
        m_device->CreateDepthStencilView(m_depth.Get(), &dvr, dsvRO);

        if (!m_shadowMap && !CreateShadowMap(error))
            return false;

        if (!CreateTarget(m_sceneColor, DXGI_FORMAT_R8G8B8A8_UNORM, 3, error, m_width, m_height)) return false;
        if (!CreateTarget(m_sceneNormal, DXGI_FORMAT_R8G8B8A8_UNORM, 4, error, m_width, m_height)) return false;
        if (!CreateTarget(m_sceneAlbedo, DXGI_FORMAT_R8G8B8A8_UNORM, 5, error, m_width, m_height)) return false;
        if (!CreateTarget(m_ao, DXGI_FORMAT_R8_UNORM, 6, error, m_width, m_height)) return false;
        if (!CreateTarget(m_aoTmp, DXGI_FORMAT_R8_UNORM, 10, error, m_width, m_height)) return false;
        if (!CreateTarget(m_final, DXGI_FORMAT_R8G8B8A8_UNORM, 11, error, m_width, m_height)) return false;
        if (!CreateGiTargets(error))
            return false;
        RefreshPostDescriptors();
        return true;
    }

    bool CreateShadowMap(std::string& error)
    {
        D3D12_RESOURCE_DESC sd{};
        sd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        sd.Width = m_shadowSize;
        sd.Height = m_shadowSize;
        sd.DepthOrArraySize = 1;
        sd.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.SampleDesc.Count = 1;
        sd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE scv{};
        scv.Format = DXGI_FORMAT_D32_FLOAT;
        scv.DepthStencil.Depth = 1.0f;
        auto hps = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(m_device->CreateCommittedResource(&hps, D3D12_HEAP_FLAG_NONE, &sd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &scv, IID_PPV_ARGS(&m_shadowMap))))
        { error = "shadow map failed"; return false; }
        m_shadowState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        D3D12_DEPTH_STENCIL_VIEW_DESC sdvd{};
        sdvd.Format = DXGI_FORMAT_D32_FLOAT;
        sdvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += m_dsvSize;
        m_device->CreateDepthStencilView(m_shadowMap.Get(), &sdvd, h);
        WriteSrv(ShadowSrvSlot, m_shadowMap.Get(), DXGI_FORMAT_R32_FLOAT);
        Log("D3D12: shadow map %dx%d", m_shadowSize, m_shadowSize);
        return true;
    }

    bool CreateGiTargets(std::string& error)
    {
        m_giW = m_giHalf ? std::max(1, m_width / 2) : m_width;
        m_giH = m_giHalf ? std::max(1, m_height / 2) : m_height;
        if (!CreateTarget(m_gi, DXGI_FORMAT_R16G16B16A16_FLOAT, 7, error, m_giW, m_giH)) return false;
        if (!CreateTarget(m_accum[0], DXGI_FORMAT_R16G16B16A16_FLOAT, 8, error, m_giW, m_giH)) return false;
        if (!CreateTarget(m_accum[1], DXGI_FORMAT_R16G16B16A16_FLOAT, 9, error, m_giW, m_giH)) return false;

        // Clear both accumulation buffers so no garbage enters the history.
        m_alloc[0]->Reset();
        m_cmd->Reset(m_alloc[0].Get(), nullptr);
        float black[4] = { 0, 0, 0, 0 };
        m_cmd->ClearRenderTargetView(Rtv(m_accum[0].rtvIndex), black, 0, nullptr);
        m_cmd->ClearRenderTargetView(Rtv(m_accum[1].rtvIndex), black, 0, nullptr);
        m_cmd->Close();
        ID3D12CommandList* lists[] = { m_cmd.Get() };
        m_queue->ExecuteCommandLists(1, lists);
        WaitForGpuIdle();
        return true;
    }

    // Post-pass descriptor groups, spaced 8 apart (the root table spans 5 SRVs).
    void RefreshPostDescriptors()
    {
        ID3D12Resource* color = m_sceneColor.res.Get();
        ID3D12Resource* normal = m_sceneNormal.res.Get();
        ID3D12Resource* albedo = m_sceneAlbedo.res.Get();
        ID3D12Resource* depth = m_depth.Get();
        DXGI_FORMAT c8 = DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_FORMAT f16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
        DXGI_FORMAT r32 = DXGI_FORMAT_R32_FLOAT;
        DXGI_FORMAT r8 = DXGI_FORMAT_R8_UNORM;

        // Pad every slot with a valid descriptor first (table prefetch safety).
        for (UINT i = 0; i < 64; ++i)
            WriteSrv(PostSrvBase + i, depth, r32);

        // g0 SSAO: normal, depth
        WriteSrv(PostSrvBase + 0, normal, c8);
        WriteSrv(PostSrvBase + 1, depth, r32);
        // g1 SSGI: color, normal, depth
        WriteSrv(PostSrvBase + 8, color, c8);
        WriteSrv(PostSrvBase + 9, normal, c8);
        WriteSrv(PostSrvBase + 10, depth, r32);
        // g2 temporal writing accum0 (history = accum1)
        WriteSrv(PostSrvBase + 16, m_gi.res.Get(), f16);
        WriteSrv(PostSrvBase + 17, depth, r32);
        WriteSrv(PostSrvBase + 18, m_accum[1].res.Get(), f16);
        // g3 temporal writing accum1 (history = accum0)
        WriteSrv(PostSrvBase + 24, m_gi.res.Get(), f16);
        WriteSrv(PostSrvBase + 25, depth, r32);
        WriteSrv(PostSrvBase + 26, m_accum[0].res.Get(), f16);
        // g4 composite after accum0: color, accum0, ao, albedo, depth
        WriteSrv(PostSrvBase + 32, color, c8);
        WriteSrv(PostSrvBase + 33, m_accum[0].res.Get(), f16);
        WriteSrv(PostSrvBase + 34, m_ao.res.Get(), r8);
        WriteSrv(PostSrvBase + 35, albedo, c8);
        WriteSrv(PostSrvBase + 36, depth, r32);
        // g5 composite after accum1
        WriteSrv(PostSrvBase + 40, color, c8);
        WriteSrv(PostSrvBase + 41, m_accum[1].res.Get(), f16);
        WriteSrv(PostSrvBase + 42, m_ao.res.Get(), r8);
        WriteSrv(PostSrvBase + 43, albedo, c8);
        WriteSrv(PostSrvBase + 44, depth, r32);
        // g6 AO blur H: raw ao, depth
        WriteSrv(PostSrvBase + 48, m_ao.res.Get(), r8);
        WriteSrv(PostSrvBase + 49, depth, r32);
        // g7 AO blur V: ao temp, depth
        WriteSrv(PostSrvBase + 56, m_aoTmp.res.Get(), r8);
        WriteSrv(PostSrvBase + 57, depth, r32);
        // g8 VFX (scorch + smoke): depth, normal
        WriteSrv(PostSrvBase + 64, depth, r32);
        WriteSrv(PostSrvBase + 65, normal, c8);
        // g9 AA: composited image
        WriteSrv(PostSrvBase + 72, m_final.res.Get(), c8);
    }

    bool CreatePipeline(std::string& error)
    {
        std::string src = ReadTextFile("shaders/Basic.hlsl");
        if (src.empty()) { error = "shaders/Basic.hlsl not found"; return false; }
        std::string postSrc = ReadTextFile("shaders/Post.hlsl");
        if (postSrc.empty()) { error = "shaders/Post.hlsl not found"; return false; }
        std::string vfxSrc = ReadTextFile("shaders/Vfx.hlsl");
        if (vfxSrc.empty()) { error = "shaders/Vfx.hlsl not found"; return false; }

        ComPtr<ID3DBlob> vsMesh = Compile(src, "Basic.hlsl", "VSMesh", "vs_5_0", error);
        ComPtr<ID3DBlob> vsMeshSkin = Compile(src, "Basic.hlsl", "VSMeshSkinned", "vs_5_0", error);
        if (!vsMeshSkin) return false;
        ComPtr<ID3DBlob> psMesh = Compile(src, "Basic.hlsl", "PSMesh", "ps_5_0", error);
        ComPtr<ID3DBlob> psShadowClip = Compile(src, "Basic.hlsl", "PSShadowClip", "ps_5_0", error);
        ComPtr<ID3DBlob> vsUi = Compile(src, "Basic.hlsl", "VSUi", "vs_5_0", error);
        ComPtr<ID3DBlob> psUi = Compile(src, "Basic.hlsl", "PSUi", "ps_5_0", error);
        ComPtr<ID3DBlob> vsFull = Compile(postSrc, "Post.hlsl", "VSFullscreen", "vs_5_0", error);
        ComPtr<ID3DBlob> psSsao = Compile(postSrc, "Post.hlsl", "PSSSAO", "ps_5_0", error);
        ComPtr<ID3DBlob> psBlurH = Compile(postSrc, "Post.hlsl", "PSAOBlurH", "ps_5_0", error);
        ComPtr<ID3DBlob> psBlurV = Compile(postSrc, "Post.hlsl", "PSAOBlurV", "ps_5_0", error);
        ComPtr<ID3DBlob> psSsgi = Compile(postSrc, "Post.hlsl", "PSSSGI", "ps_5_0", error);
        ComPtr<ID3DBlob> psTemporal = Compile(postSrc, "Post.hlsl", "PSTemporal", "ps_5_0", error);
        ComPtr<ID3DBlob> psComposite = Compile(postSrc, "Post.hlsl", "PSComposite", "ps_5_0", error);
        ComPtr<ID3DBlob> psAA = Compile(postSrc, "Post.hlsl", "PSAA", "ps_5_0", error);
        ComPtr<ID3DBlob> vsUiBurn = Compile(src, "Basic.hlsl", "VSUiBurn", "vs_5_0", error);
        ComPtr<ID3DBlob> psUiBurn = Compile(src, "Basic.hlsl", "PSUiBurn", "ps_5_0", error);
        ComPtr<ID3DBlob> vsUiTex = Compile(src, "Basic.hlsl", "VSUiTex", "vs_5_0", error);
        ComPtr<ID3DBlob> psUiTex = Compile(src, "Basic.hlsl", "PSUiTex", "ps_5_0", error);
        ComPtr<ID3DBlob> vsVfx = Compile(vfxSrc, "Vfx.hlsl", "VSVfx", "vs_5_0", error);
        ComPtr<ID3DBlob> psVfx = Compile(vfxSrc, "Vfx.hlsl", "PSVfx", "ps_5_0", error);
        ComPtr<ID3DBlob> vsVfxFull = Compile(vfxSrc, "Vfx.hlsl", "VSVfxFull", "vs_5_0", error);
        ComPtr<ID3DBlob> psScorch = Compile(vfxSrc, "Vfx.hlsl", "PSScorch", "ps_5_0", error);
        if (!vsMesh || !psMesh || !vsUi || !psUi || !vsFull || !psSsao || !psBlurH
            || !psBlurV || !psSsgi || !psTemporal || !psComposite || !psAA
            || !vsUiBurn || !psUiBurn || !vsUiTex || !psUiTex
            || !vsVfx || !psVfx || !vsVfxFull || !psScorch)
            return false;

        // Main root signature: b0, b1, table t0, table t1 (shadow),
        // static aniso sampler s0 + comparison sampler s1.
        {
            D3D12_DESCRIPTOR_RANGE range{};
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = 1;
            range.BaseShaderRegister = 0;
            D3D12_DESCRIPTOR_RANGE rangeShadow = range;
            rangeShadow.BaseShaderRegister = 1;
            D3D12_DESCRIPTOR_RANGE rangeNra = range;
            rangeNra.BaseShaderRegister = 2;

            D3D12_ROOT_PARAMETER params[6]{};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[0].Descriptor.ShaderRegister = 0;
            params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[1].Descriptor.ShaderRegister = 1;
            params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[2].DescriptorTable.NumDescriptorRanges = 1;
            params[2].DescriptorTable.pDescriptorRanges = &range;
            params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[3].DescriptorTable.NumDescriptorRanges = 1;
            params[3].DescriptorTable.pDescriptorRanges = &rangeShadow;
            params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[4].DescriptorTable.NumDescriptorRanges = 1;
            params[4].DescriptorTable.pDescriptorRanges = &rangeNra;
            params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   // bones b2
            params[5].Descriptor.ShaderRegister = 2;
            params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

            D3D12_STATIC_SAMPLER_DESC smps[2]{};
            smps[0].Filter = D3D12_FILTER_ANISOTROPIC;
            smps[0].MaxAnisotropy = 8;
            smps[0].AddressU = smps[0].AddressV = smps[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            smps[0].MaxLOD = D3D12_FLOAT32_MAX;
            smps[0].ShaderRegister = 0;
            smps[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            smps[1] = smps[0];
            smps[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
            smps[1].MaxAnisotropy = 1;
            smps[1].AddressU = smps[1].AddressV = smps[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            smps[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            smps[1].ShaderRegister = 1;

            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters = 6;
            rsd.pParameters = params;
            rsd.NumStaticSamplers = 2;
            rsd.pStaticSamplers = smps;
            rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            if (!BuildRootSig(rsd, m_rootSig, error))
                return false;
        }

        // Post root signature: b0, table t0-t4, static point + linear clamp samplers.
        {
            D3D12_DESCRIPTOR_RANGE range{};
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = 5;
            range.BaseShaderRegister = 0;

            // VFX vertex shaders also read depth/normals, so everything is
            // visible to all stages.
            D3D12_ROOT_PARAMETER params[2]{};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[0].Descriptor.ShaderRegister = 0;
            params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1].DescriptorTable.NumDescriptorRanges = 1;
            params[1].DescriptorTable.pDescriptorRanges = &range;
            params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_STATIC_SAMPLER_DESC smps[2]{};
            smps[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            smps[0].AddressU = smps[0].AddressV = smps[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            smps[0].MaxLOD = D3D12_FLOAT32_MAX;
            smps[0].ShaderRegister = 0;
            smps[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            smps[1] = smps[0];
            smps[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            smps[1].ShaderRegister = 1;

            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters = 2;
            rsd.pParameters = params;
            rsd.NumStaticSamplers = 2;
            rsd.pStaticSamplers = smps;
            if (!BuildRootSig(rsd, m_rootSigPost, error))
                return false;
        }

        D3D12_INPUT_ELEMENT_DESC meshEls[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        D3D12_INPUT_ELEMENT_DESC uiEls[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.VS = { vsMesh->GetBufferPointer(), vsMesh->GetBufferSize() };
        pso.PS = { psMesh->GetBufferPointer(), psMesh->GetBufferSize() };
        for (int i = 0; i < 3; ++i)
            pso.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        pso.RasterizerState.FrontCounterClockwise = TRUE;   // glTF winding
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pso.InputLayout = { meshEls, 4 };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 3;
        pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pso.SampleDesc.Count = 1;
        if (FAILED(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoMesh))))
        { error = "mesh PSO failed"; return false; }

        // Shadow PSO: depth-only from the sun, BACKFACES only (cull front).
        // Closed casters store their far side, so shadows meet the occluder
        // exactly at the contact line (no peter-panning gap at wall bases)
        // and the bias stays tiny -- it pushes into the caster's interior
        // instead of detaching the shadow. The up-facing ground quad drops
        // out of the shadow map automatically (it only receives).
        D3D12_GRAPHICS_PIPELINE_STATE_DESC sh = pso;
        // depth + the STEALTH shadow clip (normal objects exit instantly)
        sh.PS = { psShadowClip->GetBufferPointer(),
                  psShadowClip->GetBufferSize() };
        sh.NumRenderTargets = 0;
        sh.RTVFormats[0] = sh.RTVFormats[1] = sh.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
        // no hardware bias: the receiver-side +z bias in the shader handles
        // it; deepening stored backfaces would re-open the contact strip
        sh.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        sh.RasterizerState.DepthBias = 0;
        sh.RasterizerState.SlopeScaledDepthBias = 0.0f;
        if (FAILED(m_device->CreateGraphicsPipelineState(&sh, IID_PPV_ARGS(&m_psoShadow))))
        { error = "shadow PSO failed"; return false; }

        // Skinned variants of the scene + shadow PSOs (blend-skinned VS).
        D3D12_INPUT_ELEMENT_DESC skinEls[] = {
            { "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT",      0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT,      0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 52, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC skinPso = pso;
        skinPso.VS = { vsMeshSkin->GetBufferPointer(), vsMeshSkin->GetBufferSize() };
        skinPso.InputLayout = { skinEls, 6 };
        if (FAILED(m_device->CreateGraphicsPipelineState(&skinPso, IID_PPV_ARGS(&m_psoMeshSkinned))))
        { error = "skinned mesh PSO failed"; return false; }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC skinSh = sh;
        skinSh.VS = { vsMeshSkin->GetBufferPointer(), vsMeshSkin->GetBufferSize() };
        skinSh.InputLayout = { skinEls, 6 };
        if (FAILED(m_device->CreateGraphicsPipelineState(&skinSh, IID_PPV_ARGS(&m_psoShadowSkinned))))
        { error = "skinned shadow PSO failed"; return false; }

        // UI PSO (renders to the backbuffer after composite).
        D3D12_GRAPHICS_PIPELINE_STATE_DESC ui = pso;
        ui.VS = { vsUi->GetBufferPointer(), vsUi->GetBufferSize() };
        ui.PS = { psUi->GetBufferPointer(), psUi->GetBufferSize() };
        ui.InputLayout = { uiEls, 2 };
        ui.DepthStencilState.DepthEnable = FALSE;
        ui.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        ui.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        ui.NumRenderTargets = 1;
        ui.RTVFormats[1] = ui.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
        ui.DSVFormat = DXGI_FORMAT_UNKNOWN;
        auto& rt = ui.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        if (FAILED(m_device->CreateGraphicsPipelineState(&ui, IID_PPV_ARGS(&m_psoUi))))
        { error = "ui PSO failed"; return false; }

        // UI burn dissolve PSO: same blend/state as UI, vertex-pulled (no layout).
        D3D12_GRAPHICS_PIPELINE_STATE_DESC burn = ui;
        burn.VS = { vsUiBurn->GetBufferPointer(), vsUiBurn->GetBufferSize() };
        burn.PS = { psUiBurn->GetBufferPointer(), psUiBurn->GetBufferSize() };
        burn.InputLayout = { nullptr, 0 };
        if (FAILED(m_device->CreateGraphicsPipelineState(&burn, IID_PPV_ARGS(&m_psoUiBurn))))
        { error = "ui burn PSO failed"; return false; }

        // Textured UI PSO (icon atlas quads).
        D3D12_INPUT_ELEMENT_DESC uiTexEls[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 8,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC uiTex = ui;
        uiTex.VS = { vsUiTex->GetBufferPointer(), vsUiTex->GetBufferSize() };
        uiTex.PS = { psUiTex->GetBufferPointer(), psUiTex->GetBufferSize() };
        uiTex.InputLayout = { uiTexEls, 3 };
        if (FAILED(m_device->CreateGraphicsPipelineState(&uiTex, IID_PPV_ARGS(&m_psoUiTex))))
        { error = "ui tex PSO failed"; return false; }

        // Post PSOs (fullscreen triangle, no depth, no input layout).
        auto makePost = [&](ID3DBlob* ps, DXGI_FORMAT fmt, ComPtr<ID3D12PipelineState>& out)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
            p.pRootSignature = m_rootSigPost.Get();
            p.VS = { vsFull->GetBufferPointer(), vsFull->GetBufferSize() };
            p.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            p.SampleMask = UINT_MAX;
            p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            p.RasterizerState.DepthClipEnable = TRUE;
            p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            p.NumRenderTargets = 1;
            p.RTVFormats[0] = fmt;
            p.SampleDesc.Count = 1;
            return SUCCEEDED(m_device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&out)));
        };
        if (!makePost(psSsao.Get(), DXGI_FORMAT_R8_UNORM, m_psoSsao))
        { error = "ssao PSO failed"; return false; }
        if (!makePost(psBlurH.Get(), DXGI_FORMAT_R8_UNORM, m_psoAoBlurH))
        { error = "ao blur H PSO failed"; return false; }
        if (!makePost(psBlurV.Get(), DXGI_FORMAT_R8_UNORM, m_psoAoBlurV))
        { error = "ao blur V PSO failed"; return false; }
        if (!makePost(psSsgi.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, m_psoSsgi))
        { error = "ssgi PSO failed"; return false; }
        if (!makePost(psTemporal.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, m_psoTemporal))
        { error = "temporal PSO failed"; return false; }
        if (!makePost(psComposite.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, m_psoComposite))
        { error = "composite PSO failed"; return false; }
        if (!makePost(psAA.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, m_psoAA))
        { error = "aa PSO failed"; return false; }

        // Scorch decal PSO: multiply-blends into scene color + albedo.
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
            p.pRootSignature = m_rootSigPost.Get();
            p.VS = { vsVfxFull->GetBufferPointer(), vsVfxFull->GetBufferSize() };
            p.PS = { psScorch->GetBufferPointer(), psScorch->GetBufferSize() };
            for (int i = 0; i < 2; ++i)
            {
                auto& rt = p.BlendState.RenderTarget[i];
                rt.BlendEnable = TRUE;
                rt.SrcBlend = D3D12_BLEND_DEST_COLOR;   // multiply
                rt.DestBlend = D3D12_BLEND_ZERO;
                rt.BlendOp = D3D12_BLEND_OP_ADD;
                rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                rt.DestBlendAlpha = D3D12_BLEND_ZERO;
                rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            }
            p.SampleMask = UINT_MAX;
            p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            p.RasterizerState.DepthClipEnable = TRUE;
            p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            p.NumRenderTargets = 2;
            p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            p.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
            p.SampleDesc.Count = 1;
            if (FAILED(m_device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&m_psoScorch))))
            { error = "scorch PSO failed"; return false; }
        }

        // Smoke billboard PSO: alpha blend, depth-tested read-only.
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
            p.pRootSignature = m_rootSigPost.Get();
            p.VS = { vsVfx->GetBufferPointer(), vsVfx->GetBufferSize() };
            p.PS = { psVfx->GetBufferPointer(), psVfx->GetBufferSize() };
            auto& rt = p.BlendState.RenderTarget[0];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            p.SampleMask = UINT_MAX;
            p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            p.RasterizerState.DepthClipEnable = TRUE;
            p.DepthStencilState.DepthEnable = TRUE;
            p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            p.NumRenderTargets = 1;
            p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            p.DSVFormat = DXGI_FORMAT_D32_FLOAT;
            p.SampleDesc.Count = 1;
            if (FAILED(m_device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&m_psoVfx))))
            { error = "vfx PSO failed"; return false; }
        }
        return true;
    }

    bool BuildRootSig(const D3D12_ROOT_SIGNATURE_DESC& rsd,
                      ComPtr<ID3D12RootSignature>& out, std::string& error)
    {
        ComPtr<ID3DBlob> sig, errs;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &errs)))
        {
            error = errs ? static_cast<const char*>(errs->GetBufferPointer())
                         : "root signature serialize failed";
            return false;
        }
        if (FAILED(m_device->CreateRootSignature(0, sig->GetBufferPointer(),
                sig->GetBufferSize(), IID_PPV_ARGS(&out))))
        { error = "CreateRootSignature failed"; return false; }
        return true;
    }

    ComPtr<ID3D12Resource> CreateStaticBuffer(const void* data, UINT64 size)
    {
        ComPtr<ID3D12Resource> buf, upload;
        auto hpDef = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto hpUp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = BufferDesc(size);
        if (FAILED(m_device->CreateCommittedResource(&hpDef, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buf))))
            return nullptr;
        if (FAILED(m_device->CreateCommittedResource(&hpUp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
            return nullptr;
        void* mapped = nullptr;
        upload->Map(0, nullptr, &mapped);
        memcpy(mapped, data, size);
        upload->Unmap(0, nullptr);

        m_alloc[0]->Reset();
        m_cmd->Reset(m_alloc[0].Get(), nullptr);
        m_cmd->CopyBufferRegion(buf.Get(), 0, upload.Get(), 0, size);
        BarrierRaw(buf.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
                   | D3D12_RESOURCE_STATE_INDEX_BUFFER);
        m_cmd->Close();
        ID3D12CommandList* lists[] = { m_cmd.Get() };
        m_queue->ExecuteCommandLists(1, lists);
        WaitForGpuIdle();
        return buf;
    }

    void WaitForGpuIdle()
    {
        UINT64 v = ++m_nextFence;
        m_queue->Signal(m_fence.Get(), v);
        if (m_fence->GetCompletedValue() < v)
        {
            m_fence->SetEventOnCompletion(v, m_fenceEvent);
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    HWND m_hwnd{};
    int m_width = 0, m_height = 0;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap, m_dsvHeap, m_srvHeap;
    UINT m_rtvSize = 0, m_srvSize = 0;
    ComPtr<ID3D12Resource> m_backbuffers[FrameCount];
    ComPtr<ID3D12Resource> m_depth;
    D3D12_RESOURCE_STATES m_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    ComPtr<ID3D12Resource> m_shadowMap;
    D3D12_RESOURCE_STATES m_shadowState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    UINT m_dsvSize = 0;
    Target m_sceneColor, m_sceneNormal, m_sceneAlbedo, m_ao, m_aoTmp, m_final,
           m_gi, m_accum[2];
    int m_accumIndex = 0;
    bool m_giHalf = true;
    int m_giW = 1, m_giH = 1;
    bool m_historyValid = false;
    XMFLOAT4X4 m_prevViewProj{};
    XMFLOAT3 m_prevCamPos{};
    ComPtr<ID3D12CommandAllocator> m_alloc[FramesInFlight];
    ComPtr<ID3D12GraphicsCommandList> m_cmd;
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent{};
    UINT64 m_nextFence = 0;
    UINT64 m_fenceValues[FramesInFlight]{};
    UINT64 m_frameIndex = 0;
    ComPtr<ID3D12RootSignature> m_rootSig, m_rootSigPost;
    ComPtr<ID3D12PipelineState> m_psoMesh, m_psoMeshSkinned, m_psoUi, m_psoShadow,
                                m_psoShadowSkinned, m_psoSsao, m_psoAoBlurH,
                                m_psoAoBlurV, m_psoSsgi, m_psoTemporal, m_psoComposite,
                                m_psoScorch, m_psoVfx, m_psoAA, m_psoUiBurn, m_psoUiTex;
    int m_shadowSize = 2048;
    ComPtr<ID3D12Resource> m_cbUpload[FramesInFlight];
    uint8_t* m_cbMapped[FramesInFlight]{};
    ComPtr<ID3D12Resource> m_uiVb[FramesInFlight];
    uint8_t* m_uiMapped[FramesInFlight]{};
    ComPtr<ID3D12Resource> m_uiTexVb[FramesInFlight];
    uint8_t* m_uiTexMapped[FramesInFlight]{};
    std::vector<GpuMesh> m_meshes;
    std::vector<ComPtr<ID3D12Resource>> m_textures;
    int m_textureCount = 0;
};

} // namespace

IRenderer* CreateRendererD3D12() { return new RendererD3D12(); }

} // namespace tankaq
