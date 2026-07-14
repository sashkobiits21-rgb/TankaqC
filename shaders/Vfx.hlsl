// Stateless GPU VFX shared by both backends.
//
// Smoke bursts: billboard particles whose motion is a pure function of
// (burst, particle, age) evaluated in the vertex shader each frame. The VS
// collides each particle against the G-buffer (depth + normals): if the
// analytic position penetrates the surface plane it is pushed out along the
// stored normal, so smoke slides around walls instead of clipping them. The
// pixel shader depth-fades near geometry (soft particles).
//
// Scorch marks: a fullscreen deferred-decal pass reconstructs world position
// from depth and darkens BOTH the lit scene color and the albedo G-buffer, so
// the SSGI pass bounces less light off burnt ground/walls.
//
// Texture slots: texVfxA = depth, texVfxB = normals.

cbuffer VfxCB : register(b0)
{
    float4x4 gVfxViewProj;
    float4x4 gVfxInvViewProj;
    float4 gVfxCamRight;    // xyz
    float4 gVfxCamUp;       // xyz
    float4 gVfxCamPos;      // xyz
    float4 gVfxScreenTime;  // w, h, time, unused
    float4 gVfxCounts;      // x = bursts, y = scorches
    float4 gBursts[16];     // xyz pos, w age
    float4 gScorches[16];   // xyz pos, w age
};

Texture2D texVfxA : register(t0);   // depth
Texture2D texVfxB : register(t1);   // packed normals
SamplerState sVfxPoint : register(s0);

static const int ParticlesPerBurst = 16;   // 0 = fireball, 1..15 = smoke
static const float SmokeLife = 2.6;

float VHash(float2 p)
{
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float VNoise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(VHash(i), VHash(i + float2(1, 0)), f.x),
                lerp(VHash(i + float2(0, 1)), VHash(i + 1.0), f.x), f.y);
}

float3 VfxWorldFromDepth(float2 uv, float depth)
{
    float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 wp = mul(gVfxInvViewProj, ndc);
    return wp.xyz / wp.w;
}

// ------------------------------------------------------------ smoke billboards
struct VfxOut
{
    float4 sv : SV_Position;
    float2 uv : TEXCOORD0;      // [-1,1] billboard coords
    float4 color : COLOR0;      // rgb tint, a alpha
    float2 misc : TEXCOORD1;    // x = camera distance, y = per-particle seed
};

VfxOut VSVfx(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
    const float2 corners[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1),
                                float2(-1, 1),  float2(1, -1), float2(1, 1) };
    float2 corner = corners[vid];

    uint burst = inst / ParticlesPerBurst;
    uint pIdx = inst % ParticlesPerBurst;
    float4 b = gBursts[burst];
    float3 origin = b.xyz;
    float age = b.w;

    float seed = VHash(float2(float(burst) * 3.17 + 0.31, float(pIdx) * 7.89 + 0.77));
    float h1 = VHash(seed.xx * 17.0 + 1.3);
    float h2 = VHash(seed.xx * 29.0 + 5.9);
    float h3 = VHash(seed.xx * 43.0 + 9.2);
    float h4 = VHash(seed.xx * 61.0 + 3.4);

    float3 center = origin;
    float size = 0.0;
    float alpha = 0.0;
    float3 color = 1.0;

    if (pIdx == 0)
    {
        // fireball core: fast, bright, brief
        float t = age;
        if (t < 0.4)
        {
            size = 0.5 + 3.0 * saturate(t / 0.22);
            alpha = 0.9 * (1.0 - t / 0.4);
            color = float3(2.3, 1.35, 0.45);
            center = origin + float3(0, 0.3 + t * 1.2, 0);
        }
    }
    else
    {
        // smoke puff
        float stagger = 0.14 * h4;
        float t = age - stagger;
        if (t > 0.0 && t < SmokeLife)
        {
            float ang = h1 * 6.2831853;
            float upBias = 0.2 + 0.75 * h2;
            float3 dir = normalize(float3(cos(ang) * (1.0 - upBias * 0.7), upBias,
                                          sin(ang) * (1.0 - upBias * 0.7)));
            float range = 1.1 + 1.7 * h3;
            center = origin + dir * range * (1.0 - exp(-t * 1.9))
                   + float3(0, 0.55 * t, 0);
            size = (0.55 + 1.5 * (1.0 - exp(-t * 1.4))) * (0.75 + 0.5 * h2);
            alpha = smoothstep(0.0, 0.12, t) * saturate(1.0 - t / SmokeLife) * 0.62;
            float warm = saturate(1.0 - t * 2.6);
            color = lerp(float3(0.16, 0.155, 0.15) * (0.8 + 0.5 * h1),
                         float3(1.6, 0.75, 0.3), warm * warm);
        }
    }

    // ---- G-buffer collision: push the particle out of the surface plane ----
    if (alpha > 0.0)
    {
        float4 pc = mul(gVfxViewProj, float4(center, 1.0));
        if (pc.w > 0.0)
        {
            float2 suv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
            if (all(suv >= 0.0) && all(suv <= 1.0))
            {
                float sd = texVfxA.SampleLevel(sVfxPoint, suv, 0).r;
                if (sd < 1.0)
                {
                    float3 surf = VfxWorldFromDepth(suv, sd);
                    float3 srfN = normalize(texVfxB.SampleLevel(sVfxPoint, suv, 0).xyz * 2.0 - 1.0);
                    float clearance = size * 0.8;
                    float pen = dot(center - surf, srfN);
                    if (pen < clearance && pen > -size * 2.0)
                        center += srfN * min(clearance - pen, size * 1.2);
                }
            }
        }
    }

    float3 wpos = center + (gVfxCamRight.xyz * corner.x + gVfxCamUp.xyz * corner.y) * size;

    VfxOut o;
    o.sv = mul(gVfxViewProj, float4(wpos, 1.0));
    o.uv = corner;
    o.color = float4(color, alpha);
    o.misc = float2(length(center - gVfxCamPos.xyz), seed * 37.0);
    return o;
}

float4 PSVfx(VfxOut i) : SV_Target
{
    float r = length(i.uv);
    float base = smoothstep(0.95, 0.2, r);
    float t = gVfxScreenTime.z;
    float n = VNoise(i.uv * 2.6 + i.misc.y + float2(0, -t * 0.4)) * 0.6
            + VNoise(i.uv * 5.3 + i.misc.y * 1.7) * 0.4;
    float alpha = i.color.a * saturate(base * (0.5 + 0.65 * n) * 1.25);
    // hard guarantee: zero alpha at the quad border so its rectangle never shows
    alpha *= smoothstep(1.0, 0.82, r);

    // soft particles: fully faded where the billboard reaches scene geometry,
    // so the quad never shows a hard intersection line
    float2 suv = i.sv.xy / gVfxScreenTime.xy;
    float sd = texVfxA.SampleLevel(sVfxPoint, suv, 0).r;
    if (sd < 1.0)
    {
        float surfDist = length(VfxWorldFromDepth(suv, sd) - gVfxCamPos.xyz);
        alpha *= saturate((surfDist - i.misc.x) / 1.4);
    }

    // slight top-lighting on the puff
    float3 col = i.color.rgb * (0.9 + 0.25 * saturate(i.uv.y + n - 0.4));
    return float4(col, alpha);
}

// --------------------------------------------------------------- scorch decals
struct VfxFsOut
{
    float4 sv : SV_Position;
    float2 uv : TEXCOORD0;
};

VfxFsOut VSVfxFull(uint id : SV_VertexID)
{
    VfxFsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.sv = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

struct ScorchOut
{
    float4 color : SV_Target0;   // multiplies lit scene color
    float4 albedo : SV_Target1;  // multiplies albedo so GI bounce dims too
};

ScorchOut PSScorch(VfxFsOut i)
{
    ScorchOut o;
    float depth = texVfxA.SampleLevel(sVfxPoint, i.uv, 0).r;
    float4 nPacked = texVfxB.SampleLevel(sVfxPoint, i.uv, 0);
    float factor = 1.0;
    // Burns only stick to static world geometry: dynamic objects (tanks,
    // projectiles) tag normal.a = 0 in the G-buffer and are skipped here.
    if (depth < 1.0 && nPacked.a > 0.5)
    {
        float3 w = VfxWorldFromDepth(i.uv, depth);
        float3 N = normalize(nPacked.xyz * 2.0 - 1.0);
        int count = int(gVfxCounts.y + 0.5);
        [loop]
        for (int s = 0; s < count; ++s)
        {
            float4 sc = gScorches[s];
            const float R = 2.4;
            float d = length(w - sc.xyz);
            if (d >= R)
                continue;
            // only surfaces facing the blast keep the burn
            float facing = dot(N, normalize(sc.xyz - w + float3(0, 0.35, 0)));
            if (facing < -0.15)
                continue;
            float fade = 1.0 - saturate((sc.w - 24.0) / 6.0);   // 30 s lifetime
            float n = VNoise(w.xz * 2.1 + sc.x * 3.7) * 0.55
                    + VNoise(w.xz * 5.7 + sc.z * 1.9) * 0.45;
            float burn = (1.0 - smoothstep(R * 0.25, R, d))
                       * saturate(0.35 + 0.85 * n) * fade;
            factor *= 1.0 - 0.82 * saturate(burn);
        }
    }
    o.color = float4(factor.xxx, 1.0);
    o.albedo = float4(factor.xxx, 1.0);
    return o;
}
