#pragma once

// Tiny XAudio2 sound system. Every effect is procedurally synthesized at init
// (square/saw/noise oscillators + envelopes + bitcrush) for a stylized low-bit
// techno flavor -- no audio assets on disk. All calls are safe no-ops if audio
// init fails (no device, headless test runs).
namespace tankaq::snd
{

enum class Sfx
{
    Hover = 0,     // UI hover blip
    Click,         // UI click
    Shoot,         // projectile fired
    Explosion,     // projectile impact (techno kick)
    Burn,          // upgrade purchase (low-bit fire crackle)
    Glass,         // border slats shattering on card ejection
    Count
};

bool Init();
void Shutdown();
void Update(float dt);                  // smooths the engine loop volume/pitch
void Play(Sfx s, float volume = 1.0f, float pitch = 1.0f);
void SetEngine(float intensity);        // 0..1 movement intensity target

} // namespace tankaq::snd
