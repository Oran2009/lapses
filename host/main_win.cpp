// THE DESKTOP SHELL
//
// The same engine the Daisy runs, with a window instead of the Touch 2.
// Win32 + waveOut + GDI, so there is nothing to install: the Windows SDK that
// comes with the compiler is the whole dependency list.
//
//   pads     A S D F G H J   (or 1..7)
//   switches click them, or space (latch), C (freeze), M (mod mode)
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
// The two panel switches, drawn and clicked as three stacked cells each, so
// they behave the way the Touch 2's do rather than as keyboard toggles.
// -1 down, 0 centre, +1 up.
constexpr int kSwitches = 2;
Rect sw_r[kSwitches][3];
int  sw_pos[kSwitches] = {0, 0};

const char* kSwLabel[kSwitches][3]
    = {{"freeze", "--", "latch"},
       {"cross-mod", "no cables", "cross-feed"}};
const char* kSwName[kSwitches] = {"S07/S08", "S09/S10"};

// Exactly what panel::Poll does with them, so there is one behaviour and not
// two. Latch is re-asserted while the switch is down, which is what lets a pad
// pressed mid-hold join the ones already latched.
void ApplySwitches()
{
    static bool was_down = false;
    const bool  down     = sw_pos[0] < 0;
    if(down || was_down)
        synth::Latch(down);
    was_down = down;
    synth::Freeze(sw_pos[0] > 0);
    synth::SetCableMode(sw_pos[1] > 0   ? voices::kCableMod
                        : sw_pos[1] < 0 ? voices::kCableFeed
                                        : voices::kCableOff);
}

void Layout(int W, int H)
{
    const int m = 16;
    const int kw = (W - m * (synth::kKnobs + 1)) / synth::kKnobs;
    for(int i = 0; i < synth::kKnobs; i++)
        knob_r[i] = {m + i * (kw + m), H - 140, kw, 54};
    // Between the voice cards and the knobs
    const int sy = 240, sh = 22, sw_w = 108;
    for(int k = 0; k < kSwitches; k++)
        for(int c = 0; c < 3; c++)
            sw_r[k][c] = {m + k * (sw_w + m * 3), sy + 16 + c * sh,
                          k == 2 ? sw_w + 24 : sw_w, sh};

    const int pw = (W - m * 8) / 7;
    for(int i = 0; i < synth::kPads; i++)
        pad_r[i] = {m + i * (pw + m), H - 80, pw, 64};
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

    Layout(W, H);
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
    Text(dc, 16, 12, line, nv ? hot : ok);

    char hdr2[160];
    snprintf(hdr2, sizeof(hdr2),
             "oct %+d   out %.0f dB   %s   lapse %.2f  "
             "unrest %.2f/s   cables %s",
             synth::Octave(), synth::GainDb(),
             synth::InputMix() > 0.5f
                     ? "INPUT  "
                     : (synth::Frozen()
                            ? "FROZEN "
                            : (synth::Latched() ? "LATCHED" : "       ")), wfc::LapseChance(),
             wfc::UnrestRate(), synth::CableName(synth::CableMode()));
    Text(dc, 16, 30, hdr2, dim);

    // The voice cards, showing the EFFECTIVE values, knob scaling and mod
    // included, because that is what you hear (principle 8)
    const int cy = 58, ch = 168;
    const int cw = (W - 16 * 5) / 4;
    for(int v = 0; v < wfc::kVoices; v++)
    {
        const int x = 16 + v * (cw + 16);
        RECT      card = {x, cy, x + cw, cy + ch};
        HBRUSH    cb = CreateSolidBrush(wfc::Sounding(v) ? RGB(38, 42, 52)
                                                         : RGB(26, 28, 34));
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
            snprintf(s, sizeof(s), "%c %.0fHz", (char)kPadKey[pad],
                     voices::Pitch(v));
            Text(dc, x + cw - 86, cy + 6, s, ok);
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
        Text(dc, x + 8, cy + 76, s, dim);
        Centred(dc, {x + 8, cy + 92, cw - 16, 8}, md / 1.5f,
                RGB(150, 130, 200));

        Text(dc, x + 8, cy + 104, "amp", dim);
        Bar(dc, {x + 42, cy + 106, cw - 50, 8}, voices::Amp(v), ok);
        Text(dc, x + 8, cy + 118, "out", dim);
        Bar(dc, {x + 42, cy + 120, cw - 50, 8},
            voices::LoopLevel(v) * 4.0f > 1.0f ? 1.0f
                                              : voices::LoopLevel(v) * 4.0f,
            RGB(96, 118, 170));
        Text(dc, x + 8, cy + 132, "pan", dim);
        Centred(dc, {x + 42, cy + 134, cw - 50, 8}, voices::Pan(v),
                RGB(130, 170, 200));

        const int tgt = wfc::OptTarget(wfc::ShownPatch(v));
        if(tgt >= 0)
        {
            snprintf(s, sizeof(s), "cable -> voice %d  x%.2f", tgt + 1,
                     wfc::AMT[wfc::OptAmt(wfc::ShownPatch(v))]);
            Text(dc, x + 8, cy + 148, s, ok);
        }
    }

    for(int k = 0; k < kSwitches; k++)
    {
        Text(dc, sw_r[k][0].x, sw_r[k][0].y - 16, kSwName[k], dim);
        for(int c = 0; c < 3; c++)
        {
            // Cell 0 is up, 2 is down, so the drawing matches the hardware
            const bool on = sw_pos[k] == 1 - c;
            RECT b = {sw_r[k][c].x, sw_r[k][c].y, sw_r[k][c].x + sw_r[k][c].w,
                      sw_r[k][c].y + sw_r[k][c].h - 2};
            HBRUSH sb = CreateSolidBrush(on ? RGB(80, 120, 190)
                                            : RGB(40, 43, 52));
            FillRect(dc, &b, sb);
            DeleteObject(sb);
            Text(dc, sw_r[k][c].x + 8, sw_r[k][c].y + 3, kSwLabel[k][c],
                 on ? RGB(255, 255, 255) : dim);
        }
    }

    for(int i = 0; i < synth::kKnobs; i++)
    {
        Bar(dc, {knob_r[i].x, knob_r[i].y + 20, knob_r[i].w, 14},
            synth::Knob(i), i == sel_knob ? ok : RGB(96, 118, 170));
        char s[64];
        snprintf(s, sizeof(s), "%s %.2f", synth::KnobName(i), synth::Knob(i));
        Text(dc, knob_r[i].x, knob_r[i].y, s, i == sel_knob ? ok : dim);
    }

    for(int p = 0; p < synth::kPads; p++)
    {
        const bool on = synth::PadOn(p);
        RECT       b  = {pad_r[p].x, pad_r[p].y, pad_r[p].x + pad_r[p].w,
                   pad_r[p].y + pad_r[p].h};
        HBRUSH     pb = CreateSolidBrush(on ? RGB(80, 120, 190)
                                            : RGB(40, 43, 52));
        FillRect(dc, &b, pb);
        DeleteObject(pb);
        char s[48];
        snprintf(s, sizeof(s), "%c  %.0fHz", (char)kPadKey[p], synth::PadHz(p));
        Text(dc, pad_r[p].x + 6, pad_r[p].y + 6, s, on ? RGB(255, 255, 255)
                                                       : dim);
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
            if(w == VK_SPACE)
                sw_pos[0] = sw_pos[0] < 0 ? 0 : -1;
            if(w == 'B')
                synth::Break();
            if(w == 'N')
                synth::Settle();
            if(w == 'M')
                sw_pos[1] = sw_pos[1] == 1 ? -1 : sw_pos[1] + 1;   // cables
            if(w == 'C')
                sw_pos[0] = sw_pos[0] > 0 ? 0 : 1;
            if(w == 'Z')
                synth::ShiftOctave(-1);
            if(w == 'X')
                synth::ShiftOctave(1);
            if(w == VK_OEM_MINUS)
                synth::ShiftGain(-1);
            if(w == VK_OEM_PLUS)
                synth::ShiftGain(1);
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
                sw_pos[0] = 0;
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
                for(int c = 0; c < 3; c++)
                    if(mx >= sw_r[k][c].x && mx < sw_r[k][c].x + sw_r[k][c].w
                       && my >= sw_r[k][c].y
                       && my < sw_r[k][c].y + sw_r[k][c].h)
                        sw_pos[k] = 1 - c;
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
                const int dx = LOWORD(l) - drag_from.x;
                float     v  = drag_at + (float)dx / (float)knob_r[drag].w;
                synth::SetKnob(drag, min(1.0f, max(0.0f, v)));
                InvalidateRect(hw, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
        {
            Lock lk;
            drag = -1;
            ReleaseCapture();
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
    HWND hw = CreateWindowA("lapses", "lapses", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1060, 560, nullptr,
                            nullptr, wc.hInstance, nullptr);
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
        // lock on every pass for no benefit. The latch still re-asserts far
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
