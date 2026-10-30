#include "panel.h"

#include "synth.h"

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

}  // namespace

bool Init(DaisySeed& hw)
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
        analog[i].Init(hw.adc.GetPtr(i), hw.AudioCallbackRate());
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
    if(pads_ok)
        pad_state = mpr.Touched();
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
    // The panel's own order, against the knob order in HANDOFF 4.2. -1 is a
    // control that is wired but not yet spoken for: S35 and the S36 fader.
    static const int kKnobOf[kAnalog] = {4, 2, 1, 0, 3, -1, -1, 5};
    for(int i = 0; i < kAnalog; i++)
        if(kKnobOf[i] >= 0)
            synth::SetKnob(kKnobOf[i], analog[i].Value());

    // S07/S08 held down latches, and keeps latching, so a pad pressed while
    // it is down joins the others. Centre drops the lot.
    const bool sw_down = switch_a.Read() == Switch3::POS_DOWN;
    static bool was_down = false;
    if(sw_down || was_down)
        synth::Latch(sw_down);
    was_down = sw_down;

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
