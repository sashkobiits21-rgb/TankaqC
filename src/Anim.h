#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <DirectXMath.h>
#include "render/IRenderer.h"
#include "AssetLoad.h"

// Runtime animation for skinned models: sample glTF keyframe channels into
// local joint TRS, optionally blend two clips, compose the hierarchy, and
// produce the GPU bone palette (inverseBind * global, row-vector order).

namespace tankaq
{

struct JointPose
{
    DirectX::XMFLOAT3 t{ 0, 0, 0 };
    DirectX::XMFLOAT4 r{ 0, 0, 0, 1 };
    DirectX::XMFLOAT3 s{ 1, 1, 1 };
};

// Sample one clip at time `t` (wrapped when `loop`) into per-joint local TRS.
// `out` must hold model.joints.size() entries; starts from the rest pose.
void SampleClip(const SkinnedModel& model, int clip, float t, bool loop,
                std::vector<JointPose>& out);

// Blend two sampled poses (lerp T/S, slerp R). alpha = 1 -> b.
void BlendPoses(const std::vector<JointPose>& a, const std::vector<JointPose>& b,
                float alpha, std::vector<JointPose>& out);

// Compose the hierarchy and produce the GPU palette.
void ComposePalette(const SkinnedModel& model,
                    const std::vector<JointPose>& locals, BonePalette& out);

// Convenience: a tiny self-contained player (one clip, or a crossfade).
struct AnimPlayer
{
    const SkinnedModel* model = nullptr;
    int clip = 0;
    float time = 0;
    int prevClip = -1;
    float prevTime = 0;
    float fade = 0;          // 1 -> fully on `clip`
    float fadeSpeed = 6.0f;  // per second

    void Play(int newClip, bool restart = false);
    void Update(float dt);
    void Pose(BonePalette& out) const;

private:
    mutable std::vector<JointPose> m_a, m_b;
};

} // namespace tankaq
