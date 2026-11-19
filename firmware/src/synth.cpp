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
// Chords over two octaves, not scales inside one: 7 simultaneous voices in
// one critical band mud up. Every row >= the minor's 0.91 ERB at 220/264.
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

// A lean is a rise over the pad's own resting force, not a level: how hard
// a finger rests varies, so an absolute threshold can never re-arm.
constexpr float kLean    = 0.25f;   // rise over resting that counts
constexpr float kFloor   = 0.3f;    // below this it is a brush, not a lean
// 0.8 made the threshold a rate, not a size: rest ate the lean and a push of
// exactly kLean never fired at any speed. Measured over 203 board presses.
constexpr float kRestUp  = 4.0f;    // seconds for rest to absorb a push
constexpr float kRestDn  = 0.15f;   // and to follow a finger easing off
constexpr float kRelax   = 0.10f;   // easing off this much re-arms a pad
// Settling only: after a break rest must stay put, or the leaned-on force
// becomes the resting one and the relax cannot get under it to re-arm.
constexpr float kRestSettle = 0.05f;
float           pressure[kPads] = {};
float           rest[kPads]     = {};
double          rest_at[kPads]  = {};
float           peak[kPads]     = {};
bool            armed[kPads]    = {};
bool            settling[kPads] = {};

bool  held[kPads], latched[kPads];
bool  latch_on = false;
// A latched pad sounds with nothing touching it, so this and not contact is
// what "is this pad playing" means.
inline bool PadActive(int p) { return held[p] || latched[p]; }
// The pitch a pad was triggered with: octave and scale changes land on the
// next press, not on what is already sounding.
float pad_hz[kPads];
const int kPadVoice[kPads] = {0, 1, 2, 3, 4, 5, 6};
int   octave    = 0;
int   gain_step = 0;    // 0 dB, the cap: -12 was four presses short in use.
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
// Right-channel combs a few samples longer: decorrelates the plate from the
// mono delays, which is what makes the tail read as width.
constexpr int kSpread = 113;
const int     kApLen[kAps]     = {389, 127};
float SLOW_BSS comb_buf[2][kCombs][2432];
float SLOW_BSS ap_buf[2][kAps][512];
int            comb_i[2][kCombs], ap_i[2][kAps];
float          comb_lp[2][kCombs];
// 30 ms predelay: without it the early output correlates with the dry, the
// allpass inversion cancels part of it, and the send reads as a tone control.
constexpr int  kPre = 1440;
float SLOW_BSS pre_buf[2][kPre];
int            pre_i[2];
// The line input is detected, not switched: engages on any signal, releases
// 3 s after it falls quiet.
float          in_env = 0.0f, in_hold = 0.0f, in_mix = 0.0f;

// The reverb is fed above the mud, not through it
float          send_hp[2];
float          send_hp_c = 0.0f;
// SHIMMER, on S36: its own line fed by the plate, regenerating an octave up.
// Not the plate itself -- 28 dB comb peaks. Grained, or the comb repeats.
constexpr int  kShimGrain  = 4096;   // 128 ms, the length that measured best
// Four: triangles sum flat only at two and four, and three ripples 22%
constexpr int  kShimGrains = 4;
// Twice the grain, so a grain's read can be offset by up to a whole window
// without ever wrapping inside itself
constexpr int  kShimLen = kShimGrain * 2;
float SLOW_BSS shim_buf[2][kShimLen];
int            shim_wr = 0;
float          shim_ph[kShimGrains];
float          shim_off[kShimGrains];
uint32_t       shim_rng = 1;
// Every pass is another octave, so the tail parks at the top without this.
// 6 kHz: at 2 kHz the second pass was gone and the fader read as a level.
float          shim_lp[2];
float          shim_lp_c = 0.0f;
float          shim_g    = 0.0f;
// The shift is unity, so the loop is stable for any feedback under one; this
// leaves a few passes audible at the top of the fader and stops there.
constexpr float kShimFb  = 0.75f;
// How loud the top of the fader is, backed off by ear from 4.0. The floor is
// near: at 2.0 the shimmer stops outliving the dry tail.
constexpr float kShimIn  = 2.5f;

// One interpolated read,  samples behind the write head
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
        // Darkening tail, as the prototype's IR has, but brighter than the
        // hard-lowpassed loops or it cannot be told apart from them.
        comb_lp[ch][c] += (y - comb_lp[ch][c]) * 0.16f;
        // 0.82 over 35..47 ms combs was a small room; this is the prototype's
        // three-and-a-half seconds
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

double now_s = 0.0;

bool  user_freeze = false;


// Pushes every pad's sounding state into WFC, which is what the rules read
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
    in_env = in_hold = in_mix = 0.0f;
    send_hp_c = 1.0f - expf(-2.0f * 3.14159265f * 180.0f / sr);
    lim_env = 0.0f;

    scale = 0;
    wfc::Init(0x5afe6a7du);
    voices::Init(sample_rate, block_size);

    // HOLD floors every voice, pressed or not, so untouched ones must not sit
    // on one default. Not snapped: that marks them fresh, and fresh voices duck.
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
    // Both shells poll every pad, held or not: without this the idle zeroes
    // ate the reseed and every press read as a lean of its own full size.
    if(!held[pad])
    {
        rest[pad] = -1.0f;
        return;
    }
    // A fresh press has no resting force yet, so seed it rather than read a
    // rise from zero and break on every single press
    if(rest[pad] < 0.0f)
    {
        rest[pad]    = p;
        rest_at[pad] = now_s;
        // Disarmed as well as seeded: the panel's ramp leaves this sample
        // half way up, and the rest read as a lean -- 40 presses, 40 breaks.
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
        // Two ways in: a fresh press arms when the seed catches up, a broken
        // one on the relax. Rest alone lost 2/3 of lean-relax-lean gestures.
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
    // After the test, so a push is measured against where the finger was. A
    // time constant, not a rate: 28 ms a pad on the board, a block here.
    const float dt = (float)(now_s - rest_at[pad]);
    rest_at[pad]   = now_s;
    // Settling only: at kRestUp the rest of the panel's ramp read as a lean
    // for 1.5 s. After a break the relax re-arms, so rest must stay put.
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
    // A latched pad's note did not end, so there is no tail to re-roll
    if(v >= 0 && !latched[pad])
        wfc::RerollTail(v, now_s);
}

// S07/S08 down. Edge only -- it keeps the pads held at that moment, and a
// later press plays while touched.
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

// Next press only, via pad_hz above
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
    // The ADCs jitter, and all eight knobs are written every poll: without a
    // deadband that rebuilt the rules a thousand times a second.
    if(i < 0 || i >= kKnobs || fabsf(k[i] - val) < 0.004f)
        return;
    k[i] = val;
    // Only these reach the rules, and the rebuild is 10,000 tile pairs
    if(i == 0 || i == 1 || i == 2)
        wfc::RebuildRules();
    wfc::KnobsMoved(now_s);
}

float Knob(int i) { return (&wfc::K().delay)[i]; }

// Silence costs the same to render as sound and the block-rate work is
// audible as a tone, so the chain stops. Threshold under the reverb's tail.
constexpr float kIdleFloor  = 1.0e-5f;   // -100 dBFS
constexpr float kIdleHold_s = 8.0f;      // longer than the plate's 5 s tail
int             idle_blocks = 0;
bool            idle        = false;

void Control(double now)
{
    now_s = now;
    SyncSounding(now);
    // The one place that wakes the chain. Held pads alone left the HOLD
    // fader unable to: raising it fed a chain that was no longer rendering.
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

float InputMix() { return in_mix; }

void Process(float* l, float* r, int n, const float* in)
{
    // Still asleep: the buffers the caller gets are the silence it asked for
    if(idle && !in)
    {
        for(int i = 0; i < n; i++)
            l[i] = r[i] = 0.0f;
        return;
    }

    if(in)
    {
        float peak = 0.0f;
        for(int i = 0; i < n; i++)
            if(fabsf(in[i]) > peak)
                peak = fabsf(in[i]);
        in_env += (peak - in_env) * (peak > in_env ? 0.5f : 0.02f);
        const float dt = (float)n / sr;
        in_hold = in_env > 0.003f ? 3.0f
                                  : (in_hold > dt ? in_hold - dt : 0.0f);
    }
    else
        in_hold = 0.0f;
    // A fade, not a mix: it only ever settles at one end or the other
    in_mix += ((in_hold > 0.0f ? 1.0f : 0.0f) - in_mix)
              * (1.0f - expf(-(float)n / sr / 0.15f));

    for(int i = 0; i < n; i++)
        l[i] = r[i] = 0.0f;
    voices::Process(l, r, n, in, in_mix);
    // The input's own path to the bus, once, centred
    if(in)
        for(int i = 0; i < n; i++)
            l[i] += in[i] * in_mix * 0.5f, r[i] += in[i] * in_mix * 0.5f;
    // Fixed, not a knob. 1.8 is measured: fully wet matches fully dry, where
    // 2.2 hit the ceiling and the limiter pumped on every burst.
    constexpr float kSpace = 0.3f;
    constexpr float send   = 1.8f * kSpace;
    constexpr float dry    = 1.0f - 0.8f * kSpace;
    // A fader is swept, and stepping the regeneration once a block zippered
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
            // -1 per sample: the read advances at 2x the write rate, which
            // is the octave.
            shim_ph[g] -= 1.0f;
            if(shim_ph[g] < 0.0f)
            {
                shim_ph[g] += (float)kShimGrain;
                shim_rng    = shim_rng * 1664525u + 1013904223u;
                shim_off[g] = (float)(shim_rng >> 19) * (float)kShimGrain
                              / 8192.0f;
            }
            // Triangle, not equal power: the gains must sum to 1, not 1.41,
            // or the window is gain and the loop runs away under its feedback.
            const float w = 1.0f - fabsf(shim_ph[g] - kShimGrain * 0.5f)
                                       / (kShimGrain * 0.5f);
            const float d = shim_ph[g] + shim_off[g];
            for(int c = 0; c < 2; c++)
                up[c] += ShimRd(shim_buf[c], d) * w;
        }
        // Staggered triangles sum flat, to half their count
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
