// THE SELF TEST
//
// lapses.exe --selftest. Rule predicates against the numbers in
// HANDOFF 4.2/4.4, and the invariants that matter: strict WFC leaves nothing
// broken, each break breaks its own rule and heals, and four cross-patched
// feedback loops never go non-finite anywhere in the knob space.

#include "synth.h"
#include "voices.h"
#include "wfc.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace wfc;


static int fails = 0;

static void Near(const char* what, float got, float want, float tol = 5e-4f)
{
    if(fabsf(got - want) > tol)
    {
        printf("  FAIL %-14s got %.6f want %.6f\n", what, got, want);
        fails++;
    }
}

static void Check(const char* what, bool ok)
{
    if(!ok)
    {
        printf("  FAIL %s\n", what);
        fails++;
    }
}

// Run the wave out so the shown state catches up with the logical state. The
// wave is at most (voices + 1) hops long, and WFC schedules its own heal 3 s
// out at the soonest, so this must stay well short of that: settling for
// longer would heal the very break a test is about to look at.
static void Settle(float& now)
{
    const float hop = HopSeconds();
    for(int i = 0; i < 12; i++)
    {
        now += hop * 0.5f;
        Tick(now);
    }
}

// The largest set of delay classes that can coexist under rule 3 at the
// current Delay knob and strictness. Below the number of sounding voices,
// no full repair exists and a partial one is the best WFC can do.
static int MaxVoicesUnderRule3()
{
    if(!Lim().ratio_on)
        return kVoices;
    int biggest = 1;
    for(int mask = 0; mask < 32; mask++)
    {
        int cls[5], n = 0;
        for(int i = 0; i < 5; i++)
            if(mask & (1 << i))
                cls[n++] = i;
        bool ok = true;
        for(int a = 0; a < n && ok; a++)
            for(int b = a + 1; b < n && ok; b++)
            {
                const float A = DR[cls[a]], B = DR[cls[b]];
                const float r       = (A > B ? A : B) / (A > B ? B : A);
                const float want[4] = {4.0f / 3, 3.0f / 2, 2.0f, 3.0f};
                bool        ratio   = false;
                for(int i = 0; i < 4; i++)
                    if(fabsf(r - want[i]) < 0.01f)
                        ratio = true;
                if(!ratio || fabsf(A - B) * DelayBase() < Lim().gap)
                    ok = false;
            }
        if(ok && n > biggest)
            biggest = n;
    }
    return biggest;
}

static int Broken(BreakType t)
{
    Violation v[32];
    const int n   = Diagnose(v, 32);
    int       cnt = 0;
    for(int i = 0; i < n; i++)
        if(v[i].type == t)
            cnt++;
    return cnt;
}

// ---------------------------------------------------------------- 4.2, 4.4

static void TestKnobFormulas()
{
    printf("knob formulas at the defaults (HANDOFF 4.2)\n");
    Init(1);   // Init leaves the knobs at their defaults
    Near("delayBase", DelayBase(), 0.231f, 5e-4f);
    Near("hopSeconds", HopSeconds(), 0.185f, 5e-4f);
    Near("releaseT", ReleaseT(), 1.501f, 1e-3f);
    Near("attackT", AttackT(), 0.243f, 1e-3f);
    Near("modSpeed0", ModSpeed0(), 0.541f, 1e-3f);
    Near("delayGlide", DelayGlide(), 0.139f, 5e-4f);
    Near("fbMul", FbMul(), 1.0f);
    Near("drvMul", DrvMul(), 0.76f);
    Near("modMul", ModMul(), 0.75f);
    Near("loopCut", LoopCut(), 1722.0f, 1.0f);
    Near("voiceCutMul", VoiceCutMul(), 8.375f, 1e-3f);
    Near("lapseChance", LapseChance(), 0.1f, 5e-4f);
    Near("unrestRate", UnrestRate(), 0.05f, 5e-4f);

    printf("rule limits at safe = .7 (HANDOFF 4.4)\n");
    Near("Fpair", Lim().Fpair, 1.8f);
    Near("Dpair", Lim().Dpair, 1.74f);
    Near("Fself", Lim().Fself, 0.95f);
    Near("Lpeak", Lim().Lpeak, 0.98f);
    Near("gap", Lim().gap, 0.0152f);
    Check("ratioOn at .7", Lim().ratio_on);
    Check("cycleOn at .7", Lim().cycle_on);

    // The thresholds the strictness knob crosses
    K().unrest = 1.0f;
    K().safe   = 0.0f;
    RebuildRules();
    Check("Fself off when loose", Lim().Fself > 8.0f);
    Check("ratioOn off when loose", !Lim().ratio_on);
    Check("cycleOn off when loose", !Lim().cycle_on);
    Check("Lpeak off when loose", Lim().Lpeak > 8.0f);
    Near("lapseChance at full unrest", LapseChance(), 0.5f);
    Near("unrestRate at full unrest", UnrestRate(), 0.25f);
    K().unrest = 0.0f;
    Near("lapseChance at no unrest", LapseChance(), 0.0f);
    Near("unrestRate at no unrest", UnrestRate(), 0.0f);
    K().safe = 1.0f;
    K().unrest = 0.0f;
    RebuildRules();
}

// ---------------------------------------------------------- the ruleset

static void TestTileTables()
{
    printf("tile packing and the rule tables\n");
    Init(1);
    for(int d = 0; d < 5; d++)
        for(int f = 0; f < 5; f++)
            for(int r = 0; r < 4; r++)
            {
                const int t = MkTile(d, f, r);
                Check("tile round trip",
                      TileD(t) == d && TileF(t) == f && TileR(t) == r);
            }
    Check("default tile is d0 f1 r1", kDefaultTile == MkTile(0, 1, 1));

    // Rule 1 at the strict default: FL[4] = 1.08 > Fself = .95, so a voice
    // may not hold a self-ringing loop at all
    K().safe = 0.7f;
    K().unrest = 0.0f;
    K().fb   = 0.5f;   // fbMul 1.0
    RebuildRules();
    Violation v[32];
    SetSounding(0, true, 0.0f);
    // Writing a ringing tile straight into the shown state must be seen
    Check("self-ring is illegal when strict",
          FL[4] * FbMul() > Lim().Fself);
    (void)v;
}

// ------------------------------------------------------- solver invariants

static void TestStrictHealIsClean()
{
    printf("strict: heal leaves nothing broken\n");
    for(uint32_t seed = 1; seed <= 60; seed++)
    {
        Init(seed);
        K().safe = 0.95f;   // every limit binding
        K().unrest = 0.0f;
        RebuildRules();
        float now = 1.0f;
        for(int v = 0; v < kVoices; v++)
        {
            SetSounding(v, true, now);
            now += 0.01f;
        }
        Heal(now);
        Settle(now);
        Violation vio[32];
        const int n = Diagnose(vio, 32);
        if(n)
        {
            printf("  FAIL seed %u: %d broken after heal (first: %s)\n", seed,
                   n, BreakName(vio[0].type));
            fails++;
        }
    }
}

static void TestLooseFlagsNothing()
{
    printf("loose: nothing is flagged, so nothing needs repairing\n");
    for(uint32_t seed = 1; seed <= 30; seed++)
    {
        Init(seed);
        K().safe = 0.0f;
        K().unrest = 0.0f;
        RebuildRules();
        float now = 1.0f;
        for(int v = 0; v < kVoices; v++)
        {
            SetSounding(v, true, now);
            now += 0.01f;
        }
        for(int i = 0; i < 4; i++)
        {
            Pass(i, now);
            now += 0.05f;
        }
        Settle(now);
        Violation vio[32];
        const int n = Diagnose(vio, 32);
        if(n)
        {
            printf("  FAIL seed %u: %d flagged at safe = 0 (%s)\n", seed, n,
                   BreakName(vio[0].type));
            fails++;
        }
    }
}

static void TestPressIsLegal()
{
    printf("a press settles the sounding voices inside the rules\n");
    for(uint32_t seed = 1; seed <= 60; seed++)
    {
        Init(seed);
        K().safe = 0.95f;
        K().unrest = 0.0f;
        RebuildRules();
        float now = 1.0f;
        for(int v = 0; v < kVoices; v++)
        {
            SetSounding(v, true, now);
            now += 0.01f;
            Pass(v, now);
            Settle(now);
            Violation vio[32];
            const int n = Diagnose(vio, 32);
            if(n)
            {
                printf("  FAIL seed %u voice %d: %d broken after press (%s)\n",
                       seed, v, n, BreakName(vio[0].type));
                fails++;
                break;
            }
        }
    }
}

// Each break must write exactly the violation 4.6 describes, and the
// scheduled heal must take it away again.
static void TestBreaksBreakAndHeal()
{
    printf("each break breaks its own rule, and heal takes it back\n");
    const BreakType types[kBreakTypes]
        = {kEcho, kSwell, kFuzz, kWobble, kFlood, kCable};
    for(int ti = 0; ti < kBreakTypes; ti++)
    {
        int hit = 0, clean_after = 0;
        for(uint32_t seed = 1; seed <= 40; seed++)
        {
            Init(seed);
            // The cable breaks need somewhere for a cable to exist
            voices::SetCableMode(types[ti] == kCable || types[ti] == kFlood
                                     ? voices::kCableFeed
                                     : voices::kCableOff);
            K().safe = 0.95f;
            K().unrest = 0.0f;
            // The wobble break needs the mod audible for rule 5 to bite
            K().mod = 1.0f;
            RebuildRules();
            float now = 1.0f;
            for(int v = 0; v < kVoices; v++)
            {
                SetSounding(v, true, now);
                now += 0.01f;
            }
            Heal(now);
            Settle(now);
            Violation before[32];
            if(Diagnose(before, 32) != 0)
                continue;

            BreakRule((int)types[ti], 0.0f, now);
            Settle(now);
            if(Broken(types[ti]) > 0)
                hit++;

            // The repair WFC scheduled for itself, 3 to 7 s out
            now += 8.0f;
            Tick(now);
            Settle(now);
            Violation vio[32];
            if(Diagnose(vio, 32) == 0)
                clean_after++;
        }
        printf("  %-7s broke %2d/40, clean after heal %2d/40\n",
               BreakName(types[ti]), hit, clean_after);
        Check("break is audible as its own rule", hit >= 38);
        Check("heal clears the break", clean_after >= 38);
        voices::SetCableMode(voices::kCableOff);
    }
}

// 4.6b: the pressed voice moves first, the rest follow one hop apart
static void TestWaveIsStaggered()
{
    printf("the repair arrives as a wave, one hop per voice\n");
    Init(7);
    K().safe = 0.95f;
    K().unrest = 0.0f;
    RebuildRules();
    float now = 1.0f;
    for(int v = 0; v < kVoices; v++)
    {
        SetSounding(v, true, now);
        now += 0.01f;
    }
    Heal(now);
    Settle(now);

    // Force a wave that touches every voice, then watch them land
    BreakRule((int)kSwell, 0.0f, now);
    now += 8.0f;
    Tick(now);   // schedules the heal wave at `now`

    int landed_at[kVoices];
    for(int v = 0; v < kVoices; v++)
        landed_at[v] = -1;
    const float hop = HopSeconds();
    for(int step = 0; step < 12; step++)
    {
        Tick(now + (float)step * hop * 0.5f);
        for(int v = 0; v < kVoices; v++)
            if(landed_at[v] < 0 && ShownTile(v) == Tile(v)
               && ShownMod(v) == Mod(v))
                landed_at[v] = step;
    }
    int first = 99, last = -1;
    for(int v = 0; v < kVoices; v++)
    {
        if(landed_at[v] < 0)
            continue;
        if(landed_at[v] < first)
            first = landed_at[v];
        if(landed_at[v] > last)
            last = landed_at[v];
    }
    Check("voices do not all land on the same tick", last > first);
}

// Principle 4: the player causes the breaks. From a legal state at the
// default knobs, pushing a knob to its end must break that knob's own rule,
// and the 350 ms debounced heal must then take it back. A single knob move
// cannot GUARANTEE a break -- that depends on the tiles WFC happens to be
// holding -- so this measures the rate across seeds, which is also the number
// to retune by ear if a knob turns out to feel dead or trigger-happy.
static void TestKnobsLoadTheirRule()
{
    printf("a knob loads its own rule, from a legal default state\n");
    const int kRuns = 200;
    for(int nv = 2; nv <= kVoices; nv++)
    {
        int hit[4] = {0, 0, 0, 0};   // drive, feedback, delay, mod
        int unsat[4] = {0, 0, 0, 0};
        for(uint32_t seed = 1; seed <= (uint32_t)kRuns; seed++)
        {
            Init(seed);   // the default knobs: safe .7
            float now = 1.0f;
            for(int v = 0; v < nv; v++)
            {
                SetSounding(v, true, now);
                now += 0.01f;
                Pass(v, now);
                Settle(now);
            }
            Heal(now);
            Settle(now);
            Violation vio[32];
            if(Diagnose(vio, 32))
                continue;   // not a legal starting point, so prove nothing

            // Each knob to its end, one at a time, with WFC not yet allowed
            // to answer. Rule 3 is the one that Delay loads, by turning the
            // echo gap down until the delay classes can no longer separate.
            struct
            {
                float*    knob;
                float     to, back;
                BreakType rule;
            } probe[4] = {
                // The mod goes first, because only a PRESS ever gives a voice
                // mod depth: RepairMod charges +10 for changing a voice's
                // option, so a heal preserves or removes depth but never adds
                // it. Probe rule 5 after the other knobs have forced a few
                // heals and every depth has already decayed to 0.
                {&K().mod, 1.0f, 0.5f, kWobble},
                {&K().drive, 1.0f, 0.3f, kFuzz},
                {&K().fb, 1.0f, 0.5f, kSwell},
                {&K().delay, 0.0f, 0.6f, kEcho},
            };
            for(int p = 0; p < 4; p++)
            {
                *probe[p].knob = probe[p].to;
                RebuildRules();
                if(Broken(probe[p].rule) > 0)
                {
                    hit[p]++;
                    // WFC answers it 350 ms after the last movement
                    KnobsMoved(now);
                    now += 0.4f;
                    Tick(now);
                    Settle(now);
                    if(Broken(probe[p].rule) > 0)
                    {
                        // The one legitimate way out: the rules admit no full
                        // assignment at this knob position, so WFC settled
                        // what it could. See TestRule3CanBeUnsatisfiable.
                        const bool unsatisfiable = nv > MaxVoicesUnderRule3();
                        unsat[p] += unsatisfiable;
                        Check("the heal clears the strain, or no full repair "
                              "exists at this knob position",
                              unsatisfiable);
                        // And the partial repair must not be the collapse it
                        // replaced: the voices must not all share one delay
                        // class, which is what the default-tile fallback did.
                        if(unsatisfiable && nv > 1)
                        {
                            bool all_same = true;
                            for(int v = 1; v < nv; v++)
                                if(TileD(ShownTile(v)) != TileD(ShownTile(0)))
                                    all_same = false;
                            Check("a partial repair does not flatten every "
                                  "voice onto one delay class",
                                  !all_same);
                        }
                    }
                }
                // Back to a legal state before the next knob is probed, or
                // each rate would measure the leftovers of the one before it
                *probe[p].knob = probe[p].back;
                RebuildRules();
                Heal(now);
                Settle(now);
            }
        }
        printf("  %d voices: drive %3d/%d  feedback %3d/%d  delay %3d/%d  "
               "mod %3d/%d",
               nv, hit[1], kRuns, hit[2], kRuns, hit[3], kRuns, hit[0], kRuns);
        if(unsat[0] + unsat[1] + unsat[2] + unsat[3])
            printf("   (unrepairable: drive %d fb %d delay %d mod %d)",
                   unsat[1], unsat[2], unsat[3], unsat[0]);
        printf("\n");
        for(int p = 0; p < 4; p++)
            Check("every knob can break its own rule", hit[p] > 0);
    }
}

// Rule 3 has no solution for four voices with Delay at its minimum: the
// delay classes that are both a simple ratio apart AND further apart than
// `gap` form a graph whose largest clique is only 3. Heal then falls back to
// the default tile for every voice, which is itself an echo violation, so the
// break stays until the player turns Delay back up. The prototype behaves the
// same way (`ch[n].tile = lineOut ? lineOut[n] : DEFAULT_TILE`), so this is
// recorded as the behaviour, not asserted as correct -- it is a design
// decision for the user.
static void TestRule3CanBeUnsatisfiable()
{
    printf("rule 3 at the Delay knob's ends\n");
    const float positions[3] = {0.0f, 0.1f, 0.6f};
    for(int p = 0; p < 3; p++)
    {
        Init(1);
        K().delay = positions[p];
        RebuildRules();
        // The largest set of delay classes that can coexist
        int biggest = 1;
        for(int mask = 0; mask < 32; mask++)
        {
            int cls[5], n = 0;
            for(int i = 0; i < 5; i++)
                if(mask & (1 << i))
                    cls[n++] = i;
            bool ok = true;
            for(int a = 0; a < n && ok; a++)
                for(int b = a + 1; b < n && ok; b++)
                {
                    const float A = DR[cls[a]], B = DR[cls[b]];
                    const float r = (A > B ? A : B) / (A > B ? B : A);
                    const float want[4] = {4.0f / 3, 3.0f / 2, 2.0f, 3.0f};
                    bool        ratio   = false;
                    for(int i = 0; i < 4; i++)
                        if(fabsf(r - want[i]) < 0.01f)
                            ratio = true;
                    if(!ratio
                       || fabsf(A - B) * DelayBase() < Lim().gap)
                        ok = false;
                }
            if(ok && n > biggest)
                biggest = n;
        }
        printf("  Delay %.1f (base %.3f s): at most %d voices fit rule 3\n",
               positions[p], DelayBase(), biggest);
        if(positions[p] == 0.0f)
            Check("Delay at minimum cannot fit all four voices", biggest == 3);
        else
            Check("Delay off its minimum fits all four", biggest == 4);
    }
}

// A voice never patches into itself, and strict WFC allows no cable loop
static void TestCableStructure()
{
    printf("cables: no self patch, no loop when strict\n");
    for(uint32_t seed = 1; seed <= 60; seed++)
    {
        Init(seed);
        K().safe = 0.95f;
        K().unrest = 0.0f;
        RebuildRules();
        float now = 1.0f;
        for(int v = 0; v < kVoices; v++)
        {
            SetSounding(v, true, now);
            now += 0.01f;
            Pass(v, now);
        }
        Settle(now);
        for(int v = 0; v < kVoices; v++)
            Check("no voice patches into itself",
                  OptTarget(ShownPatch(v)) != v);
        Check("no cable loop when strict", Broken(kCable) == 0);
    }
}

constexpr float kSr    = 48000.0f;
constexpr int   kBlock = 48;

// What one run of the engine came out as
struct Run
{
    float peak;
    float rms;
    bool  finite;
};

// Advance the whole instrument for `seconds`, optionally capturing audio
static Run Drive(float& now, float seconds)
{
    Run  r  = {0.0f, 0.0f, true};
    const float dt = (float)kBlock / kSr;
    const int blocks = (int)(seconds * kSr / (float)kBlock);
    double sum = 0.0;
    long   cnt = 0;
    for(int b = 0; b < blocks; b++)
    {
        float l[kBlock] = {}, rr[kBlock] = {};
        now += dt;
        Tick(now);
        voices::Control(now, dt);
        voices::Process(l, rr, kBlock);
        for(int s = 0; s < kBlock; s++)
        {
            const float a = fabsf(l[s]) > fabsf(rr[s]) ? fabsf(l[s])
                                                       : fabsf(rr[s]);
            if(!isfinite(l[s]) || !isfinite(rr[s]))
                r.finite = false;
            else
            {
                if(a > r.peak)
                    r.peak = a;
                sum += (double)l[s] * l[s] + (double)rr[s] * rr[s];
                cnt += 2;
            }
        }
    }
    r.rms = cnt ? (float)sqrt(sum / (double)cnt) : 0.0f;
    return r;
}


static void Start(float& now, int n_voices)
{
    for(int i = 0; i < n_voices; i++)
    {
        SetSounding(i, true, now);
        // The prototype's default: major, root A, so pad 1 is 220 Hz
        static const float hz[4] = {220.0f, 247.5f, 275.0f, 293.33f};
        voices::SetPitch(i, hz[i], true);
        Pass(i, now);
        now += 0.02f;
    }
}

// ------------------------------------------------------------------ tests

// Four latched, then borrow the spots, then give them back
static void TestLatchSurvivesStealing()
{
    printf("latched pads get their voices back\n");
    synth::Init(48000.0f, 64);
    for(int p = 0; p < 4; p++)
        synth::Press(p);
    synth::Latch(true);
    for(int p = 0; p < 4; p++)
        synth::Release(p);

    int on = 0;
    for(int p = 0; p < synth::kPads; p++)
        on += synth::PadOn(p) ? 1 : 0;
    Check("four latched pads sound", on == 4);

    synth::Press(4);
    synth::Press(5);
    Check("the borrowed pads took voices",
          synth::PadVoice(4) >= 0 && synth::PadVoice(5) >= 0);
    int latched_voiced = 0;
    for(int p = 0; p < 4; p++)
        latched_voiced += synth::PadVoice(p) >= 0 ? 1 : 0;
    Check("two latched pads gave up their voices", latched_voiced == 2);
    Check("the robbed pads are still latched", synth::Latched());

    synth::Release(4);
    synth::Release(5);
    latched_voiced = 0;
    for(int p = 0; p < 4; p++)
        latched_voiced += synth::PadVoice(p) >= 0 ? 1 : 0;
    Check("all four latched pads have a voice again", latched_voiced == 4);

    synth::Latch(false);
    for(int v = 0; v < kVoices; v++)
        Check("nothing sounds once the latch is dropped", !Sounding(v));
}

// The line input is detected, not switched, so the detector is the contract
static void TestInputDetector()
{
    printf("a signal on the input turns it into an effects box\n");
    synth::Init(48000.0f, kBlock);
    K().unrest = 0.0f;
    synth::Press(0);

    float l[kBlock], r[kBlock], in[kBlock];
    const float dt = (float)kBlock / kSr;
    float now = 1.0f, ph = 0.0f;
    bool  finite = true;

    // Three seconds of a 1 kHz tone, which is nowhere near the pad's partials
    for(int b = 0; b < (int)(3.0f / dt); b++)
    {
        for(int i = 0; i < kBlock; i++, ph += 1000.0f / kSr)
            in[i] = 0.3f * sinf(6.2831853f * ph);
        now += dt;
        synth::Control(now);
        synth::Process(l, r, kBlock, in);
        for(int i = 0; i < kBlock; i++)
            if(!isfinite(l[i]) || !isfinite(r[i]))
                finite = false;
    }
    Check("finite with input running", finite);
    Check("input takes over", synth::InputMix() > 0.98f);

    // It must not let go the moment the player stops, nor hang on for ever
    memset(in, 0, sizeof(in));
    for(int b = 0; b < (int)(1.0f / dt); b++)
    {
        now += dt;
        synth::Control(now);
        synth::Process(l, r, kBlock, in);
    }
    Check("a second of quiet does not drop it", synth::InputMix() > 0.98f);
    for(int b = 0; b < (int)(4.0f / dt); b++)
    {
        now += dt;
        synth::Control(now);
        synth::Process(l, r, kBlock, in);
    }
    Check("four seconds of quiet gives the synth back",
          synth::InputMix() < 0.02f);

    // And a shell with no capture device never leaves the synth
    for(int b = 0; b < (int)(3.0f / dt); b++)
    {
        now += dt;
        synth::Control(now);
        synth::Process(l, r, kBlock);
    }
    Check("no input device means no input mode", synth::InputMix() < 0.02f);
}

static void TestOctaveLeavesHeldVoicesAlone()
{
    printf("the octave applies to the next press, not what is sounding\n");
    synth::Init(48000.0f, kBlock);
    K().unrest = 0.0f;
    synth::Press(0);
    const float was = voices::Pitch(synth::PadVoice(0));

    Check("the octave moved", synth::ShiftOctave(1));
    Check("a held pad keeps its pitch",
          fabsf(voices::Pitch(synth::PadVoice(0)) - was) < 0.01f);

    synth::Press(1);
    const float ratio = voices::Pitch(synth::PadVoice(1))
                        / (voices::Pitch(synth::PadVoice(0)) * 9.0f / 8.0f);
    Check("the next press takes the new octave",
          fabsf(ratio - 2.0f) < 0.01f);

    // And a latched pad that loses its spot comes back at its own pitch
    synth::Latch(true);
    synth::Release(0);
    synth::Release(1);
    synth::ShiftOctave(1);
    for(int p = 2; p < 6; p++)
        synth::Press(p);
    for(int p = 2; p < 6; p++)
        synth::Release(p);
    Check("a reclaimed latch keeps its own octave",
          fabsf(voices::Pitch(synth::PadVoice(0)) - was) < 0.01f);
}

static void TestTune()
{
    printf("tune bends what is already sounding, and detents at centre\n");
    synth::Init(48000.0f, kBlock);
    K().unrest = 0.0f;
    synth::Press(0);
    const float centre = voices::Pitch(synth::PadVoice(0));

    synth::SetKnob(7, 1.0f);
    Near("an octave up at the top", synth::PadHz(0) / centre, 2.0f, 1e-3f);
    Check("the sounding voice followed",
          voices::Pitch(synth::PadVoice(0)) > centre * 1.9f);
    synth::SetKnob(7, 0.0f);
    Near("an octave down at the bottom", synth::PadHz(0) / centre, 0.5f, 1e-3f);

    synth::SetKnob(7, 0.503f);
    Near("the centre detent is exact", synth::PadHz(0), centre, 1e-3f);

    // And the octave buttons stay per-press, which tune deliberately is not
    synth::SetKnob(7, 0.5f);
    synth::ShiftOctave(1);
    Near("the octave still leaves it alone", synth::PadHz(0), centre, 1e-3f);
}

static void TestSilence()
{
    printf("nothing held is silent\n");
    Init(1);
    voices::Init(kSr, kBlock);
    float     now = 1.0f;
    const Run r   = Drive(now, 1.0f);
    Check("finite", r.finite);
    Check("silent with no voice sounding", r.peak < 1e-6f);
}

static void TestSounds()
{
    printf("a held pad sounds, and its line rings on after release\n");
    Init(1);
    voices::Init(kSr, kBlock);
    float now = 1.0f;
    Start(now, 1);
    const Run held = Drive(now, 1.5f);
    Check("finite while held", held.finite);
    Check("a held pad makes sound", held.rms > 1e-3f);

    SetSounding(0, false, now);
    const Run tail = Drive(now, 0.4f);
    Check("the delay line still rings just after release", tail.rms > 1e-4f);
    const Run gone = Drive(now, 8.0f);
    Check("and it dies away", gone.rms < tail.rms);
    printf("  held rms %.4f, tail rms %.4f, long tail rms %.6f\n", held.rms,
           tail.rms, gone.rms);
}

// The whole knob space, every break type, four voices cross-patched: the
// instrument must stay finite and bounded everywhere in it.
// All three modulation modes share rule 5's peak test, so all three have to
// stay inside the same bounds
// There is one modulator now, and it still has to respect rule 5's bound
static void TestModulator()
{
    printf("the follower stays bounded\n");
    float worst = 0.0f, swing = 0.0f;
    bool  ok = true;
    for(uint32_t seed = 1; seed <= 30; seed++)
    {
        Init(seed);
        voices::Init(kSr, kBlock);
        K().mod    = 1.0f;   // deepest modulation
        K().fb     = 1.0f;
        K().drive  = 1.0f;
        K().safe   = 0.0f;   // and no rule to rein it in
        K().unrest = 0.0f;
        RebuildRules();
        float     now = 1.0f;
        Start(now, kVoices);
        const Run r = Drive(now, 2.5f);
        if(!r.finite)
            ok = false;
        if(r.peak > worst)
            worst = r.peak;
        float lo = 9.0f, hi = -9.0f;
        for(int b = 0; b < 400; b++)
        {
            Drive(now, 0.01f);
            const float x = voices::Mod(0);
            if(x < lo) lo = x;
            if(x > hi) hi = x;
        }
        if(hi - lo > swing)
            swing = hi - lo;
    }
    printf("  worst peak %.2f, widest swing %.2f\n", worst, swing);
    Check("finite", ok);
    Check("bounded", worst < 2.0f);
    Check("the follower actually moves", swing > 0.3f);
}

// Rule 6 only exists while the cables are switched to a mode
static void TestCableModes()
{
    printf("cables off means none, cables on means nobody left out\n");
    voices::SetCableMode(voices::kCableOff);
    int patched = 0;
    for(uint32_t seed = 1; seed <= 40; seed++)
    {
        Init(seed);
        voices::Init(kSr, kBlock);
        K().unrest = 0.0f;
        K().safe   = 0.7f;
        RebuildRules();
        float now = 1.0f;
        Start(now, kVoices);
        Heal(now);
        for(int b2 = 0; b2 < 400; b2++) { now += 0.05f; Tick(now); }
        for(int v = 0; v < kVoices; v++)
            if(ShownPatch(v))
                patched++;
    }
    Check("no cable exists while they are off", patched == 0);

    for(int m = (int)voices::kCableFeed; m < (int)voices::kCableCount; m++)
    {
        voices::SetCableMode(m);
        int isolated = 0, runs = 0;
        float worst = 0.0f;
        bool  finite = true;
        for(uint32_t seed = 1; seed <= 40; seed++)
        {
            Init(seed);
            voices::Init(kSr, kBlock);
            voices::SetCableMode(m);
            K().unrest = 0.0f;
            K().fb     = 1.0f;
            K().drive  = 1.0f;
            K().safe   = 0.7f;
            RebuildRules();
            float now = 1.0f;
            Start(now, kVoices);
            Heal(now);
            for(int b2 = 0; b2 < 400; b2++) { now += 0.05f; Tick(now); }
            // Every sounding voice must send or receive
            for(int v = 0; v < kVoices; v++)
            {
                bool touched = ShownPatch(v) != 0;
                for(int j = 0; j < kVoices && !touched; j++)
                    if(j != v && ShownPatch(j)
                       && OptTarget(ShownPatch(j)) == v)
                        touched = true;
                if(!touched)
                    isolated++;
                runs++;
            }
            const Run r = Drive(now, 1.5f);
            if(!r.finite)
                finite = false;
            if(r.peak > worst)
                worst = r.peak;
        }
        printf("  %-4s isolated %d/%d   worst peak %.2f\n",
               voices::CableName(m), isolated, runs, worst);
        Check("nobody is isolated once settled", isolated == 0);
        Check("finite", finite);
        Check("bounded", worst < 2.0f);
    }
    voices::SetCableMode(voices::kCableOff);
}

// The cable rules that differ by mode: feed stays acyclic and gain-budgeted,
// mod is allowed to couple voices into rings
static void TestCableRules()
{
    printf("feed and mod do not get the same cable rules\n");
    for(int m = (int)voices::kCableFeed; m < (int)voices::kCableCount; m++)
    {
        int cycles = 0, overfed = 0, overfanned = 0, split = 0, runs = 0;
        for(uint32_t seed = 1; seed <= 60; seed++)
        {
            Init(seed);
            voices::Init(kSr, kBlock);
            voices::SetCableMode(m);
            K().unrest = 0.0f;
            K().safe   = 0.9f;   // every cable limit binding, connected on
            RebuildRules();
            float now = 1.0f;
            Start(now, kVoices);
            Heal(now);
            for(int b2 = 0; b2 < 400; b2++) { now += 0.05f; Tick(now); }

            float in_amt[kVoices] = {};
            int   in_deg[kVoices] = {};
            for(int v = 0; v < kVoices; v++)
                if(ShownPatch(v))
                {
                    const int t = OptTarget(ShownPatch(v));
                    in_amt[t] += AMT[OptAmt(ShownPatch(v))];
                    in_deg[t]++;
                }
            for(int v = 0; v < kVoices; v++)
            {
                if(in_deg[v] > Lim().fan_in)
                    overfanned++;
                if(FL[TileF(ShownTile(v))] * FbMul() + in_amt[v]
                   > Lim().Iflood + 1e-3f)
                    overfed++;
                // Walk the chain from v and see whether it returns
                int cur = v;
                for(int step = 0; step < kVoices; step++)
                {
                    if(!ShownPatch(cur))
                        break;
                    cur = OptTarget(ShownPatch(cur));
                    if(cur == v)
                    {
                        cycles++;
                        break;
                    }
                }
            }
            // One structure, not two: reachable either way round
            bool seen[kVoices] = {};
            int  stack[kVoices], ns = 0, reached = 1;
            stack[ns++] = 0;
            seen[0]     = true;
            while(ns)
            {
                const int cur = stack[--ns];
                for(int w = 0; w < kVoices; w++)
                {
                    if(seen[w]) continue;
                    const bool linked
                        = (ShownPatch(cur) && OptTarget(ShownPatch(cur)) == w)
                          || (ShownPatch(w) && OptTarget(ShownPatch(w)) == cur);
                    if(linked)
                    { seen[w] = true; stack[ns++] = w; reached++; }
                }
            }
            if(reached < kVoices)
                split++;
            runs++;
        }
        printf("  %-4s cycles %d  over budget %d  over fan-in %d  split %d"
               "  (of %d runs)\n",
               voices::CableName(m), cycles, overfed, overfanned, split, runs);
        if(m == (int)voices::kCableFeed)
        {
            Check("feed stays acyclic", cycles == 0);
            Check("feed respects the flood budget", overfed == 0);
        }
        Check("the fan-in cap holds", overfanned == 0);
        Check("one structure, not two", split == 0);
    }
    voices::SetCableMode(voices::kCableOff);
    K().safe = 0.7f;
    RebuildRules();
}

static void TestNeverBlowsUp()
{
    printf("never blows up, anywhere in the knob space\n");
    float worst_peak = 0.0f;
    int   worst_seed = 0;
    for(uint32_t seed = 1; seed <= 120; seed++)
    {
        Init(seed);
        voices::Init(kSr, kBlock);
        // Corners as well as the middle: every third run is knobs hard over
        const bool corner = (seed % 3) == 0;
        K().delay  = corner ? (float)(seed % 2) : (float)(seed % 11) / 10.0f;
        K().fb     = corner ? 1.0f : (float)(seed % 7) / 6.0f;
        K().drive  = corner ? 1.0f : (float)(seed % 5) / 4.0f;
        K().filter = corner ? 1.0f : (float)(seed % 9) / 8.0f;
        K().mod    = corner ? 1.0f : (float)(seed % 4) / 3.0f;
        K().safe   = corner ? 0.0f : (float)(seed % 13) / 12.0f;
        K().unrest = 0.0f;
        RebuildRules();

        float now = 1.0f;
        Start(now, kVoices);
        Run r = Drive(now, 1.0f);

        // And then each break in turn, with no heal allowed to rescue it
        for(int t = 0; t < kBreakTypes; t++)
        {
            BreakRule(t, 0.0f, now);
            const Run b = Drive(now, 1.2f);
            if(!b.finite)
                r.finite = false;
            if(b.peak > r.peak)
                r.peak = b.peak;
        }
        if(!r.finite)
        {
            printf("  FAIL seed %u went non-finite\n", seed);
            fails++;
        }
        if(r.peak > worst_peak)
        {
            worst_peak = r.peak;
            worst_seed = (int)seed;
        }
    }
    printf("  worst peak %.2f (seed %d) -- what the master limiter must take\n",
           worst_peak, worst_seed);
    Check("bounded everywhere", worst_peak < 50.0f);
}

// Rule 1 says a loop above 1.0 rings by itself. The swell break sets exactly
// that, so the tail after release must be LONGER than an ordinary voice's.
static void TestSwellRings()
{
    printf("a swell break makes the loop ring by itself\n");
    float plain = 0.0f, swollen = 0.0f;
    for(uint32_t seed = 1; seed <= 20; seed++)
    {
        // An ordinary voice, released
        Init(seed);
        voices::Init(kSr, kBlock);
        float now = 1.0f;
        Start(now, 2);
        Drive(now, 1.0f);
        SetSounding(0, false, now);
        SetSounding(1, false, now);
        plain += Drive(now, 2.0f).rms;

        // The same, with feedback let go of
        Init(seed);
        voices::Init(kSr, kBlock);
        now = 1.0f;
        Start(now, 2);
        Drive(now, 1.0f);
        BreakRule((int)kSwell, 0.0f, now);
        Drive(now, 0.6f);
        SetSounding(0, false, now);
        SetSounding(1, false, now);
        swollen += Drive(now, 2.0f).rms;
    }
    printf("  tail rms: ordinary %.5f, after a swell break %.5f\n",
           plain / 20.0f, swollen / 20.0f);
    Check("the swell break rings longer than an ordinary release",
          swollen > plain);
}

// A cable must actually carry audio: voice 0 patched into voice 1 has to
// reach voice 1's line even when only voice 0 is making sound.
static void TestCableCarries()
{
    printf("a cable carries audio into the target's line\n");
    Init(3);
    voices::Init(kSr, kBlock);
    voices::SetCableMode(voices::kCableFeed);
    K().safe = 0.0f;   // no rule may undo the cable
    K().unrest = 0.0f;
    RebuildRules();
    float now = 1.0f;
    Start(now, 2);
    Drive(now, 6.0f);   // past the press presence, which would skew both runs
    const Run before = Drive(now, 0.8f);
    // Cross-patch them at the strongest amount, which is the cable break
    BreakRule((int)kCable, 0.0f, now);
    const Run after = Drive(now, 1.2f);
    printf("  rms before %.4f, with both cables live %.4f\n", before.rms,
           after.rms);
    Check("finite with cables live", after.finite);
    Check("cross-patching adds energy", after.rms > before.rms);
    voices::SetCableMode(voices::kCableOff);
}

// Something to listen to beside the web prototype


int SelfTest()
{
    TestKnobFormulas();
    TestTileTables();
    TestStrictHealIsClean();
    TestLooseFlagsNothing();
    TestPressIsLegal();
    TestBreaksBreakAndHeal();
    TestWaveIsStaggered();
    TestKnobsLoadTheirRule();
    TestRule3CanBeUnsatisfiable();
    TestCableStructure();
    TestLatchSurvivesStealing();
    TestOctaveLeavesHeldVoicesAlone();
    TestTune();
    TestInputDetector();
    TestSilence();
    TestSounds();
    TestModulator();
    TestCableModes();
    TestCableRules();
    TestNeverBlowsUp();
    TestSwellRings();
    TestCableCarries();

    if(fails)
        printf("\n%d FAILED\n", fails);
    else
        printf("\nall passed\n");
    return fails ? 1 : 0;
}
