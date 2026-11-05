#include "wfc.h"

#include <math.h>
#include <string.h>

namespace wfc
{
namespace
{
// How far a repair is allowed to wander from the tile a voice already has.
// The prototype's S.wild, which was never exposed as a control.
constexpr float kWild = 0.5f;

// xorshift32: every random number in here, so a desktop test can fix the seed
uint32_t rng_state = 1;
inline uint32_t Next()
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}
// 0..1
inline float Rand() { return (float)(Next() >> 8) * (1.0f / 16777216.0f); }
inline int   RandInt(int n) { return (int)(Rand() * (float)n) % n; }

inline float Clamp(float x, float a, float b)
{
    return x < a ? a : (x > b ? b : x);
}
inline float Lerp(float a, float b, float k) { return a + (b - a) * k; }
inline float Smooth(float a, float b, float x)
{
    const float k = Clamp((x - a) / (b - a), 0.0f, 1.0f);
    return k * k * (3.0f - 2.0f * k);
}

Knobs  knob   = {0.6f, 0.5f, 0.3f, 0.55f, 0.5f, 0.7f, 0.2f, 0.0f, 1.0f};
bool   frozen = false;
// How a wave travels: the gap between one voice changing and the next, and,
// past kSpread, whether it carries into the other group. It had a fader of
// its own and did not earn it -- the difference is not audible unless you
// are listening for it -- so it is pinned, below kSpread, which leaves the
// two groups as separate as they were. Off the Delay knob either way.
// Pinned. 0.35 was a guess and it cost the breaks their legibility: at a
// 0.29 s hop the wave takes 1.8 s to cross seven voices, so the victim can
// change two seconds after the LED. Measured over the first 0.4 s after a
// break, an echo moves the mix 1.3% at 0.35 and 4.8% at 0.02; the whole
// wave still takes 0.25 s, which reads as a cascade rather than a click.
float  ripple = 0.02f;
// What each voice is actually putting out, pushed in from the audio side
// once a block. wfc cannot ask voices directly -- voices is the layer above
// -- and a break needs it to avoid landing on something inaudible.
float  level[kVoices] = {};
int    cable_mode = 0;   // 0 none, 1 feed, 2 mod
const int* rp_tiles = nullptr;   // the tiles the patch search weighs
Limits limits = {};

// The rule tables, rebuilt on every knob move: which tiles may sit beside
// which (pair rules 1 to 3), and which a voice may hold at all (rule 1)
uint8_t compat[kDelays * kDelays];
uint8_t self_ok[kTiles];
// 10,000 pairs, so the tables are rebuilt when something is about to read
// them rather than every time a knob twitches. A knob moves at the block
// rate; a solve runs a few times a second.
bool tables_dirty = true;
void EnsureTables();

// Logical: what WFC has decided. Shown: what the audio has reached.
int16_t tile[kVoices], shown_tile[kVoices];
int8_t  patch[kVoices], shown_patch[kVoices];
int8_t  mod_opt[kVoices], shown_mod[kVoices];
uint8_t skew[kVoices], shown_skew[kVoices];
// Per field, so an older event in the wave never overwrites a newer one
int32_t seq_tile[kVoices], seq_patch[kVoices], seq_mod[kVoices],
    seq_skew[kVoices];
int32_t seq = 0;

bool  sounding[kVoices];
float started_at[kVoices];
int   flash[kVoices];
// Set when a break is DELIVERED, not when it is scheduled: the LED stutters
// at the moment the sound changes. Separate from flash[], which voices::
// Control consumes for the gesture.
bool  broke = false;

// A change the wave carries. -1 means the field is not part of it.
struct Change
{
    int tile, patch, mod, skew;
};
inline Change NoChange() { return Change{-1, -1, -1, -1}; }

struct Event
{
    float   t;
    int     v;
    int32_t seq;
    int     kind;   // 2 = break, 1 = repair
    Change  ch;
};
// ponytail: a fixed queue. Worst case per group in one Tick is 12 events (heal,
// lapse, unrest and a chained break, 3 voices each), but knob_settle can Heal
// each group twice and earlier ticks' events carry over: measured about 30 plus
// carry. 48 holds that; recount if a Tick starts scheduling more.
constexpr int kMaxEvents = 48;
Event         events[kMaxEvents];
int           n_events = 0, n_dropped = 0;

float heal_at[kGroups], next_lapse[kGroups], next_unrest[kGroups];

bool  joined      = false;
float solve_fm    = -1.0f;
float table_fm    = -1.0f;
// Duck, six voices, default knobs, P05 down vs up: 0.69x load at Unrest 0, 0.58x at 0.3
constexpr float kChargeGain = 4.0f;
float knob_settle = 0;

// ------------------------------------------------------------- the sounding

int Active(int* act, int group)
{
    int n = 0;
    for(int v = 0; v < kVoices; v++)
        if(sounding[v] && (group < 0 || GroupOf(v) == group))
            act[n++] = v;
    return n;
}

// What the rules read in place of FbMul() for a group: its own feedback
// inflated by the louder group's shown load. The audio never sees it. Writes
// nothing, so Diagnose can call it from the window thread; its reads of shown
// state are unsynchronised on purpose, a torn one costs a frame of diagnostic.
float ChargedFb(int group)
{
    const float fm = FbMul();
    if(!joined || group < 0)
        return fm;
    // Louder group, own included: charging only the other group let one half
    // stay loud while the other paid
    float gl[kGroups] = {};
    for(int v = 0; v < kVoices; v++)
        if(sounding[v])
            gl[GroupOf(v)] += FL[TileF(shown_tile[v])];
    float load = 0.0f;
    for(int g = 0; g < kGroups; g++)
        load = fmaxf(load, gl[g]);
    // Capped so the quietest tile always passes every per-tile rule: an empty
    // table makes RandomTile() fall back to kDefaultTile, which is louder.
    const float cap = 0.98f
                      * fminf(fminf(limits.Fself / FL[0],
                                    limits.Fshare / FL[0]),
                              limits.Lpeak / FL[0]);
    return fmaxf(fm, fminf(fm * (1.0f + kChargeGain * load / kGroupSize), cap));
}

// A voice's delay as a multiple of the base time, skew included. The rule
// reads this rather than the class alone, so a skewed voice is judged by the
// time it actually echoes at.
float ShownDly(int v)
{
    return DR[TileD(shown_tile[v])] * (shown_skew[v] ? kSkew : 1.0f);
}

// The ensemble budgets, floored at the matching per-voice limit so a single
// sounding voice is governed by its own rule and nothing stricter. At two
// voices these are exactly the pair sums they replaced.
float FbBudget(int n)
{
    return fmaxf(limits.Fself, limits.Fshare * (float)n);
}
float DrBudget(int n)
{
    return fmaxf(limits.Rself, limits.Dshare * (float)n);
}

// The charge is fixed for a whole solve (shown_tile cannot move mid-solve), and
// the mod search reads it thousands of times, so the solve entry caches it.
void SetSolveGroup(int g)
{
    solve_fm = ChargedFb(g);
    if(solve_fm != table_fm)
        tables_dirty = true;
}

// The repair wave reaches the most recently started voice first
void Recency(int* a, int n)
{
    for(int i = 1; i < n; i++)
        for(int j = i; j > 0 && started_at[a[j]] > started_at[a[j - 1]]; j--)
        {
            const int t = a[j];
            a[j]        = a[j - 1];
            a[j - 1]    = t;
        }
}

inline bool InList(const int* a, int n, int v)
{
    for(int i = 0; i < n; i++)
        if(a[i] == v)
            return true;
    return false;
}

// ---------------------------------------------------------------- the rules

// `in_amt` is the cable depth arriving at this voice, which only counts in
// How much cable is arriving at each sounding voice, as a sum of amounts.
// Feed spends it as gain, mod spends it as modulation depth, so the two modes
// budget the same number against different rules.
void InAmounts(const int8_t* pa, const int* act, int n, float* in_amt,
               int* in_deg)
{
    for(int i = 0; i < n; i++)
    {
        in_amt[act[i]] = 0.0f;
        in_deg[act[i]] = 0;
    }
    for(int i = 0; i < n; i++)
    {
        const int o = pa[act[i]];
        if(!o)
            continue;
        const int t = OptTarget(o);
        if(!InList(act, n, t))
            continue;
        in_amt[t] += AMT[OptAmt(o)];
        in_deg[t]++;
    }
}

// The cable rules. Rule 4 is the old one -- no self patch ever, and no cycle
// when strict -- but the cycle half is FEED ONLY: audio going round a ring
// multiplies gain on top of each voice's own feedback, while modulation going
// round a ring is a pair of coupled oscillators and is the whole attraction.
//
// Rule 6 is the cable cover: nobody sounding is left out of the patching, and
// at high strictness they must all be in ONE structure rather than two.
//
// Rule 7 is the per-voice gain budget, feed only: a voice may not be both
// self-ringing and heavily fed (break type `flood`). Mod has no budget --
// nothing constrains modulation depth.
bool PatchesOK(const int8_t* pa, const int* act, int n, const int* tiles)
{
    for(int i = 0; i < n; i++)
    {
        const int o = pa[act[i]];
        if(o && OptTarget(o) == act[i])
            return false;
    }
    if(!cable_mode)
    {
        // Switched off, the only cable that may exist is no cable
        for(int i = 0; i < n; i++)
            if(pa[act[i]])
                return false;
        return true;
    }

    float in_amt[kVoices];
    int   in_deg[kVoices];
    InAmounts(pa, act, n, in_amt, in_deg);

    // Rule 6: an edge cover. A single voice cannot patch to anything, so
    // there is nothing to ask of it.
    if(n >= 2)
        for(int i = 0; i < n; i++)
            if(!pa[act[i]] && !in_deg[act[i]])
                return false;

    for(int i = 0; i < n; i++)
    {
        // Rule 7a: fan-in. Feed is capped harder, because what arrives there
        // is gain; mod only gets busier.
        if(in_deg[act[i]] > limits.fan_in)
            return false;
        // Rule 7b: the flood budget, feed only
        if(cable_mode == 1 && tiles
           && FL[TileF(tiles[act[i]])] * solve_fm + in_amt[act[i]]
                  > limits.Iflood)
            return false;
    }

    // Rule 6b: one structure, not two disjoint pairs. Edges count either way
    // round for this, so a chain satisfies it without a cycle.
    if(limits.connected_on && n >= 2)
    {
        bool seen[kVoices] = {};
        int  stack[kVoices], ns = 0;
        stack[ns++]     = act[0];
        seen[act[0]]    = true;
        int reached     = 1;
        while(ns)
        {
            const int cur = stack[--ns];
            for(int i = 0; i < n; i++)
            {
                const int w = act[i];
                if(seen[w])
                    continue;
                const bool linked
                    = (pa[cur] && OptTarget(pa[cur]) == w)
                      || (pa[w] && OptTarget(pa[w]) == cur);
                if(linked)
                {
                    seen[w]     = true;
                    stack[ns++] = w;
                    reached++;
                }
            }
        }
        if(reached < n)
            return false;
    }

    // Rule 4's cycle half: feed only
    if(!limits.cycle_on || cable_mode != 1)
        return true;
    for(int i = 0; i < n; i++)
    {
        int cur = act[i];
        for(int s = 0; s < kVoices; s++)
        {
            const int o = pa[cur];
            if(!o)
                break;
            const int t = OptTarget(o);
            if(!InList(act, n, t))
                break;
            cur = t;
            if(cur == act[i])
                return false;
        }
    }
    return true;
}

// --------------------------------------------------------- arc consistency

// Narrow every node's domain to the tiles that still have a partner in every
// other node's domain. False when a domain empties: no solution from here.
bool Ac(uint8_t dom[kVoices][kTiles], const int* nodes, int n)
{
    int  q[kVoices * 4];
    bool in_q[kVoices] = {};
    int  qn            = 0;
    for(int i = 0; i < n; i++)
    {
        q[qn++]        = nodes[i];
        in_q[nodes[i]] = true;
    }
    while(qn)
    {
        const int c = q[--qn];
        in_q[c]     = false;
        uint8_t have[kDelays] = {}, allow[kDelays] = {};
        for(int t = 0; t < kTiles; t++)
            if(dom[c][t])
                have[TileD(t)] = 1;
        for(int d = 0; d < kDelays; d++)
            if(have[d])
                for(int u = 0; u < kDelays; u++)
                    if(compat[d * kDelays + u])
                        allow[u] = 1;
        for(int i = 0; i < n; i++)
        {
            const int o = nodes[i];
            if(o == c)
                continue;
            bool changed = false, any = false;
            for(int u = 0; u < kTiles; u++)
                if(dom[o][u])
                {
                    if(!allow[TileD(u)])
                    {
                        dom[o][u] = 0;
                        changed   = true;
                    }
                    else
                        any = true;
                }
            if(!any)
                return false;
            if(changed && !in_q[o] && qn < (int)(sizeof(q) / sizeof(q[0])))
            {
                in_q[o] = true;
                q[qn++] = o;
            }
        }
    }
    return true;
}

float TileDist(int a, int b)
{
    return fabsf(FL[TileF(a)] - FL[TileF(b)]) * 2.0f
           + fabsf(RL[TileR(a)] - RL[TileR(b)])
           + fabsf((float)(TileD(a) - TileD(b))) * 0.3f;
}

// The line layer. Pin the re-rolled voice (fixed_v, or -1 for none), then
// settle the other sounding voices in `order`, each keeping its own tile if
// that is still legal. out[] is filled for the nodes, -1 for a voice whose
// tile is to be left alone.
//
// Returns how many voices were left alone: 0 for a full settle, -1 when not
// even arc-consistency holds. A state can be unsatisfiable (Delay at minimum
// fits only 3 voices under rule 3), and settling what it can beats painting
// everything with the default tile. Pass must keep rolling for a 0: a partial
// is often the pinned tile's fault, not the state's.
int Repair(const int* nodes, int n, const int* order, int fixed_v,
           int fixed_t, int* out)
{
    EnsureTables();
    for(int attempt = 0; attempt < 4; attempt++)
    {
        static uint8_t dom[kVoices][kTiles];
        for(int i = 0; i < n; i++)
            memcpy(dom[nodes[i]], self_ok, sizeof(self_ok));
        if(fixed_v >= 0)
        {
            if(!dom[fixed_v][fixed_t])
                return -1;
            memset(dom[fixed_v], 0, sizeof(self_ok));
            dom[fixed_v][fixed_t] = 1;
        }
        if(!Ac(dom, nodes, n))
            return -1;

        for(int v = 0; v < kVoices; v++)
            out[v] = -1;
        int skipped = 0;
        // The ensemble budgets, spent as voices are placed. Each candidate
        // must leave room for every voice not yet placed to sit at the floor,
        // so greedy collapse can never dead-end on the budget.
        const float fmul = solve_fm, dmul = DrvMul();
        // Spent against a slightly smaller budget than Diagnose checks: the
        // feedback charge is frozen for the solve but rises with the load the
        // solve itself creates, so a solve filled to the brim reads as broken
        // the moment it lands. 5% covers the charge's step at seven voices.
        float       fleft = FbBudget(n) * 0.95f, dleft = DrBudget(n);
        if(fixed_v >= 0)
        {
            fleft -= FL[TileF(fixed_t)] * fmul;
            dleft -= RL[TileR(fixed_t)] * dmul;
            // A pinned tile that eats the whole budget is the pinned tile's
            // fault, so say so and let the caller roll a different one rather
            // than settle everyone else around it and still be broken.
            const float floor_n = (float)(n - 1);
            if(fleft < FL[0] * fmul * floor_n
               || dleft < RL[0] * dmul * floor_n)
                return -1;
        }
        for(int i = 0; i < n; i++)
        {
            const int node = order[i];
            if(node == fixed_v)
            {
                out[node] = fixed_t;
                continue;
            }
            int rest = 0;
            for(int k = i + 1; k < n; k++)
                if(order[k] != fixed_v)
                    rest++;
            const float fhold = FL[0] * fmul * (float)rest,
                        dhold = RL[0] * dmul * (float)rest;
            // Candidates, cheapest first. The tile it already has wins
            // outright; the rest are ranked by distance plus a little jitter,
            // which is what keeps repeated repairs from being identical.
            const int cur = tile[node];
            int       cand[kTiles];
            float     cost[kTiles];
            int       nc = 0;
            for(int t = 0; t < kTiles; t++)
                if(dom[node][t]
                   && FL[TileF(t)] * fmul + fhold <= fleft
                   && RL[TileR(t)] * dmul + dhold <= dleft)
                {
                    cand[nc] = t;
                    cost[nc] = t == cur
                                   ? -100.0f
                                   : TileDist(t, cur) + Rand() * kWild * 3;
                    nc++;
                }
            for(int a = 1; a < nc; a++)
                for(int b = a; b > 0 && cost[b] < cost[b - 1]; b--)
                {
                    const float c = cost[b];
                    const int   d = cand[b];
                    cost[b]       = cost[b - 1];
                    cand[b]       = cand[b - 1];
                    cost[b - 1]   = c;
                    cand[b - 1]   = d;
                }
            bool done = false;
            for(int c = 0; c < nc && !done; c++)
            {
                static uint8_t snap[kVoices][kTiles];
                memcpy(snap, dom, sizeof(dom));
                memset(dom[node], 0, sizeof(self_ok));
                dom[node][cand[c]] = 1;
                if(Ac(dom, nodes, n))
                {
                    out[node] = cand[c];
                    done      = true;
                    fleft -= FL[TileF(cand[c])] * fmul;
                    dleft -= RL[TileR(cand[c])] * dmul;
                }
                else
                    memcpy(dom, snap, sizeof(dom));
            }
            // Nothing legal for this voice: leave out[node] at -1 so it keeps
            // the tile it has, and carry on settling the others. It still
            // spends what that tile costs, or the voices after it would be
            // handed budget this one is already using.
            if(!done)
            {
                skipped++;
                fleft -= FL[TileF(cur)] * fmul;
                dleft -= RL[TileR(cur)] * dmul;
            }
        }
        // A full settle is worth another roll of the jitter; a partial one is
        // only reported once the attempts are spent.
        if(!skipped || attempt == 3)
            return skipped;
    }
    return 0;
}

// The cable layer: pin the new cable, then take the smallest change to the
// others' cables that still satisfies rule 4.
//
// This was exhaustive, and it was only ever tractable because cables could
// not cross groups: three voices to a group capped it at a few dozen
// leaves. One cluster of seven removes that cap -- 19 options over 6 other
// voices is 47 million leaves, each one a PatchesOK, which hangs outright.
// So it is a widening search instead: leave everything alone if that is
// already legal, then try moving one voice, then re-roll the lot a few
// times. Same preference for the smallest change, bounded work.
bool RepairPatches(const int* act, int n, int fixed_v, int fixed_opt,
                   const int* tiles, int8_t* out)
{
    int8_t trial[kVoices];
    memcpy(trial, patch, sizeof(trial));
    if(fixed_v >= 0)
        trial[fixed_v] = (int8_t)fixed_opt;
    rp_tiles = tiles;

    int others[kVoices], n_others = 0;
    for(int i = 0; i < n; i++)
        if(act[i] != fixed_v)
            others[n_others++] = act[i];

    // Nothing to do, which is the usual case
    if(PatchesOK(trial, act, n, tiles))
    {
        memcpy(out, trial, sizeof(trial));
        return true;
    }

    // One voice moved, cheapest first by construction
    for(int j = 0; j < n_others; j++)
    {
        const int   v    = others[j];
        const int8_t was = trial[v];
        for(int o = 0; o < kOpts; o++)
        {
            if(o && OptTarget(o) == v)
                continue;
            trial[v] = (int8_t)o;
            if(PatchesOK(trial, act, n, tiles))
            {
                memcpy(out, trial, sizeof(trial));
                return true;
            }
        }
        trial[v] = was;
    }

    // Then whole configurations at random, which is what the exhaustive
    // walk would have found eventually
    for(int t = 0; t < 64; t++)
    {
        for(int j = 0; j < n_others; j++)
        {
            const int v = others[j];
            int       o = RandInt(kOpts);
            if(o && OptTarget(o) == v)
                o = 0;
            trial[v] = (int8_t)o;
        }
        if(PatchesOK(trial, act, n, tiles))
        {
            memcpy(out, trial, sizeof(trial));
            return true;
        }
    }
    return false;
}

bool RepairMod(const int* act, int n, int fixed_v, int fixed_opt,
               const int* tl, int8_t* out)
{
    (void)act;
    (void)n;
    (void)tl;
    memcpy(out, mod_opt, sizeof(int8_t) * kVoices);
    if(fixed_v >= 0)
        out[fixed_v] = (int8_t)fixed_opt;
    return true;
}

// ---------------------------------------------------------------- the wave

// A REPLAY, not propagation: the solver already finished in one pass. Order
// is by recency. Do not turn this into a real propagating ripple.
int ApplyWave(const Change* ch, const int* order, int n, int kind,
              float offset, float now)
{
    const float iv = HopSeconds();
    const float t0 = now + offset;
    seq++;
    int changed = 0;
    for(int h = 0; h < n; h++)
    {
        const int v   = order[h];
        Change    e   = NoChange();
        bool      any = false;
        if(ch[v].tile >= 0 && ch[v].tile != tile[v])
        {
            tile[v] = (int16_t)ch[v].tile;
            e.tile  = ch[v].tile;
            any     = true;
        }
        if(ch[v].patch >= 0 && ch[v].patch != patch[v])
        {
            patch[v] = (int8_t)ch[v].patch;
            e.patch  = ch[v].patch;
            any      = true;
        }
        if(ch[v].skew >= 0 && ch[v].skew != (int)skew[v])
        {
            skew[v] = (uint8_t)ch[v].skew;
            e.skew  = ch[v].skew;
            any     = true;
        }
        if(ch[v].mod >= 0 && ch[v].mod != mod_opt[v])
        {
            mod_opt[v] = (int8_t)ch[v].mod;
            e.mod      = ch[v].mod;
            any        = true;
        }
        if(!any)
            continue;
        if(n_events < kMaxEvents)
            events[n_events++] = Event{t0 + (float)h * iv, v, seq, kind, e};
        else
            n_dropped++;
        changed++;
    }
    return changed;
}

// ------------------------------------------------------------- the choosing

// New settings are drawn evenly from whatever the rules currently allow
int RandomTile()
{
    EnsureTables();
    int list[kTiles], n = 0;
    for(int t = 0; t < kTiles; t++)
        if(self_ok[t])
            list[n++] = t;
    if(!n)
        return kDefaultTile;
    return list[RandInt(n)];
}

int RandomPatchOpt(int v, const int* act, int n)
{
    // With cables off the only legal option is none; with them on, asking for
    // one most of the time is what lets rule 6 be satisfied without the search
    // having to climb out of a hole
    if(!cable_mode)
        return 0;
    if(Rand() < 0.2f)
        return 0;
    int others[kVoices], no = 0, live[kVoices], nl = 0;
    for(int o = 0; o < kVoices; o++)
        if(o != v && GroupOf(o) == GroupOf(v))
        {
            others[no++] = o;
            if(InList(act, n, o))
                live[nl++] = o;
        }
    const bool use_live = nl && Rand() < 0.65f;
    const int  pick     = use_live ? live[RandInt(nl)] : others[RandInt(no)];
    // The amount used to be a bare dice roll -- the one thing about a cable
    // nothing on the panel could reach. The knob asks, the jitter keeps the
    // ensemble from landing on one amount, rule 5 still gets to refuse.
    const float want = knob.cable * 2.0f;
    int         amt  = (int)(want * (0.4f + 0.8f * Rand()) + 0.5f);
    amt              = amt < 0 ? 0 : (amt > 2 ? 2 : amt);
    return 1 + pick * 3 + amt;
}

int RandomModOpt()
{
    // The knob asks for depth; rule 5 decides how much of it is allowed. The
    // request used to ignore the knob entirely, so turning the mod up only
    // made the deep options illegal and the solver answered with depth 0 --
    // the knob did the opposite of what it says.
    const float want  = knob.mod * 3.0f;
    int         depth = (int)(want * (0.4f + 0.8f * Rand()) + 0.5f);
    depth             = depth < 0 ? 0 : (depth > 3 ? 3 : depth);
    return RandInt(5) * 4 + depth;
}

}  // namespace

// ---------------------------------------------------------------- the knobs

Knobs&        K() { return knob; }
bool          TookBreak()
{
    const bool was = broke;
    broke          = false;
    return was;
}
void          SetFrozen(bool on) { frozen = on; }
void          SetCableMode(int m) { cable_mode = m; }
int           CableMode() { return cable_mode; }
bool          Frozen() { return frozen; }
const Limits& Lim() { return limits; }

// For the header only: the audio never sees the charge. 1 when not joined
float ChargeMul() { return ChargedFb(0) / FbMul(); }

float DelayBase() { return 0.03f * powf(30.0f, knob.delay); }
float DelayGlide() { return Clamp(DelayBase() * 0.6f, 0.02f, 0.5f); }
// 20 ms is one event to the ear, 800 ms is a procession you can count
float HopSeconds() { return Lerp(0.02f, 0.8f, ripple); }
// Below this a break stays in the group that caused it
constexpr float kSpread = 0.45f;
void  SetRipple(float r) { ripple = Clamp(r, 0.0f, 1.0f); }
void  SetLevel(int v, float l)
{
    if(v >= 0 && v < kVoices)
        level[v] = l;
}
float Ripple() { return ripple; }
// The release outlasts the attack by more than the prototype had it: the
// voices were cutting off before their own echoes.
float ReleaseT() { return Clamp(DelayBase() * 6.5f, 0.45f, 6.5f); }
float AttackT() { return Clamp(DelayBase() * 3.0f, 0.2f, 3.0f) * 0.35f; }
float ModSpeed0() { return Clamp(1.0f / (DelayBase() * 8.0f), 0.08f, 2.5f); }
float FbMul() { return Lerp(0.5f, 1.5f, knob.fb); }
float DrvMul() { return Lerp(0.4f, 1.6f, knob.drive); }
// Raised from 1.5 when the mod rule went: nothing caps depth now, so the
// knob is the only ceiling
float ModMul() { return Lerp(0.0f, 4.0f, knob.mod); }
float CableMul() { return Clamp(knob.cable, 0.0f, 1.0f); }
float LoopCut() { return 300.0f * powf(24.0f, knob.filter); }
float VoiceCutMul() { return Lerp(1.5f, 14.0f, knob.filter); }
// Unrest used to ride on strictness, which made strict-but-restless and
// loose-but-still unreachable. Its own control, so Control says how firmly
// the rules hold and Unrest says how often the instrument moves by itself.
// Control and Unrest ride one fader, tied in the SAME direction: the old
// coupling ran them inversely, which is what made strict-but-restless and
// loose-but-still unreachable. Those are now the two ends. Unrest lags over
// the bottom third so there is still a tight-but-settled region.
void SetAgitation(float a)
{
    knob.safe   = Clamp(a, 0.0f, 1.0f);
    knob.unrest = Smooth(0.33f, 1.0f, knob.safe);
    RebuildRules();
}

float LapseChance() { return Lerp(0.0f, 0.5f, knob.unrest); }
float UnrestRate() { return Lerp(0.0f, 0.25f, knob.unrest); }

const char* BreakName(BreakType t)
{
    static const char* n[kBreakTypes]
        = {"echo", "swell", "fuzz", "flood", "cable"};
    return (int)t < kBreakTypes ? n[t] : "?";
}

void RebuildRules()
{
    const float s   = knob.safe;
    // Halved from the old pair sums so two sounding voices behave exactly as
    // they did; above two, the budget grows with the ensemble instead of
    // pinning every extra voice to the floor.
    limits.Fshare   = (3.2f - 2.0f * s) * 0.5f;
    limits.Dshare   = (3.0f - 1.8f * s) * 0.5f;
    limits.Fself    = s > 0.6f ? 0.95f : 9.0f;
    // Drive's own ceiling, the twin of Fself. Without it the top drive class
    // sits under any ensemble budget on its own, so a fuzz break had nothing
    // to violate once the pair rule went.
    limits.Rself    = s > 0.6f ? 0.7f : 9.0f;
    limits.ratio_on = s > 0.3f;
    limits.cycle_on = s > 0.35f;
    limits.Lpeak    = s > 0.6f ? 0.98f : (s > 0.3f ? 1.2f : 9.0f);
    limits.gap      = Lerp(0.004f, 0.02f, s);
    // The cable rules. Feed is held tighter than mod throughout: what arrives
    // down a feed cable is gain, what arrives down a mod cable is movement.
    limits.Iflood       = Lerp(3.2f, 1.4f, s);
    limits.fan_in       = cable_mode == 1 ? (s > 0.6f ? 1 : 2) : 3;
    limits.connected_on = s > 0.7f;

    tables_dirty = true;
}

namespace
{
void EnsureTables()
{
    if(!tables_dirty)
        return;
    tables_dirty = false;

    const float fm = solve_fm, dm = DrvMul(), base = DelayBase();
    table_fm       = fm;
    for(int t = 0; t < kTiles; t++)
        self_ok[t] = FL[TileF(t)] * fm <= limits.Fself
                             && RL[TileR(t)] * dm <= limits.Rself
                         ? 1
                         : 0;
    // Only the echo rule is left here, and it reads nothing but the delay
    // class, so the table is one entry per PAIR OF CLASSES rather than per
    // pair of tiles: 49 instead of 19,600, on a path arc consistency walks
    // thousands of times per solve. Feedback and drive are ensemble budgets
    // now, spent down as voices collapse, because a budget is n-ary and arc
    // consistency over pairs is blind to it.
    for(int a = 0; a < kDelays; a++)
        for(int b = 0; b < kDelays; b++)
        {
            bool ok = true;
            // Echoes are either the same echo or two you can tell apart --
            // a near miss is the mud. DR is a 1/6 grid, so every pair is
            // already rhythmically related and only audibility is left to
            // check; demanding a SIMPLE ratio of every pair was the old
            // four-voice encoding and no set of more than four can satisfy
            // it. This is why turning Delay down with several voices
            // sounding causes echo breaks.
            if(ok && limits.ratio_on)
            {
                const float d = fabsf(DR[a] - DR[b]);
                ok            = d < 1e-6f || d * base >= limits.gap;
            }
            compat[a * kDelays + b] = ok ? 1 : 0;
        }
}
}  // namespace

// ---------------------------------------------------------------- the state

void Init(uint32_t seed)
{
    // xorshift32 from a small seed returns small numbers for its first few
    // rounds, which biased every early roll towards list[0]: measured over
    // 400 seeds, drive classes 2 and 3 came up once between them. Scatter the
    // seed and warm the state before anything draws from it.
    rng_state = (seed ? seed : 1) * 2654435761u | 1u;
    for(int i = 0; i < 16; i++)
        Next();
    for(int v = 0; v < kVoices; v++)
    {
        tile[v] = shown_tile[v] = kDefaultTile;
        patch[v] = shown_patch[v] = 0;
        mod_opt[v] = shown_mod[v] = 0;
        skew[v] = shown_skew[v] = 0;
        seq_tile[v] = seq_patch[v] = seq_mod[v] = seq_skew[v] = 0;
        sounding[v]                                           = false;
        started_at[v]                                         = 0;
        flash[v]                                              = 0;
    }
    seq       = 0;
    broke     = false;
    n_events  = 0;
    n_dropped = 0;
    for(int g = 0; g < kGroups; g++)
        heal_at[g] = next_lapse[g] = next_unrest[g] = 0;
    knob_settle = 0;
    solve_fm    = -1.0f;
    frozen      = false;
    joined      = false;
    RebuildRules();
}

void SetSounding(int v, bool on, float now)
{
    sounding[v] = on;
    if(on)
        started_at[v] = now;
}

void SetJoined(bool on, float now)
{
    if(on == joined)
        return;
    joined = on;
    // The charge changed, so each group must re-solve against it
    for(int g = 0; g < kGroups; g++)
        heal_at[g] = now;
}
bool  Sounding(int v) { return sounding[v]; }
float StartedAt(int v) { return started_at[v]; }

int EventsDropped() { return n_dropped; }
int Tile(int v) { return tile[v]; }
int Patch(int v) { return patch[v]; }
int Mod(int v) { return mod_opt[v]; }
int Skew(int v) { return skew[v]; }
int ShownTile(int v) { return shown_tile[v]; }
int ShownPatch(int v) { return shown_patch[v]; }
int ShownMod(int v) { return shown_mod[v]; }
int ShownSkew(int v) { return shown_skew[v]; }

int TakeFlash(int v)
{
    const int was = flash[v];
    flash[v]      = 0;
    return was;
}

// ----------------------------------------------------------------- the work

void Pass(int v, float now)
{
    const int g = GroupOf(v);
    SetSolveGroup(g);
    int       act[kVoices + 1];
    int       n = Active(act, g);
    if(!InList(act, n, v))
        act[n++] = v;

    // The pressed voice changes first, then the others, newest first
    int order[kVoices], no = 0;
    order[no++] = v;
    {
        int rest[kVoices], nr = 0;
        for(int i = 0; i < n; i++)
            if(act[i] != v)
                rest[nr++] = act[i];
        Recency(rest, nr);
        for(int i = 0; i < nr; i++)
            order[no++] = rest[i];
    }

    // Keep rolling for a full settle; accept the last partial one if the
    // state is genuinely cornered and no pin can settle everybody.
    int line[kVoices];
    int skipped = -1;
    for(int k = 0; k < 30 && skipped != 0; k++)
        skipped = Repair(act, n, order, v, RandomTile(), line);
    const bool have_line = skipped >= 0;

    // Both the patches and the mod are judged against the tiles the line
    // layer just chose: the flood budget weighs feedback against what is fed
    // in, so it cannot be decided before the feedback is
    int ft[kVoices];
    for(int i = 0; i < kVoices; i++)
        ft[i] = tile[i];
    if(have_line)
        for(int i = 0; i < no; i++)
            if(line[order[i]] >= 0)   // -1 means that voice keeps its tile
                ft[order[i]] = line[order[i]];

    int8_t pat[kVoices];
    bool   have_pat = false;
    for(int k = 0; k < 10 && !have_pat; k++)
        have_pat
            = RepairPatches(act, n, v, RandomPatchOpt(v, act, n), ft, pat);
    if(!have_pat)
        have_pat = RepairPatches(act, n, v, 0, ft, pat);


    int8_t md[kVoices];
    bool   have_mod = false;
    for(int k = 0; k < 12 && !have_mod; k++)
        have_mod = RepairMod(act, n, v, RandomModOpt(), ft, md);
    if(!have_mod)
        have_mod = RepairMod(act, n, v, 0, ft, md);

    Change ch[kVoices];
    for(int i = 0; i < kVoices; i++)
        ch[i] = NoChange();
    for(int i = 0; i < no; i++)
    {
        const int m = order[i];
        if(have_line)
            ch[m].tile = line[m];
        if(have_pat)
            ch[m].patch = pat[m];
        if(have_mod)
            ch[m].mod = md[m];
    }
    ch[v].skew = 0;
    ApplyWave(ch, order, no, 1, 0.0f, now);

    if(Rand() < LapseChance())
        BreakRule(-1, g, (float)(no + 1) * HopSeconds(), now);
}

// Letting go is a gesture too. A released voice keeps ringing on its own
// tile, so give it a fresh one: the tail then fades with a different colour
// than the note sounded with. Delivered as a break (kind 2) so the delay
// crawls to it and the release scrubs.
void RerollTail(int v, float now)
{
    if(v < 0 || v >= kVoices)
        return;
    // HOLD can keep a released voice in the constraint graph, and then its
    // tile is not free: re-roll it the legal way, against its neighbours
    if(sounding[v])
    {
        Pass(v, now);
        return;
    }
    Change ch[kVoices];
    for(int i = 0; i < kVoices; i++)
        ch[i] = NoChange();
    ch[v].tile      = RandomTile();
    ch[v].skew      = 0;
    const int order[1] = {v};
    ApplyWave(ch, order, 1, 2, 0.0f, now);
}

void Heal(int g, float now)
{
    if(g < 0 || g >= kGroups)
        return;
    heal_at[g] = 0;
    SetSolveGroup(g);
    int       act[kVoices];
    const int n = Active(act, g);
    if(!n)
        return;
    int order[kVoices];
    memcpy(order, act, sizeof(int) * n);
    Recency(order, n);

    // Nothing is pinned here, so there is no alternative roll to try: a
    // partial settle is taken as it comes.
    int        line[kVoices];
    int8_t     pat[kVoices], md[kVoices];
    const bool have_line = Repair(act, n, order, -1, -1, line) >= 0;

    int ft[kVoices];
    for(int i = 0; i < kVoices; i++)
        ft[i] = tile[i];
    if(have_line)
        for(int i = 0; i < n; i++)
            if(line[order[i]] >= 0)   // -1 means that voice keeps its tile
                ft[order[i]] = line[order[i]];

    const bool have_pat = RepairPatches(act, n, -1, 0, ft, pat);
    const bool have_mod = RepairMod(act, n, -1, 0, ft, md);

    Change ch[kVoices];
    for(int i = 0; i < kVoices; i++)
        ch[i] = NoChange();
    for(int i = 0; i < n; i++)
    {
        const int m = order[i];
        ch[m].skew  = 0;
        // -1 leaves the tile alone, which is also what a total failure now
        // does: painting every voice with the default tile was the collapse
        // this repair exists to avoid.
        ch[m].tile  = have_line ? line[m] : -1;
        ch[m].patch = have_pat ? pat[m] : 0;
        ch[m].mod   = have_mod ? md[m] : 0;
    }
    ApplyWave(ch, order, n, 1, 0.0f, now);
}

void Heal(float now)
{
    for(int g = 0; g < kGroups; g++)
        Heal(g, now);
}

void BreakRule(int type, int g, float offset, float now, int prefer)
{
    if(g < 0 || g >= kGroups)
        return;
    SetSolveGroup(g);
    int       act[kVoices];
    const int n = Active(act, g);
    if(!n)
        return;
    // Not every break is available: the cable ones need two voices to cross
    // and a mode to cross in, and flooding is a feed-only idea.
    int avail[kBreakTypes], na = 0;
    for(int t = 0; t < kBreakTypes; t++)
    {
        if((t == kCable || t == kFlood) && (n < 2 || !cable_mode))
            continue;
        if(t == kFlood && cable_mode != 1)
            continue;
        avail[na++] = t;
    }
    bool ok = false;
    for(int i = 0; i < na; i++)
        if(avail[i] == type)
            ok = true;
    if(!ok)
        type = na ? avail[RandInt(na)] : kSwell;

    int order[kVoices];
    memcpy(order, act, sizeof(int) * n);
    Recency(order, n);

    // Whoever asked for the break names the voice it should land on -- a
    // leaned-on pad breaks itself. Failing that, weight the choice by how
    // loudly each voice is actually sounding: measured over 8 seeds, a flat
    // random pick left the weakest break changing nothing audible at all,
    // because it landed on a voice that was barely there.
    // ONE draw, reused as both the flat index and the weighted position:
    // an extra Rand() here shifts the whole stream, which moved every
    // measured number by 40% and made before-and-after meaningless.
    const float u = Rand();
    int         a = act[(int)(u * (float)n) % n];
    if(prefer >= 0 && InList(act, n, prefer))
        a = prefer;
    else
    {
        float tot = 0.0f;
        for(int i = 0; i < n; i++)
            tot += level[act[i]];
        if(tot > 1e-6f)
        {
            float r = u * tot;
            for(int i = 0; i < n; i++)
            {
                r -= level[act[i]];
                if(r <= 0.0f)
                {
                    a = act[i];
                    break;
                }
            }
        }
    }
    int       b = -1;
    for(int i = 0; i < n; i++)
        if(act[i] != a)
        {
            b = act[i];
            break;
        }

    Change ch[kVoices];
    for(int i = 0; i < kVoices; i++)
        ch[i] = NoChange();
    const int ta = tile[a], tb = b >= 0 ? tile[b] : 0;
    switch(type)
    {
        case kEcho:   // two echoes a few milliseconds apart, flamming
            ch[a].skew = 1;
            ch[a].tile
                = MkTile(TileD(ta), TileF(ta) > 2 ? TileF(ta) : 2, TileR(ta));
            if(b >= 0)
                ch[b].tile = MkTile(TileD(ta), TileF(tb), TileR(tb));
            break;
        case kSwell:   // feedback loops that ring by themselves
            ch[a].tile = MkTile(TileD(ta), 4, TileR(ta) < 1 ? TileR(ta) : 1);
            if(b >= 0)
                ch[b].tile
                    = MkTile(TileD(tb), 3, TileR(tb) < 1 ? TileR(tb) : 1);
            break;
        case kFuzz:   // drive overload
            ch[a].tile = MkTile(TileD(ta), TileF(ta) < 2 ? TileF(ta) : 2, 3);
            if(b >= 0)
                ch[b].tile
                    = MkTile(TileD(tb), TileF(tb) < 2 ? TileF(tb) : 2, 3);
            break;
        case kFlood:   // fed hard while already ringing, feed only
            ch[b].patch = 1 + a * 3 + 2;
            ch[a].tile  = MkTile(TileD(ta), 4, TileR(ta) < 1 ? TileR(ta) : 1);
            break;
        default:   // two voices patched into each other, hardest
            ch[a].patch = 1 + b * 3 + 2;
            ch[b].patch = 1 + a * 3 + 2;
            break;
    }
    // A break that lands on the tile the voice already holds changes
    // nothing and is never delivered -- kFuzz on a tile that is already
    // driven, kSwell on one already ringing. The player leans on the pad
    // and hears silence, so force a real move instead.
    if(ApplyWave(ch, order, n, 2, offset, now) == 0)
    {
        for(int i = 0; i < kVoices; i++)
            ch[i] = NoChange();
        int t = tile[a];
        for(int i = 0; i < 8 && t == tile[a]; i++)
            t = RandomTile();
        ch[a].tile = t;
        ApplyWave(ch, order, n, 2, offset, now);
    }
    // WFC comes back for it, but not straight away
    heal_at[g] = now + offset + 3.0f + Rand() * 4.0f;

    // Past half way the fader carries the break into the other group, one
    // wave later, so the whole instrument chain-reacts instead of one
    // cluster twitching. The guard stops the two groups breaking each
    // other round and round.
    static bool spreading = false;
    if(ripple >= kSpread && !spreading && kGroups > 1)
    {
        spreading = true;
        const float after = offset + (float)n * HopSeconds();
        BreakRule(-1, g ^ 1, after, now, -1);
        spreading = false;
    }
}

void BreakRule(int type, float offset, float now)
{
    for(int g = 0; g < kGroups; g++)
        BreakRule(type, g, offset, now, -1);
}

void KnobsMoved(float now)
{
    // The player hears the strain, then the repair
    knob_settle = now + 0.35f;
}

void Tick(float now)
{
    // Deliver what is due. An older event never overwrites a newer one.
    int keep = 0;
    for(int i = 0; i < n_events; i++)
    {
        const Event e = events[i];
        if(e.t > now)
        {
            events[keep++] = e;
            continue;
        }
        if(e.ch.tile >= 0 && e.seq >= seq_tile[e.v])
        {
            seq_tile[e.v]   = e.seq;
            shown_tile[e.v] = (int16_t)e.ch.tile;
        }
        if(e.ch.patch >= 0 && e.seq >= seq_patch[e.v])
        {
            seq_patch[e.v]   = e.seq;
            shown_patch[e.v] = (int8_t)e.ch.patch;
        }
        if(e.ch.skew >= 0 && e.seq >= seq_skew[e.v])
        {
            seq_skew[e.v]   = e.seq;
            shown_skew[e.v] = (uint8_t)e.ch.skew;
        }
        if(e.ch.mod >= 0 && e.seq >= seq_mod[e.v])
        {
            seq_mod[e.v]   = e.seq;
            shown_mod[e.v] = (int8_t)e.ch.mod;
        }
        if(e.kind > flash[e.v])
            flash[e.v] = e.kind;
        if(e.kind == 2)
            broke = true;
    }
    n_events = keep;

    if(knob_settle && now > knob_settle)
    {
        knob_settle = 0;
        for(int g = 0; g < kGroups; g++)
        {
            int act[kVoices];
            if(Active(act, g))
                Heal(g, now);
        }
    }
    if(frozen)
        return;
    for(int g = 0; g < kGroups; g++)
    {
        int       act[kVoices];
        const int n = Active(act, g);
        if(heal_at[g] && now > heal_at[g])
            Heal(g, now);

        // Now and then WFC lets a rule go with nobody pressing
        if(n && LapseChance() > 0.01f && now > next_lapse[g])
        {
            if(next_lapse[g] > 0 && Rand() < 0.6f)
                BreakRule(-1, g, 0.0f, now);
            next_lapse[g]
                = now + (6.0f + Rand() * 10.0f) / (0.15f + LapseChance() * 4);
        }
        // A loose WFC re-rolls sounding voices by itself
        if(UnrestRate() > 0.01f && n && now > next_unrest[g])
        {
            if(next_unrest[g] > 0)
                Pass(act[RandInt(n)], now);
            next_unrest[g] = now + (1.0f / UnrestRate()) * (0.5f + Rand());
        }
    }
}

// ----------------------------------------------------------- what is broken

int Diagnose(Violation* out, int max, int group)
{
    if(group < 0)
    {
        int cnt = 0;
        for(int g = 0; g < kGroups; g++)
            cnt += Diagnose(out + cnt, max - cnt, g);
        return cnt;
    }
    int         act[kVoices];
    const int   n   = Active(act, group);
    int         cnt = 0;
    const float fm = ChargedFb(group), dm = DrvMul(), base = DelayBase();
    float       in_amt[kVoices] = {};
    int         in_deg[kVoices] = {};
    InAmounts(shown_patch, act, n, in_amt, in_deg);

    for(int i = 0; i < n && cnt < max; i++)
    {
        const int a = act[i], t = shown_tile[a];
        if(FL[TileF(t)] * fm > limits.Fself)
            out[cnt++] = Violation{kSwell, {a, -1}};
        if(RL[TileR(t)] * dm > limits.Rself && cnt < max)
            out[cnt++] = Violation{kFuzz, {a, -1}};
        // Rule 7: fed and ringing at once, which only feed can be
        if(cable_mode == 1 && cnt < max
           && (FL[TileF(t)] * fm + in_amt[a] > limits.Iflood
               || in_deg[a] > limits.fan_in))
            out[cnt++] = Violation{kFlood, {a, -1}};
    }
    // The ensemble budgets. Blamed on the two biggest spenders, so a heal
    // has somewhere obvious to take the energy from.
    {
        float fsum = 0.0f, dsum = 0.0f;
        int   f1 = -1, f2 = -1, d1 = -1, d2 = -1;
        for(int i = 0; i < n; i++)
        {
            const int   a = act[i], t = shown_tile[a];
            const float fv = FL[TileF(t)] * fm, dv = RL[TileR(t)] * dm;
            fsum += fv;
            dsum += dv;
            if(f1 < 0 || fv > FL[TileF(shown_tile[f1])] * fm)
            {
                f2 = f1;
                f1 = a;
            }
            else if(f2 < 0 || fv > FL[TileF(shown_tile[f2])] * fm)
                f2 = a;
            if(d1 < 0 || dv > RL[TileR(shown_tile[d1])] * dm)
            {
                d2 = d1;
                d1 = a;
            }
            else if(d2 < 0 || dv > RL[TileR(shown_tile[d2])] * dm)
                d2 = a;
        }
        if(n && fsum > FbBudget(n) && cnt < max)
            out[cnt++] = Violation{kSwell, {f1, f2, -1}};
        if(n && dsum > DrBudget(n) && cnt < max)
            out[cnt++] = Violation{kFuzz, {d1, d2, -1}};
    }
    for(int i = 0; i < n; i++)
        for(int j = i + 1; j < n && cnt < max; j++)
        {
            const int a = act[i], b = act[j];
            const float dd = fabsf(ShownDly(a) - ShownDly(b));
            if(limits.ratio_on && cnt < max && dd >= 1e-6f
               && dd * base < limits.gap)
                out[cnt++] = Violation{kEcho, {a, b, -1}};
        }
    if(limits.cycle_on)
        for(int i = 0; i < n && cnt < max; i++)
        {
            const int v = act[i];
            int       path[kVoices + 1], np = 0, lo = v;
            path[np++] = v;
            int cur    = v;
            for(int s = 0; s < kVoices; s++)
            {
                const int o = shown_patch[cur];
                if(!o)
                    break;
                const int t = OptTarget(o);
                if(!InList(act, n, t))
                    break;
                if(t == v)
                {
                    // Reported once per cycle, by its lowest-numbered voice
                    if(v == lo)
                    {
                        Violation x;
                        x.type = kCable;
                        for(int p = 0; p < np; p++)
                            x.v[p] = path[p];
                        x.v[np]    = -1;
                        out[cnt++] = x;
                    }
                    break;
                }
                path[np++] = t;
                if(t < lo)
                    lo = t;
                cur = t;
            }
        }
    return cnt;
}

}  // namespace wfc
