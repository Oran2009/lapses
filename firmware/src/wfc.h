// Picks each sounding voice's delay/feedback/drive tile and mod, consistent
// under five rules. A knob scales what its rule reads, loading it till it breaks.

#pragma once

#include <stdint.h>

namespace wfc
{
// One voice per pad, one cluster. The two-group split cost more than it
// bought; kGroups survives at 1 so the per-group bookkeeping still reads.
constexpr int kVoices    = 7;
constexpr int kGroups    = 1;
constexpr int kGroupSize = kVoices / kGroups;
inline int    GroupOf(int v) { return v / kGroupSize; }
constexpr int kPads   = 7;

// Multiples of the base time on a 1/6 grid (6, 8, 9, 12, 15, 18, 24 sixths),
// so any set is related and rule 3 need only ask if two echoes are separable.
constexpr int   kDelays = 7;
constexpr float DR[kDelays]
    = {1.0f, 4.0f / 3, 3.0f / 2, 2.0f, 5.0f / 2, 3.0f, 4.0f};
// Small on purpose: the break lands a voice just INSIDE the gap so the two
// echoes flam. 1.19 was 44 ms at the default base -- past the gap.
constexpr float kSkew = 1.02f;
// Feedback gain. Above 1 the loop rings by itself
constexpr float FL[5] = {0.2f, 0.45f, 0.7f, 0.9f, 1.08f};
// Drive
constexpr float RL[4] = {0.05f, 0.3f, 0.6f, 1.0f};
// 140 tiles: d * 20 + f * 4 + r
constexpr int   kTiles = kDelays * 5 * 4;

// Mod SPEED (multiples of the base) and depth classes. Speed is how fast a
// voice's follower reacts; there is no oscillator. option = speed * 4 + depth
constexpr float MS[5]  = {1.0f, 4.0f / 3, 3.0f / 2, 2.0f, 3.0f};
constexpr float DP[4]  = {0.0f, 0.3f, 0.6f, 1.0f};
constexpr int   kModOpts = 5 * 4;

inline int TileD(int t) { return t / 20; }
inline int TileF(int t) { return (t % 20) / 4; }
inline int TileR(int t) { return t % 4; }
inline int MkTile(int d, int f, int r) { return d * 20 + f * 4 + r; }
constexpr int kDefaultTile = 0 * 20 + 1 * 4 + 1;

inline int ModSpeedOf(int o) { return o / 4; }
inline int ModDepthOf(int o) { return o % 4; }

// The six knobs, each 0..1 (4.2)
struct Knobs
{
    float delay, fb, drive, filter, mod, safe, unrest, hold, link, shimmer;
};

// What the rules allow at the current strictness (4.4)
struct Limits
{
    // Each sounding voice's SHARE of the ensemble budget: the group may
    // spend Fshare * n. Pairwise instead was a cardinality constraint.
    float Fshare, Dshare, Fself, Rself, Lpeak, gap;
    bool  ratio_on;
};

// A rule that is broken right now, and on which voices
enum BreakType
{
    kEcho = 0,
    kSwell,
    kFuzz,
    kBreakTypes
};
struct Violation
{
    BreakType type;
    // Up to kVoices voices, -1 terminated
    int       v[kVoices + 1];
};

const char* BreakName(BreakType t);

// ---------------------------------------------------------------- the knobs

Knobs&      K();
// Recomputes the rule tables. Call after any knob move.
void        RebuildRules();
// One fader for both: sets safe, and unrest from it. Rebuilds the rules.
void        SetAgitation(float a);

const Limits& Lim();

float DelayBase();
float DelayGlide();
float HopSeconds();
// S36: the gap between one voice of a wave changing and the next, and how
// far the wave carries
void  SetRipple(float r);
// What voice v is putting on the bus, so a break can pick one that is
// actually sounding. Pushed in once a block from the audio side.
void  SetLevel(int v, float l);
float Ripple();
float ReleaseT();
float AttackT();
float ModSpeed0();
float FbMul();
// How far the join inflates the feedback the rules read (not the audio's)
float ChargeMul();
float DrvMul();
float ModMul();
float LoopCut();
float VoiceCutMul();
float LapseChance();

// Holds the solver: no repairs, lapses or unrest. Scheduled changes still land
// and a press still rolls its voice, so the panel does not go dead.
void SetFrozen(bool on);

bool Frozen();
float UnrestRate();

// ---------------------------------------------------------------- the state

void Init(uint32_t seed);

// The host owns the pads and the latch; WFC only needs to know which voices
// are sounding and when each started, so it can order the repair wave.
void  SetSounding(int v, bool on, double now);
// While joined each group's rules are charged the louder group's load
void  SetJoined(bool on, double now);
bool  Sounding(int v);
// When this voice last started, for the wave order and for voice stealing
double StartedAt(int v);

// What WFC has decided
int  Tile(int v);
int  Mod(int v);
int  Skew(int v);

// What the audio has reached: the wave arrives over time (4.6b). Audio reads
// these, never the logical copy above.
int  ShownTile(int v);
// True once per delivered break, for the LED
bool TookBreak();
int  ShownMod(int v);
int  ShownSkew(int v);

// True once per voice when an event lands on it, for the display/LED flash.
// kind 2 = a break arriving, 1 = a repair.
int  TakeFlash(int v);

// ----------------------------------------------------------------- the work

// A pad press: re-roll this voice and settle the others around it (4.5)
void Pass(int v, double now);
// A pad let go: the voice's tail gets a tile of its own
void RerollTail(int v, double now);
// Repair with nothing pinned: after a knob move, a strictness change, or a
// scheduled return from a break
void Heal(double now);
void Heal(int group, double now);
// Let one rule go. type < 0 picks at random. (4.6)
void BreakRule(int type, float offset, double now);
// `prefer` names the voice the break should land on, -1 to pick one
void BreakRule(int type, int group, float offset, double now, int prefer = -1);
// A knob moved: the rules change now, the repair comes 350 ms after the last
// movement, so the player hears the strain first.
void KnobsMoved(double now);

// Deliver any events that are due, and run the heal/lapse/unrest timers.
void Tick(double now);

// Events that found the queue full since Init; nonzero means shown_* is stale.
int EventsDropped();

// Which rules are broken now, from the SHOWN state; group >= 0 judges one.
// Sized for every rule seven voices can break at once, or the last check starves.
constexpr int kMaxViolations
    = kVoices * 4 + kVoices * (kVoices - 1) / 2 + 2;
int  Diagnose(Violation* out, int max, int group = -1);

}  // namespace wfc
