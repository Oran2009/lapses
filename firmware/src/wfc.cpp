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

Knobs  knob   = {0.6f, 0.5f, 0.3f, 0.55f, 0.5f, 0.7f, 0.2f, 0.0f};
bool   frozen = false;
int    cable_mode = 0;   // 0 none, 1 feed, 2 mod
const int* rp_tiles = nullptr;   // the tiles the patch search weighs
float      rm_in[kVoices] = {};  // cable depth arriving, per voice
Limits limits = {};

// The rule tables, rebuilt on every knob move: which tiles may sit beside
// which (pair rules 1 to 3), and which a voice may hold at all (rule 1)
uint8_t compat[kTiles * kTiles];
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
    const float load = fmaxf(gl[0], gl[1]);
    // Capped so the quietest tile always passes every per-tile rule: an empty
    // table makes RandomTile() fall back to kDefaultTile, which is louder.
    const float cap = 0.98f
                      * fminf(fminf(limits.Fself / FL[0],
                                    limits.Fpair / (2 * FL[0])),
                              limits.Lpeak / FL[0]);
    return fmaxf(fm, fminf(fm * (1.0f + kChargeGain * load / kGroupSize), cap));
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

bool OkRatio(float a, float b)
{
    const float r       = (a > b ? a : b) / (a > b ? b : a);
    const float want[4] = {4.0f / 3, 3.0f / 2, 2.0f, 3.0f};
    for(int i = 0; i < 4; i++)
        if(fabsf(r - want[i]) < 0.01f)
            return true;
    return false;
}

// `in_amt` is the cable depth arriving at this voice, which only counts in
// cross-modulation: there it is spent on the same tap and the same feedback
// the voice modulates itself with, so it belongs in the same peak.
float ModPeak(int t, int o, float in_amt, float fm)
{
    const float dp = DP[ModDepthOf(o)] * ModMul()
                     + (cable_mode == 2 ? 0.3f * in_amt : 0.0f);
    return (FL[TileF(t)] * fm + 0.2f * dp) * (1.0f + 0.35f * dp);
}

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

// What the mod layer must weigh: the cable depth arriving at each voice,
// taken from the patches the layer before it settled on.
void SetModIn(const int8_t* pa, const int* act, int n)
{
    int in_deg[kVoices];
    InAmounts(pa, act, n, rm_in, in_deg);
}

// The cable rules. Rule 4 is the old one -- no self patch ever, and no cycle
// when strict -- but the cycle half is FEED ONLY: audio going round a ring
// multiplies gain on top of each voice's own feedback, while modulation going
// round a ring is a pair of coupled oscillators and is the whole attraction.
//
// Rule 6 is the cable cover: nobody sounding is left out of the patching, and
// at high strictness they must all be in ONE structure rather than two.
//
// Rule 7 is the per-voice budget, and it is where the modes diverge. In feed
// it is a gain budget -- a voice may not be both self-ringing and heavily fed
// (break type `flood`). In mod it is spent inside ModPeak instead, so a voice
// modulated hard from outside counts against the same peak limit as one
// modulating itself (break type `wobble`).
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
        uint8_t allow[kTiles];
        memset(allow, 0, sizeof(allow));
        for(int t = 0; t < kTiles; t++)
            if(dom[c][t])
            {
                const uint8_t* row = compat + t * kTiles;
                for(int u = 0; u < kTiles; u++)
                    if(row[u])
                        allow[u] = 1;
            }
        for(int i = 0; i < n; i++)
        {
            const int o = nodes[i];
            if(o == c)
                continue;
            bool changed = false, any = false;
            for(int u = 0; u < kTiles; u++)
                if(dom[o][u])
                {
                    if(!allow[u])
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
        for(int i = 0; i < n; i++)
        {
            const int node = order[i];
            if(node == fixed_v)
            {
                out[node] = fixed_t;
                continue;
            }
            // Candidates, cheapest first. The tile it already has wins
            // outright; the rest are ranked by distance plus a little jitter,
            // which is what keeps repeated repairs from being identical.
            const int cur = tile[node];
            int       cand[kTiles];
            float     cost[kTiles];
            int       nc = 0;
            for(int t = 0; t < kTiles; t++)
                if(dom[node][t])
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
                }
                else
                    memcpy(dom, snap, sizeof(dom));
            }
            // Nothing legal for this voice: leave out[node] at -1 so it keeps
            // the tile it has, and carry on settling the others
            if(!done)
                skipped++;
        }
        // A full settle is worth another roll of the jitter; a partial one is
        // only reported once the attempts are spent.
        if(!skipped || attempt == 3)
            return skipped;
    }
    return 0;
}

// The cable layer: pin the new cable, then take the smallest change to the
// others' cables that still satisfies rule 4. Exhaustive over 13^3 at most,
// so the recursion carries its state in statics rather than a closure.
bool       rp_found;
float      rp_best_cost;
int8_t     rp_best[kVoices], rp_trial[kVoices];
const int* rp_act;
int        rp_n;
int        rp_others[kVoices], rp_n_others;

void RpRec(int i)
{
    if(i == rp_n_others)
    {
        if(!PatchesOK(rp_trial, rp_act, rp_n, rp_tiles))
            return;
        float c = Rand() * kWild * 2;
        for(int j = 0; j < rp_n_others; j++)
            if(rp_trial[rp_others[j]] != patch[rp_others[j]])
                c += 10.0f;
        if(c < rp_best_cost)
        {
            rp_best_cost = c;
            memcpy(rp_best, rp_trial, sizeof(rp_trial));
            rp_found = true;
        }
        return;
    }
    const int n = rp_others[i];
    for(int o = 0; o < kOpts; o++)
    {
        // Cables never cross groups
        if(o && (OptTarget(o) == n || GroupOf(OptTarget(o)) != GroupOf(n)))
            continue;
        rp_trial[n] = (int8_t)o;
        RpRec(i + 1);
    }
    rp_trial[n] = patch[n];
}

bool RepairPatches(const int* act, int n, int fixed_v, int fixed_opt,
                   const int* tiles, int8_t* out)
{
    rp_tiles    = tiles;
    rp_n_others = 0;
    for(int i = 0; i < n; i++)
        if(act[i] != fixed_v)
            rp_others[rp_n_others++] = act[i];
    memcpy(rp_trial, patch, sizeof(rp_trial));
    if(fixed_v >= 0)
        rp_trial[fixed_v] = (int8_t)fixed_opt;
    rp_act       = act;
    rp_n         = n;
    rp_found     = false;
    rp_best_cost = 1e30f;
    RpRec(0);
    if(rp_found)
        memcpy(out, rp_best, sizeof(rp_best));
    return rp_found;
}

// The mod layer, judged against the tiles the line layer just chose. Rule 5
// prunes before the search, so 20^4 is never actually walked.
bool       rm_found;
float      rm_best_cost;
int8_t     rm_best[kVoices], rm_trial[kVoices];
int        rm_others[kVoices], rm_n_others;
const int* rm_tiles;

void RmRec(int i)
{
    if(i == rm_n_others)
    {
        float c = Rand() * kWild * 2;
        for(int j = 0; j < rm_n_others; j++)
            if(rm_trial[rm_others[j]] != mod_opt[rm_others[j]])
                c += 10.0f;
        if(c < rm_best_cost)
        {
            rm_best_cost = c;
            memcpy(rm_best, rm_trial, sizeof(rm_trial));
            rm_found = true;
        }
        return;
    }
    const int n = rm_others[i];
    for(int o = 0; o < kModOpts; o++)
    {
        if(ModPeak(rm_tiles[n], o, rm_in[n], solve_fm) > limits.Lpeak)
            continue;
        rm_trial[n] = (int8_t)o;
        RmRec(i + 1);
    }
    rm_trial[n] = mod_opt[n];
}

bool RepairMod(const int* act, int n, int fixed_v, int fixed_opt,
               const int* tl, int8_t* out)
{
    rm_n_others = 0;
    for(int i = 0; i < n; i++)
        if(act[i] != fixed_v)
            rm_others[rm_n_others++] = act[i];
    memcpy(rm_trial, mod_opt, sizeof(rm_trial));
    if(fixed_v >= 0)
    {
        if(ModPeak(tl[fixed_v], fixed_opt, rm_in[fixed_v], solve_fm) > limits.Lpeak)
            return false;
        rm_trial[fixed_v] = (int8_t)fixed_opt;
    }
    rm_tiles     = tl;
    rm_found     = false;
    rm_best_cost = 1e30f;
    RmRec(0);
    if(rm_found)
        memcpy(out, rm_best, sizeof(rm_best));
    return rm_found;
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
    return 1 + pick * 3 + RandInt(3);
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
void          SetFrozen(bool on) { frozen = on; }
void          SetCableMode(int m) { cable_mode = m; }
int           CableMode() { return cable_mode; }
bool          Frozen() { return frozen; }
const Limits& Lim() { return limits; }

// For the header only: the audio never sees the charge. 1 when not joined
float ChargeMul() { return ChargedFb(0) / FbMul(); }

float DelayBase() { return 0.03f * powf(30.0f, knob.delay); }
float DelayGlide() { return Clamp(DelayBase() * 0.6f, 0.02f, 0.5f); }
float HopSeconds() { return Clamp(DelayBase() * 0.8f, 0.03f, 1.0f); }
// The release outlasts the attack by more than the prototype had it: the
// voices were cutting off before their own echoes.
float ReleaseT() { return Clamp(DelayBase() * 6.5f, 0.45f, 6.5f); }
float AttackT() { return Clamp(DelayBase() * 3.0f, 0.2f, 3.0f) * 0.35f; }
float ModSpeed0() { return Clamp(1.0f / (DelayBase() * 8.0f), 0.08f, 2.5f); }
float FbMul() { return Lerp(0.5f, 1.5f, knob.fb); }
float DrvMul() { return Lerp(0.4f, 1.6f, knob.drive); }
float ModMul() { return Lerp(0.0f, 1.5f, knob.mod); }
float LoopCut() { return 300.0f * powf(24.0f, knob.filter); }
float VoiceCutMul() { return Lerp(1.5f, 14.0f, knob.filter); }
// Unrest used to ride on strictness, which made strict-but-restless and
// loose-but-still unreachable. Its own control, so Control says how firmly
// the rules hold and Unrest says how often the instrument moves by itself.
float LapseChance() { return Lerp(0.0f, 0.5f, knob.unrest); }
float UnrestRate() { return Lerp(0.0f, 0.25f, knob.unrest); }

const char* BreakName(BreakType t)
{
    static const char* n[kBreakTypes]
        = {"echo", "swell", "fuzz", "wobble", "flood", "cable"};
    return (int)t < kBreakTypes ? n[t] : "?";
}

void RebuildRules()
{
    const float s   = knob.safe;
    limits.Fpair    = 3.2f - 2.0f * s;
    limits.Dpair    = 3.0f - 1.8f * s;
    limits.Fself    = s > 0.6f ? 0.95f : 9.0f;
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
        self_ok[t] = FL[TileF(t)] * fm <= limits.Fself ? 1 : 0;
    for(int a = 0; a < kTiles; a++)
        for(int b = 0; b < kTiles; b++)
        {
            bool ok = (FL[TileF(a)] + FL[TileF(b)]) * fm <= limits.Fpair
                      && (RL[TileR(a)] + RL[TileR(b)]) * dm <= limits.Dpair;
            // Echoes must be a simple ratio apart, and far enough apart in
            // time to be heard as separate. The second half is why turning
            // Delay down causes echo breaks with several voices sounding.
            if(ok && limits.ratio_on)
                ok = TileD(a) != TileD(b)
                     && OkRatio(DR[TileD(a)], DR[TileD(b)])
                     && fabsf(DR[TileD(a)] - DR[TileD(b)]) * base
                            >= limits.gap;
            compat[a * kTiles + b] = ok ? 1 : 0;
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

    SetModIn(have_pat ? pat : patch, act, n);

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
    SetModIn(have_pat ? pat : patch, act, n);
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

void BreakRule(int type, int g, float offset, float now)
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

    const int a = act[RandInt(n)];
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
        case kEcho:   // the delay off its pitch, clashing with a neighbour
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
        case kWobble:   // full depth, same rate, on an already hot loop
            ch[a].mod = 7;
            ch[a].tile
                = MkTile(TileD(ta), TileF(ta) > 3 ? TileF(ta) : 3, TileR(ta));
            if(b >= 0)
            {
                ch[b].mod = 7;
                ch[b].tile = MkTile(TileD(tb), TileF(tb) > 3 ? TileF(tb) : 3,
                                    TileR(tb));
            }
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
    ApplyWave(ch, order, n, 2, offset, now);
    // WFC comes back for it, but not straight away
    heal_at[g] = now + offset + 3.0f + Rand() * 4.0f;
}

void BreakRule(int type, float offset, float now)
{
    for(int g = 0; g < kGroups; g++)
        BreakRule(type, g, offset, now);
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
        if(limits.ratio_on && shown_skew[a] && cnt < max)
            out[cnt++] = Violation{kEcho, {a, -1}};
        if(cable_mode
           && (DP[ModDepthOf(shown_mod[a])] > 0 || in_amt[a] > 0.0f)
           && ModPeak(t, shown_mod[a], in_amt[a], fm) > limits.Lpeak && cnt < max)
            out[cnt++] = Violation{kWobble, {a, -1}};
        else if(!cable_mode && DP[ModDepthOf(shown_mod[a])] > 0
                && ModPeak(t, shown_mod[a], 0.0f, fm) > limits.Lpeak && cnt < max)
            out[cnt++] = Violation{kWobble, {a, -1}};
        // Rule 7: fed and ringing at once, which only feed can be
        if(cable_mode == 1 && cnt < max
           && (FL[TileF(t)] * fm + in_amt[a] > limits.Iflood
               || in_deg[a] > limits.fan_in))
            out[cnt++] = Violation{kFlood, {a, -1}};
    }
    for(int i = 0; i < n; i++)
        for(int j = i + 1; j < n && cnt < max; j++)
        {
            const int a = act[i], b = act[j];
            const int ta = shown_tile[a], tb = shown_tile[b];
            if((FL[TileF(ta)] + FL[TileF(tb)]) * fm > limits.Fpair)
                out[cnt++] = Violation{kSwell, {a, b, -1}};
            if((RL[TileR(ta)] + RL[TileR(tb)]) * dm > limits.Dpair
               && cnt < max)
                out[cnt++] = Violation{kFuzz, {a, b, -1}};
            if(limits.ratio_on && cnt < max
               && (TileD(ta) == TileD(tb)
                   || !OkRatio(DR[TileD(ta)], DR[TileD(tb)])
                   || fabsf(DR[TileD(ta)] - DR[TileD(tb)]) * base
                          < limits.gap))
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
