// Shared forward shaders for both D3D11 and D3D12 backends (SM 5.0, compiled at runtime).

cbuffer PerFrame : register(b0)
{
    float4x4 gViewProj;
    float4x4 gLightViewProj;   // ortho sun camera (shadow map)
    float4   gSunDirAmbient;   // xyz = to-sun direction (normalized), w = ambient
    float4   gCamPosFog;       // xyz = camera position, w = fog density
    float4   gScreen;          // xy = viewport pixels, z = shadow texel, w = shadows on
};

cbuffer PerObject : register(b1)
{
    float4x4 gWorld;
    float4   gTint;            // rgb multiplied over albedo, a = emissive amount
    float4   gMisc;            // x = 1 for dynamic objects (burn decals skip them)
};

Texture2D    gAlbedo  : register(t0);
Texture2D    gShadowMap : register(t1);
SamplerState gSampler : register(s0);
SamplerComparisonState gShadowSampler : register(s1);

static const float3 FogColor = float3(0.62, 0.72, 0.83);

// ---------------- mesh ----------------
struct VsIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD0;
};
struct VsOut
{
    float4 sv   : SV_Position;
    float3 wpos : TEXCOORD1;
    float3 wnrm : TEXCOORD2;
    float2 uv   : TEXCOORD0;
};

VsOut VSMesh(VsIn i)
{
    VsOut o;
    float4 wp = mul(gWorld, float4(i.pos, 1.0));
    o.wpos = wp.xyz;
    o.sv = mul(gViewProj, wp);
    o.wnrm = mul((float3x3)gWorld, i.nrm);
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
    // tiny bias: the shadow map holds caster BACKFACES, so depth error pushes
    // into the caster's interior rather than detaching contact shadows
    float bias = max(0.00008, 0.0003 * (1.0 - ndl));
    float z = lc.z - bias;
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

PsOut PSMesh(VsOut i)
{
    float3 n = normalize(i.wnrm);
    float3 albedo = gAlbedo.Sample(gSampler, i.uv).rgb * gTint.rgb;

    float3 sun = gSunDirAmbient.xyz;
    float ndl = saturate(dot(n, sun));
    float shadow = SampleShadow(i.wpos, ndl);
    float3 v = normalize(gCamPosFog.xyz - i.wpos);
    float3 h = normalize(sun + v);
    float spec = pow(saturate(dot(n, h)), 32.0) * 0.25 * ndl * shadow;

    // Hemisphere ambient: cool sky light from above, warm ground bounce below.
    float hemi = n.y * 0.5 + 0.5;
    float3 ambientColor = lerp(float3(1.04, 0.96, 0.82), float3(0.82, 0.92, 1.10), hemi);

    float3 lit = albedo * (gSunDirAmbient.w * ambientColor + ndl * 0.9 * shadow) + spec;
    lit = lerp(lit, albedo, gTint.a); // emissive-ish flash

    float dist = length(gCamPosFog.xyz - i.wpos);
    float fog = 1.0 - exp(-dist * gCamPosFog.w);
    lit = lerp(lit, FogColor, saturate(fog));

    PsOut o;
    o.color = float4(lit, 1.0);
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
