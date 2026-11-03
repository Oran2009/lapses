// THE TOUCH 2 SHELL
//
// The audio callback runs the engine; the main loop reads the panel and
// drives the LED. The engine itself is the same code the PC build runs.

#include "panel.h"
#include "synth.h"

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
constexpr uint32_t kBlockUs = 1000 * kBlock / 48;   // 48 kHz
volatile uint16_t  over_n   = 0;
// 8 ms of catch-up is plenty; past that the loop is not keeping up at all
constexpr uint32_t kMaxLag  = 8;
bool               strobe   = false;
uint32_t           over_at  = 0;

// The callback counts blocks; the main loop runs that many Control passes.
// Control is where the solver lives and a solve is most of a 1 ms block, so
// it cannot be in the callback -- but it still has to advance once per block
// or every smoothing time constant is wrong. Falling behind is caught up.
volatile uint32_t blocks_done = 0;
uint32_t          blocks_seen = 0;

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
    const uint32_t us = System::GetUs() - t0;
    if(blocks_done > 1000 && us * 10 > kBlockUs * 9)
        over_n++;
}

}  // namespace

int main()
{
    hw.Init();
    hw.SetAudioBlockSize(kBlock);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

    const float sr = hw.AudioSampleRate();
    block_dt       = (float)kBlock / sr;
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
        // A block or two of lag is normal and gets caught up. A long
        // backlog must NOT be: one iteration costs more than a block under
        // load, so grinding through it never finishes -- blocks_done keeps
        // moving, now_s advances a block per iteration and so runs away
        // from real time, and the instrument tears itself apart. HOLD up
        // with a rule rebuild from the Delay knob is enough to start it.
        // Drop the backlog and keep the clock honest instead.
        const uint32_t lag = blocks_done - blocks_seen;
        if(lag > kMaxLag)
        {
            blocks_seen = blocks_done - 1;
            now_s += block_dt * (float)(lag - 1);
            over_n += (uint16_t)(lag - 1);
        }
        while(blocks_seen != blocks_done)
        {
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
            synth::Control(now_s);

            const uint32_t now = System::GetNow();
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
                strobe  = over_n >= 4;
                over_n  = 0;
                over_at = now;
            }
            hw.SetLed(strobe ? ((now / 60) & 1)
                             : (int)(blocks_seen & 7) < duty);
        }
    }
}
