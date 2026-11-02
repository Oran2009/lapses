// THE INSTRUMENT
//
// Pads, octave, voice allocation, the six knobs, and the master chain.
// Portable: the Daisy shell (main.cpp) and the desktop shell (host/) both
// drive this and add nothing of their own.

#pragma once

#include "wfc.h"

namespace synth
{
constexpr int kPads = wfc::kPads;   // 7 playable
constexpr int kConnectorPad = 2;   // momentary, no voice

void Init(float sample_rate, int block_size);

// Pads 0..6. A press re-rolls its voice; nothing sounds unless held
void Press(int pad);
void Release(int pad);
bool PadOn(int pad);
bool Joined();   // P05 is down
// Which group S07/S08 sends HOLD to: +1 left (up), 0 both, -1 right (down)
void SetHoldTarget(int pos);
// S36: one fader for Control and Unrest together
void SetAgitation(float a);

// Clamped to +-2 octaves. True when it moved.
bool ShiftOctave(int delta);
int  Octave();

constexpr int kKnobs = 8;   // HANDOFF 4.2's six, plus Unrest and Space

// 0..1 each. Rebuilds the rules and schedules the debounced heal.
void SetKnob(int i, float v);
float Knob(int i);
const char* KnobName(int i);

// Advance the control side. Call once per audio block.
void Control(float now);

// One block, replacing whatever is in the buffers. `in` is the line input,
// mono, or null when the shell has none: a signal on it turns the instrument
// into an effects box, detected rather than switched.
void Process(float* l, float* r, int n, const float* in = nullptr);

// 0..1: how far the line input has taken over from the partials
float InputMix();

// 0..1, for the LED: tracks the voices and so follows the release
float Level();

// Output trim in 3 dB steps, 0 down to -30 dB. True when it moved.
bool ShiftGain(int delta);
float GainDb();

// What a cable carries (voices::CableMode)
void SetCableMode(int m);
int  CableMode();
const char* CableName(int m);

// Break a rule now, or settle everything now
void Break();
void Settle();

// Hold the solver where it is
void Freeze(bool on);
bool Frozen();

// Which voice each pad is driving, -1 for none (for the display)
int PadVoice(int pad);
// Meaningless for kConnectorPad: it drives no voice
float PadHz(int pad);

}  // namespace synth
