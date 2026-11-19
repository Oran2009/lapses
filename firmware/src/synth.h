// Pads, octave, scale, voice allocation, the knobs and the master chain.

#pragma once

#include "wfc.h"

namespace synth
{
constexpr int kPads = wfc::kPads;   // 7, one voice each

void Init(float sample_rate, int block_size);

// a press re-rolls its voice; nothing sounds unless held
void Press(int pad);
void Release(int pad);
// sounding, from contact or from the latch
bool PadOn(int pad);
// S07/S08 down: the pads held then stay down
void Latch(bool on);
bool Latched(int pad);
// 0..1 contact force; leaning past the threshold breaks the pad's group
void  SetPressure(int pad, float p);
float Pressure(int pad);
// resting force a lean is measured against
float PressureRest(int pad);

// clamped to +-2 octaves; true when it moved
bool ShiftOctave(int delta);
int  Octave();

// P01 steps the tunings; a change lands on the next press
constexpr int kScales = 4;
void        NextScale();
int         Scale();
const char* ScaleName();

constexpr int kKnobs = 10;   // six, plus Unrest, Hold, Link, Shimmer

// 0..1 each; rebuilds the rules and schedules the debounced heal
void SetKnob(int i, float v);
float Knob(int i);
const char* KnobName(int i);

// advance the control side, once per audio block
void Control(double now);

// one block, replacing the buffers. `in` is the mono line input or null.
void Process(float* l, float* r, int n, const float* in = nullptr);

// 0..1: how far the line input has taken over
float InputMix();

// 0..1 for the LED: the loudest envelope
float Level();

// once per delivered break, for the LED's stutter
bool TookBreak();

// output trim in 3 dB steps, 0 to -30 dB; true when it moved
bool ShiftGain(int delta);
float GainDb();

// break a rule now, or settle everything now
void Break();
void Settle();

// hold the solver where it is
void Freeze(bool on);
bool Frozen();

// which voice each pad drives, -1 none
int PadVoice(int pad);
float PadHz(int pad);

}  // namespace synth
