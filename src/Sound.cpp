#include "Sound.h"
#include <windows.h>
#include <xaudio2.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Procedural synthesis notes
// --------------------------
// Everything is rendered once at init into 44.1 kHz mono 16-bit PCM. The
// shared "techno / low-bit" stylization is a bitcrush: quantize the signal to
// N bits and hold each value for M samples (a sample-rate + bit-depth crush,
// the classic chiptune degrader). Filters are one-pole lowpasses
// (y += k * (x - y)), envelopes are exponentials, oscillators are naive
// square/saw (aliasing is part of the look).

namespace tankaq::snd
{
namespace
{

constexpr int kRate = 44100;
constexpr int kPoolVoices = 24;
constexpr float kEngineMaxVol = 0.14f;   // deliberately quiet (user request)
constexpr float kTurnMaxVol = 0.11f;     // rotation whirr, also quiet

IXAudio2* s_xa = nullptr;
IXAudio2MasteringVoice* s_master = nullptr;
IXAudio2SourceVoice* s_pool[kPoolVoices]{};
IXAudio2SourceVoice* s_engine = nullptr;
IXAudio2SourceVoice* s_turn = nullptr;
std::vector<int16_t> s_pcm[int(Sfx::Count)];
std::vector<int16_t> s_enginePcm;
std::vector<int16_t> s_turnPcm;
float s_engineTarget = 0.0f;
float s_engineCur = 0.0f;
float s_turnTarget = 0.0f;
float s_turnCur = 0.0f;
bool s_ok = false;

// ---------------------------------------------------------------- helpers

uint32_t s_rng = 0xC0FFEE11u;
float Rand01()
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return float(s_rng >> 8) * (1.0f / 16777216.0f);
}
float RandS() { return Rand01() * 2.0f - 1.0f; }

float Square(float phase) { return fmodf(phase, 1.0f) < 0.5f ? 1.0f : -1.0f; }
float Saw(float phase) { return fmodf(phase, 1.0f) * 2.0f - 1.0f; }
float Tri(float phase)
{
    float f = fmodf(phase, 1.0f);
    return 4.0f * fabsf(f - 0.5f) - 1.0f;
}

// one-pole lowpass coefficient for a cutoff in Hz
float LpK(float cutoff)
{
    return 1.0f - expf(-6.2831853f * cutoff / float(kRate));
}

void Bitcrush(std::vector<float>& s, int bits, int hold)
{
    float levels = float(1 << (bits - 1));
    float held = 0.0f;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (hold <= 1 || i % size_t(hold) == 0)
            held = roundf(s[i] * levels) / levels;
        s[i] = held;
    }
}

std::vector<int16_t> ToPcm(const std::vector<float>& f, float gain)
{
    std::vector<int16_t> out(f.size());
    for (size_t i = 0; i < f.size(); ++i)
    {
        float v = std::clamp(f[i] * gain, -1.0f, 1.0f);
        out[i] = int16_t(v * 32767.0f);
    }
    return out;
}

// ------------------------------------------------------------- synthesis

// UI hover: a tiny techno blip -- short square with a slight downward chirp.
std::vector<int16_t> MakeHover()
{
    int n = int(0.045f * kRate);
    std::vector<float> s(n);
    float phase = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        float t = float(i) / kRate;
        float freq = 1500.0f * expf(-t * 6.0f);
        phase += freq / kRate;
        s[i] = Square(phase) * expf(-t * 95.0f);
    }
    Bitcrush(s, 6, 2);
    return ToPcm(s, 0.65f);
}

// UI click: punchier two-stage square chirp with a noise tick on the attack.
std::vector<int16_t> MakeClick()
{
    int n = int(0.10f * kRate);
    std::vector<float> s(n);
    float phase = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        float t = float(i) / kRate;
        float freq = 1900.0f * expf(-t * 22.0f) + 250.0f;
        phase += freq / kRate;
        float v = Square(phase) * expf(-t * 42.0f);
        if (t < 0.004f)
            v += RandS() * 0.5f * (1.0f - t / 0.004f);
        s[i] = v;
    }
    Bitcrush(s, 6, 3);
    return ToPcm(s, 0.7f);
}

// Shoot: deep bass zap -- a low saw sweep riding a half-frequency sine sub,
// with a short dark noise transient. Reads as a techno bass stab.
std::vector<int16_t> MakeShoot()
{
    int n = int(0.30f * kRate);
    std::vector<float> s(n);
    float phase = 0.0f, subPhase = 0.0f, lp = 0.0f;
    float kNoise = LpK(900.0f);
    for (int i = 0; i < n; ++i)
    {
        float t = float(i) / kRate;
        float freq = 340.0f * expf(-t * 14.0f) + 58.0f;
        phase += freq / kRate;
        subPhase += (freq * 0.5f) / kRate;
        float v = Saw(phase) * 0.6f * expf(-t * 13.0f)
                + sinf(subPhase * 6.2831853f) * 0.8f * expf(-t * 10.0f);
        lp += kNoise * (RandS() - lp);
        if (t < 0.035f)
            v += lp * 2.6f * (1.0f - t / 0.035f);
        s[i] = tanhf(v * 1.8f);
    }
    Bitcrush(s, 6, 4);
    return ToPcm(s, 0.9f);
}

// Explosion: a deep bass drop -- kick body sweeping into the sub range, a
// long 36 Hz sub tail, and a dark low rumble wash, clipped and crushed.
std::vector<int16_t> MakeExplosion()
{
    int n = int(0.95f * kRate);
    std::vector<float> s(n);
    float phase = 0.0f, subPhase = 0.0f, lp = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        float t = float(i) / kRate;
        float freq = 105.0f * expf(-t * 9.0f) + 30.0f;
        phase += freq / kRate;
        subPhase += 36.0f / kRate;
        float body = sinf(phase * 6.2831853f) * 1.1f * expf(-t * 3.4f);
        float cutoff = 1500.0f * expf(-t * 6.0f) + 120.0f;
        lp += LpK(cutoff) * (RandS() - lp);
        float rumble = lp * 2.6f * expf(-t * 3.8f);
        float sub = sinf(subPhase * 6.2831853f) * expf(-t * 5.0f) * 0.85f;
        s[i] = tanhf((body + rumble + sub) * 2.0f);
    }
    Bitcrush(s, 7, 5);
    return ToPcm(s, 0.95f);
}

// Purchase burn: low-bit fire -- swept-lowpass noise bed modulated by random
// crackle pops, crushed hard so it sounds like fire sampled into a tracker.
std::vector<int16_t> MakeBurn()
{
    int n = int(0.65f * kRate);
    std::vector<float> s(n);
    float lp = 0.0f, crackle = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        float t = float(i) / kRate;
        float cutoff = 3800.0f * expf(-t * 3.2f) + 500.0f;
        lp += LpK(cutoff) * (RandS() - lp);
        // random pops: exponential impulses at random times
        if (Rand01() < 0.0035f)
            crackle = 0.9f + 0.6f * Rand01();
        crackle *= expf(-1.0f / (0.0025f * kRate));
        float env = std::min(t / 0.012f, 1.0f)
                  * (t < 0.32f ? 1.0f : expf(-(t - 0.32f) * 7.0f));
        s[i] = (lp * 2.4f * (0.55f + crackle)) * env;
    }
    Bitcrush(s, 5, 6);
    return ToPcm(s, 0.8f);
}

// Slat break: glass -- an initial noise crack, then a cloud of high inharmonic
// sine shards with randomized starts and fast individual decays.
std::vector<int16_t> MakeGlass()
{
    int n = int(0.60f * kRate);
    std::vector<float> s(n, 0.0f);
    // crack transient (high-frequency noise = white minus its lowpass)
    float lp = 0.0f, kCrack = LpK(1500.0f);
    for (int i = 0; i < int(0.015f * kRate); ++i)
    {
        float w = RandS();
        lp += kCrack * (w - lp);
        s[i] += (w - lp) * 1.6f * (1.0f - float(i) / (0.015f * kRate));
    }
    // shards
    constexpr int kShards = 14;
    for (int sh = 0; sh < kShards; ++sh)
    {
        float f = 1900.0f + Rand01() * 5900.0f;
        float start = Rand01() * 0.14f;
        float decay = 14.0f + Rand01() * 22.0f;
        float amp = (0.5f + 0.5f * Rand01()) / sqrtf(float(kShards));
        float shimmer = 5.0f + Rand01() * 6.0f;
        int i0 = int(start * kRate);
        float phase = Rand01();
        for (int i = i0; i < n; ++i)
        {
            float t = float(i - i0) / kRate;
            phase += f / kRate;
            s[i] += sinf(phase * 6.2831853f) * expf(-t * decay) * amp
                  * (1.0f + 0.15f * sinf(t * shimmer * 6.2831853f));
        }
    }
    Bitcrush(s, 8, 2);   // light crush: stylized but still glassy
    return ToPcm(s, 0.8f);
}

// Engine loop: a quiet low hum. All layers have integer periods inside the
// 1-second buffer (and the noise bed tiles at 0.25 s), so the loop is
// mathematically seamless. Smooth waveforms on purpose: triangles and a sine
// sub instead of squares/saw, a slow noise tile instead of a 10 Hz one, and
// no bitcrush -- quantization roughness on a steady tone reads as vibration.
std::vector<int16_t> MakeEngine()
{
    int n = kRate;   // exactly 1 s
    std::vector<float> s(n);
    // periodic noise bed: one 0.25 s block tiled 4x (4 Hz cycle, no fast throb)
    int block = kRate / 4;
    std::vector<float> noise(block);
    float lp = 0.0f, kN = LpK(500.0f);
    for (int i = 0; i < block; ++i)
    {
        lp += kN * (RandS() - lp);
        noise[i] = lp;
    }
    for (int i = 0; i < n; ++i)
    {
        float t = float(i) / kRate;
        float v = Tri(t * 56.0f) * 0.42f              // fundamental, soft edges
                + Tri(t * 112.0f) * 0.12f             // octave shimmer
                + sinf(t * 28.0f * 6.2831853f) * 0.30f // smooth sub
                + noise[i % block] * 0.9f;            // slow rumble
        s[i] = v;
    }
    // final one-pole lowpass; run it twice around the loop so the filter
    // state at sample 0 matches sample n (keeps the seam silent)
    float f = 0.0f, kF = LpK(850.0f);
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < n; ++i)
        {
            f += kF * (s[i] - f);
            if (pass == 1) s[i] = f;
        }
    return ToPcm(s, 0.7f);
}

// Turn loop: the "tank rotating" layer -- a track-slip whirr. Mid-band noise
// and a soft 138 Hz triangle, both pulsing at 11 Hz like tread links
// slipping, with the pulses phase-offset so it churns instead of beeping.
// Integer periods everywhere keep the 1 s loop seamless.
std::vector<int16_t> MakeTurn()
{
    int n = kRate;
    std::vector<float> s(n);
    int block = kRate / 4;
    std::vector<float> noise(block);
    float lp = 0.0f, kN = LpK(1400.0f);
    for (int i = 0; i < block; ++i)
    {
        lp += kN * (RandS() - lp);
        noise[i] = lp;
    }
    for (int i = 0; i < n; ++i)
    {
        float t = float(i) / kRate;
        float trem = 0.72f + 0.28f * sinf(t * 11.0f * 6.2831853f);
        float trem2 = 0.80f + 0.20f * sinf(t * 11.0f * 6.2831853f + 2.1f);
        float v = Tri(t * 138.0f) * 0.26f * trem     // servo tone
                + Tri(t * 69.0f) * 0.12f             // half-speed underlayer
                + noise[i % block] * 0.85f * trem2;  // slipping treads
        s[i] = v;
    }
    float f = 0.0f, kF = LpK(1700.0f);
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < n; ++i)
        {
            f += kF * (s[i] - f);
            if (pass == 1) s[i] = f;
        }
    return ToPcm(s, 0.6f);
}

} // namespace

// ------------------------------------------------------------------ API

bool Init()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // harmless if already done
    if (FAILED(XAudio2Create(&s_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)))
        return false;
    if (FAILED(s_xa->CreateMasteringVoice(&s_master)))
    {
        s_xa->Release();
        s_xa = nullptr;
        return false;
    }

    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = kRate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = kRate * 2;

    for (int i = 0; i < kPoolVoices; ++i)
        if (FAILED(s_xa->CreateSourceVoice(&s_pool[i], &wf, 0, 2.0f)))
            return false;

    s_pcm[int(Sfx::Hover)] = MakeHover();
    s_pcm[int(Sfx::Click)] = MakeClick();
    s_pcm[int(Sfx::Shoot)] = MakeShoot();
    s_pcm[int(Sfx::Explosion)] = MakeExplosion();
    s_pcm[int(Sfx::Burn)] = MakeBurn();
    s_pcm[int(Sfx::Glass)] = MakeGlass();
    s_enginePcm = MakeEngine();
    s_turnPcm = MakeTurn();

    auto startLoop = [&](IXAudio2SourceVoice*& voice,
                         const std::vector<int16_t>& pcm)
    {
        if (FAILED(s_xa->CreateSourceVoice(&voice, &wf, 0, 2.0f)))
            return;
        XAUDIO2_BUFFER b{};
        b.AudioBytes = UINT32(pcm.size() * 2);
        b.pAudioData = reinterpret_cast<const BYTE*>(pcm.data());
        b.LoopCount = XAUDIO2_LOOP_INFINITE;
        voice->SetVolume(0.0f);
        voice->SubmitSourceBuffer(&b);
        voice->Start();
    };
    startLoop(s_engine, s_enginePcm);
    startLoop(s_turn, s_turnPcm);

    s_ok = true;
    return true;
}

void Shutdown()
{
    s_ok = false;
    for (int i = 0; i < kPoolVoices; ++i)
        if (s_pool[i]) { s_pool[i]->DestroyVoice(); s_pool[i] = nullptr; }
    if (s_engine) { s_engine->DestroyVoice(); s_engine = nullptr; }
    if (s_turn) { s_turn->DestroyVoice(); s_turn = nullptr; }
    if (s_master) { s_master->DestroyVoice(); s_master = nullptr; }
    if (s_xa) { s_xa->Release(); s_xa = nullptr; }
}

void Play(Sfx sfx, float volume, float pitch)
{
    if (!s_ok)
        return;
    const std::vector<int16_t>& pcm = s_pcm[int(sfx)];
    if (pcm.empty())
        return;
    for (int i = 0; i < kPoolVoices; ++i)
    {
        XAUDIO2_VOICE_STATE st;
        s_pool[i]->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued != 0)
            continue;
        s_pool[i]->SetVolume(std::clamp(volume, 0.0f, 1.5f));
        s_pool[i]->SetFrequencyRatio(std::clamp(pitch, 0.5f, 2.0f));
        XAUDIO2_BUFFER b{};
        b.AudioBytes = UINT32(pcm.size() * 2);
        b.pAudioData = reinterpret_cast<const BYTE*>(pcm.data());
        b.Flags = XAUDIO2_END_OF_STREAM;
        s_pool[i]->SubmitSourceBuffer(&b);
        s_pool[i]->Start();
        return;
    }
    // all 24 voices busy: drop the sound (inaudible in that much noise anyway)
}

void SetEngine(float intensity)
{
    s_engineTarget = std::clamp(intensity, 0.0f, 1.0f);
}

void SetTurn(float intensity)
{
    s_turnTarget = std::clamp(intensity, 0.0f, 1.0f);
}

void Update(float dt)
{
    if (!s_ok || !s_engine)
        return;
    // faster attack than release so the loops answer input but trail off soft
    auto smooth = [dt](float cur, float target, float atk, float rel)
    {
        float k = 1.0f - expf(-dt * (target > cur ? atk : rel));
        cur += (target - cur) * k;
        return (cur < 0.001f && target == 0.0f) ? 0.0f : cur;
    };
    s_engineCur = smooth(s_engineCur, s_engineTarget, 7.0f, 4.0f);
    s_turnCur = smooth(s_turnCur, s_turnTarget, 9.0f, 5.0f);

    // rotation feeds the engine too: the hum swells and strains a little
    // while the hull turns, and the track-slip whirr fades in on top
    float ev = std::min(1.0f, s_engineCur + s_turnCur * 0.55f);
    s_engine->SetVolume(ev * kEngineMaxVol);
    s_engine->SetFrequencyRatio(1.0f + s_engineCur * 0.26f + s_turnCur * 0.10f);
    if (s_turn)
    {
        s_turn->SetVolume(s_turnCur * kTurnMaxVol);
        s_turn->SetFrequencyRatio(0.92f + s_turnCur * 0.30f);
    }
}

} // namespace tankaq::snd
