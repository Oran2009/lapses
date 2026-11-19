// Picks each sounding voice's delay/feedback/drive tile and mod, consistent
// under five rules. A knob scales what its rule reads, loading it till it breaks.

#pragma once

#include <stdint.h>

namespace wfc
{
// One voice per pad, one cluster. kGroups stays at 1: the split cost more than it bought.
constexpr int kVoices    = 7;
constexpr int kGroups    = 1;
constexpr int kGroupSize = kVoices / kGroups;
inline int    GroupOf(int v) { return v / kGroupSize; }
constexpr int kPads   = 7;

// Multiples of the base time on a 1/6 grid, so any set is related.
constexpr int   kDelays = 7;
constexpr float DR[kDelays]
    = {1.0f, 4.0f / 3, 3.0f / 2, 2.0f, 5.0f / 2, 3.0f, 4.0f};
// Small on purpose: a break lands inside the gap and the echoes flam; 1.19 was past it.
constexpr float kSkew = 1.02f;
// Feedback gain. Above 1 the loop rings by itself
constexpr float FL[5] = {0.2f, 0.45f, 0.7f, 0.9f, 1.08f};
// Drive
constexpr float RL[4] = {0.05f, 0.3f, 0.6f, 1.0f};
// 140 tiles: d * 20 + f * 4 + r
constexpr int   kTiles = kDelays * 5 * 4;

// Mod speed (multiples of the base) and depth class. option = speed * 4 + depth
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
    // Each voice's share of the ensemble budget: the group may spend Fshare * n.
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
// S36: the gap between one voice of a wave changing and the next
void  SetRipple(float r);
// What voice v puts on the bus, so a break can pick one that is sounding
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

// Holds the solver: no repairs, lapses or unrest. Scheduled changes still land.
void SetFrozen(bool on);

bool Frozen();
float UnrestRate();

// ---------------------------------------------------------------- the state

void Init(uint32_t seed);

// WFC needs only which voices sound and when each started, for the wave order.
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

// What the audio has reached: the wave arrives over time. Audio reads these.
int  ShownTile(int v);
// True once per delivered break, for the LED
bool TookBreak();
int  ShownMod(int v);
int  ShownSkew(int v);

// Once per voice when an event lands, for the display. kind 2 = break, 1 = repair.
int  TakeFlash(int v);

// ----------------------------------------------------------------- the work

// A pad press: re-roll this voice and settle the others around it (4.5)
void Pass(int v, double now);
// A pad let go: the voice's tail gets a tile of its own
void RerollTail(int v, double now);
// Repair with nothing pinned: after a knob move, a strictness change or a break
void Heal(double now);
void Heal(int group, double now);
// Let one rule go. type < 0 picks at random. (4.6)
void BreakRule(int type, float offset, double now);
// `prefer` names the voice the break should land on, -1 to pick one
void BreakRule(int type, int group, float offset, double now, int prefer = -1);
// A knob moved: the rules change now, the repair 350 ms after the last movement.
void KnobsMoved(double now);

// Deliver any events that are due, and run the heal/lapse/unrest timers.
void Tick(double now);

// Events that found the queue full since Init; nonzero means shown_* is stale.
int EventsDropped();

// Rules broken now, from the SHOWN state; group >= 0 judges one. Sized for all seven.
constexpr int kMaxViolations
    = kVoices * 4 + kVoices * (kVoices - 1) / 2 + 2;
int  Diagnose(Violation* out, int max, int group = -1);

}  // namespace wfc
