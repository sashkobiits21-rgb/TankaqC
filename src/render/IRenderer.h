#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <DirectXMath.h>

namespace tankaq
{

struct Vertex
{
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

struct UiVertex
{
    float x, y;          // pixels, origin top-left
    float r, g, b, a;
};

struct RenderObject
{
    int mesh = -1;
    int texture = -1;
    DirectX::XMFLOAT4X4 world{};
    DirectX::XMFLOAT4 tint{ 1, 1, 1, 0 };   // rgb multiplier, a = emissive amount
    bool isDynamic = false;                 // tanks/projectiles: burn decals skip these
};

struct PostSettings
{
    bool giEnabled = true;
    bool aoEnabled = true;
    bool giHalfRes = true;       // trace GI at half resolution, upsample bilaterally
    bool shadowsEnabled = true;  // sun shadow map
    bool aaEnabled = true;       // contrast-adaptive edge anti-aliasing
    int shadowMapSize = 2048;    // 1024 / 2048 / 4096
    int shadowFilter = 1;        // 0 = sharp (1 tap), 1 = soft (3x3), 2 = softer (5x5)
    int giRays = 4;              // screen-space rays per pixel per frame (1..16)
    int temporalSamples = 8;     // temporal accumulation length (2..16)
    float giIntensity = 1.7f;
};

struct VfxBurstData
{
    DirectX::XMFLOAT3 pos{};
    float age = 0;               // seconds since the explosion
};

struct VfxScorchData
{
    DirectX::XMFLOAT3 pos{};
    float age = 0;
};

struct FrameData
{
    DirectX::XMFLOAT4X4 viewProj{};
    DirectX::XMFLOAT4X4 invViewProj{};
    DirectX::XMFLOAT4X4 lightViewProj{};    // ortho sun camera over the arena
    DirectX::XMFLOAT3 camPos{};
    DirectX::XMFLOAT3 sunDir{ 0.45f, 0.8f, 0.35f };
    float ambient = 0.34f;
    float fogDensity = 0.008f;
    float time = 0;                         // seconds, drives VFX animation
    DirectX::XMFLOAT3 camRight{ 1, 0, 0 };  // billboard basis
    DirectX::XMFLOAT3 camUp{ 0, 1, 0 };
    PostSettings post;
    std::vector<RenderObject> objects;
    std::vector<VfxBurstData> bursts;       // explosion smoke/fire (max 16 used)
    std::vector<VfxScorchData> scorches;    // burn decals (max 16 used)
    std::vector<UiVertex> ui;               // triangle list
    bool vsync = true;
};

class IRenderer
{
public:
    virtual ~IRenderer() = default;
    virtual bool Init(void* hwnd, int width, int height, std::string& error) = 0;
    virtual void Resize(int width, int height) = 0;
    virtual int CreateMesh(const Vertex* verts, size_t vertexCount,
                           const uint32_t* indices, size_t indexCount) = 0;
    // rgba: tightly packed 8-bit RGBA. Full CPU mip chain is generated internally.
    virtual int CreateTexture(const uint8_t* rgba, int width, int height) = 0;
    virtual void RenderFrame(const FrameData& frame) = 0;
    virtual bool SaveBackbufferPNG(const std::string& path) = 0;
    virtual const char* Name() const = 0;
};

IRenderer* CreateRendererD3D11();
IRenderer* CreateRendererD3D12();

} // namespace tankaq
