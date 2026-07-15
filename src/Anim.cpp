#include "Anim.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace tankaq
{

static void SampleChannel(const AnimChannel& ch, float t, XMFLOAT4& out)
{
    const size_t n = ch.times.size();
    if (n == 0)
        return;
    if (t <= ch.times.front()) { out = ch.values.front(); return; }
    if (t >= ch.times.back()) { out = ch.values.back(); return; }
    // binary search for the segment
    size_t lo = 0, hi = n - 1;
    while (hi - lo > 1)
    {
        size_t mid = (lo + hi) / 2;
        if (ch.times[mid] <= t) lo = mid;
        else hi = mid;
    }
    float span = ch.times[hi] - ch.times[lo];
    float a = span > 1e-6f ? (t - ch.times[lo]) / span : 0.0f;
    const XMFLOAT4& v0 = ch.values[lo];
    const XMFLOAT4& v1 = ch.values[hi];
    if (ch.path == 1)
    {
        // quaternion slerp (shortest path)
        XMVECTOR q0 = XMLoadFloat4(&v0);
        XMVECTOR q1 = XMLoadFloat4(&v1);
        XMStoreFloat4(&out, XMQuaternionSlerp(q0, q1, a));
    }
    else
    {
        out = XMFLOAT4(v0.x + (v1.x - v0.x) * a,
                       v0.y + (v1.y - v0.y) * a,
                       v0.z + (v1.z - v0.z) * a, 1.0f);
    }
}

void SampleClip(const SkinnedModel& model, int clip, float t, bool loop,
                std::vector<JointPose>& out)
{
    out.resize(model.joints.size());
    for (size_t j = 0; j < model.joints.size(); ++j)
    {
        out[j].t = model.joints[j].restT;
        out[j].r = model.joints[j].restR;
        out[j].s = model.joints[j].restS;
    }
    if (clip < 0 || clip >= int(model.clips.size()))
        return;
    const AnimClip& c = model.clips[clip];
    if (c.duration > 1e-5f)
        t = loop ? fmodf(t, c.duration) : std::min(t, c.duration);
    for (const AnimChannel& ch : c.channels)
    {
        if (ch.joint < 0 || ch.joint >= int(out.size()))
            continue;
        XMFLOAT4 v{};
        SampleChannel(ch, t, v);
        if (ch.path == 0) out[ch.joint].t = XMFLOAT3(v.x, v.y, v.z);
        else if (ch.path == 1) out[ch.joint].r = v;
        else out[ch.joint].s = XMFLOAT3(v.x, v.y, v.z);
    }
}

void BlendPoses(const std::vector<JointPose>& a, const std::vector<JointPose>& b,
                float alpha, std::vector<JointPose>& out)
{
    size_t n = std::min(a.size(), b.size());
    out.resize(n);
    for (size_t j = 0; j < n; ++j)
    {
        out[j].t = XMFLOAT3(a[j].t.x + (b[j].t.x - a[j].t.x) * alpha,
                            a[j].t.y + (b[j].t.y - a[j].t.y) * alpha,
                            a[j].t.z + (b[j].t.z - a[j].t.z) * alpha);
        out[j].s = XMFLOAT3(a[j].s.x + (b[j].s.x - a[j].s.x) * alpha,
                            a[j].s.y + (b[j].s.y - a[j].s.y) * alpha,
                            a[j].s.z + (b[j].s.z - a[j].s.z) * alpha);
        XMVECTOR q0 = XMLoadFloat4(&a[j].r);
        XMVECTOR q1 = XMLoadFloat4(&b[j].r);
        XMStoreFloat4(&out[j].r, XMQuaternionSlerp(q0, q1, alpha));
    }
}

void ComposePalette(const SkinnedModel& model,
                    const std::vector<JointPose>& locals, BonePalette& out)
{
    XMMATRIX global[MaxBones];
    XMMATRIX rootPre = XMLoadFloat4x4(&model.rootTransform);
    size_t n = std::min<size_t>(model.joints.size(), MaxBones);
    for (size_t j = 0; j < n; ++j)
    {
        const JointPose& p = locals[j];
        XMMATRIX local = XMMatrixScaling(p.s.x, p.s.y, p.s.z)
                       * XMMatrixRotationQuaternion(XMLoadFloat4(&p.r))
                       * XMMatrixTranslation(p.t.x, p.t.y, p.t.z);
        int par = model.joints[j].parent;
        // joints are ordered parents-first, so par < j when valid; parentless
        // joints hang off the composed non-joint ancestor chain
        global[j] = (par >= 0 && par < int(j))
                        ? local * global[par] : local * rootPre;
        XMMATRIX inv = XMLoadFloat4x4(&model.joints[j].inverseBind);
        XMStoreFloat4x4(&out.m[j], inv * global[j]);
    }
    out.count = int(n);
}

void AnimPlayer::Play(int newClip, bool restart)
{
    if (newClip == clip && !restart)
        return;
    prevClip = clip;
    prevTime = time;
    fade = 0.0f;
    clip = newClip;
    time = 0.0f;
}

void AnimPlayer::Update(float dt)
{
    time += dt;
    prevTime += dt;
    fade = std::min(1.0f, fade + fadeSpeed * dt);
}

void AnimPlayer::Pose(BonePalette& out) const
{
    if (!model)
        return;
    SampleClip(*model, clip, time, true, m_a);
    if (prevClip >= 0 && fade < 1.0f)
    {
        SampleClip(*model, prevClip, prevTime, true, m_b);
        BlendPoses(m_b, m_a, fade, m_a);
    }
    ComposePalette(*model, m_a, out);
}

} // namespace tankaq
