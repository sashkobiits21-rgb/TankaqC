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
    // tangent (xyz) + bitangent handedness (w); filled by ComputeTangents
    float tx = 1, ty = 0, tz = 0, tw = 1;
};

// Skinned meshes (rigged glTF exports): 4 bone influences per vertex.
constexpr int MaxBones = 64;
struct SkinnedVertex
{
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float tx = 1, ty = 0, tz = 0, tw = 1;
    uint8_t joints[4]{};
    float weights[4]{};
};

// One posed skeleton, ready for the GPU: palette[j] = inverseBind[j] *
// globalJointTransform[j] (row-vector order, matching the codebase).
struct BonePalette
{
    DirectX::XMFLOAT4X4 m[MaxBones];
    int count = 0;
};

struct UiVertex
{
    float x, y;          // pixels, origin top-left
    float r, g, b, a;
};

struct UiTexVertex
{
    float x, y;          // pixels, origin top-left
    float u, v;          // icon atlas coordinates
    float r, g, b, a;
};

// A rectangle being burn-dissolved (upgrade purchases). Rendered by a
// dedicated shader that eats a pixelated hole from `origin` outward.
// If u1 > u0 the quad samples the icon atlas (icons burn with the card).
struct UiBurnQuad
{
    float x, y, w, h;            // pixels
    float r, g, b, a;            // fragment color
    float originX, originY;      // burn origin in pixels (the click point)
    float progress;              // 0..1
    float maxRadius;             // radius at progress 1 (pixels)
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};

struct RenderObject
{
    int mesh = -1;
    int texture = -1;
    DirectX::XMFLOAT4X4 world{};
    DirectX::XMFLOAT4 tint{ 1, 1, 1, 0 };   // rgb multiplier, a = emissive amount
    bool isDynamic = false;                 // tanks/projectiles: burn decals skip these
    // rocket squish/spring deformation (vertex shader); dist < 0 disables
    float deformDist = -1.0f;               // distance traveled from the muzzle
    float deformAge = 0.0f;                 // seconds since fired
    // NRA map (normal rgb + roughness a); -1 = FrameData::defaultNormalTex
    int texNormal = -1;
    // skinned meshes: index into FrameData::palettes (-1 = static mesh)
    int paletteIndex = -1;
    // STEALTH: clip pixels without 2D line-of-sight from the local tank
    float losClip = 0.0f;
    // STEALTH: fully hidden right now -> also cast no shadow this frame
    bool noShadow = false;
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
    // STEALTH occlusion inputs: the local tank + the static occluder boxes
    DirectX::XMFLOAT2 losViewer{};
    int losBoxCount = 0;
    DirectX::XMFLOAT4 losBoxes[24]{};
    PostSettings post;
    std::vector<RenderObject> objects;
    std::vector<VfxBurstData> bursts;       // explosion smoke/fire (max 16 used)
    std::vector<VfxScorchData> scorches;    // burn decals (max 16 used)
    std::vector<UiVertex> ui;               // triangle list
    std::vector<UiTexVertex> uiTex;         // textured triangle list (icon atlas)
    int uiTexTexture = -1;                  // texture handle for uiTex + burn
    std::vector<UiBurnQuad> uiBurn;         // burning shop cards (max 32 used)
    std::vector<BonePalette> palettes;      // posed skeletons (max 16 used)
    int defaultNormalTex = -1;              // flat NRA for objects without one
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
    virtual int CreateSkinnedMesh(const SkinnedVertex* verts, size_t vertexCount,
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
