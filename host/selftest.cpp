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
        return kGroupSize;
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

static void TestAgitationFader()
{
    printf("one fader drives strictness and unrest, and reaches the rules\n");
    synth::Init(48000.0f, 48);
    // Through synth::SetAgitation, not wfc's: an index that never triggers
    // RebuildRules leaves the fader inert on the panel while every test that
    // calls RebuildRules by hand still passes
    synth::SetAgitation(0.0f);
    Check("loose at the bottom", Lim().Fself > 8.0f);
    Check("and still", UnrestRate() == 0.0f && LapseChance() == 0.0f);
    synth::SetAgitation(0.3f);
    Check("the bottom third stays still", UnrestRate() == 0.0f);
    Check("while strictness has already moved", K().safe == 0.3f);
    synth::SetAgitation(0.95f);
    Check("strict at the top", Lim().Fself < 1.0f);
    Check("and restless", UnrestRate() > 0.1f && LapseChance() > 0.2f);
}

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
    Near("modMul", ModMul(), 2.0f);
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
        for(int v = 0; v < kGroupSize; v++)
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
        for(int v = 0; v < kGroupSize; v++)
        {
            SetSounding(v, true, now);
            now += 0.01f;
        }
        for(int i = 0; i < kGroupSize; i++)
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
    for(int g = 0; g < kGroups; g++)
    {
        for(uint32_t seed = 1; seed <= 60; seed++)
        {
            const int base = g * kGroupSize;
            Init(seed);
            K().safe = 0.95f;
            K().unrest = 0.0f;
            RebuildRules();
            float now = 1.0f;
            for(int v = base; v < base + kGroupSize; v++)
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
}

// Each break must write exactly the violation 4.6 describes, and the
// scheduled heal must take it away again.
static void TestBreaksBreakAndHeal()
{
    printf("each break breaks its own rule, and heal takes it back\n");
    const BreakType types[kBreakTypes]
        = {kEcho, kSwell, kFuzz, kFlood, kCable};
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
            K().mod = 1.0f;
            RebuildRules();
            float now = 1.0f;
            for(int v = 0; v < kGroupSize; v++)
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
    for(int v = 0; v < kGroupSize; v++)
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
    for(int v = 0; v < kGroupSize; v++)
        landed_at[v] = -1;
    const float hop = HopSeconds();
    for(int step = 0; step < 12; step++)
    {
        Tick(now + (float)step * hop * 0.5f);
        for(int v = 0; v < kGroupSize; v++)
            if(landed_at[v] < 0 && ShownTile(v) == Tile(v)
               && ShownMod(v) == Mod(v))
                landed_at[v] = step;
    }
    int first = 99, last = -1;
    for(int v = 0; v < kGroupSize; v++)
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
    for(int nv = 2; nv <= kGroupSize; nv++)
    {
        int hit[3] = {0, 0, 0};   // drive, feedback, delay
        int unsat[3] = {0, 0, 0};
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
            } probe[] = {
                {&K().drive, 1.0f, 0.3f, kFuzz},
                {&K().fb, 1.0f, 0.5f, kSwell},
                {&K().delay, 0.0f, 0.6f, kEcho},
            };
            for(int p = 0; p < (int)(sizeof(probe) / sizeof(probe[0]));
                p++)
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
        printf("  %d voices: drive %3d/%d  feedback %3d/%d  delay %3d/%d",
               nv, hit[0], kRuns, hit[1], kRuns, hit[2], kRuns);
        if(unsat[0] + unsat[1] + unsat[2])
            printf("   (unrepairable: drive %d fb %d delay %d)",
                   unsat[0], unsat[1], unsat[2]);
        printf("\n");
        for(int p = 0; p < (int)(sizeof(hit) / sizeof(hit[0])); p++)
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
    for(int g = 0; g < kGroups; g++)
    {
        for(uint32_t seed = 1; seed <= 60; seed++)
        {
            const int base = g * kGroupSize;
            Init(seed);
            K().safe = 0.95f;
            K().unrest = 0.0f;
            RebuildRules();
            float now = 1.0f;
            for(int v = base; v < base + kGroupSize; v++)
            {
                SetSounding(v, true, now);
                now += 0.01f;
                Pass(v, now);
            }
            Settle(now);
            for(int v = base; v < base + kGroupSize; v++)
                Check("no voice patches into itself",
                      OptTarget(ShownPatch(v)) != v);
            Check("no cable loop when strict", Broken(kCable) == 0);
        }
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


// Keeps the last block so FiniteOutput can inspect it
static float g_l[kBlock], g_r[kBlock];

static void AdvanceSeconds(float& now, float secs)
{
    const int blocks = (int)(secs * kSr / (float)kBlock);
    for(int b = 0; b < blocks; b++)
    {
        now += (float)kBlock / kSr;
        synth::Control(now);
        synth::Process(g_l, g_r, kBlock);
    }
}

static bool FiniteOutput()
{
    for(int s = 0; s < kBlock; s++)
        if(!(fabsf(g_l[s]) <= 4.0f) || !(fabsf(g_r[s]) <= 4.0f))
            return false;
    return true;
}

static bool ShownStateValid()
{
    for(int v = 0; v < kVoices; v++)
        if(ShownTile(v) < 0 || ShownTile(v) >= kTiles || ShownPatch(v) < 0
           || ShownPatch(v) >= kOpts || ShownMod(v) < 0
           || ShownMod(v) >= kModOpts)
            return false;
    return true;
}

static void Start(float& now, int n_voices)
{
    for(int i = 0; i < n_voices; i++)
    {
        SetSounding(i, true, now);
        // The prototype's default: major, root A, so pad 1 is 220 Hz
        static const float hz[6] = {220.0f, 247.5f, 275.0f, 293.33f, 330.0f, 366.67f};
        voices::SetPitch(i, hz[i], true);
        Pass(i, now);
        now += 0.02f;
    }
}

// ------------------------------------------------------------------ tests

static void TestPadsAndGroups()
{
    printf("six voices, one per pad, in two groups\n");
    float now = 0.0f;
    synth::Init(48000.0f, 48);
    Check("P03 owns voice 0", synth::PadVoice(0) == 0);
    Check("P04 owns voice 1", synth::PadVoice(1) == 1);
    Check("P08 owns voice 2", synth::PadVoice(5) == 2);
    Check("P06 owns voice 3", synth::PadVoice(3) == 3);
    Check("P07 owns voice 4", synth::PadVoice(4) == 4);
    Check("P09 owns voice 5", synth::PadVoice(6) == 5);
    Check("P05 owns no voice", synth::PadVoice(2) == -1);
    Check("left group", GroupOf(0) == 0 && GroupOf(2) == 0);
    Check("right group", GroupOf(3) == 1 && GroupOf(5) == 1);

    for(int p = 0; p < synth::kPads; p++)
        synth::Press(p);
    Check("no stealing", synth::PadVoice(0) == 0 && synth::PadVoice(6) == 5);
    AdvanceSeconds(now, 0.5f);
    Settle(now);
    Check("output finite", FiniteOutput());
    Check("shown state valid", ShownStateValid());

    // A sixth, not a fifth: a fifth would land the right root on the left
    // triad's top note and P06 would play a unison with P08
    Near("right/left root", synth::PadHz(3) / synth::PadHz(0), 5.0f / 3.0f,
         1e-3f);
    for(int a = 0; a < synth::kPads; a++)
        for(int b = a + 1; b < synth::kPads; b++)
            if(synth::PadVoice(a) >= 0 && synth::PadVoice(b) >= 0)
                Check("every playable pad has its own pitch",
                      fabsf(synth::PadHz(a) - synth::PadHz(b)) > 1.0f);
    Near("left third", synth::PadHz(1) / synth::PadHz(0), 1.2f, 1e-3f);
    Near("right third", synth::PadHz(4) / synth::PadHz(3), 1.2f, 1e-3f);
}

static void TestGroupsAreIndependent()
{
    printf("a break in one group leaves the other alone\n");
    float now = 0.0f;
    synth::Init(48000.0f, 48);
    for(int p = 0; p < synth::kPads; p++)
        if(synth::PadVoice(p) >= 0)
            synth::Press(p);
    Settle(now);
    int before[kVoices];
    for(int v = 0; v < kVoices; v++)
        before[v] = ShownTile(v);

    BreakRule(kSwell, 0, 0.0f, now);   // left group only
    Settle(now);
    bool right_moved = false, left_moved = false;
    for(int v = 0; v < kVoices; v++)
        if(ShownTile(v) != before[v])
            (GroupOf(v) ? right_moved : left_moved) = true;
    Check("a left break leaves the right group alone", !right_moved);
    Check("but it does move the left group", left_moved);
}

// One Tick can heal, lapse, re-roll for unrest and chain a break in each group
// before anything is delivered; the press burst alone is only 12 events
static void TestEventQueueHolds()
{
    printf("the event queue holds a full Tick's worth across both groups\n");
    synth::Init(48000.0f, 48);
    float now = 0.0f;
    for(int p = 0; p < synth::kPads; p++)
        if(synth::PadVoice(p) >= 0)
            synth::Press(p);
    Settle(now);
    for(int g = 0; g < kGroups; g++)
    {
        BreakRule(-1, g, 0.0f, now);
        Heal(g, now);
        BreakRule(-1, g, 0.0f, now);
        Pass(g * kGroupSize, now);
    }
    Check("no event dropped", EventsDropped() == 0);
}

static void TestLoneVoiceInGroup()
{
    printf("a group of one still settles\n");
    float now = 0.0f;
    synth::Init(48000.0f, 48);
    synth::SetKnob(5, 1.0f);   // Control: strictest
    synth::SetCableMode(voices::kCableFeed);
    synth::Press(0);
    Settle(now);
    Violation vio[kVoices * kVoices + kVoices];
    Check("a lone voice is legal", Diagnose(vio, (int)(sizeof(vio) / sizeof(vio[0]))) == 0);
    synth::SetCableMode(voices::kCableOff);
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
                        / (voices::Pitch(synth::PadVoice(0)) * 6.0f / 5.0f);
    Check("the next press takes the new octave",
          fabsf(ratio - 2.0f) < 0.01f);
}

static void TestHold()
{
    printf("hold floors the selected group's level and a press rides on top\n");
    float now = 0.0f;
    auto run  = [&](int target, float hold) {
        synth::Init(48000.0f, kBlock);
        synth::SetHoldTarget(target);
        synth::SetKnob(7, hold);
        synth::Press(0);
        synth::Press(3);
        AdvanceSeconds(now, 1.0f);
        synth::Release(0);
        synth::Release(3);
        AdvanceSeconds(now, 20.0f);
    };

    run(1, 1.0f);
    Check("left voice sits at the floor after release", voices::Amp(0) > 0.9f);
    Check("right voice has released", voices::Amp(3) < 0.01f);
    synth::SetKnob(7, 0.0f);
    AdvanceSeconds(now, 20.0f);
    Check("turning hold down releases it", voices::Amp(0) < 0.01f);

    run(-1, 1.0f);
    Check("down aims at the right group",
          voices::Amp(3) > 0.9f && voices::Amp(0) < 0.01f);

    run(0, 1.0f);
    Check("centre holds both", voices::Amp(0) > 0.9f && voices::Amp(3) > 0.9f);

    run(1, 0.5f);
    Near("the floor is the knob's level", voices::Amp(0), 0.5f, 0.01f);
    Check("the unselected group still falls to silence", voices::Amp(3) < 0.01f);

    // The floor applies to a voice whose pad was never touched: that is what
    // makes HOLD a drone rather than a long tail
    synth::Init(48000.0f, kBlock);
    synth::SetHoldTarget(1);
    synth::SetKnob(7, 0.4f);
    AdvanceSeconds(now, 5.0f);
    Near("an untouched voice rises to the floor", voices::Amp(1), 0.4f, 0.01f);
    Check("an untouched voice outside the group stays silent",
          voices::Amp(4) < 0.01f);

    // Both envelopes in parallel: the press goes to full and falls back to the
    // floor, not to zero
    synth::Press(0);
    AdvanceSeconds(now, 0.3f);
    Check("a press rides above the floor", voices::Amp(0) > 0.9f);
    synth::Release(0);
    AdvanceSeconds(now, 20.0f);
    Near("and settles back onto it", voices::Amp(0), 0.4f, 0.01f);
}

static void TestHoldIsSounding()
{
    printf("a HOLD-floored voice stays in the solver\n");
    float now = 0.0f;
    synth::Init(48000.0f, 48);
    synth::SetHoldTarget(1);
    synth::SetKnob(7, 1.0f);
    synth::Press(0);
    synth::Press(3);
    AdvanceSeconds(now, 1.0f);
    synth::Release(0);
    synth::Release(3);
    Check("right after release only the selected group still sounds",
          wfc::Sounding(0) && !wfc::Sounding(3));
    AdvanceSeconds(now, 10.0f);
    Check("floored voice is sounding", wfc::Sounding(0));
    Check("unselected group is not", !wfc::Sounding(3));
    Check("a never-pressed voice in the group is too",
          wfc::Sounding(1) && wfc::Sounding(2));

    synth::Press(synth::kConnectorPad);
    synth::Press(4);
    AdvanceSeconds(now, 0.5f);
    Check("a sustained voice is a bridge endpoint",
          voices::CableCmd(0, 4) > 0.0f || voices::CableCmd(4, 0) > 0.0f);
    synth::Release(synth::kConnectorPad);
    synth::Release(4);

    synth::SetKnob(7, 0.0f);
    AdvanceSeconds(now, 1.0f);
    Check("hold down ends sounding", !wfc::Sounding(0));

    synth::Init(48000.0f, 48);
    synth::SetKnob(7, 1.0f);
    for(int p = 0; p < synth::kPads; p++)
        if(synth::PadVoice(p) >= 0)
            synth::Press(p);
    AdvanceSeconds(now, 2.0f);
    for(int p = 0; p < synth::kPads; p++)
        if(synth::PadVoice(p) >= 0)
            synth::Release(p);
    AdvanceSeconds(now, 5.0f);
    int n = 0;
    for(int v = 0; v < kVoices; v++)
        n += wfc::Sounding(v);
    Check("all six sustain", n == 6);
    for(int g = 0; g < kGroups; g++)
    {
        BreakRule(-1, g, 0.0f, now);
        Heal(g, now);
        BreakRule(-1, g, 0.0f, now);
        Pass(g * kGroupSize, now);
    }
    Check("no event dropped with six sustaining", EventsDropped() == 0);
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
            // One structure per group, reachable either way round
            for(int g = 0; g < kGroups; g++)
            {
                bool seen[kVoices] = {};
                int  stack[kVoices], ns = 0, reached = 1;
                const int first = g * kGroupSize;
                stack[ns++]  = first;
                seen[first]  = true;
                while(ns)
                {
                    const int cur = stack[--ns];
                    for(int w = first; w < first + kGroupSize; w++)
                    {
                        if(seen[w]) continue;
                        const bool linked
                            = (ShownPatch(cur)
                               && OptTarget(ShownPatch(cur)) == w)
                              || (ShownPatch(w)
                                  && OptTarget(ShownPatch(w)) == cur);
                        if(linked)
                        { seen[w] = true; stack[ns++] = w; reached++; }
                    }
                }
                if(reached < kGroupSize)
                    split++;
            }
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
static void TestBreakIsAGesture()
{
    printf("a break snaps the gain and crawls the delay\n");
    double early = 0, late = 0;
    int    n = 0, arrived = 0, moved = 0;
    for(uint32_t seed = 3; seed <= 30; seed += 3)
    {
        float now = 0.0f;
        synth::Init(48000.0f, 48);
        wfc::Init(seed);
        for(int p = 0; p < synth::kPads; p++)
            if(synth::PadVoice(p) >= 0)
                synth::Press(p);
        AdvanceSeconds(now, 3.0f);
        Settle(now);

        float d0[kVoices], f0[kVoices];
        for(int v = 0; v < kVoices; v++)
        {
            d0[v] = voices::EffDly(v);
            f0[v] = voices::EffFb(v);
        }
        // Frozen so no repair chases the break: this measures how the break
        // itself travels, not where the instrument ends up
        SetFrozen(true);
        BreakRule(-1, 0.0f, now);
        // A snap is a discontinuity, so look for it as one: the largest
        // single-block step against the whole distance travelled. A glide
        // with a ~0.05 s constant can never move more than a few percent in
        // one millisecond.
        float step[kVoices] = {}, walked[kVoices] = {}, prev[kVoices];
        for(int v = 0; v < kVoices; v++)
            prev[v] = f0[v];
        float dmid[kVoices];
        for(int k = 0; k < 250; k++)
        {
            AdvanceSeconds(now, 0.001f);
            for(int v = 0; v < kVoices; v++)
            {
                const float d = fabsf(voices::EffFb(v) - prev[v]);
                walked[v] += d;
                if(d > step[v])
                    step[v] = d;
                prev[v] = voices::EffFb(v);
            }
            if(k == 249)
                for(int v = 0; v < kVoices; v++)
                    dmid[v] = voices::EffDly(v);
        }
        AdvanceSeconds(now, 6.0f);
        SetFrozen(false);

        // The wave staggers the break across the voices, so only the
        // first has landed at 0.02 s. Count the ones ALREADY at their broken
        // value then, rather than averaging over the ones still to come.
        for(int v = 0; v < kVoices; v++)
        {
            if(walked[v] <= 1e-3f)
                continue;
            moved++;
            arrived += step[v] > 0.5f * walked[v];
        }
        int   pick = -1;
        float most = 0.0f;
        for(int v = 0; v < kVoices; v++)
            if(fabsf(voices::EffDly(v) - d0[v]) > most)
            {
                most = fabsf(voices::EffDly(v) - d0[v]);
                pick = v;
            }
        if(pick < 0 || most < 1e-4f)
            continue;
        early += fabsf(dmid[pick] - d0[pick]);
        late += most;
        n++;
    }
    printf("  %d breaks: delay %.0f%% travelled at 0.25 s, %d of %d "
           "gains arrive in one step\n",
           n, 100.0 * early / (late > 0 ? late : 1), arrived, moved);
    // Measured: 18% with the crawl, 44% without it
    Check("the delay is still crawling a quarter second in",
          n > 3 && early < 0.3 * late);
    // The first voice of each wave is at its broken gain immediately;
    // without the snap none are, because the glide needs ~0.05 s
    Check("the gain arrives in one step", arrived >= n);
}

static void TestReleaseRerollsTheTail()
{
    printf("letting go rolls the voice a new tile for its tail\n");
    int rolled = 0, audible = 0;
    const int kRuns = 20;
    for(uint32_t seed = 1; seed <= (uint32_t)kRuns; seed++)
    {
        float now = 0.0f;
        synth::Init(48000.0f, 48);
        wfc::Init(seed);
        synth::Press(0);
        AdvanceSeconds(now, 2.0f);
        Settle(now);
        const int before = ShownTile(0);
        synth::Release(0);
        AdvanceSeconds(now, 0.2f);
        rolled += ShownTile(0) != before;
        // The tail has to still be sounding, or a new tile colours nothing
        audible += voices::Amp(0) > 0.05f;
    }
    printf("  %d of %d rolled, %d still ringing\n", rolled, kRuns, audible);
    // 100 tiles, so a repeat is possible but rare
    Check("the released voice gets a new tile", rolled >= kRuns - 2);
    Check("and is still ringing to hear it", audible >= kRuns - 2);
}

static double WindowRms(float& now, float secs)
{
    double q = 0; int n = 0;
    for(int i = 0; i < (int)(secs / 0.01f); i++)
    {
        AdvanceSeconds(now, 0.01f);
        for(int k = 0; k < kBlock; k++) { q += g_l[k] * g_l[k]; n++; }
    }
    return sqrt(q / n);
}

// Mid/high band (one-pole HP at 500 Hz) on the SUM of both channels. Plain
// RMS is dominated by low-frequency energy and called the low pads the
// loudest, which is the opposite of what they sound like.
static void BandRms(float& now, float secs, double& hi, double& full)
{
    float       hp = 0.0f;
    const float k  = 1.0f - expf(-2.0f * 3.14159265f * 500.0f / kSr);
    double      qh = 0, qf = 0;
    int         n  = 0;
    for(int i = 0; i < (int)(secs / 0.01f); i++)
    {
        AdvanceSeconds(now, 0.01f);
        for(int s = 0; s < kBlock; s++)
        {
            const float x = g_l[s] + g_r[s];
            hp += k * (x - hp);
            const float h = x - hp;
            qh += h * h;
            qf += x * x;
            n++;
        }
    }
    hi = sqrt(qh / n), full = sqrt(qf / n);
}

static void TestLowPadsCarryTheBand()
{
    printf("the bottom pad puts enough energy where it can be heard\n");
    // P03 and P09, the extremes of the tuning
    static const int kPad[2] = {0, 6};
    double           frac[2] = {0, 0};
    for(int j = 0; j < 2; j++)
    {
        const int kSeeds = 8;
        double    hi = 0, full = 0;
        for(int seed = 1; seed <= kSeeds; seed++)
        {
            float now = 0.0f;
            synth::Init(48000.0f, 48);
            wfc::Init(seed);
            synth::Press(kPad[j]);
            AdvanceSeconds(now, 0.1f);
            double h = 0, f = 0;
            BandRms(now, 0.3f, h, f);
            hi += h, full += f;
        }
        frac[j] = hi / full;
        printf("  pad %d %5.0f Hz: %.0f%% of its energy above 500 Hz\n",
               kPad[j], synth::PadHz(kPad[j]), frac[j] * 100.0);
    }
    // 0.58 measured; 0.42 with the old 0.55/1.25 tilt, which is what the low
    // pads being inaudible sounded like
    Check("the bottom pad is not buried below 500 Hz", frac[0] > 0.52);
}

static void TestNewVoiceGetsRoom()
{
    printf("a new pad steps the other voices back at the bus\n");
    float now = 0.0f;
    synth::Init(48000.0f, 48);
    wfc::Init(5);
    // Three voices with tails, settled, so the wash is what a new pad has
    // to cut through
    synth::Press(0);
    synth::Press(1);
    synth::Press(5);
    AdvanceSeconds(now, 4.0f);
    Settle(now);
    const float rest = voices::OutGain(0);
    Check("at rest every voice sends its whole output",
          fabsf(rest - 1.0f) < 0.02f);
    const float quiet = WindowRms(now, 0.3f);

    synth::Press(3);
    AdvanceSeconds(now, 0.3f);
    const float others = voices::OutGain(0);
    const float fresh  = voices::DryLift(3);
    const double after = WindowRms(now, 0.3f);
    printf("  others %.2f, new %.2f, wash %.4f -> %.4f\n", others, fresh,
           quiet, after);
    // Measured 0.65 and 2.4 at 0.3 s; with neither mechanism both are 1.00
    Check("the standing voices step back", others < 0.8f);
    Check("and the new one's dry steps up", fresh > 1.5f);

    // Both gains have to reach the AUDIO, not merely be computed, so the
    // ratio is bracketed from both sides. Measured 1.02 with both wired;
    // 0.88 with the dry lift cut out, 1.17 with the duck cut out.
    const double ratio = after / quiet;
    printf("  wash ratio %.2f\n", ratio);
    Check("the bus actually ducks", ratio < 1.10);
    Check("and the new voice's dry actually lifts it", ratio > 0.96);

    AdvanceSeconds(now, 6.0f);
    Check("and it all comes back", voices::OutGain(0) > 0.95f);
}

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

// Mean over the window of the summed shown load, per group
static void MeanLoad(float& now, float secs, double* out)
{
    out[0] = out[1] = 0;
    const int n = (int)(secs / 0.05f);
    for(int i = 0; i < n; i++)
    {
        AdvanceSeconds(now, 0.05f);
        for(int v = 0; v < kVoices; v++)
            if(Sounding(v))
                out[v / kGroupSize] += FL[TileF(ShownTile(v))] / n;
    }
}

static void TestJoinedDucksAndBlooms()
{
    printf("P05 ducks the instrument while held and blooms back on release\n");
    // Summed over seeds: one seed's start state can leave little to duck
    const float unrests[2] = {0.0f, 0.3f};
    double      tot[2][3] = {};
    double      worst = 0;
    int         no_duck = 0;
    for(int u = 0; u < 2; u++)
        for(uint32_t seed = 11; seed <= 88; seed += 11)
        {
            float now = 0.0f;
            synth::Init(48000.0f, 48);
            wfc::Init(seed);
            synth::SetKnob(6, unrests[u]);
            for(int p = 0; p < synth::kPads; p++)
                if(synth::PadVoice(p) >= 0)
                    synth::Press(p);
            AdvanceSeconds(now, 3.0f);
            double up[2], down[2], after[2];
            MeanLoad(now, 20.0f, up);
            synth::Press(synth::kConnectorPad);
            AdvanceSeconds(now, 5.0f);
            MeanLoad(now, 20.0f, down);
            synth::Release(synth::kConnectorPad);
            AdvanceSeconds(now, 20.0f);
            MeanLoad(now, 20.0f, after);
            tot[u][0] += up[0] + up[1];
            tot[u][1] += down[0] + down[1];
            tot[u][2] += after[0] + after[1];
            const double ratio = (down[0] + down[1]) / (up[0] + up[1]);
            printf("  unrest %.1f seed %u duck %.2f\n", unrests[u], seed, ratio);
            if(ratio > worst)
                worst = ratio;
            no_duck += ratio >= 0.9;
        }
    for(int u = 0; u < 2; u++)
        printf("  unrest %.1f  up %.2f  down %.2f (%.2f)  after %.2f (%.2f)\n",
               unrests[u], tot[u][0], tot[u][1], tot[u][1] / tot[u][0],
               tot[u][2], tot[u][2] / tot[u][0]);
    // Unrest 0: duck must exist; a re-roll is the only way back up, so no bloom
    Check("held P05 ducks with Unrest 0", tot[0][1] < 0.8 * tot[0][0]);
    // Not every seed ducks: with no rule on mod depth, a seed that lands on
    // deep mod carries enough extra load to cancel the charge. Measured 1 of
    // 16 at worst 1.08, so the duck is a tendency, not a guarantee.
    printf("  %d of 16 seeds do not duck, worst %.2f\n", no_duck, worst);
    Check("the duck is the rule, not the exception", no_duck <= 3);
    Check("held P05 ducks hard", tot[1][1] < 0.7 * tot[1][0]);
    Check("release blooms back near unjoined", tot[1][2] > 0.8 * tot[1][0]);
}

static void TestJoinedEventQueue()
{
    printf("joined, six voices and two waves drop nothing\n");
    float now = 0.0f;
    synth::Init(48000.0f, 48);
    for(int p = 0; p < synth::kPads; p++)
        synth::Press(p);
    AdvanceSeconds(now, 20.0f);
    Check("no event dropped", EventsDropped() == 0);
    Check("every shown value is a real tile", ShownStateValid());
    Check("still finite", FiniteOutput());
}

static void TestConnector()
{
    printf("P05 patches a bridge between the groups\n");
    float now = 0.0f;
    synth::Init(48000.0f, 48);
    synth::Press(0);
    synth::Press(3);
    Settle(now);
    Check("not joined until P05", !synth::Joined());
    Check("no bridge before P05", voices::CableCmd(0, 3) == 0.0f);

    synth::Press(synth::kConnectorPad);
    Check("P05 joins", synth::Joined());
    Check("a bridge each way",
          voices::CableCmd(0, 3) > 0.0f && voices::CableCmd(3, 0) > 0.0f);
    // CableEff is what the bridge drawing reads: it must arrive, and guard
    AdvanceSeconds(now, 0.5f);
    Check("bridge is drawn arriving",
          voices::CableEff(0, 3) > 0.0f && voices::CableEff(3, 0) > 0.0f);
    Check("out of range and self cables read zero",
          voices::CableEff(-1, 0) == 0.0f && voices::CableEff(0, 99) == 0.0f
              && voices::CableEff(0, 0) == 0.0f && voices::CableCmd(0, 99) == 0.0f);

    synth::Release(synth::kConnectorPad);
    Check("release parts them", !synth::Joined());
    Check("bridges gone",
          voices::CableCmd(0, 3) == 0.0f && voices::CableCmd(3, 0) == 0.0f);
}

static void TestConnectorDegenerate()
{
    float now = 0.0f;
    synth::Init(48000.0f, 48);
    synth::Press(0);
    synth::Press(synth::kConnectorPad);
    Settle(now);
    Check("one-sided join has no bridge", voices::CableCmd(0, 3) == 0.0f);

    synth::Init(48000.0f, 48);
    synth::Press(synth::kConnectorPad);
    Settle(now);
    Check("joined with silence is legal", synth::Joined());
    AdvanceSeconds(now, 1.0f);
    Check("still finite", FiniteOutput());
}

constexpr int kBridgeBlocks = 400;

static void RenderBridge(int mode, bool join, float* out)
{
    float now = 0.0f;
    synth::Init(48000.0f, 48);
    synth::SetCableMode(mode);
    synth::Press(0);
    synth::Press(3);
    if(join)
        synth::Press(synth::kConnectorPad);
    Settle(now);
    for(int b = 0; b < kBridgeBlocks; b++)
    {
        now += (float)kBlock / kSr;
        synth::Control(now);
        synth::Process(g_l, g_r, kBlock);
        memcpy(out + b * kBlock, g_l, sizeof(g_l));
    }
}

static float Delta(const float* a, const float* b)
{
    float d = 0.0f;
    for(int i = 0; i < kBridgeBlocks * kBlock; i++)
        d += fabsf(a[i] - b[i]);
    return d;
}

static void TestBridgeSurvivesCableMode()
{
    printf("the bridge is audible in every cable mode\n");
    float now = 0.0f;
    synth::Init(48000.0f, 48);
    synth::Press(0);
    synth::Press(3);
    synth::Press(synth::kConnectorPad);
    Settle(now);
    for(int m = 0; m < voices::kCableCount; m++)
    {
        synth::SetCableMode(m);
        Settle(now);
        Check("bridge commanded in every cable mode",
              voices::CableCmd(0, 3) > 0.0f);
    }
    for(int m = 0; m < voices::kCableCount; m++)
    {
        static float a[kBridgeBlocks * kBlock], b[kBridgeBlocks * kBlock],
            c[kBridgeBlocks * kBlock];
        RenderBridge(m, false, c);   // warm-up: leftover state from earlier tests
        RenderBridge(m, false, a);
        RenderBridge(m, false, b);
        RenderBridge(m, true, c);
        // The control: two identical runs must match, or the delta is noise
        Check("unjoined runs repeat exactly", Delta(a, b) == 0.0f);
        Check("bridge changes the sound in every cable mode",
              Delta(a, c) > 0.01f);
    }
    synth::SetCableMode(voices::kCableOff);
}

// Something to listen to beside the web prototype


int SelfTest()
{
    TestKnobFormulas();
    TestAgitationFader();
    TestTileTables();
    TestStrictHealIsClean();
    TestLooseFlagsNothing();
    TestPressIsLegal();
    TestBreaksBreakAndHeal();
    TestWaveIsStaggered();
    TestKnobsLoadTheirRule();
    TestRule3CanBeUnsatisfiable();
    TestCableStructure();
    TestPadsAndGroups();
    TestGroupsAreIndependent();
    TestLoneVoiceInGroup();
    TestEventQueueHolds();
    TestJoinedDucksAndBlooms();
    TestJoinedEventQueue();
    TestHold();
    TestHoldIsSounding();
    TestOctaveLeavesHeldVoicesAlone();
    TestInputDetector();
    TestSilence();
    TestSounds();
    TestModulator();
    TestCableModes();
    TestCableRules();
    TestNeverBlowsUp();
    TestLowPadsCarryTheBand();
    TestNewVoiceGetsRoom();
    TestReleaseRerollsTheTail();
    TestBreakIsAGesture();
    TestSwellRings();
    TestCableCarries();
    TestConnector();
    TestConnectorDegenerate();
    TestBridgeSurvivesCableMode();

    if(fails)
        printf("\n%d FAILED\n", fails);
    else
        printf("\nall passed\n");
    return fails ? 1 : 0;
}
