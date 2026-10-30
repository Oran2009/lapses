// THE SOLVER
//
// WFC decides every sounding voice's delay/feedback/drive tile, its patch
// cable and its LFO, and keeps the sounding voices consistent with each other
// under five rules. The six knobs scale the values those rules read, so
// turning a knob up loads its rule until it breaks. WFC then repairs it, and
// the repair arrives as a wave across the voices, one hop apart.
//
// Portable C++: no Daisy headers, so this compiles and is tested on desktop.
// Ported from docs/safeguard-v3.html, which is the behavioural source of
// truth. Section numbers refer to docs/HANDOFF.md.

#pragma once

#include <stdint.h>

namespace wfc
{
constexpr int kVoices = 4;
constexpr int kPads   = 7;

// Delay time classes, as multiples of the base time
constexpr float DR[5] = {1.0f, 4.0f / 3, 3.0f / 2, 2.0f, 3.0f};
// Feedback gain. Above 1 the loop rings by itself
constexpr float FL[5] = {0.2f, 0.45f, 0.7f, 0.9f, 1.08f};
// Drive
constexpr float RL[4] = {0.05f, 0.3f, 0.6f, 1.0f};
// 100 tiles: d * 20 + f * 4 + r
constexpr int   kTiles = 5 * 5 * 4;

// Cable strength, and 0 = no cable, else 1 + target * 3 + amount
constexpr float AMT[3]  = {0.3f, 0.6f, 0.9f};
constexpr int   kOpts   = 1 + kVoices * 3;

// LFO rate classes (multiples of the base rate) and depth classes.
// option = rate class * 4 + depth class
constexpr float LR[5]  = {1.0f, 4.0f / 3, 3.0f / 2, 2.0f, 3.0f};
constexpr float DP[4]  = {0.0f, 0.3f, 0.6f, 1.0f};
constexpr int   kLfoOpts = 5 * 4;

inline int TileD(int t) { return t / 20; }
inline int TileF(int t) { return (t % 20) / 4; }
inline int TileR(int t) { return t % 4; }
inline int MkTile(int d, int f, int r) { return d * 20 + f * 4 + r; }
constexpr int kDefaultTile = 0 * 20 + 1 * 4 + 1;

inline int OptTarget(int o) { return o == 0 ? -1 : (o - 1) / 3; }
inline int OptAmt(int o) { return (o - 1) % 3; }
inline int LfoRate(int o) { return o / 4; }
inline int LfoDepth(int o) { return o % 4; }

// The six knobs, each 0..1 (4.2)
struct Knobs
{
    float delay, fb, drive, filter, lfo, safe, unrest, tune;
};

// What the rules allow at the current strictness (4.4)
struct Limits
{
    float Fpair, Dpair, Fself, Lpeak, gap;
    bool  ratio_on, cycle_on;
};

// A rule that is broken right now, and on which voices
enum BreakType
{
    kEcho = 0,
    kSwell,
    kFuzz,
    kWobble,
    kCable,
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
const Limits& Lim();

float DelayBase();
float DelayGlide();
float HopSeconds();
float ReleaseT();
float AttackT();
float LfoRate0();
float FbMul();
float DrvMul();
float LfoMul();
float LoopCut();
float VoiceCutMul();
float LapseChance();

// Holds the solver: no repairs, no lapses, no unrest. Scheduled changes still
// land and a press still rolls its voice, so the texture freezes without the
// panel going dead.
void SetFrozen(bool on);
bool Frozen();
float UnrestRate();

// ---------------------------------------------------------------- the state

void Init(uint32_t seed);

// The host owns the pads and the latch; WFC only needs to know which voices
// are sounding and when each started, so it can order the repair wave.
void  SetSounding(int v, bool on, float now);
bool  Sounding(int v);
// When this voice last started, for the wave order and for voice stealing
float StartedAt(int v);

// What WFC has decided
int  Tile(int v);
int  Patch(int v);
int  Lfo(int v);
int  Skew(int v);

// What the audio has reached: the wave arrives over time (4.6b). Audio reads
// these, never the logical copy above.
int  ShownTile(int v);
int  ShownPatch(int v);
int  ShownLfo(int v);
int  ShownSkew(int v);

// True once per voice when an event lands on it, for the display/LED flash.
// kind 2 = a break arriving, 1 = a repair.
int  TakeFlash(int v);

// ----------------------------------------------------------------- the work

// A pad press: re-roll this voice and settle the others around it (4.5)
void Pass(int v, float now);
// Repair with nothing pinned: after a knob move, a strictness change, or a
// scheduled return from a break
void Heal(float now);
// Let one rule go. type < 0 picks at random. (4.6)
void BreakRule(int type, float offset, float now);
// A knob moved: the rules change now, the repair comes 350 ms after the last
// movement, so the player hears the strain first.
void KnobsMoved(float now);

// Deliver any events that are due, and run the heal/lapse/unrest timers.
void Tick(float now);

// Which rules are broken right now, reading the SHOWN state. Returns the
// count; `out` must hold at least kVoices * kVoices + kVoices entries.
int  Diagnose(Violation* out, int max);

}  // namespace wfc
