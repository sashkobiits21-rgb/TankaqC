#include "AssetLoad.h"
#include "Log.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace DirectX;

namespace tankaq
{

static bool ReadPrimitives(const cgltf_node* node, MeshData& out)
{
    if (!node->mesh)
        return false;
    for (size_t p = 0; p < node->mesh->primitives_count; ++p)
    {
        const cgltf_primitive& prim = node->mesh->primitives[p];
        const cgltf_accessor* pos = nullptr;
        const cgltf_accessor* nrm = nullptr;
        const cgltf_accessor* uv = nullptr;
        for (size_t a = 0; a < prim.attributes_count; ++a)
        {
            const cgltf_attribute& at = prim.attributes[a];
            if (at.type == cgltf_attribute_type_position) pos = at.data;
            else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
            else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
        }
        if (!pos)
            continue;
        uint32_t base = uint32_t(out.verts.size());
        for (size_t v = 0; v < pos->count; ++v)
        {
            Vertex vert{};
            float tmp[3]{};
            cgltf_accessor_read_float(pos, v, tmp, 3);
            vert.px = tmp[0]; vert.py = tmp[1]; vert.pz = tmp[2];
            if (nrm)
            {
                cgltf_accessor_read_float(nrm, v, tmp, 3);
                vert.nx = tmp[0]; vert.ny = tmp[1]; vert.nz = tmp[2];
            }
            if (uv)
            {
                float t2[2]{};
                cgltf_accessor_read_float(uv, v, t2, 2);
                vert.u = t2[0]; vert.v = t2[1];
            }
            out.verts.push_back(vert);
        }
        if (prim.indices)
        {
            for (size_t i = 0; i < prim.indices->count; ++i)
                out.indices.push_back(base + uint32_t(cgltf_accessor_read_index(prim.indices, i)));
        }
        else
        {
            for (size_t i = 0; i < pos->count; ++i)
                out.indices.push_back(base + uint32_t(i));
        }
    }
    return !out.verts.empty();
}

TankModel LoadTankModel(const std::string& glbPath, const std::string& metaPath)
{
    TankModel model;

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, glbPath.c_str(), &data) != cgltf_result_success)
    {
        Log("Assets: failed to parse %s", glbPath.c_str());
        return model;
    }
    if (cgltf_load_buffers(&options, data, glbPath.c_str()) != cgltf_result_success)
    {
        Log("Assets: failed to load buffers for %s", glbPath.c_str());
        cgltf_free(data);
        return model;
    }

    for (size_t n = 0; n < data->nodes_count; ++n)
    {
        const cgltf_node& node = data->nodes[n];
        if (!node.name || !node.mesh)
            continue;
        if (strcmp(node.name, "hull") == 0)
        {
            ReadPrimitives(&node, model.hull);
        }
        else if (strcmp(node.name, "turret") == 0)
        {
            ReadPrimitives(&node, model.turret);
            model.turretPivot = XMFLOAT3(node.translation[0], node.translation[1], node.translation[2]);
        }
    }

    // Embedded palette texture (first image, PNG in a buffer view).
    if (data->images_count > 0 && data->images[0].buffer_view)
    {
        const cgltf_buffer_view* bv = data->images[0].buffer_view;
        const uint8_t* bytes = static_cast<const uint8_t*>(bv->buffer->data) + bv->offset;
        int w = 0, h = 0, comp = 0;
        uint8_t* pixels = stbi_load_from_memory(bytes, int(bv->size), &w, &h, &comp, 4);
        if (pixels)
        {
            model.palette.width = w;
            model.palette.height = h;
            model.palette.rgba.assign(pixels, pixels + size_t(w) * h * 4);
            stbi_image_free(pixels);
        }
    }
    cgltf_free(data);

    // Sidecar metadata.
    FILE* f = nullptr;
    fopen_s(&f, metaPath.c_str(), "r");
    if (f)
    {
        char key[64];
        float x, y, z;
        while (fscanf_s(f, "%63s %f %f %f", key, unsigned(sizeof(key)), &x, &y, &z) == 4)
        {
            if (!strcmp(key, "turret_pivot")) model.turretPivot = XMFLOAT3(x, y, z);
            else if (!strcmp(key, "muzzle")) model.muzzle = XMFLOAT3(x, y, z);
            else if (!strcmp(key, "hull_min")) model.hullMin = XMFLOAT3(x, y, z);
            else if (!strcmp(key, "hull_max")) model.hullMax = XMFLOAT3(x, y, z);
        }
        fclose(f);
    }
    else
    {
        Log("Assets: missing %s", metaPath.c_str());
    }

    model.valid = !model.hull.verts.empty() && !model.turret.verts.empty()
                  && model.palette.width > 0;
    Log("Assets: tank hull %zu verts / %zu tris, turret %zu verts / %zu tris, palette %dx%d%s",
        model.hull.verts.size(), model.hull.indices.size() / 3,
        model.turret.verts.size(), model.turret.indices.size() / 3,
        model.palette.width, model.palette.height, model.valid ? "" : "  INVALID");
    return model;
}

// ---------------------------------------------------------------- procedural

static void AddQuad(MeshData& m, XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c, XMFLOAT3 d,
                    XMFLOAT3 n, float u0, float v0, float u1, float v1)
{
    uint32_t base = uint32_t(m.verts.size());
    m.verts.push_back({ a.x, a.y, a.z, n.x, n.y, n.z, u0, v1 });
    m.verts.push_back({ b.x, b.y, b.z, n.x, n.y, n.z, u0, v0 });
    m.verts.push_back({ c.x, c.y, c.z, n.x, n.y, n.z, u1, v0 });
    m.verts.push_back({ d.x, d.y, d.z, n.x, n.y, n.z, u1, v1 });
    // CCW front faces (matches glTF / FrontCounterClockwise rasterizer state)
    m.indices.insert(m.indices.end(), { base, base + 2, base + 1, base, base + 3, base + 2 });
}

MeshData MakeBox(float hx, float hy, float hz, float uvScale)
{
    MeshData m;
    float sx = hx * uvScale, sy = hy * uvScale, sz = hz * uvScale;
    AddQuad(m, { -hx,-hy, hz }, { -hx, hy, hz }, {  hx, hy, hz }, {  hx,-hy, hz }, { 0,0,1 },  0, 0, sx * 2, sy * 2); // +Z
    AddQuad(m, {  hx,-hy,-hz }, {  hx, hy,-hz }, { -hx, hy,-hz }, { -hx,-hy,-hz }, { 0,0,-1 }, 0, 0, sx * 2, sy * 2); // -Z
    AddQuad(m, {  hx,-hy, hz }, {  hx, hy, hz }, {  hx, hy,-hz }, {  hx,-hy,-hz }, { 1,0,0 },  0, 0, sz * 2, sy * 2); // +X
    AddQuad(m, { -hx,-hy,-hz }, { -hx, hy,-hz }, { -hx, hy, hz }, { -hx,-hy, hz }, { -1,0,0 }, 0, 0, sz * 2, sy * 2); // -X
    AddQuad(m, { -hx, hy, hz }, { -hx, hy,-hz }, {  hx, hy,-hz }, {  hx, hy, hz }, { 0,1,0 },  0, 0, sx * 2, sz * 2); // +Y
    AddQuad(m, { -hx,-hy,-hz }, { -hx,-hy, hz }, {  hx,-hy, hz }, {  hx,-hy,-hz }, { 0,-1,0 }, 0, 0, sx * 2, sz * 2); // -Y
    return m;
}

MeshData MakeGroundPlane(float halfSize, float uvTiles)
{
    MeshData m;
    AddQuad(m, { -halfSize, 0,  halfSize }, { -halfSize, 0, -halfSize },
               {  halfSize, 0, -halfSize }, {  halfSize, 0,  halfSize },
               { 0, 1, 0 }, 0, 0, uvTiles, uvTiles);
    return m;
}

MeshData MakeSphere(float radius, int slices, int stacks)
{
    MeshData m;
    for (int st = 0; st <= stacks; ++st)
    {
        float phi = float(st) / stacks * XM_PI;
        for (int sl = 0; sl <= slices; ++sl)
        {
            float theta = float(sl) / slices * XM_2PI;
            float nx = sinf(phi) * cosf(theta);
            float ny = cosf(phi);
            float nz = sinf(phi) * sinf(theta);
            m.verts.push_back({ nx * radius, ny * radius, nz * radius,
                                nx, ny, nz,
                                float(sl) / slices, float(st) / stacks });
        }
    }
    int stride = slices + 1;
    for (int st = 0; st < stacks; ++st)
        for (int sl = 0; sl < slices; ++sl)
        {
            uint32_t a = uint32_t(st * stride + sl);
            uint32_t b = a + stride;
            m.indices.insert(m.indices.end(), { a, a + 1, b, b, a + 1, b + 1 });
        }
    return m;
}

static uint32_t Hash2D(int x, int y)
{
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

ImageData MakeGroundTexture(int size)
{
    ImageData img;
    img.width = img.height = size;
    img.rgba.resize(size_t(size) * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            // dusty ground: layered value noise + faint grid lines
            float n = 0.f, amp = 0.55f;
            for (int oct = 0; oct < 4; ++oct)
            {
                int cell = size >> (7 - oct); if (cell < 1) cell = 1;
                n += amp * float(Hash2D(x / cell, y / cell) & 1023) / 1023.f;
                amp *= 0.5f;
            }
            float base = 0.55f + 0.25f * n;
            uint8_t r = uint8_t(178 * base);
            uint8_t g = uint8_t(168 * base);
            uint8_t b = uint8_t(138 * base);
            if (x % 64 == 0 || y % 64 == 0)
            {
                r = uint8_t(r * 0.82f); g = uint8_t(g * 0.82f); b = uint8_t(b * 0.82f);
            }
            size_t i = (size_t(y) * size + x) * 4;
            img.rgba[i + 0] = r; img.rgba[i + 1] = g; img.rgba[i + 2] = b; img.rgba[i + 3] = 255;
        }
    return img;
}

ImageData MakeWallTexture(int size)
{
    ImageData img;
    img.width = img.height = size;
    img.rgba.resize(size_t(size) * size * 4);
    int bh = size / 8, bw = size / 4;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            int row = y / bh;
            int xo = (row % 2) ? bw / 2 : 0;
            bool mortar = (y % bh) < 2 || ((x + xo) % bw) < 2;
            float n = float(Hash2D(x / 3, y / 3) & 255) / 255.f;
            uint8_t r, g, b;
            if (mortar) { r = g = b = uint8_t(150 + 30 * n); }
            else
            {
                float shade = 0.75f + 0.25f * n;
                r = uint8_t(155 * shade); g = uint8_t(105 * shade); b = uint8_t(88 * shade);
            }
            size_t i = (size_t(y) * size + x) * 4;
            img.rgba[i + 0] = r; img.rgba[i + 1] = g; img.rgba[i + 2] = b; img.rgba[i + 3] = 255;
        }
    return img;
}

ImageData MakeSolidTexture(uint8_t r, uint8_t g, uint8_t b)
{
    ImageData img;
    img.width = img.height = 4;
    img.rgba.resize(4 * 4 * 4);
    for (int i = 0; i < 16; ++i)
    {
        img.rgba[i * 4 + 0] = r; img.rgba[i * 4 + 1] = g;
        img.rgba[i * 4 + 2] = b; img.rgba[i * 4 + 3] = 255;
    }
    return img;
}

std::vector<ImageData> BuildMipChain(const ImageData& src)
{
    std::vector<ImageData> chain;
    chain.push_back(src);
    while (chain.back().width > 1 || chain.back().height > 1)
    {
        const ImageData& p = chain.back();
        ImageData next;
        next.width = std::max(1, p.width / 2);
        next.height = std::max(1, p.height / 2);
        next.rgba.resize(size_t(next.width) * next.height * 4);
        for (int y = 0; y < next.height; ++y)
            for (int x = 0; x < next.width; ++x)
            {
                int x0 = std::min(x * 2, p.width - 1), x1 = std::min(x * 2 + 1, p.width - 1);
                int y0 = std::min(y * 2, p.height - 1), y1 = std::min(y * 2 + 1, p.height - 1);
                for (int c = 0; c < 4; ++c)
                {
                    int sum = p.rgba[(size_t(y0) * p.width + x0) * 4 + c]
                            + p.rgba[(size_t(y0) * p.width + x1) * 4 + c]
                            + p.rgba[(size_t(y1) * p.width + x0) * 4 + c]
                            + p.rgba[(size_t(y1) * p.width + x1) * 4 + c];
                    next.rgba[(size_t(y) * next.width + x) * 4 + c] = uint8_t(sum / 4);
                }
            }
        chain.push_back(std::move(next));
    }
    return chain;
}

} // namespace tankaq
