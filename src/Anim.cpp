#include "Anim.h"
#include "Log.h"
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

// --------------------------------------------------------------- full system

int FindJoint(const SkinnedModel& model, const char* nameSubstr)
{
    // joint names aren't stored; match through the clips' channel targets is
    // impossible -- so SkinJoint keeps no name. Use the loader-stored names.
    for (size_t j = 0; j < model.jointNames.size(); ++j)
        if (model.jointNames[j].find(nameSubstr) != std::string::npos)
            return int(j);
    return -1;
}

JointMask MaskSubtree(const SkinnedModel& model, const char* rootJointName,
                      float inW, float outW)
{
    JointMask m(model.joints.size(), outW);
    int root = FindJoint(model, rootJointName);
    if (root < 0)
    {
        // a silent all-outW mask would make the layer invisibly wrong
        Log("Anim: MaskSubtree root joint '%s' not in skeleton (%zu joints)",
            rootJointName, model.joints.size());
        return m;
    }
    for (size_t j = 0; j < model.joints.size(); ++j)
    {
        int p = int(j);
        while (p >= 0)
        {
            if (p == root) { m[j] = inW; break; }
            p = model.joints[p].parent;
        }
    }
    return m;
}

static void ComposeGlobals(const SkinnedModel& model,
                           const std::vector<JointPose>& locals,
                           XMMATRIX* globals)
{
    XMMATRIX rootPre = XMLoadFloat4x4(&model.rootTransform);
    size_t n = std::min<size_t>(model.joints.size(), MaxBones);
    for (size_t j = 0; j < n; ++j)
    {
        const JointPose& p = locals[j];
        XMMATRIX local = XMMatrixScaling(p.s.x, p.s.y, p.s.z)
                       * XMMatrixRotationQuaternion(XMLoadFloat4(&p.r))
                       * XMMatrixTranslation(p.t.x, p.t.y, p.t.z);
        int par = model.joints[j].parent;
        globals[j] = (par >= 0 && par < int(j)) ? local * globals[par]
                                                : local * rootPre;
    }
}

// Rotate joint `j`'s GLOBAL orientation by `dq` (model space) and write the
// result back into its LOCAL rotation, leaving translation/scale untouched.
static void RotateJointGlobal(const SkinnedModel& model,
                              std::vector<JointPose>& locals,
                              const XMMATRIX* globals, int j, XMVECTOR dq,
                              float weight)
{
    if (weight <= 0.0001f)
        return;
    dq = XMQuaternionSlerp(XMQuaternionIdentity(), dq, weight);
    XMVECTOR gq = XMQuaternionRotationMatrix(globals[j]);
    XMVECTOR newGq = XMQuaternionMultiply(gq, dq);   // row-vector: gq then dq
    int par = model.joints[j].parent;
    XMVECTOR parentQ = par >= 0
        ? XMQuaternionRotationMatrix(globals[par])
        : XMQuaternionRotationMatrix(XMLoadFloat4x4(&model.rootTransform));
    // local = global * inverse(parent)  (rotation part, row-vector order)
    XMVECTOR newLocal =
        XMQuaternionMultiply(newGq, XMQuaternionInverse(parentQ));
    XMStoreFloat4(&locals[j].r, XMQuaternionNormalize(newLocal));
}

void Animator::ApplyAim(std::vector<JointPose>& locals)
{
    if (!aim.active || aim.chainCount <= 0 || aim.weight <= 0.001f)
        return;
    XMMATRIX globals[MaxBones];
    ComposeGlobals(*model, locals, globals);

    // model-space direction to target from the LAST chain joint's position
    int endJ = aim.chain[aim.chainCount - 1];
    if (endJ < 0)
        return;
    XMVECTOR jointPos = globals[endJ].r[3];
    XMVECTOR dir = XMVectorSubtract(XMLoadFloat3(&aim.target), jointPos);
    XMVECTOR fwd = XMLoadFloat3(&aim.forward);
    if (aim.yawOnly)
    {
        dir = XMVectorSetY(dir, 0);
        fwd = XMVectorSetY(fwd, 0);
    }
    if (XMVectorGetX(XMVector3LengthSq(dir)) < 1e-6f)
        return;
    dir = XMVector3Normalize(dir);
    fwd = XMVector3Normalize(fwd);

    // shortest-arc rotation forward -> dir, clamped
    float cosA = std::clamp(XMVectorGetX(XMVector3Dot(fwd, dir)), -1.0f, 1.0f);
    float ang = acosf(cosA);
    if (ang < 0.002f)
        return;
    XMVECTOR axis = XMVector3Cross(fwd, dir);
    if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-8f)
        axis = XMVectorSet(0, 1, 0, 0);
    axis = XMVector3Normalize(axis);
    float total = std::min(ang, aim.maxAngle) * aim.weight;

    // distribute across the chain, re-solving globals as we go
    float frac = 1.0f / float(aim.chainCount);
    for (int c = 0; c < aim.chainCount; ++c)
    {
        int j = aim.chain[c];
        if (j < 0)
            continue;
        XMVECTOR dq = XMQuaternionRotationAxis(axis, total * frac);
        RotateJointGlobal(*model, locals, globals, j, dq, 1.0f);
        ComposeGlobals(*model, locals, globals);   // 64 joints: cheap
    }
}

void Animator::ApplyIK(const TwoBoneIK& k, std::vector<JointPose>& locals)
{
    if (!k.active || k.weight <= 0.001f || k.a < 0 || k.b < 0 || k.c < 0)
        return;
    XMMATRIX globals[MaxBones];
    ComposeGlobals(*model, locals, globals);

    XMVECTOR pa = globals[k.a].r[3];
    XMVECTOR pb = globals[k.b].r[3];
    XMVECTOR pc = globals[k.c].r[3];
    XMVECTOR t = XMLoadFloat3(&k.target);
    float la = XMVectorGetX(XMVector3Length(XMVectorSubtract(pb, pa)));
    float lb = XMVectorGetX(XMVector3Length(XMVectorSubtract(pc, pb)));
    if (la < 1e-5f || lb < 1e-5f)
        return;
    XMVECTOR toT = XMVectorSubtract(t, pa);
    float d = XMVectorGetX(XMVector3Length(toT));
    d = std::clamp(d, fabsf(la - lb) + 1e-3f, la + lb - 1e-3f);
    XMVECTOR dir = XMVector3Normalize(toT);

    // bend plane normal from the pole hint
    XMVECTOR pole = XMVectorSubtract(XMLoadFloat3(&k.pole), pa);
    XMVECTOR n = XMVector3Cross(dir, pole);
    if (XMVectorGetX(XMVector3LengthSq(n)) < 1e-8f)
        n = XMVector3Cross(dir, XMVectorSet(0, 1, 0, 0));
    n = XMVector3Normalize(n);

    // law of cosines: angle at the root between dir and the upper bone
    float cosRoot = std::clamp((la * la + d * d - lb * lb) / (2.0f * la * d),
                               -1.0f, 1.0f);
    float rootAng = acosf(cosRoot);
    XMVECTOR bendRot = XMQuaternionRotationAxis(n, rootAng);
    XMVECTOR mid = XMVectorAdd(pa,
        XMVectorScale(XMVector3Rotate(dir, bendRot), la));

    // rotate upper bone: (pb - pa) -> (mid - pa)
    auto shortestArc = [](XMVECTOR from, XMVECTOR to) -> XMVECTOR
    {
        from = XMVector3Normalize(from);
        to = XMVector3Normalize(to);
        float c = std::clamp(XMVectorGetX(XMVector3Dot(from, to)), -1.0f, 1.0f);
        XMVECTOR ax = XMVector3Cross(from, to);
        if (XMVectorGetX(XMVector3LengthSq(ax)) < 1e-9f)
            return XMQuaternionIdentity();
        return XMQuaternionRotationAxis(XMVector3Normalize(ax), acosf(c));
    };

    XMVECTOR dqA = shortestArc(XMVectorSubtract(pb, pa),
                               XMVectorSubtract(mid, pa));
    RotateJointGlobal(*model, locals, globals, k.a, dqA, k.weight);
    ComposeGlobals(*model, locals, globals);

    // rotate lower bone: (pc' - pb') -> (t - pb')
    XMVECTOR pb2 = globals[k.b].r[3];
    XMVECTOR pc2 = globals[k.c].r[3];
    XMVECTOR dqB = shortestArc(XMVectorSubtract(pc2, pb2),
                               XMVectorSubtract(t, pb2));
    RotateJointGlobal(*model, locals, globals, k.b, dqB, k.weight);
}

void Animator::PlayLayer(int layer, int clip, bool loop, float targetWeight,
                         bool restart)
{
    if (layer < 0 || layer >= MaxLayers)
        return;
    Layer& l = layers[layer];
    // Reject bad clip handles loudly: a -1 from a failed FindClip silently
    // playing clip 0 (or indexing out of range) is the classic
    // wrong-animation-no-error bug after a model re-export.
    if (clip < 0 || (model && clip >= int(model->clips.size())))
    {
        Log("Anim: PlayLayer(%d) invalid clip %d (model has %zu clips)",
            layer, clip, model ? model->clips.size() : size_t(0));
        l.clip = -1;
        l.targetWeight = 0;
        return;
    }
    if (l.clip != clip || restart)
        l.time = 0;
    l.clip = clip;
    l.loop = loop;
    l.targetWeight = targetWeight;
}

void Animator::Update(float dt)
{
    for (Layer& l : layers)
    {
        if (l.clip < 0)
            continue;
        l.time += dt * l.rate;
        float k = 1.0f - expf(-dt * l.fadeSpeed);
        l.weight += (l.targetWeight - l.weight) * k;
    }
}

void Animator::Pose(BonePalette& out)
{
    if (!model)
        return;
    // base: rest pose (layer 0 normally overrides everything)
    SampleClip(*model, -1, 0, false, m_pose);
    for (const Layer& l : layers)
    {
        if (l.clip < 0 || l.weight <= 0.003f)
            continue;
        SampleClip(*model, l.clip, l.time, l.loop, m_tmp);
        size_t n = std::min(m_pose.size(), m_tmp.size());
        for (size_t j = 0; j < n; ++j)
        {
            float w = l.weight * (l.mask.empty() ? 1.0f
                                                 : l.mask[std::min(j, l.mask.size() - 1)]);
            if (w <= 0.003f)
                continue;
            m_pose[j].t = XMFLOAT3(
                m_pose[j].t.x + (m_tmp[j].t.x - m_pose[j].t.x) * w,
                m_pose[j].t.y + (m_tmp[j].t.y - m_pose[j].t.y) * w,
                m_pose[j].t.z + (m_tmp[j].t.z - m_pose[j].t.z) * w);
            m_pose[j].s = XMFLOAT3(
                m_pose[j].s.x + (m_tmp[j].s.x - m_pose[j].s.x) * w,
                m_pose[j].s.y + (m_tmp[j].s.y - m_pose[j].s.y) * w,
                m_pose[j].s.z + (m_tmp[j].s.z - m_pose[j].s.z) * w);
            XMVECTOR q0 = XMLoadFloat4(&m_pose[j].r);
            XMVECTOR q1 = XMLoadFloat4(&m_tmp[j].r);
            XMStoreFloat4(&m_pose[j].r, XMQuaternionSlerp(q0, q1, w));
        }
    }
    ApplyAim(m_pose);
    for (const TwoBoneIK& k : ik)
        ApplyIK(k, m_pose);
    ComposePalette(*model, m_pose, out);
}

} // namespace tankaq
