// Pads, octave, scale, voice allocation, the knobs and the master chain.

#pragma once

#include "wfc.h"

namespace synth
{
constexpr int kPads = wfc::kPads;   // 7, one voice each

void Init(float sample_rate, int block_size);

// Pads 0..6. A press re-rolls its voice; nothing sounds unless held
void Press(int pad);
void Release(int pad);
// True while the pad is sounding, whether from contact or from the latch
bool PadOn(int pad);
// S07/S08 down: the pads held at that moment stay down until it is released.
void Latch(bool on);
bool Latched(int pad);
// 0..1 contact force on a held pad; leaning past the threshold breaks its group.
void  SetPressure(int pad, float p);
float Pressure(int pad);
// Resting force a lean is measured against
float PressureRest(int pad);

// Clamped to +-2 octaves. True when it moved.
bool ShiftOctave(int delta);
int  Octave();

// P01 steps the tunings. A change lands on the next press.
constexpr int kScales = 4;
void        NextScale();
int         Scale();
const char* ScaleName();

constexpr int kKnobs = 10;   // HANDOFF 4.2's six, plus Unrest, Hold, Link, Shimmer

// 0..1 each. Rebuilds the rules and schedules the debounced heal.
void SetKnob(int i, float v);
float Knob(int i);
const char* KnobName(int i);

// Advance the control side. Call once per audio block.
void Control(double now);

// One block, replacing the buffers. `in` is the mono line input or null.
void Process(float* l, float* r, int n, const float* in = nullptr);

// 0..1: how far the line input has taken over from the partials
float InputMix();

// 0..1, for the LED: the loudest voice's envelope
float Level();

// True once per delivered break, for the LED's stutter
bool TookBreak();

// Output trim in 3 dB steps, 0 down to -30 dB. True when it moved.
bool ShiftGain(int delta);
float GainDb();

// Break a rule now, or settle everything now
void Break();
void Settle();

// Hold the solver where it is
void Freeze(bool on);
bool Frozen();

// Which voice each pad is driving, -1 for none (for the display)
int PadVoice(int pad);
float PadHz(int pad);

}  // namespace synth
