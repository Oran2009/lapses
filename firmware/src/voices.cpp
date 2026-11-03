#include "voices.h"

#include <math.h>
#include <string.h>

// The delay buffers are 1.5 MB together, which only fits in the Daisy's
// SDRAM. On the desktop harness it is ordinary memory.
#ifdef __arm__
#define SLOW_BSS __attribute__((section(".sdram_bss")))
#else
#define SLOW_BSS
#endif

namespace voices
{
namespace
{
constexpr int kPartials = 8;

// SPIKE: the voice worlds.
//
// A world is its material and its body together, so a model sets both: what
// the source is made of, and what the loop does with it. The loop is already
// snapped to a whole number of the voice's own periods, which makes it a
// tuned resonator -- feed it noise instead of a matching harmonic stack and
// the loop stops repeating a pitch and starts MAKING one.
struct Model
{
    const char* name;
    float ratio[kPartials];   // partial frequencies, times the pitch
    float amp[kPartials];     // and their levels
    float rolloff;   // scales the pitch-dependent spectral tilt
    float tone;      // how much of the partial stack reaches the line
    float noise;     // and how much broadband noise goes in beside it
    float drive;     // shaping the source before the line, not inside it
    float motion;    // how fast the partials re-roll; 1 is as it was
    float cents;     // and how far they wander
    float dly_mul;   // the loop's length, so the body can be small
    float damp;      // and its lowpass, so the body can be bright or dead
    float atk;       // the VCA's speed
    float out;       // and its level, so the three audition level-matched
};

// The levels are calibrated so the three sit at about the same loudness with
// the same knobs; without that the loudest one wins the audition.
constexpr Model kModel[kModels] = {
    // 0. As it was: six harmonics and two unisons, breathing slowly.
    {"as-is",
     {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 1.0f, 2.0f},
     {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.45f, 0.45f},
     1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.6f, 1.0f, 1.0f, 1.0f, 1.00f},

    // 1. Organism: a short body and the drive moved AHEAD of the line, with
    // partials in beating pairs. This is the Lyra reading -- friction.
    {"organism",
     {1.0f, 1.007f, 2.0f, 2.013f, 3.0f, 3.02f, 1.5f, 4.5f},
     {1.0f, 0.9f, 0.8f, 0.7f, 0.55f, 0.45f, 0.5f, 0.3f},
     0.55f, 0.85f, 0.03f, 0.75f, 6.0f, 10.0f, 0.10f, 2.2f, 1.0f, 0.29f},

    // 2. Deep: sub-octave partials, a long dark body, almost no top. Mass.
    {"deep",
     {0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 0.5f, 1.0f},
     {1.0f, 0.9f, 0.5f, 0.4f, 0.2f, 0.12f, 0.6f, 0.4f},
     1.6f, 0.75f, 0.22f, 0.2f, 0.6f, 6.0f, 1.7f, 0.35f, 0.5f, 0.49f},
};

int cur_model = 0;
inline const Model& M() { return kModel[cur_model]; }

int   cable_mode = kCableOff;
// Own matrix, not cab: cab is rendered per cable mode, a bridge must always feed
struct Bridge { int src; int dst; float amt; } br[2] = {{-1, 0, 0}, {-1, 0, 0}};
float bri[wfc::kVoices][wfc::kVoices], bri_t[wfc::kVoices][wfc::kVoices];

float sr        = 48000.0f;
float inv_sr    = 1.0f / 48000.0f;
int   block     = 48;

// Each trip round the loop drops more bass. A lowpass-only loop keeps the
// low-mids and rings them for ever, which is the whole of the mud; with this
// the repeats thin out as they pile up.
constexpr float kLoopHpHz = 150.0f;

// How long a knob move takes to cross from the old tap to the new one.
// Short enough to feel immediate, long enough that the two taps' phase
// disagreement does not read as a click.
constexpr float kXfadeT = 0.03f;

// How long a broken delay takes to crawl to its new class. Long enough
// that the tap movement reads as a pitch bend, not a glide. Reasoned,
// not measured -- tune by ear.
constexpr float kSweepT = 1.2f;

// Room for a new voice is made at the BUS, not in the envelope: a voice's
// tail is mostly its loop, so lifting the new voice's vca only drives its
// line harder and takes seconds to be heard. These act on what each voice
// actually sends out. Squared so the shadow is short -- fresh decays over
// 2.5 s and a duck that long would pump on every press.
// Measured as P03's own output against the wash it lands in, 16 settled
// seeds: worst case 1.63 with the old fixed 0.45 step, 2.12 with these.
// Target down or floor down for more separation, at the cost of the wash
// pumping harder on every press.
constexpr float kWashTarget = 0.12f;   // the level the wash ducks TO
constexpr float kDuckFloor  = 0.25f;   // and the deepest it may be ducked
constexpr float kRoomCeil   = 0.55f;   // and the shallowest
constexpr float kLiftOut = 2.0f;   // and how far the new one steps up
constexpr float kRefHz   = 330.0f;   // the pitch at which that is unscaled
float           loop_hp_c = 0.0f;

// --------------------------------------------------------------- the maths

// One sine period, power of two, with a guard entry for the interpolation
constexpr int kSinBits = 12;
constexpr int kSinLen  = 1 << kSinBits;
float         sin_tab[kSinLen + 1];

// Phase as a uint32 fraction of a turn: 32 oscillators at 48 kHz is 1.5 M
// sines a second, which libm's sinf would not give away for free
inline float SinQ(uint32_t ph)
{
    constexpr int   shift = 32 - kSinBits;
    constexpr float frac  = 1.0f / (float)(1u << shift);
    const uint32_t  i     = ph >> shift;
    return sin_tab[i]
           + (float)(ph & ((1u << shift) - 1)) * frac
                 * (sin_tab[i + 1] - sin_tab[i]);
}

inline float Clamp(float x, float a, float b)
{
    return x < a ? a : (x > b ? b : x);
}

// The prototype's WaveShaper clamps its input to [-1, 1] and only then
// applies tanh, so the clamp is part of the sound and not a safety rail.
// Over that range this rational form is within 2e-4 of tanhf.
inline float Shape(float x)
{
    x            = Clamp(x, -1.0f, 1.0f);
    const float s = x * x;
    return x * (27.0f + s) / (27.0f + 9.0f * s);
}

// Web Audio's setTargetAtTime(v, t, tau) is a one-pole; at the block rate the
// coefficient is this. Called once per tick per tau, not per sample.
inline float Coeff(float tau, float dt)
{
    return tau <= 0.0f ? 1.0f : 1.0f - expf(-dt / tau);
}
inline void Approach(float& cur, float tgt, float c)
{
    cur += (tgt - cur) * c;
}

uint32_t rng_state = 0x9e3779b9u;
inline float Rand()
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (float)(rng_state >> 8) * (1.0f / 16777216.0f);
}
// -1..1
inline float Rnd() { return Rand() * 2.0f - 1.0f; }

// ------------------------------------------------------------- a one voice

// Each partial drifts on its own: a slow random walk in cents plus a fixed
// offset, and a level that wanders down and back. This is the "breathing
// partials" engine the user chose in the drone lab (HANDOFF stage 4).
struct Partial
{
    uint32_t phase;
    uint32_t inc;
    float    base;       // k^-1.2
    float    stat;       // a fixed offset, -1..1
    float    det_base;   // stat*3 plus the unison offsets, cents
    float    walk;       // the random walk, clamped to +-8 cents
    float    det, det_t;  // cents, smoothed
    float    lvl, lvl_t;  // gain, smoothed
    float    det_c, lvl_c;   // their one-pole coefficients
    float    next;       // when this partial next re-rolls
};

// A two-pole lowpass, standing in for the prototype's biquads.
//
// Topology-preserving (Zavalishin) rather than the cheaper Chamberlin form,
// because Chamberlin is only stable for g < 2 - 1/Q and the loop filter runs
// at Q = 0.5, where that bound is zero: it blew up at every cutoff. This form
// is unconditionally stable at any cutoff and Q, and its coefficients are
// computed once a block instead of a sinf per sample.
struct Svf
{
    float ic1, ic2;
    float a1, a2, a3;
    void  Reset() { ic1 = ic2 = 0.0f; }
    void  Set(float f, float q)
    {
        const float g = tanf(3.14159265f * Clamp(f, 20.0f, sr * 0.45f)
                             * inv_sr);
        const float k = 1.0f / q;
        a1            = 1.0f / (1.0f + g * (g + k));
        a2            = g * a1;
        a3            = g * a2;
    }
    inline float Process(float x)
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1            = 2.0f * v1 - ic1;
        ic2            = 2.0f * v2 - ic2;
        return v2;
    }
};

struct Voice
{
    Partial p[kPartials];
    float   hz;
    float   amp;

    Svf vlp, llp, llp2;   // llp2 is the side tap's, so both channels match

    // The delay line, written and read once a sample
    float* line;
    int    wr;

    // Everything smoothed, with its target recomputed each control tick
    float vca, vca_t;
    // Lifted for a few seconds after a press, so a new pad speaks before it
    // dissolves into the wash
    float fresh;
    float loop_hp, loop_hp2;   // the loop highpass's state, one per tap
    float vcut, vcut_t;
    float dly, dly_t;        // seconds
    float dly_n;             // the tap a knob move is crossfading TO
    float xf, xf_t;          // and how far through, at block start and end
    float fb, fb_t;
    float lcut, lcut_t;
    float pre, pre_t;
    float wet, wet_t;
    float cab[wfc::kVoices], cab_t[wfc::kVoices];

    // The mod, and the three depths it adds through
    float    mspeed, mspeed_t;   // the response speed WFC chose, in Hz
    float    fast, slow;       // the follower's two envelopes
    float    lvl_sum;          // |loop| over the last block
    int      lvl_n;
    float    out_lvl;          // and the finished value, for the duck
    // What Process resolved for this block, kept for the display
    float    eff_dly, eff_fb, eff_pre, pan_now, depth_now;
    float    m_dly, m_dly_t;
    float    m_fb, m_fb_t;
    float    m_pre, m_pre_t;
    float    sweep;   // seconds of slow delay travel left after a break
    float    outg, outg_t;   // what this voice sends to the bus
    float    dryl, dryl_t;   // extra dry while it is the fresh one
    float    mod;   // its value this tick

    // What the loop's distortion put out last sample: the feedback path and
    // every cable tap read this, which is what breaks the algebraic loop
    float post_prev;

    float pan_l, pan_r;
    float pan_base;
    uint32_t pan_ph, pan_inc;
};

Voice     v[wfc::kVoices];
float SLOW_BSS delay_mem[wfc::kVoices][kDelayMax];

// The coefficients, rebuilt whenever dt changes (which is never, in practice)
struct
{
    float dt;
    float vca, vcut, fb, lcut, pre, wet, cab, mod_speed, mod_depth, dly;
    float fresh;
} co = {};

void Recoeff(float dt, float glide)
{
    co.dt        = dt;
    co.vca       = Coeff(0.03f, dt);
    co.vcut      = Coeff(0.05f, dt);
    co.fresh     = Coeff(2.5f, dt);
    co.fb        = Coeff(0.12f, dt);
    co.lcut      = Coeff(0.1f, dt);
    co.pre       = Coeff(0.1f, dt);
    co.wet       = Coeff(0.1f, dt);
    co.cab       = Coeff(0.1f, dt);
    co.mod_speed  = Coeff(0.3f, dt);
    co.mod_depth = Coeff(0.08f, dt);
    co.dly       = Coeff(glide, dt);
}

void SetPartialFreqs(Voice& vo, bool snap)
{
    // Low notes get a shallower rolloff. Measured against the others, a
    // 220 Hz voice was the LOUDEST on a meter and the dullest above 500 Hz,
    // which is where a new note has to appear to be heard over the wash --
    // its energy lands exactly where the wash is already thickest. Slope and
    // floor were 0.55/1.25, which put 220 Hz at tilt 1.35 and 42% of its
    // energy in that band; 1.5/0.55 puts it at 0.55 and 58%. Turn the floor
    // down for more bite at the bottom, up if the low pads buzz.
    const float tilt = Clamp(1.9f - 1.5f * logf(440.0f / vo.hz) * 1.442695f,
                             0.55f, 1.9f)
                       * M().rolloff;
    for(int k = 0; k < kPartials; k++)
    {
        Partial&    p  = vo.p[k];
        const float kk = M().ratio[k];
        // A touch of inharmonicity, so the partials beat rather than lock
        const float f  = vo.hz * kk * (1.0f + 0.00015f * kk * kk);
        const float det = powf(2.0f, p.det / 1200.0f);
        p.base = powf(kk, -tilt) * M().amp[k];
        if(snap)
            p.lvl = p.lvl_t = p.base;
        else
            p.lvl_t = p.base;
        p.inc = (uint32_t)(f * det * 4294967296.0f * inv_sr);
        if(snap)
            p.phase = (uint32_t)(Rand() * 4294967296.0f);
    }
}

bool pad_held[wfc::kVoices];
}  // namespace

// ----------------------------------------------------------------- the API

void Init(float sample_rate, int block_size)
{
    sr     = sample_rate;
    inv_sr = 1.0f / sr;
    block  = block_size;
    for(bool& h : pad_held)
        h = true;
    loop_hp_c = 1.0f - expf(-2.0f * 3.14159265f * kLoopHpHz * inv_sr);

    for(int i = 0; i <= kSinLen; i++)
        sin_tab[i] = sinf(2.0f * 3.14159265358979f * (float)i / (float)kSinLen);

    rng_state = 0x9e3779b9u;
    memset(bri, 0, sizeof(bri));
    for(int i = 0; i < 2; i++)
        SetBridge(i, -1, 0, 0.0f);
    for(int i = 0; i < wfc::kVoices; i++)
    {
        Voice& vo = v[i];
        memset(&vo, 0, sizeof(vo));
        vo.line = delay_mem[i];
        memset(vo.line, 0, sizeof(float) * kDelayMax);
        vo.wr = 0;
        vo.hz = 220.0f;
        vo.outg = vo.outg_t = 1.0f;
        vo.dryl = vo.dryl_t = 1.0f;
        vo.vlp.Reset();
        vo.llp.Reset();
        vo.llp2.Reset();
        vo.vlp.Set(400.0f, 0.8f);
        vo.llp.Set(2000.0f, 0.5f);
        vo.dly = vo.dly_t = vo.dly_n = 0.2f;
        vo.xf = vo.xf_t = 0.0f;
        vo.pre = vo.pre_t = 1.0f;
        vo.mspeed = vo.mspeed_t = 0.5f;
        vo.fast = vo.slow = vo.lvl_sum = vo.out_lvl = 0.0f;
        vo.lvl_n              = 0;
        for(int k = 0; k < kPartials; k++)
        {
            Partial& p = vo.p[k];
            p.stat     = Rnd();
            p.walk     = 0.0f;
            p.det_base = p.stat * 3.0f + (k == 6 ? 7.0f : 0.0f)
                         + (k == 7 ? -9.0f : 0.0f);
            p.det = p.det_t = p.det_base;
            p.det_c         = 1.0f;
            p.lvl_c         = 1.0f;
            p.next          = 0.0f;
            p.phase         = (uint32_t)(Rand() * 4294967296.0f);
        }
        SetPartialFreqs(vo, true);

        // Equal power, matching Web Audio's StereoPanner, at the prototype's
        // fixed spread of (i / 3 * 2 - 1) * .6
        vo.pan_base = ((float)i / (float)(wfc::kVoices - 1) * 2.0f - 1.0f)
                      * 0.6f;
        // Its own slow drift, deliberately not the mod: a static image in an
        // instrument made of slow movement is the thing that sounds fixed, and
        // this has to be there whatever the mod knob is doing
        vo.pan_ph  = (uint32_t)(Rand() * 4294967296.0f);
        vo.pan_inc = (uint32_t)((0.02f + Rand() * 0.04f) * 4294967296.0f
                                * (float)block * inv_sr);
        vo.pan_l = vo.pan_r = 0.70710678f;
    }
    Recoeff((float)block * inv_sr, wfc::DelayGlide());
}

// SPIKE: the voice worlds
void SetModel(int m)
{
    m = m >= 0 && m < kModels ? m : 0;
    // Idempotent: the shells call this every pass, and re-deriving snaps
    // every partial to a new phase, which is a click per call
    if(m == cur_model)
        return;
    cur_model = m;
    // The spectrum changes shape, so re-derive it rather than glide into it
    for(int i = 0; i < wfc::kVoices; i++)
        SetPartialFreqs(v[i], true);
}
int         Model_() { return cur_model; }
const char* ModelName(int m)
{
    return m >= 0 && m < kModels ? kModel[m].name : "?";
}

void SetCableMode(int m)
{
    cable_mode = m >= 0 && m < kCableCount ? m : kCableOff;
    wfc::SetCableMode(cable_mode);
}
int  CableMode_() { return cable_mode; }
const char* CableName(int m)
{
    static const char* n[kCableCount] = {"off", "feed", "mod"};
    return m >= 0 && m < kCableCount ? n[m] : "?";
}

static bool Ok(int a, int b)
{
    return a >= 0 && a < wfc::kVoices && b >= 0 && b < wfc::kVoices;
}

void SetBridge(int i, int src, int dst, float amt)
{
    if(i < 0 || i > 1)
        return;
    br[i] = {src, dst, amt};
    memset(bri_t, 0, sizeof(bri_t));
    for(const Bridge& b : br)
        if(Ok(b.src, b.dst) && b.src != b.dst)
            bri_t[b.src][b.dst] += b.amt;
}

float CableCmd(int src, int dst)
{
    return Ok(src, dst) ? v[src].cab_t[dst] + bri_t[src][dst] : 0.0f;
}

float CableEff(int src, int dst)
{
    return Ok(src, dst) ? v[src].cab[dst] + bri[src][dst] : 0.0f;
}

void SetPitch(int i, float hz, bool snap)
{
    if(snap)
        v[i].fresh = 1.0f;
        v[i].sweep = 0.0f;
        v[i].outg = v[i].outg_t = 1.0f;
        v[i].dryl = v[i].dryl_t = 1.0f;
    v[i].hz = hz;
    SetPartialFreqs(v[i], snap);
}

float Pitch(int i) { return v[i].hz; }
float Amp(int i) { return v[i].amp; }
float EffDly(int i) { return v[i].eff_dly; }
float EffFb(int i) { return v[i].eff_fb; }
float EffDrv(int i) { return v[i].eff_pre; }
float Mod(int i) { return v[i].mod; }
float ModDepth(int i) { return v[i].depth_now; }
float Pan(int i) { return v[i].pan_now; }
float LoopLevel(int i) { return v[i].fast; }
float OutGain(int i) { return v[i].outg; }
// What this voice is actually putting on the bus, as the duck sees it
float BusLvl(int i) { return v[i].out_lvl * v[i].wet * v[i].outg; }

float DryLift(int i) { return v[i].dryl; }

float Level()
{
    float m = 0.0f;
    for(int i = 0; i < wfc::kVoices; i++)
        if(v[i].amp > m)
            m = v[i].amp;
    return m;
}

void SetPadHeld(int v, bool on) { pad_held[v] = on; }
bool HoldSustains(int v)
{
    (void)v;
    // 0.02 is -34 dB, below which the floor is inaudible and should not pin
    // a voice into the constraint graph
    return wfc::K().hold > 0.02f;
}

void Control(float now, float dt)
{
    const float glide = wfc::DelayGlide();
    if(dt != co.dt || Coeff(glide, dt) != co.dly)
        Recoeff(dt, glide);

    const float base  = wfc::DelayBase();
    const float fbm   = wfc::FbMul();
    const float drvm  = wfc::DrvMul();
    const float modm  = wfc::ModMul();
    const float lcut  = wfc::LoopCut();
    const float vcm   = wfc::VoiceCutMul();
    const float mspeed0 = wfc::ModSpeed0();
    const float atk   = wfc::AttackT();
    const float rel   = wfc::ReleaseT();
    // The longest delay the buffer can serve, with a sample spare for the
    // interpolator's second tap
    const float dly_max = (float)(kDelayMax - 2) * inv_sr;

    // A press makes room for itself: the other voices' wash steps back for as
    // long as the new one is fresh. The feedback is untouched, so the rules
    // still ring exactly as WFC set them -- only the output tap ducks.
    float duck[wfc::kVoices], room[wfc::kVoices];
    for(int i = 0; i < wfc::kVoices; i++)
    {
        duck[i]    = 0.0f;
        float wash = 0.0f;
        for(int j = 0; j < wfc::kVoices; j++)
            if(j != i)
            {
                if(v[j].fresh > duck[i])
                    duck[i] = v[j].fresh;
                wash += v[j].out_lvl * v[j].wet;
            }
        // Duck the wash to a LEVEL, not by a fixed fraction. A new voice's
        // own output hardly varies press to press (measured flat across 24
        // tiles), but the wash it lands in roams over 10x, so a fixed step
        // passed that spread straight through and the same pad read clear
        // or muted depending on nothing the player did.
        //
        // The ceiling keeps it from ducking LESS than the old fixed step: a
        // chord struck from silence has no wash yet, so a bare target would
        // duck nothing and let six fresh voices stack into the limiter.
        room[i] = Clamp(wash > 1e-6f ? kWashTarget / wash : 1.0f, kDuckFloor,
                        kRoomCeil);
    }

    for(int i = 0; i < wfc::kVoices; i++)
    {
        Voice&     vo = v[i];
        const bool on = wfc::Sounding(i);

        // The envelope, which follows the Delay knob: long echoes get a long
        // release so the loop is still fed while it rings
        {
            // HOLD is a floor, not a longer release: every voice sounds
            // continuously at the fader's level and a pad press rides on top.
            const bool  sel = true;
            // The floor steps back for a fresh press, not just the wet tap:
            // otherwise five voices droning at the floor bury the one pad
            // you just pressed
            const float flr
                = sel ? wfc::K().hold * (1.0f - 0.5f * duck[i]) : 0.0f;
            const float tgt = fmaxf(on && pad_held[i] ? 1.0f : 0.0f, flr);
            const float tau = tgt > vo.amp ? (atk / 3.0f > 0.005f ? atk / 3.0f
                                                                 : 0.005f)
                                           : (rel / 3.0f > 0.03f ? rel / 3.0f
                                                                 : 0.03f);
            Approach(vo.amp, tgt, Coeff(tau, dt));
        }

        // What the wave has actually delivered to this voice
        const int   tile = wfc::ShownTile(i);
        const int   po   = wfc::ShownPatch(i);
        const int   lo   = wfc::ShownMod(i);
        const bool  skew = wfc::ShownSkew(i) != 0;
        const float F = wfc::FL[wfc::TileF(tile)];
        const float R = wfc::RL[wfc::TileR(tile)];
        const float D = wfc::DR[wfc::TileD(tile)];

        // The partials breathe: each re-rolls its own detune and level on its
        // own schedule, so the voice never settles into one spectrum
        for(int k = 0; k < kPartials; k++)
        {
            Partial& p = vo.p[k];
            if(now > p.next)
            {
                const float mo = M().motion;
                p.walk  = Clamp(p.walk + Rnd() * M().cents,
                                -10.0f * M().cents, 10.0f * M().cents);
                p.det_t = p.det_base + p.walk;
                p.lvl_t = p.base * (1.0f - 0.51f * Rand());
                p.det_c = Coeff(2.5f / mo, dt);
                p.lvl_c = Coeff((1.2f + Rand() * 2.0f) / mo, dt);
                p.next  = now + (0.8f + Rand() * 2.0f) / mo;
            }
            Approach(p.det, p.det_t, p.det_c);
            Approach(p.lvl, p.lvl_t, p.lvl_c);
        }
        SetPartialFreqs(vo, false);

        // An idle voice's loop is allowed to die away, so a released pad does
        // not leave a line ringing for ever
        const float feff
            = Clamp((on ? F : (F < 0.75f ? F : 0.75f)) * fbm, 0.0f, 1.3f);
        // A broken delay is no longer in tune with the voice's pitch; an
        // unbroken one is snapped to a whole number of its periods, which is
        // what makes the echoes reinforce instead of smear
        const float t0 = base * D * M().dly_mul * (skew ? wfc::kSkew : 1.0f);
        float       d  = skew ? t0
                              : floorf(t0 * vo.hz + 0.5f) / vo.hz;
        if(!skew && d * vo.hz < 1.0f)
            d = 1.0f / vo.hz;
        const float dly = Clamp(d, 0.02f, dly_max < 1.9f ? dly_max : 1.9f);
        const float pre = 1.0f + 12.0f * R * drvm;

        vo.dly_t  = dly;
        vo.fb_t   = feff;
        vo.lcut_t = lcut * M().damp;
        vo.pre_t  = pre;
        vo.wet_t  = 0.8f * sqrtf(pre) * 0.7f;
        vo.outg_t = 1.0f - duck[i] * duck[i] * (1.0f - room[i]);
        // The lift goes on the DRY only. A new voice's loop has not filled
        // yet -- a low one least of all, since its delay is snapped to whole
        // periods -- so lifting its whole output lifts mostly silence. The
        // dry is the part that sounds the instant the pad goes down.
        // and more of it the lower the voice: the delay is snapped to
        // whole periods, so a 220 Hz loop takes twice as long to fill as
        // a 440 Hz one and the dry has to carry it twice as long
        const float slow = Clamp(kRefHz / vo.hz, 0.5f, 3.0f);
        vo.dryl_t = 1.0f + kLiftOut * slow * vo.fresh * vo.fresh;
        Approach(vo.fresh, 0.0f, co.fresh);
        vo.vca_t  = vo.amp * 0.26f * (1.0f + 0.9f * vo.fresh);
        vo.vcut_t = Clamp(vo.hz * vcm * (0.18f + 0.82f * vo.amp)
                              * (1.0f + vo.fresh),
                          150.0f,
                          12000.0f);

        // The mod rides the envelope, so a silent voice's line is still
        const float dpe = wfc::DP[wfc::ModDepthOf(lo)] * modm * vo.amp;
        vo.mspeed_t      = mspeed0 * wfc::MS[wfc::ModSpeedOf(lo)];
        vo.m_dly_t      = dly * 0.08f * dpe;
        vo.m_fb_t       = 0.2f * dpe;
        vo.depth_now    = dpe;
        vo.m_pre_t      = pre * 0.35f * dpe;

        // The cable, which is live only while its SOURCE voice sounds
        const int tgt = wfc::OptTarget(po);
        for(int j = 0; j < wfc::kVoices; j++)
            vo.cab_t[j] = (on && j == tgt)
                              ? wfc::AMT[wfc::OptAmt(po)] * sqrtf(pre) * 1.3f
                              : 0.0f;

        // A break is an event, a repair is a recovery, so they do not travel
        // the same way. Gain and drive snap to the broken value and heal back
        // over the ordinary glide; the delay instead CRAWLS to it, which drags
        // the read tap through the full line and scrubs everything already in
        // it. That scrub is the most audible thing the instrument does -- it
        // is what turning the Delay knob by hand sounds like.
        if(wfc::TakeFlash(i) == 2)
        {
            vo.sweep = kSweepT;
            vo.fb    = vo.fb_t;
            vo.pre   = vo.pre_t;
        }
        const bool sweeping = vo.sweep > 0.0f;
        if(sweeping)
            vo.sweep -= dt;

        // A break DRAGS the tap, and that scrub is the gesture. A knob move
        // must not: sliding the read tap Dopplers everything already in the
        // line, which is the scrubbing. So the knob crossfades to a second
        // tap sitting at the new distance instead of travelling there.
        vo.xf = vo.xf_t;
        if(sweeping)
        {
            Approach(vo.dly, vo.dly_t, Coeff(kSweepT, dt));
            vo.dly_n = vo.dly;
            vo.xf = vo.xf_t = 0.0f;
        }
        else
        {
            if(vo.xf >= 1.0f)
            {
                vo.dly = vo.dly_n;
                vo.xf = vo.xf_t = 0.0f;
            }
            if(vo.xf > 0.0f)
                vo.xf_t = fminf(vo.xf + dt / kXfadeT, 1.0f);
            // One fade at a time: a new target that lands mid-fade waits,
            // which costs a block or two and keeps the taps click-free
            else if(fabsf(vo.dly_t - vo.dly) > 1e-5f)
            {
                vo.dly_n = vo.dly_t;
                vo.xf_t  = dt / kXfadeT;
            }
        }
        Approach(vo.fb, vo.fb_t, co.fb);
        Approach(vo.lcut, vo.lcut_t, co.lcut);
        Approach(vo.pre, vo.pre_t, co.pre);
        Approach(vo.wet, vo.wet_t, co.wet);
        Approach(vo.outg, vo.outg_t, co.wet);
        Approach(vo.dryl, vo.dryl_t, co.wet);
        Approach(vo.vca, vo.vca_t, Clamp(co.vca * M().atk, 0.0f, 1.0f));
        Approach(vo.vcut, vo.vcut_t, co.vcut);
        Approach(vo.mspeed, vo.mspeed_t, co.mod_speed);
        Approach(vo.m_dly, vo.m_dly_t, co.mod_depth);
        Approach(vo.m_fb, vo.m_fb_t, co.mod_depth);
        Approach(vo.m_pre, vo.m_pre_t, co.mod_depth);
        for(int j = 0; j < wfc::kVoices; j++)
        {
            Approach(vo.cab[j], vo.cab_t[j], co.cab);
            Approach(bri[i][j], bri_t[i][j], co.cab);
        }

        // THE MODULATOR. The voice follows its own loop: rising or falling,
        // not loud or quiet, because the difference of two envelopes is
        // self-centring and needs no calibration against level.
        //
        // WFC's rate class is spent on how FAST this voice reacts rather than
        // on an oscillator frequency -- there is no oscillator any more. One
        // voice twitches, another heaves, and the rule layer keeps its shape.
        //
        // ponytail: block-rate, ~1 ms steps. Fine for an envelope follower.
        // The cross-MODULATION cables are the per-sample path; see Process().
        const float lvl = vo.lvl_n ? vo.lvl_sum / (float)vo.lvl_n : 0.0f;
        vo.out_lvl      = lvl;
        vo.lvl_sum      = 0.0f;
        vo.lvl_n        = 0;
        const float tau = 0.4f / (vo.mspeed > 0.05f ? vo.mspeed : 0.05f);
        Approach(vo.fast, lvl, Coeff(tau, dt));
        Approach(vo.slow, lvl, Coeff(tau * 8.0f, dt));
        // 45 is measured: at 14 this sat at an rms of 0.09 and read as
        // nothing happening at all
        vo.mod = Shape((vo.fast - vo.slow) * 45.0f);

        // Where the voice is, drifting slowly
        vo.pan_ph += vo.pan_inc;
        const float pan = Clamp(vo.pan_base + SinQ(vo.pan_ph) * 0.32f,
                                -1.0f, 1.0f);
        vo.pan_now    = pan;
        const float a = (pan + 1.0f) * 0.25f * 3.14159265f;
        vo.pan_l      = cosf(a);
        vo.pan_r      = sinf(a);

        // The filters take their coefficients once a tick, not once a sample
        vo.vlp.Set(vo.vcut, 0.8f);
        vo.llp.Set(vo.lcut, 0.5f);
        vo.llp2.Set(vo.lcut, 0.5f);
    }
}

void Process(float* out_l, float* out_r, int n, const float* in,
             float in_mix)
{
    // Resolved once a block: the mod and every smoothed value hold still
    // across it, which is what makes the per-sample loop cheap
    float dsamp[wfc::kVoices], dsamp2[wfc::kVoices], fbg[wfc::kVoices],
        preg[wfc::kVoices], postg[wfc::kVoices];
    // Only populated for a voice that is mid-crossfade
    float dsampn[wfc::kVoices], xf[wfc::kVoices], xfi[wfc::kVoices];
    for(int i = 0; i < wfc::kVoices; i++)
    {
        Voice& vo = v[i];
        // The mod ADDS to these, exactly as a Web Audio mod connected to an
        // AudioParam does
        const float d = (vo.dly + vo.mod * vo.m_dly) * sr;
        dsamp[i]      = Clamp(d, 2.0f, (float)(kDelayMax - 2));
        // A second tap 1.3% further back: at a fifth of a second that is a
        // couple of milliseconds, so the loop reads as a wide source instead
        // of one panned point. It never feeds back -- the loop is unchanged.
        dsamp2[i] = Clamp(dsamp[i] * 1.013f, 2.0f, (float)(kDelayMax - 2));
        xf[i]     = vo.xf;
        xfi[i]    = n > 0 ? (vo.xf_t - vo.xf) / (float)n : 0.0f;
        const float dn = (vo.dly_n + vo.mod * vo.m_dly) * sr;
        dsampn[i]      = Clamp(dn, 2.0f, (float)(kDelayMax - 2));
        fbg[i]        = Clamp(vo.fb + vo.mod * vo.m_fb, 0.0f, 1.3f);
        preg[i]       = vo.pre + vo.mod * vo.m_pre;
        if(preg[i] < 0.01f)
            preg[i] = 0.01f;
        // post is NOT modulated: that asymmetry is what lets the mod change
        // the loop's gain and not just its colour
        postg[i] = 1.0f / vo.pre;

        // Mid-crossfade both taps sound; report the louder one, so the
        // display still says what you are hearing
        vo.eff_dly = (xf[i] > 0.5f ? dsampn[i] : dsamp[i]) * inv_sr;
        vo.eff_fb  = fbg[i];
        vo.eff_pre = preg[i];
    }

    for(int s = 0; s < n; s++)
    {
        float l = 0.0f, r = 0.0f;
        // Captured before anything is written, so every cable and feedback
        // tap in this sample reads the same previous-sample output
        float prev[wfc::kVoices];
        for(int i = 0; i < wfc::kVoices; i++)
            prev[i] = v[i].post_prev;

        for(int i = 0; i < wfc::kVoices; i++)
        {
            Voice& vo = v[i];

            float src = 0.0f;
            for(int k = 0; k < kPartials; k++)
            {
                src += SinQ(vo.p[k].phase) * vo.p[k].lvl;
                vo.p[k].phase += vo.p[k].inc;
            }
            src *= 0.3f * M().tone;
            // The material. Noise here rather than after the filter, so the
            // voice filter and the tuned loop both get to shape it.
            if(M().noise != 0.0f)
                src += Rnd() * M().noise;
            // Drive AHEAD of the line, which is a different animal from the
            // drive inside the loop: it dirties the source, not the echoes.
            if(M().drive != 0.0f)
                src = Shape(src * (1.0f + 8.0f * M().drive));
            src *= M().out;
            // The partials keep running underneath so the fade back is in
            // phase; at in_mix 1 none of them reaches anything
            if(in)
                src = src * (1.0f - in_mix) + in[s] * in_mix * 0.5f;

            // The voice itself, before its line
            const float dry = vo.vlp.Process(src) * vo.vca;

            // What the cables pointed at this voice are carrying. Feed adds
            // their audio to the line; Mod spends it on the tap position and
            // the feedback instead. Mod has to be per sample because the
            // modulator IS audio -- there is nothing slow about it.
            float fed = 0.0f, dmod = 0.0f, fmod = 0.0f;
            for(int j = 0; j < wfc::kVoices; j++)
            {
                if(j == i)
                    continue;
                fed += prev[j] * bri[j][i];
                if(v[j].cab[i] == 0.0f)
                    continue;
                const float c = prev[j] * v[j].cab[i];
                if(cable_mode == kCableMod)
                {
                    // A FRACTION of the tap, not a fixed number of samples:
                    // 0.004 s of a 230 ms delay is 0.17%, about three cents,
                    // which is why this mode was inaudible. A quarter of the
                    // tap is a real bend, and the Clamp below keeps it legal.
                    dmod += c * dsamp[i] * 0.3f;
                    fmod += c * 0.8f;
                }
                else
                    fed += c;
            }

            // What goes into the line: the voice, its own feedback, and
            // whatever the cables fed it
            const float in = dry + Clamp(fbg[i] + fmod, 0.0f, 1.3f) * prev[i]
                             + fed;

            // Write, then read `dsamp` samples back, interpolated because the
            // mod and the repair glide both move the tap
            vo.line[vo.wr] = in;
            const float ds  = Clamp(dsamp[i] + dmod, 2.0f,
                                    (float)(kDelayMax - 2));
            const int   di  = (int)ds;
            const float fr  = ds - (float)di;
            const int   i0  = (vo.wr - di + kDelayMax) % kDelayMax;
            const int   i1  = (i0 - 1 + kDelayMax) % kDelayMax;
            const float out = vo.line[i0] + (vo.line[i1] - vo.line[i0]) * fr;
            float outm = out, o2;
            if(xf[i] > 0.0f)
            {
                // Mid-crossfade the WIDE tap is dropped and both channels
                // take the crossfaded one. Two reads either way, so turning
                // the knob costs no extra memory traffic -- on the board the
                // four reads this used to do saturated the audio callback.
                // The image narrows for 30 ms, which is not audible.
                const float ns = Clamp(dsampn[i] + dmod, 2.0f,
                                       (float)(kDelayMax - 2));
                const int   ni = (int)ns;
                const float nf = ns - (float)ni;
                const int   n0 = (vo.wr - ni + kDelayMax) % kDelayMax;
                const int   n1 = (n0 - 1 + kDelayMax) % kDelayMax;
                const float nv = vo.line[n0]
                                 + (vo.line[n1] - vo.line[n0]) * nf;
                // Equal power by square root, not by sin/cos: the two taps
                // read uncorrelated parts of the line so a linear fade dips
                // in the middle, but VSQRT is one instruction where sinf and
                // cosf are a libm call each, twelve per sample across the
                // voices. That is what broke the audio on the board.
                const float x  = xf[i] > 1.0f ? 1.0f : xf[i];
                outm           = out * sqrtf(1.0f - x) + nv * sqrtf(x);
                o2             = outm;
                xf[i] += xfi[i];
            }
            else
            {
                const int   dj = (int)dsamp2[i];
                const float fj = dsamp2[i] - (float)dj;
                const int   j0 = (vo.wr - dj + kDelayMax) % kDelayMax;
                const int   j1 = (j0 - 1 + kDelayMax) % kDelayMax;
                o2 = vo.line[j0] + (vo.line[j1] - vo.line[j0]) * fj;
            }

            vo.wr           = (vo.wr + 1) % kDelayMax;

            // The loop's filters and its distortion
            const float lp  = vo.llp.Process(outm);
            vo.loop_hp += (lp - vo.loop_hp) * loop_hp_c;
            const float f   = lp - vo.loop_hp;
            const float sh  = Shape(f * preg[i]);
            const float pst = sh * postg[i];
            vo.post_prev    = pst;
            vo.lvl_sum += pst < 0.0f ? -pst : pst;
            vo.lvl_n++;

            // Dry voice and wet line both go to the bus, panned together.
            // In input mode the dry is dropped here: the box passes the input
            // through once, centred, instead of four panned copies of it.
            // The side tap gets its own copy of the loop's filters. Shaping
            // it unfiltered fed the distortion a brighter, hotter signal than
            // the main tap's, and the right channel came out dirtier than the
            // left. The taps differ in position only.
            const float lp2 = vo.llp2.Process(o2);
            vo.loop_hp2 += (lp2 - vo.loop_hp2) * loop_hp_c;
            const float pst2 = Shape((lp2 - vo.loop_hp2) * preg[i]) * postg[i];

            // One tap per channel, both at the voice's own position. Panning
            // the side tap opposite instead only pulled the image to the
            // centre, which is what a correlated copy does.
            // dryl is applied HERE and not to `dry` above, so the line is
            // fed the same signal either way and the loop is unchanged
            const float mix = dry * (1.0f - in_mix) * vo.dryl;
            l += (mix + pst * vo.wet) * vo.pan_l * vo.outg;
            r += (mix + pst2 * vo.wet) * vo.pan_r * vo.outg;
        }
        out_l[s] += l;
        out_r[s] += r;
    }
}

}  // namespace voices
