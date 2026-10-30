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

Knobs  knob   = {0.6f, 0.5f, 0.3f, 0.55f, 0.5f, 0.7f};
Limits limits = {};

// The rule tables, rebuilt on every knob move: which tiles may sit beside
// which (pair rules 1 to 3), and which a voice may hold at all (rule 1)
uint8_t compat[kTiles * kTiles];
uint8_t self_ok[kTiles];

// Logical: what WFC has decided. Shown: what the audio has reached.
int16_t tile[kVoices], shown_tile[kVoices];
int8_t  patch[kVoices], shown_patch[kVoices];
int8_t  lfo_opt[kVoices], shown_lfo[kVoices];
uint8_t skew[kVoices], shown_skew[kVoices];
// Per field, so an older event in the wave never overwrites a newer one
int32_t seq_tile[kVoices], seq_patch[kVoices], seq_lfo[kVoices],
    seq_skew[kVoices];
int32_t seq = 0;

bool  sounding[kVoices];
float started_at[kVoices];
int   flash[kVoices];

// A change the wave carries. -1 means the field is not part of it.
struct Change
{
    int tile, patch, lfo, skew;
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
// ponytail: a fixed queue. Four voices with a break landing on top of a
// repair is 8; 16 is two of those. Grow it if events are ever dropped.
constexpr int kMaxEvents = 16;
Event         events[kMaxEvents];
int           n_events = 0;

float heal_at = 0, next_lapse = 0, next_unrest = 0, knob_settle = 0;

// ------------------------------------------------------------- the sounding

int Active(int* act)
{
    int n = 0;
    for(int v = 0; v < kVoices; v++)
        if(sounding[v])
            act[n++] = v;
    return n;
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

float LfoPeak(int t, int o)
{
    const float dp = DP[LfoDepth(o)] * LfoMul();
    return (FL[TileF(t)] * FbMul() + 0.2f * dp) * (1.0f + 0.35f * dp);
}

// Rule 4: following cables between sounding voices must not return to the
// start, and no voice may patch into itself
bool PatchesOK(const int8_t* pa, const int* act, int n)
{
    for(int i = 0; i < n; i++)
    {
        const int o = pa[act[i]];
        if(o && OptTarget(o) == act[i])
            return false;
    }
    if(!limits.cycle_on)
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
        if(!PatchesOK(rp_trial, rp_act, rp_n))
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
        if(o && OptTarget(o) == n)
            continue;
        rp_trial[n] = (int8_t)o;
        RpRec(i + 1);
    }
    rp_trial[n] = patch[n];
}

bool RepairPatches(const int* act, int n, int fixed_v, int fixed_opt,
                   int8_t* out)
{
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

// The LFO layer, judged against the tiles the line layer just chose. Rule 5
// prunes before the search, so 20^4 is never actually walked.
bool       rl_found;
float      rl_best_cost;
int8_t     rl_best[kVoices], rl_trial[kVoices];
int        rl_others[kVoices], rl_n_others;
const int* rl_tiles;

void RlRec(int i)
{
    if(i == rl_n_others)
    {
        float c = Rand() * kWild * 2;
        for(int j = 0; j < rl_n_others; j++)
            if(rl_trial[rl_others[j]] != lfo_opt[rl_others[j]])
                c += 10.0f;
        if(c < rl_best_cost)
        {
            rl_best_cost = c;
            memcpy(rl_best, rl_trial, sizeof(rl_trial));
            rl_found = true;
        }
        return;
    }
    const int n = rl_others[i];
    for(int o = 0; o < kLfoOpts; o++)
    {
        if(LfoPeak(rl_tiles[n], o) > limits.Lpeak)
            continue;
        rl_trial[n] = (int8_t)o;
        RlRec(i + 1);
    }
    rl_trial[n] = lfo_opt[n];
}

bool RepairLfo(const int* act, int n, int fixed_v, int fixed_opt,
               const int* tl, int8_t* out)
{
    rl_n_others = 0;
    for(int i = 0; i < n; i++)
        if(act[i] != fixed_v)
            rl_others[rl_n_others++] = act[i];
    memcpy(rl_trial, lfo_opt, sizeof(rl_trial));
    if(fixed_v >= 0)
    {
        if(LfoPeak(tl[fixed_v], fixed_opt) > limits.Lpeak)
            return false;
        rl_trial[fixed_v] = (int8_t)fixed_opt;
    }
    rl_tiles     = tl;
    rl_found     = false;
    rl_best_cost = 1e30f;
    RlRec(0);
    if(rl_found)
        memcpy(out, rl_best, sizeof(rl_best));
    return rl_found;
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
        if(ch[v].lfo >= 0 && ch[v].lfo != lfo_opt[v])
        {
            lfo_opt[v] = (int8_t)ch[v].lfo;
            e.lfo      = ch[v].lfo;
            any        = true;
        }
        if(!any)
            continue;
        if(n_events < kMaxEvents)
            events[n_events++] = Event{t0 + (float)h * iv, v, seq, kind, e};
        changed++;
    }
    return changed;
}

// ------------------------------------------------------------- the choosing

// New settings are drawn evenly from whatever the rules currently allow
int RandomTile()
{
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
    if(Rand() < 0.55f)
        return 0;
    int others[kVoices], no = 0, live[kVoices], nl = 0;
    for(int o = 0; o < kVoices; o++)
        if(o != v)
        {
            others[no++] = o;
            if(InList(act, n, o))
                live[nl++] = o;
        }
    const bool use_live = nl && Rand() < 0.65f;
    const int  pick     = use_live ? live[RandInt(nl)] : others[RandInt(no)];
    return 1 + pick * 3 + RandInt(3);
}

int RandomLfoOpt()
{
    const int depth = Rand() < 0.25f ? 0 : 1 + RandInt(3);
    return RandInt(5) * 4 + depth;
}

}  // namespace

// ---------------------------------------------------------------- the knobs

Knobs&        K() { return knob; }
const Limits& Lim() { return limits; }

float DelayBase() { return 0.03f * powf(30.0f, knob.delay); }
float DelayGlide() { return Clamp(DelayBase() * 0.6f, 0.02f, 0.5f); }
float HopSeconds() { return Clamp(DelayBase() * 0.8f, 0.03f, 1.0f); }
// The release outlasts the attack by more than the prototype had it: the
// voices were cutting off before their own echoes.
float ReleaseT() { return Clamp(DelayBase() * 6.5f, 0.45f, 6.5f); }
float AttackT() { return Clamp(DelayBase() * 3.0f, 0.2f, 3.0f) * 0.35f; }
float LfoRate0() { return Clamp(1.0f / (DelayBase() * 8.0f), 0.08f, 2.5f); }
float FbMul() { return Lerp(0.5f, 1.5f, knob.fb); }
float DrvMul() { return Lerp(0.4f, 1.6f, knob.drive); }
float LfoMul() { return Lerp(0.0f, 1.5f, knob.lfo); }
float LoopCut() { return 300.0f * powf(24.0f, knob.filter); }
float VoiceCutMul() { return Lerp(1.5f, 14.0f, knob.filter); }
float LapseChance()
{
    return Lerp(0.5f, 0.0f, Smooth(0.15f, 0.9f, knob.safe));
}
float UnrestRate() { return Lerp(0.25f, 0.0f, Smooth(0.2f, 0.95f, knob.safe)); }

const char* BreakName(BreakType t)
{
    static const char* n[kBreakTypes]
        = {"echo", "swell", "fuzz", "wobble", "cable"};
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

    const float fm = FbMul(), dm = DrvMul(), base = DelayBase();
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
        lfo_opt[v] = shown_lfo[v] = 0;
        skew[v] = shown_skew[v] = 0;
        seq_tile[v] = seq_patch[v] = seq_lfo[v] = seq_skew[v] = 0;
        sounding[v]                                           = false;
        started_at[v]                                         = 0;
        flash[v]                                              = 0;
    }
    seq      = 0;
    n_events = 0;
    heal_at = next_lapse = next_unrest = knob_settle = 0;
    RebuildRules();
}

void SetSounding(int v, bool on, float now)
{
    sounding[v] = on;
    if(on)
        started_at[v] = now;
}
bool  Sounding(int v) { return sounding[v]; }
float StartedAt(int v) { return started_at[v]; }

int Tile(int v) { return tile[v]; }
int Patch(int v) { return patch[v]; }
int Lfo(int v) { return lfo_opt[v]; }
int Skew(int v) { return skew[v]; }
int ShownTile(int v) { return shown_tile[v]; }
int ShownPatch(int v) { return shown_patch[v]; }
int ShownLfo(int v) { return shown_lfo[v]; }
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
    int act[kVoices + 1];
    int n = Active(act);
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

    int8_t pat[kVoices];
    bool   have_pat = false;
    for(int k = 0; k < 10 && !have_pat; k++)
        have_pat = RepairPatches(act, n, v, RandomPatchOpt(v, act, n), pat);
    if(!have_pat)
        have_pat = RepairPatches(act, n, v, 0, pat);

    // The LFO is judged against the tiles the line layer just chose
    int ft[kVoices];
    for(int i = 0; i < kVoices; i++)
        ft[i] = tile[i];
    if(have_line)
        for(int i = 0; i < no; i++)
            if(line[order[i]] >= 0)   // -1 means that voice keeps its tile
                ft[order[i]] = line[order[i]];

    int8_t lf[kVoices];
    bool   have_lfo = false;
    for(int k = 0; k < 12 && !have_lfo; k++)
        have_lfo = RepairLfo(act, n, v, RandomLfoOpt(), ft, lf);
    if(!have_lfo)
        have_lfo = RepairLfo(act, n, v, 0, ft, lf);

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
        if(have_lfo)
            ch[m].lfo = lf[m];
    }
    ch[v].skew = 0;
    ApplyWave(ch, order, no, 1, 0.0f, now);

    if(Rand() < LapseChance())
        BreakRule(-1, (float)(no + 1) * HopSeconds(), now);
}

void Heal(float now)
{
    heal_at = 0;
    int       act[kVoices];
    const int n = Active(act);
    if(!n)
        return;
    int order[kVoices];
    memcpy(order, act, sizeof(int) * n);
    Recency(order, n);

    // Nothing is pinned here, so there is no alternative roll to try: a
    // partial settle is taken as it comes.
    int        line[kVoices];
    int8_t     pat[kVoices], lf[kVoices];
    const bool have_line = Repair(act, n, order, -1, -1, line) >= 0;
    const bool have_pat  = RepairPatches(act, n, -1, 0, pat);

    int ft[kVoices];
    for(int i = 0; i < kVoices; i++)
        ft[i] = tile[i];
    if(have_line)
        for(int i = 0; i < n; i++)
            if(line[order[i]] >= 0)   // -1 means that voice keeps its tile
                ft[order[i]] = line[order[i]];
    const bool have_lfo = RepairLfo(act, n, -1, 0, ft, lf);

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
        ch[m].lfo   = have_lfo ? lf[m] : 0;
    }
    ApplyWave(ch, order, n, 1, 0.0f, now);
}

void BreakRule(int type, float offset, float now)
{
    int       act[kVoices];
    const int n = Active(act);
    if(!n)
        return;
    // A cable break needs two voices to cross
    const int n_types = n >= 2 ? kBreakTypes : kBreakTypes - 1;
    if(type < 0 || type >= n_types)
        type = RandInt(n_types);

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
            ch[a].lfo = 7;
            ch[a].tile
                = MkTile(TileD(ta), TileF(ta) > 3 ? TileF(ta) : 3, TileR(ta));
            if(b >= 0)
            {
                ch[b].lfo = 7;
                ch[b].tile = MkTile(TileD(tb), TileF(tb) > 3 ? TileF(tb) : 3,
                                    TileR(tb));
            }
            break;
        default:   // two voices patched into each other, hardest
            ch[a].patch = 1 + b * 3 + 2;
            ch[b].patch = 1 + a * 3 + 2;
            break;
    }
    ApplyWave(ch, order, n, 2, offset, now);
    // WFC comes back for it, but not straight away
    heal_at = now + offset + 3.0f + Rand() * 4.0f;
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
        if(e.ch.lfo >= 0 && e.seq >= seq_lfo[e.v])
        {
            seq_lfo[e.v]   = e.seq;
            shown_lfo[e.v] = (int8_t)e.ch.lfo;
        }
        if(e.kind > flash[e.v])
            flash[e.v] = e.kind;
    }
    n_events = keep;

    int       act[kVoices];
    const int n = Active(act);

    if(knob_settle && now > knob_settle)
    {
        knob_settle = 0;
        if(n)
            Heal(now);
    }
    if(heal_at && now > heal_at)
        Heal(now);

    // Now and then WFC lets a rule go with nobody pressing
    if(n && LapseChance() > 0.01f && now > next_lapse)
    {
        if(next_lapse > 0 && Rand() < 0.6f)
            BreakRule(-1, 0.0f, now);
        next_lapse
            = now + (6.0f + Rand() * 10.0f) / (0.15f + LapseChance() * 4);
    }
    // A loose WFC re-rolls sounding voices by itself
    if(UnrestRate() > 0.01f && n && now > next_unrest)
    {
        if(next_unrest > 0)
            Pass(act[RandInt(n)], now);
        next_unrest = now + (1.0f / UnrestRate()) * (0.5f + Rand());
    }
}

// ----------------------------------------------------------- what is broken

int Diagnose(Violation* out, int max)
{
    int         act[kVoices];
    const int   n   = Active(act);
    int         cnt = 0;
    const float fm = FbMul(), dm = DrvMul(), base = DelayBase();

    for(int i = 0; i < n && cnt < max; i++)
    {
        const int a = act[i], t = shown_tile[a];
        if(FL[TileF(t)] * fm > limits.Fself)
            out[cnt++] = Violation{kSwell, {a, -1}};
        if(limits.ratio_on && shown_skew[a] && cnt < max)
            out[cnt++] = Violation{kEcho, {a, -1}};
        if(DP[LfoDepth(shown_lfo[a])] > 0
           && LfoPeak(t, shown_lfo[a]) > limits.Lpeak && cnt < max)
            out[cnt++] = Violation{kWobble, {a, -1}};
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
