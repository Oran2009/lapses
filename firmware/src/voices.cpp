#include "voices.h"

#include <math.h>
#include <string.h>

// The delay buffers are 1.5 MB together, which only fits in the Daisy's SDRAM
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


// A model sets source and loop together: the loop snaps to whole periods, so noise pitches
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

// Levels calibrated so the three audition at about the same loudness
constexpr Model kModel[kModels] = {
    // 0. As it was: six harmonics and two unisons, breathing slowly.
    {"as-is",
     {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 1.0f, 2.0f},
     {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.45f, 0.45f},
     1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.6f, 1.0f, 1.0f, 1.0f, 1.00f},

    // 1. Organism: short body, drive AHEAD of the line, partials in beating pairs
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

// S09/S10 picks WHAT drives the mod, not where it goes: all three destinations move
enum
{
    kModStep = 0,   // up: jump and hold
    kModFollow,     // centre: the instrument as it was
    kModLfo         // down: a sine
};
int mod_mode = kModFollow;   // the switch at rest

float sr        = 48000.0f;
float inv_sr    = 1.0f / 48000.0f;
int   block     = 48;

// Highpass in the loop: lowpass alone rings the low-mids for ever, which is the mud
constexpr float kLoopHpHz = 150.0f;

// Crossfade from the old tap to the new on a knob move: short, but not a click
constexpr float kXfadeT = 0.03f;

// Seconds of delay the tap may shed per second. 1.0 is the write head, 0.5 an octave down.
constexpr float kMaxSlew   = 0.5f;
// How fast the tap chases the knob; the panel delivers a step every 6 ms
constexpr float kKnobGlide = 0.05f;



// Crawl time for a broken delay: long enough to read as a pitch bend. Reasoned, not measured.
constexpr float kSweepT = 1.2f;

// Room is made at the BUS, not the envelope. 16 settled seeds, separation 2.12 against 1.63.
constexpr float kWashTarget = 0.12f;   // the level the wash ducks TO
constexpr float kDuckFloor  = 0.25f;   // and the deepest it may be ducked
constexpr float kRoomCeil   = 0.55f;   // and the shallowest
// Step for a new voice: the median press goes +3.6 -> +5.4 dB. 8 peaked at 2.87.
constexpr float kLiftOut = 4.0f;
constexpr float kRefHz   = 330.0f;   // the pitch at which that is unscaled
// How long a press stays fresh: the lift, the duck and the brightness all ride on it
constexpr float kFreshT  = 0.7f;
float           loop_hp_c = 0.0f;

// --------------------------------------------------------------- the maths

// One sine period, power of two, with a guard entry for the interpolation
constexpr int kSinBits = 12;
constexpr int kSinLen  = 1 << kSinBits;
float         sin_tab[kSinLen + 1];

// Phase as a uint32 fraction of a turn: 1.5 M sines a second, which sinf will not give
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

// The prototype's clamp to [-1,1] before tanh is part of the sound; this is
// within 2e-4 of tanhf over it.
inline float Shape(float x)
{
    x            = Clamp(x, -1.0f, 1.0f);
    const float s = x * x;
    return x * (27.0f + s) / (27.0f + 9.0f * s);
}

// Web Audio's setTargetAtTime(v, t, tau) one-pole coefficient, at the block rate
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

// Per partial: a slow random walk in cents plus a fixed offset, and a wandering level
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
    double   next;      // when this partial next re-rolls
};

// Topology-preserving, not Chamberlin: that needs g < 2 - 1/Q, zero at the loop's Q = 0.5
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

    float* line;
    int    wr;

    // Everything smoothed, with its target recomputed each control tick
    float vca, vca_t;
    // Lifted for a few seconds after a press, so a new pad speaks before it dissolves
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

    // The mod, and the three depths it adds through
    float    mspeed, mspeed_t;   // the response speed WFC chose, in Hz
    float    fast, slow;       // the follower's two envelopes
    float    lvl_sum;          // |loop| over the last block
    int      lvl_n;
    float    out_lvl;          // and the finished value, for the duck
    // Loudest sample this voice alone put on the bus, for the press log
    float    pk;
    // Tap at the end of the last block: a Delay turn moved it 9000 samples in one step. -1 unset.
    float    ds_p, ds2_p;
    // What Process resolved for this block, kept for the display
    float    eff_dly, eff_fb, eff_pre, pan_now, depth_now;
    float    m_dly, m_dly_t;
    float    m_fb, m_fb_t;
    float    m_pre, m_pre_t;
    float    sweep;   // seconds of slow delay travel left after a break
    // A solver move crossfades, a knob move glides. Told apart by cause, not size.
    int      dtile;
    bool     dly_jump;
    // What the partial levels were last computed for: sixteen powf calls are not free
    float    pf_hz;
    int      pf_model;
    float    pf_ref;
    // The tilt reference, the chord's root an octave up. Travels with the voice.
    float    ref;
    float    outg, outg_t;   // what this voice sends to the bus
    float    dryl, dryl_t;   // extra dry while it is the fresh one
    float    mod;   // its value this tick
    uint32_t mod_ph;             // the LFO's phase
    float    frate;              // and its rate, the one Delay does not set
    float    step_t, step_left;  // where the step is going, and when next

    // Last out of the loop's distortion: reading it is what breaks the algebraic loop
    float post_prev;

    float pan_l, pan_r;
    float pan_base;
    uint32_t pan_ph, pan_inc;
};

Voice     v[wfc::kVoices];
float SLOW_BSS delay_mem[wfc::kVoices][kDelayMax];

// THE LINK, on S35: one delay all seven voices feed and read. Zero is out.
constexpr float kLinkDly = 0.41f;   // a multiple of no voice's period, on purpose
// Measured at the bus, one pad held: 1/7 and 0.25 left the rest 26 dB under, this 9 dB
float kLinkSend = 0.38f;   // 1/sqrt(7): seven different notes, not seven copies
float kLinkFb   = 0.55f;
float kLinkRet  = 0.20f;
// The pool DRIVES each loop's distortion too. Share above 3 kHz: off 0.068, this 0.135.
constexpr float kLinkDrv = 9.0f;
float link_env = 0.0f, link_env_c = 0.0f;
float link_g = 0.0f;   // ramped: a jump to full is a click in all seven at once
float SLOW_BSS link_mem[kDelayMax];
int   link_wr   = 0;
float link_prev = 0.0f, link_lp = 0.0f, link_lp_c = 0.0f;

// The coefficients, rebuilt whenever dt changes (which is never, in practice)
struct
{
    float dt;
    float vca, vcut, fb, lcut, pre, wet, mod_speed, mod_depth, dly;
    float fresh;
} co = {};

void Recoeff(float dt, float glide)
{
    co.dt        = dt;
    co.vca       = Coeff(0.03f, dt);
    co.vcut      = Coeff(0.05f, dt);
    co.fresh     = Coeff(kFreshT, dt);
    co.fb        = Coeff(0.12f, dt);
    co.lcut      = Coeff(0.1f, dt);
    co.pre       = Coeff(0.1f, dt);
    co.wet       = Coeff(0.1f, dt);
    co.mod_speed  = Coeff(0.3f, dt);
    co.mod_depth = Coeff(0.08f, dt);
    co.dly       = Coeff(glide, dt);
}

void SetPartialFreqs(Voice& vo, bool snap)
{
    // Shallower rolloff for low notes: 58% of a 220 Hz voice above 500 Hz, was 42%
    const bool relevel = snap || vo.pf_hz != vo.hz || vo.pf_model != cur_model
                         || vo.pf_ref != vo.ref;
    if(relevel)
    {
        // Against the chord's root, not 440: hardwired cost 8.8% in overtones an octave up
        const float tilt
            = Clamp(1.9f - 1.5f * logf(vo.ref / vo.hz) * 1.442695f, 0.55f, 1.9f)
              * M().rolloff;
        for(int k = 0; k < kPartials; k++)
            vo.p[k].base = powf(M().ratio[k], -tilt) * M().amp[k];
        vo.pf_hz    = vo.hz;
        vo.pf_model = cur_model;
        vo.pf_ref   = vo.ref;
    }
    for(int k = 0; k < kPartials; k++)
    {
        Partial&    p  = vo.p[k];
        const float kk = M().ratio[k];
        // A touch of inharmonicity, so the partials beat rather than lock
        const float f  = vo.hz * kk * (1.0f + 0.00015f * kk * kk);
        // 2^(cents/1200) by series, not powf: two terms are good to a twentieth of a cent here
        const float x   = p.det * (0.69314718f / 1200.0f);
        const float det = 1.0f + x * (1.0f + 0.5f * x);
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
    link_lp_c = 1.0f - expf(-2.0f * 3.14159265f * 2000.0f * inv_sr);
    // Init leaves the engine in a known state; both shells re-assert the switch anyway
    mod_mode  = kModFollow;
    // 30 ms: the pool's level, not its waveform
    link_env_c = 1.0f - expf(-inv_sr / 0.03f);
    link_env   = 0.0f;
    memset(link_mem, 0, sizeof(link_mem));
    link_wr   = 0;
    link_prev = link_lp = link_g = 0.0f;

    for(int i = 0; i <= kSinLen; i++)
        sin_tab[i] = sinf(2.0f * 3.14159265358979f * (float)i / (float)kSinLen);

    rng_state = 0x9e3779b9u;
    for(int i = 0; i < wfc::kVoices; i++)
    {
        Voice& vo = v[i];
        memset(&vo, 0, sizeof(vo));
        vo.line = delay_mem[i];
        memset(vo.line, 0, sizeof(float) * kDelayMax);
        vo.wr = 0;
        vo.hz = 220.0f;
        vo.ref = 440.0f;
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
        vo.fast = vo.slow = vo.lvl_sum = vo.out_lvl = vo.pk = 0.0f;
        // Its own phase and its own first step, or seven voices move as one
        vo.frate     = 0.54f;
        vo.mod_ph    = (uint32_t)(Rand() * 4294967296.0f);
        vo.step_t    = Rnd();
        vo.step_left = Rand() * 4.0f;
        vo.ds_p = vo.ds2_p = -1.0f;
        vo.dtile              = -1;
        vo.dly_jump           = false;
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

        // Equal power, matching Web Audio's StereoPanner at the prototype's fixed spread
        vo.pan_base = ((float)i / (float)(wfc::kVoices - 1) * 2.0f - 1.0f)
                      * 0.6f;
        // Own slow drift, not the mod: a static image is what sounds fixed
        vo.pan_ph  = (uint32_t)(Rand() * 4294967296.0f);
        vo.pan_inc = (uint32_t)((0.02f + Rand() * 0.04f) * 4294967296.0f
                                * (float)block * inv_sr);
        vo.pan_l = vo.pan_r = 0.70710678f;
    }
    Recoeff((float)block * inv_sr, wfc::DelayGlide());
}

// SPIKE: the voice worlds
void SetModMode(int m) { mod_mode = m >= 0 && m <= kModLfo ? m : kModFollow; }
int  ModMode() { return mod_mode; }

void SetModel(int m)
{
    m = m >= 0 && m < kModels ? m : 0;
    // Idempotent: the shells call this every pass, and re-deriving is a click per call
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

void SetPitch(int i, float hz, float ref, bool snap)
{
    v[i].ref = ref > 0.0f ? ref : 2.0f * hz;
    if(snap)
    {
        v[i].fresh = 1.0f;
        v[i].sweep = 0.0f;
        v[i].outg = v[i].outg_t = 1.0f;
        v[i].dryl = v[i].dryl_t = 1.0f;
    }
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
float BusLvl(int i) { return v[i].out_lvl * v[i].wet * v[i].outg; }
float TakePeak(int i)
{
    const float p = v[i].pk;
    v[i].pk       = 0.0f;
    return p;
}

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
    // 0.02 is -34 dB: below that the floor should not pin a voice into the graph
    return wfc::K().hold > 0.02f;
}

void Control(double now, float dt)
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
    // The longest delay the buffer can serve, with a sample for the second tap
    const float dly_max = (float)(kDelayMax - 2) * inv_sr;

    // A press makes room for itself: the other voices' wash steps back. Output tap only.
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
        // Duck to a LEVEL, not a fraction: output is flat across 24 tiles, the wash roams 10x
        room[i] = Clamp(wash > 1e-6f ? kWashTarget / wash : 1.0f, kDuckFloor,
                        kRoomCeil);
    }

    for(int i = 0; i < wfc::kVoices; i++)
    {
        Voice&     vo = v[i];
        const bool on = wfc::Sounding(i);

        // The envelope follows the Delay knob: long echoes get a long release
        {
            // HOLD is a floor, not a longer release: a pad press rides on top
            const bool  sel = true;
            // The HOLD floor ducks too, or five droning voices bury the pad you pressed
            const float flr
                = sel ? wfc::K().hold * (1.0f - 0.5f * duck[i]) : 0.0f;
            const float tgt = fmaxf(on && pad_held[i] ? 1.0f : 0.0f, flr);
            const float tau = tgt > vo.amp ? (atk / 3.0f > 0.005f ? atk / 3.0f
                                                                 : 0.005f)
                                           : (rel / 3.0f > 0.03f ? rel / 3.0f
                                                                 : 0.03f);
            Approach(vo.amp, tgt, Coeff(tau, dt));
        }

        const int   tile = wfc::ShownTile(i);
        const int   lo   = wfc::ShownMod(i);
        const bool  skew = wfc::ShownSkew(i) != 0;
        const float F = wfc::FL[wfc::TileF(tile)];
        const float R = wfc::RL[wfc::TileR(tile)];
        const float D = wfc::DR[wfc::TileD(tile)];

        // The partials breathe: each re-rolls its detune and level on its own schedule
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
                p.next  = now + (double)((0.8f + Rand() * 2.0f) / mo);
            }
            Approach(p.det, p.det_t, p.det_c);
            Approach(p.lvl, p.lvl_t, p.lvl_c);
        }
        SetPartialFreqs(vo, false);

        // An idle voice's loop dies away, so a released pad leaves nothing ringing
        const float feff
            = Clamp((on ? F : (F < 0.75f ? F : 0.75f)) * fbm, 0.0f, 1.3f);
        // Unbroken delays snap to whole periods of the pitch. Inaudible, but free.
        const float t0 = base * D * M().dly_mul * (skew ? wfc::kSkew : 1.0f);
        float       d  = skew ? t0 : floorf(t0 * vo.hz + 0.5f) / vo.hz;
        if(!skew && d * vo.hz < 1.0f)
            d = 1.0f / vo.hz;
        const float dly = Clamp(d, 0.02f, dly_max < 1.9f ? dly_max : 1.9f);
        const float pre = 1.0f + 12.0f * R * drvm;

        if(tile != vo.dtile)
        {
            vo.dly_jump = vo.dtile >= 0;
            vo.dtile    = tile;
        }
        vo.dly_t  = dly;
        vo.fb_t   = feff;
        vo.lcut_t = lcut * M().damp;
        vo.pre_t  = pre;
        vo.wet_t  = 0.8f * sqrtf(pre) * 0.7f;
        vo.outg_t = 1.0f - duck[i] * duck[i] * (1.0f - room[i]);
        // Dry only: a new voice's loop has not filled, so lifting its output lifts silence
        const float slow = Clamp(kRefHz / vo.hz, 0.5f, 3.0f);
        vo.dryl_t = 1.0f + kLiftOut * slow * vo.fresh * vo.fresh;
        Approach(vo.fresh, 0.0f, co.fresh);
        vo.vca_t  = vo.amp * 0.26f * (1.0f + 0.9f * vo.fresh);
        vo.vcut_t = Clamp(vo.hz * vcm * (0.18f + 0.82f * vo.amp)
                              * (1.0f + vo.fresh),
                          150.0f,
                          12000.0f);

        // The mod rides the envelope, so a silent voice's line stays still
        const float dpe = wfc::DP[wfc::ModDepthOf(lo)] * modm * vo.amp;
        vo.mspeed_t      = mspeed0 * wfc::MS[wfc::ModSpeedOf(lo)];
        // Not through ModSpeed0: that rate is a reaction time and an LFO reacts to nothing
        constexpr float kFreeHz = 0.54f;
        vo.frate         = kFreeHz * wfc::MS[wfc::ModSpeedOf(lo)];
        vo.m_dly_t = dly * 0.08f * dpe;
        vo.m_fb_t  = 0.2f * dpe;
        vo.m_pre_t = pre * 0.35f * dpe;
        vo.depth_now = dpe;

        // Gain and drive snap and heal back; the delay CRAWLS, and that scrub is the gesture
        if(wfc::TakeFlash(i) == 2)
        {
            vo.sweep = kSweepT;
            vo.fb    = vo.fb_t;
            vo.pre   = vo.pre_t;
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

        // Rising or falling, not level: the difference of two envelopes is self-centring
        const float lvl = vo.lvl_n ? vo.lvl_sum / (float)vo.lvl_n : 0.0f;
        vo.out_lvl      = lvl;
        vo.lvl_sum      = 0.0f;
        vo.lvl_n        = 0;
        const float msp = vo.mspeed > 0.05f ? vo.mspeed : 0.05f;
        const float tau = 0.4f / msp;
        Approach(vo.fast, lvl, Coeff(tau, dt));
        Approach(vo.slow, lvl, Coeff(tau * 8.0f, dt));
        if(mod_mode == kModFollow)
        {
            // 45 is measured: at 14 this sat at an rms of 0.09 and read as nothing
            vo.mod = Shape((vo.fast - vo.slow) * 45.0f);
        }
        else if(mod_mode == kModLfo)
        {
            // Geared down 10x: straight across was 0.18-0.55 Hz and read as an LFO
            constexpr float kLfoGear = 0.1f;
            vo.mod_ph += (uint32_t)(vo.frate * kLfoGear * dt * 4294967296.0f);
            vo.mod = SinQ(vo.mod_ph);
        }
        else
        {
            vo.step_left -= dt;
            if(vo.step_left <= 0.0f)
            {
                vo.step_left = (0.5f + Rand() * 1.5f) / vo.frate;
                vo.step_t    = Rnd();
            }
            // A step straight into feedback gain is a click, and 30 ms still reads as a jump
            Approach(vo.mod, vo.step_t, Coeff(0.03f, dt));
        }

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

// Advanced once per RENDERED block: Control skips blocks under load, and a
// crossfade stepped from there stalls mid-fade.
void AdvanceTaps(float dt)
{
    for(int i = 0; i < wfc::kVoices; i++)
    {
        Voice& vo = v[i];
        // A break DRAGS the tap and that scrub is the gesture; a knob move crossfades instead
        const bool sweeping = vo.sweep > 0.0f;
        if(sweeping)
            vo.sweep -= dt;
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
                // The main tap SNAPS to where the fade left it: glided, it pops once per fade
                vo.ds_p = vo.ds2_p = -1.0f;
            }
            if(vo.xf > 0.0f)
                vo.xf_t = fminf(vo.xf + dt / kXfadeT, 1.0f);
            else
            {
                const float d  = vo.dly_t - vo.dly;
                const float ad = d < 0.0f ? -d : d;
                // A crossfade splices the loop onto a decorrelated part of its line
                if(vo.dly_jump)
                {
                    // One fade at a time: a target landing mid-fade waits a block or two
                    vo.dly_jump = false;
                    vo.dly_n    = vo.dly_t;
                    vo.xf_t     = dt / kXfadeT;
                }
                else if(ad > 1e-5f)
                {
                    // Capped so the tap cannot outrun the write head and read backwards
                    float       st  = (vo.dly_t - vo.dly) * Coeff(kKnobGlide, dt);
                    const float lim = kMaxSlew * dt;
                    if(st > lim)
                        st = lim;
                    else if(st < -lim)
                        st = -lim;
                    vo.dly += st;
                    vo.dly_n = vo.dly;
                }
            }
        }
    }
}

// Wrapped by subtraction, not %: kDelayMax is not a power of two, and that is five divides a sample
inline float Rd(const float* line, int wr, float d)
{
    const float dc = Clamp(d, 2.0f, (float)(kDelayMax - 2));
    const int   di = (int)dc;
    const float fr = dc - (float)di;
    int         i0 = wr - di;
    if(i0 < 0)
        i0 += kDelayMax;
    int i1 = i0 - 1;
    if(i1 < 0)
        i1 += kDelayMax;
    return line[i0] + (line[i1] - line[i0]) * fr;
}

void Process(float* out_l, float* out_r, int n, const float* in,
             float in_mix)
{
    Approach(link_g, Clamp(wfc::K().link, 0.0f, 1.0f),
             Coeff(0.05f, (float)n * inv_sr));
    const float lret = kLinkRet * link_g;
    const bool  lact = link_g > 1.0e-4f;

    AdvanceTaps((float)n * inv_sr);
    // Resolved once a block: the mod and every smoothed value hold still across it
    float dsamp[wfc::kVoices], dsamp2[wfc::kVoices], fbg[wfc::kVoices],
        preg[wfc::kVoices], postg[wfc::kVoices];
    // Only populated for a voice that is mid-crossfade
    float dsampn[wfc::kVoices], xf[wfc::kVoices], xfi[wfc::kVoices];
    // Per-sample steps for the three taps, so each glides across the block
    float dsampi[wfc::kVoices], dsamp2i[wfc::kVoices], dsampni[wfc::kVoices];
    const float inv_n = n > 0 ? 1.0f / (float)n : 0.0f;
    for(int i = 0; i < wfc::kVoices; i++)
    {
        Voice& vo = v[i];
        // The mod ADDS to these, exactly as a Web Audio mod on an AudioParam does
        const float d  = (vo.dly + vo.mod * vo.m_dly) * sr;
        const float dg = Clamp(d, 2.0f, (float)(kDelayMax - 2));
        if(vo.ds_p < 0.0f)
            vo.ds_p = dg;
        dsamp[i]  = vo.ds_p;
        dsampi[i] = (dg - vo.ds_p) * inv_n;
        vo.ds_p   = dg;
        // Second tap 1.3% further back, so the loop reads wide instead of as one point
        const float d2 = Clamp(dg * 1.013f, 2.0f, (float)(kDelayMax - 2));
        if(vo.ds2_p < 0.0f)
            vo.ds2_p = d2;
        dsamp2[i]  = vo.ds2_p;
        dsamp2i[i] = (d2 - vo.ds2_p) * inv_n;
        vo.ds2_p   = d2;
        xf[i]     = vo.xf;
        xfi[i]    = n > 0 ? (vo.xf_t - vo.xf) / (float)n : 0.0f;
        // Not glided: this is the tap a crossfade moves TO, and a break must land at once
        const float dn = (vo.dly_n + vo.mod * vo.m_dly) * sr;
        const float ng = Clamp(dn, 2.0f, (float)(kDelayMax - 2));
        dsampn[i]      = ng;
        dsampni[i]     = 0.0f;
        fbg[i]        = Clamp(vo.fb + vo.mod * vo.m_fb, 0.0f, 1.3f);
        preg[i]       = vo.pre + vo.mod * vo.m_pre;
        if(preg[i] < 0.01f)
            preg[i] = 0.01f;
        // post is NOT modulated: that is what lets the mod change the loop's gain, not its colour
        postg[i] = 1.0f / vo.pre;
        // post is left alone, so this changes the loop's grit and not its level
        preg[i] *= 1.0f + kLinkDrv * link_env * link_g;

        // Mid-crossfade both taps sound; report the louder, so the display matches what you hear
        vo.eff_dly = (xf[i] > 0.5f ? ng : dg) * inv_sr;
        vo.eff_fb  = fbg[i];
        vo.eff_pre = preg[i];
    }

    for(int s = 0; s < n; s++)
    {
        float l = 0.0f, r = 0.0f;
        const float lpv  = link_prev;
        float       lsum = 0.0f;
        // Captured first, so every feedback tap this sample reads the same previous output
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
            // The material. Noise before the filter, so both it and the loop shape it.
            if(M().noise != 0.0f)
                src += Rnd() * M().noise;
            // Drive AHEAD of the line dirties the source, not the echoes
            if(M().drive != 0.0f)
                src = Shape(src * (1.0f + 8.0f * M().drive));
            src *= M().out;
            // The partials keep running underneath, so the fade back is in phase
            if(in)
                src = src * (1.0f - in_mix) + in[s] * in_mix * 0.5f;

            const float dry = vo.vlp.Process(src) * vo.vca;

            // Into the line: the voice, its own feedback, and whatever is in the link
            const float in = dry + fbg[i] * prev[i] + lret * lpv;

            // Write, then read `dsamp` back, interpolated: the glide moves the tap mid-block
            vo.line[vo.wr] = in;
            const float out = Rd(vo.line, vo.wr, dsamp[i]);
            float outm = out, o2;
            if(xf[i] > 0.0f)
            {
                // The wide tap crossfades too, or a turning knob holds the voice mid-fade
                const float nv = Rd(vo.line, vo.wr, dsampn[i]);
                const float wv = Rd(vo.line, vo.wr, dsampn[i] * 1.013f);
                const float ov = Rd(vo.line, vo.wr, dsamp2[i]);
                // Equal power by sqrt, not sin/cos: 12 libm calls a sample broke the audio
                const float x  = xf[i] > 1.0f ? 1.0f : xf[i];
                const float ga = sqrtf(1.0f - x), gb = sqrtf(x);
                outm           = out * ga + nv * gb;
                o2             = ov * ga + wv * gb;
                xf[i] += xfi[i];
            }
            else
            {
                o2 = Rd(vo.line, vo.wr, dsamp2[i]);
            }

            vo.wr           = vo.wr + 1 >= kDelayMax ? 0 : vo.wr + 1;

            const float lp  = vo.llp.Process(outm);
            vo.loop_hp += (lp - vo.loop_hp) * loop_hp_c;
            const float f   = lp - vo.loop_hp;
            const float sh  = Shape(f * preg[i]);
            const float pst = sh * postg[i];
            vo.post_prev    = pst;
            lsum += pst;
            vo.lvl_sum += pst < 0.0f ? -pst : pst;
            vo.lvl_n++;

            // The side tap gets its own copy of the loop's filters: unfiltered it went dirty
            const float lp2 = vo.llp2.Process(o2);
            vo.loop_hp2 += (lp2 - vo.loop_hp2) * loop_hp_c;
            const float pst2 = Shape((lp2 - vo.loop_hp2) * preg[i]) * postg[i];

            // One tap per channel: panning the side tap opposite only pulled the centre
            const float mix = dry * (1.0f - in_mix) * vo.dryl;
            const float cl = (mix + pst * vo.wet) * vo.pan_l * vo.outg;
            const float ca = cl < 0.0f ? -cl : cl;
            if(ca > vo.pk)
                vo.pk = ca;
            l += cl;
            r += (mix + pst2 * vo.wet) * vo.pan_r * vo.outg;
            dsamp[i] += dsampi[i];
            dsamp2[i] += dsamp2i[i];
            dsampn[i] += dsampni[i];
        }
        if(lact)
        {
            link_mem[link_wr] = lsum * kLinkSend + kLinkFb * link_prev;
            const float co = Rd(link_mem, link_wr, kLinkDly * sr);
            link_wr        = link_wr + 1 >= kDelayMax ? 0 : link_wr + 1;
            link_lp += (co - link_lp) * link_lp_c;
            // Soft clip, not a limiter: the path out and back can pass unity at high feedback
            link_prev = Shape(link_lp);
            link_env += (fabsf(link_prev) - link_env) * link_env_c;
        }
        else
            link_prev = link_lp = link_env = 0.0f;

        out_l[s] += l;
        out_r[s] += r;
    }
}

}  // namespace voices
