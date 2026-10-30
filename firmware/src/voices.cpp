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

// Not a plain harmonic stack: the top two are unisons of the fundamental and
// the octave rather than the 7th and 8th harmonics. A full harmonic series at
// a shallow rolloff is a sawtooth, which is what read as "oscillator"; these
// two beat against the ones below them instead of buzzing above them.
constexpr float kRatio[kPartials] = {1.0f, 2.0f, 3.0f, 4.0f,
                                     5.0f, 6.0f, 1.0f, 2.0f};

float sr        = 48000.0f;
float inv_sr    = 1.0f / 48000.0f;
int   block     = 48;

// Each trip round the loop drops more bass. A lowpass-only loop keeps the
// low-mids and rings them for ever, which is the whole of the mud; with this
// the repeats thin out as they pile up.
constexpr float kLoopHpHz = 150.0f;
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

    Svf vlp, llp;

    // The delay line, written and read once a sample
    float* line;
    int    wr;

    // Everything smoothed, with its target recomputed each control tick
    float vca, vca_t;
    // Lifted for a few seconds after a press, so a new pad speaks before it
    // dissolves into the wash
    float fresh;
    float loop_hp;   // the loop highpass's state
    float vcut, vcut_t;
    float dly, dly_t;        // seconds
    float fb, fb_t;
    float lcut, lcut_t;
    float pre, pre_t;
    float wet, wet_t;
    float cab[wfc::kVoices], cab_t[wfc::kVoices];

    // The LFO, and the three depths it adds through
    uint32_t lph;
    uint32_t linc;
    float    lrate, lrate_t;
    float    l_dly, l_dly_t;
    float    l_fb, l_fb_t;
    float    l_pre, l_pre_t;
    float    lfo;   // its value this tick

    // What the loop's distortion put out last sample: the feedback path and
    // every cable tap read this, which is what breaks the algebraic loop
    float post_prev;

    float pan_l, pan_r;
};

Voice     v[wfc::kVoices];
float SLOW_BSS delay_mem[wfc::kVoices][kDelayMax];

// The coefficients, rebuilt whenever dt changes (which is never, in practice)
struct
{
    float dt;
    float vca, vcut, fb, lcut, pre, wet, cab, lfo_rate, lfo_depth, dly;
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
    co.lfo_rate  = Coeff(0.3f, dt);
    co.lfo_depth = Coeff(0.08f, dt);
    co.dly       = Coeff(glide, dt);
}

void SetPartialFreqs(Voice& vo, bool snap)
{
    // Low notes get a shallower rolloff. The ear is least sensitive down
    // there and four loops put their own fundamentals in the same region, so
    // a note at the bottom octave needs more upper partials to say what one
    // at the top says with six. Without this the timbre is pitch-proportional
    // and the bottom octave is always the one that gets buried.
    const float tilt = Clamp(1.9f - 0.55f * logf(440.0f / vo.hz) * 1.442695f,
                             1.25f, 1.9f);
    for(int k = 0; k < kPartials; k++)
    {
        Partial& p = vo.p[k];
        p.base     = powf(kRatio[k], -tilt) * (k >= 6 ? 0.45f : 1.0f);
        if(snap)
            p.lvl = p.lvl_t = p.base;
        else
            p.lvl_t = p.base;
        const float kk = kRatio[k];
        // A touch of inharmonicity, so the partials beat rather than lock
        const float f  = vo.hz * kk * (1.0f + 0.00015f * kk * kk);
        const float det = powf(2.0f, p.det / 1200.0f);
        p.inc = (uint32_t)(f * det * 4294967296.0f * inv_sr);
        if(snap)
            p.phase = (uint32_t)(Rand() * 4294967296.0f);
    }
}

}  // namespace

// ----------------------------------------------------------------- the API

void Init(float sample_rate, int block_size)
{
    sr     = sample_rate;
    inv_sr = 1.0f / sr;
    block  = block_size;
    loop_hp_c = 1.0f - expf(-2.0f * 3.14159265f * kLoopHpHz * inv_sr);

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
        vo.vlp.Reset();
        vo.llp.Reset();
        vo.vlp.Set(400.0f, 0.8f);
        vo.llp.Set(2000.0f, 0.5f);
        vo.dly = vo.dly_t = 0.2f;
        vo.pre = vo.pre_t = 1.0f;
        vo.lrate = vo.lrate_t = 0.5f;
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
        const float pan = ((float)i / (float)(wfc::kVoices - 1) * 2.0f - 1.0f)
                          * 0.6f;
        const float a = (pan + 1.0f) * 0.25f * 3.14159265f;
        vo.pan_l      = cosf(a);
        vo.pan_r      = sinf(a);
    }
    Recoeff((float)block * inv_sr, wfc::DelayGlide());
}

void SetPitch(int i, float hz, bool snap)
{
    if(snap)
        v[i].fresh = 1.0f;
    v[i].hz = hz;
    SetPartialFreqs(v[i], snap);
}

float Pitch(int i) { return v[i].hz; }
float Amp(int i) { return v[i].amp; }

float Level()
{
    float m = 0.0f;
    for(int i = 0; i < wfc::kVoices; i++)
        if(v[i].amp > m)
            m = v[i].amp;
    return m;
}

void Control(float now, float dt)
{
    const float glide = wfc::DelayGlide();
    if(dt != co.dt || Coeff(glide, dt) != co.dly)
        Recoeff(dt, glide);

    const float base  = wfc::DelayBase();
    const float fbm   = wfc::FbMul();
    const float drvm  = wfc::DrvMul();
    const float lfom  = wfc::LfoMul();
    const float lcut  = wfc::LoopCut();
    const float vcm   = wfc::VoiceCutMul();
    const float lrate0 = wfc::LfoRate0();
    const float atk   = wfc::AttackT();
    const float rel   = wfc::ReleaseT();
    // The longest delay the buffer can serve, with a sample spare for the
    // interpolator's second tap
    const float dly_max = (float)(kDelayMax - 2) * inv_sr;

    // A press makes room for itself: the other voices' wash steps back for as
    // long as the new one is fresh. The feedback is untouched, so the rules
    // still ring exactly as WFC set them -- only the output tap ducks.
    float duck[wfc::kVoices];
    for(int i = 0; i < wfc::kVoices; i++)
    {
        duck[i] = 0.0f;
        for(int j = 0; j < wfc::kVoices; j++)
            if(j != i && v[j].fresh > duck[i])
                duck[i] = v[j].fresh;
    }

    for(int i = 0; i < wfc::kVoices; i++)
    {
        Voice&     vo = v[i];
        const bool on = wfc::Sounding(i);

        // The envelope, which follows the Delay knob: long echoes get a long
        // release so the loop is still fed while it rings
        {
            const float tgt = on ? 1.0f : 0.0f;
            const float tau = tgt > vo.amp ? (atk / 3.0f > 0.005f ? atk / 3.0f
                                                                 : 0.005f)
                                           : (rel / 3.0f > 0.03f ? rel / 3.0f
                                                                 : 0.03f);
            Approach(vo.amp, tgt, Coeff(tau, dt));
        }

        // What the wave has actually delivered to this voice
        const int   tile = wfc::ShownTile(i);
        const int   po   = wfc::ShownPatch(i);
        const int   lo   = wfc::ShownLfo(i);
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
                p.walk  = Clamp(p.walk + Rnd() * 1.6f, -16.0f, 16.0f);
                p.det_t = p.det_base + p.walk;
                p.lvl_t = p.base * (1.0f - 0.51f * Rand());
                p.det_c = Coeff(2.5f, dt);
                p.lvl_c = Coeff(1.2f + Rand() * 2.0f, dt);
                p.next  = now + 0.8f + Rand() * 2.0f;
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
        const float t0 = base * D * (skew ? 1.19f : 1.0f);
        float       d  = skew ? t0
                              : floorf(t0 * vo.hz + 0.5f) / vo.hz;
        if(!skew && d * vo.hz < 1.0f)
            d = 1.0f / vo.hz;
        const float dly = Clamp(d, 0.02f, dly_max < 1.9f ? dly_max : 1.9f);
        const float pre = 1.0f + 12.0f * R * drvm;

        vo.dly_t  = dly;
        vo.fb_t   = feff;
        vo.lcut_t = lcut;
        vo.pre_t  = pre;
        vo.wet_t  = 0.8f * sqrtf(pre) * 0.7f * (1.0f - 0.45f * duck[i]);
        Approach(vo.fresh, 0.0f, co.fresh);
        vo.vca_t  = vo.amp * 0.26f * (1.0f + 0.9f * vo.fresh);
        vo.vcut_t = Clamp(vo.hz * vcm * (0.18f + 0.82f * vo.amp)
                              * (1.0f + vo.fresh),
                          150.0f,
                          12000.0f);

        // The LFO rides the envelope, so a silent voice's line is still
        const float dpe = wfc::DP[wfc::LfoDepth(lo)] * lfom * vo.amp;
        vo.lrate_t      = lrate0 * wfc::LR[wfc::LfoRate(lo)];
        vo.l_dly_t      = dly * 0.08f * dpe;
        vo.l_fb_t       = 0.2f * dpe;
        vo.l_pre_t      = pre * 0.35f * dpe;

        // The cable, which is live only while its SOURCE voice sounds
        const int tgt = wfc::OptTarget(po);
        for(int j = 0; j < wfc::kVoices; j++)
            vo.cab_t[j] = (on && j == tgt)
                              ? wfc::AMT[wfc::OptAmt(po)] * sqrtf(pre) * 0.8f
                              : 0.0f;

        Approach(vo.dly, vo.dly_t, co.dly);
        Approach(vo.fb, vo.fb_t, co.fb);
        Approach(vo.lcut, vo.lcut_t, co.lcut);
        Approach(vo.pre, vo.pre_t, co.pre);
        Approach(vo.wet, vo.wet_t, co.wet);
        Approach(vo.vca, vo.vca_t, co.vca);
        Approach(vo.vcut, vo.vcut_t, co.vcut);
        Approach(vo.lrate, vo.lrate_t, co.lfo_rate);
        Approach(vo.l_dly, vo.l_dly_t, co.lfo_depth);
        Approach(vo.l_fb, vo.l_fb_t, co.lfo_depth);
        Approach(vo.l_pre, vo.l_pre_t, co.lfo_depth);
        for(int j = 0; j < wfc::kVoices; j++)
            Approach(vo.cab[j], vo.cab_t[j], co.cab);

        // The LFO advances once a tick: it is sub-Hz to a few Hz, so there is
        // nothing to gain from running it per sample.
        // ponytail: block-rate modulation, ~1 ms steps. Fine for this LFO and
        // for cross-FEED cables, which carry audio and modulate nothing. If
        // the cables become cross-MODULATION, the modulator is audio and this
        // has to move into Process() per sample.
        vo.linc = (uint32_t)(vo.lrate * 4294967296.0f * dt);
        vo.lph += vo.linc;
        vo.lfo = SinQ(vo.lph);

        // The filters take their coefficients once a tick, not once a sample
        vo.vlp.Set(vo.vcut, 0.8f);
        vo.llp.Set(vo.lcut, 0.5f);
    }
}

void Process(float* out_l, float* out_r, int n)
{
    // Resolved once a block: the LFO and every smoothed value hold still
    // across it, which is what makes the per-sample loop cheap
    float dsamp[wfc::kVoices], fbg[wfc::kVoices], preg[wfc::kVoices],
        postg[wfc::kVoices];
    for(int i = 0; i < wfc::kVoices; i++)
    {
        Voice& vo = v[i];
        // The LFO ADDS to these, exactly as a Web Audio LFO connected to an
        // AudioParam does
        const float d = (vo.dly + vo.lfo * vo.l_dly) * sr;
        dsamp[i]      = Clamp(d, 2.0f, (float)(kDelayMax - 2));
        fbg[i]        = Clamp(vo.fb + vo.lfo * vo.l_fb, 0.0f, 1.3f);
        preg[i]       = vo.pre + vo.lfo * vo.l_pre;
        if(preg[i] < 0.01f)
            preg[i] = 0.01f;
        // post is NOT modulated: that asymmetry is what lets the LFO change
        // the loop's gain and not just its colour
        postg[i] = 1.0f / vo.pre;
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
            src *= 0.3f;

            // The voice itself, before its line
            const float dry = vo.vlp.Process(src) * vo.vca;

            // What goes into the line: the voice, its own feedback, and any
            // cable pointed at it
            float in = dry + prev[i] * fbg[i];
            for(int j = 0; j < wfc::kVoices; j++)
                if(j != i)
                    in += prev[j] * v[j].cab[i];

            // Write, then read `dsamp` samples back, interpolated because the
            // LFO and the repair glide both move the tap
            vo.line[vo.wr] = in;
            const int   di  = (int)dsamp[i];
            const float fr  = dsamp[i] - (float)di;
            const int   i0  = (vo.wr - di + kDelayMax) % kDelayMax;
            const int   i1  = (i0 - 1 + kDelayMax) % kDelayMax;
            const float out = vo.line[i0] + (vo.line[i1] - vo.line[i0]) * fr;
            vo.wr           = (vo.wr + 1) % kDelayMax;

            // The loop's filters and its distortion
            const float lp  = vo.llp.Process(out);
            vo.loop_hp += (lp - vo.loop_hp) * loop_hp_c;
            const float f   = lp - vo.loop_hp;
            const float sh  = Shape(f * preg[i]);
            const float pst = sh * postg[i];
            vo.post_prev    = pst;

            // Dry voice and wet line both go to the bus, panned together
            const float mix = dry + pst * vo.wet;
            l += mix * vo.pan_l;
            r += mix * vo.pan_r;
        }
        out_l[s] += l;
        out_r[s] += r;
    }
}

}  // namespace voices
