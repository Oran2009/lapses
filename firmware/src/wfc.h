// Picks each sounding voice's delay/feedback/drive tile and mod, consistent
// under five rules. A knob scales what its rule reads, loading it till it breaks.

#pragma once

#include <stdint.h>

namespace wfc
{
// one voice per pad, one cluster; kGroups stays at 1
constexpr int kVoices    = 7;
constexpr int kGroups    = 1;
constexpr int kGroupSize = kVoices / kGroups;
inline int    GroupOf(int v) { return v / kGroupSize; }
constexpr int kPads   = 7;

// multiples of the base time on a 1/6 grid, so any set is related
constexpr int   kDelays = 7;
constexpr float DR[kDelays]
    = {1.0f, 4.0f / 3, 3.0f / 2, 2.0f, 5.0f / 2, 3.0f, 4.0f};
// small on purpose: a break lands inside the gap and the echoes flam
constexpr float kSkew = 1.02f;
// feedback gain; above 1 the loop rings by itself
constexpr float FL[5] = {0.2f, 0.45f, 0.7f, 0.9f, 1.08f};
// drive
constexpr float RL[4] = {0.05f, 0.3f, 0.6f, 1.0f};
// 140 tiles: d * 20 + f * 4 + r
constexpr int   kTiles = kDelays * 5 * 4;

// mod speed and depth class; option = speed * 4 + depth
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

// the six knobs, each 0..1
struct Knobs
{
    float delay, fb, drive, filter, mod, safe, unrest, hold, link, shimmer;
};

// what the rules allow at the current strictness
struct Limits
{
    // each voice's share: the group may spend Fshare * n
    float Fshare, Dshare, Fself, Rself, Lpeak, gap;
    bool  ratio_on;
};

// a rule broken right now, and on which voices
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
    // up to kVoices, -1 terminated
    int       v[kVoices + 1];
};

const char* BreakName(BreakType t);

// ---------------------------------------------------------------- the knobs

Knobs&      K();
// recompute the rule tables; call after any knob move
void        RebuildRules();
// one fader for both: sets safe, and unrest from it
void        SetAgitation(float a);

const Limits& Lim();

float DelayBase();
float DelayGlide();
float HopSeconds();
// S36: the gap between one voice of a wave changing and the next
void  SetRipple(float r);
// what voice v puts on the bus, so a break picks one that sounds
void  SetLevel(int v, float l);
float Ripple();
float ReleaseT();
float AttackT();
float ModSpeed0();
float FbMul();
// how far the join inflates the feedback the rules read
float ChargeMul();
float DrvMul();
float ModMul();
float LoopCut();
float VoiceCutMul();
float LapseChance();

// holds the solver: no repairs, lapses or unrest; scheduled changes still land
void SetFrozen(bool on);

bool Frozen();
float UnrestRate();

// ---------------------------------------------------------------- the state

void Init(uint32_t seed);

// which voices sound and when each started, for the wave order
void  SetSounding(int v, bool on, double now);
// while joined each group's rules are charged the louder group's load
void  SetJoined(bool on, double now);
bool  Sounding(int v);
// when this voice last started, for the wave order and voice stealing
double StartedAt(int v);

// what WFC has decided
int  Tile(int v);
int  Mod(int v);
int  Skew(int v);

// what the audio has reached; the wave arrives over time
int  ShownTile(int v);
// once per delivered break, for the LED
bool TookBreak();
int  ShownMod(int v);
int  ShownSkew(int v);

// once per voice when an event lands; kind 2 = break, 1 = repair
int  TakeFlash(int v);

// ----------------------------------------------------------------- the work

// a pad press: re-roll this voice and settle the others
void Pass(int v, double now);
// a pad let go: the voice's tail gets a tile of its own
void RerollTail(int v, double now);
// repair with nothing pinned: after a knob move, a strictness change or a break
void Heal(double now);
void Heal(int group, double now);
// let one rule go; type < 0 picks at random
void BreakRule(int type, float offset, double now);
// `prefer` names the voice the break lands on, -1 to pick
void BreakRule(int type, int group, float offset, double now, int prefer = -1);
// a knob moved: the rules change now, the repair 350 ms after the last movement
void KnobsMoved(double now);

// deliver due events, and run the heal/lapse/unrest timers
void Tick(double now);

// events that found the queue full; nonzero means shown_* is stale
int EventsDropped();

// rules broken now, from the SHOWN state; group >= 0 judges one
constexpr int kMaxViolations
    = kVoices * 4 + kVoices * (kVoices - 1) / 2 + 2;
int  Diagnose(Violation* out, int max, int group = -1);

}  // namespace wfc
