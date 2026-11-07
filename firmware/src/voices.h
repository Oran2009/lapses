// THE VOICES
//
// Four voices. Each is 8 breathing sine partials into its own delay line,
// whose feedback path runs through a distortion, with a per-voice mod on that
// line's delay time, feedback and drive, and
// loop output into another voice's loop input.
//
// Everything WFC decided is read from the SHOWN state (wfc::Shown*), never
// from the logical copy: the repair arrives as a wave in time, and the audio
// must hear it arrive.
//
// Portable C++ apart from one `section` attribute on the delay buffers, so
// this compiles and runs in the desktop harness. The master chain (reverb,
// limiter, output gain) is deliberately NOT here -- it needs SDRAM placement
// on the Daisy, and keeping it out leaves this file testable.
//
// Section numbers refer to docs/HANDOFF.md; 4.7 is the signal flow.

#pragma once

#include "wfc.h"

namespace voices
{
// There is one modulator: each voice follows its own loop's level, rising or
// falling. The mod and chaos modes are gone, and WFC's rate class now sets how
// quickly a voice reacts rather than an oscillator's frequency.
//
// 2 s at 48 kHz, which is the prototype's DelayNode size. The delay itself is
// clamped to 1.9 s, so there is always room for the interpolator's next tap.
constexpr int kDelayMax = 96000;

// SPIKE: three voice baselines to choose between by ear. GUI only -- the
// Touch 2 does not map this. 0 is the voice as it was.
constexpr int kModels = 3;
void        SetModel(int m);
int         Model_();
const char* ModelName(int m);

// S09/S10: what the mod follower moves. 0 delay (the echoes bend), 1 the
// blend it has always been, 2 feedback (the loop rings longer).
void SetModMode(int m);
int  ModMode();

void Init(float sample_rate, int block_size);

// The pad's pitch for this voice. `snap` jumps the partials there instead of
// gliding, which is what a fresh press wants.
// `ref` is the pitch the voice's spectral tilt is measured against -- the
// chord's root an octave up, so the ensemble keeps the same brightness
// spread whatever octave it is played at. 0 means twice this voice's own.
void SetPitch(int v, float hz, float ref, bool snap);
float Pitch(int v);

// One control tick (~1 kHz, from the audio callback's block rate): advances
// the amp envelopes, the partial wander and the mods, and moves every
// smoothed parameter towards what the shown WFC state and the knobs ask for.
void Control(float now, float dt);

// Bit g set: HOLD (wfc::K().hold) is group g's amplitude floor

// The pad's own state, apart from wfc::Sounding: a voice HOLD keeps sounding
// is still released as far as the envelope is concerned. Defaults to true.
void SetPadHeld(int v, bool on);
// HOLD is floating this voice above silence, so the solver must count it
bool HoldSustains(int v);

// One block of audio. Adds into out_l/out_r, which must already be the dry
// bus the caller wants (normally silence).
//
// `in_mix` crossfades every voice's source from its own partials to `in`: at
// 1 the partials are gone and the loops are running on the line input alone,
// which is the effects box. The pads still gate the loops and still tune the
// delay snap, so a hard-driven loop rings at the pad's pitch -- that ringing
// is the resonator, not a surviving oscillator.
void Process(float* out_l, float* out_r, int n, const float* in = nullptr,
             float in_mix = 0.0f);

// 0..1, the voice's envelope. The pad allocator prefers the quietest voice.
float Amp(int v);

// The values the audio is actually using this block: the solver's target as
// glided, with the modulation added. A display showing the tile's numbers is
// showing what was decided, not what is being heard.
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
