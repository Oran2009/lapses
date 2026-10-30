// THE TOUCH 2 PANEL
//
// The hardware half of the old project's src/panel/touch2.cpp, with its
// engine half dropped. The MPR121 register settings are the ones that were
// tuned against the real board -- libDaisy's defaults miss presses.
//
// Pads:   P03..P09 play, P00/P01 octave down/up, P10/P11 volume down/up
// Knobs:  S30..S35 on ADC 0..5, in HANDOFF 4.2 order
// Switch: S07/S08 down latches
// Spare:  the two faders (ADC 6,7), S09/S10 and P02 are unmapped

#pragma once

#include "daisy_seed.h"

namespace panel
{
constexpr int kPads = 12;

bool Init(daisy::DaisySeed& hw);

// Reads the knobs and the switch, and edge-triggers the pads into synth
void Poll();

// The MPR121 is an I2C transaction, so the main loop calls this often
void PollPads();

// False when the MPR121 did not answer at Init: every pad then reads untouched
bool PadsOk();

// True once per octave or volume pad touch, for the LED blink.
// 2 means the control was already at its end.
int TakeBlink();

}  // namespace panel
