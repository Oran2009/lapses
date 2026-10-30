// THE DESKTOP SHELL
//
// The same engine the Daisy runs, with a window instead of the Touch 2.
// Win32 + waveOut + GDI, so there is nothing to install: the Windows SDK that
// comes with the compiler is the whole dependency list.
//
//   pads     A S D F G H J   (or 1..7)
//   latch    space
//   octave   Z / X
//   volume   - / =
//   knobs    drag with the mouse, or F1..F6 then arrow keys
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
// waveOut double buffering. 8 blocks is about 11 ms, which is well inside
// what a synth with 30 ms minimum echoes needs.
constexpr int kBufs    = 8;
constexpr int kBufFrames = kBlock * 4;

HWAVEOUT   hwo = nullptr;
WAVEHDR    hdr[kBufs];
short      buf[kBufs][kBufFrames * 2];
volatile LONG queued = 0;
HANDLE     wake = nullptr;       // WaveProc signals, the audio thread fills
volatile LONG stop_audio = 0;
CRITICAL_SECTION engine_cs;      // the window thread's input vs the render

double start_qpc = 0, qpc_freq = 1;

float Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (float)(((double)t.QuadPart - start_qpc) / qpc_freq);
}

void Fill(WAVEHDR* h)
{
    float l[kBlock], r[kBlock];
    short* out = (short*)h->lpData;
    for(int off = 0; off < kBufFrames; off += kBlock)
    {
        synth::Control(Now());
        synth::Process(l, r, kBlock);
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
    CloseHandle(CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr));
    return true;
}

// Keeps every buffer in flight, on its own thread: the window thread stalls
// for as long as a window drag or a menu lasts, and the audio cannot wait.
DWORD WINAPI AudioThread(LPVOID)
{
    timeBeginPeriod(1);
    while(!stop_audio)
    {
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

int   sel_knob = -1;      // the knob F1..F6 picked
int   drag     = -1;      // the knob the mouse has hold of
int   mouse_pad = -1;     // the pad the mouse pressed, so key-held pads survive
POINT drag_from;
float drag_at;

const int kPadKey[synth::kPads] = {'A', 'S', 'D', 'F', 'G', 'H', 'J'};

struct Rect
{
    int x, y, w, h;
};
Rect knob_r[6], pad_r[synth::kPads];

void Layout(int W, int H)
{
    const int m = 16;
    const int kw = (W - m * 7) / 6;
    for(int i = 0; i < 6; i++)
        knob_r[i] = {m + i * (kw + m), H - 150, kw, 54};
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
             "oct %+d   out %.0f dB   %s   lapse %.2f  unrest %.2f/s",
             synth::Octave(), synth::GainDb(),
             synth::Latched() ? "LATCHED" : "       ", wfc::LapseChance(),
             wfc::UnrestRate());
    Text(dc, 16, 30, hdr2, dim);

    // The voice cards, showing the EFFECTIVE values, knob scaling and LFO
    // included, because that is what you hear (principle 8)
    const int cy = 58, ch = 92;
    const int cw = (W - 16 * 5) / 4;
    for(int v = 0; v < wfc::kVoices; v++)
    {
        const int x = 16 + v * (cw + 16);
        RECT      card = {x, cy, x + cw, cy + ch};
        HBRUSH    cb = CreateSolidBrush(wfc::Sounding(v) ? RGB(38, 42, 52)
                                                         : RGB(26, 28, 34));
        FillRect(dc, &card, cb);
        DeleteObject(cb);

        const int t = wfc::ShownTile(v), lo = wfc::ShownLfo(v);
        const float F = wfc::FL[wfc::TileF(t)] * wfc::FbMul();
        const float R = wfc::RL[wfc::TileR(t)] * wfc::DrvMul();
        const float D = wfc::DelayBase() * wfc::DR[wfc::TileD(t)]
                        * (wfc::ShownSkew(v) ? 1.19f : 1.0f);
        char s[128];
        snprintf(s, sizeof(s), "voice %d%s", v + 1,
                 wfc::ShownSkew(v) ? "  SKEW" : "");
        Text(dc, x + 8, cy + 6, s, wfc::Sounding(v) ? fgc : dim);
        snprintf(s, sizeof(s), "dly %4.0f ms", D * 1000.0f);
        Text(dc, x + 8, cy + 24, s, dim);
        snprintf(s, sizeof(s), "fb  %.2f%s", F, F > 1.0f ? " RING" : "");
        Text(dc, x + 8, cy + 40, s, F > 1.0f ? hot : dim);
        snprintf(s, sizeof(s), "drv %.2f", R);
        Text(dc, x + 8, cy + 56, s, dim);
        const int tgt = wfc::OptTarget(wfc::ShownPatch(v));
        snprintf(s, sizeof(s), "lfo %.1fHz x%.2f %s",
                 wfc::LfoRate0() * wfc::LR[wfc::LfoRate(lo)],
                 wfc::DP[wfc::LfoDepth(lo)] * wfc::LfoMul(),
                 tgt >= 0 ? "->" : "");
        Text(dc, x + 8, cy + 72, s, dim);
        if(tgt >= 0)
        {
            snprintf(s, sizeof(s), "%d", tgt + 1);
            Text(dc, x + cw - 18, cy + 72, s, ok);
        }
    }

    for(int i = 0; i < 6; i++)
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
                synth::Latch(!synth::Latched());
            if(w == 'Z')
                synth::ShiftOctave(-1);
            if(w == 'X')
                synth::ShiftOctave(1);
            if(w == VK_OEM_MINUS)
                synth::ShiftGain(-1);
            if(w == VK_OEM_PLUS)
                synth::ShiftGain(1);
            if(w >= VK_F1 && w <= VK_F6)
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
                synth::Latch(false);
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
            for(int i = 0; i < 6; i++)
                if(mx >= knob_r[i].x && mx < knob_r[i].x + knob_r[i].w
                   && my >= knob_r[i].y && my < knob_r[i].y + knob_r[i].h)
                {
                    drag = i;
                    sel_knob = i;
                    drag_from = {mx, my};
                    drag_at   = synth::Knob(i);
                    SetCapture(hw);
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
                            CW_USEDEFAULT, CW_USEDEFAULT, 980, 460, nullptr,
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
        const DWORD now = GetTickCount();
        if(now >= next_paint)
        {
            next_paint = now + 33;   // 30 fps is plenty for bars and text
            InvalidateRect(hw, nullptr, FALSE);
        }
        Sleep(1);
    }
done:
    InterlockedExchange(&stop_audio, 1);
    SetEvent(wake);
    Sleep(30);
    waveOutReset(hwo);
    for(int i = 0; i < kBufs; i++)
        waveOutUnprepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
    waveOutClose(hwo);
    (void)argc;
    (void)argv;
    return 0;
}
