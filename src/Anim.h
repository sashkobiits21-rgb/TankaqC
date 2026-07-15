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

// --------------------------------------------------------------- full system
// Layered animation + procedural constraints. Per pose: sample every active
// layer -> masked blend (per-joint weights) -> compose globals -> aim
// constraint (analytic chain twist) -> two-bone IK (law of cosines, pole
// vector), each blended against the animated pose by its weight -> palette.

using JointMask = std::vector<float>;   // per-joint blend weight, 0..1

int FindJoint(const SkinnedModel& model, const char* nameSubstr);
// weight `inW` for `rootJointName` and every descendant, `outW` elsewhere
JointMask MaskSubtree(const SkinnedModel& model, const char* rootJointName,
                      float inW = 1.0f, float outW = 0.0f);

struct Animator
{
    static constexpr int MaxLayers = 4;

    struct Layer
    {
        int clip = -1;
        float time = 0;
        float weight = 0;        // current, eased toward target
        float targetWeight = 0;
        float fadeSpeed = 8.0f;  // per second
        bool loop = true;
        float rate = 1.0f;       // playback speed
        JointMask mask;          // empty = full body
    };

    // Aim: rotate a chain (ordered parent->child) so the character's forward
    // axis points at a MODEL-SPACE target; twist is split across the chain.
    struct Aim
    {
        bool active = false;
        int chain[4]{ -1, -1, -1, -1 };
        int chainCount = 0;
        DirectX::XMFLOAT3 target{ 0, 0, 1 };  // model space
        DirectX::XMFLOAT3 forward{ 0, 0, 1 }; // character forward, model space
        float weight = 1.0f;
        float maxAngle = 1.2f;                // clamp (radians)
        bool yawOnly = true;
    };

    // Two-bone IK: a (upper), b (lower), c (effector). The solved pose is
    // blended against the animated pose by `weight` -- ramp it inside a clip
    // to make e.g. a pick-up animation reach a specific object.
    struct TwoBoneIK
    {
        bool active = false;
        int a = -1, b = -1, c = -1;
        DirectX::XMFLOAT3 target{};           // model space
        DirectX::XMFLOAT3 pole{ 0, 0, 1 };    // bend hint, model space
        float weight = 1.0f;
    };

    const SkinnedModel* model = nullptr;
    Layer layers[MaxLayers];
    Aim aim;
    TwoBoneIK ik[2];

    void PlayLayer(int layer, int clip, bool loop = true,
                   float targetWeight = 1.0f, bool restart = false);
    void StopLayer(int layer) { layers[layer].targetWeight = 0; }
    void Update(float dt);
    void Pose(BonePalette& out);

private:
    std::vector<JointPose> m_pose, m_tmp;
    void ApplyAim(std::vector<JointPose>& locals);
    void ApplyIK(const TwoBoneIK& k, std::vector<JointPose>& locals);
};

} // namespace tankaq
