// P03..P09 play and report force, P00/P02 octave, P01 scale, P10/P11 volume.
// S30..S35 on ADC 0..5, faders S36/S37 on ADC 6,7. S07/S08 freeze / latch,
// S09/S10 mod source. Control runs in the main loop: the engine is not reentrant.

#pragma once

#include "daisy_seed.h"

namespace panel
{
constexpr int kPads = 12;

// 250 Hz; block-rate panel work couples in as a tone
constexpr int kPollDiv = 4;

// sets AnalogControl's slew; wrong if guessed
bool Init(daisy::DaisySeed& hw, float poll_rate);

// knobs, switches, pads edge-triggered into synth
void Poll();

// separate from Poll so the two bursts land on different blocks
void PollPads();

// false if the MPR121 never answered; pads read untouched
bool PadsOk();

// once per octave or volume pad touch, for the LED. 2 = already at the end.
int TakeBlink();

}  // namespace panel
