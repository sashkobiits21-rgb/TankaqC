#pragma once
#include <algorithm>
#include <cmath>
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

// ---- frustum culling (shared by both backends) --------------------------
// Per-mesh bounding spheres are computed at upload; each pass culls against
// its own frustum (camera for the scene pass, the sun's ortho box for the
// shadow pass). Objects visible in neither pass never cost a CB slot.
struct BoundingSphere { DirectX::XMFLOAT3 center{}; float radius = 0; };

struct Frustum { DirectX::XMFLOAT4 planes[6]; };

inline Frustum FrustumFromViewProj(const DirectX::XMFLOAT4X4& m)
{
    // Gribb-Hartmann for row-vector matrices (v * M), D3D z in [0,1]
    Frustum f;
    auto plane = [&](float a, float b, float c, float d, int i)
    {
        float len = sqrtf(a * a + b * b + c * c);
        if (len < 1e-6f) len = 1.0f;
        f.planes[i] = DirectX::XMFLOAT4(a / len, b / len, c / len, d / len);
    };
    plane(m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41, 0);
    plane(m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41, 1);
    plane(m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42, 2);
    plane(m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42, 3);
    plane(m._13, m._23, m._33, m._43, 4);                       // near
    plane(m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43, 5);
    return f;
}

inline bool SphereInFrustum(const Frustum& f, const DirectX::XMFLOAT3& c,
                            float r)
{
    for (const DirectX::XMFLOAT4& p : f.planes)
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r)
            return false;
    return true;
}

// World-space sphere of a mesh instance: transform the center, scale the
// radius by the largest basis-vector length (handles non-uniform scale).
inline void TransformSphere(const BoundingSphere& bs,
                            const DirectX::XMFLOAT4X4& world,
                            DirectX::XMFLOAT3& outC, float& outR)
{
    using namespace DirectX;
    XMMATRIX w = XMLoadFloat4x4(&world);
    XMVECTOR c = XMVector3TransformCoord(XMLoadFloat3(&bs.center), w);
    XMStoreFloat3(&outC, c);
    float sx = XMVectorGetX(XMVector3Length(w.r[0]));
    float sy = XMVectorGetX(XMVector3Length(w.r[1]));
    float sz = XMVectorGetX(XMVector3Length(w.r[2]));
    outR = bs.radius * std::max(sx, std::max(sy, sz));
}

// Tight sphere at upload time: AABB center + true max distance. Works for
// Vertex and SkinnedVertex (both start with px/py/pz).
template <typename V>
inline BoundingSphere ComputeMeshBounds(const V* v, size_t n)
{
    BoundingSphere bs;
    if (!v || n == 0)
        return bs;
    float mn[3] = { v[0].px, v[0].py, v[0].pz };
    float mx[3] = { v[0].px, v[0].py, v[0].pz };
    for (size_t i = 1; i < n; ++i)
    {
        mn[0] = std::min(mn[0], v[i].px); mx[0] = std::max(mx[0], v[i].px);
        mn[1] = std::min(mn[1], v[i].py); mx[1] = std::max(mx[1], v[i].py);
        mn[2] = std::min(mn[2], v[i].pz); mx[2] = std::max(mx[2], v[i].pz);
    }
    bs.center = { (mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f,
                  (mn[2] + mx[2]) * 0.5f };
    float r2 = 0;
    for (size_t i = 0; i < n; ++i)
    {
        float dx = v[i].px - bs.center.x, dy = v[i].py - bs.center.y,
              dz = v[i].pz - bs.center.z;
        r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
    }
    bs.radius = sqrtf(r2);
    return bs;
}

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
