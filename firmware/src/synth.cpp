#include "synth.h"

#include "voices.h"

#include <math.h>
#include <string.h>

#ifdef __arm__
#define SLOW_BSS __attribute__((section(".sdram_bss")))
#else
#define SLOW_BSS
#endif

namespace synth
{
namespace
{
// Major, 5-limit. Scale and root are fixed (HANDOFF 4.1).
const int kRatN[kPads] = {1, 9, 5, 4, 3, 5, 15};
const int kRatD[kPads] = {1, 8, 4, 3, 2, 3, 8};
constexpr int kRoot = 9;   // A

bool  held[kPads], latched[kPads];
int   pad_voice[kPads], voice_pad[wfc::kVoices];
int   octave    = 0;
int   gain_step = -4;   // -12 dB, safe for headphones
float gain_lin  = 1.0f;
float sr        = 48000.0f;
int   block     = 48;

float RootHz()
{
    return 65.406f * powf(2.0f, (float)kRoot / 12.0f)
           * powf(2.0f, (float)(octave + 1));
}

// ------------------------------------------------------------- the master

// A plate-ish FDN standing in for the prototype's 3.6 s convolution. Tune by
// ear against the web version (HANDOFF 6: the reverb level was a guess).
constexpr int kCombs = 4, kAps = 2;
const int     kCombLen[kCombs] = {1687, 1801, 2053, 2251};
const int     kApLen[kAps]     = {389, 127};
float SLOW_BSS comb_buf[2][kCombs][2304];
float SLOW_BSS ap_buf[2][kAps][512];
int            comb_i[2][kCombs], ap_i[2][kAps];
float          comb_lp[2][kCombs];
// The reverb is fed above the mud, not through it
float          send_hp[2];
float          send_hp_c = 0.0f;

float Verb(int ch, float x)
{
    float sum = 0.0f;
    for(int c = 0; c < kCombs; c++)
    {
        float* b = comb_buf[ch][c];
        int&   i = comb_i[ch][c];
        const float y = b[i];
        // A darkening tail, as the prototype's IR has
        comb_lp[ch][c] += (y - comb_lp[ch][c]) * 0.38f;
        b[i] = x + comb_lp[ch][c] * 0.82f;
        i    = (i + 1) % kCombLen[c];
        sum += y;
    }
    sum *= 0.25f;
    for(int a = 0; a < kAps; a++)
    {
        float* b = ap_buf[ch][a];
        int&   i = ap_i[ch][a];
        const float y = b[i];
        const float w = sum + y * 0.5f;
        b[i]          = w;
        i             = (i + 1) % kApLen[a];
        sum           = y - w * 0.5f;
    }
    return sum;
}

// The measured peak out of the voices is about 1.0, so this sits just above
// it and only catches the breaks (HANDOFF 6: the compressor was a guess).
float lim_env = 0.0f;

float Limit(float x)
{
    const float a = fabsf(x);
    if(a > lim_env)
        lim_env += (a - lim_env) * 0.3f;          // attack
    else
        lim_env += (a - lim_env) * 0.0008f;       // release
    const float g = lim_env > 1.0f ? 1.0f / lim_env : 1.0f;
    const float y = x * g;
    return y > 1.0f ? 1.0f : (y < -1.0f ? -1.0f : y);
}

bool PadActive(int p) { return held[p] || latched[p]; }

bool VoiceLive(int v)
{
    const int p = voice_pad[v];
    return p >= 0 && PadActive(p) && pad_voice[p] == v;
}

// The pad's own voice if it still sounds, else the quietest free one, else
// steal the oldest and take its pad away from it
int Allocate(int pad, float now)
{
    const int cur = pad_voice[pad];
    if(cur >= 0 && VoiceLive(cur))
        return cur;

    int   best = -1;
    float lo   = 2.0f;
    for(int v = 0; v < wfc::kVoices; v++)
        if(!VoiceLive(v) && voices::Amp(v) < lo)
        {
            lo   = voices::Amp(v);
            best = v;
        }
    if(best >= 0)
        return best;

    // Take a parked pad's voice before a held one's: a finger on a pad
    // outranks one that was latched and left
    int oldest = -1;
    for(int pass = 0; pass < 2 && oldest < 0; pass++)
        for(int v = 0; v < wfc::kVoices; v++)
        {
            const int p = voice_pad[v];
            if(pass == 0 && !(p >= 0 && latched[p] && !held[p]))
                continue;
            if(oldest < 0 || wfc::StartedAt(v) < wfc::StartedAt(oldest))
                oldest = v;
        }
    // It keeps its latch, so it takes a voice back once one frees up
    if(voice_pad[oldest] >= 0)
        pad_voice[voice_pad[oldest]] = -1;
    (void)now;
    return oldest;
}

// Pushes every pad's sounding state into WFC, which is what the rules read
void SyncSounding(float now)
{
    for(int v = 0; v < wfc::kVoices; v++)
    {
        const bool on = VoiceLive(v);
        if(on != wfc::Sounding(v))
            wfc::SetSounding(v, on, now);
    }
}

// Hands `pad` voice `v`, re-rolling it
void Give(int pad, int v, float now)
{
    pad_voice[pad] = v;
    voice_pad[v]   = pad;
    voices::SetPitch(v, PadHz(pad), true);
    wfc::SetSounding(v, true, now);
    SyncSounding(now);
    wfc::Pass(v, now);
}

// An active pad that lost its voice to a steal takes a free one back, so
// releasing a borrowed pad hands the latched ones their voices again
void Reclaim(float now)
{
    for(int p = 0; p < kPads; p++)
    {
        if(!PadActive(p) || pad_voice[p] >= 0)
            continue;
        for(int v = 0; v < wfc::kVoices; v++)
            if(!VoiceLive(v))
            {
                Give(p, v, now);
                break;
            }
    }
}

float now_s = 0.0f;

}  // namespace

const char* KnobName(int i)
{
    static const char* n[6]
        = {"Delay", "Feedback", "Distortion", "Filter", "LFO", "Control"};
    return i >= 0 && i < 6 ? n[i] : "?";
}

void Init(float sample_rate, int block_size)
{
    sr    = sample_rate;
    block = block_size;
    for(int p = 0; p < kPads; p++)
    {
        held[p] = latched[p] = false;
        pad_voice[p]         = -1;
    }
    for(int v = 0; v < wfc::kVoices; v++)
        voice_pad[v] = -1;
    octave = 0;
    ShiftGain(0);

    memset(comb_buf, 0, sizeof(comb_buf));
    memset(ap_buf, 0, sizeof(ap_buf));
    memset(comb_i, 0, sizeof(comb_i));
    memset(ap_i, 0, sizeof(ap_i));
    memset(comb_lp, 0, sizeof(comb_lp));
    memset(send_hp, 0, sizeof(send_hp));
    send_hp_c = 1.0f - expf(-2.0f * 3.14159265f * 300.0f / sr);
    lim_env = 0.0f;

    wfc::Init(0x5afe6a7du);
    voices::Init(sample_rate, block_size);
}

float PadHz(int p)
{
    return RootHz() * (float)kRatN[p] / (float)kRatD[p];
}

int  PadVoice(int p) { return pad_voice[p]; }
bool PadOn(int p) { return PadActive(p); }
bool Latched()
{
    for(int p = 0; p < kPads; p++)
        if(latched[p])
            return true;
    return false;
}

void Press(int pad)
{
    if(held[pad])
        return;
    held[pad] = true;
    Give(pad, Allocate(pad, now_s), now_s);
}

void Release(int pad)
{
    held[pad] = false;
    if(!latched[pad])
        pad_voice[pad] = -1;
    SyncSounding(now_s);
    Reclaim(now_s);
}

void Latch(bool on)
{
    for(int p = 0; p < kPads; p++)
        latched[p] = on ? (latched[p] || held[p]) : false;
    for(int p = 0; p < kPads; p++)
        if(!PadActive(p))
            pad_voice[p] = -1;
    SyncSounding(now_s);
    Reclaim(now_s);
}

bool ShiftOctave(int delta)
{
    const int was = octave;
    octave += delta;
    if(octave > 2)
        octave = 2;
    else if(octave < -2)
        octave = -2;
    if(octave == was)
        return false;
    // Every sounding pad follows, gliding rather than jumping
    for(int p = 0; p < kPads; p++)
        if(PadActive(p) && pad_voice[p] >= 0)
            voices::SetPitch(pad_voice[p], PadHz(p), false);
    return true;
}

int Octave() { return octave; }

bool ShiftGain(int delta)
{
    const int was = gain_step;
    gain_step += delta;
    if(gain_step > 0)
        gain_step = 0;
    else if(gain_step < -10)
        gain_step = -10;
    gain_lin = powf(10.0f, (float)gain_step * 3.0f / 20.0f);
    return gain_step != was;
}

float GainDb() { return (float)gain_step * 3.0f; }

void SetKnob(int i, float val)
{
    float* k = &wfc::K().delay;
    if(i < 0 || i > 5 || k[i] == val)
        return;
    k[i] = val;
    wfc::RebuildRules();
    wfc::KnobsMoved(now_s);
}

float Knob(int i) { return (&wfc::K().delay)[i]; }

void Control(float now)
{
    now_s = now;
    wfc::Tick(now);
    voices::Control(now, (float)block / sr);
}

void Process(float* l, float* r, int n)
{
    for(int i = 0; i < n; i++)
        l[i] = r[i] = 0.0f;
    voices::Process(l, r, n);
    for(int i = 0; i < n; i++)
    {
        const float sl = l[i], sr_ = r[i];
        send_hp[0] += (sl - send_hp[0]) * send_hp_c;
        send_hp[1] += (sr_ - send_hp[1]) * send_hp_c;
        l[i] = Limit((sl + Verb(0, (sl - send_hp[0]) * 0.22f) * 0.7f) * gain_lin);
        r[i] = Limit((sr_ + Verb(1, (sr_ - send_hp[1]) * 0.22f) * 0.7f) * gain_lin);
    }
}

float Level() { return voices::Level(); }

}  // namespace synth
