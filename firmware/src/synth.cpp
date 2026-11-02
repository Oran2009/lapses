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
// Two just minor triads a SIXTH apart (1, 6/5, 3/2 and 5/3, 2, 5/2). Not a
// fifth: the triad itself spans a fifth, so transposing by one put the right
// group's root on the left group's top note and P06 and P08 played a unison.
// Index 2 is the connector and never read for pitch.
const int kRatN[kPads] = {1, 6, 0, 5, 2, 3, 5};
const int kRatD[kPads] = {1, 5, 1, 3, 1, 2, 2};
constexpr int kRoot = 9;   // A

bool  held[kPads];
// The octave a pad was pressed at. A pad keeps the pitch it was triggered
// with until it is released, so shifting the octave leaves what is sounding
// alone and only applies to the next press.
float pad_hz[kPads];
const int kPadVoice[kPads] = {0, 1, -1, 3, 4, 2, 5};
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
// The right channel's combs are a few samples longer. Without this the plate
// is mono and sits exactly where the delays already are; with it the tail is
// wide and the delays are not, which is what makes it read as space at all.
constexpr int kSpread = 113;
const int     kApLen[kAps]     = {389, 127};
float SLOW_BSS comb_buf[2][kCombs][2432];
float SLOW_BSS ap_buf[2][kAps][512];
int            comb_i[2][kCombs], ap_i[2][kAps];
float          comb_lp[2][kCombs];
// 30 ms before the plate hears anything. Without it the early output is
// correlated with the dry and the allpass chain's inversion cancels part of
// it, so raising the send changed the tone instead of adding space.
constexpr int  kPre = 1440;
float SLOW_BSS pre_buf[2][kPre];
int            pre_i[2];
// An effects box that silently stayed a synth because a toggle was missed is
// the worse failure, so the line input is detected. It engages as soon as
// anything is on it and lets go three seconds after it falls quiet.
float          in_env = 0.0f, in_hold = 0.0f, in_mix = 0.0f;

// The reverb is fed above the mud, not through it
float          send_hp[2];
float          send_hp_c = 0.0f;

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
        // A darkening tail, as the prototype's IR has
        // Brighter than the loops, which are lowpassed hard: a tail that is
        // as dark as they are cannot be told apart from them
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

float now_s = 0.0f;

bool  user_freeze = false;
float agit_last   = -1.0f;

void UpdateBridges()
{
    int a[wfc::kGroups];
    for(int g = 0; g < wfc::kGroups; g++)
    {
        a[g] = -1;
        for(int v = g * wfc::kGroupSize; v < (g + 1) * wfc::kGroupSize; v++)
            if(wfc::Sounding(v) && (a[g] < 0 || wfc::StartedAt(v) > wfc::StartedAt(a[g])))
                a[g] = v;
    }
    wfc::SetJoined(held[kConnectorPad], now_s);
    const bool  on  = held[kConnectorPad] && a[0] >= 0 && a[1] >= 0;
    const float amt = wfc::AMT[1];
    voices::SetBridge(0, on ? a[0] : -1, on ? a[1] : -1, amt);
    voices::SetBridge(1, on ? a[1] : -1, on ? a[0] : -1, amt);
}

// Pushes every pad's sounding state into WFC, which is what the rules read
void SyncSounding(float now)
{
    for(int p = 0; p < kPads; p++)
    {
        const int v = kPadVoice[p];
        if(v < 0)
            continue;
        voices::SetPadHeld(v, held[p]);
        const bool on = held[p] || voices::HoldSustains(v);
        if(on != wfc::Sounding(v))
            wfc::SetSounding(v, on, now);
    }
}

}  // namespace

const char* KnobName(int i)
{
    static const char* n[kKnobs]
        = {"Delay",  "Feedback", "Distortion", "Filter",
           "Mod",    "Control",  "Unrest",     "Hold"};
    return i >= 0 && i < kKnobs ? n[i] : "?";
}

void Init(float sample_rate, int block_size)
{
    sr    = sample_rate;
    block = block_size;
    for(int p = 0; p < kPads; p++)
    {
        held[p]   = false;
        pad_hz[p] = 0.0f;
    }
    octave         = 0;
    voices::SetHoldMask(3);
    wfc::K().hold = 0.0f;
    now_s          = 0.0f;
    user_freeze = false;
    agit_last   = -1.0f;
    ShiftGain(0);

    memset(comb_buf, 0, sizeof(comb_buf));
    memset(ap_buf, 0, sizeof(ap_buf));
    memset(comb_i, 0, sizeof(comb_i));
    memset(ap_i, 0, sizeof(ap_i));
    memset(comb_lp, 0, sizeof(comb_lp));
    memset(pre_buf, 0, sizeof(pre_buf));
    memset(pre_i, 0, sizeof(pre_i));
    memset(send_hp, 0, sizeof(send_hp));
    in_env = in_hold = in_mix = 0.0f;
    send_hp_c = 1.0f - expf(-2.0f * 3.14159265f * 180.0f / sr);
    lim_env = 0.0f;

    wfc::Init(0x5afe6a7du);
    voices::Init(sample_rate, block_size);
}

float PadHz(int p)
{
    if(held[p] && pad_hz[p] > 0.0f)
        return pad_hz[p];
    return RootHz() * (float)kRatN[p] / (float)kRatD[p];
}

int  PadVoice(int p) { return kPadVoice[p]; }
bool PadOn(int p) { return held[p]; }
bool Joined() { return held[kConnectorPad]; }

void Press(int pad)
{
    if(held[pad])
        return;
    held[pad]   = true;
    pad_hz[pad] = RootHz() * (float)kRatN[pad] / (float)kRatD[pad];
    const int v = kPadVoice[pad];
    if(v < 0)
    {
        UpdateBridges();
        return;
    }
    voices::SetPitch(v, PadHz(pad), true);
    wfc::SetSounding(v, true, now_s);
    SyncSounding(now_s);
    wfc::Pass(v, now_s);
    UpdateBridges();
}

void Release(int pad)
{
    held[pad] = false;
    SyncSounding(now_s);
    const int v = kPadVoice[pad];
    if(v >= 0)
        wfc::RerollTail(v, now_s);
    UpdateBridges();
}

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
    // The panel's ADCs jitter by a few counts, and the panel writes all eight
    // knobs every poll. Without a deadband every poll read as a move, which
    // rebuilt the rules and rescheduled a heal a thousand times a second: the
    // texture never stopped re-settling.
    if(i < 0 || i >= kKnobs || fabsf(k[i] - val) < 0.004f)
        return;
    k[i] = val;
    // Only these reach the rules, and the rebuild is 10,000 tile pairs
    if(i == 0 || i == 1 || i == 2)
        wfc::RebuildRules();
    wfc::KnobsMoved(now_s);
}

float Knob(int i) { return (&wfc::K().delay)[i]; }

// Nothing sounding is the normal state of this instrument between phrases,
// and rendering four silent voices and a silent plate costs the same as
// rendering four loud ones. The burst of work every block couples into the
// analog side as a tone at the block rate, which is audible with the mixer
// up, so once everything really has gone quiet the chain stops running.
// The threshold is well under the reverb's own tail, and anything the player
// does clears the counter, so the first block after a touch renders in full.
constexpr float kIdleFloor  = 1.0e-5f;   // -100 dBFS
constexpr float kIdleHold_s = 8.0f;      // longer than the plate's 5 s tail
int             idle_blocks = 0;
bool            idle        = false;

void Control(float now)
{
    now_s = now;
    // Only a sounding pad can break the silence, so this is the one place
    // that has to wake the chain
    for(int p = 0; p < kPads; p++)
        if(held[p])
        {
            idle_blocks = 0;
            idle        = false;
            break;
        }
    SyncSounding(now);
    wfc::Tick(now);
    UpdateBridges();
    voices::Control(now, (float)block / sr);
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
    // Fixed, not a knob: a reverb balance is set once and left.
    // 1.8 per unit is measured, not derived -- fully wet matches fully dry in
    // level, where 2.2 put bursts of samples on the ceiling and the master
    // limiter's 26 ms release pumped on every one of them.
    constexpr float kSpace = 0.3f;
    constexpr float send   = 1.8f * kSpace;
    constexpr float dry    = 1.0f - 0.8f * kSpace;
    for(int i = 0; i < n; i++)
    {
        const float sl = l[i], sr_ = r[i];
        send_hp[0] += (sl - send_hp[0]) * send_hp_c;
        send_hp[1] += (sr_ - send_hp[1]) * send_hp_c;
        l[i] = Limit((sl * dry + Verb(0, (sl - send_hp[0]) * send) * 0.7f)
                     * gain_lin);
        r[i] = Limit((sr_ * dry + Verb(1, (sr_ - send_hp[1]) * send) * 0.7f)
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

void SetCableMode(int m)
{
    if(m == voices::CableMode_())
        return;
    voices::SetCableMode(m);
    // Settle what is already sounding into the new mode: before this the
    // switch only took effect on the next press, so you had to release
    // everything to hear it
    wfc::Heal(now_s);
}
int  CableMode() { return voices::CableMode_(); }
const char* CableName(int m) { return voices::CableName(m); }

void Break() { wfc::BreakRule(-1, 0.0f, now_s); }
void Settle() { wfc::Heal(now_s); }
// Same deadband as SetKnob, against the fader's own value rather than a
// struct field, because this one writes two
void SetAgitation(float a)
{
    if(fabsf(agit_last - a) < 0.004f)
        return;
    agit_last = a;
    wfc::SetAgitation(a);
    wfc::KnobsMoved(now_s);
}

void SetHoldTarget(int pos) { voices::SetHoldMask(pos > 0 ? 1 : pos < 0 ? 2 : 3); }
void Freeze(bool on)
{
    user_freeze = on;
    wfc::SetFrozen(on);
}
bool Frozen() { return wfc::Frozen(); }

float Level() { return voices::Level(); }

}  // namespace synth
