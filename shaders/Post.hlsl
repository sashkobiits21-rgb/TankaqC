// Screen-space post passes shared by the D3D11 and D3D12 backends:
// SSAO (+ depth-aware separable blur), SSGI (screen-space ray-marched single
// bounce, optionally at half resolution), temporal accumulation with camera
// reprojection, and the final composite with bilateral GI upsampling.
//
// Per-pass texture slot assignment (bind before each pass):
//   SSAO:      texA = normals, texB = depth
//   AO blur H: texA = raw AO,  texB = depth        (writes AO temp)
//   AO blur V: texA = AO temp, texB = depth        (writes final AO)
//   SSGI:      texA = scene color, texB = normals, texC = depth
//   Temporal:  texA = fresh GI,    texB = depth,   texC = GI history
//   Composite: texA = scene color, texB = GI accum, texC = AO, texD = albedo,
//              texE = depth

cbuffer PostCB : register(b0)
{
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float4x4 gPrevViewProj;
    float4 gCamPos;        // xyz = camera, w = frame index
    float4 gPrevCamPos;    // xyz = previous-frame camera
    float4 gScreen;        // w, h, 1/w, 1/h (full render resolution)
    float4 gParams;        // x = gi rays, y = temporal samples, z = gi on, w = ao on
    float4 gParams2;       // x = gi intensity, y = ao radius, z = ao strength, w = sky gi
    float4 gParams3;       // giW, giH, 1/giW, 1/giH (GI target resolution)
    float4 gShock[4];      // refraction rings: world x, z, radius, strength
    float4 gShockMeta;     // x = count
};

Texture2D texA : register(t0);
Texture2D texB : register(t1);
Texture2D texC : register(t2);
Texture2D texD : register(t3);
Texture2D texE : register(t4);
SamplerState sPoint  : register(s0);
SamplerState sLinear : register(s1);

static const float3 SkyColor = float3(0.62, 0.72, 0.83);

struct FsOut
{
    float4 sv : SV_Position;
    float2 uv : TEXCOORD0;
};

FsOut VSFullscreen(uint id : SV_VertexID)
{
    FsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.sv = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

float3 WorldFromDepth(float2 uv, float depth)
{
    float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 wp = mul(gInvViewProj, ndc);
    return wp.xyz / wp.w;
}

float Ign(float2 px, float shift)
{
    px += shift * 5.588238;
    return frac(52.9829189 * frac(0.06711056 * px.x + 0.00583715 * px.y));
}

float Hash1(float n)
{
    return frac(sin(n) * 43758.5453123);
}

float3x3 NormalBasis(float3 n)
{
    float3 up = abs(n.y) < 0.98 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);
    return float3x3(t, b, n);
}

float3 CosineSample(float3 n, float u1, float u2)
{
    float r = sqrt(u1);
    float phi = 6.2831853 * u2;
    float3 local = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u1)));
    return normalize(mul(local, NormalBasis(n)));
}

// ------------------------------------------------------------------- SSAO
// Golden-angle spiral over a cosine hemisphere with per-pixel rotation.
// Candidates are filtered on raw NDC depth first; world reconstruction only
// happens for potential occluders.
float4 PSSSAO(FsOut i) : SV_Target
{
    float depth = texB.SampleLevel(sPoint, i.uv, 0).r;
    if (depth >= 1.0)
        return float4(1, 1, 1, 1);

    float3 P = WorldFromDepth(i.uv, depth);
    float3 N = normalize(texA.SampleLevel(sPoint, i.uv, 0).xyz * 2.0 - 1.0);
    float3x3 tbn = NormalBasis(N);
    float2 px = i.uv * gScreen.xy;
    float radius = gParams2.y;
    float rot = Ign(px, 0.0) * 6.2831853;
    float cr = cos(rot), sr = sin(rot);

    const int Samples = 12;
    const float GoldenAngle = 2.3999632;
    float occ = 0.0;

    [loop]
    for (int s = 0; s < Samples; ++s)
    {
        float u1 = (s + 0.5) / Samples;
        float ang = s * GoldenAngle;
        float2 disk = float2(cos(ang), sin(ang));
        disk = float2(disk.x * cr - disk.y * sr, disk.x * sr + disk.y * cr);
        float rd = sqrt(u1);
        float3 dir = normalize(mul(float3(disk * rd, sqrt(max(0.0, 1.0 - u1))), tbn));
        float3 sp = P + N * 0.02 + dir * (radius * sqrt(u1));

        float4 clip = mul(gViewProj, float4(sp, 1.0));
        if (clip.w <= 0.0)
            continue;
        float invW = 1.0 / clip.w;
        float2 suv = float2(clip.x * invW * 0.5 + 0.5, 0.5 - clip.y * invW * 0.5);
        if (any(suv < 0.0) || any(suv > 1.0))
            continue;
        float sd = texB.SampleLevel(sPoint, suv, 0).r;
        if (sd >= clip.z * invW || sd >= 1.0)
            continue;   // nothing in front of the sample point
        // candidate occluder: reconstruct only now
        float surfDist = length(WorldFromDepth(suv, sd) - gCamPos.xyz);
        float spDist = length(sp - gCamPos.xyz);
        float diff = spDist - surfDist;
        if (diff > 0.02)
            occ += saturate(1.0 - diff / radius);
    }
    float ao = 1.0 - (occ / Samples) * gParams2.z;
    return float4(saturate(ao).xxx, 1);
}

// ------------------------------------------- depth-aware separable AO blur
float BlurAO(float2 uv, float2 step)
{
    const float kernel[7] = { 0.106, 0.140, 0.166, 0.176, 0.166, 0.140, 0.106 };
    float centerZ = texB.SampleLevel(sPoint, uv, 0).r;
    float sum = 0.0, wsum = 0.0;
    [unroll]
    for (int k = -3; k <= 3; ++k)
    {
        float2 tuv = uv + step * k;
        float z = texB.SampleLevel(sPoint, tuv, 0).r;
        float w = kernel[k + 3] * exp(-abs(z - centerZ) * 2000.0);
        sum += texA.SampleLevel(sPoint, tuv, 0).r * w;
        wsum += w;
    }
    return sum / max(wsum, 1e-4);
}

float4 PSAOBlurH(FsOut i) : SV_Target
{
    return float4(BlurAO(i.uv, float2(gScreen.z, 0)).xxx, 1);
}

float4 PSAOBlurV(FsOut i) : SV_Target
{
    return float4(BlurAO(i.uv, float2(0, gScreen.w)).xxx, 1);
}

// ------------------------------------------------------------------- SSGI
// Clip coordinates are affine in world position, so the march interpolates
// the projected segment instead of re-projecting every step; the world-space
// thickness test only runs for steps whose NDC depth says "candidate hit".
float4 PSSSGI(FsOut i) : SV_Target
{
    float depth = texC.SampleLevel(sPoint, i.uv, 0).r;
    if (depth >= 1.0)
        return float4(0, 0, 0, 60000.0);

    float3 P = WorldFromDepth(i.uv, depth);
    float3 N = normalize(texB.SampleLevel(sPoint, i.uv, 0).xyz * 2.0 - 1.0);
    float camDist = length(P - gCamPos.xyz);
    float2 px = i.uv * gParams3.xy;
    float frame = gCamPos.w;
    float3 origin = P + N * 0.015;
    float4 originClip = mul(gViewProj, float4(origin, 1.0));

    const float MaxDist = 9.0;
    const int Steps = 10;
    int rays = clamp(int(gParams.x + 0.5), 1, 16);
    float3 accum = 0;

    [loop]
    for (int r = 0; r < rays; ++r)
    {
        float u1 = Ign(px, frac(frame * 0.6180339) + r * 1.5183);
        float u2 = Hash1(dot(px, float2(0.1031, 0.2297)) + r * 13.77 + frame * 0.371);
        float3 dir = CosineSample(N, u1, u2);
        float4 endClip = mul(gViewProj, float4(origin + dir * MaxDist, 1.0));

        bool hit = false;
        float t = 0.14;
        [loop]
        for (int s = 0; s < Steps; ++s)
        {
            float4 clip = lerp(originClip, endClip, t / MaxDist);
            if (clip.w <= 0.0)
                break;
            float invW = 1.0 / clip.w;
            float2 suv = float2(clip.x * invW * 0.5 + 0.5, 0.5 - clip.y * invW * 0.5);
            if (any(suv < 0.0) || any(suv > 1.0))
                break;
            float sd = texC.SampleLevel(sPoint, suv, 0).r;
            if (sd < clip.z * invW && sd < 1.0)
            {
                // candidate: surface is in front of the ray point
                float surfDist = length(WorldFromDepth(suv, sd) - gCamPos.xyz);
                float posDist = length(origin + dir * t - gCamPos.xyz);
                float diff = posDist - surfDist;
                if (diff > 0.02)
                {
                    if (diff < 0.55 + t * 0.35)
                    {
                        float3 hitN = normalize(texB.SampleLevel(sPoint, suv, 0).xyz * 2.0 - 1.0);
                        if (dot(hitN, -dir) > 0.0)
                        {
                            float3 col = texA.SampleLevel(sLinear, suv, 0).rgb;
                            accum += col * (1.0 / (1.0 + t * 0.55));
                            hit = true;
                        }
                    }
                    break;   // the path is blocked either way
                }
            }
            t *= 1.55;
        }
        if (!hit)
            accum += SkyColor * gParams2.w * saturate(dir.y * 0.6 + 0.4);
    }
    return float4(accum / rays, camDist);
}

// -------------------------------------------------- temporal accumulation
float4 PSTemporal(FsOut i) : SV_Target
{
    float4 cur = texA.SampleLevel(sPoint, i.uv, 0);
    float depth = texB.SampleLevel(sPoint, i.uv, 0).r;
    if (depth >= 1.0)
        return cur;

    float3 P = WorldFromDepth(i.uv, depth);
    float4 prevClip = mul(gPrevViewProj, float4(P, 1.0));
    if (prevClip.w <= 0.0)
        return cur;
    float2 puv = float2(prevClip.x / prevClip.w * 0.5 + 0.5,
                        0.5 - prevClip.y / prevClip.w * 0.5);
    if (any(puv < 0.0) || any(puv > 1.0))
        return cur;

    float4 hist = texC.SampleLevel(sLinear, puv, 0);
    // clamp scrubs NaN/Inf (D3D min/max return the non-NaN operand), so bad
    // history can never poison the accumulator
    hist.rgb = clamp(hist.rgb, 0.0, 64.0);
    // History stores each texel's distance to the camera that wrote it; a static
    // world point predicts that distance exactly, so a mismatch = disocclusion.
    float expected = length(P - gPrevCamPos.xyz);
    bool valid = abs(hist.a - expected) < 0.35 + expected * 0.02;

    float n = clamp(gParams.y, 2.0, 16.0);
    float a = valid ? 1.0 / n : 0.85;
    // sparkle limiter: a single frame may only move converged history so far,
    // which stops per-frame ray noise from shimmering the whole image
    if (valid)
        cur.rgb = clamp(cur.rgb, hist.rgb - 0.30, hist.rgb + 0.30);
    return float4(lerp(hist.rgb, cur.rgb, a), cur.a);
}

// ---------------------------------------------------------------- composite
float4 PSComposite(FsOut i) : SV_Target
{
    float3 scene = texA.SampleLevel(sPoint, i.uv, 0).rgb;

    float ao = 1.0;
    if (gParams.w > 0.5)
        ao = texC.SampleLevel(sLinear, i.uv, 0).r;

    float3 outc = scene * lerp(1.0, ao, 0.92);
    if (gParams.z > 0.5)
    {
        float3 gi;
        if (gParams3.x >= gScreen.x - 0.5)
        {
            gi = texB.SampleLevel(sLinear, i.uv, 0).rgb;
        }
        else
        {
            // bilateral upsample from the half-res GI accumulator; GI alpha
            // stores each texel's camera distance, so weight by how well it
            // matches this pixel's own distance to avoid silhouette bleeding
            float depth = texE.SampleLevel(sPoint, i.uv, 0).r;
            float distF = (depth < 1.0)
                ? length(WorldFromDepth(i.uv, depth) - gCamPos.xyz) : 60000.0;
            float2 pos = i.uv * gParams3.xy - 0.5;
            float2 base = floor(pos);
            float2 f = pos - base;
            float wsum = 0.0;
            float3 sum = 0.0;
            [unroll]
            for (int k = 0; k < 4; ++k)
            {
                float2 off = float2(k & 1, k >> 1);
                float2 tuv = (base + off + 0.5) * gParams3.zw;
                float4 tap = texB.SampleLevel(sPoint, tuv, 0);
                float wb = (off.x > 0 ? f.x : 1 - f.x) * (off.y > 0 ? f.y : 1 - f.y);
                float w = wb / (0.01 + abs(tap.a - distF));
                sum += clamp(tap.rgb, 0.0, 64.0) * w;
                wsum += w;
            }
            gi = sum / max(wsum, 1e-4);
        }
        float3 albedo = texD.SampleLevel(sPoint, i.uv, 0).rgb;
        outc += gi * albedo * gParams2.x * ao;
    }

    // FILMIC FINISH: exposure + the ACES fit (Narkowicz). The scene target
    // is small-float now, so highlights arrive unclipped and ROLL OFF here
    // instead of slamming into white; mids keep their contrast.
    const float Exposure = 1.05;
    float luma = dot(outc, float3(0.299, 0.587, 0.114));
    outc = max(0.0, lerp(luma.xxx, outc, 1.06));   // mild pre-grade sat
    float3 x = outc * Exposure;
    outc = saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
    // display-space grade: a saturation push + a gentle contrast S around
    // the mid -- pennies of ALU in a pass that already touches every pixel
    float l2 = dot(outc, float3(0.299, 0.587, 0.114));
    outc = saturate(lerp(l2.xxx, outc, 1.22));
    outc = saturate((outc - 0.46) * 1.08 + 0.46);
    return float4(outc, 1.0);
}

// ------------------------------------------------------------- edge AA
// Contrast-adaptive tent smoothing. Deliberately direction-free: a gradient-
// steered blur wobbles when per-frame GI/AO noise perturbs the gradient, so
// high-contrast pixels instead blend toward a fixed symmetric neighborhood.
// Kills the stair-step crawl on edges while leaving flat areas sharp.
float4 PSAA(FsOut i) : SV_Target
{
    float2 px = gScreen.zw;
    float2 uv = i.uv;

    // SHOCKWAVE REFRACTION: each blast is an invisible expanding glass
    // ring. Project its world center + radius into screen space; pixels in
    // a band around the ring sample the frame from a radially displaced
    // position (light bending), and the band also boosts the tent blur so
    // the rim smears glassily in screen space.
    float blurBoost = 0.0;
    float aspect = gScreen.x / gScreen.y;
    for (int sIdx = 0; sIdx < int(gShockMeta.x); ++sIdx)
    {
        float4 sw = gShock[sIdx];
        float4 cp = mul(float4(sw.x, 0.1, sw.y, 1.0), gViewProj);
        if (cp.w <= 0.01) continue;
        float2 css = cp.xy / cp.w * float2(0.5, -0.5) + 0.5;
        float4 rp = mul(float4(sw.x + sw.z, 0.1, sw.y, 1.0), gViewProj);
        float2 rss = rp.xy / rp.w * float2(0.5, -0.5) + 0.5;
        float radSS = length((rss - css) * float2(aspect, 1.0));
        float2 d = (uv - css) * float2(aspect, 1.0);
        float dist = length(d);
        float band = max(radSS, 0.001) * 0.12 + 0.012;
        float x = (dist - radSS) / band;
        if (abs(x) > 1.6) continue;
        float prof = exp(-x * x * 2.0);
        float2 dir = dist > 1e-4 ? d / dist : float2(0, 0);
        dir.x /= aspect;
        uv -= dir * prof * sw.w * 0.016;    // pull inward: a lens front
        blurBoost = max(blurBoost, prof * sw.w);
    }

    float3 c = texA.SampleLevel(sLinear, uv, 0).rgb;
    float3 n = texA.SampleLevel(sLinear, uv + float2(0, -px.y), 0).rgb;
    float3 s = texA.SampleLevel(sLinear, uv + float2(0, px.y), 0).rgb;
    float3 e = texA.SampleLevel(sLinear, uv + float2(px.x, 0), 0).rgb;
    float3 w = texA.SampleLevel(sLinear, uv + float2(-px.x, 0), 0).rgb;

    const float3 L = float3(0.299, 0.587, 0.114);
    float lC = dot(c, L), lN = dot(n, L), lS = dot(s, L), lE = dot(e, L), lW = dot(w, L);
    float lMin = min(lC, min(min(lN, lS), min(lE, lW)));
    float lMax = max(lC, max(max(lN, lS), max(lE, lW)));
    float contrast = lMax - lMin;
    if (contrast < 0.06 && blurBoost < 0.05)
        return float4(c, 1.0);

    float3 tent = (n + s + e + w) * 0.25;
    float blend = saturate(contrast * 5.0 - 0.2) * 0.62;
    blend = max(blend, saturate(blurBoost) * 0.85);   // glassy smeared rim
    return float4(lerp(c, tent, blend), 1.0);
}
