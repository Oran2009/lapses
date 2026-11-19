// Seven voices: 8 breathing sine partials into a delay line with distorted
// feedback. Reads WFC's SHOWN state.

#pragma once

#include "wfc.h"

namespace voices
{
// 2 s at 48 kHz; the delay clamps to 1.9 s
constexpr int kDelayMax = 96000;

// SPIKE: three voice baselines, GUI only; 0 is the voice as it was
constexpr int kModels = 3;
void        SetModel(int m);
int         Model_();
const char* ModelName(int m);

// S09/S10 picks what drives the mod: 0 steps, 1 follower, 2 LFO
void SetModMode(int m);
int  ModMode();

void Init(float sample_rate, int block_size);

// `snap` jumps the partials; `ref` is the tilt reference, 0 = twice the root
void SetPitch(int v, float hz, float ref, bool snap);
float Pitch(int v);

// one control tick (~1 kHz): envelopes, partial wander, mods, glide
void Control(double now, float dt);

// bit g set: HOLD is group g's amplitude floor

// a voice HOLD keeps sounding is still released here
void SetPadHeld(int v, bool on);
// HOLD is floating this voice above silence, so the solver must count it
bool HoldSustains(int v);

// adds into out_l/out_r; `in_mix` fades partials to `in`
void Process(float* out_l, float* out_r, int n, const float* in = nullptr,
             float in_mix = 0.0f);

// 0..1 envelope; the pad allocator prefers the quietest voice
float Amp(int v);

// the glided target plus modulation
float EffDly(int v);     // seconds
float EffFb(int v);
float EffDrv(int v);     // the loop's pre-gain
float Mod(int v);        // the modulator right now, -1..1
float ModDepth(int v);   // how much of it is being spent, 0..1.5
float Pan(int v);        // -1..1
float LoopLevel(int v);
// 1 at rest, less while another voice is fresh
float OutGain(int v);
float BusLvl(int v);   // what it is putting on the bus right now
// loudest sample on the bus since the last call
float TakePeak(int v);
// extra dry a fresh voice sends before its loop has filled
float DryLift(int v);

// the loudest envelope, for the LED
float Level();

}  // namespace voices
