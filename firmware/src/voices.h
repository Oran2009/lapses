// Seven voices: 8 breathing sine partials into a delay line whose feedback
// runs through a distortion. Reads WFC's SHOWN state -- a repair is a wave.

#pragma once

#include "wfc.h"

namespace voices
{
// 2 s at 48 kHz. The delay is clamped to 1.9 s, leaving room for the
// interpolator's next tap.
constexpr int kDelayMax = 96000;

// SPIKE: three voice baselines to choose between by ear. GUI only -- the
// Touch 2 does not map this. 0 is the voice as it was.
constexpr int kModels = 3;
void        SetModel(int m);
int         Model_();
const char* ModelName(int m);

// S09/S10 picks what DRIVES the mod: 0 steps, 1 follower, 2 LFO. Destination
// is fixed. The follower stops when you do; the other two do not.
void SetModMode(int m);
int  ModMode();

void Init(float sample_rate, int block_size);

// `snap` jumps the partials instead of gliding. `ref` is what the spectral
// tilt is measured against -- the chord's root an octave up; 0 means twice.
void SetPitch(int v, float hz, float ref, bool snap);
float Pitch(int v);

// One control tick (~1 kHz, the block rate): advances envelopes, partial
// wander and mods, and glides every parameter towards the shown state.
void Control(double now, float dt);

// Bit g set: HOLD (wfc::K().hold) is group g's amplitude floor

// The pad's own state, apart from wfc::Sounding: a voice HOLD keeps sounding
// is still released as far as the envelope is concerned. Defaults to true.
void SetPadHeld(int v, bool on);
// HOLD is floating this voice above silence, so the solver must count it
bool HoldSustains(int v);

// Adds into out_l/out_r, which must already hold the dry bus. `in_mix`
// crossfades each voice's source from its partials to `in` (1 = effects box).
void Process(float* out_l, float* out_r, int n, const float* in = nullptr,
             float in_mix = 0.0f);

// 0..1, the voice's envelope. The pad allocator prefers the quietest voice.
float Amp(int v);

// What the audio is using this block: the glided target plus modulation. The
// tile's own numbers are what was decided, not what is heard.
float EffDly(int v);     // seconds
float EffFb(int v);
float EffDrv(int v);     // the loop's pre-gain
float Mod(int v);        // the modulator right now, -1..1
float ModDepth(int v);   // how much of it is being spent, 0..1.5
float Pan(int v);        // -1..1
float LoopLevel(int v);
// What this voice is sending to the bus right now: 1 at rest, less while
// another voice is fresh, more while it is the fresh one
float OutGain(int v);
float BusLvl(int v);   // what it is putting on the bus right now
// Loudest sample this voice alone has put on the bus since the last call
float TakePeak(int v);
// Extra dry a freshly pressed voice sends, before its loop has filled
float DryLift(int v);

// The loudest envelope, for the LED
float Level();

}  // namespace voices
