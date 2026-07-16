#include "AssetLoad.h"
#include "Game.h"
#include "Log.h"
#include <array>
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

    ComputeTangents(model.hull);
    ComputeTangents(model.turret);
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
    ComputeTangents(m);
    return m;
}

MeshData MakeGroundPlane(float halfSize, float uvTiles)
{
    MeshData m;
    AddQuad(m, { -halfSize, 0,  halfSize }, { -halfSize, 0, -halfSize },
               {  halfSize, 0, -halfSize }, {  halfSize, 0,  halfSize },
               { 0, 1, 0 }, 0, 0, uvTiles, uvTiles);
    ComputeTangents(m);
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
    ComputeTangents(m);
    return m;
}

// ------------------------------------------------------------ skinned rigs

static void SkinnedTangents(SkinnedPart& m)
{
    std::vector<XMFLOAT3> acc(m.verts.size(), { 0, 0, 0 });
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
    {
        const SkinnedVertex& v0 = m.verts[m.indices[i]];
        const SkinnedVertex& v1 = m.verts[m.indices[i + 1]];
        const SkinnedVertex& v2 = m.verts[m.indices[i + 2]];
        float du1 = v1.u - v0.u, dv1 = v1.v - v0.v;
        float du2 = v2.u - v0.u, dv2 = v2.v - v0.v;
        float det = du1 * dv2 - du2 * dv1;
        if (fabsf(det) < 1e-9f)
            continue;
        float r = 1.0f / det;
        XMFLOAT3 t{ ((v1.px - v0.px) * dv2 - (v2.px - v0.px) * dv1) * r,
                    ((v1.py - v0.py) * dv2 - (v2.py - v0.py) * dv1) * r,
                    ((v1.pz - v0.pz) * dv2 - (v2.pz - v0.pz) * dv1) * r };
        for (int c = 0; c < 3; ++c)
        {
            uint32_t idx = m.indices[i + c];
            acc[idx].x += t.x; acc[idx].y += t.y; acc[idx].z += t.z;
        }
    }
    for (size_t i = 0; i < m.verts.size(); ++i)
    {
        SkinnedVertex& v = m.verts[i];
        float nx = v.nx, ny = v.ny, nz = v.nz;
        float tx = acc[i].x, ty = acc[i].y, tz = acc[i].z;
        if (tx * tx + ty * ty + tz * tz < 1e-10f)
        {
            if (fabsf(ny) < 0.9f) { tx = -nz; ty = 0; tz = nx; }
            else { tx = 1; ty = 0; tz = 0; }
        }
        float d = tx * nx + ty * ny + tz * nz;
        tx -= nx * d; ty -= ny * d; tz -= nz * d;
        float len = sqrtf(tx * tx + ty * ty + tz * tz);
        if (len < 1e-6f) { tx = 1; ty = 0; tz = 0; len = 1; }
        v.tx = tx / len; v.ty = ty / len; v.tz = tz / len; v.tw = 1.0f;
    }
}

SkinnedModel LoadSkinnedGLB(const std::string& path)
{
    SkinnedModel model;
    cgltf_options opts{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success)
    {
        Log("Assets: cannot parse %s", path.c_str());
        return model;
    }
    if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success)
    {
        Log("Assets: cannot load buffers of %s", path.c_str());
        cgltf_free(data);
        return model;
    }
    if (data->skins_count == 0)
    {
        Log("Assets: %s has no skin", path.c_str());
        cgltf_free(data);
        return model;
    }

    const cgltf_skin& skin = data->skins[0];
    size_t jointCount = std::min<size_t>(skin.joints_count, MaxBones);

    // Order joints parents-before-children so composition is a single pass.
    std::vector<const cgltf_node*> ordered;
    ordered.reserve(jointCount);
    {
        auto inSkin = [&](const cgltf_node* n) -> int
        {
            for (size_t j = 0; j < jointCount; ++j)
                if (skin.joints[j] == n)
                    return int(j);
            return -1;
        };
        std::vector<bool> added(jointCount, false);
        bool progressed = true;
        while (ordered.size() < jointCount && progressed)
        {
            progressed = false;
            for (size_t j = 0; j < jointCount; ++j)
            {
                if (added[j])
                    continue;
                const cgltf_node* n = skin.joints[j];
                int par = n->parent ? inSkin(n->parent) : -1;
                bool parentReady = par < 0 || added[size_t(par)];
                if (parentReady)
                {
                    added[j] = true;
                    ordered.push_back(n);
                    progressed = true;
                }
            }
        }
        // cycles can't happen in valid glTF; append leftovers defensively
        for (size_t j = 0; j < jointCount; ++j)
            if (!added[j])
                ordered.push_back(skin.joints[j]);
    }
    auto orderedIndex = [&](const cgltf_node* n) -> int
    {
        for (size_t j = 0; j < ordered.size(); ++j)
            if (ordered[j] == n)
                return int(j);
        return -1;
    };
    auto skinIndex = [&](const cgltf_node* n) -> int
    {
        for (size_t j = 0; j < jointCount; ++j)
            if (skin.joints[j] == n)
                return int(j);
        return -1;
    };

    model.joints.resize(ordered.size());
    model.jointNames.resize(ordered.size());
    for (size_t j = 0; j < ordered.size(); ++j)
    {
        const cgltf_node* n = ordered[j];
        model.jointNames[j] = n->name ? n->name : "";
        SkinJoint& out = model.joints[j];
        out.parent = n->parent ? orderedIndex(n->parent) : -1;
        if (n->has_translation)
            out.restT = XMFLOAT3(n->translation[0], n->translation[1],
                                 n->translation[2]);
        if (n->has_rotation)
            out.restR = XMFLOAT4(n->rotation[0], n->rotation[1],
                                 n->rotation[2], n->rotation[3]);
        if (n->has_scale)
            out.restS = XMFLOAT3(n->scale[0], n->scale[1], n->scale[2]);
        // inverse bind from the skin (indexed by the SKIN's joint order)
        XMStoreFloat4x4(&out.inverseBind, XMMatrixIdentity());
        int si = skinIndex(n);
        if (skin.inverse_bind_matrices && si >= 0)
        {
            float mtx[16];
            cgltf_accessor_read_float(skin.inverse_bind_matrices, si, mtx, 16);
            // glTF is column-major; this codebase stores row-vector matrices,
            // which use the same memory layout for M^T... glTF stores column-
            // major m[col][row]; loading raw gives us the TRANSPOSE in row-
            // major terms, which is exactly the row-vector convention here.
            memcpy(&out.inverseBind, mtx, sizeof(mtx));
        }
    }

    // Ancestor pre-transform: FBX->glTF conversions often park a scale-100 +
    // axis-flip node ABOVE the armature root; the skin's joints don't include
    // it, so compose the chain here and let ComposePalette apply it.
    XMStoreFloat4x4(&model.rootTransform, XMMatrixIdentity());
    if (!ordered.empty())
    {
        XMMATRIX pre = XMMatrixIdentity();
        for (const cgltf_node* a = ordered[0]->parent; a; a = a->parent)
        {
            XMMATRIX local;
            if (a->has_matrix)
            {
                XMFLOAT4X4 m;
                memcpy(&m, a->matrix, sizeof(m));
                local = XMLoadFloat4x4(&m);
            }
            else
            {
                XMVECTOR q = a->has_rotation
                    ? XMVectorSet(a->rotation[0], a->rotation[1],
                                  a->rotation[2], a->rotation[3])
                    : XMQuaternionIdentity();
                local = XMMatrixScaling(a->has_scale ? a->scale[0] : 1.0f,
                                        a->has_scale ? a->scale[1] : 1.0f,
                                        a->has_scale ? a->scale[2] : 1.0f)
                      * XMMatrixRotationQuaternion(q)
                      * XMMatrixTranslation(
                            a->has_translation ? a->translation[0] : 0.0f,
                            a->has_translation ? a->translation[1] : 0.0f,
                            a->has_translation ? a->translation[2] : 0.0f);
            }
            pre = pre * local;   // child-then-parent (row-vector order)
        }
        XMStoreFloat4x4(&model.rootTransform, pre);
    }

    auto nodeLocal = [](const cgltf_node* a) -> XMMATRIX
    {
        if (a->has_matrix)
        {
            XMFLOAT4X4 m;
            memcpy(&m, a->matrix, sizeof(m));
            return XMLoadFloat4x4(&m);
        }
        XMVECTOR q = a->has_rotation
            ? XMVectorSet(a->rotation[0], a->rotation[1],
                          a->rotation[2], a->rotation[3])
            : XMQuaternionIdentity();
        return XMMatrixScaling(a->has_scale ? a->scale[0] : 1.0f,
                               a->has_scale ? a->scale[1] : 1.0f,
                               a->has_scale ? a->scale[2] : 1.0f)
             * XMMatrixRotationQuaternion(q)
             * XMMatrixTranslation(a->has_translation ? a->translation[0] : 0.0f,
                                   a->has_translation ? a->translation[1] : 0.0f,
                                   a->has_translation ? a->translation[2] : 0.0f);
    };

    // Geometry: every skinned primitive of EVERY skinned node. Characters
    // are often split into several parts (body/head/feet), each carrying its
    // own skin object over the same armature -- joint indices are remapped
    // through that node's own skin into our ordered joint list.
    for (size_t ni = 0; ni < data->nodes_count; ++ni)
    {
        const cgltf_node& node = data->nodes[ni];
        if (!node.skin || !node.mesh)
            continue;
        const cgltf_skin& nodeSkin = *node.skin;
        for (size_t p = 0; p < node.mesh->primitives_count; ++p)
        {
            const cgltf_primitive& prim = node.mesh->primitives[p];
            const cgltf_accessor* pos = nullptr;
            const cgltf_accessor* nrm = nullptr;
            const cgltf_accessor* uv = nullptr;
            const cgltf_accessor* jnt = nullptr;
            const cgltf_accessor* wgt = nullptr;
            for (size_t a = 0; a < prim.attributes_count; ++a)
            {
                const cgltf_attribute& at = prim.attributes[a];
                if (at.type == cgltf_attribute_type_position) pos = at.data;
                else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
                else if (at.type == cgltf_attribute_type_joints && at.index == 0) jnt = at.data;
                else if (at.type == cgltf_attribute_type_weights && at.index == 0) wgt = at.data;
            }
            if (!pos || !jnt || !wgt)
                continue;
            SkinnedPart part;
            for (size_t v = 0; v < pos->count; ++v)
            {
                SkinnedVertex vert{};
                float tmp[4]{};
                cgltf_accessor_read_float(pos, v, tmp, 3);
                vert.px = tmp[0]; vert.py = tmp[1]; vert.pz = tmp[2];
                if (nrm)
                {
                    cgltf_accessor_read_float(nrm, v, tmp, 3);
                    vert.nx = tmp[0]; vert.ny = tmp[1]; vert.nz = tmp[2];
                }
                if (uv)
                {
                    cgltf_accessor_read_float(uv, v, tmp, 2);
                    vert.u = tmp[0]; vert.v = tmp[1];
                }
                cgltf_uint ji[4]{};
                cgltf_accessor_read_uint(jnt, v, ji, 4);
                cgltf_accessor_read_float(wgt, v, tmp, 4);
                float wsum = tmp[0] + tmp[1] + tmp[2] + tmp[3];
                if (wsum < 1e-6f) { tmp[0] = 1; wsum = 1; }
                for (int k = 0; k < 4; ++k)
                {
                    // remap THIS node's skin-order index to our ordered list
                    int si = int(ji[k]);
                    int oi = (si >= 0 && si < int(nodeSkin.joints_count))
                                 ? orderedIndex(nodeSkin.joints[si]) : 0;
                    vert.joints[k] = uint8_t(std::clamp(oi, 0, MaxBones - 1));
                    vert.weights[k] = tmp[k] / wsum;
                }
                part.verts.push_back(vert);
            }
            if (prim.indices)
                for (size_t i = 0; i < prim.indices->count; ++i)
                    part.indices.push_back(
                        uint32_t(cgltf_accessor_read_index(prim.indices, i)));
            else
                for (size_t i = 0; i < pos->count; ++i)
                    part.indices.push_back(uint32_t(i));

            if (prim.material && prim.material->has_pbr_metallic_roughness)
            {
                const auto& pbr = prim.material->pbr_metallic_roughness;
                part.baseColor = XMFLOAT4(pbr.base_color_factor[0],
                                          pbr.base_color_factor[1],
                                          pbr.base_color_factor[2],
                                          pbr.base_color_factor[3]);
                // base color texture (first one seen wins)
                const cgltf_texture* tex = pbr.base_color_texture.texture;
                if (model.texture.width == 0 && tex && tex->image
                    && tex->image->buffer_view)
                {
                    const cgltf_buffer_view* bv = tex->image->buffer_view;
                    const uint8_t* bytes =
                        static_cast<const uint8_t*>(bv->buffer->data) + bv->offset;
                    int w = 0, h = 0, comp = 0;
                    if (uint8_t* px = stbi_load_from_memory(bytes, int(bv->size),
                                                            &w, &h, &comp, 4))
                    {
                        model.texture.width = w;
                        model.texture.height = h;
                        model.texture.rgba.assign(px, px + size_t(w) * h * 4);
                        stbi_image_free(px);
                    }
                }
            }
            part.name = node.name ? node.name
                                  : (node.mesh->name ? node.mesh->name : "");
            SkinnedTangents(part);
            model.parts.push_back(std::move(part));
        }
    }

    // Bone-attached STATIC meshes (heads, shoulder pads, hand-held weapons):
    // nodes with a mesh but no skin whose ancestry reaches a joint. Convert
    // to single-influence skinned parts: vertices pre-transformed by the
    // local chain up to the joint, weighted 100% to it -- they then animate
    // through the ordinary skinned path.
    for (size_t ni = 0; ni < data->nodes_count; ++ni)
    {
        const cgltf_node& node = data->nodes[ni];
        if (node.skin || !node.mesh)
            continue;
        XMMATRIX chain = nodeLocal(&node);
        int joint = -1;
        for (const cgltf_node* a = node.parent; a; a = a->parent)
        {
            int oi = orderedIndex(a);
            if (oi >= 0) { joint = oi; break; }
            chain = chain * nodeLocal(a);
        }
        if (joint < 0)
            continue;   // not part of the rig (standalone prop)
        // The skinned path computes v * inverseBind * global, expecting
        // BIND-SPACE vertices; our chain lands in JOINT-LOCAL space, so
        // append the joint's bind matrix (inverse of its inverse bind) --
        // the shader's inverseBind then cancels exactly: v * chain * global.
        chain = chain * XMMatrixInverse(
            nullptr, XMLoadFloat4x4(&model.joints[joint].inverseBind));
        for (size_t p = 0; p < node.mesh->primitives_count; ++p)
        {
            const cgltf_primitive& prim = node.mesh->primitives[p];
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
            SkinnedPart part;
            part.name = node.name ? node.name : "";
            for (size_t v = 0; v < pos->count; ++v)
            {
                SkinnedVertex vert{};
                float tmp[4]{};
                cgltf_accessor_read_float(pos, v, tmp, 3);
                XMVECTOR pv = XMVector3Transform(
                    XMVectorSet(tmp[0], tmp[1], tmp[2], 1), chain);
                vert.px = XMVectorGetX(pv);
                vert.py = XMVectorGetY(pv);
                vert.pz = XMVectorGetZ(pv);
                if (nrm)
                {
                    cgltf_accessor_read_float(nrm, v, tmp, 3);
                    XMVECTOR nv = XMVector3Normalize(XMVector3TransformNormal(
                        XMVectorSet(tmp[0], tmp[1], tmp[2], 0), chain));
                    vert.nx = XMVectorGetX(nv);
                    vert.ny = XMVectorGetY(nv);
                    vert.nz = XMVectorGetZ(nv);
                }
                if (uv)
                {
                    cgltf_accessor_read_float(uv, v, tmp, 2);
                    vert.u = tmp[0]; vert.v = tmp[1];
                }
                vert.joints[0] = uint8_t(std::clamp(joint, 0, MaxBones - 1));
                vert.weights[0] = 1.0f;
                part.verts.push_back(vert);
            }
            if (prim.indices)
                for (size_t i = 0; i < prim.indices->count; ++i)
                    part.indices.push_back(
                        uint32_t(cgltf_accessor_read_index(prim.indices, i)));
            else
                for (size_t i = 0; i < pos->count; ++i)
                    part.indices.push_back(uint32_t(i));
            if (prim.material && prim.material->has_pbr_metallic_roughness)
            {
                const auto& pbr = prim.material->pbr_metallic_roughness;
                part.baseColor = XMFLOAT4(pbr.base_color_factor[0],
                                          pbr.base_color_factor[1],
                                          pbr.base_color_factor[2],
                                          pbr.base_color_factor[3]);
            }
            SkinnedTangents(part);
            model.parts.push_back(std::move(part));
        }
    }

    // Animations: keep channels that target our joints.
    for (size_t ai = 0; ai < data->animations_count; ++ai)
    {
        const cgltf_animation& anim = data->animations[ai];
        AnimClip clip;
        clip.name = anim.name ? anim.name : "clip";
        for (size_t ci = 0; ci < anim.channels_count; ++ci)
        {
            const cgltf_animation_channel& ch = anim.channels[ci];
            int joint = ch.target_node ? orderedIndex(ch.target_node) : -1;
            if (joint < 0 || !ch.sampler)
                continue;
            int path = ch.target_path == cgltf_animation_path_type_translation ? 0
                     : ch.target_path == cgltf_animation_path_type_rotation ? 1
                     : ch.target_path == cgltf_animation_path_type_scale ? 2 : -1;
            if (path < 0)
                continue;
            AnimChannel out;
            out.joint = joint;
            out.path = path;
            const cgltf_accessor* in = ch.sampler->input;
            const cgltf_accessor* val = ch.sampler->output;
            bool cubic = ch.sampler->interpolation
                         == cgltf_interpolation_type_cubic_spline;
            size_t stride = cubic ? 3 : 1;      // cubic: inTan, value, outTan
            size_t offset = cubic ? 1 : 0;
            for (size_t k = 0; k < in->count; ++k)
            {
                float t = 0;
                cgltf_accessor_read_float(in, k, &t, 1);
                out.times.push_back(t);
                float v4[4]{ 0, 0, 0, 1 };
                cgltf_accessor_read_float(val, k * stride + offset, v4,
                                          path == 1 ? 4 : 3);
                out.values.push_back(XMFLOAT4(v4[0], v4[1], v4[2], v4[3]));
                clip.duration = std::max(clip.duration, t);
            }
            clip.channels.push_back(std::move(out));
        }
        if (!clip.channels.empty())
            model.clips.push_back(std::move(clip));
    }

    cgltf_free(data);
    model.valid = !model.parts.empty() && !model.joints.empty();
    size_t tv = 0, ti = 0;
    for (const SkinnedPart& p : model.parts) { tv += p.verts.size(); ti += p.indices.size(); }
    Log("Assets: skinned %s: %zu parts, %zu verts / %zu tris, %zu joints, %zu clips",
        path.c_str(), model.parts.size(), tv, ti / 3,
        model.joints.size(), model.clips.size());
    for (size_t c = 0; c < model.clips.size(); ++c)
        Log("Assets:   clip %zu: %s (%.2fs)", c, model.clips[c].name.c_str(),
            model.clips[c].duration);
    return model;
}

int SkinnedModel::FindClip(const char* nameSubstr) const
{
    for (size_t c = 0; c < clips.size(); ++c)
        if (clips[c].name.find(nameSubstr) != std::string::npos)
            return int(c);
    return -1;
}

void ComputeTangents(MeshData& m)
{
    std::vector<XMFLOAT3> accT(m.verts.size(), { 0, 0, 0 });
    std::vector<XMFLOAT3> accB(m.verts.size(), { 0, 0, 0 });
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
    {
        const Vertex& v0 = m.verts[m.indices[i]];
        const Vertex& v1 = m.verts[m.indices[i + 1]];
        const Vertex& v2 = m.verts[m.indices[i + 2]];
        float e1x = v1.px - v0.px, e1y = v1.py - v0.py, e1z = v1.pz - v0.pz;
        float e2x = v2.px - v0.px, e2y = v2.py - v0.py, e2z = v2.pz - v0.pz;
        float du1 = v1.u - v0.u, dv1 = v1.v - v0.v;
        float du2 = v2.u - v0.u, dv2 = v2.v - v0.v;
        float det = du1 * dv2 - du2 * dv1;
        if (fabsf(det) < 1e-9f)
            continue;   // degenerate UVs (palette models): fallback below
        float r = 1.0f / det;
        XMFLOAT3 t{ (e1x * dv2 - e2x * dv1) * r,
                    (e1y * dv2 - e2y * dv1) * r,
                    (e1z * dv2 - e2z * dv1) * r };
        XMFLOAT3 b{ (e2x * du1 - e1x * du2) * r,
                    (e2y * du1 - e1y * du2) * r,
                    (e2z * du1 - e1z * du2) * r };
        for (int c = 0; c < 3; ++c)
        {
            uint32_t idx = m.indices[i + c];
            accT[idx].x += t.x; accT[idx].y += t.y; accT[idx].z += t.z;
            accB[idx].x += b.x; accB[idx].y += b.y; accB[idx].z += b.z;
        }
    }
    for (size_t i = 0; i < m.verts.size(); ++i)
    {
        Vertex& v = m.verts[i];
        float nx = v.nx, ny = v.ny, nz = v.nz;
        float tx = accT[i].x, ty = accT[i].y, tz = accT[i].z;
        if (tx * tx + ty * ty + tz * tz < 1e-10f)
        {
            // no UV gradient: any perpendicular will do
            if (fabsf(ny) < 0.9f) { tx = ny * 0 - nz * 1; ty = 0; tz = nx; } // n x up-ish
            else { tx = 1; ty = 0; tz = 0; }
        }
        // Gram-Schmidt against the normal
        float d = tx * nx + ty * ny + tz * nz;
        tx -= nx * d; ty -= ny * d; tz -= nz * d;
        float len = sqrtf(tx * tx + ty * ty + tz * tz);
        if (len < 1e-6f) { tx = 1; ty = 0; tz = 0; len = 1; }
        v.tx = tx / len; v.ty = ty / len; v.tz = tz / len;
        // handedness: does the accumulated bitangent agree with n x t?
        float cx = ny * v.tz - nz * v.ty;
        float cy = nz * v.tx - nx * v.tz;
        float cz = nx * v.ty - ny * v.tx;
        float hb = cx * accB[i].x + cy * accB[i].y + cz * accB[i].z;
        v.tw = (hb < 0.0f) ? -1.0f : 1.0f;
    }
}

ImageData MakeFlatNRA(float roughness)
{
    ImageData img;
    img.width = img.height = 4;
    img.rgba.resize(4 * 4 * 4);
    uint8_t r = uint8_t(std::clamp(roughness, 0.0f, 1.0f) * 255.0f);
    for (int i = 0; i < 16; ++i)
    {
        img.rgba[i * 4 + 0] = 128;
        img.rgba[i * 4 + 1] = 128;
        img.rgba[i * 4 + 2] = 255;
        img.rgba[i * 4 + 3] = r;
    }
    return img;
}

ImageData MakeNormalRoughFromTexture(const ImageData& src, float strength,
                                     float roughMin, float roughMax)
{
    ImageData img;
    img.width = src.width;
    img.height = src.height;
    img.rgba.resize(size_t(img.width) * img.height * 4);
    auto lum = [&](int x, int y)
    {
        x = (x + src.width) % src.width;
        y = (y + src.height) % src.height;
        const uint8_t* p = &src.rgba[(size_t(y) * src.width + x) * 4];
        return (p[0] * 0.299f + p[1] * 0.587f + p[2] * 0.114f) / 255.0f;
    };
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
        {
            float dx = lum(x + 1, y) - lum(x - 1, y);
            float dy = lum(x, y + 1) - lum(x, y - 1);
            float nx = -dx * strength, ny = -dy * strength, nz = 1.0f;
            float il = 1.0f / sqrtf(nx * nx + ny * ny + nz * nz);
            uint8_t* o = &img.rgba[(size_t(y) * img.width + x) * 4];
            o[0] = uint8_t((nx * il * 0.5f + 0.5f) * 255.0f);
            o[1] = uint8_t((ny * il * 0.5f + 0.5f) * 255.0f);
            o[2] = uint8_t((nz * il * 0.5f + 0.5f) * 255.0f);
            float rough = roughMax + (roughMin - roughMax) * lum(x, y);
            o[3] = uint8_t(std::clamp(rough, 0.0f, 1.0f) * 255.0f);
        }
    return img;
}

// Little rocket, local +Z forward, centered: z spans [-0.5, +0.5].
// Lathed profile (exhaust ring, tail taper, body, nose cone) + 4 fins.
// The squish/spring vertex shader assumes exactly this length.
MeshData MakeRocket()
{
    MeshData m;
    constexpr int kSides = 24;
    // key profile points: (z, radius) -- nozzle, tail step, boat-tail, body,
    // then an elliptical ogive nose sampled into the key list
    std::vector<std::array<float, 2>> key = {
        { -0.50f, 0.050f },   // exhaust lip
        { -0.46f, 0.058f },
        { -0.44f, 0.095f },   // tail step
        { -0.30f, 0.130f },   // boat-tail into body
        {  0.10f, 0.130f },   // body end
    };
    for (int i = 1; i <= 8; ++i)
    {
        float t = float(i) / 8.0f;
        key.push_back({ 0.10f + 0.40f * t,
                        0.130f * sqrtf(std::max(0.0f, 1.0f - t * t)) });
    }
    const int kKeys = int(key.size());
    // Subdivide long segments (max ring gap ~0.04): the squish/spring vertex
    // deformation needs intermediate rings to bend at the exit plane --
    // without them the traveling bulge quantizes into visible pops.
    std::vector<std::array<float, 2>> prof;
    for (int k = 0; k < kKeys - 1; ++k)
    {
        float dz = key[k + 1][0] - key[k][0];
        int steps = std::max(1, int(ceilf(fabsf(dz) / 0.04f)));
        for (int s = 0; s < steps; ++s)
        {
            float t = float(s) / steps;
            prof.push_back({ key[k][0] + dz * t,
                             key[k][1] + (key[k + 1][1] - key[k][1]) * t });
        }
    }
    prof.push_back({ key[kKeys - 1][0], key[kKeys - 1][1] });
    const int kRings = int(prof.size());

    for (int ring = 0; ring < kRings; ++ring)
    {
        // slope normal from neighbouring rings
        int r0 = ring > 0 ? ring - 1 : ring;
        int r1 = ring < kRings - 1 ? ring + 1 : ring;
        float dz = prof[r1][0] - prof[r0][0];
        float dr = prof[r1][1] - prof[r0][1];
        float len = sqrtf(dz * dz + dr * dr);
        float nRad = (len > 1e-5f) ? dz / len : 1.0f;   // radial component
        float nZ = (len > 1e-5f) ? -dr / len : 0.0f;    // axial component
        for (int s = 0; s <= kSides; ++s)
        {
            float a = XM_2PI * float(s) / kSides;
            float ca = cosf(a), sa = sinf(a);
            m.verts.push_back({ ca * prof[ring][1], sa * prof[ring][1],
                                prof[ring][0],
                                ca * nRad, sa * nRad, nZ,
                                float(s) / kSides, prof[ring][0] + 0.5f });
        }
    }
    int stride = kSides + 1;
    for (int ring = 0; ring < kRings - 1; ++ring)
        for (int s = 0; s < kSides; ++s)
        {
            uint32_t a = uint32_t(ring * stride + s);
            uint32_t b = a + stride;
            m.indices.insert(m.indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
        }
    // exhaust cap (fan, facing -Z)
    uint32_t center = uint32_t(m.verts.size());
    m.verts.push_back({ 0, 0, -0.5f, 0, 0, -1, 0.5f, 0 });
    for (int s = 0; s < kSides; ++s)
        m.indices.insert(m.indices.end(),
                         { center, uint32_t(s), uint32_t(s + 1) });

    // 4 fins at 45-degree offsets, with real thickness (thin swept prisms)
    for (int f = 0; f < 4; ++f)
    {
        float a = XM_2PI * (f + 0.5f) / 4.0f;
        float ca = cosf(a), sa = sinf(a);
        float tx = -sa, ty = ca;            // fin tangent (thickness axis)
        const float th = 0.011f;            // half thickness
        // fin corners in (radial, z): swept back
        const float rz[4][2] = {
            { 0.10f, -0.30f }, { 0.24f, -0.48f },
            { 0.24f, -0.36f }, { 0.10f, -0.14f },
        };
        uint32_t base = uint32_t(m.verts.size());
        for (int side = 0; side < 2; ++side)
        {
            float s = side == 0 ? 1.0f : -1.0f;
            for (int c = 0; c < 4; ++c)
                m.verts.push_back({ ca * rz[c][0] + tx * th * s,
                                    sa * rz[c][0] + ty * th * s,
                                    rz[c][1],
                                    tx * s, ty * s, 0,
                                    0.5f, rz[c][1] + 0.5f });
        }
        // faces: +side, -side (reversed winding), outer swept edge, top edge
        uint32_t A = base, B = base + 4;
        m.indices.insert(m.indices.end(),
            { A, A + 1, A + 2,  A, A + 2, A + 3,          // +side
              B, B + 2, B + 1,  B, B + 3, B + 2 });       // -side
        // outer edge (corners 1-2) and trailing edge (corners 2-3)
        auto edge = [&](uint32_t c0, uint32_t c1, float ex, float ey, float ez)
        {
            uint32_t e = uint32_t(m.verts.size());
            const Vertex& v0 = m.verts[A + c0];
            const Vertex& v1 = m.verts[A + c1];
            const Vertex& v2 = m.verts[B + c1];
            const Vertex& v3 = m.verts[B + c0];
            for (const Vertex* v : { &v0, &v1, &v2, &v3 })
                m.verts.push_back({ v->px, v->py, v->pz, ex, ey, ez,
                                    v->u, v->v });
            m.indices.insert(m.indices.end(),
                             { e, e + 1, e + 2, e, e + 2, e + 3 });
        };
        // outer edge normal ~ radial; trailing edge ~ -z
        edge(1, 2, ca, sa, 0);
        edge(2, 3, ca * 0.4f, sa * 0.4f, 0.9f);
    }
    ComputeTangents(m);
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

MeshData MakeDisc(float radius, float y, int segments)
{
    MeshData m;
    Vertex c{};
    c.px = 0; c.py = y; c.pz = 0;
    c.nx = 0; c.ny = 1; c.nz = 0;
    c.u = 0.5f; c.v = 0.5f;
    m.verts.push_back(c);
    for (int i = 0; i <= segments; ++i)
    {
        float a = XM_2PI * float(i) / float(segments);
        Vertex v{};
        v.px = sinf(a) * radius; v.py = y; v.pz = cosf(a) * radius;
        v.nx = 0; v.ny = 1; v.nz = 0;
        v.u = 0.5f + sinf(a) * 0.5f; v.v = 0.5f + cosf(a) * 0.5f;
        m.verts.push_back(v);
    }
    for (int i = 1; i <= segments; ++i)
    {
        m.indices.push_back(0);
        m.indices.push_back(uint32_t(i));
        m.indices.push_back(uint32_t(i + 1));
    }
    ComputeTangents(m);
    return m;
}

MeshData MakeRing(float radius, float width, int segments)
{
    MeshData m;
    float r0 = radius - width * 0.5f, r1 = radius + width * 0.5f;
    for (int i = 0; i <= segments; ++i)
    {
        float a = XM_2PI * float(i) / float(segments);
        float s = sinf(a), c = cosf(a);
        Vertex vin{}, vout{};
        vin.px = s * r0; vin.py = 0; vin.pz = c * r0;
        vout.px = s * r1; vout.py = 0; vout.pz = c * r1;
        vin.nx = vout.nx = 0; vin.ny = vout.ny = 1; vin.nz = vout.nz = 0;
        vin.u = float(i) / segments; vin.v = 0;
        vout.u = float(i) / segments; vout.v = 1;
        m.verts.push_back(vin);
        m.verts.push_back(vout);
    }
    for (int i = 0; i < segments; ++i)
    {
        uint32_t a = uint32_t(i * 2), b = a + 1, c2 = a + 2, d = a + 3;
        m.indices.push_back(a); m.indices.push_back(b); m.indices.push_back(c2);
        m.indices.push_back(b); m.indices.push_back(d); m.indices.push_back(c2);
    }
    ComputeTangents(m);
    return m;
}

ImageData LoadImageFile(const std::string& path)
{
    ImageData img;
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!px)
    {
        Log("Assets: cannot load image %s", path.c_str());
        return img;
    }
    img.width = w;
    img.height = h;
    img.rgba.assign(px, px + size_t(w) * h * 4);
    stbi_image_free(px);
    return img;
}

ImageData MakeNraFromMaps(const ImageData& normal, const ImageData& rough)
{
    ImageData out;
    if (normal.width <= 0)
        return out;
    out.width = normal.width;
    out.height = normal.height;
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x)
        {
            size_t di = (size_t(y) * out.width + x) * 4;
            out.rgba[di + 0] = normal.rgba[di + 0];
            out.rgba[di + 1] = normal.rgba[di + 1];
            out.rgba[di + 2] = normal.rgba[di + 2];
            uint8_t r = 128;
            if (rough.width > 0)
            {
                int sx = x * rough.width / out.width;
                int sy = y * rough.height / out.height;
                size_t si = (size_t(sy) * rough.width + sx) * 4;
                // roughness maps are effectively grayscale: take green
                r = rough.rgba[si + 1];
            }
            out.rgba[di + 3] = r;
        }
    return out;
}

MeshData MakeGhostMesh()
{
    // lathe profile bottom-to-top: wavy hem, skirt, pinched waist,
    // shoulders, dome closing at the crown
    const int SEG = 18, RINGS = 9;
    const float hs[RINGS] = { 0.00f, 0.10f, 0.28f, 0.50f, 0.74f,
                              0.98f, 1.16f, 1.30f, 1.40f };
    const float rs[RINGS] = { 0.48f, 0.52f, 0.44f, 0.37f, 0.36f,
                              0.43f, 0.38f, 0.24f, 0.02f };
    MeshData m;
    for (int i = 0; i < RINGS; ++i)
    {
        for (int j = 0; j <= SEG; ++j)
        {
            float a = XM_2PI * float(j) / float(SEG);
            float wav = (i == 0) ? 0.06f * sinf(3.0f * a) : 0.0f;
            float r = rs[i] + wav;
            Vertex v{};
            v.px = sinf(a) * r;
            v.py = hs[i] + (i == 0 ? 0.04f * sinf(3.0f * a + 1.6f) : 0.0f);
            v.pz = cosf(a) * r;
            // approximate outward normal with a slight upward lean
            float nl = sqrtf(sinf(a) * sinf(a) + 0.12f + cosf(a) * cosf(a));
            v.nx = sinf(a) / nl; v.ny = 0.35f / nl; v.nz = cosf(a) / nl;
            v.u = float(j) / SEG;
            v.v = float(i) / (RINGS - 1);
            m.verts.push_back(v);
        }
    }
    for (int i = 0; i < RINGS - 1; ++i)
        for (int j = 0; j < SEG; ++j)
        {
            uint32_t a = uint32_t(i * (SEG + 1) + j);
            uint32_t b = a + 1;
            uint32_t c = a + (SEG + 1);
            uint32_t e = c + 1;
            m.indices.push_back(a); m.indices.push_back(b); m.indices.push_back(c);
            m.indices.push_back(b); m.indices.push_back(e); m.indices.push_back(c);
        }
    ComputeTangents(m);
    return m;
}

MeshData MakePieWedge()
{
    // one 15-degree sector of a unit disc: the countdown draws K of these
    // rotated in sequence, filling the circle clockwise like a clock hand
    MeshData m;
    const int SUB = 2;
    const float kStep = XM_2PI / 24.0f;
    Vertex c{};
    c.px = 0; c.py = 0; c.pz = 0;
    c.nx = 0; c.ny = 1; c.nz = 0;
    c.u = 0.5f; c.v = 0.5f;
    m.verts.push_back(c);
    for (int i = 0; i <= SUB; ++i)
    {
        float a = kStep * float(i) / float(SUB);
        Vertex v{};
        v.px = sinf(a); v.py = 0; v.pz = cosf(a);
        v.nx = 0; v.ny = 1; v.nz = 0;
        v.u = 0.5f + sinf(a) * 0.5f; v.v = 0.5f + cosf(a) * 0.5f;
        m.verts.push_back(v);
    }
    for (int i = 1; i <= SUB; ++i)
    {
        m.indices.push_back(0);
        m.indices.push_back(uint32_t(i));
        m.indices.push_back(uint32_t(i + 1));
    }
    ComputeTangents(m);
    return m;
}

// ------------------------------------------------------------ icon atlas

namespace
{
struct IconCanvas
{
    ImageData* img;
    int ox, size;
    // logical 16x16 grid scaled to the icon size
    void Px(int gx, int gy, uint32_t rgba)
    {
        int s = size / 16;
        for (int y = gy * s; y < (gy + 1) * s; ++y)
            for (int x = gx * s; x < (gx + 1) * s; ++x)
            {
                size_t i = (size_t(y) * img->width + ox + x) * 4;
                img->rgba[i + 0] = uint8_t(rgba >> 24);
                img->rgba[i + 1] = uint8_t(rgba >> 16);
                img->rgba[i + 2] = uint8_t(rgba >> 8);
                img->rgba[i + 3] = uint8_t(rgba);
            }
    }
    void Fill(int x0, int y0, int x1, int y1, uint32_t c)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                Px(x, y, c);
    }
};

void DrawIconGlyph(IconCanvas& c, int icon)
{
    const uint32_t W = 0xF2F2E6FFu;   // warm white
    const uint32_t D = 0x1E1E1EFFu;   // dark outline-ish accents
    switch (icon)
    {
    case 0:  // ENGINE: double chevron
        for (int i = 0; i < 5; ++i)
        {
            c.Fill(3 + i, 3 + i, 3 + i, 4 + i, W);
            c.Fill(3 + i, 11 - i, 3 + i, 12 - i, W);
            c.Fill(8 + i, 3 + i, 8 + i, 4 + i, W);
            c.Fill(8 + i, 11 - i, 8 + i, 12 - i, W);
        }
        break;
    case 1:  // TURBO: speed lines + wedge
        c.Fill(2, 5, 7, 5, W); c.Fill(2, 8, 6, 8, W); c.Fill(2, 11, 7, 11, W);
        for (int i = 0; i < 5; ++i) c.Fill(9, 4 + i, 9 + i, 4 + i, W);
        for (int i = 0; i < 4; ++i) c.Fill(9, 12 - i, 9 + i, 12 - i, W);
        break;
    case 2:  // AP ROUNDS: bullet
        c.Fill(6, 2, 9, 3, W); c.Fill(5, 4, 10, 9, W);
        c.Fill(4, 10, 11, 13, D); c.Fill(5, 10, 10, 12, W);
        break;
    case 3:  // HEAVY SHELLS: fat shell
        c.Fill(5, 2, 10, 4, W); c.Fill(4, 5, 11, 10, W);
        c.Fill(3, 11, 12, 13, D); c.Fill(4, 11, 11, 12, W);
        break;
    case 4:  // AUTOLOADER: circular arrows (two arcs)
        c.Fill(4, 3, 11, 4, W); c.Fill(3, 4, 4, 8, W);
        c.Fill(11, 8, 12, 12, W); c.Fill(4, 11, 11, 12, W);
        c.Fill(10, 2, 13, 5, W); c.Fill(2, 10, 5, 13, W);
        break;
    case 5:  // GREASED BREECH: droplet
        c.Fill(7, 2, 8, 4, W); c.Fill(6, 5, 9, 7, W);
        c.Fill(5, 8, 10, 12, W); c.Fill(6, 13, 9, 13, W);
        break;
    case 6:  // PLATING: plus
        c.Fill(6, 2, 9, 13, W); c.Fill(2, 6, 13, 9, W);
        break;
    case 7:  // COMPOSITE: layered plates
        c.Fill(2, 3, 13, 5, W); c.Fill(3, 7, 12, 9, W); c.Fill(4, 11, 11, 13, W);
        break;
    case 8:  // GYRO: ring + dot
        c.Fill(5, 2, 10, 3, W); c.Fill(5, 12, 10, 13, W);
        c.Fill(2, 5, 3, 10, W); c.Fill(12, 5, 13, 10, W);
        c.Fill(6, 6, 9, 9, W);
        break;
    case 9:  // REACTIVE ARMOR: shield
        c.Fill(3, 2, 12, 8, W);
        for (int i = 0; i < 4; ++i) c.Fill(3 + i, 9 + i, 12 - i, 9 + i, W);
        c.Fill(7, 13, 8, 13, W);
        break;
    case 10: // OVERDRIVE: flame
        c.Fill(7, 1, 8, 3, W); c.Fill(5, 4, 9, 6, W); c.Fill(4, 7, 11, 10, W);
        c.Fill(5, 11, 10, 13, W); c.Fill(10, 3, 11, 5, W);
        break;
    case 12: // RICOCHET: V-shaped bouncing arrow
        for (int i = 0; i < 6; ++i) c.Fill(2 + i, 3 + i, 3 + i, 3 + i, W);
        for (int i = 0; i < 6; ++i) c.Fill(8 + i, 8 - i, 9 + i, 8 - i, W);
        c.Fill(11, 2, 13, 2, W); c.Fill(13, 2, 13, 4, W);   // arrow tip
        c.Fill(2, 12, 13, 13, D);                            // floor line
        break;
    case 13: // SUPERBALL: ball + bounce arcs
        c.Fill(6, 2, 9, 2, W); c.Fill(5, 3, 10, 6, W); c.Fill(6, 7, 9, 7, W);
        c.Fill(3, 10, 5, 10, W); c.Fill(2, 11, 3, 12, W);   // left arc
        c.Fill(10, 10, 12, 10, W); c.Fill(12, 11, 13, 12, W); // right arc
        break;
    case 14: // NITRO TANK: fuel canister with cap + spout
        c.Fill(4, 5, 11, 13, W); c.Fill(5, 3, 8, 4, W);
        c.Fill(9, 2, 12, 3, W); c.Fill(11, 4, 12, 5, W);
        c.Fill(6, 8, 9, 10, D);   // label notch
        break;
    case 15: // AFTERBURNER: flame with speed chevrons
        c.Fill(8, 2, 9, 4, W); c.Fill(7, 5, 10, 7, W);
        c.Fill(6, 8, 11, 10, W); c.Fill(7, 11, 10, 13, W);
        for (int i = 0; i < 3; ++i)
        { c.Fill(2, 4 + i, 3 + i, 4 + i, W); c.Fill(2, 12 - i, 3 + i, 12 - i, W); }
        break;
    case 16: // QUICK PUMP: droplet + up arrow
        c.Fill(4, 6, 5, 8, W); c.Fill(3, 9, 6, 12, W); c.Fill(4, 13, 5, 13, W);
        c.Fill(10, 3, 11, 12, W);
        c.Fill(8, 6, 9, 6, W); c.Fill(12, 6, 13, 6, W);
        c.Fill(9, 4, 9, 5, W); c.Fill(12, 4, 12, 5, W);
        break;
    case 17: // PIT CREW: clock face with fast hand
        c.Fill(5, 2, 10, 3, W); c.Fill(3, 4, 4, 11, W); c.Fill(11, 4, 12, 11, W);
        c.Fill(5, 12, 10, 13, W);
        c.Fill(7, 5, 8, 8, W); c.Fill(9, 8, 10, 9, W);   // hand
        break;
    case 18: // FUEL INJECTION: syringe diagonal
        for (int i = 0; i < 6; ++i) c.Fill(3 + i, 11 - i, 4 + i, 12 - i, W);
        c.Fill(9, 3, 12, 6, W); c.Fill(12, 2, 13, 3, W);
        c.Fill(2, 12, 3, 13, D);
        break;
    case 19: // SOLDIER CLASS: helmet
        c.Fill(5, 3, 10, 4, W); c.Fill(4, 5, 11, 7, W);        // dome
        c.Fill(2, 8, 13, 9, W);                                // brim
        c.Fill(6, 10, 9, 12, W); c.Fill(7, 10, 8, 11, D);      // chin strap
        break;
    case 20: // BOUNCY CLASS: ball mid-bounce with arcs
        c.Fill(6, 3, 9, 3, W); c.Fill(5, 4, 10, 7, W);
        c.Fill(6, 8, 9, 8, W);
        c.Fill(2, 10, 4, 10, W); c.Fill(2, 11, 2, 12, W);      // left arc
        c.Fill(11, 10, 13, 10, W); c.Fill(13, 11, 13, 12, W);  // right arc
        c.Fill(5, 13, 10, 13, D);                              // ground
        break;
    case 21: // RECRUITER: whistle
        c.Fill(3, 5, 9, 10, W); c.Fill(9, 6, 12, 8, W);        // body + mouth
        c.Fill(5, 7, 7, 8, D);                                 // pea hole
        c.Fill(10, 2, 10, 4, W); c.Fill(12, 2, 13, 3, W);      // toots
        break;
    case 22: // DOUBLE TIME: boot with speed lines
        c.Fill(6, 2, 9, 8, W); c.Fill(6, 9, 12, 12, W);        // shaft + foot
        c.Fill(2, 4, 4, 4, W); c.Fill(2, 7, 4, 7, W);
        c.Fill(2, 10, 4, 10, W);
        break;
    case 23: // FLAK VEST: vest with collar notch and belt
        c.Fill(3, 2, 5, 3, W); c.Fill(10, 2, 12, 3, W);        // shoulders
        c.Fill(3, 4, 12, 12, W);
        c.Fill(7, 2, 8, 5, D);                                 // collar notch
        c.Fill(5, 8, 10, 9, D);                                // belt
        break;
    case 24: // HOLLOW POINTS: bullet, tip notched
        c.Fill(6, 1, 9, 2, W); c.Fill(5, 3, 10, 9, W);         // tip + case
        c.Fill(4, 10, 11, 13, W);                              // rim
        c.Fill(7, 1, 8, 3, D);                                 // hollow tip
        break;
    case 25: // RAPID FIRE: stacked muzzle dashes
        c.Fill(2, 3, 7, 4, W);  c.Fill(9, 3, 11, 4, W);  c.Fill(13, 3, 13, 4, W);
        c.Fill(2, 7, 7, 8, W);  c.Fill(9, 7, 11, 8, W);  c.Fill(13, 7, 13, 8, W);
        c.Fill(2, 11, 7, 12, W); c.Fill(9, 11, 11, 12, W); c.Fill(13, 11, 13, 12, W);
        break;
    case 26: // PLATOON: three helmets in rank
        c.Fill(2, 3, 6, 5, W);  c.Fill(1, 6, 7, 6, W);
        c.Fill(9, 3, 13, 5, W); c.Fill(8, 6, 13, 6, W);
        c.Fill(5, 9, 10, 11, W); c.Fill(4, 12, 11, 12, W);
        break;
    case 27: // RUBBER SHELLS: shell with a zigzag tread
        c.Fill(6, 2, 9, 3, W); c.Fill(5, 4, 10, 10, W);        // ogive + body
        c.Fill(4, 11, 11, 12, W);                              // skirt
        c.Fill(6, 6, 6, 6, D); c.Fill(7, 7, 7, 7, D);          // zigzag
        c.Fill(8, 6, 8, 6, D); c.Fill(9, 7, 9, 7, D);
        break;
    case 28: // SLINGSHOT: Y-frame with band
        c.Fill(7, 7, 8, 13, W);                                // handle
        c.Fill(4, 2, 5, 7, W); c.Fill(10, 2, 11, 7, W);        // forks
        c.Fill(5, 3, 10, 3, D);                                // band
        break;
    case 29: // NECRO CLASS: skull
        c.Fill(4, 2, 11, 8, W); c.Fill(5, 9, 10, 10, W);
        c.Fill(5, 4, 6, 6, D); c.Fill(9, 4, 10, 6, D);
        c.Fill(5, 11, 5, 13, W); c.Fill(7, 11, 8, 13, W);
        c.Fill(10, 11, 10, 13, W);
        break;
    case 30: // RADAR CLASS: hub with sweep + arcs
        c.Fill(7, 7, 8, 8, W);
        c.Fill(3, 3, 3, 5, W); c.Fill(4, 2, 6, 2, W);
        c.Fill(12, 10, 12, 12, W); c.Fill(9, 13, 11, 13, W);
        for (int i = 0; i < 5; ++i) c.Fill(8 + i, 7 - i, 8 + i, 7 - i, W);
        break;
    case 31: // BONE FURNACE: flame over a bone
        c.Fill(7, 2, 8, 4, W); c.Fill(5, 5, 10, 8, W);
        c.Fill(3, 11, 4, 12, W); c.Fill(11, 11, 12, 12, W);
        c.Fill(4, 11, 11, 12, W);
        break;
    case 32: // CAUSTIC BREW: dripping flask
        c.Fill(6, 2, 9, 3, W); c.Fill(7, 4, 8, 6, W);
        c.Fill(5, 7, 10, 12, W); c.Fill(6, 9, 9, 11, D);
        c.Fill(12, 5, 12, 6, W); c.Fill(3, 8, 3, 9, W);
        break;
    case 33: // LINGERING ROT: droplets over a pool
        c.Fill(4, 2, 4, 4, W); c.Fill(7, 1, 8, 4, W); c.Fill(11, 3, 11, 5, W);
        c.Fill(3, 9, 12, 11, W); c.Fill(2, 10, 13, 10, W);
        break;
    case 34: // DEEP GRIP: grabbing hand
        c.Fill(4, 3, 4, 7, W); c.Fill(6, 2, 6, 7, W);
        c.Fill(8, 2, 8, 7, W); c.Fill(10, 3, 10, 7, W);
        c.Fill(4, 8, 11, 12, W); c.Fill(12, 6, 12, 9, W);
        break;
    case 35: // SOUL LEECH: spiral
        c.Fill(4, 3, 11, 3, W); c.Fill(11, 4, 11, 9, W);
        c.Fill(6, 9, 11, 9, W); c.Fill(6, 6, 6, 9, W);
        c.Fill(6, 6, 9, 6, W); c.Fill(9, 6, 9, 7, W);
        c.Fill(2, 3, 2, 12, D);
        break;
    case 36: // FAST LOCK: crosshair
        c.Fill(7, 2, 8, 5, W); c.Fill(7, 10, 8, 13, W);
        c.Fill(2, 7, 5, 8, W); c.Fill(10, 7, 13, 8, W);
        c.Fill(7, 7, 8, 8, W);
        break;
    case 37: // WIDE SCAN: expanding arcs
        c.Fill(3, 7, 4, 8, W);
        c.Fill(6, 5, 6, 10, W); c.Fill(7, 4, 7, 4, W); c.Fill(7, 11, 7, 11, W);
        c.Fill(10, 3, 10, 12, W); c.Fill(11, 2, 11, 2, W); c.Fill(11, 13, 11, 13, W);
        break;
    case 38: // PAYLOAD: bomb
        c.Fill(5, 6, 10, 12, W); c.Fill(4, 7, 11, 11, W);
        c.Fill(7, 4, 8, 5, W); c.Fill(9, 3, 10, 3, W);
        c.Fill(11, 1, 12, 2, D);
        break;
    case 39: // SHARP PING: dot with one tight ring
        c.Fill(7, 7, 8, 8, W);
        c.Fill(5, 4, 10, 4, W); c.Fill(5, 11, 10, 11, W);
        c.Fill(4, 5, 4, 10, W); c.Fill(11, 5, 11, 10, W);
        break;
    case 41: // FISSION SHELLS: shell forking into two arrows
        c.Fill(7, 10, 8, 13, W);                               // stem
        for (int i = 0; i < 4; ++i)
        { c.Fill(6 - i, 8 - i, 7 - i, 9 - i, W);
          c.Fill(8 + i, 8 - i, 9 + i, 9 - i, W); }
        c.Fill(2, 2, 4, 2, W); c.Fill(2, 3, 2, 4, W);          // arrowheads
        c.Fill(11, 2, 13, 2, W); c.Fill(13, 3, 13, 4, W);
        break;
    case 40: // NESTED ARRAY: concentric squares
        c.Fill(2, 2, 13, 2, W); c.Fill(2, 13, 13, 13, W);
        c.Fill(2, 3, 2, 12, W); c.Fill(13, 3, 13, 12, W);
        c.Fill(5, 5, 10, 5, W); c.Fill(5, 10, 10, 10, W);
        c.Fill(5, 6, 5, 9, W); c.Fill(10, 6, 10, 9, W);
        c.Fill(7, 7, 8, 8, W);
        break;
    default: // FIELD KIT: wrench
        c.Fill(2, 2, 5, 3, W); c.Fill(2, 5, 5, 6, W); c.Fill(4, 3, 5, 5, W);
        for (int i = 0; i < 8; ++i) c.Fill(5 + i, 4 + i, 6 + i, 5 + i, W);
        c.Fill(11, 10, 13, 13, W);
        break;
    }
}
} // namespace

ImageData MakeIconAtlas(int iconSize, int count)
{
    ImageData atlas;
    atlas.width = iconSize * count;
    atlas.height = iconSize;
    atlas.rgba.assign(size_t(atlas.width) * atlas.height * 4, 0);   // transparent

    for (int i = 0; i < count; ++i)
    {
        // optional override: assets/icons/<slug>.png
        std::string slug = kUpgradePool[i].name;
        for (char& ch : slug)
            ch = (ch == ' ') ? '_' : char(tolower(uint8_t(ch)));
        std::string path = "assets/icons/" + slug + ".png";
        int w = 0, h = 0, comp = 0;
        if (uint8_t* pixels = stbi_load(path.c_str(), &w, &h, &comp, 4))
        {
            for (int y = 0; y < iconSize; ++y)
                for (int x = 0; x < iconSize; ++x)
                {
                    int sx = x * w / iconSize, sy = y * h / iconSize;
                    size_t d = (size_t(y) * atlas.width + i * iconSize + x) * 4;
                    size_t s = (size_t(sy) * w + sx) * 4;
                    memcpy(&atlas.rgba[d], &pixels[s], 4);
                }
            stbi_image_free(pixels);
            Log("Assets: icon override %s", path.c_str());
            continue;
        }
        IconCanvas canvas{ &atlas, i * iconSize, iconSize };
        DrawIconGlyph(canvas, i);   // icon atlas slot == pool index
    }
    return atlas;
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
