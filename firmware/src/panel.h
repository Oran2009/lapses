// THE TOUCH 2 PANEL
//
// The hardware half of the old project's src/panel/touch2.cpp, with its
// engine half dropped. The MPR121 register settings are the ones that were
// tuned against the real board -- libDaisy's defaults miss presses.
//
// Pads:   P03..P09 play, P00/P02 octave down/up, P10/P11 volume down/up
//         A playing pad also reports contact force, and leaning on one
//         breaks its group -- see synth::SetPressure
// Knobs:  S30..S35 on ADC 0..5, faders S36/S37 on ADC 6,7
// Switch: S09/S10 cross-mod/none/cross-feed; S07/S08 is unassigned
// Spare:  P01, the S35 knob, the S36 fader and the S07/S08 switch
//
// The engine is not reentrant and a solve is most of an audio block, so the
// whole control side -- this and synth::Control -- runs in the main loop.
// The audio callback only renders.

#pragma once

#include "daisy_seed.h"

namespace panel
{
constexpr int kPads = 12;

// Poll and PollPads run once every this many audio blocks, so at 48 kHz in
// blocks of 48 they run at 250 Hz. Synthux's own firmware for this board
// polls at the same rate: the block-rate burst of panel work couples into
// the analog side as a tone, and 4 ms of touch latency is the price of
// moving it down and thinning it out.
constexpr int kPollDiv = 4;

// `poll_rate` is how often Poll will actually be called, in Hz. It sets
// AnalogControl's slew, which is wrong by that factor if it is guessed.
bool Init(daisy::DaisySeed& hw, float poll_rate);

// Reads the knobs and the switches, and edge-triggers the pads into synth
void Poll();

// Separate from Poll only so the two bursts of work land on different
// blocks instead of one bigger burst on the same one
void PollPads();

// False when the MPR121 did not answer at Init: every pad then reads untouched
bool PadsOk();

// True once per octave or volume pad touch, for the LED blink.
// 2 means the control was already at its end.
int TakeBlink();

}  // namespace panel
