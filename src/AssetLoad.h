#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <DirectXMath.h>
#include "render/IRenderer.h"

namespace tankaq
{

struct MeshData
{
    std::vector<Vertex> verts;
    std::vector<uint32_t> indices;
};

struct ImageData
{
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
};

struct TankModel
{
    MeshData hull;
    MeshData turret;                 // vertices relative to turret pivot
    ImageData palette;
    DirectX::XMFLOAT3 turretPivot{}; // in hull space
    DirectX::XMFLOAT3 muzzle{};      // in turret-pivot space
    DirectX::XMFLOAT3 hullMin{}, hullMax{};
    bool valid = false;
};

// Loads assets/tank/tank_baked.glb + tank_meta.txt.
TankModel LoadTankModel(const std::string& glbPath, const std::string& metaPath);

// ------------------------------------------------------------ skinned rigs
// Rigged glTF/GLB (Blender: File > Export > glTF 2.0). First skin in the
// file; joints capped at MaxBones; LINEAR/STEP keyframes (CUBICSPLINE is
// sampled from its middle values).
struct AnimChannel
{
    int joint = -1;
    int path = 0;                     // 0 = translation, 1 = rotation, 2 = scale
    std::vector<float> times;
    std::vector<DirectX::XMFLOAT4> values;   // vec3 in xyz, quats full
};

struct AnimClip
{
    std::string name;
    float duration = 0;
    std::vector<AnimChannel> channels;
};

struct SkinJoint
{
    int parent = -1;                  // index into the joints array (ordered
                                      // parents-before-children)
    DirectX::XMFLOAT4X4 inverseBind;
    DirectX::XMFLOAT3 restT{ 0, 0, 0 };
    DirectX::XMFLOAT4 restR{ 0, 0, 0, 1 };
    DirectX::XMFLOAT3 restS{ 1, 1, 1 };
};

struct SkinnedModel
{
    std::vector<SkinnedVertex> verts;
    std::vector<uint32_t> indices;
    std::vector<SkinJoint> joints;
    std::vector<AnimClip> clips;
    ImageData texture;                // base color if present (else 0x0)
    bool valid = false;
};

SkinnedModel LoadSkinnedGLB(const std::string& path);

// Procedural meshes (unit-ish sizes, uv-mapped).
MeshData MakeBox(float halfX, float halfY, float halfZ, float uvScale);
MeshData MakeGroundPlane(float halfSize, float uvTiles);
MeshData MakeSphere(float radius, int slices, int stacks);
MeshData MakeRocket();   // +Z forward, z in [-0.5, 0.5] (squish shader relies on it)

// Procedural textures.
ImageData MakeGroundTexture(int size);
ImageData MakeWallTexture(int size);
ImageData MakeSolidTexture(uint8_t r, uint8_t g, uint8_t b);

// Upgrade icon atlas: a horizontal strip of `count` square icons. Each icon is
// a generated pixel-art glyph unless assets/icons/<slug>.png exists, in which
// case that image is loaded into the slot instead (names = upgrade names,
// lowercased, spaces -> '_').
ImageData MakeIconAtlas(int iconSize, int count);

// Per-vertex tangents from UV derivatives (Lengyel accumulation +
// Gram-Schmidt + handedness). Degenerate UV triangles (palette-mapped
// models) fall back to an arbitrary perpendicular -- harmless with flat maps.
void ComputeTangents(MeshData& m);

// NRA material maps: normal in rgb (tangent space, +Z out), roughness in a.
ImageData MakeFlatNRA(float roughness);
// Derive an NRA map from a color texture: height = luminance, normal = Sobel
// of the height (scaled by `strength`), roughness = lerp(roughMax, roughMin,
// luminance) so dark crevices are rough and bright faces smoother.
ImageData MakeNormalRoughFromTexture(const ImageData& src, float strength,
                                     float roughMin, float roughMax);

// Full box-filtered mip chain, level 0 = source. Each level tightly packed RGBA.
std::vector<ImageData> BuildMipChain(const ImageData& src);

} // namespace tankaq
