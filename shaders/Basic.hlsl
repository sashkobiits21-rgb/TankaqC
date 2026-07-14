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
    float bias = max(0.0005, 0.0018 * (1.0 - ndl));
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
