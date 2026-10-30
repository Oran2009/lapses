// THE INSTRUMENT
//
// Pads, latch, octave, voice allocation, the six knobs, and the master chain.
// Portable: the Daisy shell (main.cpp) and the desktop shell (host/) both
// drive this and add nothing of their own.

#pragma once

#include "wfc.h"

namespace synth
{
constexpr int kPads = wfc::kPads;   // 7 playable

void Init(float sample_rate, int block_size);

// Pads 0..6. A press re-rolls its voice; nothing sounds unless held or latched
void Press(int pad);
void Release(int pad);
bool PadOn(int pad);

// On: latches every pad held right now, on top of any already latched.
// Off: drops them all.
void Latch(bool on);
bool Latched();

// Clamped to +-2 octaves. True when it moved.
bool ShiftOctave(int delta);
int  Octave();

// 0..1 each, in the order of HANDOFF 4.2. Rebuilds the rules and schedules
// the debounced heal.
void SetKnob(int i, float v);
float Knob(int i);
const char* KnobName(int i);

// Advance the control side. Call once per audio block.
void Control(float now);

// One block, replacing whatever is in the buffers.
void Process(float* l, float* r, int n);

// 0..1, for the LED: tracks the voices and so follows the release
float Level();

// Output trim in 3 dB steps, 0 down to -30 dB. True when it moved.
bool ShiftGain(int delta);
float GainDb();

// Which voice each pad is driving, -1 for none (for the display)
int PadVoice(int pad);
float PadHz(int pad);

}  // namespace synth
