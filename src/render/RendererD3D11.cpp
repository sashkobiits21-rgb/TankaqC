#include "render/IRenderer.h"
#include "AssetLoad.h"
#include "Log.h"
#include <d3d11.h>
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

struct GpuMesh
{
    ComPtr<ID3D11Buffer> vb;
    ComPtr<ID3D11Buffer> ib;
    UINT indexCount = 0;
    UINT stride = sizeof(Vertex);
    bool skinned = false;
};

struct RenderTexture
{
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;

    bool Create(ID3D11Device* dev, int w, int h, DXGI_FORMAT fmt)
    {
        tex.Reset(); rtv.Reset(); srv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) return false;
        if (FAILED(dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) return false;
        if (FAILED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return false;
        return true;
    }
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

class RendererD3D11 final : public IRenderer
{
public:
    ~RendererD3D11() override
    {
        // Release the swapchain cleanly so another renderer can own the HWND.
        if (m_ctx)
        {
            m_ctx->ClearState();
            m_ctx->Flush();
        }
    }

    bool Init(void* hwnd, int width, int height, std::string& error) override
    {
        m_hwnd = static_cast<HWND>(hwnd);
        m_width = width;
        m_height = height;

        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                       levels, 1, D3D11_SDK_VERSION, &m_device, nullptr,
                                       &m_ctx);
        if (FAILED(hr))
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   levels, 1, D3D11_SDK_VERSION, &m_device, nullptr, &m_ctx);
        if (FAILED(hr)) { error = "D3D11CreateDevice failed"; return false; }

        ComPtr<IDXGIDevice> dxgiDevice;
        m_device.As(&dxgiDevice);
        ComPtr<IDXGIAdapter> adapter;
        dxgiDevice->GetAdapter(&adapter);
        ComPtr<IDXGIFactory2> factory;
        adapter->GetParent(IID_PPV_ARGS(&factory));

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width;
        sd.Height = height;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_NONE;   // size mismatch = cut, never stretch
        hr = factory->CreateSwapChainForHwnd(m_device.Get(), m_hwnd, &sd, nullptr,
                                             nullptr, &m_swapChain);
        if (FAILED(hr)) { error = "CreateSwapChainForHwnd failed"; return false; }
        factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);

        if (!CreateSizedResources(error) || !CreatePipeline(error))
            return false;
        Log("D3D11: initialized %dx%d", width, height);
        return true;
    }

    void Resize(int width, int height) override
    {
        if (!m_swapChain || width <= 0 || height <= 0)
            return;
        m_width = width;
        m_height = height;
        m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        m_backRtv.Reset();
        m_dsv.Reset();
        m_depthSrv.Reset();
        m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        std::string err;
        CreateSizedResources(err);
        m_historyValid = false;
    }

    int CreateMesh(const Vertex* verts, size_t vertexCount,
                   const uint32_t* indices, size_t indexCount) override
    {
        GpuMesh mesh;
        D3D11_BUFFER_DESC vbd{};
        vbd.ByteWidth = UINT(vertexCount * sizeof(Vertex));
        vbd.Usage = D3D11_USAGE_IMMUTABLE;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vsd{ verts };
        if (FAILED(m_device->CreateBuffer(&vbd, &vsd, &mesh.vb)))
            return -1;

        D3D11_BUFFER_DESC ibd{};
        ibd.ByteWidth = UINT(indexCount * sizeof(uint32_t));
        ibd.Usage = D3D11_USAGE_IMMUTABLE;
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA isd{ indices };
        if (FAILED(m_device->CreateBuffer(&ibd, &isd, &mesh.ib)))
            return -1;
        mesh.indexCount = UINT(indexCount);
        m_meshes.push_back(std::move(mesh));
        return int(m_meshes.size()) - 1;
    }

    int CreateSkinnedMesh(const SkinnedVertex* verts, size_t vertexCount,
                          const uint32_t* indices, size_t indexCount) override
    {
        GpuMesh mesh;
        mesh.stride = sizeof(SkinnedVertex);
        mesh.skinned = true;
        D3D11_BUFFER_DESC vbd{};
        vbd.ByteWidth = UINT(vertexCount * sizeof(SkinnedVertex));
        vbd.Usage = D3D11_USAGE_IMMUTABLE;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vsd{ verts };
        if (FAILED(m_device->CreateBuffer(&vbd, &vsd, &mesh.vb)))
            return -1;
        D3D11_BUFFER_DESC ibd{};
        ibd.ByteWidth = UINT(indexCount * sizeof(uint32_t));
        ibd.Usage = D3D11_USAGE_IMMUTABLE;
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA isd{ indices };
        if (FAILED(m_device->CreateBuffer(&ibd, &isd, &mesh.ib)))
            return -1;
        mesh.indexCount = UINT(indexCount);
        m_meshes.push_back(std::move(mesh));
        return int(m_meshes.size()) - 1;
    }

    // Select the static or skinned vertex path for a draw; skinned objects
    // also get their bone palette uploaded to b2.
    void BindMeshVs(const GpuMesh& mesh, const RenderObject& obj,
                    const FrameData& frame)
    {
        if (mesh.skinned)
        {
            m_ctx->IASetInputLayout(m_skinLayout.Get());
            m_ctx->VSSetShader(m_vsMeshSkinned.Get(), nullptr, 0);
            if (obj.paletteIndex >= 0
                && obj.paletteIndex < int(frame.palettes.size()))
            {
                const BonePalette& p = frame.palettes[obj.paletteIndex];
                UpdateCB(m_cbBones.Get(), p.m, sizeof(p.m));
            }
            m_ctx->VSSetConstantBuffers(2, 1, m_cbBones.GetAddressOf());
        }
        else
        {
            m_ctx->IASetInputLayout(m_meshLayout.Get());
            m_ctx->VSSetShader(m_vsMesh.Get(), nullptr, 0);
        }
    }

    int CreateTexture(const uint8_t* rgba, int width, int height) override
    {
        ImageData src;
        src.width = width;
        src.height = height;
        src.rgba.assign(rgba, rgba + size_t(width) * height * 4);
        auto chain = BuildMipChain(src);

        std::vector<D3D11_SUBRESOURCE_DATA> initData(chain.size());
        for (size_t i = 0; i < chain.size(); ++i)
        {
            initData[i].pSysMem = chain[i].rgba.data();
            initData[i].SysMemPitch = UINT(chain[i].width * 4);
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = UINT(chain.size());
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(m_device->CreateTexture2D(&td, initData.data(), &tex)))
            return -1;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(m_device->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
            return -1;
        m_textures.push_back(srv);
        return int(m_textures.size()) - 1;
    }

    void RenderFrame(const FrameData& frame) override
    {
        if (frame.post.shadowMapSize != m_shadowSize)
        {
            m_shadowSize = frame.post.shadowMapSize;
            std::string err;
            CreateShadowMap(err);
        }

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

        // ---------------- shadow pass (depth only, sun's ortho camera) ----------------
        if (frame.post.shadowsEnabled)
        {
            ID3D11ShaderResourceView* nullSrv = nullptr;
            m_ctx->PSSetShaderResources(1, 1, &nullSrv);   // release shadow SRV
            m_ctx->ClearDepthStencilView(m_shadowDsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
            m_ctx->OMSetRenderTargets(0, nullptr, m_shadowDsv.Get());
            D3D11_VIEWPORT svp{ 0, 0, float(m_shadowSize), float(m_shadowSize), 0, 1 };
            m_ctx->RSSetViewports(1, &svp);
            m_ctx->RSSetState(m_rasterShadow.Get());
            m_ctx->OMSetDepthStencilState(m_depthOn.Get(), 0);
            m_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);

            PerFrameCB sf = pf;
            sf.viewProj = frame.lightViewProj;   // render from the sun
            UpdateCB(m_cbShadowFrame.Get(), &sf, sizeof(sf));
            m_ctx->VSSetConstantBuffers(0, 1, m_cbShadowFrame.GetAddressOf());
            m_ctx->VSSetConstantBuffers(1, 1, m_cbObject.GetAddressOf());
            m_ctx->IASetInputLayout(m_meshLayout.Get());
            m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_ctx->VSSetShader(m_vsMesh.Get(), nullptr, 0);
            m_ctx->PSSetShader(nullptr, nullptr, 0);       // depth only

            for (const RenderObject& obj : frame.objects)
            {
                if (obj.mesh < 0 || obj.mesh >= int(m_meshes.size()))
                    continue;
                PerObjectCB po{};
                po.world = obj.world;
                po.tint = obj.tint;
                UpdateCB(m_cbObject.Get(), &po, sizeof(po));
                const GpuMesh& mesh = m_meshes[obj.mesh];
                BindMeshVs(mesh, obj, frame);   // static or skinned VS/layout
                UINT stride = mesh.stride, offset = 0;
                m_ctx->IASetVertexBuffers(0, 1, mesh.vb.GetAddressOf(), &stride, &offset);
                m_ctx->IASetIndexBuffer(mesh.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
                m_ctx->DrawIndexed(mesh.indexCount, 0, 0);
            }
            m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        }

        // ---------------- scene pass (G-buffer MRT) ----------------
        float clearColor[4] = { 0.62f, 0.72f, 0.83f, 1.0f };
        float clearNormal[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
        float clearBlack[4] = { 0, 0, 0, 0 };
        m_ctx->ClearRenderTargetView(m_sceneColor.rtv.Get(), clearColor);
        m_ctx->ClearRenderTargetView(m_sceneNormal.rtv.Get(), clearNormal);
        m_ctx->ClearRenderTargetView(m_sceneAlbedo.rtv.Get(), clearBlack);
        m_ctx->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

        ID3D11RenderTargetView* mrt[3] = { m_sceneColor.rtv.Get(),
                                           m_sceneNormal.rtv.Get(),
                                           m_sceneAlbedo.rtv.Get() };
        m_ctx->OMSetRenderTargets(3, mrt, m_dsv.Get());

        D3D11_VIEWPORT vp{ 0, 0, float(m_width), float(m_height), 0, 1 };
        m_ctx->RSSetViewports(1, &vp);
        m_ctx->RSSetState(m_raster.Get());
        m_ctx->OMSetDepthStencilState(m_depthOn.Get(), 0);
        m_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);

        UpdateCB(m_cbFrame.Get(), &pf, sizeof(pf));
        m_ctx->VSSetConstantBuffers(0, 1, m_cbFrame.GetAddressOf());
        m_ctx->PSSetConstantBuffers(0, 1, m_cbFrame.GetAddressOf());
        m_ctx->VSSetConstantBuffers(1, 1, m_cbObject.GetAddressOf());
        m_ctx->PSSetConstantBuffers(1, 1, m_cbObject.GetAddressOf());

        m_ctx->IASetInputLayout(m_meshLayout.Get());
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->VSSetShader(m_vsMesh.Get(), nullptr, 0);
        m_ctx->PSSetShader(m_psMesh.Get(), nullptr, 0);
        ID3D11SamplerState* meshSamplers[2] = { m_samplerAniso.Get(), m_samplerShadow.Get() };
        m_ctx->PSSetSamplers(0, 2, meshSamplers);
        m_ctx->PSSetShaderResources(1, 1, m_shadowSrv.GetAddressOf());

        for (const RenderObject& obj : frame.objects)
        {
            if (obj.mesh < 0 || obj.mesh >= int(m_meshes.size()))
                continue;
            PerObjectCB po{};
            po.world = obj.world;
            po.tint = obj.tint;
            po.misc = XMFLOAT4(obj.isDynamic ? 1.0f : 0.0f,
                               obj.deformDist, obj.deformAge,
                               obj.deformDist >= 0.0f ? 1.0f : 0.0f);
            po.misc2 = XMFLOAT4(obj.losClip, 0.0f, 0.0f, 0.0f);
            UpdateCB(m_cbObject.Get(), &po, sizeof(po));

            const GpuMesh& mesh = m_meshes[obj.mesh];
            BindMeshVs(mesh, obj, frame);       // static or skinned VS/layout
            UINT stride = mesh.stride, offset = 0;
            m_ctx->IASetVertexBuffers(0, 1, mesh.vb.GetAddressOf(), &stride, &offset);
            m_ctx->IASetIndexBuffer(mesh.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
            ID3D11ShaderResourceView* srv =
                (obj.texture >= 0 && obj.texture < int(m_textures.size()))
                    ? m_textures[obj.texture].Get() : nullptr;
            m_ctx->PSSetShaderResources(0, 1, &srv);
            int nraIdx = obj.texNormal >= 0 ? obj.texNormal
                                            : frame.defaultNormalTex;
            ID3D11ShaderResourceView* nra =
                (nraIdx >= 0 && nraIdx < int(m_textures.size()))
                    ? m_textures[nraIdx].Get() : nullptr;
            m_ctx->PSSetShaderResources(2, 1, &nra);
            m_ctx->DrawIndexed(mesh.indexCount, 0, 0);
        }

        // Release G-buffer + depth for shader reads.
        m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        UnbindSrvs();

        // ---------------- VFX: scorch decals + smoke billboards ----------------
        if (!frame.bursts.empty() || !frame.scorches.empty())
        {
            VfxCB vc{};
            vc.viewProj = frame.viewProj;
            vc.invViewProj = frame.invViewProj;
            vc.camRight = XMFLOAT4(frame.camRight.x, frame.camRight.y, frame.camRight.z, 0);
            vc.camUp = XMFLOAT4(frame.camUp.x, frame.camUp.y, frame.camUp.z, 0);
            vc.camPos = XMFLOAT4(frame.camPos.x, frame.camPos.y, frame.camPos.z, 0);
            vc.screenTime = XMFLOAT4(float(m_width), float(m_height), frame.time, 0);
            size_t nb = std::min<size_t>(frame.bursts.size(), 16);
            size_t ns = std::min<size_t>(frame.scorches.size(), 16);
            vc.counts = XMFLOAT4(float(nb), float(ns), 0, 0);
            for (size_t i = 0; i < nb; ++i)
                vc.bursts[i] = XMFLOAT4(frame.bursts[i].pos.x, frame.bursts[i].pos.y,
                                        frame.bursts[i].pos.z, frame.bursts[i].age);
            for (size_t i = 0; i < ns; ++i)
                vc.scorches[i] = XMFLOAT4(frame.scorches[i].pos.x, frame.scorches[i].pos.y,
                                          frame.scorches[i].pos.z, frame.scorches[i].age);
            UpdateCB(m_cbVfx.Get(), &vc, sizeof(vc));

            ID3D11ShaderResourceView* vfxSrvs[2] = { m_depthSrv.Get(), m_sceneNormal.srv.Get() };
            ID3D11SamplerState* pointSampler = m_samplerPoint.Get();
            m_ctx->RSSetState(m_rasterUi.Get());
            m_ctx->VSSetConstantBuffers(0, 1, m_cbVfx.GetAddressOf());
            m_ctx->PSSetConstantBuffers(0, 1, m_cbVfx.GetAddressOf());
            m_ctx->PSSetSamplers(0, 1, &pointSampler);
            m_ctx->PSSetShaderResources(0, 2, vfxSrvs);

            if (ns > 0)
            {
                // burn marks multiply into lit color AND albedo (GI sees them)
                ID3D11RenderTargetView* rts[2] = { m_sceneColor.rtv.Get(),
                                                   m_sceneAlbedo.rtv.Get() };
                m_ctx->OMSetRenderTargets(2, rts, nullptr);
                m_ctx->OMSetDepthStencilState(m_depthOff.Get(), 0);
                float bf[4]{};
                m_ctx->OMSetBlendState(m_blendMultiply.Get(), bf, 0xffffffff);
                m_ctx->IASetInputLayout(nullptr);
                m_ctx->VSSetShader(m_vsVfxFull.Get(), nullptr, 0);
                m_ctx->PSSetShader(m_psScorch.Get(), nullptr, 0);
                m_ctx->Draw(3, 0);
            }
            if (nb > 0)
            {
                // smoke: depth-tested read-only so walls occlude it, VS collides
                // particles against the G-buffer, PS soft-fades near surfaces
                m_ctx->OMSetRenderTargets(1, m_sceneColor.rtv.GetAddressOf(),
                                          m_dsvReadOnly.Get());
                m_ctx->OMSetDepthStencilState(m_depthRead.Get(), 0);
                float bf[4]{};
                m_ctx->OMSetBlendState(m_blendAlpha.Get(), bf, 0xffffffff);
                m_ctx->IASetInputLayout(nullptr);
                m_ctx->VSSetShader(m_vsVfx.Get(), nullptr, 0);
                m_ctx->PSSetShader(m_psVfx.Get(), nullptr, 0);
                m_ctx->VSSetShaderResources(0, 2, vfxSrvs);
                m_ctx->VSSetSamplers(0, 1, &pointSampler);
                m_ctx->DrawInstanced(6, UINT(nb) * VfxParticlesPerBurst, 0, 0);
                ID3D11ShaderResourceView* vsNulls[2] = {};
                m_ctx->VSSetShaderResources(0, 2, vsNulls);
            }
            m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
            m_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            UnbindSrvs();
        }

        // ---------------- post constants ----------------
        if (frame.post.giHalfRes != m_giHalf)
        {
            m_giHalf = frame.post.giHalfRes;
            std::string err;
            CreateGiTargets(err);
            m_historyValid = false;
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
        UpdateCB(m_cbPost.Get(), &pc, sizeof(pc));

        m_ctx->IASetInputLayout(nullptr);
        m_ctx->VSSetShader(m_vsFull.Get(), nullptr, 0);
        m_ctx->PSSetConstantBuffers(0, 1, m_cbPost.GetAddressOf());
        ID3D11SamplerState* postSamplers[2] = { m_samplerPoint.Get(), m_samplerLinear.Get() };
        m_ctx->PSSetSamplers(0, 2, postSamplers);
        m_ctx->RSSetState(m_rasterUi.Get());
        m_ctx->OMSetDepthStencilState(m_depthOff.Get(), 0);

        // ---------------- SSAO + blur ----------------
        if (frame.post.aoEnabled)
        {
            ID3D11ShaderResourceView* srvs[2] = { m_sceneNormal.srv.Get(), m_depthSrv.Get() };
            RunPass(m_psSsao.Get(), m_ao.rtv.Get(), srvs, 2);
            ID3D11ShaderResourceView* h[2] = { m_ao.srv.Get(), m_depthSrv.Get() };
            RunPass(m_psAoBlurH.Get(), m_aoTmp.rtv.Get(), h, 2);
            ID3D11ShaderResourceView* v[2] = { m_aoTmp.srv.Get(), m_depthSrv.Get() };
            RunPass(m_psAoBlurV.Get(), m_ao.rtv.Get(), v, 2);
        }

        // ---------------- SSGI + temporal (at GI resolution) ----------------
        int written = m_accumIndex;
        if (frame.post.giEnabled)
        {
            D3D11_VIEWPORT giVp{ 0, 0, float(m_giW), float(m_giH), 0, 1 };
            m_ctx->RSSetViewports(1, &giVp);

            ID3D11ShaderResourceView* giSrvs[3] = { m_sceneColor.srv.Get(),
                                                    m_sceneNormal.srv.Get(),
                                                    m_depthSrv.Get() };
            RunPass(m_psSsgi.Get(), m_gi.rtv.Get(), giSrvs, 3);

            int prev = 1 - m_accumIndex;
            ID3D11ShaderResourceView* tSrvs[3] = { m_gi.srv.Get(), m_depthSrv.Get(),
                                                   m_accum[prev].srv.Get() };
            RunPass(m_psTemporal.Get(), m_accum[m_accumIndex].rtv.Get(), tSrvs, 3);
            written = m_accumIndex;
            m_accumIndex = prev;

            m_ctx->RSSetViewports(1, &vp);
        }

        // ---------------- composite (+ optional edge AA) ----------------
        {
            ID3D11ShaderResourceView* cSrvs[5] = { m_sceneColor.srv.Get(),
                                                   m_accum[written].srv.Get(),
                                                   m_ao.srv.Get(),
                                                   m_sceneAlbedo.srv.Get(),
                                                   m_depthSrv.Get() };
            if (frame.post.aaEnabled)
            {
                RunPass(m_psComposite.Get(), m_final.rtv.Get(), cSrvs, 5);
                ID3D11ShaderResourceView* aaSrv[1] = { m_final.srv.Get() };
                RunPass(m_psAA.Get(), m_backRtv.Get(), aaSrv, 1);
            }
            else
            {
                RunPass(m_psComposite.Get(), m_backRtv.Get(), cSrvs, 5);
            }
        }

        // ---------------- UI ----------------
        if (!frame.ui.empty())
        {
            size_t bytes = frame.ui.size() * sizeof(UiVertex);
            if (bytes <= UiVbBytes)
            {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(m_ctx->Map(m_uiVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                {
                    memcpy(mapped.pData, frame.ui.data(), bytes);
                    m_ctx->Unmap(m_uiVb.Get(), 0);
                    m_ctx->OMSetRenderTargets(1, m_backRtv.GetAddressOf(), nullptr);
                    m_ctx->IASetInputLayout(m_uiLayout.Get());
                    m_ctx->VSSetShader(m_vsUi.Get(), nullptr, 0);
                    m_ctx->PSSetShader(m_psUi.Get(), nullptr, 0);
                    m_ctx->VSSetConstantBuffers(0, 1, m_cbFrame.GetAddressOf());
                    float bf[4]{};
                    m_ctx->OMSetBlendState(m_blendAlpha.Get(), bf, 0xffffffff);
                    UINT stride = sizeof(UiVertex), offset = 0;
                    m_ctx->IASetVertexBuffers(0, 1, m_uiVb.GetAddressOf(), &stride, &offset);
                    m_ctx->Draw(UINT(frame.ui.size()), 0);

                    ID3D11ShaderResourceView* atlasSrv =
                        (frame.uiTexTexture >= 0
                         && frame.uiTexTexture < int(m_textures.size()))
                            ? m_textures[frame.uiTexTexture].Get() : nullptr;

                    // textured UI (icon atlas quads)
                    size_t texBytes = frame.uiTex.size() * sizeof(UiTexVertex);
                    if (!frame.uiTex.empty() && texBytes <= UiVbBytes && atlasSrv)
                    {
                        D3D11_MAPPED_SUBRESOURCE tmap{};
                        if (SUCCEEDED(m_ctx->Map(m_uiTexVb.Get(), 0,
                                                 D3D11_MAP_WRITE_DISCARD, 0, &tmap)))
                        {
                            memcpy(tmap.pData, frame.uiTex.data(), texBytes);
                            m_ctx->Unmap(m_uiTexVb.Get(), 0);
                            m_ctx->IASetInputLayout(m_uiTexLayout.Get());
                            m_ctx->VSSetShader(m_vsUiTex.Get(), nullptr, 0);
                            m_ctx->PSSetShader(m_psUiTex.Get(), nullptr, 0);
                            ID3D11SamplerState* lin = m_samplerLinear.Get();
                            m_ctx->PSSetSamplers(0, 1, &lin);
                            m_ctx->PSSetShaderResources(0, 1, &atlasSrv);
                            UINT ts = sizeof(UiTexVertex), to = 0;
                            m_ctx->IASetVertexBuffers(0, 1, m_uiTexVb.GetAddressOf(),
                                                      &ts, &to);
                            m_ctx->Draw(UINT(frame.uiTex.size()), 0);
                        }
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
                            bc.param[q] = XMFLOAT4(b.originX, b.originY,
                                                   b.progress, b.maxRadius);
                            bc.uv[q] = XMFLOAT4(b.u0, b.v0, b.u1, b.v1);
                        }
                        bc.misc = XMFLOAT4(float(burnCount), frame.time, 5.0f, 0);
                        UpdateCB(m_cbUiBurn.Get(), &bc, sizeof(bc));
                        m_ctx->IASetInputLayout(nullptr);
                        m_ctx->VSSetShader(m_vsUiBurn.Get(), nullptr, 0);
                        m_ctx->PSSetShader(m_psUiBurn.Get(), nullptr, 0);
                        m_ctx->VSSetConstantBuffers(1, 1, m_cbUiBurn.GetAddressOf());
                        m_ctx->PSSetConstantBuffers(1, 1, m_cbUiBurn.GetAddressOf());
                        ID3D11SamplerState* lin = m_samplerLinear.Get();
                        m_ctx->PSSetSamplers(0, 1, &lin);
                        if (atlasSrv)
                            m_ctx->PSSetShaderResources(0, 1, &atlasSrv);
                        m_ctx->DrawInstanced(6, UINT(burnCount), 0, 0);
                    }
                    m_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
                }
            }
        }

        m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        m_swapChain->Present(frame.vsync ? 1 : 0, 0);

        m_prevViewProj = frame.viewProj;
        m_prevCamPos = frame.camPos;
        m_historyValid = true;
        ++m_frameIndex;
    }

    bool SaveBackbufferPNG(const std::string& path) override
    {
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&back))))
            return false;
        D3D11_TEXTURE2D_DESC desc{};
        back->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &staging)))
            return false;
        m_ctx->CopyResource(staging.Get(), back.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return false;
        std::vector<uint8_t> pixels(size_t(desc.Width) * desc.Height * 4);
        for (UINT y = 0; y < desc.Height; ++y)
            memcpy(pixels.data() + size_t(y) * desc.Width * 4,
                   static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                   size_t(desc.Width) * 4);
        m_ctx->Unmap(staging.Get(), 0);
        int ok = stbi_write_png(path.c_str(), int(desc.Width), int(desc.Height), 4,
                                pixels.data(), int(desc.Width) * 4);
        Log("D3D11: screenshot %s -> %s", path.c_str(), ok ? "ok" : "FAILED");
        return ok != 0;
    }

    const char* Name() const override { return "D3D11"; }

private:
    void RunPass(ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv,
                 ID3D11ShaderResourceView** srvs, int srvCount)
    {
        m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        m_ctx->PSSetShader(ps, nullptr, 0);
        m_ctx->PSSetShaderResources(0, UINT(srvCount), srvs);
        m_ctx->Draw(3, 0);
        m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        UnbindSrvs();
    }

    void UnbindSrvs()
    {
        ID3D11ShaderResourceView* nulls[5] = {};
        m_ctx->PSSetShaderResources(0, 5, nulls);
    }

    bool CreateShadowMap(std::string& error)
    {
        m_shadowDsv.Reset();
        m_shadowSrv.Reset();
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = m_shadowSize;
        sd.Height = m_shadowSize;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_DEFAULT;
        sd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> shadowTex;
        if (FAILED(m_device->CreateTexture2D(&sd, nullptr, &shadowTex)))
        { error = "shadow map failed"; return false; }
        D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
        dvd.Format = DXGI_FORMAT_D32_FLOAT;
        dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(shadowTex.Get(), &dvd, &m_shadowDsv);
        D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format = DXGI_FORMAT_R32_FLOAT;
        svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        svd.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(shadowTex.Get(), &svd, &m_shadowSrv);
        Log("D3D11: shadow map %dx%d", m_shadowSize, m_shadowSize);
        return true;
    }

    bool CreateGiTargets(std::string& error)
    {
        m_giW = m_giHalf ? std::max(1, m_width / 2) : m_width;
        m_giH = m_giHalf ? std::max(1, m_height / 2) : m_height;
        bool ok = m_gi.Create(m_device.Get(), m_giW, m_giH, DXGI_FORMAT_R16G16B16A16_FLOAT)
               && m_accum[0].Create(m_device.Get(), m_giW, m_giH, DXGI_FORMAT_R16G16B16A16_FLOAT)
               && m_accum[1].Create(m_device.Get(), m_giW, m_giH, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (!ok) { error = "gi target creation failed"; return false; }
        float black[4] = { 0, 0, 0, 0 };
        m_ctx->ClearRenderTargetView(m_accum[0].rtv.Get(), black);
        m_ctx->ClearRenderTargetView(m_accum[1].rtv.Get(), black);
        return true;
    }

    bool CreateSizedResources(std::string& error)
    {
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&back))))
        { error = "GetBuffer failed"; return false; }
        if (FAILED(m_device->CreateRenderTargetView(back.Get(), nullptr, &m_backRtv)))
        { error = "CreateRenderTargetView failed"; return false; }

        // Depth with SRV access.
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = m_width;
        dd.Height = m_height;
        dd.MipLevels = 1;
        dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_R32_TYPELESS;
        dd.SampleDesc.Count = 1;
        dd.Usage = D3D11_USAGE_DEFAULT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> depth;
        if (FAILED(m_device->CreateTexture2D(&dd, nullptr, &depth)))
        { error = "depth texture failed"; return false; }
        D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
        dvd.Format = DXGI_FORMAT_D32_FLOAT;
        dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(m_device->CreateDepthStencilView(depth.Get(), &dvd, &m_dsv)))
        { error = "CreateDepthStencilView failed"; return false; }
        dvd.Flags = D3D11_DSV_READ_ONLY_DEPTH;   // depth-tested VFX while depth is an SRV
        if (FAILED(m_device->CreateDepthStencilView(depth.Get(), &dvd, &m_dsvReadOnly)))
        { error = "read-only DSV failed"; return false; }
        D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format = DXGI_FORMAT_R32_FLOAT;
        svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        svd.Texture2D.MipLevels = 1;
        if (FAILED(m_device->CreateShaderResourceView(depth.Get(), &svd, &m_depthSrv)))
        { error = "depth SRV failed"; return false; }

        bool ok = m_sceneColor.Create(m_device.Get(), m_width, m_height, DXGI_FORMAT_R8G8B8A8_UNORM)
               && m_sceneNormal.Create(m_device.Get(), m_width, m_height, DXGI_FORMAT_R8G8B8A8_UNORM)
               && m_sceneAlbedo.Create(m_device.Get(), m_width, m_height, DXGI_FORMAT_R8G8B8A8_UNORM)
               && m_ao.Create(m_device.Get(), m_width, m_height, DXGI_FORMAT_R8_UNORM)
               && m_aoTmp.Create(m_device.Get(), m_width, m_height, DXGI_FORMAT_R8_UNORM)
               && m_final.Create(m_device.Get(), m_width, m_height, DXGI_FORMAT_R8G8B8A8_UNORM);
        if (!ok) { error = "offscreen target creation failed"; return false; }
        return CreateGiTargets(error);
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
        ComPtr<ID3DBlob> psMesh = Compile(src, "Basic.hlsl", "PSMesh", "ps_5_0", error);
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
        if (!vsMesh || !vsMeshSkin || !psMesh || !vsUi || !psUi || !vsFull || !psSsao || !psBlurH
            || !psBlurV || !psSsgi || !psTemporal || !psComposite
            || !psAA || !vsUiBurn || !psUiBurn || !vsUiTex || !psUiTex
            || !vsVfx || !psVfx || !vsVfxFull || !psScorch)
            return false;

        m_device->CreateVertexShader(vsMesh->GetBufferPointer(), vsMesh->GetBufferSize(), nullptr, &m_vsMesh);
        m_device->CreateVertexShader(vsMeshSkin->GetBufferPointer(), vsMeshSkin->GetBufferSize(), nullptr, &m_vsMeshSkinned);
        m_device->CreatePixelShader(psMesh->GetBufferPointer(), psMesh->GetBufferSize(), nullptr, &m_psMesh);
        m_device->CreateVertexShader(vsUi->GetBufferPointer(), vsUi->GetBufferSize(), nullptr, &m_vsUi);
        m_device->CreatePixelShader(psUi->GetBufferPointer(), psUi->GetBufferSize(), nullptr, &m_psUi);
        m_device->CreateVertexShader(vsFull->GetBufferPointer(), vsFull->GetBufferSize(), nullptr, &m_vsFull);
        m_device->CreatePixelShader(psSsao->GetBufferPointer(), psSsao->GetBufferSize(), nullptr, &m_psSsao);
        m_device->CreatePixelShader(psBlurH->GetBufferPointer(), psBlurH->GetBufferSize(), nullptr, &m_psAoBlurH);
        m_device->CreatePixelShader(psBlurV->GetBufferPointer(), psBlurV->GetBufferSize(), nullptr, &m_psAoBlurV);
        m_device->CreatePixelShader(psSsgi->GetBufferPointer(), psSsgi->GetBufferSize(), nullptr, &m_psSsgi);
        m_device->CreatePixelShader(psTemporal->GetBufferPointer(), psTemporal->GetBufferSize(), nullptr, &m_psTemporal);
        m_device->CreatePixelShader(psComposite->GetBufferPointer(), psComposite->GetBufferSize(), nullptr, &m_psComposite);
        m_device->CreatePixelShader(psAA->GetBufferPointer(), psAA->GetBufferSize(), nullptr, &m_psAA);
        m_device->CreateVertexShader(vsUiBurn->GetBufferPointer(), vsUiBurn->GetBufferSize(), nullptr, &m_vsUiBurn);
        m_device->CreatePixelShader(psUiBurn->GetBufferPointer(), psUiBurn->GetBufferSize(), nullptr, &m_psUiBurn);
        m_device->CreateVertexShader(vsUiTex->GetBufferPointer(), vsUiTex->GetBufferSize(), nullptr, &m_vsUiTex);
        m_device->CreatePixelShader(psUiTex->GetBufferPointer(), psUiTex->GetBufferSize(), nullptr, &m_psUiTex);

        D3D11_INPUT_ELEMENT_DESC uiTexEls[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(m_device->CreateInputLayout(uiTexEls, 3, vsUiTex->GetBufferPointer(),
                                               vsUiTex->GetBufferSize(), &m_uiTexLayout)))
        { error = "ui tex input layout failed"; return false; }
        m_device->CreateVertexShader(vsVfx->GetBufferPointer(), vsVfx->GetBufferSize(), nullptr, &m_vsVfx);
        m_device->CreatePixelShader(psVfx->GetBufferPointer(), psVfx->GetBufferSize(), nullptr, &m_psVfx);
        m_device->CreateVertexShader(vsVfxFull->GetBufferPointer(), vsVfxFull->GetBufferSize(), nullptr, &m_vsVfxFull);
        m_device->CreatePixelShader(psScorch->GetBufferPointer(), psScorch->GetBufferSize(), nullptr, &m_psScorch);

        D3D11_INPUT_ELEMENT_DESC meshEls[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(m_device->CreateInputLayout(meshEls, 4, vsMesh->GetBufferPointer(),
                                               vsMesh->GetBufferSize(), &m_meshLayout)))
        { error = "mesh input layout failed"; return false; }

        D3D11_INPUT_ELEMENT_DESC skinEls[] = {
            { "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TANGENT",      0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT,      0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(m_device->CreateInputLayout(skinEls, 6, vsMeshSkin->GetBufferPointer(),
                                               vsMeshSkin->GetBufferSize(), &m_skinLayout)))
        { error = "skinned input layout failed"; return false; }

        D3D11_INPUT_ELEMENT_DESC uiEls[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(m_device->CreateInputLayout(uiEls, 2, vsUi->GetBufferPointer(),
                                               vsUi->GetBufferSize(), &m_uiLayout)))
        { error = "ui input layout failed"; return false; }

        D3D11_BUFFER_DESC cbd{};
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        cbd.ByteWidth = sizeof(PerFrameCB);
        m_device->CreateBuffer(&cbd, nullptr, &m_cbFrame);
        m_device->CreateBuffer(&cbd, nullptr, &m_cbShadowFrame);
        cbd.ByteWidth = sizeof(PerObjectCB);
        m_device->CreateBuffer(&cbd, nullptr, &m_cbObject);
        cbd.ByteWidth = sizeof(PostCB);
        m_device->CreateBuffer(&cbd, nullptr, &m_cbPost);
        cbd.ByteWidth = sizeof(VfxCB);
        m_device->CreateBuffer(&cbd, nullptr, &m_cbVfx);
        cbd.ByteWidth = sizeof(UiBurnCB);
        m_device->CreateBuffer(&cbd, nullptr, &m_cbUiBurn);
        cbd.ByteWidth = sizeof(XMFLOAT4X4) * MaxBones;   // bone palette (b2)
        m_device->CreateBuffer(&cbd, nullptr, &m_cbBones);

        if (!CreateShadowMap(error))
            return false;

        D3D11_BUFFER_DESC uvb{};
        uvb.ByteWidth = UiVbBytes;
        uvb.Usage = D3D11_USAGE_DYNAMIC;
        uvb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        uvb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_device->CreateBuffer(&uvb, nullptr, &m_uiVb);
        m_device->CreateBuffer(&uvb, nullptr, &m_uiTexVb);

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_BACK;
        rd.FrontCounterClockwise = TRUE;    // glTF winding
        rd.DepthClipEnable = TRUE;
        m_device->CreateRasterizerState(&rd, &m_raster);
        // Shadow pass renders BACKFACES (cull front): closed casters store
        // their far side, so shadows meet the occluder exactly at the contact
        // line (no peter-panning gap at wall bases) and the bias stays tiny --
        // it pushes into the caster's interior instead of detaching the
        // shadow. The up-facing ground quad drops out of the shadow map
        // automatically (it only receives).
        rd.CullMode = D3D11_CULL_FRONT;
        rd.DepthBias = 0;      // bias lives on the receiver side (shader, +z):
        rd.SlopeScaledDepthBias = 0.0f;   // pushing stored backfaces deeper
        m_device->CreateRasterizerState(&rd, &m_rasterShadow);   // would re-open
        // the lit contact strip at coplanar box bottoms
        rd.DepthBias = 0;
        rd.SlopeScaledDepthBias = 0.0f;
        rd.CullMode = D3D11_CULL_NONE;      // UI + fullscreen passes
        m_device->CreateRasterizerState(&rd, &m_rasterUi);

        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = TRUE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        m_device->CreateDepthStencilState(&dsd, &m_depthOn);
        dsd.DepthEnable = FALSE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        m_device->CreateDepthStencilState(&dsd, &m_depthOff);
        dsd.DepthEnable = TRUE;                       // test but never write (VFX)
        dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        m_device->CreateDepthStencilState(&dsd, &m_depthRead);

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        m_device->CreateBlendState(&bd, &m_blendAlpha);
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR;   // multiply (decals)
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        m_device->CreateBlendState(&bd, &m_blendMultiply);

        D3D11_SAMPLER_DESC smp{};
        smp.Filter = D3D11_FILTER_ANISOTROPIC;
        smp.MaxAnisotropy = 8;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        smp.MaxLOD = D3D11_FLOAT32_MAX;
        m_device->CreateSamplerState(&smp, &m_samplerAniso);
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        smp.MaxAnisotropy = 1;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        m_device->CreateSamplerState(&smp, &m_samplerPoint);
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        m_device->CreateSamplerState(&smp, &m_samplerLinear);
        smp.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        smp.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        m_device->CreateSamplerState(&smp, &m_samplerShadow);
        return true;
    }

    void UpdateCB(ID3D11Buffer* cb, const void* data, size_t size)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(m_ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            memcpy(mapped.pData, data, size);
            m_ctx->Unmap(cb, 0);
        }
    }

    static constexpr UINT UiVbBytes = 4 * 1024 * 1024;

    HWND m_hwnd{};
    int m_width = 0, m_height = 0;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<IDXGISwapChain1> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_backRtv;
    ComPtr<ID3D11DepthStencilView> m_dsv;
    ComPtr<ID3D11ShaderResourceView> m_depthSrv;
    RenderTexture m_sceneColor, m_sceneNormal, m_sceneAlbedo, m_ao, m_aoTmp, m_final,
                  m_gi, m_accum[2];
    int m_accumIndex = 0;
    bool m_giHalf = true;
    int m_giW = 1, m_giH = 1;
    bool m_historyValid = false;
    XMFLOAT4X4 m_prevViewProj{};
    XMFLOAT3 m_prevCamPos{};
    uint64_t m_frameIndex = 0;
    ComPtr<ID3D11VertexShader> m_vsMesh, m_vsMeshSkinned, m_vsUi, m_vsFull,
                               m_vsVfx, m_vsVfxFull, m_vsUiBurn, m_vsUiTex;
    ComPtr<ID3D11PixelShader> m_psMesh, m_psUi, m_psSsao, m_psAoBlurH, m_psAoBlurV,
                              m_psSsgi, m_psTemporal, m_psComposite, m_psAA, m_psVfx,
                              m_psScorch, m_psUiBurn, m_psUiTex;
    ComPtr<ID3D11InputLayout> m_meshLayout, m_skinLayout, m_uiLayout, m_uiTexLayout;
    ComPtr<ID3D11Buffer> m_cbFrame, m_cbShadowFrame, m_cbObject, m_cbPost, m_cbVfx,
                         m_cbUiBurn, m_cbBones, m_uiVb, m_uiTexVb;
    ComPtr<ID3D11RasterizerState> m_raster, m_rasterShadow, m_rasterUi;
    ComPtr<ID3D11DepthStencilState> m_depthOn, m_depthOff, m_depthRead;
    ComPtr<ID3D11BlendState> m_blendAlpha, m_blendMultiply;
    ComPtr<ID3D11SamplerState> m_samplerAniso, m_samplerPoint, m_samplerLinear, m_samplerShadow;
    ComPtr<ID3D11DepthStencilView> m_shadowDsv, m_dsvReadOnly;
    ComPtr<ID3D11ShaderResourceView> m_shadowSrv;
    int m_shadowSize = 2048;
    std::vector<GpuMesh> m_meshes;
    std::vector<ComPtr<ID3D11ShaderResourceView>> m_textures;
};

} // namespace

IRenderer* CreateRendererD3D11() { return new RendererD3D11(); }

} // namespace tankaq
