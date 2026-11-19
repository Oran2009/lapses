// The Touch 2 shell: the callback runs the engine, the main loop the panel and LED.

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
// float32 stops advancing past 32768 s
double    now_s     = 0.0;
float     block_dt  = 0.0f;
uint32_t  blink_until = 0, stutter_until = 0;

// 48 kHz left the board at 96%
constexpr auto kRate = SaiHandle::Config::SampleRate::SAI_32KHZ;
// from the hardware, not a constant
uint32_t       block_us = 0;
volatile uint16_t  over_n   = 0;
// past this the loop is not keeping up
constexpr uint32_t kMaxLag  = 8;
// different fixes, so counted apart
volatile uint16_t  lag_n    = 0;
volatile uint16_t  cb_max   = 0;
uint16_t           ctl_max  = 0;
int                strobe   = 0;
uint32_t           over_at  = 0;

// out here, but once per block exactly
volatile uint32_t blocks_done = 0;
uint32_t          blocks_seen = 0;

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out,
                   size_t size)
{
    blocks_done++;
    const uint32_t t0 = System::GetUs();


    float l[kBlock], r[kBlock];
    synth::Process(l, r, (int)size);
    for(size_t i = 0; i < size; i++)
    {
        out[0][i] = l[i];
        out[1][i] = r[i];
    }
// 90% of the budget
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
    // not blocking
    hw.StartLog(false);
    hw.SetAudioBlockSize(kBlock);
    hw.SetAudioSampleRate(kRate);

    const float sr = hw.AudioSampleRate();
    block_dt       = (float)kBlock / sr;
    block_us       = (uint32_t)(block_dt * 1e6f);
    synth::Init(sr, kBlock);

    // every decaying tail falls into them
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
        // the only accurate clock here
        while(blocks_seen != blocks_done)
        {
            // dropped: grinding a backlog never finishes
            const uint32_t lag = blocks_done - blocks_seen;
            if(lag > kMaxLag)
            {
                blocks_seen = blocks_done - 1;
                now_s += block_dt * (float)(lag - 1);
                lag_n += (uint16_t)(lag - 1);
            }
            blocks_seen++;
            now_s += block_dt;

            // halves on different blocks
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
            if(const int b = panel::PopBlink())
                blink_until = now + (b == 2 ? 600 : 150);

            // a break chops the LED
            if(synth::PopBreak())
                stutter_until = now + 200;

            // 8 ms PWM period
            float lv = synth::Level();
            if(now < stutter_until && ((now / 25) & 1))
                lv = 0.0f;
            if(now < blink_until)
                lv = 1.0f - lv;
            const int duty = (int)(lv * 8.0f + 0.5f);
            // not latched: a cold start is not a slow board
            if(now - over_at >= 1000)
            {
                strobe  = over_n >= 4 ? 1 : (lag_n >= 4 ? 2 : 0);
                // blocks until the host takes it
                hw.PrintLine("cb %u us over %u | ctl %u us lag %u",
                             (unsigned)cb_max, (unsigned)over_n,
                             (unsigned)ctl_max, (unsigned)lag_n);
                over_n  = 0;
                lag_n   = 0;
                cb_max  = 0;
                ctl_max = 0;
                over_at = now;
            }
            hw.SetLed(strobe ? ((now / (strobe == 1 ? 60 : 300)) & 1)
                             : (int)(blocks_seen & 7) < duty);
        }
    }
}
