// ==WindhawkMod==
// @id              taskbar-music-lounge-ytm
// @name            Taskbar Music Lounge - YouTube Music
// @description     Spotify-style miniplayer for YouTube Music with album art, controls, progress bar and crossfade transitions.
// @version         1.5.0
// @author          Hashah2311 / fork
// @include         explorer.exe
// @compilerOptions -lole32 -ldwmapi -lgdi32 -luser32 -lwindowsapp -lshcore -lgdiplus -lshell32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Music Lounge — YouTube Music Edition

A Spotify-style floating miniplayer that activates **only** when YouTube Music is playing.

## Features
- YouTube Music only — ignores all other media sources
- Album art with rounded corners + smooth crossfade on track change
- Title + artist with ellipsis overflow and scroll
- Prev / Play-Pause / Next controls
- Real-time interpolated progress bar (no jumpy ticks)
- Seek by clicking or dragging — stays in place after seek
- Smart idle timeout & fullscreen hiding
- Auto light/dark theme

## Requirements
- Windows 11
- Disable Taskbar Widgets
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- PanelWidth: 500
  $name: Panel Width
- PanelHeight: 130
  $name: Panel Height
- FontSize: 18
  $name: Font Size (track title)
- ButtonScale: 2.0
  $name: Button Scale (1.0 = Normal, 2.0 = 4K)
- HideFullscreen: false
  $name: Hide when Fullscreen
- IdleTimeout: 25
  $name: Auto-hide when paused (seconds, 0 = off)
- OffsetX: 1417
  $name: X position offset from taskbar left
- OffsetY: -95
  $name: Y position offset from taskbar center
- AutoTheme: true
  $name: Auto light/dark theme
- TextColor: 0xFFFFFF
  $name: Manual text color (hex, used when AutoTheme off)
- BgOpacity: 128
  $name: Acrylic tint opacity (0-255)
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <windowsx.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shcore.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <chrono>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace Gdiplus;
using namespace std;
using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Storage::Streams;

// ─── Constants ───────────────────────────────────────────────────────────────
const WCHAR* FONT_TITLE = L"Segoe UI Variable Display";

// ─── DWM / Acrylic ───────────────────────────────────────────────────────────
typedef enum _WINDOWCOMPOSITIONATTRIB { WCA_ACCENT_POLICY = 19 } WINDOWCOMPOSITIONATTRIB;
typedef enum _ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
} ACCENT_STATE;
typedef struct _ACCENT_POLICY  { ACCENT_STATE AccentState; DWORD AccentFlags; DWORD GradientColor; DWORD AnimationId; } ACCENT_POLICY;
typedef struct _WINDOWCOMPOSITIONATTRIBDATA { WINDOWCOMPOSITIONATTRIB Attribute; PVOID Data; SIZE_T SizeOfData; } WINDOWCOMPOSITIONATTRIBDATA;
typedef BOOL(WINAPI* pSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

// ─── Z-Band ──────────────────────────────────────────────────────────────────
enum ZBID { ZBID_IMMERSIVE_NOTIFICATION = 4 };
typedef HWND(WINAPI* pCreateWindowInBand)(DWORD,LPCWSTR,LPCWSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID,DWORD);

// ─── Settings ────────────────────────────────────────────────────────────────
struct ModSettings {
    int    width          = 500;
    int    height         = 130;
    int    fontSize       = 18;
    double buttonScale    = 2.0;
    bool   hideFullscreen = false;
    int    idleTimeout    = 25;
    int    offsetX        = 1417;
    int    offsetY        = -95;
    bool   autoTheme      = true;
    DWORD  manualTextColor= 0xFFFFFFFF;
    int    bgOpacity      = 128;
} g_Settings;

void LoadSettings() {
    g_Settings.width          = Wh_GetIntSetting(L"PanelWidth");
    g_Settings.height         = Wh_GetIntSetting(L"PanelHeight");
    g_Settings.fontSize       = Wh_GetIntSetting(L"FontSize");
    g_Settings.hideFullscreen = Wh_GetIntSetting(L"HideFullscreen") != 0;
    g_Settings.idleTimeout    = Wh_GetIntSetting(L"IdleTimeout");
    g_Settings.offsetX        = Wh_GetIntSetting(L"OffsetX");
    g_Settings.offsetY        = Wh_GetIntSetting(L"OffsetY");
    g_Settings.autoTheme      = Wh_GetIntSetting(L"AutoTheme") != 0;
    g_Settings.bgOpacity      = max(0, min(255, Wh_GetIntSetting(L"BgOpacity")));

    PCWSTR sc = Wh_GetStringSetting(L"ButtonScale");
    g_Settings.buttonScale = sc ? max(0.5, min(4.0, _wtof(sc))) : 2.0;
    if (sc) Wh_FreeStringSetting(sc);

    PCWSTR tc = Wh_GetStringSetting(L"TextColor");
    DWORD rgb = 0xFFFFFF;
    if (tc) { if (wcslen(tc) > 0) rgb = wcstoul(tc, nullptr, 16); Wh_FreeStringSetting(tc); }
    g_Settings.manualTextColor = 0xFF000000 | rgb;

    if (g_Settings.width  < 200) g_Settings.width  = 500;
    if (g_Settings.height < 60)  g_Settings.height = 130;
}

// ─── Global State ────────────────────────────────────────────────────────────
HWND  g_hWnd           = NULL;
bool  g_Running        = true;
int   g_HoverState     = -1;
bool  g_IsHiddenByIdle = false;
int   g_IdleCounter    = 0;
HWINEVENTHOOK g_Hook   = nullptr;
UINT  g_TaskbarMsg     = RegisterWindowMessage(L"TaskbarCreated");

// Progress scrub
bool  g_Scrubbing  = false;
float g_ScrubPos   = 0.0f;

// Real-time position interpolation
ULONGLONG g_LastPosTick   = 0;
double    g_LocalPos      = 0.0;
ULONGLONG g_SeekLockUntil = 0;

// High-resolution interpolation anchor
std::chrono::steady_clock::time_point g_InterpolationAnchor;

// Adaptive polling
int  g_CurrentPollInterval = 500;
bool g_IsPlaying           = false;

// Dirty rectangles for efficient redraws
bool g_NeedsProgressRedraw = false;
bool g_NeedsArtRedraw      = false;
RECT g_ArtRect             = {0, 0, 0, 0};
RECT g_TitleRect           = {0, 0, 0, 0};
RECT g_ArtistRect          = {0, 0, 0, 0};

// Cached GDI+ resources
SolidBrush* g_CachedTrackBrush  = nullptr;
SolidBrush* g_CachedFillBrush   = nullptr;
SolidBrush* g_CachedTimeBrush   = nullptr;
Font*       g_CachedTitleFont   = nullptr;
Font*       g_CachedArtistFont  = nullptr;
Font*       g_CachedTimeFont    = nullptr;
FontFamily* g_CachedFontFamily  = nullptr;
Color       g_CachedTextColor;
bool        g_CacheValid        = false;

// Async art loading
bool  g_ArtLoadPending  = false;
HANDLE g_ArtLoadThread   = nullptr;

// ─── Crossfade State ─────────────────────────────────────────────────────────
Bitmap* g_PrevArt    = nullptr;
int     g_FadeAlpha  = 255;
bool    g_Fading     = false;
mutex   g_FadeMtx;
wstring g_PrevTitle  = L"";
wstring g_PrevArtist = L"";

// Fade constants — ~340 ms at 60 fps
static const int FADE_STEP     = 12;
static const int FADE_TIMER_MS = 16;

// ─── Media State ─────────────────────────────────────────────────────────────
struct MediaState {
    wstring title    = L"Nothing playing";
    wstring artist   = L"";
    bool isPlaying   = false;
    bool hasMedia    = false;
    bool isYTM       = false;
    double position  = 0.0;
    double duration  = 0.0;
    Bitmap* albumArt = nullptr;
    mutex   lock;
} g_Media;

// Art retry
wstring g_LastKnownTitle  = L"";
int     g_ArtRetryCount   = 0;

// Scroll
int  g_ScrollOffset = 0;
int  g_TextWidth    = 0;
bool g_IsScrolling  = false;
int  g_ScrollWait   = 80;

// ─── WinRT Session Manager ───────────────────────────────────────────────────
GlobalSystemMediaTransportControlsSessionManager g_SessionManager = nullptr;

// ─── Helpers ─────────────────────────────────────────────────────────────────
bool IsSystemLightMode() {
    DWORD v = 0, sz = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", RRF_RT_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS && v != 0;
}

DWORD GetTextColor() {
    if (g_Settings.autoTheme) return IsSystemLightMode() ? 0xFF1a1a1a : 0xFFFFFFFF;
    return g_Settings.manualTextColor;
}

Color GdipColor(DWORD argb) {
    return Color((argb>>24)&0xFF,(argb>>16)&0xFF,(argb>>8)&0xFF,argb&0xFF);
}

wstring FormatTime(double sec) {
    if (sec < 0) sec = 0;
    int s = (int)sec, m = s/60; s %= 60;
    WCHAR buf[16]; swprintf_s(buf, L"%d:%02d", m, s);
    return buf;
}

double GetHighResTime() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count() / 1000.0;
}

// ─── Resource Cache ──────────────────────────────────────────────────────────
void InvalidateCache() {
    g_CacheValid = false;
}

void UpdateResourceCache(Graphics& gfx, int titleFontSize, int artistFontSize, int timeFont, Color textCol, Color subCol) {
    if (g_CacheValid && 
        g_CachedTextColor.GetValue() == textCol.GetValue()) return;
    
    if (g_CachedTrackBrush) { delete g_CachedTrackBrush; g_CachedTrackBrush = nullptr; }
    if (g_CachedFillBrush) { delete g_CachedFillBrush; g_CachedFillBrush = nullptr; }
    if (g_CachedTimeBrush) { delete g_CachedTimeBrush; g_CachedTimeBrush = nullptr; }
    
    g_CachedTrackBrush = new SolidBrush(Color(70, textCol.GetRed(), textCol.GetGreen(), textCol.GetBlue()));
    g_CachedFillBrush = new SolidBrush(textCol);
    g_CachedTimeBrush = new SolidBrush(subCol);
    
    if (g_CachedTitleFont) { delete g_CachedTitleFont; g_CachedTitleFont = nullptr; }
    if (g_CachedArtistFont) { delete g_CachedArtistFont; g_CachedArtistFont = nullptr; }
    if (g_CachedTimeFont) { delete g_CachedTimeFont; g_CachedTimeFont = nullptr; }
    if (g_CachedFontFamily) { delete g_CachedFontFamily; g_CachedFontFamily = nullptr; }
    
    g_CachedFontFamily = new FontFamily(FONT_TITLE);
    g_CachedTitleFont = new Font(g_CachedFontFamily, (REAL)titleFontSize, FontStyleBold, UnitPixel);
    g_CachedArtistFont = new Font(g_CachedFontFamily, (REAL)artistFontSize, FontStyleRegular, UnitPixel);
    g_CachedTimeFont = new Font(g_CachedFontFamily, (REAL)timeFont, FontStyleBold, UnitPixel);
    
    g_CachedTextColor = textCol;
    g_CacheValid = true;
}

void AddRoundedRect(GraphicsPath& path, int x, int y, int w, int h, int r) {
    int d = r*2;
    path.AddArc(x,         y,         d, d, 180, 90);
    path.AddArc(x+w-d,     y,         d, d, 270, 90);
    path.AddArc(x+w-d, y+h-d,         d, d,   0, 90);
    path.AddArc(x,     y+h-d,         d, d,  90, 90);
    path.CloseFigure();
}

Bitmap* StreamToBitmap(IRandomAccessStreamWithContentType const& stream) {
    if (!stream) return nullptr;
    IStream* ns = nullptr;
    if (SUCCEEDED(CreateStreamOverRandomAccessStream(
            reinterpret_cast<IUnknown*>(winrt::get_abi(stream)), IID_PPV_ARGS(&ns)))) {
        Bitmap* b = Bitmap::FromStream(ns);
        ns->Release();
        if (b && b->GetLastStatus() == Ok) return b;
        delete b;
    }
    return nullptr;
}

// ─── YouTube Music Detection ─────────────────────────────────────────────────
bool IsYouTubeMusic(GlobalSystemMediaTransportControlsSession const& session) {
    if (!session) return false;
    try {
        hstring src = session.SourceAppUserModelId();
        wstring id(src.c_str());
        wstring low = id;
        transform(low.begin(), low.end(), low.begin(), ::towlower);
        if (low.find(L"youtubemusic")  != wstring::npos) return true;
        if (low.find(L"youtube music") != wstring::npos) return true;
        if (low.find(L"youtube_music") != wstring::npos) return true;
        if (low.find(L"ytm")           != wstring::npos) return true;
        return false;
    } catch (...) { return false; }
}

struct YTMFindData { bool found; };
BOOL CALLBACK FindYTMWindow(HWND hwnd, LPARAM lp) {
    WCHAR t[512] = {};
    GetWindowTextW(hwnd, t, 512);
    wstring tl(t);
    transform(tl.begin(), tl.end(), tl.begin(), ::towlower);
    if (tl.find(L"youtube music") != wstring::npos) {
        ((YTMFindData*)lp)->found = true; return FALSE;
    }
    return TRUE;
}
bool IsYTMWindowOpen() {
    YTMFindData fd = {false};
    EnumWindows(FindYTMWindow, (LPARAM)&fd);
    return fd.found;
}

// forward declaration
void TriggerCrossfade(Bitmap* newArt);

// ─── Media Update ─────────────────────────────────────────────────────────────
void UpdateMediaInfo() {
    try {
        if (!g_SessionManager)
            g_SessionManager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        if (!g_SessionManager) return;

        GlobalSystemMediaTransportControlsSession ytmSession = nullptr;
        auto sessions = g_SessionManager.GetSessions();
        for (auto const& s : sessions) { if (IsYouTubeMusic(s)) { ytmSession = s; break; } }
        if (!ytmSession) {
            for (auto const& s : sessions) { if (IsYTMWindowOpen()) { ytmSession = s; break; } }
        }

        if (!ytmSession) {
            lock_guard<mutex> g(g_Media.lock);
            g_Media.hasMedia = false; g_Media.isYTM = false;
            g_Media.title = L"Nothing playing"; g_Media.artist = L"";
            g_Media.position = 0; g_Media.duration = 0;
            g_LocalPos = 0; g_LastPosTick = 0; g_SeekLockUntil = 0;
            if (g_Media.albumArt) { delete g_Media.albumArt; g_Media.albumArt = nullptr; }
            g_LastKnownTitle = L""; g_ArtRetryCount = 0;
            return;
        }

        auto props = ytmSession.TryGetMediaPropertiesAsync().get();
        auto info  = ytmSession.GetPlaybackInfo();
        auto tline = ytmSession.GetTimelineProperties();

        wstring newTitle  = props.Title().c_str();
        wstring newArtist = props.Artist().c_str();

        bool titleChanged = (newTitle != g_LastKnownTitle);
        bool needArt      = false;

        {
            lock_guard<mutex> g(g_Media.lock);
            needArt = (g_Media.albumArt == nullptr);
        }

        if (titleChanged) {
            g_ArtRetryCount = 6;
        } else if (needArt && g_ArtRetryCount > 0) {
            g_ArtRetryCount--;
        } else if (!needArt) {
            g_ArtRetryCount = 0;
        }

        bool shouldFetchArt = titleChanged || (needArt && g_ArtRetryCount > 0);

        Bitmap* freshArt = nullptr;
        if (shouldFetchArt) {
            auto thumb = props.Thumbnail();
            if (thumb) {
                try {
                    auto stream = thumb.OpenReadAsync().get();
                    if (stream) freshArt = StreamToBitmap(stream);
                } catch (...) {}
            }
        }

        wstring oldTitle, oldArtist;
        if (titleChanged) {
            lock_guard<mutex> g(g_Media.lock);
            oldTitle  = g_Media.title;
            oldArtist = g_Media.artist;
        }

        {
            lock_guard<mutex> g(g_Media.lock);

            if (titleChanged) {
                // Reset interpolation for the new track only — do NOT touch g_SeekLockUntil
                // so an in-flight seek on the current track isn't cancelled.
                g_LocalPos    = 0;
                g_LastPosTick = 0;
            }

            g_Media.title    = newTitle;
            g_Media.artist   = newArtist;
            g_Media.isPlaying = (info.PlaybackStatus() ==
                GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
            g_Media.hasMedia = true;
            g_Media.isYTM    = true;

            if (tline) {
                ULONGLONG now = GetTickCount64();
                auto end = tline.EndTime();
                g_Media.duration = chrono::duration<double>(end).count();

                // ── High-resolution position sync ───────────────────────────────
                // Only update when not scrubbing and seek-lock has expired.
                if (!g_Scrubbing && now >= g_SeekLockUntil) {
                    auto pos      = tline.Position();
                    double newPos = chrono::duration<double>(pos).count();
                    double drift  = newPos - g_LocalPos;
                    if (g_LastPosTick == 0 || fabs(drift) > 1.5) {
                        g_Media.position = newPos;
                        g_LocalPos       = newPos;
                        g_LastPosTick    = now;
                        g_InterpolationAnchor = std::chrono::steady_clock::now();
                    }
                }
            }

            // ── Art swap + crossfade ──────────────────────────────────────────
            if (freshArt) {
                Bitmap* prevArt = g_Media.albumArt;
                g_Media.albumArt = freshArt;
                g_LastKnownTitle = newTitle;

                {
                    lock_guard<mutex> fg(g_FadeMtx);
                    g_PrevTitle  = oldTitle;
                    g_PrevArtist = oldArtist;
                }
                if (g_hWnd) PostMessage(g_hWnd, WM_APP + 20, (WPARAM)prevArt, 0);

            } else if (titleChanged && !freshArt) {
                {
                    lock_guard<mutex> fg(g_FadeMtx);
                    g_PrevTitle  = oldTitle;
                    g_PrevArtist = oldArtist;
                }
                if (g_Media.albumArt) {
                    Bitmap* prevArt = g_Media.albumArt;
                    g_Media.albumArt = nullptr;
                    if (g_hWnd) PostMessage(g_hWnd, WM_APP + 20, (WPARAM)prevArt, 0);
                }
                g_LastKnownTitle = newTitle;
            }
        }

    } catch (...) {
        lock_guard<mutex> g(g_Media.lock);
        g_Media.hasMedia = false;
    }
}

// ─── Media Commands ──────────────────────────────────────────────────────────
void SendMediaCommand(int cmd) {
    try {
        if (!g_SessionManager) return;
        auto sessions = g_SessionManager.GetSessions();
        GlobalSystemMediaTransportControlsSession s = nullptr;
        for (auto const& sess : sessions) { if (IsYouTubeMusic(sess)) { s = sess; break; } }
        if (!s) s = g_SessionManager.GetCurrentSession();
        if (!s) return;
        if      (cmd == 1) s.TrySkipPreviousAsync();
        else if (cmd == 2) s.TryTogglePlayPauseAsync();
        else if (cmd == 3) s.TrySkipNextAsync();
    } catch (...) {}
}

void SeekTo(double seconds) {
    try {
        if (!g_SessionManager) return;
        auto sessions = g_SessionManager.GetSessions();
        for (auto const& s : sessions) {
            if (IsYouTubeMusic(s)) {
                auto ticks = chrono::duration_cast<
                    chrono::duration<long long, ratio<1, 10000000>>>(
                    chrono::duration<double>(seconds)).count();
                s.TryChangePlaybackPositionAsync(ticks);

                // Anchor interpolator at the seek target
                ULONGLONG now   = GetTickCount64();
                g_LocalPos      = seconds;
                g_LastPosTick   = now;
                g_SeekLockUntil = now + 1000;
                g_InterpolationAnchor = std::chrono::steady_clock::now();

                { lock_guard<mutex> lk(g_Media.lock); g_Media.position = seconds; }
                break;
            }
        }
    } catch (...) {}
}

// ─── Appearance ──────────────────────────────────────────────────────────────
void UpdateAppearance(HWND hwnd) {
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    HMODULE hU = GetModuleHandle(L"user32.dll");
    if (!hU) return;
    auto SetComp = (pSetWindowCompositionAttribute)GetProcAddress(hU, "SetWindowCompositionAttribute");
    if (!SetComp) return;
    DWORD tint;
    if (g_Settings.autoTheme)
        tint = IsSystemLightMode()
            ? ((g_Settings.bgOpacity << 24) | 0xEEEEEE)
            : ((g_Settings.bgOpacity << 24) | 0x111111);
    else
        tint = (g_Settings.bgOpacity << 24) | 0x111111;
    ACCENT_POLICY policy = { ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, tint, 0 };
    WINDOWCOMPOSITIONATTRIBDATA data = { WCA_ACCENT_POLICY, &policy, sizeof(policy) };
    SetComp(hwnd, &data);
}

// ─── Drawing ─────────────────────────────────────────────────────────────────
struct ProgressRect { int x, y, w, h; } g_ProgressRect = {0,0,0,0};
struct HitArea { int x, y, r; }         g_HitPrev, g_HitPlay, g_HitNext;

void DrawArtWithFade(Graphics& gfx, GraphicsPath& clipPath,
                     int artX, int artY, int artSize,
                     Color textCol)
{
    Bitmap* curArt  = nullptr;
    Bitmap* prevArt = nullptr;
    int     alpha   = 255;
    bool    fading  = false;

    {
        lock_guard<mutex> mg(g_Media.lock);
        curArt = g_Media.albumArt ? g_Media.albumArt->Clone() : nullptr;
    }
    {
        lock_guard<mutex> fg(g_FadeMtx);
        prevArt = g_PrevArt ? g_PrevArt->Clone() : nullptr;
        alpha   = g_FadeAlpha;
        fading  = g_Fading;
    }

    gfx.SetClip(&clipPath);

    bool drewSomething = false;

    if (fading && prevArt) {
        ImageAttributes iaOld;
        ColorMatrix cmOld = {
            1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0,
            0,0,0,1.0f,0, 0,0,0,0,1
        };
        iaOld.SetColorMatrix(&cmOld);
        gfx.DrawImage(prevArt, Rect(artX, artY, artSize, artSize),
            0, 0, prevArt->GetWidth(), prevArt->GetHeight(),
            UnitPixel, &iaOld);
        drewSomething = true;
    }

    if (curArt) {
        float a = fading ? (float)alpha / 255.0f : 1.0f;
        ImageAttributes iaCur;
        ColorMatrix cmCur = {
            1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0,
            0,0,0,a,0, 0,0,0,0,1
        };
        iaCur.SetColorMatrix(&cmCur);
        gfx.DrawImage(curArt, Rect(artX, artY, artSize, artSize),
            0, 0, curArt->GetWidth(), curArt->GetHeight(),
            UnitPixel, &iaCur);
        drewSomething = true;
    }

    if (!drewSomething) {
        LinearGradientBrush grad(
            Point(artX, artY), Point(artX + artSize, artY + artSize),
            Color(255, 40, 40, 60), Color(255, 80, 60, 100));
        gfx.FillPath(&grad, &clipPath);
        SolidBrush nb(Color(80, 255, 255, 255));
        FontFamily ff0(FONT_TITLE);
        Font nf(&ff0, (REAL)(artSize * 0.35f), FontStyleRegular, UnitPixel);
        RectF nr((REAL)artX, (REAL)artY, (REAL)artSize, (REAL)artSize);
        StringFormat sf0;
        sf0.SetAlignment(StringAlignmentCenter);
        sf0.SetLineAlignment(StringAlignmentCenter);
        gfx.DrawString(L"♪", -1, &nf, nr, &sf0, &nb);
    }

    gfx.ResetClip();

    if (curArt)  delete curArt;
    if (prevArt) delete prevArt;
}

void DrawPanel(HDC hdc, int W, int H) {
    // Skip drawing if window is hidden
    if (!IsWindowVisible(g_hWnd)) return;

    Graphics gfx(hdc);
    gfx.SetSmoothingMode(SmoothingModeAntiAlias);
    gfx.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    gfx.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    gfx.Clear(Color(0, 0, 0, 0));

    wstring title, artist;
    bool isPlaying, hasMedia;
    double dur;

    ULONGLONG lastTick = g_LastPosTick;
    double    localPos = g_LocalPos;

    {
        lock_guard<mutex> g(g_Media.lock);
        title     = g_Media.title;
        artist    = g_Media.artist;
        isPlaying = g_Media.isPlaying;
        hasMedia  = g_Media.hasMedia;
        dur       = g_Media.duration;
    }

    // ── High-resolution position interpolation ─────────────────────────────────
    double displayPos = localPos;
    if (isPlaying && lastTick > 0 && dur > 0) {
        using namespace std::chrono;
        auto now = steady_clock::now();
        double elapsed = duration<double>(now - g_InterpolationAnchor).count();
        displayPos = min(dur, localPos + elapsed);
    }

    float s = (float)g_Settings.buttonScale;

    int pad      = 12;
    int artSize  = H - pad * 2;
    int artX     = pad, artY = pad;
    int contentX = artX + artSize + (int)(10 * s * 0.5f);
    int contentW = W - contentX - pad;

    // Store art rect for dirty region updates
    g_ArtRect = {artX, artY, artX + artSize, artY + artSize};

    // ── 1. Album Art with crossfade ──────────────────────────────────────────
    GraphicsPath artPath;
    AddRoundedRect(artPath, artX, artY, artSize, artSize, 10);
    DrawArtWithFade(gfx, artPath, artX, artY, artSize, Color(255,255,255,255));

    // ── 2. Title & Artist ────────────────────────────────────────────────────
    DWORD rawColor = GetTextColor();
    Color textCol(GdipColor(rawColor));
    Color subCol(180, textCol.GetRed(), textCol.GetGreen(), textCol.GetBlue());

    FontFamily ff(FONT_TITLE);
    int titleFontSize  = g_Settings.fontSize;
    Font titleFont(&ff, (REAL)titleFontSize, FontStyleBold, UnitPixel);
    SolidBrush titleBrush(textCol), subBrush(subCol);

    int artistFontSize = max(10, (int)(titleFontSize * 0.72f));
    Font artistFont(&ff, (REAL)artistFontSize, FontStyleRegular, UnitPixel);

    float ctrlH           = 28.0f * s;
    float progressH       = 5.0f + s * 0.8f;
    float progressMarginT = 6.0f;
    int   timeFont        = max(12, (int)(titleFontSize * 0.75f));

    float lineH      = (float)(titleFontSize + 4);
    float subH       = (float)(artistFontSize + 4);
    float totalTextH = lineH + subH;
    float totalH     = totalTextH + 6.0f + ctrlH + progressMarginT + progressH + 10.0f + timeFont;
    float startY     = (H - totalH) / 2.0f;
    if (startY < pad) startY = (float)pad;

    RectF measureRect(0, 0, 4000, 100);

    if (!hasMedia) {
        SolidBrush dimBrush(Color(100, textCol.GetRed(), textCol.GetGreen(), textCol.GetBlue()));
        RectF r((REAL)contentX, startY, (REAL)contentW, lineH);
        StringFormat sf2; sf2.SetTrimming(StringTrimmingEllipsisCharacter);
        sf2.SetFormatFlags(StringFormatFlagsNoWrap);
        gfx.DrawString(L"Nothing playing", -1, &titleFont, r, &sf2, &dimBrush);
    } else {
        // ── Snapshot fade state ──────────────────────────────────────────────
        int     fadeAlpha  = 255;
        bool    fading     = false;
        wstring prevTitle, prevArtist;
        {
            lock_guard<mutex> fg(g_FadeMtx);
            fadeAlpha  = g_FadeAlpha;
            fading     = g_Fading;
            prevTitle  = g_PrevTitle;
            prevArtist = g_PrevArtist;
        }

        BYTE newAlpha  = fading ? (BYTE)min(255, fadeAlpha)         : 255;
        BYTE oldAlpha  = fading ? (BYTE)max(0,   255 - fadeAlpha)   : 0;

        // ── Title ────────────────────────────────────────────────────────────
        Region titleClip(Rect(contentX, (int)startY, contentW, (int)(lineH + 2)));
        gfx.SetClip(&titleClip);

        if (fading && !prevTitle.empty() && oldAlpha > 0) {
            SolidBrush oldTitleBrush(Color(oldAlpha,
                textCol.GetRed(), textCol.GetGreen(), textCol.GetBlue()));
            RectF rOld((REAL)contentX, startY, (REAL)contentW, lineH);
            StringFormat sfOld; sfOld.SetTrimming(StringTrimmingEllipsisCharacter);
            sfOld.SetFormatFlags(StringFormatFlagsNoWrap);
            gfx.DrawString(prevTitle.c_str(), -1, &titleFont, rOld, &sfOld, &oldTitleBrush);
        }

        if (newAlpha > 0) {
            SolidBrush newTitleBrush(Color(newAlpha,
                textCol.GetRed(), textCol.GetGreen(), textCol.GetBlue()));
            RectF titleBound;
            gfx.MeasureString(title.c_str(), -1, &titleFont, measureRect, &titleBound);
            g_TextWidth = (int)titleBound.Width;

            if (g_TextWidth > contentW) {
                g_IsScrolling = true;
                float drawX = (float)(contentX - g_ScrollOffset);
                gfx.DrawString(title.c_str(), -1, &titleFont, PointF(drawX, startY), &newTitleBrush);
                if (drawX + g_TextWidth < contentX + contentW + 20)
                    gfx.DrawString(title.c_str(), -1, &titleFont,
                        PointF(drawX + g_TextWidth + 40, startY), &newTitleBrush);
            } else {
                g_IsScrolling = false; g_ScrollOffset = 0;
                gfx.DrawString(title.c_str(), -1, &titleFont,
                    PointF((float)contentX, startY), &newTitleBrush);
            }
        }
        gfx.ResetClip();

        // ── Artist ───────────────────────────────────────────────────────────
        if (fading && !prevArtist.empty() && oldAlpha > 0) {
            SolidBrush oldArtBrush(Color((BYTE)((oldAlpha * 180) / 255),
                textCol.GetRed(), textCol.GetGreen(), textCol.GetBlue()));
            RectF rOld((REAL)contentX, startY + lineH, (REAL)contentW, subH);
            StringFormat sfOld; sfOld.SetTrimming(StringTrimmingEllipsisCharacter);
            sfOld.SetFormatFlags(StringFormatFlagsNoWrap);
            gfx.DrawString(prevArtist.c_str(), -1, &artistFont, rOld, &sfOld, &oldArtBrush);
        }

        if (newAlpha > 0) {
            BYTE artAlpha = (BYTE)((newAlpha * 180) / 255);
            SolidBrush newArtBrush(Color(artAlpha,
                textCol.GetRed(), textCol.GetGreen(), textCol.GetBlue()));
            RectF artistR((REAL)contentX, startY + lineH, (REAL)contentW, subH);
            StringFormat sf3; sf3.SetTrimming(StringTrimmingEllipsisCharacter);
            sf3.SetFormatFlags(StringFormatFlagsNoWrap);
            gfx.DrawString(artist.empty() ? L"Unknown artist" : artist.c_str(),
                -1, &artistFont, artistR, &sf3, &newArtBrush);
        }
    }

    // ── 3. Transport Controls ────────────────────────────────────────────────
    float ctrlY  = startY + totalTextH + 6.0f;
    float ctrlCY = ctrlY + ctrlH / 2.0f;

    float unit      = 11.0f * s * 0.5f;
    float playR     = unit * 1.9f;
    float prevNextR = unit * 1.3f;
    float btnGap    = unit * 2.6f;

    float trioW      = prevNextR*2 + btnGap + playR*2 + btnGap + prevNextR*2;
    float trioStartX = contentX + (contentW - trioW) / 2.0f;
    float prevCX     = trioStartX + prevNextR;
    float playCX     = prevCX + prevNextR + btnGap + playR;
    float nextCX     = playCX + playR + btnGap + prevNextR;

    g_HitPrev = { (int)prevCX, (int)ctrlCY, (int)(prevNextR + 7) };
    g_HitPlay = { (int)playCX, (int)ctrlCY, (int)(playR     + 6) };
    g_HitNext = { (int)nextCX, (int)ctrlCY, (int)(prevNextR + 7) };

    bool lightMode = IsSystemLightMode();
    Color iconColor     = hasMedia ? textCol : Color(70, textCol.GetRed(), textCol.GetGreen(), textCol.GetBlue());
    Color hoverBgCol    = Color(35,  textCol.GetRed(), textCol.GetGreen(), textCol.GetBlue());
    Color playCircleCol = hasMedia
        ? Color(255, textCol.GetRed(), textCol.GetGreen(), textCol.GetBlue())
        : Color(50,  textCol.GetRed(), textCol.GetGreen(), textCol.GetBlue());
    Color playIconCol   = Color(255,
        lightMode ? 250 : 15, lightMode ? 250 : 15, lightMode ? 250 : 15);

    SolidBrush iconBrush(iconColor), hoverBrush(Color(255, textCol.GetRed(), textCol.GetGreen(), textCol.GetBlue()));
    SolidBrush hoverBgBrush(hoverBgCol), playCircleBrush(playCircleCol), playIconBrush(playIconCol);

    // PREV
    {
        bool hov = (g_HoverState == 1);
        if (hov) gfx.FillEllipse(&hoverBgBrush,
            prevCX - prevNextR - 3, ctrlCY - prevNextR - 3,
            (prevNextR+3)*2, (prevNextR+3)*2);
        SolidBrush& b = hov ? hoverBrush : iconBrush;
        float tw = unit*0.72f, th = unit*1.05f, bw = unit*0.22f, gap2 = unit*0.18f;
        gfx.FillRectangle(&b, prevCX - tw - gap2 - bw, ctrlCY - th, bw, th*2);
        PointF tri[3]={ PointF(prevCX+tw, ctrlCY-th), PointF(prevCX+tw, ctrlCY+th), PointF(prevCX-tw+gap2, ctrlCY) };
        gfx.FillPolygon(&b, tri, 3);
    }

    // PLAY/PAUSE
    {
        bool hov = (g_HoverState == 2);
        float r = hov ? playR*1.06f : playR;
        gfx.FillEllipse(&playCircleBrush, playCX-r, ctrlCY-r, r*2, r*2);
        if (isPlaying && hasMedia) {
            float bw=playR*0.19f, bh=playR*0.80f, bx=playR*0.22f;
            GraphicsPath bar1, bar2;
            int ibw=(int)(bw+0.5f), ibh=(int)(bh+0.5f);
            AddRoundedRect(bar1,(int)(playCX-bx-bw/2),(int)(ctrlCY-bh/2),ibw,ibh,ibw/2);
            AddRoundedRect(bar2,(int)(playCX+bx-bw/2),(int)(ctrlCY-bh/2),ibw,ibh,ibw/2);
            gfx.FillPath(&playIconBrush,&bar1); gfx.FillPath(&playIconBrush,&bar2);
        } else {
            float tw=playR*0.38f, th=playR*0.50f, ox=playR*0.07f;
            PointF tri[3]={ PointF(playCX-tw+ox,ctrlCY-th), PointF(playCX-tw+ox,ctrlCY+th), PointF(playCX+tw+ox,ctrlCY) };
            gfx.FillPolygon(&playIconBrush,tri,3);
        }
    }

    // NEXT
    {
        bool hov = (g_HoverState == 3);
        if (hov) gfx.FillEllipse(&hoverBgBrush,
            nextCX - prevNextR - 3, ctrlCY - prevNextR - 3,
            (prevNextR+3)*2, (prevNextR+3)*2);
        SolidBrush& b = hov ? hoverBrush : iconBrush;
        float tw=unit*0.72f, th=unit*1.05f, bw=unit*0.22f, gap2=unit*0.18f;
        PointF tri[3]={ PointF(nextCX-tw-gap2,ctrlCY-th), PointF(nextCX-tw-gap2,ctrlCY+th), PointF(nextCX+tw,ctrlCY) };
        gfx.FillPolygon(&b,tri,3);
        gfx.FillRectangle(&b, nextCX+tw+gap2, ctrlCY-th, bw, th*2);
    }

    // ── 4. Progress Bar ───────────────────────────────────────────────────────
    float progY   = ctrlY + ctrlH + progressMarginT;
    int   progX   = contentX, progW = contentW;
    int   progBarH = (int)(progressH + 2*s*0.3f);
    int   progBarY = (int)progY;

    double showPos = g_Scrubbing ? (double)(g_ScrubPos * dur) : displayPos;
    wstring tPos = FormatTime(showPos);
    wstring tDur = dur > 0 ? FormatTime(dur) : L"--:--";

    Font tf(&ff, (REAL)timeFont, FontStyleBold, UnitPixel);
    RectF timeBound;
    gfx.MeasureString(tDur.c_str(), -1, &tf, measureRect, &timeBound);
    int timeLabelW = (int)(timeBound.Width + 2);

    int barX = progX, barW2 = progW, barY2 = progBarY + timeFont + 3;
    g_ProgressRect = { barX, barY2, barW2, progBarH + 4 };

    int cornerR = progBarH / 2;
    GraphicsPath trackPath;
    AddRoundedRect(trackPath, barX, barY2, barW2, progBarH, cornerR);
    SolidBrush trackBrush(Color(70, textCol.GetRed(), textCol.GetGreen(), textCol.GetBlue()));
    gfx.FillPath(&trackBrush, &trackPath);

    double progress = g_Scrubbing ? g_ScrubPos : (dur > 0 ? min(1.0, displayPos/dur) : 0.0);
    int fillW = (int)(barW2 * progress);
    if (fillW > 2) {
        GraphicsPath fillPath;
        AddRoundedRect(fillPath, barX, barY2, fillW, progBarH, cornerR);
        SolidBrush fillBrush(textCol);
        gfx.FillPath(&fillBrush, &fillPath);
        if (g_Scrubbing || g_HoverState == 4) {
            float thumbR = (float)progBarH * 1.4f;
            gfx.FillEllipse(&fillBrush,
                (float)(barX+fillW)-thumbR, barY2+progBarH/2.0f-thumbR,
                thumbR*2, thumbR*2);
        }
    }

    SolidBrush timeBrush(subCol);
    gfx.DrawString(tPos.c_str(), -1, &tf, PointF((float)barX, (float)progBarY), &timeBrush);
    RectF durRect((REAL)(barX+barW2-timeLabelW),(REAL)progBarY,(REAL)timeLabelW,(REAL)(timeFont+2));
    StringFormat sfR; sfR.SetAlignment(StringAlignmentFar);
    gfx.DrawString(tDur.c_str(), -1, &tf, durRect, &sfR, &timeBrush);
}

// ─── Taskbar Hook ─────────────────────────────────────────────────────────────
bool IsTaskbar(HWND hwnd) {
    WCHAR cls[64]; GetClassNameW(hwnd, cls, 64);
    return wcscmp(cls, L"Shell_TrayWnd") == 0;
}
void CALLBACK TaskbarEventProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (!IsTaskbar(hwnd) || !g_hWnd) return;
    PostMessage(g_hWnd, WM_APP+10, 0, 0);
}
void RegisterHook(HWND hwnd) {
    HWND hTB = FindWindow(L"Shell_TrayWnd", nullptr);
    if (hTB) {
        DWORD pid=0, tid=GetWindowThreadProcessId(hTB, &pid);
        if (tid) g_Hook = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            nullptr, TaskbarEventProc, pid, tid,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    PostMessage(hwnd, WM_APP+10, 0, 0);
}

// ─── Window Procedure ─────────────────────────────────────────────────────────
#define IDT_MEDIA    1001
#define IDT_ANIM     1002
#define IDT_PROGRESS 1003
#define IDT_FADE     1004
#define APP_CLOSE    WM_APP

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wP, LPARAM lP) {
    switch (msg) {
    case WM_CREATE:
        UpdateAppearance(hwnd);
        g_CurrentPollInterval = 500;
        SetTimer(hwnd, IDT_MEDIA,    g_CurrentPollInterval, NULL);
        SetTimer(hwnd, IDT_PROGRESS, 100, NULL);
        RegisterHook(hwnd);
        g_HoverState = -1;
        g_InterpolationAnchor = std::chrono::steady_clock::now();
        return 0;

    case WM_ERASEBKGND: return 1;
    case WM_CLOSE:      return 0;
    case APP_CLOSE:     DestroyWindow(hwnd); return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_MEDIA);
        KillTimer(hwnd, IDT_PROGRESS);
        KillTimer(hwnd, IDT_FADE);
        KillTimer(hwnd, IDT_ANIM);
        if (g_Hook) { UnhookWinEvent(g_Hook); g_Hook = nullptr; }
        g_SessionManager = nullptr;
        { lock_guard<mutex> fg(g_FadeMtx);
          if (g_PrevArt) { delete g_PrevArt; g_PrevArt = nullptr; } }
        // Cleanup cached resources
        if (g_CachedTrackBrush) { delete g_CachedTrackBrush; g_CachedTrackBrush = nullptr; }
        if (g_CachedFillBrush) { delete g_CachedFillBrush; g_CachedFillBrush = nullptr; }
        if (g_CachedTimeBrush) { delete g_CachedTimeBrush; g_CachedTimeBrush = nullptr; }
        if (g_CachedTitleFont) { delete g_CachedTitleFont; g_CachedTitleFont = nullptr; }
        if (g_CachedArtistFont) { delete g_CachedArtistFont; g_CachedArtistFont = nullptr; }
        if (g_CachedTimeFont) { delete g_CachedTimeFont; g_CachedTimeFont = nullptr; }
        if (g_CachedFontFamily) { delete g_CachedFontFamily; g_CachedFontFamily = nullptr; }
        PostQuitMessage(0);
        return 0;

    case WM_SETTINGCHANGE:
        UpdateAppearance(hwnd);
        InvalidateCache();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;

    // ── Crossfade trigger ─────────────────────────────────────────────────────
    case WM_APP + 20: {
        Bitmap* oldArt = reinterpret_cast<Bitmap*>(wP);
        {
            lock_guard<mutex> fg(g_FadeMtx);
            if (g_PrevArt && g_PrevArt != oldArt) delete g_PrevArt;
            g_PrevArt   = oldArt;
            g_FadeAlpha = 0;
            g_Fading    = true;
        }
        g_ScrollOffset = 0;
        g_ScrollWait   = 80;
        g_IsScrolling  = false;
        SetTimer(hwnd, IDT_FADE, FADE_TIMER_MS, NULL);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (wP == IDT_MEDIA) {
            UpdateMediaInfo();
            bool shouldHide = false;
            if (g_Settings.hideFullscreen) {
                QUERY_USER_NOTIFICATION_STATE qs;
                if (SUCCEEDED(SHQueryUserNotificationState(&qs)))
                    if (qs==QUNS_BUSY||qs==QUNS_RUNNING_D3D_FULL_SCREEN||qs==QUNS_PRESENTATION_MODE)
                        shouldHide = true;
            }
            bool playing=false, ytm=false;
            { lock_guard<mutex> g(g_Media.lock); playing=g_Media.isPlaying; ytm=g_Media.isYTM; }
            
            // Adaptive polling: faster when playing, slower when paused/idle
            int newInterval = (playing && ytm) ? 250 : 1000;
            if (newInterval != g_CurrentPollInterval) {
                g_CurrentPollInterval = newInterval;
                SetTimer(hwnd, IDT_MEDIA, newInterval, NULL);
            }
            g_IsPlaying = playing;

            if (!ytm) { shouldHide=true; g_IdleCounter=0; g_IsHiddenByIdle=false; }
            if (ytm && g_Settings.idleTimeout>0) {
                if (playing) { g_IdleCounter=0; g_IsHiddenByIdle=false; }
                else { if (++g_IdleCounter>=g_Settings.idleTimeout) g_IsHiddenByIdle=true; }
            } else if (playing) { g_IsHiddenByIdle=false; }
            if (g_IsHiddenByIdle) shouldHide=true;
            if (shouldHide  && IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            if (!shouldHide && !IsWindowVisible(hwnd)) {
                HWND hTB=FindWindow(L"Shell_TrayWnd",nullptr);
                if (hTB && IsWindowVisible(hTB)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        else if (wP == IDT_PROGRESS) {
            InvalidateRect(hwnd, NULL, FALSE);
        }
        else if (wP == IDT_FADE) {
            bool done = false;
            {
                lock_guard<mutex> fg(g_FadeMtx);
                g_FadeAlpha += FADE_STEP;
                if (g_FadeAlpha >= 255) {
                    g_FadeAlpha = 255;
                    g_Fading    = false;
                    done        = true;
                    if (g_PrevArt) { delete g_PrevArt; g_PrevArt = nullptr; }
                }
            }
            if (done) KillTimer(hwnd, IDT_FADE);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        else if (wP == IDT_ANIM) {
            if (g_IsScrolling) {
                if (g_ScrollWait > 0) g_ScrollWait--;
                else {
                    g_ScrollOffset++;
                    if (g_ScrollOffset > g_TextWidth+40) { g_ScrollOffset=0; g_ScrollWait=80; }
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            } else KillTimer(hwnd, IDT_ANIM);
        }
        return 0;

    case WM_APP+10: {
        HWND hTB = FindWindow(TEXT("Shell_TrayWnd"), nullptr);
        if (!hTB) break;
        if (!IsWindowVisible(hTB)) { if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE); return 0; }
        if (!g_IsHiddenByIdle && !IsWindowVisible(hwnd)) {
            bool ytm=false; { lock_guard<mutex> g(g_Media.lock); ytm=g_Media.isYTM; }
            if (ytm) {
                bool gameHide=false;
                if (g_Settings.hideFullscreen) {
                    QUERY_USER_NOTIFICATION_STATE qs;
                    if (SUCCEEDED(SHQueryUserNotificationState(&qs)))
                        if (qs==QUNS_BUSY||qs==QUNS_RUNNING_D3D_FULL_SCREEN||qs==QUNS_PRESENTATION_MODE)
                            gameHide=true;
                }
                if (!gameHide) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            }
        }
        RECT rc; GetWindowRect(hTB, &rc);
        int tbH=rc.bottom-rc.top;
        int x=rc.left+g_Settings.offsetX;
        int y=rc.top+tbH/2-g_Settings.height/2+g_Settings.offsetY;
        RECT mine; GetWindowRect(hwnd, &mine);
        if (mine.left!=x||mine.top!=y||mine.right-mine.left!=g_Settings.width||mine.bottom-mine.top!=g_Settings.height)
            SetWindowPos(hwnd, HWND_TOPMOST, x, y, g_Settings.width, g_Settings.height, SWP_NOACTIVATE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int mx=GET_X_LPARAM(lP), my=GET_Y_LPARAM(lP);
        if (g_Scrubbing) {
            g_ScrubPos = max(0.0f, min(1.0f,
                (float)(mx-g_ProgressRect.x) / max(1, g_ProgressRect.w)));
            InvalidateRect(hwnd, NULL, FALSE);
        }
        int ns=-1;
        auto inCircle=[](int px,int py,HitArea& ha){ int dx=px-ha.x,dy=py-ha.y; return dx*dx+dy*dy<=ha.r*ha.r; };
        if      (inCircle(mx,my,g_HitPrev)) ns=1;
        else if (inCircle(mx,my,g_HitPlay)) ns=2;
        else if (inCircle(mx,my,g_HitNext)) ns=3;
        else if (mx>=g_ProgressRect.x && mx<=g_ProgressRect.x+g_ProgressRect.w &&
                 my>=g_ProgressRect.y-6 && my<=g_ProgressRect.y+g_ProgressRect.h+6) ns=4;
        if (ns!=g_HoverState) { g_HoverState=ns; InvalidateRect(hwnd,NULL,FALSE); }
        TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,hwnd,0};
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        g_HoverState=-1; InvalidateRect(hwnd,NULL,FALSE); break;

    case WM_LBUTTONDOWN: {
        int mx=GET_X_LPARAM(lP), my=GET_Y_LPARAM(lP);
        if (mx>=g_ProgressRect.x && mx<=g_ProgressRect.x+g_ProgressRect.w &&
            my>=g_ProgressRect.y-8 && my<=g_ProgressRect.y+g_ProgressRect.h+8) {
            g_Scrubbing=true;
            g_ScrubPos=max(0.0f,min(1.0f,(float)(mx-g_ProgressRect.x)/max(1,g_ProgressRect.w)));
            SetCapture(hwnd); InvalidateRect(hwnd,NULL,FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int mx=GET_X_LPARAM(lP), my=GET_Y_LPARAM(lP);
        if (g_Scrubbing) {
            g_Scrubbing=false; ReleaseCapture();
            g_ScrubPos=max(0.0f,min(1.0f,(float)(mx-g_ProgressRect.x)/max(1,g_ProgressRect.w)));
            double dur2=0; { lock_guard<mutex> g(g_Media.lock); dur2=g_Media.duration; }
            if (dur2>0) SeekTo(g_ScrubPos*dur2);
            RedrawWindow(hwnd,NULL,NULL,RDW_INVALIDATE|RDW_UPDATENOW);
        } else {
            auto inCircle=[](int px,int py,HitArea& ha){ int dx=px-ha.x,dy=py-ha.y; return dx*dx+dy*dy<=ha.r*ha.r; };
            if      (inCircle(mx,my,g_HitPrev)) SendMediaCommand(1);
            else if (inCircle(mx,my,g_HitPlay)) SendMediaCommand(2);
            else if (inCircle(mx,my,g_HitNext)) SendMediaCommand(3);
        }
        return 0;
    }

    case WM_KEYDOWN:
        switch (wP) {
            case VK_SPACE: SendMediaCommand(2); break;
            case VK_LEFT:  SendMediaCommand(1); break;
            case VK_RIGHT: SendMediaCommand(3); break;
        }
        return 0;

    case WM_MOUSEWHEEL: {
        short z=GET_WHEEL_DELTA_WPARAM(wP);
        keybd_event(z>0?VK_VOLUME_UP:VK_VOLUME_DOWN,0,0,0);
        keybd_event(z>0?VK_VOLUME_UP:VK_VOLUME_DOWN,0,KEYEVENTF_KEYUP,0);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc=BeginPaint(hwnd,&ps);
        RECT rc; GetClientRect(hwnd,&rc);
        HDC mem=CreateCompatibleDC(hdc);
        HBITMAP bmp=CreateCompatibleBitmap(hdc,rc.right,rc.bottom);
        HBITMAP old=(HBITMAP)SelectObject(mem,bmp);
        DrawPanel(mem,rc.right,rc.bottom);
        if (g_IsScrolling) SetTimer(hwnd,IDT_ANIM,16,NULL);
        BitBlt(hdc,0,0,rc.right,rc.bottom,mem,0,0,SRCCOPY);
        SelectObject(mem,old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd,&ps);
        return 0;
    }

    default:
        if (msg==g_TaskbarMsg) {
            if (g_Hook) { UnhookWinEvent(g_Hook); g_Hook=nullptr; }
            RegisterHook(hwnd); return 0;
        }
        break;
    }
    return DefWindowProc(hwnd,msg,wP,lP);
}

void TriggerCrossfade(Bitmap* newArt) {
    if (g_hWnd) PostMessage(g_hWnd, WM_APP+20, (WPARAM)newArt, 0);
}

// ─── Thread ───────────────────────────────────────────────────────────────────
void MediaThread() {
    winrt::init_apartment();
    GdiplusStartupInput gsi; ULONG_PTR tok;
    GdiplusStartup(&tok, &gsi, NULL);

    WNDCLASS wc={};
    wc.lpfnWndProc=WndProc; wc.hInstance=GetModuleHandle(NULL);
    wc.lpszClassName=TEXT("WindhawkMusicLounge_YTM");
    wc.hCursor=LoadCursor(NULL,IDC_HAND);
    RegisterClass(&wc);

    HMODULE hU=GetModuleHandle(L"user32.dll");
    pCreateWindowInBand CWB=hU?(pCreateWindowInBand)GetProcAddress(hU,"CreateWindowInBand"):nullptr;
    if (CWB) {
        g_hWnd=CWB(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,
            wc.lpszClassName,TEXT("MusicLoungeYTM"),WS_POPUP|WS_VISIBLE,
            0,0,g_Settings.width,g_Settings.height,
            NULL,NULL,wc.hInstance,NULL,ZBID_IMMERSIVE_NOTIFICATION);
    }
    if (!g_hWnd) {
        g_hWnd=CreateWindowEx(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,
            wc.lpszClassName,TEXT("MusicLoungeYTM"),WS_POPUP|WS_VISIBLE,
            0,0,g_Settings.width,g_Settings.height,
            NULL,NULL,wc.hInstance,NULL);
    }
    SetLayeredWindowAttributes(g_hWnd,0,255,LWA_ALPHA);

    MSG msg;
    while (GetMessage(&msg,NULL,0,0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

    UnregisterClass(wc.lpszClassName,wc.hInstance);
    GdiplusShutdown(tok);
    winrt::uninit_apartment();
}

std::thread* g_pThread=nullptr;

BOOL WhTool_ModInit() {
    SetCurrentProcessExplicitAppUserModelID(L"taskbar-music-lounge-ytm");
    LoadSettings(); g_Running=true;
    g_pThread=new std::thread(MediaThread);
    return TRUE;
}
void WhTool_ModUninit() {
    g_Running=false;
    if (g_hWnd) SendMessage(g_hWnd,APP_CLOSE,0,0);
    if (g_pThread) { if (g_pThread->joinable()) g_pThread->join(); delete g_pThread; g_pThread=nullptr; }
}
void WhTool_ModSettingsChanged() {
    LoadSettings();
    if (g_hWnd) { SendMessage(g_hWnd,WM_TIMER,IDT_MEDIA,0); SendMessage(g_hWnd,WM_SETTINGCHANGE,0,0); }
}

// ─── Windhawk Boilerplate ─────────────────────────────────────────────────────
bool g_isLauncher; HANDLE g_mutex;
void WINAPI EntryPoint_Hook() { Wh_Log(L">"); ExitThread(0); }

BOOL Wh_ModInit() {
    bool isService=false,isTool=false,isCurrent=false;
    int argc; LPWSTR* argv=CommandLineToArgvW(GetCommandLine(),&argc);
    if (!argv) return FALSE;
    for (int i=1;i<argc;i++) if (wcscmp(argv[i],L"-service")==0){isService=true;break;}
    for (int i=1;i<argc-1;i++) if (wcscmp(argv[i],L"-tool-mod")==0){
        isTool=true; if(wcscmp(argv[i+1],WH_MOD_ID)==0) isCurrent=true; break;
    }
    LocalFree(argv);
    if (isService) return FALSE;
    if (isCurrent) {
        g_mutex=CreateMutex(nullptr,TRUE,L"windhawk-tool-mod_" WH_MOD_ID);
        if (!g_mutex) ExitProcess(1);
        if (GetLastError()==ERROR_ALREADY_EXISTS) ExitProcess(1);
        if (!WhTool_ModInit()) ExitProcess(1);
        IMAGE_DOS_HEADER* dos=(IMAGE_DOS_HEADER*)GetModuleHandle(nullptr);
        IMAGE_NT_HEADERS* nt=(IMAGE_NT_HEADERS*)((BYTE*)dos+dos->e_lfanew);
        void* ep=(BYTE*)dos+nt->OptionalHeader.AddressOfEntryPoint;
        Wh_SetFunctionHook(ep,(void*)EntryPoint_Hook,nullptr);
        return TRUE;
    }
    if (isTool) return FALSE;
    g_isLauncher=true; return TRUE;
}

void Wh_ModAfterInit() {
    if (!g_isLauncher) return;
    WCHAR path[MAX_PATH];
    if (!GetModuleFileName(nullptr,path,MAX_PATH)) return;
    WCHAR cmd[MAX_PATH+64];
    swprintf_s(cmd,L"\"%s\" -tool-mod \"%s\"",path,WH_MOD_ID);
    HMODULE hK=GetModuleHandle(L"kernelbase.dll");
    if (!hK) hK=GetModuleHandle(L"kernel32.dll");
    if (!hK) return;
    using CPIW=BOOL(WINAPI*)(HANDLE,LPCWSTR,LPWSTR,LPSECURITY_ATTRIBUTES,LPSECURITY_ATTRIBUTES,WINBOOL,DWORD,LPVOID,LPCWSTR,LPSTARTUPINFOW,LPPROCESS_INFORMATION,PHANDLE);
    CPIW pCPIW=(CPIW)GetProcAddress(hK,"CreateProcessInternalW");
    if (!pCPIW) return;
    STARTUPINFO si{.cb=sizeof(STARTUPINFO),.dwFlags=STARTF_FORCEOFFFEEDBACK};
    PROCESS_INFORMATION pi;
    if (pCPIW(nullptr,path,cmd,nullptr,nullptr,FALSE,NORMAL_PRIORITY_CLASS,nullptr,nullptr,&si,&pi,nullptr)){
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
}

void Wh_ModSettingsChanged() { if (!g_isLauncher) WhTool_ModSettingsChanged(); }
void Wh_ModUninit() { if (!g_isLauncher) { WhTool_ModUninit(); ExitProcess(0); } }