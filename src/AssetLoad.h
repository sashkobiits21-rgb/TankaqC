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

// Procedural meshes (unit-ish sizes, uv-mapped).
MeshData MakeBox(float halfX, float halfY, float halfZ, float uvScale);
MeshData MakeGroundPlane(float halfSize, float uvTiles);
MeshData MakeSphere(float radius, int slices, int stacks);

// Procedural textures.
ImageData MakeGroundTexture(int size);
ImageData MakeWallTexture(int size);
ImageData MakeSolidTexture(uint8_t r, uint8_t g, uint8_t b);

// Full box-filtered mip chain, level 0 = source. Each level tightly packed RGBA.
std::vector<ImageData> BuildMipChain(const ImageData& src);

} // namespace tankaq
