#include "panel.h"

#include "synth.h"
#include "voices.h"

#include "dev/mpr121.h"
#include "hid/switch3.h"

using namespace daisy;

namespace panel
{
namespace
{
constexpr int kAnalog   = 8;
constexpr int kFirstPad = 3;   // P03..P09 play
constexpr int kOctDown = 0, kOctUp = 2;     // P00, P02
constexpr int kVolDown = 10, kVolUp = 11;   // P10, P11

AnalogControl analog[kAnalog];
Switch3       switch_a, switch_b;
Mpr121I2C     mpr;

bool     pads_ok   = false;
uint16_t pad_state = 0, last_pads = 0;
int      blink     = 0;

// Delta at full press, per playable electrode, measured on this board by
// Synthux Academy (github.com/Synthux-Academy/TouchString, touch/pads.cpp)
const float kMaxDelta[synth::kPads]
    = {375.0f, 460.0f, 375.0f, 375.0f, 375.0f, 425.0f, 425.0f};
float pressure[synth::kPads] = {};
int   press_turn             = 0;

}  // namespace

bool Init(DaisySeed& hw, float poll_rate)
{
    AdcChannelConfig cfg[kAnalog];
    cfg[0].InitSingle(seed::A0);
    cfg[1].InitSingle(seed::A1);
    cfg[2].InitSingle(seed::A2);
    cfg[3].InitSingle(seed::A3);
    cfg[4].InitSingle(seed::A4);
    cfg[5].InitSingle(seed::A5);
    cfg[6].InitSingle(seed::A6);
    cfg[7].InitSingle(seed::A7);
    hw.adc.Init(cfg, kAnalog);
    for(int i = 0; i < kAnalog; i++)
        analog[i].Init(hw.adc.GetPtr(i), poll_rate);
    hw.adc.Start();

    switch_a.Init(seed::D7, seed::D6);
    switch_b.Init(seed::D9, seed::D8);

    Mpr121I2C::Config mcfg;
    mcfg.touch_threshold   = 6;
    mcfg.release_threshold = 3;
    pads_ok                = mpr.Init(mcfg) == Mpr121I2C::Result::OK;
    if(pads_ok)
    {
        // Only the registers that differ from what Init just wrote
        static const uint8_t kReg[][2] = {
            {0x30, 0x01},   // NHDF   falling filter delta
            {0x31, 0x10},   // NCLF   16 samples before the baseline falls
            {0x32, 0x04},   // FDLF   falling filter delay
            {0x5B, 0x11},   // DEBOUNCE  1 sample each way
            {0x5C, 0xD0},   // CONFIG1   FFI 34 samples, CDC 16uA
            {0x5D, 0x51},   // CONFIG2   CDT 1us, SFI 10, ESI 2ms
            {0x7D, 200},    // USL  (3.3-0.7)/3.3*256
            {0x7E, 130},    // LSL  USL * 0.65
            {0x7F, 180},    // TL   USL * 0.9
            {0x7B, 0xCB},   // AUTOCONFIG0 -- enables it, so it goes last
        };
        for(size_t i = 0; i < sizeof(kReg) / sizeof(kReg[0]); i++)
            mpr.WriteRegister(kReg[i][0], kReg[i][1]);
        System::Delay(80);   // autoconfiguration's own settling time
    }
    return pads_ok;
}

bool PadsOk() { return pads_ok; }

void PollPads()
{
    if(!pads_ok)
        return;
    pad_state = mpr.Touched();

    // Contact force, one electrode per poll. Reading all seven every time
    // costs 21 bytes of blocking I2C in the main loop where the touch read
    // costs two, which starved the loop and broke up the audio. Round robin
    // refreshes each pad every 28 ms -- ample for leaning on a pad, which is
    // a gesture you make over a tenth of a second at least.
    const int p = press_turn;
    press_turn  = (press_turn + 1) % synth::kPads;
    const int e = p + kFirstPad;

    float target = 0.0f;
    if(pad_state & (1u << e))
    {
        const int base  = (int)mpr.BaselineData(e);
        const int filt  = (int)(mpr.FilteredData(e) & 0x03FF);
        const int delta = base > filt ? base - filt : 0;
        if(delta >= 6)
        {
            // Squared, so an ordinary press sits low and the top of the
            // range is somewhere you have to lean to reach
            const float n = (float)(delta - 5) / (kMaxDelta[p] - 5.0f);
            target        = n > 1.0f ? 1.0f : n * n;
        }
    }
    pressure[p] += (target - pressure[p]) * 0.5f;
    if(pressure[p] < 0.01f)
        pressure[p] = 0.0f;
    synth::SetPressure(p, pressure[p]);
}

int TakeBlink()
{
    const int was = blink;
    blink         = 0;
    return was;
}

void Poll()
{
    for(int i = 0; i < kAnalog; i++)
        analog[i].Process();
    // S30 Mod, S31 Feedback, S32 Delay, S33 Distortion, S34 Filter,
    // S35 Shift. The S36 fader is unassigned; S37 is Hold.
    static const int kKnobOf[kAnalog] = {4, 1, 0, 2, 3, 8, -1, 7};
    for(int i = 0; i < kAnalog; i++)
        synth::SetKnob(kKnobOf[i], analog[i].Value());
    // S07/S08 up freezes the solver, down latches the pads
    const int sa = switch_a.Read();
    synth::Freeze(sa == Switch3::POS_UP);
    synth::Latch(sa == Switch3::POS_DOWN);
    voices::SetCommon(switch_b.Read() == Switch3::POS_UP);

    const uint16_t changed = (uint16_t)(pad_state ^ last_pads);
    for(int i = 0; i < kPads; i++)
    {
        if(!(changed & (1u << i)))
            continue;
        const bool down = (pad_state & (1u << i)) != 0;
        if(i >= kFirstPad && i < kFirstPad + synth::kPads)
        {
            if(down)
                synth::Press(i - kFirstPad);
            else
                synth::Release(i - kFirstPad);
        }
        else if(down && (i == kOctDown || i == kOctUp))
            blink = synth::ShiftOctave(i == kOctUp ? 1 : -1) ? 1 : 2;
        else if(down && (i == kVolDown || i == kVolUp))
            blink = synth::ShiftGain(i == kVolUp ? 1 : -1) ? 1 : 2;
    }
    last_pads = pad_state;
}

}  // namespace panel
