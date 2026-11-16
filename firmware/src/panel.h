// P03..P09 play and report force, P00/P02 octave, P01 scale, P10/P11 volume.
// S30..S35 on ADC 0..5, faders S36/S37 on ADC 6,7. S07/S08 freeze / latch,
// S09/S10 mod source. Control runs in the main loop: the engine is not reentrant.

#pragma once

#include "daisy_seed.h"

namespace panel
{
constexpr int kPads = 12;

// 250 Hz, the rate Synthux's own firmware uses: panel work at block rate
// couples into the analog side as a tone. 4 ms of latency is the price.
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
