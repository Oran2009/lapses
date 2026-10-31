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
uint32_t  blink_until = 0;

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
        // below is paced by it rather than by a counted Delay(1). Falling
        // behind is caught up; the rates stay right either way.
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

            // On with the pads, fading off with the release. One block is a
            // millisecond, so this is an 8 ms PWM period
            float lv = synth::Level();
            if(now < blink_until)
                lv = 1.0f - lv;
            const int duty = (int)(lv * 8.0f + 0.5f);
            hw.SetLed((int)(blocks_seen & 7) < duty);
        }
    }
}
