// Shared forward shaders for both D3D11 and D3D12 backends (SM 5.0, compiled at runtime).

cbuffer PerFrame : register(b0)
{
    float4x4 gViewProj;
    float4x4 gLightViewProj;   // ortho sun camera (shadow map)
    float4   gSunDirAmbient;   // xyz = to-sun direction (normalized), w = ambient
    float4   gCamPosFog;       // xyz = camera position, w = fog density
    float4   gScreen;          // xy = viewport pixels, z = shadow texel, w = shadows on
    float4   gViewer;          // xy = LOCAL tank xz, w = LOS box count
    float4   gLosBoxes[56];    // STEALTH occluders: cx, cz, hx, hz
};

cbuffer PerObject : register(b1)
{
    float4x4 gWorld;
    float4   gTint;            // rgb multiplied over albedo, a = emissive amount
    float4   gMisc;            // x = dynamic flag, y = rocket distance from
                               // muzzle, z = rocket age (s), w = rocket flag
    float4   gMisc2;           // x = STEALTH LOS-clip flag
};

cbuffer Bones : register(b2)
{
    float4x4 gBones[64];   // inverseBind * globalJoint, row-vector order
};

Texture2D    gAlbedo  : register(t0);
Texture2D    gShadowMap : register(t1);
Texture2D    gNra     : register(t2);   // normal rgb (tangent space) + roughness a
SamplerState gSampler : register(s0);
SamplerComparisonState gShadowSampler : register(s1);

static const float3 FogColor = float3(0.72, 0.82, 1.00);   // pre-tonemap sky

// ---------------- mesh ----------------
struct VsIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD0;
    float4 tan : TANGENT;    // xyz tangent, w bitangent handedness
};
struct VsOut
{
    float4 sv   : SV_Position;
    float3 wpos : TEXCOORD1;
    float3 wnrm : TEXCOORD2;
    float2 uv   : TEXCOORD0;
    float4 wtan : TEXCOORD3; // world tangent + handedness
};

VsOut VSMesh(VsIn i)
{
    VsOut o;
    float3 p = i.pos;
    float3 n = i.nrm;

    // Rocket squish & spring (gMisc.w = 1). Local +Z is forward, the mesh
    // spans z in [-0.5, +0.5]. gMisc.y = distance traveled from the muzzle:
    // the barrel's exit plane therefore sits at local z = 0.5 - dist and
    // sweeps tailward as the rocket leaves the pipe.
    if (gMisc.w > 0.5)
    {
        float d = gMisc.y;
        float age = gMisc.z;
        // the exit plane sweeps FASTER than travel (x1.4): the whole squeeze
        // resolves in ~0.7 rocket lengths (~50 ms) so firing feels instant
        float zExit = 0.5 - d * 1.4;
        // everything is released by the PLANE passing it (smooth, per-vertex);
        // this factor only retires the residual bulge once the plane has
        // cleared the tail entirely -- no distance-window pop
        float pipeActive = smoothstep(-0.62, -0.45, zExit);

        // all weights from the undeformed z
        float zi = p.z;

        // jello out of a pipe: vertices still behind the exit plane squeeze
        // to the bore, with a bulge riding the plane itself
        float inside = (1.0 - smoothstep(zExit - 0.12, zExit + 0.08, zi))
                     * pipeActive;
        float radial = lerp(1.0, 0.55, inside);
        radial *= 1.0 + exp(-abs(zi - zExit) * 7.0) * 0.45
                        * step(0.02, d) * pipeActive;

        // longitudinal compression ANCHORED AT THE NOSE: the emerged front
        // holds its shape while the unexited rear stays compressed and
        // relaxes rearward as the plane frees it (nose never wobbles)
        float fin = saturate(zExit + 0.5);        // fraction still in pipe
        float squash = 1.0 - 0.30 * fin;
        p.z = 0.5 - (0.5 - zi) * squash;

        // spring: tail-weighted decaying ring-down. Only the rear -- the part
        // that left the barrel last -- oscillates; weight fades to zero at
        // the nose so the front flies clean.
        float freed = smoothstep(0.65, 0.95, d);
        float osc = sin(age * 24.0) * exp(-age * 5.0) * freed;
        float tailW = saturate((0.30 - zi) / 0.80);   // 0 nose, 1 tail
        p.z -= osc * 0.18 * tailW;
        radial *= 1.0 + osc * 0.30 * tailW;           // counter-bulge

        p.xy *= radial;
        n = normalize(float3(n.xy / max(radial, 0.05), n.z));
    }

    float4 wp = mul(gWorld, float4(p, 1.0));
    o.wpos = wp.xyz;
    o.sv = mul(gViewProj, wp);
    o.wnrm = mul((float3x3)gWorld, n);
    o.wtan = float4(mul((float3x3)gWorld, i.tan.xyz), i.tan.w);
    o.uv = i.uv;
    return o;
}

// ---------------- skinned mesh (rigged glTF) ----------------
struct VsInSkin
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD0;
    float4 tan : TANGENT;
    uint4  jnt : BLENDINDICES;
    float4 wgt : BLENDWEIGHT;
};

VsOut VSMeshSkinned(VsInSkin i)
{
    // 4-influence linear blend skinning on position, normal and tangent
    float4x4 skin = gBones[i.jnt.x] * i.wgt.x
                  + gBones[i.jnt.y] * i.wgt.y
                  + gBones[i.jnt.z] * i.wgt.z
                  + gBones[i.jnt.w] * i.wgt.w;
    float3 p = mul(skin, float4(i.pos, 1.0)).xyz;
    float3 n = mul((float3x3)skin, i.nrm);
    float3 t = mul((float3x3)skin, i.tan.xyz);

    VsOut o;
    float4 wp = mul(gWorld, float4(p, 1.0));
    o.wpos = wp.xyz;
    o.sv = mul(gViewProj, wp);
    o.wnrm = mul((float3x3)gWorld, n);
    o.wtan = float4(mul((float3x3)gWorld, t), i.tan.w);
    o.uv = i.uv;
    return o;
}

// G-buffer output: direct-lit color, packed world normal, and surface albedo
// (the last two feed the screen-space GI / AO passes).
struct PsOut
{
    float4 color  : SV_Target0;
    float4 normal : SV_Target1;
    float4 albedo : SV_Target2;
};

// PCF against the ortho sun shadow map; 1 = fully lit.
// gScreen.w encodes the mode: 0 = off, 1 = sharp (1 tap), 2 = 3x3, 3 = 5x5.
float SampleShadow(float3 wpos, float ndl)
{
    int mode = int(gScreen.w + 0.5);
    if (mode == 0)
        return 1.0;
    float4 lc = mul(gLightViewProj, float4(wpos, 1.0));
    float2 uv = float2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || lc.z <= 0.0 || lc.z >= 1.0)
        return 1.0;
    // The shadow map holds caster BACKFACES, so the receiver bias must push
    // the compared depth AWAY from the light (z + bias): box bottom faces are
    // coplanar with the ground, and a subtractive bias would flip those
    // contact texels to lit (a bright strip hugging every wall base). The
    // magnitude stays well under the light-depth thickness of the thinnest
    // caster (the barrel) so lit sides never self-shadow.
    float bias = max(0.00008, 0.0003 * (1.0 - ndl));
    float z = lc.z + bias;
    float t = gScreen.z;   // shadow map texel size
    if (mode == 1)
        return gShadowMap.SampleCmpLevelZero(gShadowSampler, uv, z);
    int r = (mode == 2) ? 1 : 2;
    float sum = 0.0;
    [loop]
    for (int y = -r; y <= r; ++y)
        [loop]
        for (int x = -r; x <= r; ++x)
            sum += gShadowMap.SampleCmpLevelZero(gShadowSampler,
                                                 uv + float2(x, y) * t, z);
    float taps = float((2 * r + 1) * (2 * r + 1));
    return sum / taps;
}

// STEALTH: how deeply (world units) the 2D sight line from the LOCAL tank
// to this pixel's world position runs INSIDE occluder boxes. 0 = clear
// sight; the deeper the line is buried, the more hidden the pixel.
float LosPenetration(float2 a, float2 b)
{
    float2 d = b - a;
    float pen = 0.0;
    int n = int(gViewer.w);
    for (int k = 0; k < n; ++k)
    {
        float4 box = gLosBoxes[k];
        float tmin = 0.004, tmax = 0.996;    // exclude only the endpoints
        bool skip = false;
        if (abs(d.x) < 1e-5)
        {
            if (a.x < box.x - box.z || a.x > box.x + box.z) skip = true;
        }
        else
        {
            float t1 = (box.x - box.z - a.x) / d.x;
            float t2 = (box.x + box.z - a.x) / d.x;
            tmin = max(tmin, min(t1, t2));
            tmax = min(tmax, max(t1, t2));
        }
        if (!skip)
        {
            if (abs(d.y) < 1e-5)
            {
                if (a.y < box.y - box.w || a.y > box.y + box.w) skip = true;
            }
            else
            {
                float t1 = (box.y - box.w - a.y) / d.y;
                float t2 = (box.y + box.w - a.y) / d.y;
                tmin = max(tmin, min(t1, t2));
                tmax = min(tmax, max(t1, t2));
            }
        }
        if (!skip && tmin < tmax)
            pen = max(pen, (tmax - tmin) * length(d));
    }
    return pen;
}

// Shadow-map rasterization with the same STEALTH clip: texels whose caster
// point has no line of sight from the local tank never enter the shadow map
// -- so the shadow follows the visible faces exactly, dithered edges and
// all. Normal objects (gMisc2.x = 0) exit immediately: depth-only cost.
void PSShadowClip(VsOut i)
{
    if (gMisc2.x < 0.5)
        return;
    float fade = saturate(LosPenetration(gViewer.xy, i.wpos.xz) / 0.9);
    if (fade >= 1.0)
        discard;
    if (fade > 0.0)
    {
        uint2 px = uint2(i.sv.xy) & 3;
        const float bayer[16] = { 0.0625, 0.5625, 0.1875, 0.6875,
                                  0.8125, 0.3125, 0.9375, 0.4375,
                                  0.25,   0.75,   0.125,  0.625,
                                  1.0,    0.5,    0.875,  0.375 };
        if (fade > bayer[px.y * 4 + px.x] - 0.03)
            discard;
    }
}

PsOut PSMesh(VsOut i)
{
    if (gMisc2.x > 0.5)
    {
        // faces with a clear sight line draw fully; buried faces vanish;
        // the boundary gets a screen-door Bayer dither (CS2-style fade)
        float fade = saturate(LosPenetration(gViewer.xy, i.wpos.xz) / 0.9);
        if (fade >= 1.0)
            discard;
        if (fade > 0.0)
        {
            uint2 px = uint2(i.sv.xy) & 3;
            const float bayer[16] = { 0.0625, 0.5625, 0.1875, 0.6875,
                                      0.8125, 0.3125, 0.9375, 0.4375,
                                      0.25,   0.75,   0.125,  0.625,
                                      1.0,    0.5,    0.875,  0.375 };
            if (fade > bayer[px.y * 4 + px.x] - 0.03)
                discard;
        }
    }
    float3 ng = normalize(i.wnrm);           // geometric normal
    float3 albedo = gAlbedo.Sample(gSampler, i.uv).rgb * gTint.rgb;

    // NRA material map: tangent-space normal in rgb, roughness in a
    float4 nra = gNra.Sample(gSampler, i.uv);
    float rough = max(nra.a, 0.045);
    float3 tn = nra.rgb * 2.0 - 1.0;
    float3 T = i.wtan.xyz - ng * dot(ng, i.wtan.xyz);   // re-orthogonalize
    float tl = length(T);
    float3 n = ng;
    if (tl > 1e-4)
    {
        T /= tl;
        float3 B = cross(ng, T) * i.wtan.w;
        n = normalize(tn.x * T + tn.y * B + tn.z * ng);
    }

    float3 sun = gSunDirAmbient.xyz;
    float ndl = saturate(dot(n, sun));
    float shadow = SampleShadow(i.wpos, ndl);
    float3 v = normalize(gCamPosFog.xyz - i.wpos);
    float3 h = normalize(sun + v);

    // Cook-Torrance GGX, dielectric F0 = 0.04
    float a = rough * rough;
    float a2 = a * a;
    float ndh = saturate(dot(n, h));
    float ndv = max(dot(n, v), 1e-4);
    float denom = ndh * ndh * (a2 - 1.0) + 1.0;
    float D = a2 / max(3.14159 * denom * denom, 1e-5);
    float k = a * 0.5;
    float G = (ndl / (ndl * (1.0 - k) + k + 1e-5))
            * (ndv / (ndv * (1.0 - k) + k + 1e-5));
    float F = 0.04 + 0.96 * pow(1.0 - saturate(dot(h, v)), 5.0);
    float spec = D * G * F / max(4.0 * ndv * ndl, 1e-3) * ndl * shadow;

    // SPLIT LIGHT TEMPERATURE: the sun is a warm body, the fill is cool
    // sky. Lit faces lean golden, shaded faces fall toward blue -- hue
    // contrast on top of value contrast (the orange-teal axis), which is
    // what makes the sun/shade boundary read instead of just dimming.
    const float3 SunColor = float3(1.05, 0.93, 0.76);
    // Hemisphere ambient: cool sky light from above, warm ground bounce below.
    float hemi = n.y * 0.5 + 0.5;
    float3 ambientColor = lerp(float3(1.00, 0.92, 0.78), float3(0.68, 0.79, 1.12), hemi);

    float3 lit = albedo * (gSunDirAmbient.w * ambientColor
                           + SunColor * (ndl * 0.9 * shadow))
               + spec * SunColor;
    lit = lerp(lit, albedo, gTint.a); // emissive-ish flash

    float dist = length(gCamPosFog.xyz - i.wpos);
    float fog = 1.0 - exp(-dist * gCamPosFog.w);
    lit = lerp(lit, FogColor, saturate(fog));

    PsOut o;
    o.color = float4(lit, 1.0);
    // MAPPED normal into the G-buffer: SSGI and SSAO react to the bumps too.
    // normal alpha = static flag: 1 for world geometry, 0 for tanks/projectiles
    o.normal = float4(n * 0.5 + 0.5, gMisc.x > 0.5 ? 0.0 : 1.0);
    o.albedo = float4(albedo, gTint.a);
    return o;
}

// ---------------- UI (screen-space colored triangles) ----------------
struct UiIn
{
    float2 pos : POSITION;   // pixels, origin top-left
    float4 col : COLOR0;
};
struct UiOut
{
    float4 sv  : SV_Position;
    float4 col : COLOR0;
};

UiOut VSUi(UiIn i)
{
    UiOut o;
    float2 ndc = float2(i.pos.x / gScreen.x * 2.0 - 1.0,
                        1.0 - i.pos.y / gScreen.y * 2.0);
    o.sv = float4(ndc, 0.0, 1.0);
    o.col = i.col;
    return o;
}

float4 PSUi(UiOut i) : SV_Target
{
    return i.col;
}

// ---------------- textured UI (icon atlas quads) ----------------
struct UiTexIn
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};
struct UiTexOut
{
    float4 sv  : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

UiTexOut VSUiTex(UiTexIn i)
{
    UiTexOut o;
    o.sv = float4(i.pos.x / gScreen.x * 2.0 - 1.0,
                  1.0 - i.pos.y / gScreen.y * 2.0, 0.0, 1.0);
    o.uv = i.uv;
    o.col = i.col;
    return o;
}

float4 PSUiTex(UiTexOut i) : SV_Target
{
    return gAlbedo.Sample(gSampler, i.uv) * i.col;
}

// ---------------- UI burn dissolve (upgrade purchases) ----------------
// Quads are vertex-pulled from the constant buffer. The pixel shader
// quantizes screen position into chunky cells, offsets each cell's distance
// from the burn origin with a hash, and discards cells inside the growing
// radius -- a pixelated hole eats the card from the click point, rimmed by
// a flickering ember edge over charred pixels.
cbuffer UiBurnCB : register(b1)
{
    float4 gBurnRect[32];    // x, y, w, h (pixels)
    float4 gBurnColor[32];   // rgba of the fragment being burned
    float4 gBurnParam[32];   // origin x, origin y (pixels), progress, max radius
    float4 gBurnUv[32];      // atlas uv rect (u0,v0,u1,v1); u1<=u0 = untextured
    float4 gBurnMisc;        // count, time, cell size, unused
};

struct UiBurnOut
{
    float4 sv  : SV_Position;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD1;
    nointerpolation float4 param : TEXCOORD0;
    nointerpolation float texFlag : TEXCOORD2;
};

UiBurnOut VSUiBurn(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1),
                                float2(0, 1), float2(1, 0), float2(1, 1) };
    float4 rect = gBurnRect[inst];
    float2 pos = rect.xy + corners[vid] * rect.zw;
    UiBurnOut o;
    o.sv = float4(pos.x / gScreen.x * 2.0 - 1.0,
                  1.0 - pos.y / gScreen.y * 2.0, 0.0, 1.0);
    o.col = gBurnColor[inst];
    float4 uvr = gBurnUv[inst];
    o.uv = lerp(uvr.xy, uvr.zw, corners[vid]);
    o.texFlag = (uvr.z > uvr.x) ? 1.0 : 0.0;
    o.param = gBurnParam[inst];
    return o;
}

float BurnHash(float2 p)
{
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

float4 PSUiBurn(UiBurnOut i) : SV_Target
{
    float cell = max(2.0, gBurnMisc.z);
    float2 cellPos = (floor(i.sv.xy / cell) + 0.5) * cell;
    float2 origin = i.param.xy;
    float progress = i.param.z;
    float maxR = i.param.w;

    float h = BurnHash(cellPos);
    float d = length(cellPos - origin) + (h - 0.5) * cell * 3.0;
    float r = progress * maxR;

    if (d < r)
        discard;                       // the hole

    float4 baseCol = i.col;
    if (i.texFlag > 0.5)
        baseCol *= gAlbedo.Sample(gSampler, i.uv);
    if (baseCol.a < 0.02)
        discard;                       // transparent icon texels never burn

    float edge = cell * 4.0;
    float t = saturate((d - r) / edge);
    float3 col = baseCol.rgb;
    if (t < 1.0)
    {
        // char band then glowing ember toward the hole, with per-cell flicker
        float flicker = BurnHash(cellPos + floor(gBurnMisc.y * 24.0));
        float3 emberHot = float3(2.2, 0.9, 0.18) * (0.75 + 0.5 * flicker);
        float3 charDark = float3(0.05, 0.035, 0.03);
        float3 burnCol = (t < 0.35) ? emberHot : charDark;
        col = lerp(burnCol, col, smoothstep(0.35, 1.0, t));
    }
    return float4(col, baseCol.a);
}
