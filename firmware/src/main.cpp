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

void AudioCallback(AudioHandle::InputBuffer, AudioHandle::OutputBuffer out,
                   size_t size)
{
    now_s += block_dt;
    synth::Control(now_s);

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

    panel::Init(hw);
    hw.StartAudio(AudioCallback);

    while(1)
    {
        // The pads are an I2C transaction, so they are polled far more often
        // than anything else in here
        for(int i = 0; i < 500; i++)
        {
            panel::PollPads();
            panel::Poll();

            const uint32_t now = System::GetNow();
            if(const int b = panel::TakeBlink())
                blink_until = now + (b == 2 ? 600 : 150);

            // On with the pads, fading off with the release
            float lv = synth::Level();
            if(now < blink_until)
                lv = 1.0f - lv;
            const int duty = (int)(lv * 8.0f + 0.5f);
            hw.SetLed((i & 7) < duty);
            System::Delay(1);
        }
    }
}
