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
// One just minor triad and its octaves, ascending across the pads: the
// two-group tuning put the groups a sixth apart, which has no meaning with
// one cluster. Spread over two octaves rather than packed into one -- seven
// voices all sound at once now, and at 220 Hz a critical band is only 48 Hz
// wide, so a close scale would simply mud up. The tightest neighbouring
// pair here is 220/264, which is what the instrument already had.
const int kRatN[kPads] = {1, 6, 3, 2, 12, 3, 4};
const int kRatD[kPads] = {1, 5, 2, 1, 5, 1, 1};
constexpr int kRoot = 9;   // A

// Leaning on a held pad breaks its group's rules. A lean is an INCREASE,
// not a level: how hard a finger rests varies with the finger and the pad,
// and an absolute threshold that an ordinary hold already sits above can
// never re-arm without letting go. So each pad tracks its own resting
// force and the gesture is a rise above that -- slowly up, quickly down,
// so a deliberate push stands out and easing off re-arms at once.
constexpr float kLean    = 0.25f;   // rise over resting that counts
constexpr float kFloor   = 0.3f;    // below this it is a brush, not a lean
constexpr float kRestUp  = 0.8f;    // seconds for rest to absorb a push
constexpr float kRestDn  = 0.15f;   // and to follow a finger easing off
float           pressure[kPads] = {};
float           rest[kPads]     = {};
float           rest_at[kPads]  = {};
bool            armed[kPads]    = {};

bool  held[kPads], latched[kPads];
// A latched pad goes on sounding with nothing touching it. Everything that
// asks "is this pad playing" means this, not the contact.
inline bool PadActive(int p) { return held[p] || latched[p]; }
// The octave a pad was pressed at. A pad keeps the pitch it was triggered
// with until it is released, so shifting the octave leaves what is sounding
// alone and only applies to the next press.
float pad_hz[kPads];
const int kPadVoice[kPads] = {0, 1, 2, 3, 4, 5, 6};
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


// Pushes every pad's sounding state into WFC, which is what the rules read
void SyncSounding(float now)
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
        = {"Delay",  "Feedback", "Distortion", "Filter", "Mod",
           "Control", "Unrest",   "Hold",       "Detune"};
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
    octave         = 0;
    wfc::K().hold = 0.0f;
    now_s          = 0.0f;
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
    in_env = in_hold = in_mix = 0.0f;
    send_hp_c = 1.0f - expf(-2.0f * 3.14159265f * 180.0f / sr);
    lim_env = 0.0f;

    wfc::Init(0x5afe6a7du);
    voices::Init(sample_rate, block_size);

    // A voice carries its pad's note from the start. HOLD is a floor under
    // every voice, pressed or not, and a voice that had never been pressed
    // still sat on the 220 Hz default -- so raising the fader droned the
    // untouched pads in unison and it read as one voice sounding out of
    // seven, until you tapped each one to tune it.
    //
    // Not snapped: snapping marks a voice fresh, and seven fresh voices at
    // boot duck each other for the first seconds. Everything snapping would
    // set is already at its value here anyway.
    for(int p = 0; p < kPads; p++)
        if(kPadVoice[p] >= 0)
            voices::SetPitch(kPadVoice[p], PadHz(p), 2.0f * RootHz(), false);
}

float PadHz(int p)
{
    if(PadActive(p) && pad_hz[p] > 0.0f)
        return pad_hz[p];
    return RootHz() * (float)kRatN[p] / (float)kRatD[p];
}

int  PadVoice(int p) { return kPadVoice[p]; }
bool PadOn(int p) { return PadActive(p); }

void SetPressure(int pad, float p)
{
    if(pad < 0 || pad >= kPads)
        return;
    pressure[pad] = p;
    // A fresh press has no resting force yet, so seed it rather than read a
    // rise from zero and break on every single press
    if(rest[pad] < 0.0f)
    {
        rest[pad]    = p;
        rest_at[pad] = now_s;
        // Disarmed as well as seeded. The panel ramps a pad's pressure by
        // half the remaining distance every 28 ms, so this first sample is
        // only half way up the finger's real force and the rest of the
        // ramp reads as a deliberate lean: measured, every press firmer
        // than 0.5 broke its own voice, 40 times out of 40, which re-rolled
        // the tile of the note being played as it spoke. Arming waits for
        // the finger to settle, which the lean < kLean * 0.4 test below
        // does once rest has caught up.
        armed[pad]   = false;
        return;
    }
    const float lean = p - rest[pad];
    if(lean < kLean * 0.4f)
        armed[pad] = true;
    else if(lean >= kLean && p >= kFloor && armed[pad] && held[pad])
    {
        armed[pad] = false;
        const int v = kPadVoice[pad];
        if(v >= 0)
            wfc::BreakRule(-1, v / wfc::kGroupSize, 0.0f, now_s, v);
    }
    // After the test, so the push that fires is measured against where the
    // finger was, not against itself. The panel polls each pad every 28 ms
    // and the desktop every block, hence a time constant and not a rate.
    const float dt = now_s - rest_at[pad];
    rest_at[pad]   = now_s;
    if(dt > 0.0f)
        rest[pad] += (p - rest[pad])
                     * (1.0f - expf(-dt / (p > rest[pad] ? kRestUp
                                                         : kRestDn)));
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
    pad_hz[pad] = RootHz() * (float)kRatN[pad] / (float)kRatD[pad];
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

// S07/S08 down. Called every poll, so pads pressed and let go while it is
// down keep stacking up; off clears the lot.
void Latch(bool on)
{
    for(int p = 0; p < kPads; p++)
        latched[p] = on && (latched[p] || held[p]);
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
    SyncSounding(now);
    // Anything that ought to be sounding wakes the chain, and this is the one
    // place that can. Checking held pads alone left the HOLD fader unable to
    // wake it: drop the fader, wait out the sleep, and raising it again set
    // every voice sounding into a chain that was no longer rendering.
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
