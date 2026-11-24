// Standalone adaptation of mod.wh.cpp (Unlicense, sidlikesgrapess).
// No Windhawk runtime or process injection is used.
#include <windows.h>
#include "blend.h"
#include "trail.h"
#include <atomic>
#include <mmsystem.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef CURMB_SHARP_HEAD
#define CURMB_SHARP_HEAD 0
#endif
#ifndef CURMB_FAST_FRAMES
#define CURMB_FAST_FRAMES 0
#endif

#define HOLD_MS        10      // how long after the last fast moment the cursor may come back
#define SHOW_WINDOW_MS 12      // the speed that brings the cursor back is measured over just this many ms
// ~px of travel one copy covers at cursor size 32 (keeps the opacity independent of copy density)
#define COVER_PX       10.0
#define MAX_SAMPLES    512
#define MAX_COPIES     100     // the most copies spread along the trail in one frame
#define COPY_BUF       (MAX_SAMPLES + MAX_COPIES)
#define MAX_SPRITES    16      // cursor pictures kept before the cache is emptied
#define WM_APP_PAUSED  (WM_APP + 1)

struct Sample { int x, y; LONGLONG t; };
struct Sprite { int w, h, hx, hy, id; UINT32* px; };   // cursor picture at its own size: premultiplied BGRA, hotspot

// Values baked in from settings.txt.
static constexpr int g_opacity = 45;
static constexpr int g_trailMs = 20;
static constexpr int g_drawSpeedCm = 50;
static constexpr bool g_hideWhenFast = true;
static constexpr int g_hideSpeedCm = 90;
static constexpr bool g_pauseInFullscreen = true;

// shared between the threads (guarded by g_gate)
static CRITICAL_SECTION g_gate;
static Sample g_hist[MAX_SAMPLES];
static int g_histN;
static HCURSOR g_curHandle;
static bool g_curVisible, g_hideWanted;
static int g_monCursor = 32;          // cursor width on the monitor the cursor is on
static std::atomic<int> g_monGap{7};     // ms between frames: half a refresh of the monitor the cursor is on
static std::atomic<bool> g_blankActive;  // the system cursors are currently swapped for an invisible one
static std::atomic<bool> g_drawFailed;   // the overlay could not draw: then never hide the real cursor
static std::atomic<bool> g_monStale, g_spritesStale;   // display or cursor settings changed
static std::atomic<bool> g_recheck;      // the active window changed: look at the window in front now
static std::atomic<bool> g_paused;       // a fullscreen app is in front
static std::atomic<bool> g_drawing;      // the blur is drawn: above the minimum blur speed, or while the cursor is hidden
static std::atomic<HCURSOR> g_pictureOf; // the cursor whose picture the blur has: hiding waits until it is the current one
static std::atomic<bool> g_stop;
static std::atomic<HWND> g_wnd;          // the overlay window (owned by the render thread)

static HANDLE g_moved, g_mouseWake, g_hideChanged;   // auto-reset events
static HANDLE g_ready;                               // set when the render thread has set up (or failed to)
static HANDLE g_threads[3];
static LARGE_INTEGER g_freq, g_start;

static LONGLONG Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (t.QuadPart - g_start.QuadPart) * 1000 / g_freq.QuadPart;
}

static double Dist(Sample a, Sample b) {
    double dx = b.x - a.x, dy = b.y - a.y;
    return sqrt(dx * dx + dy * dy);
}

static int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// A monitor's refresh rate.
static int RefreshRate(HMONITOR mon) {
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
        dm.dmDisplayFrequency > 1)
        return dm.dmDisplayFrequency;
    return 60;
}

// A monitor's physical pixel density, from the size Windows reports for it (its DPI setting if that looks wrong).
static double PixelsPerCm(HMONITOR mon, UINT dpi) {
    double fallback = dpi / 2.54 < 20 ? 20 : dpi / 2.54;
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return fallback;
    HDC dc = CreateDCW(L"DISPLAY", mi.szDevice, nullptr, nullptr);
    if (!dc) return fallback;
    int mm = GetDeviceCaps(dc, HORZSIZE), px = GetDeviceCaps(dc, HORZRES);
    DeleteDC(dc);
    return mm >= 150 && mm <= 2500 && px >= 320 ? px / (mm / 10.0) : fallback;
}

static bool FullscreenAppRunning() {
    QUERY_USER_NOTIFICATION_STATE s;
    return SHQueryUserNotificationState(&s) == S_OK &&
           (s == QUNS_BUSY || s == QUNS_RUNNING_D3D_FULL_SCREEN || s == QUNS_PRESENTATION_MODE);
}

// The pointer size from Settings > Accessibility > Mouse pointer and touch, at 100% scaling: 32 px at size 1 and 16 px
// more per step. (The cursor pictures Windows hands out can stay at size 1 when this is larger.)
static int PointerSize() {
    DWORD size = 0, len = sizeof(size);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize", RRF_RT_REG_DWORD, nullptr, &size,
                     &len) == ERROR_SUCCESS && size >= 32 && size <= 256)
        return (int)size;
    return 32;
}

// The persistent marker is written before changing system cursors. The watchdog
// restores them even if this process is terminated without running cleanup.
static constexpr wchar_t kRegistryPath[] = L"Software\\curmb";
static constexpr wchar_t kMutexName[] = L"Local\\curmb-motion-blur";
static HANDLE g_watchdog;

static bool SetHiddenMarker(bool hidden) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    DWORD value = hidden ? 1 : 0;
    LSTATUS result = RegSetValueExW(key, L"CursorsHidden", 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

static bool HiddenMarker() {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kRegistryPath, L"CursorsHidden", RRF_RT_REG_DWORD,
                        nullptr, &value, &size) == ERROR_SUCCESS && value != 0;
}

static bool RestoreCursors() {
    if (!SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0)) return false;
    SetHiddenMarker(false);
    return true;
}

// A process's integrity level (medium for normal apps, high for administrator ones), or -1 if its token can't be read.
static int Integrity(HANDLE process) {
    int level = -1;
    HANDLE token;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        alignas(TOKEN_MANDATORY_LABEL) BYTE buf[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_MANDATORY_LABEL)];
        DWORD len;
        if (GetTokenInformation(token, TokenIntegrityLevel, buf, sizeof(buf), &len)) {
            PSID sid = ((TOKEN_MANDATORY_LABEL*)buf)->Label.Sid;
            level = (int)*GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
        }
        CloseHandle(token);
    }
    return level;
}

// Whether a window belongs to a process with a higher integrity level than this one (an administrator app such as
// Task Manager or an admin terminal): Windows sends this process no raw mouse input meanwhile.
static int g_ownIntegrity;   // set at start

static bool HigherRights(HWND w) {
    DWORD pid = 0;
    if (!w || !GetWindowThreadProcessId(w, &pid)) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return true;
    int level = Integrity(p);
    CloseHandle(p);
    return level < 0 || level > g_ownIntegrity;
}

// ---- sampler ----

static DWORD WINAPI SampleThread(void*) {
    RECT monRect = {};          // the monitor the cursor is on
    HWND lastFront = nullptr;
    int pointerSize = 0;        // the pointer size set in Windows, at 100% scaling (0: not read yet)
    POINT lastPt = {LONG_MIN, LONG_MIN};   // where the cursor was at the last look
    UINT mainDpi = 96;
    double mainPxPerCm = 0;     // pixel density of the main monitor (0: not measured yet)
    double hidePxPerCm = 38;    // turns the hide speed (cm/s on the main monitor) into px/s on the monitor in use
    LONGLONG lastMove = 0, lastFast = 0, lastDrawn = 0, lastLook = -1000, crossedAt = 0;
    bool paused = false, timerHigh = false, adminFront = false;
    bool fastEnough = false;    // above the minimum blur speed (switched only after 16 ms past the line)
    while (!g_stop) {
        // The window in front: whenever another window is activated (the shell hook says so, administrator windows
        // included), and every half second while the mouse moves, as an app can also go fullscreen in place.
        bool forced = g_recheck.exchange(false);
        LONGLONG t = Now();
        if (forced || t - lastLook >= 500) {
            lastLook = t;
            HWND front = GetForegroundWindow();
            bool frontChanged = front != lastFront;
            if (frontChanged) {
                lastFront = front;
                bool admin = HigherRights(front);
                adminFront = admin;
            }
            // A fullscreen app: looked for when the front window changes, and while the mouse moves or the application is
            // paused, as an app can also go fullscreen (or leave it) in place.
            if (forced || frontChanged || paused || t - lastMove < 1000) {
                bool was = paused;
                paused = g_pauseInFullscreen && FullscreenAppRunning();
                g_paused = paused;
                if (paused != was) {
                    // stop listening to the mouse during the game
                    if (g_wnd) PostMessageW(g_wnd, WM_APP_PAUSED, paused, 0);
                }
            }
        }
        if (paused) {
            EnterCriticalSection(&g_gate);
            g_curVisible = false;
            g_histN = 0;
            if (g_hideWanted) { g_hideWanted = false; SetEvent(g_hideChanged); }
            LeaveCriticalSection(&g_gate);
            if (timerHigh) { timeEndPeriod(1); timerHigh = false; }
            WaitForSingleObject(g_mouseWake, 500);
            continue;
        }

        CURSORINFO ci;
        ci.cbSize = sizeof(ci);
        bool showing = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
        // Crossing to another monitor (or display or cursor settings changed): measure it here, outside the lock, as
        // some of these calls are slow.
        bool stale = showing && g_monStale.exchange(false);
        // Windows tells no one when the pointer size changes: look at it again when the mouse moves after a pause.
        bool resized = false;
        if (showing && (ci.ptScreenPos.x != lastPt.x || ci.ptScreenPos.y != lastPt.y)) {
            if (pointerSize && Now() - lastMove >= 1000) {
                int size = PointerSize();
                resized = size != pointerSize;
                pointerSize = size;
            }
            lastPt = ci.ptScreenPos;
        }
        bool newMon = showing && (stale || resized || !PtInRect(&monRect, ci.ptScreenPos));
        int monCursor = 32;
        if (newMon) {
            HMONITOR mon = MonitorFromPoint(ci.ptScreenPos, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) monRect = mi.rcMonitor;
            UINT dpi = 96, dy;
            GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dy);
            if (stale || !pointerSize) pointerSize = PointerSize();
            // Windows scales the pointer in steps, not with the monitor's scaling: x1 up to 125%, x1.5 up to 175%, x2 up
            // to 250%... SM_CXCURSOR is the size-1 pointer at this scaling (32, 48, 64, 96 px).
            monCursor = MulDiv(pointerSize, GetSystemMetricsForDpi(SM_CXCURSOR, dpi), 32);
            int hz = RefreshRate(mon);
            // The hide speed is set in cm/s as measured on the main monitor. Windows moves the pointer further on a
            // monitor with more scaling (twice the pixels at 200%), so scale it by the DPI to hide at the same hand
            // speed on every monitor.
            if (stale || !mainPxPerCm) {
                POINT origin = {0, 0};
                HMONITOR mainMon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
                GetDpiForMonitor(mainMon, MDT_EFFECTIVE_DPI, &mainDpi, &dy);
                mainPxPerCm = PixelsPerCm(mainMon, mainDpi);
            }
            hidePxPerCm = mainPxPerCm * dpi / mainDpi;
            // Half a refresh: frames are not timed to the screen's refreshes, so a whole one would leave the blur
            // fresh on some refreshes and almost a refresh old on others, and it would jitter against the pointer.
            g_monGap = FrameGapMs(hz, CURMB_FAST_FRAMES != 0);
        }

        EnterCriticalSection(&g_gate);
        if (!showing) {
            g_curVisible = false;
            g_histN = 0;
            if (g_hideWanted) { g_hideWanted = false; SetEvent(g_hideChanged); }
        } else {
            g_curVisible = true;
            if (!g_blankActive) g_curHandle = ci.hCursor;   // keep the real cursor's picture while it is hidden
            bool changed = g_histN == 0 || g_hist[g_histN - 1].x != ci.ptScreenPos.x ||
                           g_hist[g_histN - 1].y != ci.ptScreenPos.y;

            // The pointer jumped across a large part of the desktop in one step: not a hand movement (PowerToys Cursor
            // Wrap sends it from one edge to the other, an app can call SetCursorPos). No hand covers 40% of the
            // desktop between two mouse reports.
            bool jumped = false;
            if (changed && g_histN) {
                int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                int dx = abs(ci.ptScreenPos.x - g_hist[g_histN - 1].x), dy = abs(ci.ptScreenPos.y - g_hist[g_histN - 1].y);
                jumped = dx * 5 > vw * 2 || dy * 5 > vh * 2;
            }

            if (newMon) {   // drop the trail so it doesn't smear across the gap between monitors
                g_histN = 0;
                g_monCursor = monCursor;
            } else if (jumped) {   // likewise across a jump
                g_histN = 0;
            }

            LONGLONG now = Now();
            if (changed && g_histN && g_hist[g_histN - 1].t == now) {   // several reports within a ms: keep the newest
                g_hist[g_histN - 1].x = ci.ptScreenPos.x;
                g_hist[g_histN - 1].y = ci.ptScreenPos.y;
            } else if (changed || g_histN == 0 || now - g_hist[g_histN - 1].t >= 4) {
                if (g_histN == MAX_SAMPLES) {
                    memmove(g_hist, g_hist + 1, (MAX_SAMPLES - 1) * sizeof(Sample));
                    g_histN--;
                }
                g_hist[g_histN++] = {ci.ptScreenPos.x, ci.ptScreenPos.y, now};
            }
            if (changed) {
                if (now - lastMove >= 1000) lastLook = -1000;   // moving again: first see what is in front
                lastMove = now;
            }
            int drop = 0;
            while (g_histN - drop > 1 && now - g_hist[drop].t > g_trailMs) drop++;
            if (drop) {
                memmove(g_hist, g_hist + drop, (g_histN - drop) * sizeof(Sample));
                g_histN -= drop;
            }

            // Speed over the trail window: the blur is drawn above the draw speed and stops below half of it, each only
            // once the speed has stayed past that line for 16 ms (mouse reports arrive unevenly, so a single reading can
            // be a third off). The real cursor is hidden when very fast; bringing it back looks at only the last few ms
            // (from sample j on), so it returns as soon as the mouse stops or slows.
            LONGLONG dt = g_hist[g_histN - 1].t - g_hist[0].t;
            int j = g_histN - 1;
            while (j > 0 && now - g_hist[j - 1].t <= SHOW_WINDOW_MS) j--;
            double path = 0, recentPath = 0;
            for (int i = 1; i < g_histN; i++) {
                double d = Dist(g_hist[i - 1], g_hist[i]);
                path += d;
                if (i > j) recentPath += d;
            }
            double speed = dt >= 4 ? path * 1000.0 / dt : 0;
            double drawAt = g_drawSpeedCm * hidePxPerCm * (fastEnough ? 0.5 : 1);
            bool want = g_histN > 1 && (g_drawSpeedCm == 0 || (dt >= 4 && speed >= drawAt));
            if (want == fastEnough || g_histN < 2 || g_drawSpeedCm == 0) {
                crossedAt = 0;
                fastEnough = want;
            } else if (!crossedAt) {
                crossedAt = now;
            } else if (now - crossedAt >= 16) {
                crossedAt = 0;
                fastEnough = want;
            }
            bool wasHidden = g_hideWanted;
            if (!g_hideWhenFast || g_drawFailed) {
                g_hideWanted = false;
            } else if (dt >= 8) {
                double hide = g_hideSpeedCm * hidePxPerCm;   // px/s on this monitor, the same hand speed everywhere
                LONGLONG recentDt = now - g_hist[j].t;
                double recent = recentDt >= 4 ? recentPath * 1000.0 / recentDt : speed;
                if (speed > hide) {
                    g_hideWanted = true;
                    lastFast = now;
                } else if (g_hideWanted && recent < hide * 0.5 && now - lastFast > HOLD_MS) {
                    g_hideWanted = false;
                }
            }
            if (g_hideWanted && now - lastMove > HOLD_MS + SHOW_WINDOW_MS) g_hideWanted = false;   // stopped
            if (g_hideWanted != wasHidden) SetEvent(g_hideChanged);
            // The minimum blur speed only decides drawing; hiding goes by its own speed. A hidden cursor always gets its
            // blur, so the pointer never disappears.
            g_drawing = fastEnough || g_hideWanted;
            if ((changed || g_hideWanted != wasHidden) && g_drawing) {   // draw now
                lastDrawn = now;
                SetEvent(g_moved);
            }
        }
        bool hidden = g_hideWanted;
        LeaveCriticalSection(&g_gate);

        // drawing (or the trail still fading), or the real cursor hidden: look often; below the draw speed, only on reports
        if (Now() - lastDrawn < g_trailMs + 40 || hidden) {
            if (!timerHigh) { timeBeginPeriod(1); timerHigh = true; }
            // look again on every mouse report; the timeout keeps the path ageing after a stop, and checks every 2 ms
            // while the real cursor is hidden so it comes back quickly
            WaitForSingleObject(g_mouseWake, hidden ? 2 : 10);
        } else {
            if (timerHigh) { timeEndPeriod(1); timerHigh = false; }
            // Sleep until the mouse moves or another window is activated; while an app hides the cursor, just look once
            // per report. While a window with more rights is in front no mouse reports arrive, so look at the cursor
            // every 10 ms instead, and every 100 ms once the mouse has been still for a second.
            if (fastEnough) {   // the trail has faded: the next movement starts below the minimum blur speed again
                fastEnough = false;
                g_drawing = false;
                crossedAt = 0;
            }
            DWORD poll = INFINITE;
            if (adminFront) poll = Now() - lastMove < 1000 ? 10 : 100;
            WaitForSingleObject(g_mouseWake, poll);
        }
    }
    if (timerHigh) timeEndPeriod(1);
    return 0;
}

// ---- hider ----

static DWORD WINAPI HideThread(void*) {
    // static system cursors blanked while the mouse is fast (the animated wait/app-starting ones are left alone)
    static const UINT ids[] = {32512, 32513, 32515, 32516, 32642, 32643, 32644,
                               32645, 32646, 32648, 32649, 32651, 32671, 32672};
    BYTE andMask[128], xorMask[128];
    memset(andMask, 0xFF, sizeof(andMask));
    memset(xorMask, 0, sizeof(xorMask));
    while (!g_stop) {
        EnterCriticalSection(&g_gate);
        bool want = g_hideWanted;
        HCURSOR cur = g_curHandle;
        LeaveCriticalSection(&g_gate);
        // Swap only once the blur has this cursor's picture: from the invisible one it can't be taken. The render thread
        // takes it on its next frame (a few ms) and wakes this thread.
        if (want && !g_blankActive && g_pictureOf == cur && g_watchdog &&
            WaitForSingleObject(g_watchdog, 0) == WAIT_TIMEOUT && SetHiddenMarker(true)) {
            g_blankActive = true;
            for (UINT id : ids) {
                HCURSOR c = CreateCursor(nullptr, 0, 0, 32, 32, andMask, xorMask);
                if (c && !SetSystemCursor(c, id)) DestroyCursor(c);   // on success the system owns it
            }
        } else if (g_blankActive && (!want || !g_watchdog ||
                                      WaitForSingleObject(g_watchdog, 0) != WAIT_TIMEOUT)) {
            if (RestoreCursors()) g_blankActive = false;
        }
        // Also detect a lost watchdog while cursors are hidden.
        WaitForSingleObject(g_hideChanged, g_blankActive ? 50 : INFINITE);
    }
    if (g_blankActive) {   // never leave the user with an invisible cursor
        if (RestoreCursors()) g_blankActive = false;
    }
    return 0;
}

// ---- cursor pictures (render thread only) ----

static struct { HCURSOR h; int size; Sprite* sp; } g_sprites[MAX_SPRITES];   // keyed by cursor and drawn size
static int g_nSprites, g_nextId;
// Cursors an app made invisible on purpose: no blur for them. Checked again after a while, because another tool that
// hides the cursor the way this program does (swapping the system cursors) would otherwise switch the blur off for good.
static struct { HCURSOR h; LONGLONG t; } g_empties[MAX_SPRITES];
static int g_nEmpties;
static Sprite* g_lastSprite;             // the last real picture, drawn while the real cursor is hidden

// The standard cursors, with their names in the cursor scheme (HKCU\Control Panel\Cursors).
static const struct { WORD id; PCWSTR name; } kStandardCursors[] = {
    {32512, L"Arrow"},    {32513, L"IBeam"},    {32514, L"Wait"},     {32515, L"Crosshair"}, {32516, L"UpArrow"},
    {32631, L"NWPen"},    {32642, L"SizeNWSE"}, {32643, L"SizeNESW"}, {32644, L"SizeWE"},    {32645, L"SizeNS"},
    {32646, L"SizeAll"},  {32648, L"No"},       {32649, L"Hand"},     {32650, L"AppStarting"}, {32651, L"Help"},
    {32671, L"Pin"},      {32672, L"Person"},
};

// The cursor at size x size, for the caller to destroy: a standard cursor is loaded again from its scheme file (or from
// Windows' own cursors) at that size, so a large pointer stays sharp; any other is copied, enlarged if need be.
static HCURSOR AtSize(HCURSOR h, int size) {
    for (const auto& c : kStandardCursors) {
        if (LoadCursorW(nullptr, MAKEINTRESOURCEW(c.id)) != h) continue;
        WCHAR path[MAX_PATH];
        DWORD len = sizeof(path);
        HANDLE loaded = nullptr;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", c.name, RRF_RT_REG_SZ, nullptr, path, &len) ==
                ERROR_SUCCESS && path[0])
            loaded = LoadImageW(nullptr, path, IMAGE_CURSOR, size, size, LR_LOADFROMFILE);
        if (!loaded) loaded = LoadImageW(nullptr, MAKEINTRESOURCEW(c.id), IMAGE_CURSOR, size, size, 0);
        if (loaded) return (HCURSOR)loaded;
        break;
    }
    return (HCURSOR)CopyImage(h, IMAGE_CURSOR, size, size, 0);
}

// A cursor's picture size and hotspot.
static bool CursorInfo(HCURSOR h, int* w, int* ht, POINT* hot) {
    ICONINFO ii;
    if (!GetIconInfo(h, &ii)) return false;
    BITMAP bm = {};
    GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
    *w = bm.bmWidth;
    *ht = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;   // a monochrome mask holds AND and XOR halves
    *hot = {(LONG)ii.xHotspot, (LONG)ii.yHotspot};
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return true;
}

// Takes the cursor's picture by drawing it on black and on white: what shows through gives the opacity. It is taken at
// the size it will be drawn (up to 256 px): the cursor as it is when that is its size, else a copy at that size.
static Sprite* Grab(HCURSOR h, int size, bool* empty) {
    *empty = false;
    if (size > 256) size = 256;
    int w, ht;
    POINT hot;
    if (!CursorInfo(h, &w, &ht, &hot)) return nullptr;
    HCURSOR sized = w != size ? AtSize(h, size) : nullptr;
    if (sized && CursorInfo(sized, &w, &ht, &hot)) h = sized;
    if (w <= 0 || ht <= 0 || w > 256 || ht > 256) {
        if (sized) DestroyCursor(sized);
        return nullptr;
    }

    int n = w * ht;
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -ht, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    Sprite* sp = dib ? (Sprite*)malloc(sizeof(Sprite) + n * 4) : nullptr;
    if (sp) {
        HGDIOBJ old = SelectObject(dc, dib);
        UINT32 *px = (UINT32*)(sp + 1), *onWhite = (UINT32*)bits;
        memset(bits, 0, n * 4);
        DrawIconEx(dc, 0, 0, h, w, ht, 0, nullptr, DI_NORMAL);
        GdiFlush();
        memcpy(px, bits, n * 4);   // on black
        memset(bits, 0xFF, n * 4);
        DrawIconEx(dc, 0, 0, h, w, ht, 0, nullptr, DI_NORMAL);
        GdiFlush();
        bool any = false;
        for (int i = 0; i < n; i++) {
            UINT32 b = px[i];
            // how much of the background shows through
            int a = 255 - ((int)(onWhite[i] >> 8 & 255) - (int)(b >> 8 & 255));
            a = Clamp(a, 0, 255);
            // on black = premultiplied colour
            UINT32 r = b >> 16 & 255, g = b >> 8 & 255, bl = b & 255, ua = (UINT32)a;
            px[i] = ua << 24 | (r < ua ? r : ua) << 16 | (g < ua ? g : ua) << 8 | (bl < ua ? bl : ua);
            any |= a != 0;
        }
        SelectObject(dc, old);
        *sp = {w, ht, (int)hot.x, (int)hot.y, ++g_nextId, px};
        if (!any) { free(sp); sp = nullptr; *empty = true; }
    }
    if (dib) DeleteObject(dib);
    if (dc) DeleteDC(dc);
    if (sized) DestroyCursor(sized);
    return sp;
}

static void FreeSprites() {
    for (int i = 0; i < g_nSprites; i++) free(g_sprites[i].sp);
    g_nSprites = g_nEmpties = 0;
    g_lastSprite = nullptr;
}

static bool IsEmpty(HCURSOR h) {
    for (int i = 0; i < g_nEmpties; i++)
        if (g_empties[i].h == h) {
            if (Now() - g_empties[i].t < 2000) return true;
            g_empties[i] = g_empties[--g_nEmpties];   // look at it again
            return false;
        }
    return false;
}

static Sprite* GetSprite(HCURSOR h, int size) {
    for (int i = 0; i < g_nSprites; i++)
        if (g_sprites[i].h == h && g_sprites[i].size == size) return g_sprites[i].sp;
    if (IsEmpty(h)) return nullptr;
    bool hiddenBefore = g_blankActive, empty;
    Sprite* sp = Grab(h, size, &empty);
    // Only keep a picture taken while the real cursor is showing: blanking replaces the content under the same handle.
    if (sp && g_blankActive) { free(sp); sp = nullptr; }
    if (sp) {
        if (g_nSprites == MAX_SPRITES) FreeSprites();
        g_sprites[g_nSprites++] = {h, size, sp};
    }
    // Empty while our own blanking was off the whole time: the app itself shows an invisible cursor.
    if (empty && !hiddenBefore && !g_blankActive) {
        if (g_nEmpties == MAX_SPRITES) g_nEmpties = 0;
        g_empties[g_nEmpties++] = {h, Now()};
    }
    return sp;
}

// ---- drawing (render thread only) ----


static HDC g_memDc;
static HBITMAP g_dib;
static HGDIOBJ g_dibOld;
// the canvas: premultiplied BGRA, top-down, g_canvasW wide; frames use its top-left corner
static UINT32* g_bits;
static int g_canvasW, g_canvasH;
static UINT32* g_pic;          // the cursor picture at the drawn size
static int* g_span;            // per row of g_pic: first and one-past-last visible pixel
static int g_picId, g_picW, g_picH;
static bool g_shown;

// Where the window drawn in sits (screen coordinates; g_placed: the window that was put there) and the part of it the
// last frame drew on (window and canvas coordinates). Windows tells the window under a still pointer about every move or
// resize of a window above it, as if the mouse had moved, and an app like Office then redraws its hovered menu item
// over and over. So the window is larger than the blur and stays put while the pointer is still.
static HWND g_placed;
static RECT g_win, g_inked;

static void ReleaseCanvas() {
    if (g_dib) {
        SelectObject(g_memDc, g_dibOld);
        DeleteObject(g_dib);
        g_dib = nullptr;
    }
    g_canvasW = g_canvasH = 0;
    g_inked = {};
}

static void EnsureCanvas(int w, int h) {   // grows only, in 128 px steps
    if (g_dib && w <= g_canvasW && h <= g_canvasH) return;
    int nw = (w + 127) / 128 * 128, nh = (h + 127) / 128 * 128;
    if (nw < g_canvasW) nw = g_canvasW;
    if (nh < g_canvasH) nh = g_canvasH;
    ReleaseCanvas();
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), nw, -nh, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    g_dib = CreateDIBSection(g_memDc, &bi, DIB_RGB_COLORS, (void**)&g_bits, nullptr, 0);
    if (!g_dib) return;
    g_dibOld = SelectObject(g_memDc, g_dib);
    g_canvasW = nw;
    g_canvasH = nh;
}

// One pixel of the cursor picture resized to w x h, on premultiplied colours: the average of the pixels it covers when
// shrinking, bilinear when growing.
static UINT32 Pixel(const Sprite* sp, int x, int y, int w, int h) {
    float acc[4] = {}, n = 0;
    if (sp->w > w) {
        int x0 = x * sp->w / w, x1 = (x + 1) * sp->w / w, y0 = y * sp->h / h, y1 = (y + 1) * sp->h / h;
        if (x1 <= x0) x1 = x0 + 1;
        if (y1 <= y0) y1 = y0 + 1;
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++, n++)
                for (int s = 0; s < 4; s++) acc[s] += sp->px[yy * sp->w + xx] >> (8 * s) & 255;
    } else {
        float fx = (x + 0.5f) * sp->w / w - 0.5f, fy = (y + 0.5f) * sp->h / h - 0.5f;
        if (fx < 0) fx = 0;
        if (fy < 0) fy = 0;
        int x0 = (int)fx, y0 = (int)fy, x1 = x0 + 1 < sp->w ? x0 + 1 : x0, y1 = y0 + 1 < sp->h ? y0 + 1 : y0;
        float ax = fx - x0, ay = fy - y0, wt[4] = {(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay};
        UINT32 p[4] = {sp->px[y0 * sp->w + x0], sp->px[y0 * sp->w + x1],
                       sp->px[y1 * sp->w + x0], sp->px[y1 * sp->w + x1]};
        for (int i = 0; i < 4; i++)
            for (int s = 0; s < 4; s++) acc[s] += (p[i] >> (8 * s) & 255) * wt[i];
        n = 1;
    }
    UINT32 out = 0;
    for (int s = 0; s < 4; s++) out |= (UINT32)(acc[s] / n + 0.5f) << (8 * s);
    return out;
}

// The cursor picture at the drawn size, made again only when the cursor or the size changes.
static bool Resize(const Sprite* sp, int w, int h) {
    if (g_pic && g_picId == sp->id && g_picW == w && g_picH == h) return true;
    free(g_pic);
    g_pic = (UINT32*)malloc(w * h * 4 + h * 2 * sizeof(int));
    if (!g_pic) return false;
    g_span = (int*)(g_pic + w * h);
    g_picId = sp->id;
    g_picW = w;
    g_picH = h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            g_pic[y * w + x] = w == sp->w && h == sp->h ? sp->px[y * w + x] : Pixel(sp, x, y, w, h);
    // most of a cursor is transparent: blending only touches the visible part of each row
    for (int y = 0; y < h; y++) {
        int a = 0, b = w;
        while (a < w && !g_pic[y * w + a]) a++;
        while (b > a && !g_pic[y * w + b - 1]) b--;
        g_span[2 * y] = a;
        g_span[2 * y + 1] = b;
    }
    return true;
}

// Draw one premultiplied cursor copy into the canvas.
static void Blend(int a, int ox, int oy) {
    BlendPixels(g_bits, g_canvasW, g_pic, g_picW, g_picH, g_span, a, ox, oy);
}

static HWND g_target;

static void ShowTarget(bool show) {
    SetWindowPos(g_target, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                 (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW | SWP_NOZORDER));
}

// Gives the window the canvas: all of it, or only the dirty part. Position and size are passed even when they stay the
// same, as Windows ignores the dirty rectangle without them.
static bool Update(const RECT* dirty) {
    POINT dst = {g_win.left, g_win.top}, src = {0, 0};
    SIZE size = {g_win.right - g_win.left, g_win.bottom - g_win.top};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO u = {sizeof(u), nullptr, &dst, &size, g_memDc, &src, 0, &bf, ULW_ALPHA, dirty};
    return UpdateLayeredWindowIndirect(g_target, &u);
}

static void Wipe() {   // clears the last frame from the canvas
    for (int r = g_inked.top; r < g_inked.bottom; r++)
        memset(g_bits + r * g_canvasW + g_inked.left, 0, (g_inked.right - g_inked.left) * 4);
    g_inked = {};
}

// Makes the hidden window 1 x 1, which gives back the memory of its picture.
static void Shrink() {
    if (!g_placed || g_placed != g_target || !g_dib || g_shown) return;
    Wipe();
    g_win.right = g_win.left + 1;
    g_win.bottom = g_win.top + 1;
    if (!Update(nullptr)) g_placed = nullptr;
}

static bool Paint(int sw, int sh, const Copy* c, int n, int x, int y, int w, int h, POINT at) {
    RECT box = {x, y, x + w, y + h};
    // Moved only when the blur doesn't fit in it any more, so never while the pointer is still: the trail then only
    // shrinks toward the pointer.
    bool place = !g_dib || g_placed != g_target || box.left < g_win.left || box.top < g_win.top ||
                 box.right > g_win.right || box.bottom > g_win.bottom;
    RECT inked = g_inked;
    if (place) {   // half the monitor's width and height around the pointer, within the monitor
        MONITORINFO mi = {sizeof(mi)};
        if (!GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi)) return false;
        RECT mon = mi.rcMonitor;
        int hw = (mon.right - mon.left) / 4, hh = (mon.bottom - mon.top) / 4, margin = sw > sh ? sw : sh;
        RECT win = {at.x - hw, at.y - hh, at.x + hw, at.y + hh};
        InflateRect(&mon, margin, margin);   // the cursor reaches past the edge of the monitor
        IntersectRect(&win, &win, &mon);
        UnionRect(&win, &win, &box);
        // Moved at the same size, the window can show up at its new place a frame before its new picture, and the blur
        // jumps ahead; with a new size Windows takes both at once.
        if (g_placed == g_target && win.right - win.left == g_win.right - g_win.left &&
            win.bottom - win.top == g_win.bottom - g_win.top)
            win.right++;
        EnsureCanvas(win.right - win.left, win.bottom - win.top);
        if (!g_dib) return false;
        g_win = win;
    }
    Wipe();
    int ox = x - g_win.left, oy = y - g_win.top;
    for (int i = 0; i < n; i++) {
        int a = (int)(c[i].a * 255 + 0.5f);
        if (a > 0 && c[i].x >= 0 && c[i].y >= 0 && c[i].x + sw <= w && c[i].y + sh <= h) Blend(a, c[i].x + ox, c[i].y + oy);
    }
    RECT ink = {ox, oy, ox + w, oy + h}, dirty;
    UnionRect(&dirty, &inked, &ink);
    g_inked = ink;
    if (!Update(place ? nullptr : &dirty)) {
        g_placed = nullptr;
        return false;
    }
    g_placed = g_target;
    return true;
}

static bool Draw(const Sprite* sp, int sw, int sh, const Copy* c, int n, int x, int y, int w, int h, POINT at) {
    if (!Resize(sp, sw, sh)) return false;
    g_target = g_wnd.load();
    if (!Paint(sw, sh, c, n, x, y, w, h, at)) return false;
    if (!g_shown) {
        ShowTarget(true);
        g_shown = true;
    }
    return true;
}

static bool Hide() {
    if (g_shown) {
        ShowTarget(false);
        g_shown = false;
    }
    return false;
}

// Draws one frame of the blur; false if there is nothing to draw (then the overlay is hidden).
static bool Render() {
    static Sample pts[MAX_SAMPLES];
    static Copy copies[COPY_BUF];
    double lengths[MAX_SAMPLES];
    int np, size;
    HCURSOR handle;
    bool visible;
    EnterCriticalSection(&g_gate);
    visible = g_curVisible;
    handle = g_curHandle;
    size = g_monCursor;
    np = g_histN;
    memcpy(pts, g_hist, np * sizeof(Sample));
    LeaveCriticalSection(&g_gate);
    // Keep a replacement pointer visible while the system cursors are being restored,
    // including after motion stops. A fullscreen pause or an app-hidden cursor still wins.
    const bool drawHead = CURMB_SHARP_HEAD && g_blankActive.load();
    if (!visible || np < (drawHead ? 1 : 2) || (!g_drawing && !drawHead)) return Hide();
    if (g_spritesStale && !g_blankActive) {   // the cursor scheme, size or colour may have changed
        g_spritesStale = 0;
        FreeSprites();
    }

    Sprite* sp;
    if (g_blankActive) {
        sp = g_lastSprite;   // the real cursor is hidden: keep drawing the picture taken before
    } else {
        if (IsEmpty(handle)) return Hide();   // the app hid its cursor: don't blur the previous one
        sp = GetSprite(handle, size);
        if (sp) {
            g_lastSprite = sp;
        } else {
            sp = g_lastSprite;
        }
    }
    if (!sp) return Hide();

    Sample last = pts[np - 1];
    bool anyMove = false;
    for (int i = 0; i < np && !anyMove; i++) anyMove = pts[i].x != last.x || pts[i].y != last.y;
    if (!anyMove && !drawHead) return Hide();   // stationary: only the hidden cursor needs a frame

    // the picture at the size Windows draws the cursor on this monitor
    double f = (double)size / sp->w;
    int sw = size, sh = (int)lround(sp->h * f), hx = (int)lround(sp->hx * f), hy = (int)lround(sp->hy * f);

    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    double total = 0;
    for (int i = 0; i < np; i++) {
        if (pts[i].x - hx < minX) minX = pts[i].x - hx;
        if (pts[i].y - hy < minY) minY = pts[i].y - hy;
        if (pts[i].x - hx + sw > maxX) maxX = pts[i].x - hx + sw;
        if (pts[i].y - hy + sh > maxY) maxY = pts[i].y - hy + sh;
        if (i) {
            lengths[i] = Dist(pts[i - 1], pts[i]);
            total += lengths[i];
        }
    }
    double step = total / MAX_COPIES > 1 ? total / MAX_COPIES : 1;   // dense, faint copies read as a smooth blur
    double cover = COVER_PX * size / 32.0, peak = g_opacity / 100.0, trail = g_trailMs;
    LONGLONG now = Now();
    int n = 0;
    const int trailLimit = COPY_BUF - (drawHead ? 1 : 0);
    for (int i = 1; i < np && n < trailLimit; i++) {
        Sample a = pts[i - 1], b = pts[i];
        double dx = b.x - a.x, dy = b.y - a.y, d = lengths[i];
        if (d < 0.5) continue;
        int k = (int)(d / step);
        if (k < 1) k = 1;
        for (int c = 0; c < k && n < trailLimit; c++) {
            double t = (double)c / k;
            double age = (now - (a.t + (b.t - a.t) * t)) / trail;   // 0 at the pointer, 1 at the end of the trail
            if (age >= 1) continue;
            double life = 1.0 - age * age;   // nearly full strength behind the pointer, fading out toward the end
            // About cover/step copies overlap at each point; layered on top of each other they add up to the target
            // opacity when each has 1 - (1 - target)^(step/cover).
            double target = peak * life, overlap = cover / step;
            double alpha = overlap > 1 ? 1 - pow(1 - target, 1 / overlap) : target;
            if (alpha * 255 < 0.5) continue;
            copies[n++] = {(int)lround(a.x + dx * t) - hx - minX, (int)lround(a.y + dy * t) - hy - minY,
                           alpha > 1 ? 1.0f : (float)alpha};
        }
    }
    if (drawHead && g_blankActive.load())
        AppendCursorHead(copies, n, last.x, last.y, hx, hy, minX, minY);
    if (!n) return Hide();
    g_drawFailed = !Draw(sp, sw, sh, copies, n, minX, minY, maxX - minX, maxY - minY, {last.x, last.y});
    if (g_drawFailed) {
        SetEvent(g_mouseWake);
        return Hide();
    }
    if (!g_blankActive && g_pictureOf != handle) {
        g_pictureOf = handle;
        SetEvent(g_hideChanged);
    }
    return true;
}

// Raw mouse input: while on, Windows tells the overlay about every mouse report (even while other apps are active), so
// the sampler can sleep until the mouse really moves. Off while paused for a fullscreen game.
static bool ListenToMouse(bool on) {
    // generic desktop / mouse
    RAWINPUTDEVICE mouse = {1, 2, on ? (DWORD)RIDEV_INPUTSINK : (DWORD)RIDEV_REMOVE, on ? g_wnd.load() : nullptr};
    if (RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) return true;
    return false;
}

static UINT g_shellHookMsg;   // the shell hook's message: a window was activated, created, destroyed...

static LRESULT CALLBACK OverlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // Another window was activated (the high bit marks a fullscreen one): look at the window in front.
    if (msg == g_shellHookMsg && msg && (wp & 0x7FFF) == HSHELL_WINDOWACTIVATED) {
        g_recheck = true;
        SetEvent(g_mouseWake);
        return 0;
    }
    switch (msg) {
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) {
                g_stop = true;
                SetEvent(g_mouseWake);
                SetEvent(g_hideChanged);
                SetEvent(g_moved);
            }
            return 0;
        case WM_INPUT: {   // wake the sampler for movement only (not for wheel or button reports)
            RAWINPUT ri;
            UINT size = sizeof(ri);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                ri.header.dwType == RIM_TYPEMOUSE &&
                (ri.data.mouse.lLastX || ri.data.mouse.lLastY || (ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)))
                SetEvent(g_mouseWake);
            break;   // then DefWindowProc, which cleans up after the input
        }
        case WM_APP_PAUSED:
            ListenToMouse(!g_paused);
            return 0;
        case WM_SETTINGCHANGE:
        case WM_DISPLAYCHANGE:   // monitors, scaling or cursor scheme changed
            g_monStale = 1;
            g_spritesStale = 1;
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Draws when the mouse moves, at most once per half refresh of the monitor, and keeps drawing until the trail has faded
// after a stop. Idle, it wakes once more to give back memory, then sleeps until the mouse moves.
static DWORD WINAPI RenderThread(void*) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"PointerAfterimageMotionBlur";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                            wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    g_memDc = CreateCompatibleDC(nullptr);
    if (!g_wnd || !g_memDc || !ListenToMouse(true)) {
        if (g_wnd) DestroyWindow(g_wnd);
        g_wnd = nullptr;
    } else {
        g_shellHookMsg = RegisterWindowMessageW(L"SHELLHOOK");
        RegisterShellHookWindow(g_wnd);
    }
    SetEvent(g_ready);

    LONGLONG lastDraw = -1000;
    bool trimmed = true, pending = false;   // pending: the mouse moved, but too soon after the last frame
    while (!g_stop && g_wnd) {
        LONGLONG wait = g_monGap - (Now() - lastDraw);
        DWORD timeout = pending || g_shown ? (wait > 0 ? (DWORD)wait : 0) : trimmed ? INFINITE : 1500;
        DWORD r = MsgWaitForMultipleObjects(1, &g_moved, FALSE, timeout, QS_ALLINPUT);
        if (g_stop) break;
        if (r == WAIT_OBJECT_0 + 1) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
            continue;
        }
        if (r == WAIT_OBJECT_0) pending = true;
        if (!pending && !g_shown) {   // idle for 1.5 s: give back the memory that can be rebuilt
            Shrink();
            ReleaseCanvas();
            free(g_pic);
            g_pic = nullptr;
            trimmed = true;
            continue;
        }
        if (Now() - lastDraw < g_monGap) continue;
        pending = false;
#if CURMB_FAST_FRAMES
        LONGLONG drawStarted = Now();
#endif
        if (Render()) {
#if CURMB_FAST_FRAMES
            lastDraw = drawStarted;
#else
            lastDraw = Now();
#endif
            trimmed = false;
        }
    }

    Hide();
    Shrink();
    if (g_wnd) {
        DeregisterShellHookWindow(g_wnd);
        ListenToMouse(false);
        DestroyWindow(g_wnd);
        g_wnd = nullptr;
    }
    UnregisterClassW(wc.lpszClassName, inst);
    ReleaseCanvas();
    if (g_memDc) DeleteDC(g_memDc);
    g_memDc = nullptr;
    free(g_pic);
    g_pic = nullptr;
    FreeSprites();
    return 0;
}

// ---- process lifecycle ----

static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter;

// Try to restore immediately on a crash; the watchdog also checks after exit.
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* e) {
    if (g_blankActive) RestoreCursors();
    return g_prevFilter ? g_prevFilter(e) : EXCEPTION_CONTINUE_SEARCH;
}

// Signals every thread to finish, waits for them, and makes sure the user's cursors are back.
static void StopThreads() {
    g_stop = true;
    SetEvent(g_mouseWake);
    SetEvent(g_moved);
    SetEvent(g_hideChanged);
    for (HANDLE& t : g_threads)
        if (t) {
            WaitForSingleObject(t, INFINITE);
            CloseHandle(t);
            t = nullptr;
        }
    if (g_blankActive) {
        if (RestoreCursors()) g_blankActive = false;
    }
}

static bool StartThreads() {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    InitializeCriticalSection(&g_gate);
    g_ownIntegrity = Integrity(GetCurrentProcess());
    g_prevFilter = SetUnhandledExceptionFilter(CrashFilter);

    // Windows 11 ignores a finer timer for a process with no visible window; the short waits here need it while moving.
    PROCESS_POWER_THROTTLING_STATE pt = {PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                         PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));

    g_moved = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_mouseWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_hideChanged = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_moved || !g_mouseWake || !g_hideChanged || !g_ready) return false;

    // The render thread first: it creates the overlay window that receives the mouse input, and the others need it.
    LPTHREAD_START_ROUTINE procs[3] = {RenderThread, SampleThread, HideThread};
    for (int i = 0; i < 3; i++) {
        g_threads[i] = CreateThread(nullptr, 0, procs[i], nullptr, 0, nullptr);
        if (!g_threads[i]) {
            StopThreads();
            return FALSE;
        }
        if (i < 2) SetThreadPriority(g_threads[i], THREAD_PRIORITY_ABOVE_NORMAL);
        if (i == 0 && (WaitForSingleObject(g_ready, 5000) != WAIT_OBJECT_0 || !g_wnd)) {
            StopThreads();
            return FALSE;
        }
    }
    return TRUE;
}

// The same executable also acts as a minimal recovery process. It inherits only
// a synchronization handle to its parent and cannot change cursors until the
// primary instance's mutex has been released by Windows.
static bool StartWatchdog() {
    HANDLE parent = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent,
                         SYNCHRONIZE, TRUE, 0)) return false;
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!n || n >= ARRAYSIZE(path)) { CloseHandle(parent); return false; }
    wchar_t command[32768 + 80];
    swprintf(command, ARRAYSIZE(command), L"\"%ls\" --watch %llu", path,
             static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(parent)));
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    bool ok = CreateProcessW(path, command, nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi) != FALSE;
    CloseHandle(parent);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    g_watchdog = pi.hProcess;
    return true;
}

static int WatchParent(const wchar_t* arg) {
    wchar_t* end = nullptr;
    unsigned long long value = _wcstoui64(arg, &end, 10);
    if (!value || !end || *end) return 1;
    HANDLE parent = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(value));
    if (WaitForSingleObject(parent, INFINITE) != WAIT_OBJECT_0) return 1;
    CloseHandle(parent);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!mutex) return 1;
    DWORD result = WaitForSingleObject(mutex, INFINITE);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
        if (HiddenMarker()) RestoreCursors();
        ReleaseMutex(mutex);
    }
    CloseHandle(mutex);
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    if (argc == 3 && wcscmp(argv[1], L"--watch") == 0) {
        int result = WatchParent(argv[2]);
        LocalFree(argv);
        return result;
    }
    LocalFree(argv);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 0; }
    if (HiddenMarker() && !RestoreCursors()) { ReleaseMutex(mutex); CloseHandle(mutex); return 1; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // If recovery cannot be started, run the effect but never hide system cursors.
    StartWatchdog();
    bool started = StartThreads();
    if (started) {
        WaitForSingleObject(g_threads[0], INFINITE);
        StopThreads();
    }
    SetUnhandledExceptionFilter(g_prevFilter);
    if (g_moved) CloseHandle(g_moved);
    if (g_mouseWake) CloseHandle(g_mouseWake);
    if (g_hideChanged) CloseHandle(g_hideChanged);
    if (g_ready) CloseHandle(g_ready);
    DeleteCriticalSection(&g_gate);
    if (g_watchdog) CloseHandle(g_watchdog);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return started ? 0 : 1;
}
