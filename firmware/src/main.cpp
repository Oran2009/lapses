// The Touch 2 shell: the callback runs the engine, the main loop reads the
// panel and drives the LED. Same engine as the PC build.

#include "panel.h"
#include "synth.h"
#include "voices.h"
#include "wfc.h"

#include "daisy_seed.h"

using namespace daisy;

namespace
{
constexpr int kBlock = 48;


DaisySeed hw;
// Double: as a float32 the 1.5 ms step rounds to nothing past 32768 s and
// the clock stops dead, taking every scheduled break and heal with it.
double    now_s     = 0.0;
float     block_dt  = 0.0f;
uint32_t  blink_until = 0, stutter_until = 0;

// 32 kHz: seven voices at 48 left the board at 96%, and the band it costs
// us sits 63 dB down.
constexpr auto kRate = SaiHandle::Config::SampleRate::SAI_32KHZ;
// From the rate the hardware reports, not a constant: the overruns below
// are measured against it.
uint32_t       block_us = 0;
volatile uint16_t  over_n   = 0;
// 8 ms of catch-up is plenty; past that the loop is not keeping up at all
constexpr uint32_t kMaxLag  = 8;
// Counted apart because they want different fixes: a long callback, and a
// main loop falling behind one.
volatile uint16_t  lag_n    = 0;
volatile uint16_t  cb_max   = 0;
uint16_t           ctl_max  = 0;
int                strobe   = 0;
uint32_t           over_at  = 0;

// Control holds the solver, which is most of a 1 ms block, so it runs out
// here -- but once per block, or every smoothing constant is wrong.
volatile uint32_t blocks_done = 0;
uint32_t          blocks_seen = 0;

// ponytail: press logging, for why one press is loud and the next quiet.
// Both hunts it served are closed -- delete with its prints.
volatile float    pk          = 0.0f;
int               log_pad     = -1;
uint32_t          log_at      = 0;
bool              was_on[synth::kPads] = {};

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out,
                   size_t size)
{
    blocks_done++;
    const uint32_t t0 = System::GetUs();

    // ponytail: input mode off while the synth side is tuned. Pass `mono` to
    // synth::Process to re-enable; the engine path and its tests are intact.
    float mono[kBlock];
    for(size_t i = 0; i < size; i++)
        mono[i] = (in[0][i] + in[1][i]) * 0.5f;
    (void)mono;

    float l[kBlock], r[kBlock];
    synth::Process(l, r, (int)size);
    for(size_t i = 0; i < size; i++)
    {
        out[0][i] = l[i];
        out[1][i] = r[i];
    }
    for(size_t i = 0; i < size; i++)
    {
        const float m = l[i] < 0.0f ? -l[i] : l[i];
        if(m > pk)
            pk = m;
    }
    // 90% of the budget leaves the main loop the rest; the first second is
    // skipped with the SDRAM lines still cold.
    const uint32_t us = System::GetUs() - t0;
    if(blocks_done > 1000 && us * 10 > block_us * 9)
        over_n++;
    if(us > cb_max)
        cb_max = (uint16_t)(us > 65535 ? 65535 : us);
}

}  // namespace

int main()
{
    hw.Init();
    // Not blocking: the instrument has to work with nothing attached
    hw.StartLog(false);
    hw.SetAudioBlockSize(kBlock);
    hw.SetAudioSampleRate(kRate);

    const float sr = hw.AudioSampleRate();
    block_dt       = (float)kBlock / sr;
    block_us       = (uint32_t)(block_dt * 1e6f);
    synth::Init(sr, kBlock);

    // Flush denormals: every decaying tail falls into them otherwise
    {
        uint32_t fpscr;
        __asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
        fpscr |= (1u << 24);
        __asm volatile("vmsr fpscr, %0" ::"r"(fpscr));
        FPU->FPDSCR |= (1u << 24);
    }

    panel::Init(hw, hw.AudioCallbackRate() / (float)panel::kPollDiv);
    hw.StartAudio(AudioCallback);

    while(1)
    {
        // Paced by the callback: it is the only accurate clock here
        while(blocks_seen != blocks_done)
        {
            // Dropped, not caught up: grinding a backlog never finishes, and
            // the check is inside the loop because that is when it never exits.
            const uint32_t lag = blocks_done - blocks_seen;
            if(lag > kMaxLag)
            {
                blocks_seen = blocks_done - 1;
                now_s += block_dt * (float)(lag - 1);
                lag_n += (uint16_t)(lag - 1);
            }
            blocks_seen++;
            now_s += block_dt;

            // Quarter rate, the two halves on different blocks to spread
            // the work. Control cannot move off every block.
            if((blocks_seen % panel::kPollDiv) == 0)
                panel::PollPads();
            else if((blocks_seen % panel::kPollDiv) == 2)
                panel::Poll();
            {
                const uint32_t c0 = System::GetUs();
                synth::Control(now_s);
                const uint32_t cu = System::GetUs() - c0;
                if(cu > ctl_max)
                    ctl_max = (uint16_t)(cu > 65535 ? 65535 : cu);
            }

            const uint32_t now = System::GetNow();
            for(int p = 0; p < synth::kPads; p++)
            {
                const bool on = synth::PadOn(p);
                if(on && !was_on[p] && log_pad < 0)
                {
                    log_pad = p;
                    log_at  = now;
                    pk      = 0.0f;
                    voices::TakePeak(synth::PadVoice(p));
                }
                was_on[p] = on;
            }
            if(const int b = panel::TakeBlink())
                blink_until = now + (b == 2 ? 600 : 150);

            // A break chops the LED, so what you hear has something to see
            if(synth::TookBreak())
                stutter_until = now + 200;

            // Loudest envelope, so the LED follows pads and the HOLD floor
            // alike. A block is 1 ms, so this is an 8 ms PWM period.
            float lv = synth::Level();
            if(now < stutter_until && ((now / 25) & 1))
                lv = 0.0f;
            if(now < blink_until)
                lv = 1.0f - lv;
            const int duty = (int)(lv * 8.0f + 0.5f);
            // Cleared each second, not latched: a latch cannot tell one
            // cold block at startup from a board that is too slow.
            if(now - over_at >= 1000)
            {
                strobe  = over_n >= 4 ? 1 : (lag_n >= 4 ? 2 : 0);
                // Main loop only: each transmit blocks until the host takes it
                hw.PrintLine("cb %u us over %u | ctl %u us lag %u",
                             (unsigned)cb_max, (unsigned)over_n,
                             (unsigned)ctl_max, (unsigned)lag_n);
                over_n  = 0;
                lag_n   = 0;
                cb_max  = 0;
                ctl_max = 0;
                over_at = now;
            }
            if(log_pad >= 0 && now - log_at >= 400)
            {
                const int v = synth::PadVoice(log_pad);
                const int t = wfc::ShownTile(v);
                hw.PrintLine("P%02d voice %u of mix %u | dly %u ms fb %u"
                             " drv %u skew %u | dropped %u",
                             (unsigned)(log_pad + 3),
                             (unsigned)(voices::TakePeak(v) * 1000.0f),
                             (unsigned)(pk * 1000.0f),
                             (unsigned)(wfc::DR[wfc::TileD(t)]
                                        * wfc::DelayBase() * 1000.0f),
                             (unsigned)(wfc::FL[wfc::TileF(t)] * 100.0f),
                             (unsigned)(wfc::RL[wfc::TileR(t)] * 100.0f),
                             (unsigned)wfc::ShownSkew(v),
                             (unsigned)wfc::EventsDropped());
                hw.PrintLine("    dly %u fb %u drv %u flt %u mod %u ctl %u"
                             " unr %u hold %u | oct %d gain %d dB",
                             (unsigned)(synth::Knob(0) * 100.0f),
                             (unsigned)(synth::Knob(1) * 100.0f),
                             (unsigned)(synth::Knob(2) * 100.0f),
                             (unsigned)(synth::Knob(3) * 100.0f),
                             (unsigned)(synth::Knob(4) * 100.0f),
                             (unsigned)(synth::Knob(5) * 100.0f),
                             (unsigned)(synth::Knob(6) * 100.0f),
                             (unsigned)(synth::Knob(7) * 100.0f),
                             synth::Octave(), (int)synth::GainDb());
                log_pad = -1;
            }
            hw.SetLed(strobe ? ((now / (strobe == 1 ? 60 : 300)) & 1)
                             : (int)(blocks_seen & 7) < duty);
        }
    }
}
