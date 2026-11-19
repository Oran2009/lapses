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
// chords over two octaves, not scales
const uint8_t kRatN[kScales][kPads] = {
    {1, 6, 3, 2, 12, 3, 4},    // just minor triad, what it has always played
    {1, 5, 3, 2, 5, 3, 4},     // the same shape, major
    {1, 3, 2, 5, 3, 7, 4},     // harmonics 2..8 of a 110 Hz fundamental
    {1, 4, 2, 8, 4, 16, 8},    // stacked fourths, no third at all
};
const uint8_t kRatD[kScales][kPads] = {
    {1, 5, 2, 1, 5, 1, 1},
    {1, 4, 2, 1, 2, 1, 1},
    {1, 2, 1, 2, 1, 2, 1},
    {1, 3, 1, 3, 1, 3, 1},
};
int scale = 0;
constexpr int kRoot = 9;   // A

// a rise over resting force, not a level
constexpr float kLean    = 0.25f;   // rise over resting that counts
constexpr float kFloor   = 0.3f;    // below this it is a brush, not a lean
constexpr float kRestUp  = 4.0f;    // seconds for rest to absorb a push
constexpr float kRestDn  = 0.15f;   // and to follow a finger easing off
constexpr float kRelax   = 0.10f;   // easing off this much re-arms a pad
// settling only
constexpr float kRestSettle = 0.05f;
float           pressure[kPads] = {};
float           rest[kPads]     = {};
double          rest_at[kPads]  = {};
float           peak[kPads]     = {};
bool            armed[kPads]    = {};
bool            settling[kPads] = {};

bool  held[kPads], latched[kPads];
bool  latch_on = false;
// latched pads sound untouched
inline bool PadActive(int p) { return held[p] || latched[p]; }
// changes land on the next press
float pad_hz[kPads];
const int kPadVoice[kPads] = {0, 1, 2, 3, 4, 5, 6};
int   octave    = 0;
int   gain_step = 0;    // 0 dB, the cap
float gain_lin  = 1.0f;
float sr        = 48000.0f;
int   block     = 48;

float RootHz()
{
    return 65.406f * powf(2.0f, (float)kRoot / 12.0f)
           * powf(2.0f, (float)(octave + 1));
}

// ------------------------------------------------------------- the master

// plate-ish FDN, 3.5 s
constexpr int kCombs = 4, kAps = 2;
const int     kCombLen[kCombs] = {1687, 1801, 2053, 2251};
// decorrelated, which is the width
constexpr int kSpread = 113;
const int     kApLen[kAps]     = {389, 127};
float SLOW_BSS comb_buf[2][kCombs][2432];
float SLOW_BSS ap_buf[2][kAps][512];
int            comb_i[2][kCombs], ap_i[2][kAps];
float          comb_lp[2][kCombs];
// predelay, or the allpass cancels the dry
constexpr int  kPre = 1440;
float SLOW_BSS pre_buf[2][kPre];
int            pre_i[2];

// fed above the mud
float          send_hp[2];
float          send_hp_c = 0.0f;
// SHIMMER on S36: its own line, an octave up
constexpr int  kShimGrain  = 4096;   // 128 ms
// triangles sum flat at two and four
constexpr int  kShimGrains = 4;
// twice the grain: a read can offset a window
constexpr int  kShimLen = kShimGrain * 2;
float SLOW_BSS shim_buf[2][kShimLen];
int            shim_wr = 0;
float          shim_ph[kShimGrains];
float          shim_off[kShimGrains];
uint32_t       shim_rng = 1;
float          shim_lp[2];
float          shim_lp_c = 0.0f;
float          shim_g    = 0.0f;
// unity shift, so the loop is stable
constexpr float kShimFb  = 0.75f;
// backed off by ear
constexpr float kShimIn  = 2.5f;

float ShimRd(const float* b, float d)
{
    const int   di = (int)d;
    const float fr = d - (float)di;
    int         i0 = shim_wr - di;
    if(i0 < 0)
        i0 += kShimLen;
    int i1 = i0 - 1;
    if(i1 < 0)
        i1 += kShimLen;
    return b[i0] + (b[i1] - b[i0]) * fr;
}

float Verb(int ch, float x)
{
    const float pre = pre_buf[ch][pre_i[ch]];
    pre_buf[ch][pre_i[ch]] = x;
    pre_i[ch]              = (pre_i[ch] + 1) % kPre;
    x                      = pre;

    float sum = 0.0f;
    for(int c = 0; c < kCombs; c++)
    {
        float* b = comb_buf[ch][c];
        int&   i = comb_i[ch][c];
        const float y = b[i];
        // brighter than the loops
        comb_lp[ch][c] += (y - comb_lp[ch][c]) * 0.16f;
        b[i] = x + comb_lp[ch][c] * 0.96f;
        i    = (i + 1) % (kCombLen[c] + (ch ? kSpread : 0));
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

// only catches the breaks
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

double now_s = 0.0;

bool  user_freeze = false;


void SyncSounding(double now)
{
    for(int p = 0; p < kPads; p++)
    {
        const int v = kPadVoice[p];
        if(v < 0)
            continue;
        voices::SetPadHeld(v, PadActive(p));
        const bool on = PadActive(p) || voices::HoldSustains(v);
        if(on != wfc::Sounding(v))
            wfc::SetSounding(v, on, now);
    }
}

}  // namespace

const char* KnobName(int i)
{
    static const char* n[kKnobs]
        = {"Delay",   "Feedback", "Distortion", "Filter", "Mod",
           "Control", "Unrest",   "Hold",       "Link",   "Shimmer"};
    return i >= 0 && i < kKnobs ? n[i] : "?";
}

void Init(float sample_rate, int block_size)
{
    sr    = sample_rate;
    block = block_size;
    for(int p = 0; p < kPads; p++)
    {
        held[p] = latched[p] = false;
        pad_hz[p]   = 0.0f;
        pressure[p] = 0.0f;
        rest[p]     = -1.0f;
        rest_at[p]  = 0.0f;
        armed[p]    = true;
    }
    latch_on       = false;
    octave         = 0;
    wfc::K().hold = 0.0f;
    now_s          = 0.0;
    user_freeze = false;
    ShiftGain(0);

    memset(comb_buf, 0, sizeof(comb_buf));
    memset(ap_buf, 0, sizeof(ap_buf));
    memset(comb_i, 0, sizeof(comb_i));
    memset(ap_i, 0, sizeof(ap_i));
    memset(comb_lp, 0, sizeof(comb_lp));
    memset(pre_buf, 0, sizeof(pre_buf));
    memset(pre_i, 0, sizeof(pre_i));
    memset(send_hp, 0, sizeof(send_hp));
    memset(shim_buf, 0, sizeof(shim_buf));
    memset(shim_lp, 0, sizeof(shim_lp));
    shim_wr = 0;
    for(int g = 0; g < kShimGrains; g++)
    {
        shim_ph[g]  = (float)kShimGrain * (float)g / (float)kShimGrains;
        shim_off[g] = 0.0f;
    }
    shim_rng  = 1;
    shim_g    = 0.0f;
    shim_lp_c = 1.0f - expf(-2.0f * 3.14159265f * 6000.0f / sr);
    send_hp_c = 1.0f - expf(-2.0f * 3.14159265f * 180.0f / sr);
    lim_env = 0.0f;

    scale = 0;
    wfc::Init(0x5afe6a7du);
    voices::Init(sample_rate, block_size);

    // untouched voices must not share one default
    for(int p = 0; p < kPads; p++)
        if(kPadVoice[p] >= 0)
            voices::SetPitch(kPadVoice[p], PadHz(p), 2.0f * RootHz(), false);
}

float PadHz(int p)
{
    if(PadActive(p) && pad_hz[p] > 0.0f)
        return pad_hz[p];
    return RootHz() * (float)kRatN[scale][p] / (float)kRatD[scale][p];
}

int  PadVoice(int p) { return kPadVoice[p]; }
bool PadOn(int p) { return PadActive(p); }

void SetPressure(int pad, float p)
{
    if(pad < 0 || pad >= kPads)
        return;
    pressure[pad] = p;
    // the idle zeroes ate the reseed
    if(!held[pad])
    {
        rest[pad] = -1.0f;
        return;
    }
    // no resting force yet: seed it
    if(rest[pad] < 0.0f)
    {
        rest[pad]    = p;
        rest_at[pad] = now_s;
        // disarmed too, or the ramp reads as a lean
        armed[pad]    = false;
        settling[pad] = true;
        peak[pad]     = p;
        return;
    }
    const float lean = p - rest[pad];
    if(!armed[pad])
    {
        if(p > peak[pad])
            peak[pad] = p;
        // fresh arms on the seed, broken on the relax
        if(lean < kLean * 0.4f || p < peak[pad] - kRelax)
        {
            armed[pad]    = true;
            settling[pad] = false;
        }
    }
    else if(lean >= kLean && p >= kFloor && held[pad])
    {
        armed[pad] = false;
        peak[pad]  = p;
        const int v = kPadVoice[pad];
        if(v >= 0)
            wfc::BreakRule(-1, v / wfc::kGroupSize, 0.0f, now_s, v);
    }
    // after the test: measured from where the finger was
    const float dt = (float)(now_s - rest_at[pad]);
    rest_at[pad]   = now_s;
    // settling only
    const float up = settling[pad] ? kRestSettle : kRestUp;
    if(dt > 0.0f)
        rest[pad] += (p - rest[pad])
                     * (1.0f - expf(-dt / (p > rest[pad] ? up : kRestDn)));
}

float PressureRest(int pad)
{
    return (pad >= 0 && pad < kPads && rest[pad] > 0.0f) ? rest[pad] : 0.0f;
}

float Pressure(int pad)
{
    return (pad >= 0 && pad < kPads) ? pressure[pad] : 0.0f;
}

void Press(int pad)
{
    if(held[pad])
        return;
    held[pad]   = true;
    pad_hz[pad] = RootHz() * (float)kRatN[scale][pad] / (float)kRatD[scale][pad];
    const int v = kPadVoice[pad];
    if(v < 0)
    {
        return;
    }
    voices::SetPitch(v, PadHz(pad), 2.0f * RootHz(), true);
    wfc::SetSounding(v, true, now_s);
    SyncSounding(now_s);
    wfc::Pass(v, now_s);
}

void Release(int pad)
{
    held[pad]     = false;
    pressure[pad] = 0.0f;
    rest[pad]     = -1.0f;   // reseed on the next press
    armed[pad]    = true;
    SyncSounding(now_s);
    const int v = kPadVoice[pad];
    // no tail to re-roll
    if(v >= 0 && !latched[pad])
        wfc::RerollTail(v, now_s);
}

// edge only
void Latch(bool on)
{
    if(on == latch_on)
        return;
    latch_on = on;
    for(int p = 0; p < kPads; p++)
        latched[p] = on && held[p];
    SyncSounding(now_s);
}

bool Latched(int p) { return latched[p]; }

bool ShiftOctave(int delta)
{
    const int was = octave;
    octave += delta;
    if(octave > 2)
        octave = 2;
    else if(octave < -2)
        octave = -2;
    return octave != was;
}

int Octave() { return octave; }

// next press only, via pad_hz
void NextScale() { scale = (scale + 1) % kScales; }
int  Scale() { return scale; }
const char* ScaleName()
{
    static const char* const kName[kScales]
        = {"minor", "major", "harmonic", "fourths"};
    return kName[scale];
}

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
    // or ADC jitter rebuilds the rules constantly
    if(i < 0 || i >= kKnobs || fabsf(k[i] - val) < 0.004f)
        return;
    k[i] = val;
    // only these reach the rules
    if(i == 0 || i == 1 || i == 2)
        wfc::RebuildRules();
    wfc::KnobsMoved(now_s);
}

float Knob(int i) { return (&wfc::K().delay)[i]; }

// block-rate work is audible as a tone
constexpr float kIdleFloor  = 1.0e-5f;   // -100 dBFS
constexpr float kIdleHold_s = 8.0f;      // outlasts the plate
int             idle_blocks = 0;
bool            idle        = false;

void Control(double now)
{
    now_s = now;
    SyncSounding(now);
    // the one place that wakes the chain
    for(int v = 0; v < wfc::kVoices; v++)
        if(wfc::Sounding(v))
        {
            idle_blocks = 0;
            idle        = false;
            break;
        }
    wfc::Tick(now);
    voices::Control(now, (float)block / sr);
    for(int v = 0; v < wfc::kVoices; v++)
        wfc::SetLevel(v, voices::BusLvl(v));
}

void Process(float* l, float* r, int n)
{
    for(int i = 0; i < n; i++)
        l[i] = r[i] = 0.0f;
    if(idle)
        return;
    voices::Process(l, r, n);
    // fully wet matches fully dry
    constexpr float kSpace = 0.3f;
    constexpr float send   = 1.8f * kSpace;
    constexpr float dry    = 1.0f - 0.8f * kSpace;
    // stepping once a block zippered
    const float sk = wfc::K().shimmer < 0.0f
                         ? 0.0f
                         : (wfc::K().shimmer > 1.0f ? 1.0f : wfc::K().shimmer);
    shim_g += (sk - shim_g)
              * (1.0f - expf(-(float)n / sr / 0.05f));
    for(int i = 0; i < n; i++)
    {
        const float sl = l[i], sr_ = r[i];
        send_hp[0] += (sl - send_hp[0]) * send_hp_c;
        send_hp[1] += (sr_ - send_hp[1]) * send_hp_c;
        float up[2] = {0.0f, 0.0f};
        for(int g = 0; g < kShimGrains; g++)
        {
            // 2x read rate = the octave
            shim_ph[g] -= 1.0f;
            if(shim_ph[g] < 0.0f)
            {
                shim_ph[g] += (float)kShimGrain;
                shim_rng    = shim_rng * 1664525u + 1013904223u;
                shim_off[g] = (float)(shim_rng >> 19) * (float)kShimGrain
                              / 8192.0f;
            }
            // the gains must sum to 1
            const float w = 1.0f - fabsf(shim_ph[g] - kShimGrain * 0.5f)
                                       / (kShimGrain * 0.5f);
            const float d = shim_ph[g] + shim_off[g];
            for(int c = 0; c < 2; c++)
                up[c] += ShimRd(shim_buf[c], d) * w;
        }
        // sums flat
        for(int c = 0; c < 2; c++)
            shim_lp[c] += (up[c] * (2.0f / kShimGrains) - shim_lp[c])
                          * shim_lp_c;
        const float wetl     = Verb(0, (sl - send_hp[0]) * send);
        const float wetr     = Verb(1, (sr_ - send_hp[1]) * send);
        const float fb       = kShimFb * shim_g;
        shim_buf[0][shim_wr] = wetl * kShimIn + shim_lp[0] * fb;
        shim_buf[1][shim_wr] = wetr * kShimIn + shim_lp[1] * fb;
        shim_wr              = shim_wr + 1 >= kShimLen ? 0 : shim_wr + 1;
        l[i] = Limit((sl * dry + (wetl + shim_lp[0] * shim_g) * 0.7f)
                     * gain_lin);
        r[i] = Limit((sr_ * dry + (wetr + shim_lp[1] * shim_g) * 0.7f)
                     * gain_lin);
    }

    float peak = 0.0f;
    for(int i = 0; i < n; i++)
    {
        const float a = fabsf(l[i]), b = fabsf(r[i]);
        if(a > peak)
            peak = a;
        if(b > peak)
            peak = b;
    }
    if(peak > kIdleFloor)
        idle_blocks = 0;
    else if(++idle_blocks > (int)(kIdleHold_s * sr / (float)n))
        idle = true;
}

void Break() { wfc::BreakRule(-1, 0.0f, now_s); }
void Settle() { wfc::Heal(now_s); }

void Freeze(bool on)
{
    user_freeze = on;
    wfc::SetFrozen(on);
}
bool Frozen() { return wfc::Frozen(); }

float Level() { return voices::Level(); }
bool  TookBreak() { return wfc::TookBreak(); }

}  // namespace synth
