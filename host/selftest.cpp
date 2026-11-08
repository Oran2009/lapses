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
#include <chrono>

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
    // One hop per voice plus a margin, so a whole wave is drained. A fixed
    // 12 half-hops is exactly the span of a seven-voice wave, which left
    // the last voice on a float-equality coin flip.
    const float hop = HopSeconds();
    for(int i = 0; i < 2 * (kVoices + 2); i++)
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
    Violation v[kMaxViolations];
    const int n   = Diagnose(v, kMaxViolations);
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
    // Pinned, and off the Delay knob: 0.02 + 0.78 * 0.02
    Near("hopSeconds", HopSeconds(), 0.0356f, 5e-4f);
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
    Near("Fshare", Lim().Fshare, 0.9f);
    Near("Dshare", Lim().Dshare, 0.87f);
    Near("Fself", Lim().Fself, 0.95f);
    Near("Lpeak", Lim().Lpeak, 0.98f);
    Near("gap", Lim().gap, 0.0152f);
    Check("ratioOn at .7", Lim().ratio_on);

    // The thresholds the strictness knob crosses
    K().unrest = 1.0f;
    K().safe   = 0.0f;
    RebuildRules();
    Check("Fself off when loose", Lim().Fself > 8.0f);
    Check("ratioOn off when loose", !Lim().ratio_on);
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
    Violation v[kMaxViolations];
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
        Violation vio[kMaxViolations];
        const int n = Diagnose(vio, kMaxViolations);
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
        Violation vio[kMaxViolations];
        const int n = Diagnose(vio, kMaxViolations);
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
                Violation vio[kMaxViolations];
                const int n = Diagnose(vio, kMaxViolations);
                if(n)
                {
                    printf("  FAIL seed %u voice %d: %d broken after press (%s %d,%d)\n",
                           seed, v, n, BreakName(vio[0].type), vio[0].v[0], vio[0].v[1]);
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
    const BreakType types[kBreakTypes] = {kEcho, kSwell, kFuzz};
    for(int ti = 0; ti < kBreakTypes; ti++)
    {
        int hit = 0, clean_after = 0;
        for(uint32_t seed = 1; seed <= 40; seed++)
        {
            Init(seed);
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
            Violation before[kMaxViolations];
            if(Diagnose(before, kMaxViolations) != 0)
                continue;

            BreakRule((int)types[ti], 0.0f, now);
            Settle(now);
            if(seed <= 4)
            {
                printf("      PROBE %s seed %u:", BreakName(types[ti]), seed);
                for(int q = 0; q < kBreakTypes; q++)
                    printf(" %s=%d", BreakName((BreakType)q),
                           Broken((BreakType)q));
                int undeliv = 0;
                for(int v = 0; v < kVoices; v++)
                    if(Tile(v) != ShownTile(v))
                        undeliv++;
                printf(" undelivered=%d dropped=%d", undeliv,
                       EventsDropped());
                printf("\n");
            }
            if(Broken(types[ti]) > 0)
                hit++;

            // The repair WFC scheduled for itself, 3 to 7 s out, and then
            // the heal's own wave, which is a hop per sounding voice
            for(int k = 0; k < 40; k++)
            {
                now += 0.25f;
                Tick(now);
                Settle(now);
            }
            Violation vio[kMaxViolations];
            if(Diagnose(vio, kMaxViolations) == 0)
                clean_after++;
        }
        printf("  %-7s broke %2d/40, clean after heal %2d/40\n",
               BreakName(types[ti]), hit, clean_after);
        Check("break is audible as its own rule", hit >= 38);
        Check("heal clears the break", clean_after >= 38);
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
            Violation vio[kMaxViolations];
            if(Diagnose(vio, kMaxViolations))
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
static void TestRule3DistinctEchoes()
{
    printf("rule 3: how many distinct echoes fit, by Delay position\n");
    const float positions[3] = {0.0f, 0.1f, 0.6f};
    int         fits[3]      = {};
    for(int p = 0; p < 3; p++)
    {
        Init(1);
        K().delay = positions[p];
        RebuildRules();
        // The largest set of delay classes that can coexist. Voices sharing a
        // class are always legal, so this is a ceiling on how many SEPARATE
        // echoes you can hear, never a ceiling on how many voices may sound.
        int biggest = 1;
        for(int mask = 0; mask < (1 << 7); mask++)
        {
            int cls[7], n = 0;
            for(int i = 0; i < 7; i++)
                if(mask & (1 << i))
                    cls[n++] = i;
            bool ok = true;
            for(int a = 0; a < n && ok; a++)
                for(int b = a + 1; b < n && ok; b++)
                    if(fabsf(DR[cls[a]] - DR[cls[b]]) * DelayBase()
                       < Lim().gap)
                        ok = false;
            if(ok && n > biggest)
                biggest = n;
        }
        fits[p] = biggest;
        printf("  Delay %.1f (base %.3f s): %d distinct echoes fit\n",
               positions[p], DelayBase(), biggest);
    }
    // Short delays crowd the echoes together, so fewer stay separate. What
    // must never happen again is the rule running out of room for the voices
    // themselves, which is what the old all-pairs simple-ratio form did.
    Check("short delays fit fewer distinct echoes", fits[0] < fits[2]);
    Check("every voice can still sound at the shortest delay", fits[0] >= 1);
    Check("all seven echoes separate at a long delay", fits[2] == 7);
}

constexpr float kSr    = 32000.0f;   // what the board runs at
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
        if(ShownTile(v) < 0 || ShownTile(v) >= kTiles || ShownMod(v) < 0
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
        static const float hz[kVoices]
            = {220.0f, 264.0f, 330.0f, 440.0f, 528.0f, 660.0f, 880.0f};
        voices::SetPitch(i, hz[i], 2.0f * hz[0], true);
        Pass(i, now);
        now += 0.02f;
    }
}

// ------------------------------------------------------------------ tests

static void TestPadsAndVoices()
{
    printf("seven voices, one per pad, one group\n");
    float now = 0.0f;
    synth::Init(kSr, 48);
    for(int p = 0; p < synth::kPads; p++)
        Check("each pad owns its own voice", synth::PadVoice(p) == p);
    Check("one group", kGroups == 1 && kGroupSize == kVoices);

    for(int p = 0; p < synth::kPads; p++)
        synth::Press(p);
    Check("nothing is stolen", synth::PadVoice(0) == 0
                                   && synth::PadVoice(synth::kPads - 1)
                                          == synth::kPads - 1);
    AdvanceSeconds(now, 0.5f);
    Settle(now);
    Check("output finite", FiniteOutput());
    Check("shown state valid", ShownStateValid());

    // 220 264 330 440 528 660 880: two octaves of a major triad, so any
    // handful of pads is consonant and the spread covers the band
    const float want[7] = {1.0f, 1.2f, 1.5f, 2.0f, 2.4f, 3.0f, 4.0f};
    for(int p = 0; p < synth::kPads; p++)
        Near("pad sits on its interval", synth::PadHz(p) / synth::PadHz(0),
             want[p], 1e-3f);
    for(int a = 0; a < synth::kPads; a++)
        for(int b = a + 1; b < synth::kPads; b++)
            Check("every pad has its own pitch",
                  fabsf(synth::PadHz(a) - synth::PadHz(b)) > 1.0f);
}


// One Tick can heal, lapse, re-roll for unrest and chain a break in each group
// before anything is delivered; the press burst alone is only 12 events
static void TestEventQueueHolds()
{
    printf("the event queue holds a full Tick's worth across both groups\n");
    synth::Init(kSr, 48);
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
    synth::Init(kSr, 48);
    synth::SetKnob(5, 1.0f);   // Control: strictest
    synth::Press(0);
    Settle(now);
    Violation vio[kVoices * kVoices + kVoices];
    Check("a lone voice is legal", Diagnose(vio, (int)(sizeof(vio) / sizeof(vio[0]))) == 0);
}

// The line input is detected, not switched, so the detector is the contract
static void TestInputDetector()
{
    printf("a signal on the input turns it into an effects box\n");
    synth::Init(kSr, kBlock);
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
    synth::Init(kSr, kBlock);
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
    printf("hold floors every voice and a press rides on top\n");
    float now = 0.0f;
    auto run  = [&](float hold) {
        synth::Init(kSr, kBlock);
        synth::SetKnob(7, hold);
        synth::Press(0);
        synth::Press(3);
        AdvanceSeconds(now, 1.0f);
        synth::Release(0);
        synth::Release(3);
        AdvanceSeconds(now, 20.0f);
    };

    run(1.0f);
    Check("both voices sit at the floor after release",
          voices::Amp(0) > 0.9f && voices::Amp(3) > 0.9f);
    synth::SetKnob(7, 0.0f);
    AdvanceSeconds(now, 20.0f);
    Check("turning hold down releases them",
          voices::Amp(0) < 0.01f && voices::Amp(3) < 0.01f);

    run(0.5f);
    Near("the floor is the fader's level", voices::Amp(0), 0.5f, 0.01f);

    // The floor applies to a voice whose pad was never touched: that is what
    // makes HOLD a drone rather than a long tail
    synth::Init(kSr, kBlock);
    synth::SetKnob(7, 0.4f);
    AdvanceSeconds(now, 5.0f);
    Near("an untouched voice rises to the floor", voices::Amp(1), 0.4f, 0.01f);
    Near("and so does the last one", voices::Amp(wfc::kVoices - 1), 0.4f,
         0.01f);

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
    synth::Init(kSr, 48);
    synth::SetKnob(7, 1.0f);
    synth::Press(0);
    AdvanceSeconds(now, 1.0f);
    synth::Release(0);
    AdvanceSeconds(now, 10.0f);
    Check("floored voice is sounding", wfc::Sounding(0));
    Check("a never-pressed voice is too",
          wfc::Sounding(1) && wfc::Sounding(wfc::kVoices - 1));

    synth::SetKnob(7, 0.0f);
    AdvanceSeconds(now, 1.0f);
    Check("hold down ends sounding", !wfc::Sounding(0));
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
        // The common line is in the loop too, so it belongs in the sweep
        K().link = corner ? 1.0f : (float)(seed % 6) / 5.0f;
        K().shimmer = corner ? 1.0f : (float)(seed % 8) / 7.0f;
        voices::SetModMode((int)(seed % 3));
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
    voices::SetModMode(1);
    K().link = 0.0f;
    K().shimmer = 0.0f;
}

// Rule 1 says a loop above 1.0 rings by itself. The swell break sets exactly
// that, so the tail after release must be LONGER than an ordinary voice's.
// How long a break takes to finish arriving, and whether it leaves the
// group it started in. Both read from the SHOWN state, which is what the
// audio follows -- not from the hop formula, which would pass with the
// fader wired to nothing.
static void RippleShape(float r, int seed, double& span)
{
    float now = 0.0f;
    synth::Init(kSr, 48);
    wfc::Init(seed);
    wfc::SetRipple(r);
    for(int p = 0; p < synth::kPads; p++)
        if(synth::PadVoice(p) >= 0)
            synth::Press(p);
    AdvanceSeconds(now, 3.0f);
    Settle(now);
    synth::Freeze(true);

    int before[kVoices];
    for(int v = 0; v < kVoices; v++)
        before[v] = ShownTile(v);
    const float t0 = now;
    wfc::BreakRule(-1, 0, 0.0f, now);

    double last       = 0;
    int    seen[kVoices] = {};
    for(int i = 0; i < 400; i++)
    {
        AdvanceSeconds(now, 0.02f);
        for(int v = 0; v < kVoices; v++)
            if(!seen[v] && ShownTile(v) != before[v])
            {
                seen[v] = 1;
                last    = now - t0;
            }
    }
    span = last;
    synth::Freeze(false);
    wfc::SetRipple(0.35f);
}

static void TestRippleStagger()
{
    printf("the ripple sets how long a break takes to cross the voices\n");
    double   tight = 0, loose = 0;
    const int kSeeds = 8;
    for(int seed = 1; seed <= kSeeds; seed++)
    {
        double sp;
        RippleShape(0.0f, seed, sp);
        tight += sp / kSeeds;
        RippleShape(1.0f, seed, sp);
        loose += sp / kSeeds;
    }
    printf("  last voice lands at %.2f s tight, %.2f s loose\n", tight,
           loose);
    Check("a slow ripple takes longer to finish", loose > tight + 0.5);
    Check("a tight one is over at once", tight < 0.3);
}

static void TestBreakIsAGesture()
{
    printf("a break snaps the gain and crossfades the delay\n");
    double early = 0, late = 0;
    int    n = 0, arrived = 0, moved = 0;
    // Delay changes in place by 0.05 s, and by 0.10 s
    static int settled[2] = {};
    for(uint32_t seed = 3; seed <= 30; seed += 3)
    {
        float now = 0.0f;
        synth::Init(kSr, 48);
        wfc::Init(seed);
        for(int p = 0; p < synth::kPads; p++)
            if(synth::PadVoice(p) >= 0)
                synth::Press(p);
        // The wave's stagger is S36's business and this test is about what
        // one voice does when its turn comes, so pin it short
        wfc::SetRipple(0.0f);
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
        // one block. Stepped by the block, not by a fixed millisecond: a
        // block is 1.5 ms at the rate the board runs at, and asking
        // AdvanceSeconds for less than one advances nothing at all.
        float step[kVoices] = {}, walked[kVoices] = {}, prev[kVoices];
        for(int v = 0; v < kVoices; v++)
            prev[v] = f0[v];
        float dmid[kVoices];
        const float bdt  = (float)kBlock / kSr;
        const int   kEnd = (int)(0.25f / bdt), k50 = (int)(0.05f / bdt),
                    k100 = (int)(0.10f / bdt);
        for(int k = 0; k < kEnd; k++)
        {
            AdvanceSeconds(now, bdt);
            for(int v = 0; v < kVoices; v++)
            {
                const float d = fabsf(voices::EffFb(v) - prev[v]);
                walked[v] += d;
                if(d > step[v])
                    step[v] = d;
                prev[v] = voices::EffFb(v);
            }
            if(k == kEnd - 1)
                for(int v = 0; v < kVoices; v++)
                    dmid[v] = voices::EffDly(v);
            if(k == k50 || k == k100)
                for(int v = 0; v < kVoices; v++)
                    if(fabsf(voices::EffDly(v) - d0[v]) > 1e-5f)
                        settled[k == k50 ? 0 : 1]++;
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
    printf("  delay changes in place by 0.05 s: %d of %d\n", settled[0],
           settled[1]);
    wfc::SetRipple(0.35f);   // back to the default for whatever runs next
    // The delay used to glide to its new length, which is the scrub the
    // crossfade was put in to remove: the tap now switches under an
    // equal-power fade instead, so a break's delay lands as fast as its
    // gain. A reinstated glide needs ~0.14 s and fails this.
    //
    // The ratio counts how many voices the break wave has REACHED by 0.05 s
    // against 0.10 s, so it tracks the order the wave visits them in, and a
    // break picks its voice by level -- which means any change to the audio
    // reshuffles it. It read 49 of 51 before the read tap was interpolated
    // across the block and 44 of 51 after, with the landings themselves
    // unchanged: 386 either way, averaging 711 ms against 670 ms, 373
    // against 371 of them slower than 50 ms. 4/5 is the ordering slack that
    // leaves; a real glide lands nowhere near it.
    Check("the delay crossfades rather than sliding",
          n > 3 && settled[1] > 0 && settled[0] * 5 >= settled[1] * 4);
    (void)early;
    (void)late;
    // Measured over these ten seeds: 9 of 41 arrive in one step with the
    // snap, 2 of 41 without it -- the glide needs ~0.05 s, so the handful
    // that still look instant are voices whose gain barely moved at all.
    Check("the gain arrives in one step", arrived >= 5);
}

static void TestReleaseRerollsTheTail()
{
    printf("letting go rolls the voice a new tile for its tail\n");
    int rolled = 0, audible = 0;
    const int kRuns = 20;
    for(uint32_t seed = 1; seed <= (uint32_t)kRuns; seed++)
    {
        float now = 0.0f;
        synth::Init(kSr, 48);
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

// Seeds whose settled wash happens to sit at the two ends of its range
static double WashAfterPress(int seed, bool press, float& now)
{
    synth::Init(kSr, 48);
    wfc::Init(seed);
    for(int j = 0; j < synth::kPads; j++)
        if(j != 0)
            synth::Press(j);
    now = 0.0f;
    AdvanceSeconds(now, 4.0f);
    Settle(now);
    if(press)
    {
        synth::Press(0);
        AdvanceSeconds(now, 0.3f);
    }
    double w = 0;
    for(int v = 1; v < wfc::kVoices; v++)
        w += voices::BusLvl(v);
    return w;
}

static void TestTheDuckTargetsALevel()
{
    printf("a press ducks the wash to a level, not by a fixed step\n");
    // The loudest and quietest wash the solver settles to. Found rather
    // than named: hard-coded seeds stopped being the extremes the moment
    // the rules changed, and the test then measured nothing.
    float  now = 0.0f;
    double loud = -1.0, soft = 1e9;
    int    lseed = 1, sseed = 1;
    for(int seed = 1; seed <= 24; seed++)
    {
        const double w = WashAfterPress(seed, false, now);
        if(w > loud)
        {
            loud  = w;
            lseed = seed;
        }
        if(w < soft)
        {
            soft  = w;
            sseed = seed;
        }
    }
    const double loud_d = WashAfterPress(lseed, true, now);
    const double soft_d = WashAfterPress(sseed, true, now);
    printf("  wash %.3f (seed %d) and %.3f (seed %d) -> %.3f and %.3f after"
           " the press\n",
           loud, lseed, soft, sseed, loud_d, soft_d);
    Check("the two seeds really do differ", loud > soft * 1.5);
    // A fixed-fraction duck passes the spread straight through, which is
    // what made the same pad read clear or muted depending on nothing the
    // player did. The duck must close part of that gap, not preserve it.
    Check("and the press closes most of the gap",
          loud_d / soft_d < 0.70 * (loud / soft));
}

// Pressing hard is the player's own break. Frozen, so a tile that moves can
// only have been moved by the squeeze.
static int TilesAfter(float& now, bool squeeze, int seed)
{
    synth::Init(kSr, 48);
    wfc::Init(seed);
    now = 0.0f;
    synth::Press(0);
    synth::Press(1);
    AdvanceSeconds(now, 2.0f);
    synth::Freeze(true);
    Settle(now);
    const int before = ShownTile(0) * 100 + ShownTile(1);
    for(int i = 0; i < 40; i++)
    {
        synth::SetPressure(0, squeeze ? 0.03f * (float)i : 0.2f);
        AdvanceSeconds(now, 0.05f);
    }
    AdvanceSeconds(now, 1.5f);
    const int after = ShownTile(0) * 100 + ShownTile(1);
    synth::Freeze(false);
    synth::Release(0);
    synth::Release(1);
    return after != before;
}

// Everything sounding, which is the worst case the board renders.
// Relative numbers only: this is an x86 host and the Daisy keeps its delay
// lines in SDRAM. Best of several runs -- a single run varies by over 10%
// here, which is wider than most of the differences worth measuring.
static double BenchOne()
{
    const int kReps = 5, kBlocks = 20000;
    double    best = 1e30;
    for(int rep = 0; rep < kReps; rep++)
    {
        float now = 0.0f;
        synth::Init(kSr, 48);
        wfc::Init(5);
        for(int p = 0; p < synth::kPads; p++)
            synth::Press(p);
        AdvanceSeconds(now, 2.0f);
        const auto t0 = std::chrono::steady_clock::now();
        for(int b = 0; b < kBlocks; b++)
        {
            now += (float)kBlock / kSr;
            synth::Control(now);
            synth::Process(g_l, g_r, kBlock);
        }
        const double ns
            = std::chrono::duration<double>(
                  std::chrono::steady_clock::now() - t0)
                  .count()
              * 1e9 / (double)(kBlocks * kBlock);
        if(ns < best)
            best = ns;
        for(int p = 0; p < synth::kPads; p++)
            synth::Release(p);
    }
    return best;
}

static void Bench()
{
    // Interleaved, not one after the other: the host's own speed drifts by
    // more than 10% over a session, so an earlier figure is not a baseline
    const double a0 = BenchOne();
    const double s0 = BenchOne();
    const double a1 = BenchOne();
    const double s1 = BenchOne();
    const double off = a0 < a1 ? a0 : a1, on = s0 < s1 ? s0 : s1;
    printf("BENCH %.1f ns/sample\n", off < on ? off : on);
}

// All seven voices sounding is the normal case with HOLD up, and the solver
// runs on every knob move. A heal that outruns the control rate is what the
// board lockup looked like, so the cost is measured, not assumed.
static double BenchSolveOnce()
{
    float now = 1.0f;
    Init(9);
    K().safe   = 0.95f;
    K().unrest = 0.0f;
    RebuildRules();
    for(int v = 0; v < kVoices; v++)
    {
        SetSounding(v, true, now);
        now += 0.01f;
    }
    const clock_t t0 = clock();
    const int     kN = 300;
    for(int i = 0; i < kN; i++)
    {
        K().delay = 0.2f + 0.6f * (float)(i % 7) / 7.0f;
        RebuildRules();
        Heal(0, now);
        now += 0.01f;
    }
    return (double)(clock() - t0) / CLOCKS_PER_SEC / kN * 1e6;
}

static void BenchSolve()
{
    double best = 1e18;
    for(int i = 0; i < 5; i++)
    {
        const double t = BenchSolveOnce();
        if(t < best)
            best = t;
    }
    printf("BENCH SOLVE %.0f us per heal, seven voices\n", best);
}

static void TestBreaksFindTheLoudVoice()
{
    printf("a break lands on a voice that is actually sounding\n");
    int loud_hit = 0;
    const int kRuns = 60;
    for(int seed = 1; seed <= kRuns; seed++)
    {
        float now = 0.0f;
        synth::Init(kSr, 48);
        wfc::Init(seed);
        synth::Press(0);   // voice 0
        synth::Press(1);   // voice 1
        synth::Press(5);   // voice 2, the whole of group 0
        AdvanceSeconds(now, 2.0f);
        Settle(now);
        synth::Freeze(true);
        int before[kVoices];
        for(int v = 0; v < kVoices; v++)
            before[v] = ShownTile(v);
        // Set AFTER the last Control, which would overwrite these with the
        // real bus levels. Voice 1 is the only one worth breaking.
        wfc::SetLevel(0, 0.001f);
        wfc::SetLevel(1, 1.0f);
        wfc::SetLevel(2, 0.001f);
        wfc::BreakRule(-1, 0, 0.0f, now);
        AdvanceSeconds(now, 2.0f);
        if(ShownTile(1) != before[1])
            loud_hit++;
        synth::Freeze(false);
        for(int p = 0; p < synth::kPads; p++)
            synth::Release(p);
    }
    printf("  the loud voice moved in %d of %d breaks\n", loud_hit, kRuns);
    // Measured 40 of 60 weighted against 23 of 60 picking flat.
    Check("the loud one is the usual victim", loud_hit >= 32);
}

static void TestTheLedSeesBreaks()
{
    printf("the LED is told when a break actually arrives\n");
    float now = 0.0f;
    synth::Init(kSr, 48);
    wfc::Init(7);
    synth::Press(0);
    synth::Press(1);
    AdvanceSeconds(now, 2.0f);
    synth::Freeze(true);
    Settle(now);
    synth::TookBreak();   // clear whatever settling delivered

    // The control first: frozen and untouched, nothing may be reported
    AdvanceSeconds(now, 1.5f);
    Check("no break, nothing to flash", !synth::TookBreak());

    wfc::BreakRule(-1, 0, 0.0f, now);
    AdvanceSeconds(now, 1.5f);
    Check("a break reaches the LED", synth::TookBreak());
    // Reading it clears it, or the LED would stutter for ever
    Check("and only once", !synth::TookBreak());

    synth::Freeze(false);
    synth::Release(0);
    synth::Release(1);
}

static void TestLeaningOnAPadBreaksIt()
{
    printf("leaning on a held pad breaks its group\n");
    float now = 0.0f;
    int   hard = 0, light = 0;
    for(int seed = 1; seed <= 10; seed++)
    {
        hard += TilesAfter(now, true, seed);
        light += TilesAfter(now, false, seed);
    }
    printf("  %d of 10 squeezed changed, %d of 10 resting changed\n", hard,
           light);
    Check("a squeeze breaks the group", hard >= 9);
    // The control: without it nothing moves, so the test cannot pass by
    // counting breaks the solver was going to deliver anyway
    Check("resting on the pad does not", light == 0);

    // Hysteresis: held past the threshold it must not fire again
    synth::Init(kSr, 48);
    wfc::Init(4);
    now = 0.0f;
    synth::Press(0);
    AdvanceSeconds(now, 2.0f);
    synth::Freeze(true);
    Settle(now);
    synth::SetPressure(0, 0.9f);
    AdvanceSeconds(now, 1.5f);
    const int settled = ShownTile(0);
    for(int i = 0; i < 20; i++)
    {
        synth::SetPressure(0, 0.9f + 0.005f * (float)i);
        AdvanceSeconds(now, 0.05f);
    }
    AdvanceSeconds(now, 1.0f);
    Check("and holding it down does not fire again",
          ShownTile(0) == settled);

    // The bug this replaced: an ordinary hold sat above the old absolute
    // re-arm level, so a second lean did nothing until the pad was let go.
    // Ease off without releasing, then lean again.
    for(int i = 0; i < 20; i++)
    {
        synth::SetPressure(0, 0.35f);
        AdvanceSeconds(now, 0.05f);
    }
    AdvanceSeconds(now, 0.5f);
    synth::TookBreak();   // clear, then watch for the next one
    for(int i = 0; i < 20; i++)
    {
        synth::SetPressure(0, 0.35f + 0.035f * (float)i);
        AdvanceSeconds(now, 0.05f);
    }
    AdvanceSeconds(now, 1.5f);
    // On the break signal, not on the tile: one voice re-rolling can land
    // on the tile it already had, which would make this flaky
    Check("but easing off and leaning again does, without releasing",
          synth::TookBreak());

    synth::Freeze(false);
    synth::Release(0);
}

static void TestAPressDoesNotBreakItself()
{
    printf("pressing a pad does not break the note it is playing\n");
    // panel.cpp ramps a pad's pressure by half the remaining distance every
    // 28 ms, so SetPressure sees a rise, not a step. The host shell never
    // calls SetPressure at all, which is why running lapses.exe never
    // showed this: every press firmer than 0.5 used to break its own voice,
    // 40 times out of 40, re-rolling the tile of the note as it spoke.
    int broke = 0;
    const int kTries = 20;
    for(int i = 0; i < kTries; i++)
    {
        float now = 0.0f;
        synth::Init(kSr, 48);
        wfc::Init(i + 1);
        synth::SetKnob(6, 0.0f);   // Unrest off: it breaks things on its own
        AdvanceSeconds(now, 1.0f);
        synth::Freeze(true);       // and so does a scheduled heal
        synth::TookBreak();
        synth::Press(0);
        float       p    = 0.0f;
        const float kTgt = 1.0f;   // as hard as the pad reads
        for(int b = 0; b < (int)(0.5f * kSr / kBlock); b++)
        {
            if((b % (int)(0.028f * kSr / kBlock)) == 0)
            {
                p += (kTgt - p) * 0.5f;
                synth::SetPressure(0, p);
            }
            AdvanceSeconds(now, (float)kBlock / kSr);
        }
        if(synth::TookBreak())
            broke++;
        // And the deliberate squeeze it must still allow: hold, let the
        // finger settle, then push past it
        synth::TookBreak();
        for(int b = 0; b < 60; b++)
        {
            synth::SetPressure(0, kTgt);
            AdvanceSeconds(now, 0.05f);
        }
        synth::Freeze(false);
        synth::Release(0);
    }
    printf("  %d of %d presses broke their own voice\n", broke, kTries);
    Check("a press is not read as a lean", broke == 0);
}

static void TestTheRestIsWhereYouPressed()
{
    printf("the break is a push past the press, not a fixed force\n");
    // Both the board and the GUI call SetPressure for every pad on every
    // poll, held or not. TestAPressDoesNotBreakItself never sends those
    // idle zeroes, so it never saw that they consumed the reseed and left
    // the resting force at 0: on the board a firm press broke its own
    // voice, and leaning harder afterwards did nothing.
    int       press = 0, push = 0;
    const int kTries = 20;
    for(int i = 0; i < kTries; i++)
    {
        float now = 0.0f;
        synth::Init(kSr, 48);
        wfc::Init(i + 1);
        synth::SetKnob(6, 0.0f);   // Unrest off: it breaks things on its own
        AdvanceSeconds(now, 1.0f);
        synth::Freeze(true);       // and so does a scheduled heal
        for(int b = 0; b < 20; b++)
        {
            synth::SetPressure(0, 0.0f);
            AdvanceSeconds(now, 0.028f);
        }
        synth::TookBreak();
        synth::Press(0);
        // A firm press, well over kFloor, through the panel's own ramp
        float       p    = 0.0f;
        const float kMid = 0.5f;
        for(int b = 0; b < 18; b++)
        {
            p += (kMid - p) * 0.5f;
            synth::SetPressure(0, p);
            AdvanceSeconds(now, 0.028f);
        }
        if(synth::TookBreak())
            press++;
        // Then lean, from where the finger already is
        for(int b = 0; b < 18; b++)
        {
            p += (1.0f - p) * 0.5f;
            synth::SetPressure(0, p);
            AdvanceSeconds(now, 0.028f);
        }
        AdvanceSeconds(now, 0.5f);
        if(synth::TookBreak())
            push++;
        synth::Freeze(false);
        synth::Release(0);
    }
    printf("  %d of %d broke on the press, %d of %d on the push after\n",
           press, kTries, push, kTries);
    Check("a firm press is not itself a lean", press == 0);
    Check("and pushing past it breaks straight away", push == kTries);

    // A push is a lean AND a relax, so this does not test leaning further
    // from an already-leaned-on pad: that gesture is not made, and making
    // rest chase it would move the resting force up onto the leaned force,
    // which is what the relax then cannot get back under. Measured, letting
    // rest chase after a break cost two thirds of the leans at 4 Hz below.
    // The gesture as actually played: lean, relax, lean again, over and
    // over on one press. Each relax drops rest back, so this does not climb
    // the range the way the stepped test above does.
    const float kPeriod[] = {1.0f, 0.5f, 0.25f};
    for(int t = 0; t < 3; t++)
    {
        int got = 0;
        const int kCycles = 6;
        for(int i = 0; i < kTries; i++)
        {
            float now = 0.0f;
            synth::Init(kSr, 48);
            wfc::Init(i + 1);
            synth::SetKnob(6, 0.0f);
            AdvanceSeconds(now, 1.0f);
            synth::Freeze(true);
            synth::Press(0);
            float p = 0.0f;
            for(int b = 0; b < 18; b++)   // settle on the resting force
            {
                p += (0.35f - p) * 0.5f;
                synth::SetPressure(0, p);
                AdvanceSeconds(now, 0.028f);
            }
            synth::TookBreak();
            for(int c = 0; c < kCycles; c++)
            {
                const int half = (int)(kPeriod[t] * 0.5f / 0.028f);
                for(int phase = 0; phase < 2; phase++)
                {
                    const float tgt = phase ? 0.35f : 0.75f;
                    for(int b = 0; b < half; b++)
                    {
                        p += (tgt - p) * 0.5f;
                        synth::SetPressure(0, p);
                        AdvanceSeconds(now, 0.028f);
                    }
                }
                if(synth::TookBreak())
                    got++;
            }
            synth::Freeze(false);
            synth::Release(0);
        }
        printf("  at %.0f ms a cycle: %d of %d leans broke\n",
               kPeriod[t] * 1000.0f, got, kTries * kCycles);
        Check("every lean of a lean-relax gesture breaks",
              got == kTries * kCycles);
    }
}

static void TestTheKnobDoesNotDragTheTap()
{
    printf("turning Delay bends the tap, at a capped rate\n");
    float now = 0.0f;
    synth::Init(kSr, 48);
    wfc::Init(3);
    synth::SetKnob(4, 0.0f);   // mod off: it moves the tap on purpose
    synth::SetKnob(0, 0.3f);
    synth::Press(0);
    AdvanceSeconds(now, 2.0f);
    synth::Freeze(true);
    Settle(now);

    // This used to demand that the tap never travel at all, on the grounds
    // that sliding it resamples the line and Dopplers everything in it.
    // Right diagnosis, wrong quantity: the scrub came from the tap being
    // stepped once a BLOCK, and once it was interpolated per sample a bend
    // read as tape rather than as damage. What hurts is RATE. Past 1.0 s of
    // delay a second the tap walks backwards through the line faster than
    // the write head fills it, and that reverse scrub is what a fast turn
    // sounded like, so the cap is the thing worth pinning. Confirmed at the
    // panel: with the tap frozen and everything else the knob does left
    // alone, the wash went; capped, it went and the echoes still move.
    const float blk   = (float)kBlock / kSr;
    const float cap   = 0.5f * blk;   // kMaxSlew, in seconds of delay a block
    float       worst = 0.0f;
    int         moved = 0;
    const float first = voices::EffDly(0);
    float       prev  = first;
    for(int i = 0; i < 40; i++)
    {
        synth::SetKnob(0, 0.3f + 0.4f * (float)i / 39.0f);
        for(int b = 0; b < 20; b++)
        {
            AdvanceSeconds(now, blk);
            const float d = fabsf(voices::EffDly(0) - prev);
            prev          = voices::EffDly(0);
            if(d > 1e-7f)
                moved++;
            if(d > worst)
                worst = d;
        }
    }
    const float travelled = fabsf(prev - first);

    // And the case the cap is actually for: the knob thrown across its
    // travel. A slow turn never reaches the cap, so a test that only turns
    // slowly passes whether the cap is there or not -- raise kMaxSlew a
    // hundredfold and the slow sweep above does not budge.
    float fast = 0.0f;
    for(int i = 0; i < 8; i++)
    {
        synth::SetKnob(0, 0.7f - 0.4f * (float)i / 7.0f);
        for(int b = 0; b < 6; b++)
        {
            AdvanceSeconds(now, blk);
            const float d = fabsf(voices::EffDly(0) - prev);
            prev          = voices::EffDly(0);
            if(d > fast)
                fast = d;
        }
    }
    synth::Freeze(false);
    synth::Release(0);
    // wfc's knobs are file-scope and Init does not reset them, and this
    // test ends 20 s into its own timeline. Hand the next test a clean
    // engine rather than relying on where it sits in the list.
    synth::SetKnob(0, 0.6f);
    synth::SetKnob(4, 0.5f);
    printf("  tap moved in %d of 800 blocks, %.0f ms travelled,"
           " worst step %.3f ms against a %.3f ms cap\n",
           moved, travelled * 1000.0f, worst * 1000.0f, cap * 1000.0f);
    Check("the knob does move the delay", travelled > 0.05f);
    // The whole point of the cap: one block over it is a reverse scrub
    printf("  thrown across its travel, worst step %.3f ms\n",
           fast * 1000.0f);
    Check("but never faster than the cap", worst <= cap * 1.05f
                                               && fast <= cap * 1.05f);
    // Without the cap this sweep steps many times it, which is the tap
    // walking backwards through the line: the reverse scrub
    Check("and the cap is what is holding it", fast > cap * 0.5f);
    // A bend, not a staircase. Stepped once a block the tap moved in well
    // under half the blocks; glided it moves in nearly all of them.
    Check("and it bends rather than stepping", moved > 700);
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
            synth::Init(kSr, 48);
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
    synth::Init(kSr, 48);
    wfc::Init(5);
    // Three voices with tails, settled, so the wash is what a new pad has
    // to cut through
    synth::Press(0);
    synth::Press(1);
    synth::Press(5);
    AdvanceSeconds(now, 4.0f);
    Settle(now);
    // Four seconds is not rest: `fresh` decays with a 2.5 s tau, so the
    // duck is still 3% in at that point
    AdvanceSeconds(now, 6.0f);
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
    // ratio is bracketed from both sides. Measured 0.77 with both wired;
    // 1.10 with the duck cut out, 0.68 with the dry lift cut out.
    // The level targeting holds the bus roughly where it was instead of
    // lifting it, which is why this band sits below 1 and the old one above.
    const double ratio = after / quiet;
    printf("  wash ratio %.2f\n", ratio);
    Check("the bus actually ducks", ratio < 0.94);
    Check("and the new voice's dry actually lifts it", ratio > 0.72);

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

// Mod bends the target's partials, which lands as sidebands above them. The
// phase offset goes through a cast and a clamp, and if it came out zero
// every other test here would still pass, so this is the one that says the
// mode does anything at all.
// The HOLD fader has to be able to wake the instrument on its own. With it
// down and no pad held the chain goes to sleep after a few seconds of
// silence, and raising the fader then has to bring the drone back without a
// press -- it used to need one, which left the instrument dead in the hand.
// HOLD is a floor under every voice, so a voice can be made to sound without
// ever having been pressed -- and Press was the only thing that tuned one.
static void TestEveryVoiceIsTunedBeforeItIsPlayed()
{
    printf("a voice carries its pad's note before it is ever pressed\n");
    synth::Init(kSr, kBlock);
    int off = 0;
    for(int p = 0; p < synth::kPads; p++)
    {
        const int v = synth::PadVoice(p);
        if(v >= 0 && fabsf(voices::Pitch(v) - synth::PadHz(p)) > 0.01f)
            off++;
    }
    printf("  %d of %d voices off their pad's note, untouched\n", off,
           synth::kPads);
    Check("every voice is already on its own note", off == 0);
}

// LATCH keeps exactly the pads that were down when the switch went down.
// A pad pressed afterwards is momentary again, which is what makes the
// switch a snapshot and not a sustain pedal.
static void TestLatchKeepsThePadsDown()
{
    printf("LATCH keeps the pads held at the moment it went down, and no others\n");
    synth::Init(kSr, kBlock);
    wfc::Init(5);
    float now = 1.0f;
    synth::Press(0);
    AdvanceSeconds(now, 0.5f);
    synth::Latch(true);
    synth::Release(0);
    AdvanceSeconds(now, 0.5f);
    const bool kept = synth::PadOn(0) && wfc::Sounding(synth::PadVoice(0));
    // Pressed after the switch went down. The panel calls Latch every poll,
    // so this is the case that used to collect it.
    synth::Press(3);
    AdvanceSeconds(now, 0.3f);
    synth::Latch(true);
    const bool sounds = synth::PadOn(3);
    synth::Release(3);
    AdvanceSeconds(now, 0.3f);
    const bool dropped = !synth::PadOn(3);
    synth::Latch(false);
    AdvanceSeconds(now, 0.2f);
    const bool cleared = !synth::PadOn(0);
    printf("  kept %d, later pad sounds while held %d, drops on release %d, "
           "cleared %d\n", (int)kept, (int)sounds, (int)dropped, (int)cleared);
    Check("the pad held at the edge stays down", kept);
    Check("a pad pressed later still plays while held", sounds);
    Check("but it is not latched", dropped);
    Check("letting the switch go clears the lot", cleared);
}

// Share of energy above 3 kHz. The 500 Hz split BandRms uses cannot resolve
// this: three quarters of the material is already above 500 Hz, so added
// harmonics do not move it. Saturation products land high.
static void TopShare(float& now, float secs, double& top, double& full)
{
    float  hp1 = 0.0f, hp2 = 0.0f;
    double t = 0, f = 0;
    int    n = 0;
    const float c = 1.0f - expf(-2.0f * 3.14159265f * 3000.0f / kSr);
    for(int i = 0; i < (int)(secs / 0.01f); i++)
    {
        AdvanceSeconds(now, 0.01f);
        for(int k = 0; k < kBlock; k++)
        {
            const float x = g_l[k];
            hp1 += (x - hp1) * c;
            const float y = x - hp1;
            hp2 += (y - hp2) * c;   // two poles, so the skirt is not the signal
            t += (double)(y - hp2) * (y - hp2);
            f += (double)x * x;
            n++;
        }
    }
    top  = n ? sqrt(t / n) : 0;
    full = n ? sqrt(f / n) : 0;
}

// THE LINK is one delay every voice feeds and reads, so a voice with
// nothing touching it still rings with what the rest of the instrument is
// doing. S35 is how much, and it has to grade: off, half and full in order.
static void TestLinkRingsTheUntouchedVoices()
{
    printf("the link knob rings the voices nobody is touching, by degrees\n");
    const float kv[3] = {0.0f, 0.5f, 1.0f};
    double      q[3]  = {};
    bool        ok    = true;
    for(int c = 0; c < 3; c++)
    {
        synth::Init(kSr, kBlock);
        wfc::Init(5);
        K().fb     = 0.8f;
        K().link = kv[c];
        float now  = 1.0f;
        synth::Press(0);
        AdvanceSeconds(now, 4.0f);
        const int held = synth::PadVoice(0);
        for(int v = 0; v < wfc::kVoices; v++)
            if(v != held)
                q[c] += voices::LoopLevel(v);
        if(!FiniteOutput())
            ok = false;
        printf("  knob %.1f: untouched voices sum %.5f\n", kv[c], q[c]);
        synth::Release(0);
    }
    K().link = 0.0f;
    Check("at zero the untouched voices are silent", q[0] < 1.0e-4);
    Check("half rings them", q[1] > 1.0e-3);
    Check("and full rings them harder", q[2] > q[1] * 1.3);
    Check("bounded throughout", ok);
}

// The pool has to make something the voices were not already making. Simply
// returning it into each line does not: that is a linear sum of the same
// material, measured at +6.5 dB on the bus with the band split unmoved, and
// it reads as nothing at all. Driving each loop's distortion with it does,
// and this is the measurement that separates the two.
static void TestLinkMakesNewContent()
{
    printf("the link dirties the voices, not just fills them\n");
    double share[2] = {}, lvl[2] = {};
    for(int on = 0; on < 2; on++)
    {
        synth::Init(kSr, kBlock);
        wfc::Init(5);
        K().link = on ? 1.0f : 0.0f;
        float now  = 1.0f;
        synth::Press(0);
        AdvanceSeconds(now, 4.0f);
        double top, full;
        TopShare(now, 2.0f, top, full);
        share[on] = full > 0 ? top / full : 0;
        lvl[on]   = full;
        printf("  knob %.0f: level %.4f, share above 3 kHz %.4f\n", on ? 1.0f : 0.0f,
               full, share[on]);
        synth::Release(0);
    }
    K().link = 0.0f;
    printf("  x%.2f the level, x%.2f the top share\n",
           lvl[0] > 0 ? lvl[1] / lvl[0] : 0, share[0] > 0 ? share[1] / share[0] : 0);
    Check("it makes new high content, not just more of the same",
          share[1] > share[0] * 1.5);
}

// S09/S10 picks what DRIVES the mod. Measured with nothing pressed at all
// and HOLD floating the voices, which is the case the switch exists for.
//
// It does NOT separate them into one that stops and two that do not: the
// loops feed each other, so the ensemble never settles and the follower
// always has something to follow. It parks only in the sense of moving
// less. What does separate them is HOW they move, as the largest step the
// value takes over its average step: a sine is pi/2 and nothing else, a
// jump-and-hold is most of its range at once over a long flat average, and
// the follower is between the two because the thing it follows is noisy.
static void TestWhatDrivesTheMod()
{
    printf("the three mod sources move in three different ways\n");
    double swing[3] = {}, jumpy[3] = {};
    for(int m = 0; m < 3; m++)
    {
        synth::Init(kSr, kBlock);
        wfc::Init(5);
        K().mod    = 1.0f;
        K().unrest = 0.0f;   // it moves things on its own
        K().hold   = 1.0f;   // the drone, with no pad touched
        voices::SetModMode(m);
        float now = 1.0f;
        AdvanceSeconds(now, 8.0f);
        synth::Freeze(true);   // and no scheduled heal either
        const int v = synth::PadVoice(1);
        // 12 s covers several turns of the slowest LFO
        double    lo = 9e9, hi = -9e9, dsum = 0.0, dmax = 0.0;
        float     prev = voices::Mod(v);
        const int kN   = 600;
        for(int i = 0; i < kN; i++)
        {
            AdvanceSeconds(now, 0.02f);
            const float x = voices::Mod(v);
            if(x < lo)
                lo = x;
            if(x > hi)
                hi = x;
            const double d = fabs(x - prev);
            dsum += d;
            if(d > dmax)
                dmax = d;
            prev = x;
        }
        swing[m] = hi - lo;
        jumpy[m] = dsum > 0.0 ? dmax / (dsum / (double)kN) : 0.0;
        printf("  %-7s swing %.3f   largest move over average %.1f\n",
               m == 0 ? "steps" : (m == 1 ? "follow" : "LFO"), swing[m],
               jumpy[m]);
        synth::Freeze(false);
    }
    K().mod    = 0.5f;
    K().unrest = 0.2f;
    K().hold   = 0.0f;
    voices::SetModMode(1);
    for(int m = 0; m < 3; m++)
        Check("every source keeps moving with nothing played", swing[m] > 0.5);
    Check("a step is a jump and the follower is not", jumpy[0] > jumpy[1] * 4.0);
    Check("and the follower is not a sine either", jumpy[1] > jumpy[2] * 1.5);
}

// S36, on the instrument rather than on a tone: the fader has to outlive the
// dry tail without running the level away with it.
// The shift itself, with nothing else in the way: a tone on the line input
// and no pads, so the voices are silent and the only things between the
// input and the output are the plate and the shimmer line.
static void TestShimmerMakesAnOctave()
{
    printf("S36 puts an octave above whatever it is given\n");
    const float kHz = 300.0f;
    double      rel[2];
    for(int w = 0; w < 2; w++)
    {
        synth::Init(kSr, kBlock);
        wfc::Init(7);
        K().shimmer = (float)w;
        float in[kBlock];
        float ph = 0.0f;
        // Goertzel at the tone and at its octave, over the last second
        double c1[2] = {0, 0}, c2[2] = {0, 0}, g = 0;
        const int blocks = (int)(6.0f * kSr / (float)kBlock);
        float     now    = 1.0f;
        for(int b = 0; b < blocks; b++)
        {
            for(int i = 0; i < kBlock; i++)
            {
                in[i] = 0.3f * sinf(ph);
                ph += 2.0f * 3.14159265f * kHz / kSr;
                if(ph > 6.2831853f)
                    ph -= 6.2831853f;
            }
            now += (float)kBlock / kSr;
            synth::Control(now);
            synth::Process(g_l, g_r, kBlock, in);
            if(b < blocks - (int)(kSr / kBlock))
                continue;
            for(int i = 0; i < kBlock; i++)
            {
                const double t = g * 2.0 * 3.14159265 / kSr;
                c1[0] += g_l[i] * cos(t * kHz);
                c1[1] += g_l[i] * sin(t * kHz);
                c2[0] += g_l[i] * cos(t * 2.0 * kHz);
                c2[1] += g_l[i] * sin(t * 2.0 * kHz);
                g += 1.0;
            }
        }
        const double f = sqrt(c1[0] * c1[0] + c1[1] * c1[1]);
        const double o = sqrt(c2[0] * c2[0] + c2[1] * c2[1]);
        rel[w]         = f > 0 ? o / f : 0;
        printf("  shimmer %.0f: octave is %.3f of the tone\n", (double)w, rel[w]);
    }
    K().shimmer = 0.0f;
    Check("the octave is there only with the fader up", rel[1] > rel[0] * 4.0);
}

static void TestShimmerClimbsTheTail()
{
    printf("S36 pushes the tail up an octave a pass\n");
    double share[2], lvl[2], tail[2];
    for(int w = 0; w < 2; w++)
    {
        synth::Init(kSr, kBlock);
        wfc::Init(5);
        K().shimmer = (float)w;
        float now = 1.0f;
        synth::Press(0);
        AdvanceSeconds(now, 3.0f);
        double top, full;
        TopShare(now, 2.0f, top, full);
        share[w] = full > 0 ? top / full : 0;
        lvl[w]   = full;
        synth::Release(0);
        AdvanceSeconds(now, 4.0f);
        tail[w] = WindowRms(now, 1.0f);
        printf("  shimmer %.0f: level %.4f  share above 3 kHz %.4f  tail %.5f\n",
               (double)w, lvl[w], share[w], tail[w]);
    }
    K().shimmer = 0.0f;
    // The share above 3 kHz is printed and not checked: the plate's output is
    // dark enough that an octave of it still lands under 3 kHz, so the split
    // cannot see the thing it was chosen to see. What proves this is not just
    // a louder send is the octave itself, which TestShimmerMakesAnOctave
    // measures directly against a tone.
    Check("the tail outlives the dry one", tail[1] > tail[0] * 1.3);
}

static void TestHoldWakesTheChain()
{
    printf("the HOLD fader wakes the chain by itself\n");
    synth::Init(kSr, kBlock);
    wfc::Init(3);
    K().hold = 0.0f;
    float now = 1.0f;
    synth::Press(0);
    AdvanceSeconds(now, 1.0f);
    synth::Release(0);
    // Long enough for the tail to die and the sleep to engage
    AdvanceSeconds(now, 20.0f);
    const double asleep = WindowRms(now, 0.5f);
    K().hold = 0.8f;
    const double woke = WindowRms(now, 3.0f);
    printf("  rms %.6f asleep, %.6f after the fader comes up\n", asleep, woke);
    Check("the fader brings the drone back with no press", woke > 0.01);
    K().hold = 0.0f;
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





// The Delay knob is the instrument's tempo: echo length, glide, release,
// the follower's reaction time. An attack is not one of those -- it answers
// nothing, it IS the press -- and tied to the knob it reached 0.95 s at the
// top, where the lift that announces a press is long spent before the note
// has arrived. So the attack is now flat across the knob and the release is
// deliberately not, which is the asymmetry this pins.
//
// Last in the suite: it runs four settled ensembles, and every Rand() it
// draws shifts the draws of whatever follows.
static void TestAttackIsFlatAndReleaseIsNot()
{
    printf("the Delay knob sets the release but not the attack\n");
    const float kDly[] = {0.0f, 0.3f, 0.6f, 1.0f};
    int         t90[4] = {}, t10[4] = {};
    for(int di = 0; di < 4; di++)
    {
        synth::Init(kSr, kBlock);
        wfc::Init(5);
        K().delay  = kDly[di];
        K().unrest = 0.0f;   // it moves things on its own
        float now  = 1.0f;
        for(int p = 0; p < 4; p++)
            synth::Press(p);
        AdvanceSeconds(now, 6.0f);
        synth::Freeze(true);
        const int v = synth::PadVoice(5);
        synth::Press(5);
        // 3 s up, then 8 s down: the slowest release is nearly 6 s
        float up[300] = {}, dn[800] = {};
        for(int i = 0; i < 300; i++)
        {
            AdvanceSeconds(now, 0.01f);
            up[i] = voices::Amp(v);
        }
        synth::Release(5);
        for(int i = 0; i < 800; i++)
        {
            AdvanceSeconds(now, 0.01f);
            dn[i] = voices::Amp(v);
        }
        const float top = up[299];
        t90[di] = t10[di] = -1;
        for(int i = 0; i < 300; i++)
            if(t90[di] < 0 && up[i] >= top * 0.9f)
                t90[di] = i * 10;
        for(int i = 0; i < 800; i++)
            if(t10[di] < 0 && dn[i] <= top * 0.1f)
                t10[di] = i * 10;
        printf("  delay %.1f: up to 90%% in %d ms, down to 10%% in %d ms\n",
               kDly[di], t90[di], t10[di]);
        synth::Freeze(false);
        for(int p = 0; p < 6; p++)
            synth::Release(p);
    }
    K().delay  = 0.6f;
    K().unrest = 0.2f;
    for(int di = 1; di < 4; di++)
        Check("the attack is the same wherever Delay is",
              t90[di] == t90[0] && t90[0] > 0);
    // The control: the same run, the same measurement, on the one the knob
    // is still meant to set
    Check("and the release is not", t10[3] > t10[0] * 4);
}

constexpr int kBridgeBlocks = 400;


static float Delta(const float* a, const float* b)
{
    float d = 0.0f;
    for(int i = 0; i < kBridgeBlocks * kBlock; i++)
        d += fabsf(a[i] - b[i]);
    return d;
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
    TestRule3DistinctEchoes();
    TestPadsAndVoices();
    TestLoneVoiceInGroup();
    TestEventQueueHolds();
    TestHold();
    TestHoldIsSounding();
    TestOctaveLeavesHeldVoicesAlone();
    TestInputDetector();
    TestSilence();
    TestSounds();
    TestModulator();
    TestNeverBlowsUp();

    TestTheDuckTargetsALevel();
    TestLowPadsCarryTheBand();
    TestNewVoiceGetsRoom();
    TestReleaseRerollsTheTail();
    TestRippleStagger();
    TestBreakIsAGesture();
    TestSwellRings();
    // Last: it ends 20 s into its own timeline and several tests below
    // inherit knob and timing state rather than setting their own
    Bench();
    BenchSolve();
    TestBreaksFindTheLoudVoice();
    TestTheLedSeesBreaks();
    TestLeaningOnAPadBreaksIt();
    TestEveryVoiceIsTunedBeforeItIsPlayed();
    TestWhatDrivesTheMod();
    TestLinkMakesNewContent();
    TestLatchKeepsThePadsDown();
    TestLinkRingsTheUntouchedVoices();
    TestShimmerMakesAnOctave();
    TestShimmerClimbsTheTail();
    TestHoldWakesTheChain();
    TestAPressDoesNotBreakItself();
    TestTheRestIsWhereYouPressed();
    TestTheKnobDoesNotDragTheTap();
    TestAttackIsFlatAndReleaseIsNot();

    if(fails)
        printf("\n%d FAILED\n", fails);
    else
        printf("\nall passed\n");
    return fails ? 1 : 0;
}
