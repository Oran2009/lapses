// THE VOICES
//
// Four voices. Each is 8 breathing sine partials into its own delay line,
// whose feedback path runs through a distortion, with a per-voice LFO on that
// line's delay time, feedback and drive, and a patch cable that can feed its
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
// 2 s at 48 kHz, which is the prototype's DelayNode size. The delay itself is
// clamped to 1.9 s, so there is always room for the interpolator's next tap.
constexpr int kDelayMax = 96000;

void Init(float sample_rate, int block_size);

// The pad's pitch for this voice. `snap` jumps the partials there instead of
// gliding, which is what a fresh press wants.
void SetPitch(int v, float hz, bool snap);
float Pitch(int v);

// One control tick (~1 kHz, from the audio callback's block rate): advances
// the amp envelopes, the partial wander and the LFOs, and moves every
// smoothed parameter towards what the shown WFC state and the knobs ask for.
void Control(float now, float dt);

// One block of audio. Adds into out_l/out_r, which must already be the dry
// bus the caller wants (normally silence).
void Process(float* out_l, float* out_r, int n);

// 0..1, the voice's envelope. The pad allocator prefers the quietest voice.
float Amp(int v);

// The loudest envelope, for the LED
float Level();

}  // namespace voices
