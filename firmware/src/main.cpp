// THE TOUCH 2 SHELL
//
// The audio callback runs the engine; the main loop reads the panel and
// drives the LED. The engine itself is the same code the PC build runs.

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
float     now_s     = 0.0f;
float     block_dt  = 0.0f;
uint32_t  blink_until = 0, stutter_until = 0;

// SPIKE: does the render still fit in a block? Serial would give the exact
// load but libDaisy's USB stack is 12 KB of flash we do not have, and
// whether it fits is the whole question. Timed in the CALLBACK, where the
// rendering is -- a callback that runs long makes the main loop see fewer
// blocks, not a backlog, so watching the loop would miss it entirely.
// Latched and strobed on the LED: a glitch you have to listen for is a
// judgement call, a strobe is not.
// 32 kHz, not 48: the render costs the same per sample whatever the rate, so
// a third fewer samples is a third less CPU, and the wash has nothing up
// there to lose -- measured over the band, 8-16 kHz sits 63 dB under the
// signal and everything above 16 kHz, which is all that actually goes, sits
// 73 dB under. Seven voices at 48 kHz left the board at 96% with no room for
// a solve to land in.
constexpr auto kRate = SaiHandle::Config::SampleRate::SAI_32KHZ;
// Taken from the rate the hardware reports, not from a constant: this is the
// yardstick every overrun below is measured against.
uint32_t       block_us = 0;
volatile uint16_t  over_n   = 0;
// 8 ms of catch-up is plenty; past that the loop is not keeping up at all
constexpr uint32_t kMaxLag  = 8;
// Two things can fail to keep up and they want different fixes, so they
// are counted apart: the callback running long, and the main loop falling
// behind it. Worst block of the last second each, printed once a second.
volatile uint16_t  lag_n    = 0;
volatile uint16_t  cb_max   = 0;
uint16_t           ctl_max  = 0;
int                strobe   = 0;
uint32_t           over_at  = 0;

// The callback counts blocks; the main loop runs that many Control passes.
// Control is where the solver lives and a solve is most of a 1 ms block, so
// it cannot be in the callback -- but it still has to advance once per block
// or every smoothing time constant is wrong. Falling behind is caught up.
volatile uint32_t blocks_done = 0;
uint32_t          blocks_seen = 0;

// Why one press of a pad is loud and the next one quiet. The peak the
// callback actually puts out over the 400 ms after a press, printed next
// to the tile that press rolled -- the only way to tell a solver problem
// from a panel one, since the engine measures consistent on the desktop.
volatile float    pk          = 0.0f;
int               log_pad     = -1;
uint32_t          log_at      = 0;
bool              was_on[synth::kPads] = {};

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out,
                   size_t size)
{
    blocks_done++;
    const uint32_t t0 = System::GetUs();

    // ponytail: input mode is off while the synth side is being tuned. The
    // engine path and its tests are intact -- put `mono` back to re-enable.
    //
    // The loops are panned themselves, so the input goes in mono
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
    // 90% of the budget: the main loop needs the rest. The first second is
    // ignored, with the SDRAM lines and caches still cold.
    for(size_t i = 0; i < size; i++)
    {
        const float m = l[i] < 0.0f ? -l[i] : l[i];
        if(m > pk)
            pk = m;
    }
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

    // Denormals: every decaying delay tail in here would otherwise fall into
    // them and cost hundreds of cycles a sample
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
        // The audio callback is the only accurate clock here, so everything
        // below is paced by it rather than by a counted Delay(1).
        //
        while(blocks_seen != blocks_done)
        {
            // A block or two of lag is normal and gets caught up. A long
            // backlog must NOT be: one iteration costs more than a block
            // under load, so grinding through it never finishes --
            // blocks_done keeps moving, now_s advances a block per
            // iteration and so runs away from real time, and the
            // instrument tears itself apart. HOLD up with a rule rebuild
            // from the Delay knob is enough to start it.
            //
            // Checked HERE, every iteration, not before the loop: once a
            // pass costs more than a block this loop never exits, so a
            // check outside it never runs a second time and the drop that
            // is supposed to save the clock is unreachable exactly when it
            // is needed.
            const uint32_t lag = blocks_done - blocks_seen;
            if(lag > kMaxLag)
            {
                blocks_seen = blocks_done - 1;
                now_s += block_dt * (float)(lag - 1);
                lag_n += (uint16_t)(lag - 1);
            }
            blocks_seen++;
            now_s += block_dt;

            // The panel runs at a quarter of the block rate, and the two
            // halves of it land on different blocks so the work is spread
            // rather than bunched. Control cannot move: it is the per-block
            // smoothing and has to advance once per block.
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

            // A break chops the LED for a moment, so the thing you hear
            // happening has something to look at
            if(synth::TookBreak())
                stutter_until = now + 200;

            // The loudest voice's envelope, so the LED follows the pads and
            // the HOLD floor alike. One block is a millisecond, so this is
            // an 8 ms PWM period.
            float lv = synth::Level();
            if(now < stutter_until && ((now / 25) & 1))
                lv = 0.0f;
            if(now < blink_until)
                lv = 1.0f - lv;
            const int duty = (int)(lv * 8.0f + 0.5f);
            // Counted over the last second and cleared, not latched: a
            // latch cannot tell a single cold block at startup from a board
            // that is genuinely too slow, and the first version said both
            // the same way.
            if(now - over_at >= 1000)
            {
                strobe  = over_n >= 4 ? 1 : (lag_n >= 4 ? 2 : 0);
                // One line, from the main loop only: each transmit
                // blocks until the host takes it.
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
