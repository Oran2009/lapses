// THE SOLVER
//
// WFC decides every sounding voice's delay/feedback/drive tile and its mod,
// and keeps the sounding voices consistent with each other
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
// One voice per playable pad, and one cluster. The two-group split and its
// connector pad bought less than the complexity they cost: a bridge whose
// effect was never immediate, and voice stealing to hide six voices behind
// seven pads. kGroups survives at 1 so the solver's per-group bookkeeping
// still reads naturally; GroupOf is 0 for everything.
constexpr int kVoices    = 7;
constexpr int kGroups    = 1;
constexpr int kGroupSize = kVoices / kGroups;
inline int    GroupOf(int v) { return v / kGroupSize; }
constexpr int kPads   = 7;

// Delay time classes, as multiples of the base time. All seven sit on a
// common 1/6 grid (6, 8, 9, 12, 15, 18, 24 sixths), which is what makes any
// set of them rhythmically related and lets rule 3 ask only whether two
// echoes are far enough apart to hear separately.
constexpr int   kDelays = 7;
constexpr float DR[kDelays]
    = {1.0f, 4.0f / 3, 3.0f / 2, 2.0f, 5.0f / 2, 3.0f, 4.0f};
// What a broken delay is multiplied by. Small on purpose: the echo break's
// job is to land a voice just INSIDE the gap from another one, so the two
// echoes flam against each other. It was 1.19, which at the default base is
// 44 ms -- past the gap, so the break satisfied the rule it was breaking.
constexpr float kSkew = 1.02f;
// Feedback gain. Above 1 the loop rings by itself
constexpr float FL[5] = {0.2f, 0.45f, 0.7f, 0.9f, 1.08f};
// Drive
constexpr float RL[4] = {0.05f, 0.3f, 0.6f, 1.0f};
// 140 tiles: d * 20 + f * 4 + r
constexpr int   kTiles = kDelays * 5 * 4;

// Mod SPEED classes (multiples of the base) and depth classes. Speed is how
// fast a voice's follower reacts, not an oscillator's frequency -- there is
// no oscillator. option = speed class * 4 + depth class
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
    float delay, fb, drive, filter, mod, safe, unrest, hold, detune;
};

// What the rules allow at the current strictness (4.4)
struct Limits
{
    // Fshare/Dshare are each sounding voice's SHARE of the ensemble budget:
    // the whole group may spend Fshare * n. A pairwise sum instead meant
    // "no two voices both hot", which at seven voices is a cardinality
    // constraint the pairwise solver cannot see.
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
// While joined each group's rules are charged the louder group's load
void  SetJoined(bool on, float now);
bool  Sounding(int v);
// When this voice last started, for the wave order and for voice stealing
float StartedAt(int v);

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
void Pass(int v, float now);
// A pad let go: the voice's tail gets a tile of its own
void RerollTail(int v, float now);
// Repair with nothing pinned: after a knob move, a strictness change, or a
// scheduled return from a break
void Heal(float now);
void Heal(int group, float now);
// Let one rule go. type < 0 picks at random. (4.6)
void BreakRule(int type, float offset, float now);
// `prefer` names the voice the break should land on, -1 to pick one
void BreakRule(int type, int group, float offset, float now, int prefer = -1);
// A knob moved: the rules change now, the repair comes 350 ms after the last
// movement, so the player hears the strain first.
void KnobsMoved(float now);

// Deliver any events that are due, and run the heal/lapse/unrest timers.
void Tick(float now);

// Events that found the queue full since Init; nonzero means shown_* is stale.
int EventsDropped();

// Which rules are broken right now, reading the SHOWN state. Returns the
// count; `out` must hold at least kVoices * kVoices + kVoices entries. Each
// group is judged with its own charge; group >= 0 judges only that one.
// Every rule a seven-voice ensemble can break at once: three per voice, one
// per pair, one cycle per voice, plus the two ensemble budgets. A buffer
// shorter than this silently starves whatever Diagnose checks LAST, which
// with four voices it never did and with seven it does.
constexpr int kMaxViolations
    = kVoices * 4 + kVoices * (kVoices - 1) / 2 + 2;
int  Diagnose(Violation* out, int max, int group = -1);

}  // namespace wfc
