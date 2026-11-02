// THE DESKTOP SHELL
//
// The same engine the Daisy runs, with a window instead of the Touch 2.
// Win32 + waveOut + GDI, so there is nothing to install: the Windows SDK that
// comes with the compiler is the whole dependency list.
//
//   pads     A S D F G H J   (or 1..7)
//   switches click them, or C (freeze, no hardware switch), M (mod mode)
//   octave   Z / X
//   volume   - / =
//   knobs    drag with the mouse, or F1..F6 then arrow keys
//   break    B,   settle  N,   freeze  C,   cables  M
//   panic    escape

#include "synth.h"
#include "voices.h"
#include "wfc.h"

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace
{
constexpr int kSr    = 48000;
constexpr int kBlock = 64;
// waveOut double buffering. Measured: with the window busy, this thread can
// go unscheduled for ~36 ms at a time, and at 8 buffers (43 ms) the queue was
// running dry -- which is what stuttered. 16 buffers is 85 ms, a margin a
// drone instrument with 30 ms minimum echoes can afford.
constexpr int kBufs    = 16;
constexpr int kBufFrames = kBlock * 4;

HWAVEOUT   hwo = nullptr;
WAVEHDR    hdr[kBufs];
short      buf[kBufs][kBufFrames * 2];
volatile LONG queued = 0;
HANDLE     wake = nullptr;       // WaveProc signals, the audio thread fills
volatile LONG stop_audio = 0;
CRITICAL_SECTION engine_cs;      // the window thread's input vs the render

// Capture, so the effects box can be heard here and not only on the Touch 2.
// waveIn mirrors waveOut: prepare, queue, and re-add from the audio thread.
constexpr int kRing = kBufFrames * kBufs * 4;
// The renderer keeps a whole output queue in flight, so it asks for input it
// has not been handed yet. Nothing may be taken from the ring until it holds
// that much: starting early is what made it alternate signal and silence.
constexpr int kPrime = kBufFrames * (kBufs + 2);
HWAVEIN       hwi = nullptr;
WAVEHDR       ihdr[kBufs];
short         ibuf[kBufs][kBufFrames];
float         in_ring[kRing];
int           in_w = 0, in_r = 0;
bool          in_ready = false;
float         in_last = 0.0f;
// Capture and render share neither a clock nor, necessarily, a rate: measured
// here, capture ran 17% ahead of what the renderer asked for. So the ring is
// read at a fractional step nudged to hold its level at kPrime. Dropping the
// surplus a sample at a time is what crackled.
double        in_pos = 0.0, in_step = 1.0;

int InRingUsed() { return (in_w - in_r + kRing) % kRing; }

double start_qpc = 0, qpc_freq = 1;

float Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (float)(((double)t.QuadPart - start_qpc) / qpc_freq);
}

void Fill(WAVEHDR* h)
{
    float  l[kBlock], r[kBlock], in[kBlock];
    short* out = (short*)h->lpData;
    for(int off = 0; off < kBufFrames; off += kBlock)
    {
        if(!in_ready && InRingUsed() >= kPrime)
            in_ready = true;
        {
            const double err = (double)(InRingUsed() - kPrime) / (double)kPrime;
            const double st  = 1.0 + err * 0.08;
            in_step          = st < 0.75 ? 0.75 : (st > 1.25 ? 1.25 : st);
        }
        for(int i = 0; i < kBlock; i++)
        {
            if(in_ready && in_r != in_w)
            {
                const float a = in_ring[in_r];
                const float b = in_ring[(in_r + 1) % kRing];
                in_last       = a + (b - a) * (float)in_pos;
                in_pos += in_step;
                while(in_pos >= 1.0 && in_r != in_w)
                {
                    in_pos -= 1.0;
                    in_r = (in_r + 1) % kRing;
                }
            }
            // Holding the last sample is a dull moment; a zero is a click
            in[i] = in_ready ? in_last : 0.0f;
        }
        synth::Control(Now());
        synth::Process(l, r, kBlock, hwi ? in : nullptr);
        for(int i = 0; i < kBlock; i++)
        {
            out[(off + i) * 2]     = (short)(l[i] * 32000.0f);
            out[(off + i) * 2 + 1] = (short)(r[i] * 32000.0f);
        }
    }
    waveOutWrite(hwo, h, sizeof(WAVEHDR));
}

void CALLBACK WaveProc(HWAVEOUT, UINT msg, DWORD_PTR, DWORD_PTR p1, DWORD_PTR)
{
    if(msg == WOM_DONE)
    {
        InterlockedDecrement(&queued);
        SetEvent(wake);   // one of the few calls allowed in here
    }
    (void)p1;
}

DWORD WINAPI AudioThread(LPVOID);

// Default capture device, mono, same rate. Absent is not an error: without it
// the instrument is simply always a synth here.
void CaptureStart()
{
    WAVEFORMATEX wf    = {};
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 1;
    wf.nSamplesPerSec  = kSr;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = 2;
    wf.nAvgBytesPerSec = kSr * 2;
    if(waveInGetNumDevs() == 0
       || waveInOpen(&hwi, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL)
              != MMSYSERR_NOERROR)
    {
        hwi = nullptr;
        return;
    }
    for(int i = 0; i < kBufs; i++)
    {
        ihdr[i]                = {};
        ihdr[i].lpData         = (LPSTR)ibuf[i];
        ihdr[i].dwBufferLength = sizeof(ibuf[i]);
        waveInPrepareHeader(hwi, &ihdr[i], sizeof(WAVEHDR));
        waveInAddBuffer(hwi, &ihdr[i], sizeof(WAVEHDR));
    }
    in_w = in_r = 0;
    in_ready = false;
    in_last  = 0.0f;
    in_pos   = 0.0;
    in_step  = 1.0;
    waveInStart(hwi);
}

// Drains whatever capture has finished into the ring and re-queues it
void CapturePump()
{
    if(!hwi)
        return;
    for(int i = 0; i < kBufs; i++)
        if(ihdr[i].dwFlags & WHDR_DONE)
        {
            const short* p = (const short*)ihdr[i].lpData;
            for(int j = 0; j < kBufFrames; j++)
            {
                in_ring[in_w] = (float)p[j] * (1.0f / 32768.0f);
                in_w          = (in_w + 1) % kRing;
            }
            ihdr[i].dwFlags &= ~WHDR_DONE;
            waveInAddBuffer(hwi, &ihdr[i], sizeof(WAVEHDR));
        }
    // A backstop only: the fractional read is what holds the level normally
    while(InRingUsed() > kRing - kBufFrames * 2)
        in_r = (in_r + 1) % kRing;
}

bool AudioStart()
{
    InitializeCriticalSection(&engine_cs);
    wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    WAVEFORMATEX wf = {};
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 2;
    wf.nSamplesPerSec  = kSr;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = 4;
    wf.nAvgBytesPerSec = kSr * 4;
    if(waveOutOpen(&hwo, WAVE_MAPPER, &wf, (DWORD_PTR)WaveProc, 0,
                   CALLBACK_FUNCTION)
       != MMSYSERR_NOERROR)
        return false;
    for(int i = 0; i < kBufs; i++)
    {
        hdr[i]                  = {};
        hdr[i].lpData           = (LPSTR)buf[i];
        hdr[i].dwBufferLength   = sizeof(buf[i]);
        waveOutPrepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
    }
    // ponytail: off while the synth side is being tuned, same as the Daisy
    // shell. Put this call back to re-enable.
    // CaptureStart();
    CloseHandle(CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr));
    return true;
}

// Keeps every buffer in flight, on its own thread: the window thread stalls
// for as long as a window drag or a menu lasts, and the audio cannot wait.
DWORD WINAPI AudioThread(LPVOID)
{
    timeBeginPeriod(1);
    // Normal priority against a window thread is what let this be starved in
    // the first place. Audio does not get to wait its turn.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while(!stop_audio)
    {
        CapturePump();
        for(int i = 0; i < kBufs; i++)
            if(!(hdr[i].dwFlags & WHDR_INQUEUE))
            {
                InterlockedIncrement(&queued);
                EnterCriticalSection(&engine_cs);
                Fill(&hdr[i]);
                LeaveCriticalSection(&engine_cs);
            }
        WaitForSingleObject(wake, 5);
    }
    timeEndPeriod(1);
    return 0;
}

// ------------------------------------------------------------------- the UI

int   sel_knob = -1;      // the knob F1..F8 picked
int   drag     = -1;      // the knob the mouse has hold of
int   mouse_pad = -1;     // the pad the mouse pressed, so key-held pads survive
POINT drag_from;
float drag_at;

const int kPadKey[synth::kPads] = {'A', 'S', 'D', 'F', 'G', 'H', 'J'};

struct Rect
{
    int x, y, w, h;
};
Rect knob_r[synth::kKnobs], pad_r[synth::kPads];
bool knob_vert[synth::kKnobs];
// Panel pads with no voice: P00 oct down, P01 unmapped, P02 oct up, P10 vol
// down, P11 vol up. Same order everywhere below.
Rect xp_r[5];
bool xp_on[5];
int  mouse_xp = -1;
const char* kXpName[5] = {"P00", "P01", "P02", "P10", "P11"};
const char* kXpFn[5]   = {"oct -", "unmapped", "oct +", "vol -", "vol +"};
const char  kXpKey[5]  = {'Z', 0, 'X', '-', '='};
// Engine knob index -> the panel's name, so the window reads like the board
const char* kKnobLabel[synth::kKnobs]
    = {"S32 delay", "S31 feedback", "S33 distortion", "S34 filter",
       "S30 mod",   "S36 agitation", "",              "S37 HOLD"};

// Panel replica on the left, cards and GUI-only controls to its right
constexpr int kPx = 16, kPy = 16, kPW = 540, kPH = 908;
constexpr int kRx = kPx + kPW + 24;
constexpr int kGuiY = 350;
constexpr int kSwW = 140;
// The panel switches, drawn and clicked as stacked cells, so they behave the
// way the Touch 2's do rather than as keyboard toggles. Cell 0 is the top.
// Pos() gives the -1 down / 0 centre / +1 up the engine expects, for the two
// hardware switches. The third is the voice-model spike and exists only here.
constexpr int kSwitches   = 4;
constexpr int kSwMaxCells = voices::kModels;
const int     kSwCells[kSwitches] = {3, 3, voices::kModels, 2};
Rect          sw_r[kSwitches][kSwMaxCells];
int           sw_cell[kSwitches] = {1, 1, 0, 0};

inline int Pos(int k) { return 1 - sw_cell[k]; }

const char* kSwLabel[kSwitches][kSwMaxCells]
    = {{"hold left", "hold both", "hold right"},
       {"cross-mod", "no cables", "cross-feed"},
       {},
       {"live", "freeze"}};
const char* kSwName[kSwitches] = {"S07/S08 HOLD", "S09/S10 cables",
                                  "voice model (V)", "freeze (C)"};

// The model cells are named by the engine, so there is one list and not two
const char* SwLabel(int k, int c)
{
    return k == 2 ? voices::ModelName(c) : kSwLabel[k][c];
}

// Exactly what panel::Poll does with them, so there is one behaviour and not
// two. Freeze has no hardware switch; it is the fourth cell group here.

void ApplySwitches()
{
    synth::SetHoldTarget(Pos(0));
    synth::Freeze(sw_cell[3]);
    synth::SetCableMode(Pos(1) > 0   ? voices::kCableMod
                        : Pos(1) < 0 ? voices::kCableFeed
                                     : voices::kCableOff);
    voices::SetModel(sw_cell[2]);
}

void Layout()
{
    auto X = [](float f) { return kPx + (int)(kPW * f); };
    auto Y = [](float f) { return kPy + (int)(kPH * f); };
    struct Spot { int k; float fx, fy; };
    static const Spot knobs[] = {{1, .2f, .35f}, {0, .4f, .35f}, {2, .6f, .35f},
                                 {3, .8f, .35f}, {4, .2f, .48f}};
    for(const Spot& sp : knobs)
    {
        knob_r[sp.k]    = {X(sp.fx) - 50, Y(sp.fy) - 46, 100, 96};
        knob_vert[sp.k] = false;
    }
    const int fh = Y(.54f) - Y(.29f);
    knob_r[5] = {X(.06f) - 18, Y(.29f), 36, fh};   // S36 agitation
    knob_r[7] = {X(.94f) - 18, Y(.29f), 36, fh};   // S37 hold
    knob_r[6] = {0, 0, 0, 0};   // unrest has no control of its own now
    knob_vert[5] = knob_vert[7] = true;

    // Engine pad order is P03 P04 P05 P06 P07 P08 P09
    static const float pf[synth::kPads][2] = {{.10f, .83f}, {.30f, .80f},
                                              {.50f, .80f}, {.70f, .80f},
                                              {.90f, .83f}, {.38f, .925f},
                                              {.62f, .925f}};
    for(int i = 0; i < synth::kPads; i++)
        pad_r[i] = {X(pf[i][0]) - 43, Y(pf[i][1]) - 32, 86, 64};
    static const float xf[5][2] = {{.30f, .585f}, {.50f, .585f}, {.70f, .585f},
                                   {.38f, .045f}, {.62f, .045f}};
    for(int i = 0; i < 5; i++)
        xp_r[i] = i < 3 ? Rect{X(xf[i][0]) - 43, Y(xf[i][1]) - 32, 86, 64}
                        : Rect{X(xf[i][0]) - 54, Y(xf[i][1]) - 22, 108, 44};

    // Both switches sit in the gap under pad row A; S07/S08 is the lower
    const int base = Y(.585f) + 50;
    for(int c = 0; c < 3; c++)
    {
        sw_r[1][c] = {X(.33f) - kSwW / 2, base + c * 22, kSwW, 22};
        sw_r[0][c] = {X(.67f) - kSwW / 2, base + 30 + c * 22, kSwW, 22};
    }
    for(int k = 2; k < kSwitches; k++)
        for(int c = 0; c < kSwCells[k]; c++)
            sw_r[k][c] = {kRx + 16 + (k - 2) * (kSwW + 16),
                          kGuiY + 52 + c * 22, kSwW, 22};
}

void Bar(HDC dc, Rect r, float f, COLORREF c)
{
    RECT b = {r.x, r.y, r.x + r.w, r.y + r.h};
    HBRUSH bg = CreateSolidBrush(RGB(32, 34, 40));
    FillRect(dc, &b, bg);
    DeleteObject(bg);
    if(f > 0.0f)
    {
        RECT f2 = {r.x, r.y, r.x + (int)(r.w * f), r.y + r.h};
        HBRUSH fg = CreateSolidBrush(c);
        FillRect(dc, &f2, fg);
        DeleteObject(fg);
    }
}

// For values that swing both ways: the modulator and the pan position
void Centred(HDC dc, Rect r, float f, COLORREF c)
{
    RECT b = {r.x, r.y, r.x + r.w, r.y + r.h};
    HBRUSH bg = CreateSolidBrush(RGB(32, 34, 40));
    FillRect(dc, &b, bg);
    DeleteObject(bg);
    const int mid = r.x + r.w / 2;
    const int end = mid + (int)((float)(r.w / 2) * (f < -1.0f ? -1.0f
                                                             : (f > 1.0f ? 1.0f
                                                                         : f)));
    RECT f2 = {end < mid ? end : mid, r.y, end < mid ? mid : end, r.y + r.h};
    if(f2.right > f2.left)
    {
        HBRUSH fg = CreateSolidBrush(c);
        FillRect(dc, &f2, fg);
        DeleteObject(fg);
    }
    HBRUSH tick = CreateSolidBrush(RGB(90, 95, 108));
    RECT   t    = {mid, r.y, mid + 1, r.y + r.h};
    FillRect(dc, &t, tick);
    DeleteObject(tick);
}

void Text(HDC dc, int x, int y, const char* s, COLORREF c)
{
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    TextOutA(dc, x, y, s, (int)strlen(s));
}

void TextC(HDC dc, int cx, int y, const char* s, COLORREF c)
{
    SetTextAlign(dc, TA_CENTER | TA_TOP);
    Text(dc, cx, y, s, c);
    SetTextAlign(dc, TA_LEFT | TA_TOP);
}

void Box(HDC dc, Rect r, COLORREF c)
{
    RECT b = {r.x, r.y, r.x + r.w, r.y + r.h};
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &b, br);
    DeleteObject(br);
}

// The two voice groups read as two families: blue left, green right
COLORREF Family(int group, bool on)
{
    return group ? (on ? RGB(90, 200, 140) : RGB(30, 64, 50))
                 : (on ? RGB(90, 140, 230) : RGB(30, 44, 76));
}
COLORREF FamilyText(int group)
{
    return group ? RGB(120, 190, 150) : RGB(130, 160, 220);
}

void Paint(HWND hw)
{
    PAINTSTRUCT ps;
    HDC         real = BeginPaint(hw, &ps);
    RECT        cr;
    GetClientRect(hw, &cr);
    const int W = cr.right, H = cr.bottom;

    // Double buffered, or the bars flicker
    HDC     dc = CreateCompatibleDC(real);
    HBITMAP bm = CreateCompatibleBitmap(real, W, H);
    HBITMAP ob = (HBITMAP)SelectObject(dc, bm);
    HBRUSH  back = CreateSolidBrush(RGB(18, 19, 23));
    FillRect(dc, &cr, back);
    DeleteObject(back);
    HFONT font = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0,
                             CLEARTYPE_QUALITY, 0, "Consolas");
    HFONT of = (HFONT)SelectObject(dc, font);

    Layout();
    const COLORREF dim = RGB(120, 126, 140), fgc = RGB(222, 226, 235);
    const COLORREF hot = RGB(226, 96, 84), ok = RGB(110, 190, 150);

    // What is broken right now, read from the SHOWN state: this is the line
    // that tells you which rule you are hearing
    wfc::Violation vio[32];
    const int      nv = wfc::Diagnose(vio, 32);
    char           line[256] = "in check";
    if(nv)
    {
        int n = snprintf(line, sizeof(line), "BROKEN:");
        for(int i = 0; i < nv && n < (int)sizeof(line) - 24; i++)
        {
            n += snprintf(line + n, sizeof(line) - n, " %s(",
                          wfc::BreakName(vio[i].type));
            for(int j = 0; vio[i].v[j] >= 0 && j < wfc::kVoices; j++)
                n += snprintf(line + n, sizeof(line) - n, "%s%d", j ? "," : "",
                              vio[i].v[j] + 1);
            n += snprintf(line + n, sizeof(line) - n, ")");
        }
    }
    Text(dc, kRx, 12, line, nv ? hot : ok);

    char hdr2[160], chg[24] = "";
    if(wfc::ChargeMul() > 1.0f)
        snprintf(chg, sizeof(chg), "   CHARGED x%.2f", wfc::ChargeMul());
    snprintf(hdr2, sizeof(hdr2),
             "oct %+d   out %.0f dB   %s   lapse %.2f  "
             "unrest %.2f/s   cables %s%s",
             synth::Octave(), synth::GainDb(),
             synth::InputMix() > 0.5f
                     ? "INPUT  "
                     : (synth::Frozen() ? "FROZEN " : "       "), wfc::LapseChance(),
             wfc::UnrestRate(), synth::CableName(synth::CableMode()), chg);
    Text(dc, kRx, 30, hdr2, dim);

    // The voice cards, showing the EFFECTIVE values, knob scaling and mod
    // included, because that is what you hear (principle 8)
    const int ch = 104, gap = 34, cols = wfc::kGroupSize;
    const int cw = (W - kRx - 16 - 16 * (cols - 1)) / cols;
    const int lw = 188;   // text column; the bars take the rest of the card
    auto cardX = [&](int v) { return kRx + (v % cols) * (cw + 16); };
    auto cardY = [&](int v) { return 74 + wfc::GroupOf(v) * (ch + gap); };

    for(int g = 0; g < wfc::kGroups; g++)
    {
        char gl[32];
        snprintf(gl, sizeof(gl), "%s group", g ? "right" : "left");
        Text(dc, kRx, cardY(g * cols) - 16, gl, FamilyText(g));
    }

    for(int v = 0; v < wfc::kVoices; v++)
    {
        const int x = cardX(v), cy = cardY(v);
        const int bx = x + lw, bw = cw - lw - 8;
        RECT      card = {x, cy, x + cw, cy + ch};
        const bool rg = wfc::GroupOf(v) != 0;
        HBRUSH    cb = CreateSolidBrush(
            wfc::Sounding(v) ? (rg ? RGB(32, 58, 48) : RGB(34, 44, 70))
                             : (rg ? RGB(22, 32, 28) : RGB(22, 27, 38)));
        FillRect(dc, &card, cb);
        DeleteObject(cb);

        const int t = wfc::ShownTile(v), lo = wfc::ShownMod(v);
        const float F = wfc::FL[wfc::TileF(t)] * wfc::FbMul();
        const float R = wfc::RL[wfc::TileR(t)] * wfc::DrvMul();
        const float D = wfc::DelayBase() * wfc::DR[wfc::TileD(t)]
                        * (wfc::ShownSkew(v) ? 1.19f : 1.0f);
        // Live first, the solver's target in brackets: the gap between them
        // is the glide and the modulation, which is the thing worth watching
        const float eD = voices::EffDly(v), eF = voices::EffFb(v);
        const float eR = (voices::EffDrv(v) - 1.0f) / 12.0f;
        int pad = -1;
        for(int p = 0; p < synth::kPads; p++)
            if(synth::PadVoice(p) == v)
                pad = p;

        char s[128];
        snprintf(s, sizeof(s), "voice %d%s", v + 1,
                 wfc::ShownSkew(v) ? "  SKEW" : "");
        Text(dc, x + 8, cy + 6, s, wfc::Sounding(v) ? fgc : dim);
        if(pad >= 0)
        {
            snprintf(s, sizeof(s), "P%02d %c %.0fHz", pad + 3,
                     (char)kPadKey[pad], voices::Pitch(v));
            Text(dc, bx, cy + 6, s, ok);
        }

        snprintf(s, sizeof(s), "dly %4.0f (%.0f)", eD * 1000.0f, D * 1000.0f);
        Text(dc, x + 8, cy + 26, s, dim);
        snprintf(s, sizeof(s), "fb  %.2f (%.2f)%s", eF, F,
                 eF > 1.0f ? " RING" : "");
        Text(dc, x + 8, cy + 42, s, eF > 1.0f ? hot : dim);
        snprintf(s, sizeof(s), "drv %.2f (%.2f)", eR, R);
        Text(dc, x + 8, cy + 58, s, dim);

        // What is actually being spent: the modulator times its depth. The
        // bare modulator swings full scale even at depth zero, which made the
        // bar look alive while nothing was being modulated.
        const float md = voices::Mod(v) * voices::ModDepth(v);
        snprintf(s, sizeof(s), "mod %+.2f  %.2fHz x%.2f", md,
                 wfc::ModSpeed0() * wfc::MS[wfc::ModSpeedOf(lo)],
                 voices::ModDepth(v));
        Text(dc, x + 8, cy + 74, s, dim);
        Centred(dc, {x + 8, cy + 90, lw - 16, 8}, md / 1.5f,
                RGB(150, 130, 200));

        Text(dc, bx, cy + 26, "amp", dim);
        Bar(dc, {bx + 34, cy + 28, bw - 34, 8}, voices::Amp(v), ok);
        Text(dc, bx, cy + 42, "out", dim);
        Bar(dc, {bx + 34, cy + 44, bw - 34, 8},
            voices::LoopLevel(v) * 4.0f > 1.0f ? 1.0f
                                              : voices::LoopLevel(v) * 4.0f,
            RGB(96, 118, 170));
        Text(dc, bx, cy + 58, "pan", dim);
        Centred(dc, {bx + 34, cy + 60, bw - 34, 8}, voices::Pan(v),
                RGB(130, 170, 200));

        const int tgt = wfc::OptTarget(wfc::ShownPatch(v));
        if(tgt >= 0)
        {
            snprintf(s, sizeof(s), "cable>v%d x%.2f", tgt + 1,
                     wfc::AMT[wfc::OptAmt(wfc::ShownPatch(v))]);
            Text(dc, bx, cy + 76, s, ok);
        }
    }

    // Bridges bypass the solver, so they come from the voices matrix and not
    // from OptTarget. Effective, not commanded, so they fade with the sound.
    for(int src = 0; src < wfc::kVoices; src++)
        for(int dst = 0; dst < wfc::kVoices; dst++)
        {
            if(wfc::GroupOf(src) == wfc::GroupOf(dst))
                continue;
            const float e = voices::CableEff(src, dst);
            if(e < 0.01f)
                continue;
            const float f = e > 1.0f ? 1.0f : e;
            // src above, so a bridge leaves its bottom edge; the reverse
            // direction is nudged so the pair does not draw as one line
            const int up = wfc::GroupOf(src) == 0, off = up ? -8 : 8;
            const int x0 = cardX(src) + cw / 2 + off, x1 = cardX(dst) + cw / 2 + off;
            const int y0 = up ? cardY(src) + ch : cardY(src);
            const int y1 = up ? cardY(dst) : cardY(dst) + ch;
            const COLORREF bc = RGB(30 + (int)(210 * f), 30 + (int)(150 * f), 30);
            HPEN   pen = CreatePen(PS_SOLID, 1 + (int)(3 * f), bc);
            HPEN   op  = (HPEN)SelectObject(dc, pen);
            MoveToEx(dc, x0, y0, nullptr);
            LineTo(dc, x1, y1);
            SelectObject(dc, op);
            DeleteObject(pen);
            HBRUSH hb = CreateSolidBrush(bc);
            RECT   head = {x1 - 4, y1 - 4, x1 + 4, y1 + 4};
            FillRect(dc, &head, hb);
            DeleteObject(hb);
        }

    // The panel plate, then the GUI-only strip, so the two never blur together
    {
        RECT pr = {kPx, kPy, kPx + kPW, kPy + kPH};
        HBRUSH pb = CreateSolidBrush(RGB(26, 28, 35));
        FillRect(dc, &pr, pb);
        DeleteObject(pb);
        HBRUSH fb = CreateSolidBrush(RGB(70, 74, 88));
        FrameRect(dc, &pr, fb);
        DeleteObject(fb);
        RECT gr = {kRx, kGuiY, W - 16, kGuiY + 160};
        HBRUSH gb = CreateSolidBrush(RGB(90, 82, 50));
        FrameRect(dc, &gr, gb);
        DeleteObject(gb);
        Text(dc, kRx + 8, kGuiY + 6, "GUI ONLY - not on the panel", RGB(200, 180, 100));
        Text(dc, kRx + 16, kGuiY + 130,
             "pads A S D F G H J / 1-7   oct Z X   vol - =   break B   "
             "settle N   cables M   F1-F8 + arrows: knob",
             dim);
    }

    for(int k = 0; k < kSwitches; k++)
    {
        Text(dc, sw_r[k][0].x, sw_r[k][0].y - 16, kSwName[k], dim);
        for(int c = 0; c < kSwCells[k]; c++)
        {
            // Cell 0 is up, so the drawing matches the hardware
            const bool on = sw_cell[k] == c;
            Box(dc, {sw_r[k][c].x, sw_r[k][c].y, sw_r[k][c].w, sw_r[k][c].h - 2},
                on ? RGB(80, 120, 190) : RGB(40, 43, 52));
            Text(dc, sw_r[k][c].x + 8, sw_r[k][c].y + 3, SwLabel(k, c),
                 on ? RGB(255, 255, 255) : dim);
        }
    }

    for(int i = 0; i < synth::kKnobs; i++)
    {
        const Rect r   = knob_r[i];
        if(r.w == 0)
            continue;
        const bool sel = i == sel_knob;
        const COLORREF c = sel ? ok : RGB(96, 118, 170);
        char s[32];
        snprintf(s, sizeof(s), "%.2f", synth::Knob(i));
        if(knob_vert[i])
        {
            const int cx = r.x + r.w / 2;
            char nm[2][16];
            snprintf(nm[0], 16, "%.3s", kKnobLabel[i]);
            snprintf(nm[1], 16, "%s", kKnobLabel[i] + 4);
            TextC(dc, cx, r.y - 36, nm[0], sel ? ok : dim);
            TextC(dc, cx, r.y - 20, nm[1], sel ? ok : dim);
            Box(dc, r, RGB(32, 34, 40));
            const int fh = (int)(r.h * synth::Knob(i));
            Box(dc, {r.x + 6, r.y + r.h - fh, r.w - 12, fh}, c);
            TextC(dc, cx, r.y + r.h + 4, s, sel ? ok : dim);
        }
        else
        {
            const int cx = r.x + r.w / 2, cy = r.y + 46, rad = 30;
            TextC(dc, cx, r.y, kKnobLabel[i], sel ? ok : dim);
            HPEN   pen = CreatePen(PS_SOLID, 2, c);
            HPEN   op  = (HPEN)SelectObject(dc, pen);
            HBRUSH ob2 = (HBRUSH)SelectObject(dc, CreateSolidBrush(RGB(36, 39, 48)));
            Ellipse(dc, cx - rad, cy - rad, cx + rad, cy + rad);
            // 270 degree sweep, minimum at lower left
            const double a = (225.0 - 270.0 * synth::Knob(i)) * 3.14159265 / 180.0;
            MoveToEx(dc, cx, cy, nullptr);
            LineTo(dc, cx + (int)(cos(a) * (rad - 4)), cy - (int)(sin(a) * (rad - 4)));
            DeleteObject(SelectObject(dc, ob2));
            SelectObject(dc, op);
            DeleteObject(pen);
            TextC(dc, cx, cy + rad + 4, s, sel ? ok : dim);
        }
    }

    for(int p = 0; p < synth::kPads; p++)
    {
        const bool on   = synth::PadOn(p);
        const bool conn = p == synth::kConnectorPad;
        const int  v    = synth::PadVoice(p);
        const int  grp  = v >= 0 ? wfc::GroupOf(v) : 0;
        Box(dc, pad_r[p], conn ? (on ? RGB(214, 140, 40) : RGB(70, 52, 30))
                               : Family(grp, on));
        const COLORREF tc = on ? RGB(255, 255, 255)
                               : (conn ? RGB(190, 150, 90) : FamilyText(grp));
        char s[48];
        snprintf(s, sizeof(s), "P%02d  %c", p + 3, (char)kPadKey[p]);
        Text(dc, pad_r[p].x + 6, pad_r[p].y + 6, s, tc);
        if(conn)
            snprintf(s, sizeof(s), "bridge");
        else
            snprintf(s, sizeof(s), "%c%d %.0fHz", grp ? 'R' : 'L', v + 1,
                     synth::PadHz(p));
        Text(dc, pad_r[p].x + 6, pad_r[p].y + 36, s, tc);
    }

    for(int i = 0; i < 5; i++)
    {
        const bool mapped = i != 1;
        Box(dc, xp_r[i], xp_on[i] ? RGB(150, 154, 168)
                                  : (mapped ? RGB(52, 55, 66) : RGB(34, 36, 43)));
        const COLORREF tc = xp_on[i] ? RGB(20, 20, 24) : (mapped ? dim : RGB(80, 84, 96));
        char s[32];
        if(kXpKey[i])
            snprintf(s, sizeof(s), "%s  %c", kXpName[i], kXpKey[i]);
        else
            snprintf(s, sizeof(s), "%s", kXpName[i]);
        Text(dc, xp_r[i].x + 6, xp_r[i].y + 4, s, tc);
        Text(dc, xp_r[i].x + 6, xp_r[i].y + xp_r[i].h - 20, kXpFn[i], tc);
    }

    BitBlt(real, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, of);
    DeleteObject(font);
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
    EndPaint(hw, &ps);
}

// Everything the engine is told from the window thread goes through here
struct Lock
{
    Lock() { EnterCriticalSection(&engine_cs); }
    ~Lock() { LeaveCriticalSection(&engine_cs); }
};

void XpAct(int i)
{
    if(i == 0 || i == 2)
        synth::ShiftOctave(i - 1);
    else if(i == 3 || i == 4)
        synth::ShiftGain(i == 3 ? -1 : 1);
}

LRESULT CALLBACK Proc(HWND hw, UINT m, WPARAM w, LPARAM l)
{
    switch(m)
    {
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: Paint(hw); return 0;

        case WM_KEYDOWN:
        {
            Lock lk;
            if(l & 0x40000000)   // autorepeat: a pad press must be one press
                return 0;
            for(int p = 0; p < synth::kPads; p++)
                if((int)w == kPadKey[p] || (int)w == '1' + p)
                    synth::Press(p);
            if(w == 'B')
                synth::Break();
            if(w == 'N')
                synth::Settle();
            if(w == 'V')
                sw_cell[2] = (sw_cell[2] + 1) % kSwCells[2];   // spike
            if(w == 'M')
                sw_cell[1] = (sw_cell[1] + 2) % 3;   // cables
            if(w == 'C')
                sw_cell[3] = 1 - sw_cell[3];
            {
                const int xi = w == 'Z' ? 0 : w == 'X' ? 2
                               : w == VK_OEM_MINUS ? 3 : w == VK_OEM_PLUS ? 4 : -1;
                if(xi >= 0)
                {
                    XpAct(xi);
                    xp_on[xi] = true;
                }
            }
            if(w >= VK_F1 && w <= VK_F8)
                sel_knob = (int)w - VK_F1;
            if((w == VK_LEFT || w == VK_RIGHT) && sel_knob >= 0)
                synth::SetKnob(sel_knob,
                               min(1.0f, max(0.0f, synth::Knob(sel_knob)
                                                       + (w == VK_RIGHT ? 0.02f
                                                                        : -0.02f))));
            if(w == VK_ESCAPE)
            {
                for(int p = 0; p < synth::kPads; p++)
                    synth::Release(p);
                sw_cell[0] = 1;
            }
            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        }
        case WM_KEYUP:
        {
            Lock lk;
            for(int p = 0; p < synth::kPads; p++)
                if((int)w == kPadKey[p] || (int)w == '1' + p)
                    synth::Release(p);
            if(w == 'Z' || w == 'X' || w == VK_OEM_MINUS || w == VK_OEM_PLUS)
                xp_on[w == 'Z' ? 0 : w == 'X' ? 2 : w == VK_OEM_MINUS ? 3 : 4] = false;
            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN:
        {
            Lock lk;
            const int mx = LOWORD(l), my = HIWORD(l);
            for(int i = 0; i < synth::kKnobs; i++)
                if(mx >= knob_r[i].x && mx < knob_r[i].x + knob_r[i].w
                   && my >= knob_r[i].y && my < knob_r[i].y + knob_r[i].h)
                {
                    drag = i;
                    sel_knob = i;
                    drag_from = {mx, my};
                    drag_at   = synth::Knob(i);
                    SetCapture(hw);
                }
            for(int k = 0; k < kSwitches; k++)
                for(int c = 0; c < kSwCells[k]; c++)
                    if(mx >= sw_r[k][c].x && mx < sw_r[k][c].x + sw_r[k][c].w
                       && my >= sw_r[k][c].y
                       && my < sw_r[k][c].y + sw_r[k][c].h)
                        sw_cell[k] = c;
            for(int i = 0; i < 5; i++)
                if(mx >= xp_r[i].x && mx < xp_r[i].x + xp_r[i].w
                   && my >= xp_r[i].y && my < xp_r[i].y + xp_r[i].h)
                {
                    mouse_xp = i;
                    xp_on[i] = true;
                    XpAct(i);
                }
            for(int p = 0; p < synth::kPads; p++)
                if(mx >= pad_r[p].x && mx < pad_r[p].x + pad_r[p].w
                   && my >= pad_r[p].y && my < pad_r[p].y + pad_r[p].h)
                {
                    mouse_pad = p;
                    synth::Press(p);
                }
            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE:
            if(drag >= 0)
            {
                Lock      lk;
                // Faders are dragged up, knobs right
                const float d = knob_vert[drag]
                                    ? (float)(drag_from.y - (short)HIWORD(l))
                                          / (float)knob_r[drag].h
                                    : (float)(LOWORD(l) - drag_from.x)
                                          / (float)knob_r[drag].w;
                const float v = min(1.0f, max(0.0f, drag_at + d));
                if(drag == 5)
                    synth::SetAgitation(v);
                else
                    synth::SetKnob(drag, v);
                InvalidateRect(hw, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
        {
            Lock lk;
            drag = -1;
            ReleaseCapture();
            if(mouse_xp >= 0)
            {
                xp_on[mouse_xp] = false;
                mouse_xp = -1;
            }
            if(mouse_pad >= 0)
            {
                synth::Release(mouse_pad);
                mouse_pad = -1;
            }
            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        }
    }
    return DefWindowProcA(hw, m, w, l);
}

}  // namespace

int SelfTest();

int main(int argc, char** argv)
{
    for(int i = 1; i < argc; i++)
        if(!strcmp(argv[i], "--selftest"))
            return SelfTest();

    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    qpc_freq  = (double)f.QuadPart;
    start_qpc = (double)t.QuadPart;

    synth::Init((float)kSr, kBlock);

    WNDCLASSA wc = {};
    wc.lpfnWndProc   = Proc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "lapses";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    // Client area sized for the panel plus the cards; fits 1080p with its taskbar
    RECT wr = {0, 0, 1740, 940};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hw = CreateWindowA("lapses", "lapses", WS_OVERLAPPEDWINDOW,
                            0, 0, wr.right - wr.left, wr.bottom - wr.top,
                            nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hw, SW_SHOW);

    if(!AudioStart())
    {
        MessageBoxA(hw, "waveOut would not open", "lapses", MB_OK);
        return 1;
    }

    MSG msg;
    DWORD next_paint = 0;
    for(;;)
    {
        while(PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if(msg.message == WM_QUIT)
                goto done;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        {
            Lock lk;
            ApplySwitches();
        }
        const DWORD now = GetTickCount();
        if(now >= next_paint)
        {
            next_paint = now + 33;   // 30 fps is plenty for bars and text
            InvalidateRect(hw, nullptr, FALSE);
        }
        // 1 ms spun the window thread 1000 times a second, taking the engine
        // lock on every pass for no benefit. The switches still re-assert far
        // faster than a finger moves.
        Sleep(5);
    }
done:
    InterlockedExchange(&stop_audio, 1);
    SetEvent(wake);
    Sleep(30);
    if(hwi)
    {
        waveInReset(hwi);
        for(int i = 0; i < kBufs; i++)
            waveInUnprepareHeader(hwi, &ihdr[i], sizeof(WAVEHDR));
        waveInClose(hwi);
    }
    waveOutReset(hwo);
    for(int i = 0; i < kBufs; i++)
        waveOutUnprepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
    waveOutClose(hwo);
    (void)argc;
    (void)argv;
    return 0;
}
