// Glassy v2.0.1 - a round "liquid glass" screen magnifier for Windows (Win32 / C, no dependencies)
//
// Hotkey:      Ctrl+Alt+Z toggles the lens on/off
// While on:    Mouse wheel = zoom, Shift+Wheel = lens size
// Tray icon:   left click = toggle, right click = menu (settings, size, startup, exit)
//
// Architecture (designed to stay lag-free):
//  - Hook thread : only listens for mouse wheel events, never draws (the mouse can never stall)
//  - Lens thread : capture -> liquid-glass scaling -> UpdateLayeredWindow (position + image in one step),
//                  paced by DwmFlush to the display refresh, skips drawing when nothing changed
//  - Main thread : tray icon, hotkey, toast notification, settings window
//
// License: MIT

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <dwmapi.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x11
#endif

// ---- Defaults ------------------------------------------------------
#define HOTKEY_MOD   (MOD_CONTROL | MOD_ALT | MOD_NOREPEAT)
#define HOTKEY_VK    'Z'
#define ZOOM_MIN_Q   1250      // 1.25x (x1000)
#define ZOOM_MAX_Q   20000     // 20x
#define ZOOM_DEF_Q   2500
#define LENS_MIN     150
#define LENS_MAX     900
#define LENS_DEF     400
#define LENS_STEP    40
#define RING_W       3.0
#define MAXSRC       1200
// --------------------------------------------------------------------

#define WM_TRAY       (WM_APP + 1)
#define WM_HOOK_ON    (WM_APP + 20)
#define WM_HOOK_OFF   (WM_APP + 21)
#define WM_LENS_SHOW  (WM_APP + 30)
#define WM_LENS_HIDE  (WM_APP + 31)
#define ID_TRAY       1
#define ID_HOTKEY     1
#define IDM_TOGGLE    100
#define IDM_SETTINGS  101
#define IDM_STARTUP   102
#define IDM_EXIT      103
#define IDM_SIZE0     300
#define T_TOASTHIDE   2
#define T_FADE        3
#define ID_TBSIZE     210
#define ID_TBZOOM     211
#define ID_CHKSTART   212
#define ID_CHKCTRL    213
#define ID_BTNTEST    214
#define ID_BTNCLOSE   215

static const wchar_t *RUN_KEY  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t *APP_KEY  = L"Software\\Glassy";
static const wchar_t *APP_NAME = L"Glassy";

static const int  PRESETS[4]      = { 250, 400, 600, 800 };
static const wchar_t *PRESET_NAME[4] = { L"Small", L"Medium", L"Large", L"Extra large" };

static HINSTANCE g_inst;
static HWND g_main, g_lens, g_toast, g_set;
static HANDLE g_lensReady, g_hookReady;
static DWORD g_hookTid;
static volatile LONG g_active;
static volatile LONG g_visible;
static volatile LONG g_size    = LENS_DEF;
static volatile LONG g_zoomQ   = ZOOM_DEF_Q;
static volatile LONG g_ctrlOnly = 0;
static volatile LONG g_affOk   = 1;   // 0: this Windows build cannot exclude the lens from capture
static BOOL g_toastOn;
static int  g_toastAlpha;
static UINT g_wmTaskbar;
static NOTIFYICONDATAW g_nid;

// ====================== Registry (settings + startup) =================
static BOOL StartupEnabled(void) {
    HKEY k; BOOL ok = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        ok = RegQueryValueExW(k, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return ok;
}
static void SetStartup(BOOL on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH + 4], val[MAX_PATH + 8];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        wsprintfW(val, L"\"%s\"", path);
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (BYTE *)val, (DWORD)((lstrlenW(val) + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, APP_NAME);
    RegCloseKey(k);
}
static DWORD GetDw(HKEY k, const wchar_t *n, DWORD def) {
    DWORD v, sz = sizeof(v), t;
    if (RegQueryValueExW(k, n, NULL, &t, (BYTE *)&v, &sz) == ERROR_SUCCESS && t == REG_DWORD) return v;
    return def;
}
static void SetDw(HKEY k, const wchar_t *n, DWORD v) { RegSetValueExW(k, n, 0, REG_DWORD, (BYTE *)&v, sizeof(v)); }

static LONG Clamp(LONG v, LONG lo, LONG hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void LoadSettings(void) {
    HKEY k; DWORD disp;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APP_KEY, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &k, &disp) != ERROR_SUCCESS) return;
    if (disp == REG_CREATED_NEW_KEY) SetStartup(TRUE);        // enable autostart on first run
    g_size     = Clamp((LONG)GetDw(k, L"Size", LENS_DEF), LENS_MIN, LENS_MAX);
    g_zoomQ    = Clamp((LONG)GetDw(k, L"ZoomQ", ZOOM_DEF_Q), ZOOM_MIN_Q, ZOOM_MAX_Q);
    g_ctrlOnly = GetDw(k, L"CtrlOnly", 0) ? 1 : 0;
    RegCloseKey(k);
}
static void SaveSettings(void) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APP_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    SetDw(k, L"Size", (DWORD)g_size);
    SetDw(k, L"ZoomQ", (DWORD)g_zoomQ);
    SetDw(k, L"CtrlOnly", (DWORD)g_ctrlOnly);
    RegCloseKey(k);
}

// ====================== Hook thread (mouse events only) ==============
static HHOOK g_hook;

static LRESULT CALLBACK MouseProc(int c, WPARAM w, LPARAM l) {
    if (c == HC_ACTION) {
        if (w == WM_MOUSEWHEEL) {
            if (g_ctrlOnly && !(GetAsyncKeyState(VK_CONTROL) & 0x8000))
                return CallNextHookEx(NULL, c, w, l);          // no Ctrl: let the wheel scroll normally
            MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)l;
            short d = (short)HIWORD(m->mouseData);
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
                InterlockedExchange(&g_size, Clamp(g_size + (d > 0 ? LENS_STEP : -LENS_STEP), LENS_MIN, LENS_MAX));
            } else {
                double z = (g_zoomQ / 1000.0) * pow(1.15, d / 120.0);
                InterlockedExchange(&g_zoomQ, Clamp((LONG)(z * 1000.0 + 0.5), ZOOM_MIN_Q, ZOOM_MAX_Q));
            }
            
            return 1;                                           // swallow the wheel event
        }
    }
    return CallNextHookEx(NULL, c, w, l);
}

static DWORD WINAPI HookThread(LPVOID p) {
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    MSG m; PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE);           // create the message queue
    g_hookTid = GetCurrentThreadId();
    SetEvent(g_hookReady);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_HOOK_ON) {
            if (!g_hook) g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, g_inst, 0);
        } else if (m.message == WM_HOOK_OFF) {
            if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
        }
    }
    if (g_hook) UnhookWindowsHookEx(g_hook);
    return 0;
}

// ====================== Lens thread (rendering + liquid glass) =======
// GLASS-BEGIN
// Liquid glass: light refraction at the edge, chromatic aberration, a bright specular rim,
// a soft glint and an inner shadow. All heavy math is precomputed into tables whenever the
// size changes; every frame only does sampling + blending.
#define GL_K      0.45f    // edge refraction strength
#define GL_R0     0.70f    // radius ratio where the refraction band starts
#define GL_CA_R0  0.86f    // chromatic aberration start radius
#define GL_CA     0.050f   // chromatic aberration strength

static int g_gsz;
static uint8_t *g_cov, *g_aw, *g_ak, *g_ca;
static int16_t *g_offG, *g_offR, *g_offB;

static float sstep(float a, float b, float x) {
    float t = (x - a) / (b - a);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return t * t * (3.f - 2.f * t);
}
static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static void GlassFree(void) {
    free(g_cov); free(g_aw); free(g_ak); free(g_ca); free(g_offG); free(g_offR); free(g_offB);
    g_cov = g_aw = g_ak = g_ca = NULL; g_offG = g_offR = g_offB = NULL;
}

static void GlassBuild(int sz) {
    GlassFree();
    size_t n = (size_t)sz * sz;
    g_cov = (uint8_t *)malloc(n); g_aw = (uint8_t *)malloc(n); g_ak = (uint8_t *)malloc(n); g_ca = (uint8_t *)malloc(n);
    g_offG = (int16_t *)malloc(n * 2 * sizeof(int16_t));
    g_offR = (int16_t *)malloc(n * 2 * sizeof(int16_t));
    g_offB = (int16_t *)malloc(n * 2 * sizeof(int16_t));
    float R = sz / 2.0f;
    const float lx = -0.70710678f, ly = -0.70710678f;           // light comes from the top-left
    for (int y = 0; y < sz; y++) {
        for (int x = 0; x < sz; x++) {
            size_t i = (size_t)y * sz + x;
            float nx = (x + 0.5f - R) / R, ny = (y + 0.5f - R) / R;
            float r = sqrtf(nx * nx + ny * ny);
            float dist = (1.f - r) * R;                         // pixels inward from the edge
            g_cov[i] = (uint8_t)(clamp01(dist + 0.5f) * 255.f + 0.5f);

            // --- refraction: a wider area gets squeezed in near the edge
            float rr = r > 1.f ? 1.f : r;
            float t = sstep(GL_R0, 1.f, rr);
            float f = 1.f + GL_K * t * t;
            float ox = nx * f, oy = ny * f;
            g_offG[2 * i]     = (int16_t)lrintf(ox * 4096.f);
            g_offG[2 * i + 1] = (int16_t)lrintf(oy * 4096.f);
            float tc = sstep(GL_CA_R0, 1.f, rr);
            g_ca[i] = (r >= GL_CA_R0 && r < 1.f) ? 1 : 0;
            float fr = 1.f + GL_CA * tc, fb = 1.f - GL_CA * tc;
            g_offR[2 * i]     = (int16_t)lrintf(ox * fr * 4096.f);
            g_offR[2 * i + 1] = (int16_t)lrintf(oy * fr * 4096.f);
            g_offB[2 * i]     = (int16_t)lrintf(ox * fb * 4096.f);
            g_offB[2 * i + 1] = (int16_t)lrintf(oy * fb * 4096.f);

            // --- light: direction-dependent brightness along the edge (strong top-left, weak bottom-right)
            float cosl = r > 1e-4f ? (nx * lx + ny * ly) / r : 0.f;
            float I1 = cosl > 0 ? cosl * cosl : 0.f;
            float I2 = cosl < 0 ? cosl * cosl : 0.f;
            float dirI = 0.28f + 0.72f * fmaxf(I1, 0.65f * I2);

            float d1 = dist - 1.3f;
            float rim = expf(-(d1 * d1) / 2.42f);               // thin bright rim line
            float aw = rim * dirI * 0.95f;
            float sheen = sstep(0.80f, 1.f, rr) * (0.05f + 0.20f * fmaxf(I1, 0.6f * I2));
            aw = 1.f - (1.f - aw) * (1.f - sheen);
            float gx = nx + 0.38f, gy = ny + 0.52f;
            float glint = 0.09f * expf(-(gx * gx + gy * gy) * 9.f);   // soft glint at the top-left
            aw = 1.f - (1.f - aw) * (1.f - glint);

            float d2 = dist - 5.5f;
            float inner = expf(-(d2 * d2) / 12.5f) * 0.10f * (1.f - I1); // thin shadow just inside the rim
            float ak = inner + 0.10f * sstep(0.82f, 1.f, rr) * (1.f - I1);
            g_aw[i] = (uint8_t)(clamp01(aw) * 255.f + 0.5f);
            g_ak[i] = (uint8_t)(clamp01(ak) * 255.f + 0.5f);
        }
    }
    g_gsz = sz;
}

static inline uint32_t lerp32(uint32_t a, uint32_t b, uint32_t t) {
    uint32_t rb = (((a & 0x00FF00FFu) * (256 - t) + (b & 0x00FF00FFu) * t) >> 8) & 0x00FF00FFu;
    uint32_t g  = (((a & 0x0000FF00u) * (256 - t) + (b & 0x0000FF00u) * t) >> 8) & 0x0000FF00u;
    return rb | g;
}
static inline uint32_t bilin(const uint32_t *s, int stride, int lim, int X, int Y) {
    if (X < 0) X = 0; else if (X > lim) X = lim;
    if (Y < 0) Y = 0; else if (Y > lim) Y = lim;
    int x0 = X >> 16, y0 = Y >> 16;
    uint32_t fx = (X >> 8) & 255, fy = (Y >> 8) & 255;
    const uint32_t *r0 = s + (size_t)y0 * stride + x0, *r1 = r0 + stride;
    return lerp32(lerp32(r0[0], r0[1], fx), lerp32(r1[0], r1[1], fx), fy);
}

// src: captured sc x sc pixel area (center = cursor), sw: source width covered by the lens
static void GlassRender(const uint32_t *src, int stride, int sc, int sw, uint32_t *out) {
    size_t n = (size_t)g_gsz * g_gsz;
    int cfp = (sc / 2) << 16;
    int lim = ((sc - 1) << 16) - 1;
    int mul = sw * 8;
    for (size_t i = 0; i < n; i++) {
        unsigned cv = g_cov[i];
        if (!cv) { out[i] = 0; continue; }
        uint32_t c = bilin(src, stride, lim, cfp + g_offG[2 * i] * mul, cfp + g_offG[2 * i + 1] * mul);
        if (g_ca[i]) {
            uint32_t cr = bilin(src, stride, lim, cfp + g_offR[2 * i] * mul, cfp + g_offR[2 * i + 1] * mul);
            uint32_t cb = bilin(src, stride, lim, cfp + g_offB[2 * i] * mul, cfp + g_offB[2 * i + 1] * mul);
            c = (cr & 0x00FF0000u) | (c & 0x0000FF00u) | (cb & 0x000000FFu);
        }
        unsigned aw = g_aw[i], ak = g_ak[i];
        if (aw | ak) {
            unsigned R = (c >> 16) & 255, G = (c >> 8) & 255, B = c & 255;
            if (aw) { R += ((255 - R) * aw + 127) / 255; G += ((255 - G) * aw + 127) / 255; B += ((255 - B) * aw + 127) / 255; }
            if (ak) { R = R * (255 - ak) / 255; G = G * (255 - ak) / 255; B = B * (255 - ak) / 255; }
            c = (R << 16) | (G << 8) | B;
        }
        if (cv == 255) out[i] = c | 0xFF000000u;
        else {
            unsigned R = ((c >> 16) & 255) * cv / 255, G = ((c >> 8) & 255) * cv / 255, B = (c & 255) * cv / 255;
            out[i] = (cv << 24) | (R << 16) | (G << 8) | B;
        }
    }
}
// GLASS-END

static HDC g_dcLens, g_dcSrc;
static HBITMAP g_bmpLens, g_bmpSrc, g_oldLens, g_oldSrc;
static uint32_t *g_pix, *g_srcPix, *g_prev;
static int g_cur;
static int g_dirty = 1;
static int g_kx, g_ky, g_ksz, g_ksc;
static LONG g_kzq;

static void LensResize(int sz) {
    if (g_bmpLens) { SelectObject(g_dcLens, g_oldLens); DeleteObject(g_bmpLens); g_bmpLens = NULL; }
    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sz; bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    g_bmpLens = CreateDIBSection(g_dcLens, &bi, DIB_RGB_COLORS, (void **)&g_pix, NULL, 0);
    g_oldLens = (HBITMAP)SelectObject(g_dcLens, g_bmpLens);
    GlassBuild(sz);
    g_cur = sz;
    g_dirty = 1;
}

static void LensRender(void) {
    POINT p; GetCursorPos(&p);
    int sz = (int)g_size; LONG zq = g_zoomQ;
    if (sz != g_cur) LensResize(sz);

    double z = zq / 1000.0;
    int sw = (int)(sz / z + 0.5);
    if (sw < 1) sw = 1;
    int sc = (int)(sw * 1.5) + 4;                               // extra margin for edge refraction
    if (sc > MAXSRC) sc = MAXSRC;

    HDC scr = GetDC(NULL);
    BitBlt(g_dcSrc, 0, 0, sc, sc, scr, p.x - sc / 2, p.y - sc / 2, SRCCOPY);
    ReleaseDC(NULL, scr);
    GdiFlush();

    // skip drawing if nothing changed (cursor, settings, screen content) -> ~0 CPU when idle
    int same = !g_dirty && p.x == g_kx && p.y == g_ky && sz == g_ksz && zq == g_kzq && sc == g_ksc;
    if (same) {
        for (int y = 0; y < sc; y++)
            if (memcmp(g_prev + (size_t)y * MAXSRC, g_srcPix + (size_t)y * MAXSRC, (size_t)sc * 4)) { same = 0; break; }
    }
    if (same) return;
    for (int y = 0; y < sc; y++)
        memcpy(g_prev + (size_t)y * MAXSRC, g_srcPix + (size_t)y * MAXSRC, (size_t)sc * 4);
    g_kx = p.x; g_ky = p.y; g_ksz = sz; g_kzq = zq; g_ksc = sc; g_dirty = 0;

    GlassRender(g_srcPix, MAXSRC, sc, sw, g_pix);

    POINT dst = { p.x - sz / 2, p.y - sz / 2 }, srcp = { 0, 0 };
    SIZE s = { sz, sz };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_lens, NULL, &dst, &s, g_dcLens, &srcp, 0, &bf, ULW_ALPHA);
}

// The lens is ALWAYS excluded from screen capture (GDI BitBlt, screenshots, recordings, streams).
// Glassy captures the screen under the lens itself; if the lens window were part of that capture,
// the lens would magnify itself and show lenses nested inside lenses (this happens on Windows 10,
// where GDI captures layered windows). WDA_EXCLUDEFROMCAPTURE needs Windows 10 version 2004 or later.
static void ExcludeLensFromCapture(HWND h) {
    if (SetWindowDisplayAffinity(h, WDA_EXCLUDEFROMCAPTURE)) InterlockedExchange(&g_affOk, 1);
    else InterlockedExchange(&g_affOk, 0);
}

static LRESULT CALLBACK LensProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_LENS_SHOW:
        ExcludeLensFromCapture(h);
        InterlockedExchange(&g_visible, 1);
        g_dirty = 1; LensRender();
        ShowWindow(h, SW_SHOWNOACTIVATE);
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        g_dirty = 1; LensRender();
        return 0;
    case WM_LENS_HIDE:
        InterlockedExchange(&g_visible, 0);
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    case WM_NCHITTEST: return HTTRANSPARENT;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI LensThread(LPVOID p) {
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    g_lens = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                             L"GlassyLens", L"", WS_POPUP, 0, 0, LENS_DEF, LENS_DEF, NULL, NULL, g_inst, NULL);
    ExcludeLensFromCapture(g_lens);                             // never capture ourselves (no lens inside lens)

    g_dcLens = CreateCompatibleDC(NULL);
    g_dcSrc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = MAXSRC; bi.bmiHeader.biHeight = -MAXSRC;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    g_bmpSrc = CreateDIBSection(g_dcSrc, &bi, DIB_RGB_COLORS, (void **)&g_srcPix, NULL, 0);
    g_oldSrc = (HBITMAP)SelectObject(g_dcSrc, g_bmpSrc);
    g_prev = (uint32_t *)calloc((size_t)MAXSRC * MAXSRC, 4);
    LensResize((int)g_size);
    SetEvent(g_lensReady);

    ULONGLONG lastTop = 0;
    MSG m;
    for (;;) {
        if (!g_visible) MsgWaitForMultipleObjects(0, NULL, FALSE, INFINITE, QS_ALLINPUT);   // sleep while hidden
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) return 0;
            TranslateMessage(&m); DispatchMessageW(&m);
        }
        if (g_visible) {
            LensRender();
            ULONGLONG now = GetTickCount64();
            if (now - lastTop > 500) {                          // reclaim the top spot if another topmost window covers us
                SetWindowPos(g_lens, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                lastTop = now;
            }
            if (FAILED(DwmFlush())) Sleep(8);                   // sync to the display refresh (60/144 Hz)
        }
    }
}

// ====================== Notification (toast) ==========================
static void ShowToast(BOOL on) {
    g_toastOn = on;
    KillTimer(g_main, T_TOASTHIDE); KillTimer(g_main, T_FADE);
    g_toastAlpha = 235;
    SetLayeredWindowAttributes(g_toast, 0, (BYTE)g_toastAlpha, LWA_ALPHA);
    POINT p; GetCursorPos(&p);
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
    int w = 250, h = 56;
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    int y = mi.rcWork.top + 30;
    SetWindowPos(g_toast, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_toast, NULL, TRUE);
    UpdateWindow(g_toast);
    SetTimer(g_main, T_TOASTHIDE, 1100, NULL);
}

static LRESULT CALLBACK ToastProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        HBRUSH bg = CreateSolidBrush(RGB(30, 30, 34));
        FillRect(dc, &r, bg); DeleteObject(bg);
        HBRUSH dot = CreateSolidBrush(g_toastOn ? RGB(70, 210, 110) : RGB(235, 75, 75));
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        HGDIOBJ ob = SelectObject(dc, dot);
        Ellipse(dc, 20, 18, 40, 38);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(dot);
        HFONT f = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ of = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        RECT tr = { 54, 0, r.right - 10, r.bottom };
        DrawTextW(dc, g_toastOn ? L"Glassy ON" : L"Glassy OFF", -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        SelectObject(dc, of); DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ====================== Toggle ========================================
static void SetActive(BOOL on) {
    if ((BOOL)g_active == on) return;
    InterlockedExchange(&g_active, on);
    if (on) {
        PostThreadMessageW(g_hookTid, WM_HOOK_ON, 0, 0);
        PostMessageW(g_lens, WM_LENS_SHOW, 0, 0);
    } else {
        PostThreadMessageW(g_hookTid, WM_HOOK_OFF, 0, 0);
        PostMessageW(g_lens, WM_LENS_HIDE, 0, 0);
        SaveSettings();                                         // also persist a size changed with Shift+wheel
    }
    ShowToast(on);
}

// ====================== Settings window ===============================
static HWND g_tbSize, g_tbZoom, g_lblSize, g_lblZoom, g_chkStart, g_chkCtrl;
static HFONT g_font;

static void UpdateLabels(void) {
    wchar_t b[64];
    wsprintfW(b, L"Lens size: %d px", (int)g_size);
    SetWindowTextW(g_lblSize, b);
    int zq = (int)g_zoomQ;
    wsprintfW(b, L"Zoom level: %d.%02dx", zq / 1000, (zq % 1000) / 10);
    SetWindowTextW(g_lblZoom, b);
}
static void ApplySize(int s) {
    InterlockedExchange(&g_size, Clamp(s, LENS_MIN, LENS_MAX));
    
    SaveSettings();
}
static int GetDpi(void) {
    typedef UINT (WINAPI *F)(void);
    F f = (F)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem");
    return f ? (int)f() : 96;
}
static BOOL CALLBACK SetFontProc(HWND h, LPARAM l) { SendMessageW(h, WM_SETFONT, (WPARAM)l, TRUE); return TRUE; }

static LRESULT CALLBACK SettingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_HSCROLL: {
        HWND tb = (HWND)l;
        int pos = (int)SendMessageW(tb, TBM_GETPOS, 0, 0);
        if (tb == g_tbSize) ApplySize(pos);
        else if (tb == g_tbZoom) {
            InterlockedExchange(&g_zoomQ, Clamp(pos * 250, ZOOM_MIN_Q, ZOOM_MAX_Q));
             SaveSettings();
        }
        UpdateLabels();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_CHKSTART: SetStartup(SendMessageW(g_chkStart, BM_GETCHECK, 0, 0) == BST_CHECKED); break;
        case ID_CHKCTRL:
            InterlockedExchange(&g_ctrlOnly, SendMessageW(g_chkCtrl, BM_GETCHECK, 0, 0) == BST_CHECKED);
            SaveSettings(); break;
        case ID_BTNTEST: SetActive(!g_active); break;
        case ID_BTNCLOSE: DestroyWindow(h); break;
        }
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        SaveSettings();
        if (g_font) { DeleteObject(g_font); g_font = NULL; }
        g_set = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void OpenSettings(void) {
    if (g_set) { ShowWindow(g_set, SW_SHOW); SetForegroundWindow(g_set); return; }
    int dpi = GetDpi();
#define S(v) MulDiv((v), dpi, 96)
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, S(400), S(322) };
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    g_set = CreateWindowExW(0, L"GlassySettings", L"Glassy Settings", style, x, y, ww, wh, NULL, NULL, g_inst, NULL);
    HWND hw = g_set;

    g_lblSize = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, S(20), S(18), S(360), S(22), hw, NULL, g_inst, NULL);
    g_tbSize  = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS,
                                S(14), S(42), S(372), S(34), hw, (HMENU)ID_TBSIZE, g_inst, NULL);
    SendMessageW(g_tbSize, TBM_SETRANGE, TRUE, MAKELONG(LENS_MIN, LENS_MAX));
    SendMessageW(g_tbSize, TBM_SETTICFREQ, 50, 0);
    SendMessageW(g_tbSize, TBM_SETLINESIZE, 0, 10);
    SendMessageW(g_tbSize, TBM_SETPAGESIZE, 0, 50);
    SendMessageW(g_tbSize, TBM_SETPOS, TRUE, g_size);

    g_lblZoom = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, S(20), S(92), S(360), S(22), hw, NULL, g_inst, NULL);
    g_tbZoom  = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS,
                                S(14), S(116), S(372), S(34), hw, (HMENU)ID_TBZOOM, g_inst, NULL);
    SendMessageW(g_tbZoom, TBM_SETRANGE, TRUE, MAKELONG(5, 80));       // 1.25x .. 20x (0.25 steps)
    SendMessageW(g_tbZoom, TBM_SETTICFREQ, 4, 0);
    SendMessageW(g_tbZoom, TBM_SETLINESIZE, 0, 1);
    SendMessageW(g_tbZoom, TBM_SETPAGESIZE, 0, 4);
    SendMessageW(g_tbZoom, TBM_SETPOS, TRUE, g_zoomQ / 250);

    g_chkStart = CreateWindowExW(0, L"BUTTON", L"Run at Windows startup",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, S(20), S(168), S(360), S(24), hw, (HMENU)ID_CHKSTART, g_inst, NULL);
    SendMessageW(g_chkStart, BM_SETCHECK, StartupEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    g_chkCtrl = CreateWindowExW(0, L"BUTTON", L"Zoom with the wheel only while Ctrl is held",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, S(20), S(198), S(360), S(24), hw, (HMENU)ID_CHKCTRL, g_inst, NULL);
    SendMessageW(g_chkCtrl, BM_SETCHECK, g_ctrlOnly ? BST_CHECKED : BST_UNCHECKED, 0);

    CreateWindowExW(0, L"STATIC", L"Hotkey: Ctrl+Alt+Z (toggle)\nWhile active: Shift+Wheel changes the lens size",
                    WS_CHILD | WS_VISIBLE, S(20), S(232), S(360), S(40), hw, NULL, g_inst, NULL);
    CreateWindowExW(0, L"BUTTON", L"Try it (toggle)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    S(20), S(278), S(170), S(32), hw, (HMENU)ID_BTNTEST, g_inst, NULL);
    CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    S(210), S(278), S(170), S(32), hw, (HMENU)ID_BTNCLOSE, g_inst, NULL);

    g_font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    EnumChildWindows(hw, SetFontProc, (LPARAM)g_font);
    UpdateLabels();
    ShowWindow(hw, SW_SHOW);
    SetForegroundWindow(hw);
#undef S
}

// ====================== Tray icon =====================================
static HICON MakeIcon(void) {
    HDC sdc = GetDC(NULL); HDC m = CreateCompatibleDC(sdc);
    HBITMAP color = CreateCompatibleBitmap(sdc, 32, 32);
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, NULL);
    HGDIOBJ old = SelectObject(m, color);
    RECT r = { 0, 0, 32, 32 };
    HBRUSH bg = CreateSolidBrush(RGB(30, 90, 200)); FillRect(m, &r, bg); DeleteObject(bg);
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
    HGDIOBJ op = SelectObject(m, pen), ob = SelectObject(m, GetStockObject(NULL_BRUSH));
    Ellipse(m, 4, 4, 22, 22); MoveToEx(m, 20, 20, NULL); LineTo(m, 28, 28);
    SelectObject(m, ob); SelectObject(m, op); DeleteObject(pen);
    SelectObject(m, mask); PatBlt(m, 0, 0, 32, 32, BLACKNESS);
    SelectObject(m, old);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask); DeleteDC(m); ReleaseDC(NULL, sdc);
    return h;
}
static void AddTray(void) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main; g_nid.uID = ID_TRAY;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    if (!g_nid.hIcon) g_nid.hIcon = MakeIcon();
    lstrcpyW(g_nid.szTip, L"Glassy (Ctrl+Alt+Z)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void TrayMenu(void) {
    HMENU mn = CreatePopupMenu(), sub = CreatePopupMenu();
    for (int i = 0; i < 4; i++) {
        wchar_t b[64]; wsprintfW(b, L"%s (%d px)", PRESET_NAME[i], PRESETS[i]);
        AppendMenuW(sub, MF_STRING | (g_size == PRESETS[i] ? MF_CHECKED : 0), IDM_SIZE0 + i, b);
    }
    AppendMenuW(mn, MF_STRING, IDM_TOGGLE, g_active ? L"Turn off\tCtrl+Alt+Z" : L"Turn on\tCtrl+Alt+Z");
    AppendMenuW(mn, MF_STRING, IDM_SETTINGS, L"Settings...");
    AppendMenuW(mn, MF_POPUP, (UINT_PTR)sub, L"Lens size");
    AppendMenuW(mn, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), IDM_STARTUP,
                L"Run at Windows startup");
    AppendMenuW(mn, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mn, MF_STRING, IDM_EXIT, L"Exit");
    SetMenuDefaultItem(mn, IDM_TOGGLE, FALSE);
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(g_main);
    int cmd = TrackPopupMenu(mn, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_main, NULL);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(mn);
    if (cmd == IDM_TOGGLE) SetActive(!g_active);
    else if (cmd == IDM_SETTINGS) OpenSettings();
    else if (cmd == IDM_STARTUP) SetStartup(!StartupEnabled());
    else if (cmd == IDM_EXIT) DestroyWindow(g_main);
    else if (cmd >= IDM_SIZE0 && cmd < IDM_SIZE0 + 4) {
        ApplySize(PRESETS[cmd - IDM_SIZE0]);
        if (g_set) { SendMessageW(g_tbSize, TBM_SETPOS, TRUE, g_size); UpdateLabels(); }
    }
}

// ====================== Main window ===================================
static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_wmTaskbar) { AddTray(); return 0; }              // re-add the tray icon if Explorer restarted
    switch (m) {
    case WM_HOTKEY: SetActive(!g_active); return 0;
    case WM_TIMER:
        if (w == T_TOASTHIDE) { KillTimer(h, T_TOASTHIDE); SetTimer(h, T_FADE, 16, NULL); }
        else if (w == T_FADE) {
            g_toastAlpha -= 25;
            if (g_toastAlpha <= 0) { KillTimer(h, T_FADE); ShowWindow(g_toast, SW_HIDE); }
            else SetLayeredWindowAttributes(g_toast, 0, (BYTE)g_toastAlpha, LWA_ALPHA);
        }
        return 0;
    case WM_TRAY:
        if (l == WM_LBUTTONUP) SetActive(!g_active);
        else if (l == WM_RBUTTONUP) TrayMenu();
        return 0;
    case WM_DESTROY:
        SetActive(FALSE);
        SaveSettings();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        UnregisterHotKey(h, ID_HOTKEY);
        PostThreadMessageW(g_hookTid, WM_QUIT, 0, 0);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmd, int show) {
    (void)hp; (void)cmd; (void)show;
    g_inst = hi;

    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\GlassySingleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    LoadSettings();

    g_lensReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_hookReady = CreateEventW(NULL, TRUE, FALSE, NULL);

    WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
    wc.hInstance = hi; wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"GlassyMain";  wc.lpfnWndProc = MainProc;  RegisterClassW(&wc);
    wc.lpszClassName = L"GlassyLens";  wc.lpfnWndProc = LensProc;  RegisterClassW(&wc);
    wc.lpszClassName = L"GlassyToast"; wc.lpfnWndProc = ToastProc; RegisterClassW(&wc);
    wc.lpszClassName = L"GlassySettings"; wc.lpfnWndProc = SettingsProc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); RegisterClassW(&wc);

    g_main = CreateWindowExW(0, L"GlassyMain", L"Glassy", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    g_toast = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                              L"GlassyToast", L"", WS_POPUP, 0, 0, 250, 56, NULL, NULL, hi, NULL);
    SetLayeredWindowAttributes(g_toast, 0, 235, LWA_ALPHA);
    SetWindowRgn(g_toast, CreateRoundRectRgn(0, 0, 251, 57, 28, 28), FALSE);

    CreateThread(NULL, 0, HookThread, NULL, 0, NULL);
    CreateThread(NULL, 0, LensThread, NULL, 0, NULL);
    WaitForSingleObject(g_hookReady, 5000);
    WaitForSingleObject(g_lensReady, 5000);

    if (!g_affOk)
        MessageBoxW(NULL, L"This Windows version cannot hide the lens from screen capture, so the lens may appear "
                          L"inside itself.\n\nPlease update to Windows 10 version 2004 (build 19041) or later.",
                    L"Glassy", MB_ICONWARNING);

    g_wmTaskbar = RegisterWindowMessageW(L"TaskbarCreated");
    AddTray();
    if (!RegisterHotKey(g_main, ID_HOTKEY, HOTKEY_MOD, HOTKEY_VK))
        MessageBoxW(NULL, L"The Ctrl+Alt+Z hotkey is already in use by another application.", L"Glassy", MB_ICONWARNING);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_set && IsDialogMessageW(g_set, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    timeEndPeriod(1);
    if (mtx) CloseHandle(mtx);
    return 0;
}
