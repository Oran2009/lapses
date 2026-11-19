#include "wfc.h"

#include <math.h>
#include <string.h>

namespace wfc
{
namespace
{
// repair wander, the prototype's S.wild
constexpr float kWild = 0.5f;

// fixable seed, for tests
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

Knobs  knob   = {0.6f, 0.5f, 0.3f, 0.55f, 0.5f, 0.7f, 0.2f, 0.0f, 0.0f, 0.0f};
bool   frozen = false;
// wave hop
float  ripple = 0.02f;
// so a break lands on an audible voice
float  level[kVoices] = {};
Limits limits = {};

// rebuilt on every knob move
uint8_t compat[kDelays * kDelays];
uint8_t self_ok[kTiles];
// on read, not on every knob twitch
bool tables_dirty = true;
void EnsureTables();

// logical = decided, shown = reached
int16_t tile[kVoices], shown_tile[kVoices];
int8_t  mod_opt[kVoices], shown_mod[kVoices];
uint8_t skew[kVoices], shown_skew[kVoices];
// so an older event never overwrites a newer
int32_t seq_tile[kVoices], seq_mod[kVoices], seq_skew[kVoices];
int32_t seq = 0;

bool  sounding[kVoices];
double started_at[kVoices];
int   flash[kVoices];
// on delivery, so the LED tracks the sound
bool  broke = false;

// -1 = not in this change
struct Change
{
    int tile, mod, skew;
};
inline Change NoChange() { return Change{-1, -1, -1}; }

struct Event
{
    double  t;
    int     v;
    int32_t seq;
    int     kind;   // 2 = break, 1 = repair
    Change  ch;
};
constexpr int kMaxEvents = 48;
Event         events[kMaxEvents];
int           n_events = 0, n_dropped = 0;

double heal_at[kGroups], next_lapse[kGroups], next_unrest[kGroups];

bool  joined      = false;
float solve_fm    = -1.0f;
float table_fm    = -1.0f;
constexpr float kChargeGain = 4.0f;
double knob_settle = 0;

// ------------------------------------------------------------- the sounding

int Active(int* act, int group)
{
    int n = 0;
    for(int v = 0; v < kVoices; v++)
        if(sounding[v] && (group < 0 || GroupOf(v) == group))
            act[n++] = v;
    return n;
}

// writes nothing, so any thread may call it
float ChargedFb(int group)
{
    const float fm = FbMul();
    if(!joined || group < 0)
        return fm;
    // louder group, own included
    float gl[kGroups] = {};
    for(int v = 0; v < kVoices; v++)
        if(sounding[v])
            gl[GroupOf(v)] += FL[TileF(shown_tile[v])];
    float load = 0.0f;
    for(int g = 0; g < kGroups; g++)
        load = fmaxf(load, gl[g]);
    // an empty table falls back to a louder default
    const float cap = 0.98f
                      * fminf(fminf(limits.Fself / FL[0],
                                    limits.Fshare / FL[0]),
                              limits.Lpeak / FL[0]);
    return fmaxf(fm, fminf(fm * (1.0f + kChargeGain * load / kGroupSize), cap));
}

// multiple of the base time, skew included
float ShownDly(int v)
{
    return DR[TileD(shown_tile[v])] * (shown_skew[v] ? kSkew : 1.0f);
}

// ensemble budgets, floored at the per-voice limit
float FbBudget(int n)
{
    return fmaxf(limits.Fself, limits.Fshare * (float)n);
}
float DrBudget(int n)
{
    return fmaxf(limits.Rself, limits.Dshare * (float)n);
}

// frozen for the solve, read thousands of times
void SetSolveGroup(int g)
{
    solve_fm = ChargedFb(g);
    if(solve_fm != table_fm)
        tables_dirty = true;
}

// newest voice first
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

// --------------------------------------------------------- arc consistency

// false when a domain empties
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

// pin fixed_v (-1 none); returns voices left alone, -1 no arc consistency
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
        // leave the unplaced voices room at the floor
        const float fmul = solve_fm, dmul = DrvMul();
        // under Diagnose: the frozen charge rises with its own load
        float       fleft = FbBudget(n) * 0.95f, dleft = DrBudget(n);
        if(fixed_v >= 0)
        {
            fleft -= FL[TileF(fixed_t)] * fmul;
            dleft -= RL[TileR(fixed_t)] * dmul;
            // a pin that eats the budget fails
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
            // current tile first, then distance plus jitter
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
            // nothing legal: keep the tile
            if(!done)
            {
                skipped++;
                fleft -= FL[TileF(cur)] * fmul;
                dleft -= RL[TileR(cur)] * dmul;
            }
        }
        // another roll only for a full settle
        if(!skipped || attempt == 3)
            return skipped;
    }
    return 0;
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

// a replay, not propagation; newest first
int ApplyWave(const Change* ch, const int* order, int n, int kind,
              float offset, double now)
{
    const float iv = HopSeconds();
    const double t0 = now + offset;
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

int RandomModOpt()
{
    // or turning mod up answers with depth 0
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
bool          Frozen() { return frozen; }
const Limits& Lim() { return limits; }

float ChargeMul() { return ChargedFb(0) / FbMul(); }

float DelayBase() { return 0.03f * powf(30.0f, knob.delay); }
float DelayGlide() { return Clamp(DelayBase() * 0.6f, 0.02f, 0.5f); }
float HopSeconds() { return Lerp(0.02f, 0.8f, ripple); }
void  SetRipple(float r) { ripple = Clamp(r, 0.0f, 1.0f); }
void  SetLevel(int v, float l)
{
    if(v >= 0 && v < kVoices)
        level[v] = l;
}
float Ripple() { return ripple; }
float ReleaseT() { return Clamp(DelayBase() * 6.5f, 0.45f, 6.5f); }
float AttackT() { return 0.243f; }
float ModSpeed0() { return Clamp(1.0f / (DelayBase() * 8.0f), 0.08f, 2.5f); }
float FbMul() { return Lerp(0.5f, 1.5f, knob.fb); }
float DrvMul() { return Lerp(0.4f, 1.6f, knob.drive); }
// the knob is the only ceiling
float ModMul() { return Lerp(0.0f, 4.0f, knob.mod); }
float LoopCut() { return 300.0f * powf(24.0f, knob.filter); }
float VoiceCutMul() { return Lerp(1.5f, 14.0f, knob.filter); }

float LapseChance() { return Lerp(0.0f, 0.5f, knob.unrest); }
float UnrestRate() { return Lerp(0.0f, 0.25f, knob.unrest); }

const char* BreakName(BreakType t)
{
    static const char* n[kBreakTypes]
        = {"echo", "swell", "fuzz"};
    return (int)t < kBreakTypes ? n[t] : "?";
}

void RebuildRules()
{
    const float s   = knob.safe;
    // halved from the old pair sums
    limits.Fshare   = (3.2f - 2.0f * s) * 0.5f;
    limits.Dshare   = (3.0f - 1.8f * s) * 0.5f;
    limits.Fself    = s > 0.6f ? 0.95f : 9.0f;
    // drive's ceiling, twin of Fself
    limits.Rself    = s > 0.6f ? 0.7f : 9.0f;
    limits.ratio_on = s > 0.3f;
    limits.Lpeak    = s > 0.6f ? 0.98f : (s > 0.3f ? 1.2f : 9.0f);
    limits.gap      = Lerp(0.004f, 0.02f, s);

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
    // per class pair, not per tile
    for(int a = 0; a < kDelays; a++)
        for(int b = 0; b < kDelays; b++)
        {
            bool ok = true;
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
    rng_state = (seed ? seed : 1) * 2654435761u | 1u;
    for(int i = 0; i < 16; i++)
        Next();
    for(int v = 0; v < kVoices; v++)
    {
        tile[v] = shown_tile[v] = kDefaultTile;
        mod_opt[v] = shown_mod[v] = 0;
        skew[v] = shown_skew[v] = 0;
        seq_tile[v] = seq_mod[v] = seq_skew[v] = 0;
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

void SetSounding(int v, bool on, double now)
{
    sounding[v] = on;
    if(on)
        started_at[v] = now;
}

void SetJoined(bool on, double now)
{
    if(on == joined)
        return;
    joined = on;
    // charge changed: re-solve
    for(int g = 0; g < kGroups; g++)
        heal_at[g] = now;
}
bool  Sounding(int v) { return sounding[v]; }
double StartedAt(int v) { return started_at[v]; }

int EventsDropped() { return n_dropped; }
int Tile(int v) { return tile[v]; }
int Mod(int v) { return mod_opt[v]; }
int Skew(int v) { return skew[v]; }
int ShownTile(int v) { return shown_tile[v]; }
int ShownMod(int v) { return shown_mod[v]; }
int ShownSkew(int v) { return shown_skew[v]; }

int TakeFlash(int v)
{
    const int was = flash[v];
    flash[v]      = 0;
    return was;
}

// ----------------------------------------------------------------- the work

void Pass(int v, double now)
{
    const int g = GroupOf(v);
    SetSolveGroup(g);
    int       act[kVoices + 1];
    int       n = Active(act, g);
    if(!InList(act, n, v))
        act[n++] = v;

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

    // accept the last partial
    int line[kVoices];
    int skipped = -1;
    for(int k = 0; k < 30 && skipped != 0; k++)
        skipped = Repair(act, n, order, v, RandomTile(), line);
    const bool have_line = skipped >= 0;

    // against the tiles just chosen
    int ft[kVoices];
    for(int i = 0; i < kVoices; i++)
        ft[i] = tile[i];
    if(have_line)
        for(int i = 0; i < no; i++)
            if(line[order[i]] >= 0)   // -1 means that voice keeps its tile
                ft[order[i]] = line[order[i]];

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
        if(have_mod)
            ch[m].mod = md[m];
    }
    ch[v].skew = 0;
    ApplyWave(ch, order, no, 1, 0.0f, now);

    if(Rand() < LapseChance())
        BreakRule(-1, g, (float)(no + 1) * HopSeconds(), now);
}

void RerollTail(int v, double now)
{
    if(v < 0 || v >= kVoices)
        return;
    // HOLD can hold it in the graph
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

void Heal(int g, double now)
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

    int        line[kVoices];
    int8_t     md[kVoices];
    const bool have_line = Repair(act, n, order, -1, -1, line) >= 0;

    int ft[kVoices];
    for(int i = 0; i < kVoices; i++)
        ft[i] = tile[i];
    if(have_line)
        for(int i = 0; i < n; i++)
            if(line[order[i]] >= 0)   // -1 means that voice keeps its tile
                ft[order[i]] = line[order[i]];

    const bool have_mod = RepairMod(act, n, -1, 0, ft, md);

    Change ch[kVoices];
    for(int i = 0; i < kVoices; i++)
        ch[i] = NoChange();
    for(int i = 0; i < n; i++)
    {
        const int m = order[i];
        ch[m].skew  = 0;
        // -1 and failure both keep the tile
        ch[m].tile = have_line ? line[m] : -1;
        ch[m].mod  = have_mod ? md[m] : 0;
    }
    ApplyWave(ch, order, n, 1, 0.0f, now);
}

void Heal(double now)
{
    for(int g = 0; g < kGroups; g++)
        Heal(g, now);
}

void BreakRule(int type, int g, float offset, double now, int prefer)
{
    if(g < 0 || g >= kGroups)
        return;
    SetSolveGroup(g);
    int       act[kVoices];
    const int n = Active(act, g);
    if(!n)
        return;
    if(type < 0 || type >= kBreakTypes)
        type = RandInt(kBreakTypes);

    int order[kVoices];
    memcpy(order, act, sizeof(int) * n);
    Recency(order, n);

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
        default:   // kFuzz, drive overload
            ch[a].tile = MkTile(TileD(ta), TileF(ta) < 2 ? TileF(ta) : 2, 3);
            if(b >= 0)
                ch[b].tile
                    = MkTile(TileD(tb), TileF(tb) < 2 ? TileF(tb) : 2, 3);
            break;
    }
    // 0 = nothing moved: reroll
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
    heal_at[g] = now + offset + 3.0f + Rand() * 4.0f;
}

void BreakRule(int type, float offset, double now)
{
    for(int g = 0; g < kGroups; g++)
        BreakRule(type, g, offset, now, -1);
}

void KnobsMoved(double now)
{
    knob_settle = now + 0.35f;
}

void Tick(double now)
{
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

        if(n && LapseChance() > 0.01f && now > next_lapse[g])
        {
            if(next_lapse[g] > 0 && Rand() < 0.6f)
                BreakRule(-1, g, 0.0f, now);
            next_lapse[g]
                = now + (6.0f + Rand() * 10.0f) / (0.15f + LapseChance() * 4);
        }
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

    for(int i = 0; i < n && cnt < max; i++)
    {
        const int a = act[i], t = shown_tile[a];
        if(FL[TileF(t)] * fm > limits.Fself)
            out[cnt++] = Violation{kSwell, {a, -1}};
        if(RL[TileR(t)] * dm > limits.Rself && cnt < max)
            out[cnt++] = Violation{kFuzz, {a, -1}};
    }
    // ensemble budgets
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
    return cnt;
}

}  // namespace wfc
