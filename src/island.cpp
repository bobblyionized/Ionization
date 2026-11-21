#include "island.h"
#include "common.h"
#include "media.h"
#include "audio.h"
#include "shellutil.h"
#include "clipboard.h"
#include "json.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <oleidl.h>
#include <atomic>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <shellscalingapi.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <map>
#include <cmath>

using Microsoft::WRL::ComPtr;
ComPtr<IDWriteFontCollection1> LoadEmbeddedFonts(IDWriteFactory5* f);   // clock.cpp

namespace {
D2D1_COLOR_F Hex(UINT rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}
enum { T_POLL = 1, T_ANIM = 2, T_SLOW = 3 };
const UINT WM_MEDIA = WM_APP + 40, WM_VOLUME = WM_APP + 41;
const float CARD_W = 480, CARD_H = 204, CARD_H_MAX = 300;
const float CLIP_ROW = 46;

struct Spring {
    float x = 0, v = 0, target = 0;
    bool Step(float k = 0.22f, float d = 0.68f) {
        v = (v + (target - x) * k) * d;
        x += v;
        if (std::fabs(target - x) < 0.25f && std::fabs(v) < 0.25f) { x = target; v = 0; return false; }
        return true;
    }
};

struct Hit { D2D1_RECT_F r; int id; };
enum HitId { H_PREV = 1, H_PLAY, H_NEXT, H_SEEK, H_MIC, H_PICK, H_SNIP, H_VOL, H_BRI,
             H_T5, H_T15, H_T25, H_T45, H_TSTOP,
             H_TAB0 = 100,            // + tab
             H_CHIP0 = 200,           // + clipboard filter
             H_SHELFCLEAR = 300,
             H_CLIPROW = 1000,        // + row * 4 + {0 row, 1 pin, 2 delete}
             H_SHELF = 5000 };        // + item * 2 + {0 tile, 1 remove}
const int kTabs = 5;

std::wstring Ago(long long ms) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    long long now = (long long)(((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ULL) / 10000);
    long long s = std::max(0LL, (now - ms) / 1000);
    wchar_t t[32];
    if (s < 60) return L"now";
    if (s < 3600) swprintf_s(t, L"%lldm", s / 60);
    else if (s < 86400) swprintf_s(t, L"%lldh", s / 3600);
    else swprintf_s(t, L"%lldd", s / 86400);
    return t;
}

// ---------------------------------------------------------------- color picker
struct Picker {
    HWND catcher = nullptr, mag = nullptr;
    std::function<void(COLORREF)> done;
    static LRESULT CALLBACK CatchProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* p = (Picker*)GetWindowLongPtrW(h, GWLP_USERDATA);
        switch (m) {
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_CROSS)); return TRUE;
        case WM_MOUSEMOVE: if (p) p->Move(); return 0;
        case WM_LBUTTONDOWN: if (p) p->Finish(true); return 0;
        case WM_RBUTTONDOWN: if (p) p->Finish(false); return 0;
        case WM_KEYDOWN: if (w == VK_ESCAPE && p) p->Finish(false); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
    static LRESULT CALLBACK MagProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_PAINT) {
            PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            POINT pt; GetCursorPos(&pt);
            HDC sdc = GetDC(nullptr);
            int zoomW = rc.right, zoomH = rc.right;   // square zoom area
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchBlt(dc, 0, 0, zoomW, zoomH, sdc, pt.x - 7, pt.y - 7, 15, 15, SRCCOPY);
            COLORREF c = GetPixel(sdc, pt.x, pt.y);
            ReleaseDC(nullptr, sdc);
            // centre pixel frame
            HBRUSH frame = CreateSolidBrush(RGB(212, 245, 60));
            RECT cr{ 7 * zoomW / 15, 7 * zoomH / 15, 8 * zoomW / 15, 8 * zoomH / 15 };
            FrameRect(dc, &cr, frame);
            DeleteObject(frame);
            RECT bar{ 0, zoomH, rc.right, rc.bottom };
            HBRUSH bg = CreateSolidBrush(RGB(13, 14, 17)); FillRect(dc, &bar, bg); DeleteObject(bg);
            RECT sw{ 8, zoomH + 7, 24, zoomH + 23 };
            HBRUSH sb = CreateSolidBrush(c); FillRect(dc, &sw, sb); DeleteObject(sb);
            wchar_t hex[16]; swprintf_s(hex, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(243, 244, 246));
            HFONT f = CreateFontW(-14, 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Cascadia Mono");
            HGDIOBJ old = SelectObject(dc, f);
            RECT tr{ 32, zoomH, rc.right, rc.bottom };
            DrawTextW(dc, hex, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old); DeleteObject(f);
            EndPaint(h, &ps);
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
    void Start() {
        if (catcher) return;
        static bool reg = false;
        if (!reg) {
            WNDCLASSW a{}; a.lpfnWndProc = CatchProc; a.hInstance = GetModuleHandleW(nullptr); a.lpszClassName = L"IonizationPickCatch"; RegisterClassW(&a);
            WNDCLASSW b{}; b.lpfnWndProc = MagProc; b.hInstance = GetModuleHandleW(nullptr); b.lpszClassName = L"IonizationPickMag"; RegisterClassW(&b);
            reg = true;
        }
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        // Almost-invisible full-screen catcher: takes the click, never shows up in the pixels we read.
        catcher = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"IonizationPickCatch", L"", WS_POPUP,
            vx, vy, vw, vh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowLongPtrW(catcher, GWLP_USERDATA, (LONG_PTR)this);
        SetLayeredWindowAttributes(catcher, 0, 1, LWA_ALPHA);
        SetWindowDisplayAffinity(catcher, WDA_EXCLUDEFROMCAPTURE);
        mag = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"IonizationPickMag", L"", WS_POPUP,
            0, 0, 150, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowDisplayAffinity(mag, WDA_EXCLUDEFROMCAPTURE);
        HRGN rgn = CreateRoundRectRgn(0, 0, 151, 181, 14, 14); SetWindowRgn(mag, rgn, FALSE);
        ShowWindow(catcher, SW_SHOW);
        SetForegroundWindow(catcher);
        Move();
        ShowWindow(mag, SW_SHOWNOACTIVATE);
    }
    void Move() {
        if (!mag) return;
        POINT pt; GetCursorPos(&pt);
        HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(hm, &mi);
        int x = pt.x + 24, y = pt.y + 24;
        if (x + 150 > mi.rcMonitor.right) x = pt.x - 174;
        if (y + 180 > mi.rcMonitor.bottom) y = pt.y - 204;
        SetWindowPos(mag, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        InvalidateRect(mag, nullptr, FALSE);
    }
    void Finish(bool pick) {
        COLORREF c = 0;
        if (pick) { POINT pt; GetCursorPos(&pt); HDC sdc = GetDC(nullptr); c = GetPixel(sdc, pt.x, pt.y); ReleaseDC(nullptr, sdc); }
        if (mag) { DestroyWindow(mag); mag = nullptr; }
        if (catcher) { DestroyWindow(catcher); catcher = nullptr; }
        if (pick && done) done(c);
    }
};

} // namespace

struct Island::Impl {
    IslandSettings s;
    bool inGame = false;
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> dev;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<ID2D1Factory1> d2d;
    ComPtr<ID2D1Device> d2dDev;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> bitmap;
    ComPtr<IDWriteFactory5> dw;
    ComPtr<IDWriteFontCollection1> fonts;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1Bitmap> art;
    unsigned artVersion = ~0u;

    MediaWatcher media;
    MediaInfo mi;
    Privacy::InUse use;
    int micMuted = -1;
    float volume = -1;
    int brightness = -1;
    bool noExclude = false;
    ULONGLONG lastBriSet = 0;
    bool briPending = false;
    std::map<uint64_t, ComPtr<IDWriteTextFormat>> fontCache;
    Picker picker;
    // clipboard tab
    ClipboardHistory* clip = nullptr;
    int clipFilter = 0;
    float clipScroll = 0;
    std::vector<uint64_t> clipIds;            // ids of the rows drawn last, by row index
    std::map<uint64_t, ComPtr<ID2D1Bitmap>> thumbs;
    // shelf tab
    std::vector<std::wstring> shelf;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> icons;
    int shelfScroll = 0;
    bool dropHover = false, dragOut = false, oleInit = false;
    int pressId = 0; POINT pressPt{};
    int hoverId = 0;
    ULONGLONG pinnedSince = 0;
    bool prevButton = false, pressOnPill = false;

    int winW = 0, winH = 0;
    float k = 1;
    RECT mon{};
    Spring w, h;               // pill size (px)
    float expand = 0;          // 0 collapsed .. 1 card
    bool expanded = false, pinnedOpen = false, dragging = false;
    int dragId = 0, tab = 0;
    std::vector<Hit> hits;
    ULONGLONG hoverSince = 0, leaveSince = 0, osdUntil = 0, msgUntil = 0, timerEnd = 0, timerTotal = 0, lastEq = 0;
    float osdValue = 0;
    std::wstring msg;
    unsigned msgColor = 0;
    bool clickThrough = true, visibleNow = false;

    // ---------------------------------------------------------------- setup
    bool EnsureDevice() {
        if (dev) return true;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, &dev, nullptr, nullptr))) return false;
        ComPtr<IDXGIDevice> dxgi; dev.As(&dxgi);
        if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp)))) return false;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, (void**)d2d.GetAddressOf());
        if (!d2d || FAILED(d2d->CreateDevice(dxgi.Get(), &d2dDev))) return false;
        d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), (IUnknown**)dw.GetAddressOf());
        fonts = LoadEmbeddedFonts(dw.Get());
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        return dc && dw && wic;
    }

    static LRESULT CALLBACK Proc(HWND hw, UINT m, WPARAM wp, LPARAM lp) {
        auto* self = (Impl*)GetWindowLongPtrW(hw, GWLP_USERDATA);
        return self ? self->Handle(hw, m, wp, lp) : DefWindowProcW(hw, m, wp, lp);
    }

    void Create() {
        if (hwnd) return;
        static bool reg = false;
        if (!reg) {
            WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"IonizationIsland"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.style = CS_DBLCLKS;
            RegisterClassW(&wc); reg = true;
        }
        hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP,
            L"IonizationIsland", L"Ionization Island", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        SetWindowDisplayAffinity(hwnd, noExclude ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE);   // stays crisp above the blur overlay
        dcomp->CreateTargetForHwnd(hwnd, TRUE, &target);
        dcomp->CreateVisual(&visual);
        target->SetRoot(visual.Get());
        SetTimer(hwnd, T_POLL, 50, nullptr);
        SetTimer(hwnd, T_SLOW, 2000, nullptr);
        media.Start(hwnd, WM_MEDIA);
        Audio::WatchVolume(hwnd, WM_VOLUME);
        micMuted = Audio::MicMuted();
        use = Privacy::Poll();
        picker.done = [this](COLORREF c) {
            wchar_t hex[16]; swprintf_s(hex, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
            if (OpenClipboard(hwnd)) {
                EmptyClipboard();
                size_t bytes = (wcslen(hex) + 1) * sizeof(wchar_t);
                HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
                memcpy(GlobalLock(g), hex, bytes); GlobalUnlock(g);
                SetClipboardData(CF_UNICODETEXT, g);
                CloseClipboard();
            }
            Message(std::wstring(hex) + L" copied", (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c) | 0x1000000u);
        };
        LoadShelf();
        RegisterDrop();
        Layout();
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    void RegisterDrop();   // after ShelfDrop

    void Destroy() {
        media.Stop();
        Audio::StopWatching();
        if (hwnd) RevokeDragDrop(hwnd);
        if (oleInit) { OleUninitialize(); oleInit = false; }
        if (hwnd) { DestroyWindow(hwnd); hwnd = nullptr; }
        thumbs.clear(); icons.clear();
        bitmap.Reset(); swap.Reset(); visual.Reset(); target.Reset();
        winW = winH = 0;
    }

    void Layout() {
        auto mons = EnumerateMonitors();
        RECT mr{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        if (!mons.empty()) mr = mons[std::clamp(s.monitor, 0, (int)mons.size() - 1)].rect;
        HMONITOR hm = MonitorFromRect(&mr, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi2{ sizeof(mi2) }; GetMonitorInfoW(hm, &mi2);
        mon = mi2.rcMonitor;
        UINT dx = 96, dy = 96; GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
        float nk = dx / 96.0f * std::clamp(s.size, 70.0f, 140.0f) / 100.0f;
        if (nk != k) fontCache.clear();
        k = nk;
        int ww = (int)std::ceil((CARD_W + 40) * k), hh = (int)std::ceil((CARD_H_MAX + 30) * k);
        SetWindowPos(hwnd, HWND_TOPMOST, mon.left + ((mon.right - mon.left) - ww) / 2, mon.top, ww, hh, SWP_NOACTIVATE);
        if (ww != winW || hh != winH) {
            bitmap.Reset(); dc->SetTarget(nullptr);
            if (swap) swap->ResizeBuffers(2, ww, hh, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
            else {
                ComPtr<IDXGIDevice> dxgi; dev.As(&dxgi);
                ComPtr<IDXGIAdapter> ad; dxgi->GetAdapter(&ad);
                ComPtr<IDXGIFactory2> f; ad->GetParent(IID_PPV_ARGS(&f));
                DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = ww; sd.Height = hh; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
                f->CreateSwapChainForComposition(dev.Get(), &sd, nullptr, &swap);
                visual->SetContent(swap.Get());
            }
            ComPtr<IDXGISurface> surf; swap->GetBuffer(0, IID_PPV_ARGS(&surf));
            auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &bitmap);
            dc->SetTarget(bitmap.Get());
            winW = ww; winH = hh;
        }
        UpdateTargets(true);
    }

    // ---------------------------------------------------------------- state
    bool TimerRunning() { return timerEnd && GetTickCount64() < timerEnd; }
    bool Activity() {
        ULONGLONG now = GetTickCount64();
        return now < msgUntil || now < osdUntil || TimerRunning() || use.mic || use.camera || (mi.has && mi.playing) || micMuted == 1;
    }
    // Collapsed pill size for what's going on right now (DIP).
    void PillTarget(float& pw, float& ph) {
        ULONGLONG now = GetTickCount64();
        ph = 34;
        if (now < msgUntil) {
            auto f = Font(13, DWRITE_FONT_WEIGHT_MEDIUM);
            ComPtr<IDWriteTextLayout> l; dw->CreateTextLayout(msg.c_str(), (UINT32)msg.size(), f.Get(), 2000, 50, &l);
            DWRITE_TEXT_METRICS tm{}; if (l) l->GetMetrics(&tm);
            pw = std::min(CARD_W, tm.width / k * 1.0f + (msgColor ? 70.0f : 44.0f));
        } else if (now < osdUntil) pw = 230;
        else if (TimerRunning()) pw = 176;
        else if (use.mic || use.camera) pw = 168;
        else if (mi.has && mi.playing) pw = 210;
        else if (micMuted == 1) pw = 150;
        else if (s.hidden) { pw = 0; ph = 0; }
        else { pw = 126; ph = 30; }
    }
    void UpdateTargets(bool snap = false) {
        float pw, ph;
        PillTarget(pw, ph);
        float tw = expanded ? CARD_W : pw, th = expanded ? CardH() : ph;
        w.target = tw * k; h.target = th * k;
        if (snap) { w.x = w.target; h.x = h.target; }
        SetTimer(hwnd, T_ANIM, 16, nullptr);
    }

    float CardH() const { return tab == 3 ? CARD_H_MAX : tab == 4 ? 236.0f : CARD_H; }

    D2D1_RECT_F PillRect() {
        float top = s.shape == "notch" ? 0 : 8 * k;
        float cx = winW / 2.0f;
        return D2D1::RectF(cx - w.x / 2, top, cx + w.x / 2, top + h.x);
    }

    void Message(const std::wstring& text, unsigned color = 0, int ms = 5000) {
        msg = text; msgColor = color; msgUntil = GetTickCount64() + ms;
        UpdateTargets();
    }

    // ---------------------------------------------------------------- drawing helpers
    ComPtr<IDWriteTextFormat> Font(float size, DWRITE_FONT_WEIGHT wgt, bool mono = false, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING) {
        uint64_t key = ((uint64_t)(size * 100) << 24) | ((uint64_t)wgt << 8) | ((uint64_t)mono << 4) | (uint64_t)al;
        if (auto it = fontCache.find(key); it != fontCache.end()) return it->second;
        ComPtr<IDWriteTextFormat> f;
        dw->CreateTextFormat(mono ? L"JetBrains Mono" : L"Inter Tight", fonts.Get(), wgt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * k, L"en-us", &f);
        if (f) {
            f->SetTextAlignment(al);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            ComPtr<IDWriteInlineObject> ell; dw->CreateEllipsisTrimmingSign(f.Get(), &ell);
            f->SetTrimming(&tr, ell.Get());
            fontCache[key] = f;
        }
        return f;
    }
    void Text(const std::wstring& t, D2D1_RECT_F r, IDWriteTextFormat* f, ID2D1SolidColorBrush* b, D2D1_COLOR_F c) {
        b->SetColor(c);
        dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    D2D1_RECT_F R(float x, float y, float ww, float hh, D2D1_RECT_F base) {
        return D2D1::RectF(base.left + x * k, base.top + y * k, base.left + (x + ww) * k, base.top + (y + hh) * k);
    }
    void Circle(D2D1_POINT_2F c, float r, ID2D1SolidColorBrush* b, D2D1_COLOR_F col) { b->SetColor(col); dc->FillEllipse(D2D1::Ellipse(c, r, r), b); }
    void Icon(int which, D2D1_RECT_F r, ID2D1SolidColorBrush* b, D2D1_COLOR_F col) {
        float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, u = (r.right - r.left) / 24.0f, sw = 1.8f * u;
        b->SetColor(col);
        auto line = [&](float x1, float y1, float x2, float y2) { dc->DrawLine(D2D1::Point2F(cx + x1 * u, cy + y1 * u), D2D1::Point2F(cx + x2 * u, cy + y2 * u), b, sw); };
        switch (which) {
        case 0: { // play
            ComPtr<ID2D1PathGeometry> g; d2d->CreatePathGeometry(&g); ComPtr<ID2D1GeometrySink> sk; g->Open(&sk);
            sk->BeginFigure(D2D1::Point2F(cx - 4 * u, cy - 6 * u), D2D1_FIGURE_BEGIN_FILLED);
            sk->AddLine(D2D1::Point2F(cx + 6.5f * u, cy)); sk->AddLine(D2D1::Point2F(cx - 4 * u, cy + 6 * u));
            sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); dc->FillGeometry(g.Get(), b); break; }
        case 1: // pause
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 5 * u, cy - 6 * u, cx - 1.5f * u, cy + 6 * u), u, u), b);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx + 1.5f * u, cy - 6 * u, cx + 5 * u, cy + 6 * u), u, u), b); break;
        case 2: case 3: { // next / prev
            float d = which == 2 ? 1.0f : -1.0f;
            ComPtr<ID2D1PathGeometry> g; d2d->CreatePathGeometry(&g); ComPtr<ID2D1GeometrySink> sk; g->Open(&sk);
            sk->BeginFigure(D2D1::Point2F(cx - 5 * u * d, cy - 5 * u), D2D1_FIGURE_BEGIN_FILLED);
            sk->AddLine(D2D1::Point2F(cx + 3 * u * d, cy)); sk->AddLine(D2D1::Point2F(cx - 5 * u * d, cy + 5 * u));
            sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); dc->FillGeometry(g.Get(), b);
            dc->FillRectangle(D2D1::RectF(cx + (d > 0 ? 3.5f : -5.5f) * u, cy - 5 * u, cx + (d > 0 ? 5.5f : -3.5f) * u, cy + 5 * u), b); break; }
        case 4: case 5: { // mic, mic muted
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 3 * u, cy - 8 * u, cx + 3 * u, cy + 2 * u), 3 * u, 3 * u), b, sw);
            ComPtr<ID2D1PathGeometry> g; d2d->CreatePathGeometry(&g); ComPtr<ID2D1GeometrySink> sk; g->Open(&sk);
            sk->BeginFigure(D2D1::Point2F(cx - 6 * u, cy - 1 * u), D2D1_FIGURE_BEGIN_HOLLOW);
            sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + 6 * u, cy - 1 * u), D2D1::SizeF(6 * u, 6 * u), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sk->EndFigure(D2D1_FIGURE_END_OPEN); sk->Close(); dc->DrawGeometry(g.Get(), b, sw);
            line(0, 5, 0, 8); line(-3, 8, 3, 8);
            if (which == 5) { b->SetColor(Hex(0xff6b5e)); line(-7, -8, 7, 8); }
            break; }
        case 6: // speaker
            dc->FillRectangle(D2D1::RectF(cx - 7 * u, cy - 3 * u, cx - 3.5f * u, cy + 3 * u), b);
            line(-3.5f, -3, 1.5f, -7); line(1.5f, -7, 1.5f, 7); line(1.5f, 7, -3.5f, 3);
            line(4.5f, -3, 4.5f, 3); line(7, -5.5f, 7, 5.5f); break;
        case 7: // sun
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.5f * u, 3.5f * u), b, sw);
            for (int i = 0; i < 8; ++i) { float a = i * 3.14159f / 4; line(std::cos(a) * 6, std::sin(a) * 6, std::cos(a) * 8.5f, std::sin(a) * 8.5f); }
            break;
        case 8: // eyedropper
            line(-6, 6, 2, -2); line(1, -5, 5, -1);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 4.5f * u, cy - 4.5f * u), 2.6f * u, 2.6f * u), b);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 6.5f * u, cy + 6.5f * u), 1.2f * u, 1.2f * u), b); break;
        case 9: // snip (dashed frame + scissors-ish corner)
            dc->DrawRectangle(D2D1::RectF(cx - 7 * u, cy - 6 * u, cx + 7 * u, cy + 6 * u), b, sw * 0.8f);
            line(-3, -2, 3, 2); line(-3, 2, 3, -2); break;
        case 10: // timer ring
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + u), 7 * u, 7 * u), b, sw);
            line(0, 1, 0, -3); line(-2, -8.5f, 2, -8.5f); break;
        }
    }

    // ---------------------------------------------------------------- render
    void Render() {
        if (!dc || !bitmap) return;
        ULONGLONG now = GetTickCount64();
        hits.clear();
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        ComPtr<ID2D1SolidColorBrush> b;
        dc->CreateSolidColorBrush(Hex(0xffffff), &b);
        D2D1_RECT_F p = PillRect();
        if (p.right - p.left < 2 || p.bottom - p.top < 2) { dc->EndDraw(); swap->Present(1, 0); dcomp->Commit(); return; }

        // body
        float rad = std::min((p.bottom - p.top) / 2, 26 * k);
        D2D1_COLOR_F fill = s.theme == "graphite" ? Hex(0x16181d) : s.theme == "glass" ? Hex(0x0d0e11, 0.88f) : Hex(0x000000);
        b->SetColor(fill);
        if (s.shape == "notch") {
            // flat top edge, rounded bottom
            dc->FillRoundedRectangle(D2D1::RoundedRect(p, rad, rad), b.Get());
            dc->FillRectangle(D2D1::RectF(p.left, p.top, p.right, p.top + rad), b.Get());
        } else dc->FillRoundedRectangle(D2D1::RoundedRect(p, rad, rad), b.Get());
        if (s.theme != "black") {
            b->SetColor(Hex(0x2b2e35, 0.9f));
            if (s.shape == "notch") {
                // no line along the screen edge: down, round the bottom corners, back up
                float l = p.left + .5f, r = p.right - .5f, bt = p.bottom - .5f, rr = std::max(0.0f, rad - .5f);
                ComPtr<ID2D1PathGeometry> g; d2d->CreatePathGeometry(&g);
                ComPtr<ID2D1GeometrySink> sk; g->Open(&sk);
                sk->BeginFigure(D2D1::Point2F(l, p.top), D2D1_FIGURE_BEGIN_HOLLOW);
                sk->AddLine(D2D1::Point2F(l, bt - rr));
                sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(l + rr, bt), D2D1::SizeF(rr, rr), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sk->AddLine(D2D1::Point2F(r - rr, bt));
                sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(r, bt - rr), D2D1::SizeF(rr, rr), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sk->AddLine(D2D1::Point2F(r, p.top));
                sk->EndFigure(D2D1_FIGURE_END_OPEN); sk->Close();
                dc->DrawGeometry(g.Get(), b.Get(), 1.0f);
            } else
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(p.left + .5f, p.top + .5f, p.right - .5f, p.bottom - .5f), rad, rad), b.Get(), 1.0f);
        }
        dc->PushAxisAlignedClip(p, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        float cardAlpha = std::clamp((h.x / k - 60) / (std::min(CardH(), CARD_H) - 60), 0.0f, 1.0f);
        if (cardAlpha > 0.02f) DrawCard(p, b.Get(), cardAlpha);
        if (cardAlpha < 0.6f) DrawPill(p, b.Get(), 1 - cardAlpha / 0.6f, now);
        dc->PopAxisAlignedClip();
        dc->EndDraw();
        swap->Present(1, 0);
        dcomp->Commit();
    }

    void DrawPill(D2D1_RECT_F p, ID2D1SolidColorBrush* b, float a, ULONGLONG now) {
        float cy = (p.top + p.bottom) / 2, ph = p.bottom - p.top;
        auto fs = Font(12.5f, DWRITE_FONT_WEIGHT_MEDIUM);
        auto fm = Font(12, DWRITE_FONT_WEIGHT_REGULAR, true, DWRITE_TEXT_ALIGNMENT_TRAILING);
        D2D1_COLOR_F fg = Hex(0xf3f4f6, a), volt = Hex(0xd4f53c, a), dim = Hex(0x8c919a, a);
        float L = p.left + 12 * k, Rr = p.right - 12 * k;
        if (now < msgUntil) {
            float x = L;
            if (msgColor) {
                b->SetColor(Hex(msgColor & 0xffffff, a));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, cy - 8 * k, x + 16 * k, cy + 8 * k), 4 * k, 4 * k), b);
                x += 24 * k;
            } else { Circle(D2D1::Point2F(x + 4 * k, cy), 3.5f * k, b, volt); x += 14 * k; }
            Text(msg, D2D1::RectF(x, p.top, Rr, p.bottom), fs.Get(), b, fg);
        } else if (now < osdUntil) {
            Icon(6, D2D1::RectF(L, cy - 10 * k, L + 20 * k, cy + 10 * k), b, fg);
            float bx = L + 30 * k, bw = Rr - 44 * k - bx;
            b->SetColor(Hex(0x2b2e35, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, cy - 3 * k, bx + bw, cy + 3 * k), 3 * k, 3 * k), b);
            b->SetColor(volt); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, cy - 3 * k, bx + bw * osdValue, cy + 3 * k), 3 * k, 3 * k), b);
            wchar_t t[8]; swprintf_s(t, L"%d", (int)std::lround(osdValue * 100));
            Text(t, D2D1::RectF(Rr - 34 * k, p.top, Rr, p.bottom), fm.Get(), b, fg);
        } else if (TimerRunning()) {
            Icon(10, D2D1::RectF(L, cy - 10 * k, L + 20 * k, cy + 10 * k), b, volt);
            ULONGLONG left = (timerEnd - now) / 1000;
            wchar_t t[16]; swprintf_s(t, L"%llu:%02llu", left / 60, left % 60);
            Text(L"Focus", D2D1::RectF(L + 28 * k, p.top, Rr, p.bottom), fs.Get(), b, dim);
            Text(t, D2D1::RectF(L, p.top, Rr, p.bottom), fm.Get(), b, fg);
        } else if (use.mic || use.camera) {
            Circle(D2D1::Point2F(L + 5 * k, cy), 4 * k, b, use.camera ? Hex(0x3ddc84, a) : Hex(0xf5a23c, a));
            if (use.mic && use.camera) Circle(D2D1::Point2F(L + 16 * k, cy), 4 * k, b, Hex(0xf5a23c, a));
            ULONGLONG since = use.micSinceMs ? (now - use.micSinceMs) / 1000 : 0;
            Text(use.mic ? L"Mic on" : L"Camera on", D2D1::RectF(L + (use.mic && use.camera ? 28 : 16) * k, p.top, Rr, p.bottom), fs.Get(), b, fg);
            if (use.mic && since) { wchar_t t[16]; swprintf_s(t, L"%llu:%02llu", since / 60, since % 60); Text(t, D2D1::RectF(L, p.top, Rr, p.bottom), fm.Get(), b, dim); }
        } else if (mi.has && mi.playing) {
            D2D1_RECT_F ar = D2D1::RectF(L - 4 * k, cy - 11 * k, L + 18 * k, cy + 11 * k);
            if (art) dc->DrawBitmap(art.Get(), ar, a, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            else { b->SetColor(Hex(0x2b2e35, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(ar, 5 * k, 5 * k), b); }
            Text(mi.title, D2D1::RectF(L + 26 * k, p.top, Rr - 30 * k, p.bottom), fs.Get(), b, fg);
            // equalizer
            float t = now / 1000.0f;
            for (int i = 0; i < 4; ++i) {
                float hh = (0.35f + 0.65f * std::fabs(std::sin(t * (3.1f + i * 1.7f) + i))) * 14 * k;
                float x = Rr - 22 * k + i * 6 * k;
                b->SetColor(volt);
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, cy + 7 * k - hh, x + 3.2f * k, cy + 7 * k), 1.5f * k, 1.5f * k), b);
            }
        } else if (micMuted == 1) {
            Icon(5, D2D1::RectF(L, cy - 10 * k, L + 20 * k, cy + 10 * k), b, fg);
            Text(L"Mic muted", D2D1::RectF(L + 28 * k, p.top, Rr, p.bottom), fs.Get(), b, fg);
        } else if (ph > 20 * k) {
            // idle: a quiet camera-lens dot, like a notch
            Circle(D2D1::Point2F(p.right - 18 * k, cy), 4 * k, b, Hex(0x15171b, a));
            Circle(D2D1::Point2F(p.right - 18 * k, cy), 1.8f * k, b, Hex(0x2b3a5a, a));
        }
    }

    void Button(int id, int icon, D2D1_RECT_F r, ID2D1SolidColorBrush* b, float a, bool primary = false, bool on = false) {
        float rr = (r.bottom - r.top) / 2;
        b->SetColor(primary ? Hex(0xf3f4f6, a) : on ? Hex(0xd4f53c, a) : Hex(0x1d1f24, a));
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rr, rr), b);
        float pad = (r.bottom - r.top) * 0.26f;
        Icon(icon, D2D1::RectF(r.left + pad + ((r.right - r.left) - (r.bottom - r.top)) / 2, r.top + pad, r.right - pad - ((r.right - r.left) - (r.bottom - r.top)) / 2, r.bottom - pad),
            b, primary || on ? Hex(0x0b0d02, a) : Hex(0xf3f4f6, a));
        hits.push_back({ r, id });
    }
    void Slider(int id, int icon, const wchar_t* label, float v, D2D1_RECT_F r, ID2D1SolidColorBrush* b, float a) {
        Icon(icon, D2D1::RectF(r.left, r.top, r.left + 22 * k, r.bottom), b, Hex(0xb3b7bf, a));
        float bx = r.left + 32 * k, bw = r.right - 46 * k - bx, cy = (r.top + r.bottom) / 2;
        b->SetColor(Hex(0x1d1f24, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, cy - 4 * k, bx + bw, cy + 4 * k), 4 * k, 4 * k), b);
        if (v >= 0) {
            b->SetColor(Hex(0xd4f53c, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, cy - 4 * k, bx + bw * v, cy + 4 * k), 4 * k, 4 * k), b);
            Circle(D2D1::Point2F(bx + bw * v, cy), 7 * k, b, Hex(0xf3f4f6, a));
        }
        wchar_t t[8]; swprintf_s(t, v >= 0 ? L"%d" : L"-", (int)std::lround(v * 100));
        Text(t, D2D1::RectF(r.right - 40 * k, r.top, r.right, r.bottom), Font(11.5f, DWRITE_FONT_WEIGHT_REGULAR, true, DWRITE_TEXT_ALIGNMENT_TRAILING).Get(), b, Hex(0x8c919a, a));
        (void)label;
        hits.push_back({ D2D1::RectF(bx - 6 * k, r.top, bx + bw + 6 * k, r.bottom), id });
    }

    void DrawCard(D2D1_RECT_F p, ID2D1SolidColorBrush* b, float a) {
        D2D1_RECT_F c = D2D1::RectF((p.left + p.right) / 2 - CARD_W * k / 2, p.top, (p.left + p.right) / 2 + CARD_W * k / 2, p.top + CardH() * k);
        // tabs
        const wchar_t* names[kTabs] = { L"Now playing", L"Controls", L"Timer", L"Clipboard", L"Shelf" };
        const float widths[kTabs] = { 92, 72, 52, 76, 50 };
        auto ft = Font(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        float x = 16;
        for (int i = 0; i < kTabs; ++i) {
            float tw = widths[i];
            D2D1_RECT_F r = R(x, 10, tw, 26, c);
            if (tab == i || hoverId == H_TAB0 + i) { b->SetColor(Hex(tab == i ? 0x1d1f24 : 0x15171b, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, 13 * k, 13 * k), b); }
            Text(names[i], r, ft.Get(), b, tab == i ? Hex(0xf3f4f6, a) : Hex(0x8c919a, a));
            hits.push_back({ r, H_TAB0 + i });
            x += tw + 4;
        }
        if (tab == 3) { DrawClipboard(c, b, a); return; }
        if (tab == 4) { DrawShelf(c, b, a); return; }
        // right side of the tab row: mic state + time
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t clock[8]; swprintf_s(clock, L"%02d:%02d", st.wHour, st.wMinute);
        Text(clock, R(CARD_W - 76, 10, 60, 26, c), Font(12, DWRITE_FONT_WEIGHT_REGULAR, true, DWRITE_TEXT_ALIGNMENT_TRAILING).Get(), b, Hex(0x8c919a, a));
        if (use.mic || use.camera) Circle(D2D1::Point2F(c.left + (CARD_W - 92) * k, c.top + 23 * k), 3.5f * k, b, use.camera ? Hex(0x3ddc84, a) : Hex(0xf5a23c, a));

        auto fTitle = Font(16, DWRITE_FONT_WEIGHT_SEMI_BOLD), fSub = Font(13, DWRITE_FONT_WEIGHT_REGULAR);
        auto fMono = Font(11, DWRITE_FONT_WEIGHT_REGULAR, true);
        if (tab == 0) {
            if (!mi.has) {
                Text(L"Nothing playing", R(16, 70, CARD_W - 32, 26, c), Font(16, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0xf3f4f6, a));
                Text(L"Play something in Spotify, a browser or any media app", R(16, 98, CARD_W - 32, 22, c), Font(12.5f, DWRITE_FONT_WEIGHT_REGULAR, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0x8c919a, a));
                return;
            }
            D2D1_RECT_F ar = R(16, 48, 100, 100, c);
            if (art) dc->DrawBitmap(art.Get(), ar, a, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            else { b->SetColor(Hex(0x1d1f24, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(ar, 12 * k, 12 * k), b); Icon(0, R(50, 82, 32, 32, c), b, Hex(0x5d626b, a)); }
            Text(mi.title.empty() ? L"Unknown title" : mi.title, R(130, 50, CARD_W - 146, 24, c), fTitle.Get(), b, Hex(0xf3f4f6, a));
            Text(mi.artist, R(130, 74, CARD_W - 146, 20, c), fSub.Get(), b, Hex(0x8c919a, a));
            // progress
            float frac = mi.duration > 0 ? (float)std::clamp(mi.position / mi.duration, 0.0, 1.0) : 0;
            D2D1_RECT_F bar = R(130, 108, CARD_W - 146, 4, c);
            b->SetColor(Hex(0x1d1f24, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(bar, 2 * k, 2 * k), b);
            b->SetColor(Hex(0xd4f53c, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bar.left, bar.top, bar.left + (bar.right - bar.left) * frac, bar.bottom), 2 * k, 2 * k), b);
            hits.push_back({ D2D1::RectF(bar.left, bar.top - 6 * k, bar.right, bar.bottom + 6 * k), H_SEEK });
            auto tm = [](double sec) { wchar_t t[16]; int s2 = (int)sec; swprintf_s(t, L"%d:%02d", s2 / 60, s2 % 60); return std::wstring(t); };
            if (mi.duration > 0) {
                Text(tm(mi.position), R(130, 116, 60, 16, c), fMono.Get(), b, Hex(0x8c919a, a));
                Text(tm(mi.duration), R(CARD_W - 76, 116, 60, 16, c), Font(11, DWRITE_FONT_WEIGHT_REGULAR, true, DWRITE_TEXT_ALIGNMENT_TRAILING).Get(), b, Hex(0x8c919a, a));
            }
            float bx = 130 + (CARD_W - 146) / 2;
            Button(H_PREV, 3, R(bx - 74, 142, 40, 40, c), b, a);
            Button(H_PLAY, mi.playing ? 1 : 0, R(bx - 23, 138, 46, 46, c), b, a, true);
            Button(H_NEXT, 2, R(bx + 34, 142, 40, 40, c), b, a);
        } else if (tab == 1) {
            bool muted = micMuted == 1;
            Button(H_MIC, muted ? 5 : 4, R(16, 52, 44, 44, c), b, a, false, muted);
            Text(micMuted < 0 ? L"No mic" : muted ? L"Muted" : L"Mic on", R(10, 100, 56, 18, c), Font(11.5f, DWRITE_FONT_WEIGHT_REGULAR, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0xb3b7bf, a));
            Button(H_PICK, 8, R(76, 52, 44, 44, c), b, a);
            Text(L"Pick color", R(66, 100, 64, 18, c), Font(11.5f, DWRITE_FONT_WEIGHT_REGULAR, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0xb3b7bf, a));
            Button(H_SNIP, 9, R(136, 52, 44, 44, c), b, a);
            Text(L"Snip", R(126, 100, 64, 18, c), Font(11.5f, DWRITE_FONT_WEIGHT_REGULAR, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0xb3b7bf, a));
            Slider(H_VOL, 6, L"Volume", volume, R(204, 56, CARD_W - 220, 30, c), b, a);
            Slider(H_BRI, 7, L"Brightness", brightness >= 0 ? brightness / 100.0f : -1, R(204, 100, CARD_W - 220, 30, c), b, a);
            Text(L"Ctrl+Alt+M mutes the mic anywhere  ·  Ctrl+Alt+P picks a color", R(16, 150, CARD_W - 32, 22, c), Font(11.5f, DWRITE_FONT_WEIGHT_REGULAR).Get(), b, Hex(0x5d626b + 0x1b1b1b, a));
        } else {
            if (TimerRunning()) {
                ULONGLONG left = (timerEnd - GetTickCount64()) / 1000;
                wchar_t t[16]; swprintf_s(t, L"%llu:%02llu", left / 60, left % 60);
                Text(t, R(16, 52, CARD_W - 32, 56, c), Font(44, DWRITE_FONT_WEIGHT_MEDIUM, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0xf3f4f6, a));
                float frac = timerTotal ? 1.0f - (float)(timerEnd - GetTickCount64()) / timerTotal : 0;
                D2D1_RECT_F bar = R(60, 118, CARD_W - 120, 4, c);
                b->SetColor(Hex(0x1d1f24, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(bar, 2 * k, 2 * k), b);
                b->SetColor(Hex(0xd4f53c, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bar.left, bar.top, bar.left + (bar.right - bar.left) * frac, bar.bottom), 2 * k, 2 * k), b);
                D2D1_RECT_F stop = R(CARD_W / 2 - 46, 140, 92, 36, c);
                b->SetColor(Hex(0x1d1f24, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(stop, 18 * k, 18 * k), b);
                Text(L"Stop", stop, Font(13, DWRITE_FONT_WEIGHT_MEDIUM, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0xf3f4f6, a));
                hits.push_back({ stop, H_TSTOP });
            } else {
                Text(L"Focus timer", R(16, 50, CARD_W - 32, 24, c), Font(15, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0xf3f4f6, a));
                Text(L"The island counts down and lets you know when time is up", R(16, 74, CARD_W - 32, 20, c), Font(12, DWRITE_FONT_WEIGHT_REGULAR, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0x8c919a, a));
                int mins[4] = { 5, 15, 25, 45 };
                for (int i = 0; i < 4; ++i) {
                    D2D1_RECT_F r = R(CARD_W / 2 - 172 + i * 88, 112, 80, 52, c);
                    b->SetColor(i == 2 ? Hex(0xd4f53c, a) : Hex(0x1d1f24, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, 14 * k, 14 * k), b);
                    wchar_t t[8]; swprintf_s(t, L"%d", mins[i]);
                    Text(t, D2D1::RectF(r.left, r.top + 4 * k, r.right, r.bottom - 14 * k), Font(18, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, i == 2 ? Hex(0x0b0d02, a) : Hex(0xf3f4f6, a));
                    Text(L"min", D2D1::RectF(r.left, r.bottom - 22 * k, r.right, r.bottom - 4 * k), Font(11, DWRITE_FONT_WEIGHT_REGULAR, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, i == 2 ? Hex(0x0b0d02, a) : Hex(0x8c919a, a));
                    hits.push_back({ r, H_T5 + i });
                }
            }
        }
    }


    // ---------------------------------------------------------------- clipboard tab
    void SmallIcon(int which, D2D1_RECT_F r, ID2D1SolidColorBrush* b, D2D1_COLOR_F col, bool filled = false) {
        float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, u = (r.right - r.left) / 24.0f, sw = 1.7f * u;
        b->SetColor(col);
        auto line = [&](float x1, float y1, float x2, float y2) { dc->DrawLine(D2D1::Point2F(cx + x1 * u, cy + y1 * u), D2D1::Point2F(cx + x2 * u, cy + y2 * u), b, sw); };
        switch (which) {
        case 0: line(-6, -5, 6, -5); line(-6, 0, 6, 0); line(-6, 5, 2, 5); break;                         // text
        case 1: {                                                                                       // link
            D2D1_MATRIX_3X2_F old; dc->GetTransform(&old);
            dc->SetTransform(D2D1::Matrix3x2F::Rotation(-45, D2D1::Point2F(cx, cy)) * old);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 8.5f * u, cy - 3.2f * u, cx + 0.5f * u, cy + 3.2f * u), 3.2f * u, 3.2f * u), b, sw);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 0.5f * u, cy - 3.2f * u, cx + 8.5f * u, cy + 3.2f * u), 3.2f * u, 3.2f * u), b, sw);
            dc->SetTransform(old);
            break; }
        case 2: {                                                                                       // file
            ComPtr<ID2D1PathGeometry> g; d2d->CreatePathGeometry(&g); ComPtr<ID2D1GeometrySink> sk; g->Open(&sk);
            sk->BeginFigure(D2D1::Point2F(cx - 5 * u, cy - 7 * u), D2D1_FIGURE_BEGIN_HOLLOW);
            sk->AddLine(D2D1::Point2F(cx + 1.5f * u, cy - 7 * u)); sk->AddLine(D2D1::Point2F(cx + 5 * u, cy - 3.5f * u));
            sk->AddLine(D2D1::Point2F(cx + 5 * u, cy + 7 * u)); sk->AddLine(D2D1::Point2F(cx - 5 * u, cy + 7 * u));
            sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); dc->DrawGeometry(g.Get(), b, sw);
            line(1.5f, -7, 1.5f, -3.5f); line(1.5f, -3.5f, 5, -3.5f); break; }
        case 3: {                                                                                       // pin
            ComPtr<ID2D1PathGeometry> g; d2d->CreatePathGeometry(&g); ComPtr<ID2D1GeometrySink> sk; g->Open(&sk);
            sk->BeginFigure(D2D1::Point2F(cx - 3 * u, cy - 7 * u), filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
            sk->AddLine(D2D1::Point2F(cx + 3 * u, cy - 7 * u)); sk->AddLine(D2D1::Point2F(cx + 3 * u, cy - 2 * u));
            sk->AddLine(D2D1::Point2F(cx + 5.5f * u, cy + 1.5f * u)); sk->AddLine(D2D1::Point2F(cx - 5.5f * u, cy + 1.5f * u));
            sk->AddLine(D2D1::Point2F(cx - 3 * u, cy - 2 * u));
            sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close();
            if (filled) dc->FillGeometry(g.Get(), b); else dc->DrawGeometry(g.Get(), b, sw);
            line(0, 1.5f, 0, 8); break; }
        case 4: line(-5, -5, 5, 5); line(-5, 5, 5, -5); break;                                         // remove
        case 5: {                                                                                       // tray + arrow
            line(0, -8, 0, 2); line(-4, -2, 0, 2); line(4, -2, 0, 2);
            ComPtr<ID2D1PathGeometry> g; d2d->CreatePathGeometry(&g); ComPtr<ID2D1GeometrySink> sk; g->Open(&sk);
            sk->BeginFigure(D2D1::Point2F(cx - 9 * u, cy + 1 * u), D2D1_FIGURE_BEGIN_HOLLOW);
            sk->AddLine(D2D1::Point2F(cx - 9 * u, cy + 8 * u)); sk->AddLine(D2D1::Point2F(cx + 9 * u, cy + 8 * u));
            sk->AddLine(D2D1::Point2F(cx + 9 * u, cy + 1 * u));
            sk->EndFigure(D2D1_FIGURE_END_OPEN); sk->Close(); dc->DrawGeometry(g.Get(), b, sw); break; }
        case 6: {                                                                                       // image
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 7 * u, cy - 6 * u, cx + 7 * u, cy + 6 * u), 2 * u, 2 * u), b, sw);
            line(-7, 4, -2, -1); line(-2, -1, 2, 3); line(1, 2, 3, 0); line(3, 0, 7, 4);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 3 * u, cy - 2.5f * u), 1.4f * u, 1.4f * u), b); break; }
        }
    }

    bool ClipMatch(const ClipItem& it) const {
        switch (clipFilter) {
        case 1: return it.kind == "text";
        case 2: return it.kind == "link";
        case 3: return it.kind == "image";
        case 4: return it.kind == "files";
        case 5: return it.pinned;
        }
        return true;
    }

    ID2D1Bitmap* Thumb(const ClipItem& it) {
        if (it.thumb.empty()) return nullptr;
        if (auto f = thumbs.find(it.id); f != thumbs.end()) return f->second.Get();
        if (thumbs.size() > 48) thumbs.clear();
        std::vector<uint8_t> px; int tw = 0, th = 0;
        ComPtr<ID2D1Bitmap> bmp;
        if (ImageUtil::DecodePng(it.thumb, (int)(64 * k), px, tw, th))
            dc->CreateBitmap(D2D1::SizeU(tw, th), px.data(), tw * 4,
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bmp);
        thumbs[it.id] = bmp;
        return bmp.Get();
    }

    void Empty(D2D1_RECT_F c, const wchar_t* title, const wchar_t* sub, ID2D1SolidColorBrush* b, float a, float y = 110) {
        Text(title, R(16, y, CARD_W - 32, 24, c), Font(15, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0xf3f4f6, a));
        Text(sub, R(16, y + 26, CARD_W - 32, 20, c), Font(12.5f, DWRITE_FONT_WEIGHT_REGULAR, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0x8c919a, a));
    }

    void DrawClipboard(D2D1_RECT_F c, ID2D1SolidColorBrush* b, float a) {
        const wchar_t* chips[6] = { L"All", L"Text", L"Links", L"Images", L"Files", L"Pinned" };
        auto fc = Font(12, DWRITE_FONT_WEIGHT_MEDIUM, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        float x = 16;
        for (int i = 0; i < 6; ++i) {
            float cw = 24.0f + (float)wcslen(chips[i]) * 6.4f;
            D2D1_RECT_F r = R(x, 46, cw, 24, c);
            bool on = clipFilter == i;
            b->SetColor(on ? Hex(0xd4f53c, a) : Hex(hoverId == H_CHIP0 + i ? 0x24272d : 0x1a1c21, a));
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, 12 * k, 12 * k), b);
            Text(chips[i], r, fc.Get(), b, on ? Hex(0x0b0d02, a) : Hex(0xb3b7bf, a));
            hits.push_back({ r, H_CHIP0 + i });
            x += cw + 6;
        }
        clipIds.clear();
        if (!clip || (!clip->Enabled() && clip->Items().empty())) {
            Empty(c, L"Clipboard history is off", L"Turn it on in Ionization, under Clipboard", b, a);
            return;
        }
        std::vector<const ClipItem*> list;
        for (auto& it : clip->Items()) if (ClipMatch(it)) list.push_back(&it);
        wchar_t cnt[32]; swprintf_s(cnt, list.size() == 1 ? L"%zu item" : L"%zu items", list.size());
        Text(cnt, R(CARD_W - 116, 46, 100, 24, c), Font(11, DWRITE_FONT_WEIGHT_REGULAR, true, DWRITE_TEXT_ALIGNMENT_TRAILING).Get(), b, Hex(0x5d626b, a));
        if (list.empty()) {
            Empty(c, clipFilter == 5 ? L"Nothing pinned" : L"Nothing here yet",
                clipFilter == 5 ? L"Pin an item to keep it past the history limit" : L"Copy something and it shows up here", b, a, 140);
            return;
        }
        const float top = 80, bottom = CardH() - 8;
        float maxScroll = std::max(0.0f, list.size() * CLIP_ROW - (bottom - top));
        clipScroll = std::clamp(clipScroll, 0.0f, maxScroll);
        D2D1_RECT_F area = R(6, top, CARD_W - 12, bottom - top, c);
        dc->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        auto f1 = Font(13, DWRITE_FONT_WEIGHT_MEDIUM), f2 = Font(11, DWRITE_FONT_WEIGHT_REGULAR, true);
        for (size_t i = 0; i < list.size(); ++i) {
            const ClipItem& it = *list[i];
            clipIds.push_back(it.id);
            float y = top + i * CLIP_ROW - clipScroll;
            if (y + CLIP_ROW < top || y > bottom) continue;
            int base = H_CLIPROW + (int)i * 4;
            bool hov = hoverId >= base && hoverId < base + 4;
            D2D1_RECT_F row = R(10, y + 2, CARD_W - 20, CLIP_ROW - 4, c);
            if (hov) { b->SetColor(Hex(0x15171b, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(row, 10 * k, 10 * k), b); }
            D2D1_RECT_F ib = R(18, y + 7, 32, 32, c);
            ID2D1Bitmap* th = it.kind == "image" ? Thumb(it) : nullptr;
            b->SetColor(Hex(0x1d1f24, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(ib, 8 * k, 8 * k), b);
            if (th) {
                D2D1_SIZE_F sz = th->GetSize();
                float s2 = std::min((ib.right - ib.left) / sz.width, (ib.bottom - ib.top) / sz.height);
                float dw2 = sz.width * s2, dh2 = sz.height * s2, cx = (ib.left + ib.right) / 2, cy = (ib.top + ib.bottom) / 2;
                dc->DrawBitmap(th, D2D1::RectF(cx - dw2 / 2, cy - dh2 / 2, cx + dw2 / 2, cy + dh2 / 2), a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                int ic = it.kind == "link" ? 1 : it.kind == "files" ? 2 : it.kind == "image" ? 6 : 0;
                SmallIcon(ic, D2D1::RectF(ib.left + 6 * k, ib.top + 6 * k, ib.right - 6 * k, ib.bottom - 6 * k), b, it.kind == "link" ? Hex(0xd4f53c, a) : Hex(0xb3b7bf, a));
            }
            std::wstring l1, l2;
            if (it.kind == "files") {
                const wchar_t* nm = it.files.empty() ? L"" : PathFindFileNameW(it.files[0].c_str());
                l1 = nm;
                if (it.files.size() > 1) l1 += L"  +" + std::to_wstring(it.files.size() - 1) + L" more";
                l2 = (it.files.size() == 1 ? L"File" : std::to_wstring(it.files.size()) + L" files");
            } else if (it.kind == "image") {
                l1 = L"Image";
                l2 = std::to_wstring(it.w) + L" x " + std::to_wstring(it.h);
            } else {
                l1.reserve(140);
                bool sp = false;
                for (wchar_t ch : it.text) {
                    if (l1.size() >= 140) break;
                    if (ch == L'\r' || ch == L'\n' || ch == L'\t' || ch == L' ') { if (!l1.empty()) sp = true; continue; }
                    if (sp) { l1 += L' '; sp = false; }
                    l1 += ch;
                }
                if (it.kind == "link") l2 = L"Link";
                else { wchar_t t[48]; swprintf_s(t, L"Text, %zu chars", it.text.size()); l2 = t; }
            }
            l2 += L"  \u00b7  " + Ago(it.time);
            float textR = CARD_W - (hov || it.pinned ? 92.0f : 28.0f);
            Text(l1, R(60, y + 5, textR - 60, 20, c), f1.Get(), b, Hex(0xf3f4f6, a));
            Text(l2, R(60, y + 24, textR - 60, 16, c), f2.Get(), b, Hex(0x8c919a, a));
            D2D1_RECT_F pinR = R(CARD_W - 84, y + 9, 28, 28, c), delR = R(CARD_W - 52, y + 9, 28, 28, c);
            if (hov || it.pinned) {
                if (hoverId == base + 1) { b->SetColor(Hex(0x24272d, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(pinR, 8 * k, 8 * k), b); }
                SmallIcon(3, D2D1::RectF(pinR.left + 5 * k, pinR.top + 5 * k, pinR.right - 5 * k, pinR.bottom - 5 * k), b, it.pinned ? Hex(0xd4f53c, a) : Hex(0x8c919a, a), it.pinned);
            }
            if (hov) {
                if (hoverId == base + 2) { b->SetColor(Hex(0x24272d, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(delR, 8 * k, 8 * k), b); }
                SmallIcon(4, D2D1::RectF(delR.left + 7 * k, delR.top + 7 * k, delR.right - 7 * k, delR.bottom - 7 * k), b, Hex(0x8c919a, a));
            }
            if (row.bottom > area.top && row.top < area.bottom) {
                hits.push_back({ pinR, base + 1 });
                hits.push_back({ delR, base + 2 });
                hits.push_back({ D2D1::RectF(row.left, std::max(row.top, area.top), row.right, std::min(row.bottom, area.bottom)), base });
            }
        }
        dc->PopAxisAlignedClip();
        if (maxScroll > 0) {
            float trackH = bottom - top, thumbH = std::max(24.0f, trackH * trackH / (list.size() * CLIP_ROW));
            float ty = top + (trackH - thumbH) * (clipScroll / maxScroll);
            b->SetColor(Hex(0x2b2e35, a));
            dc->FillRoundedRectangle(D2D1::RoundedRect(R(CARD_W - 9, ty, 3, thumbH, c), 1.5f * k, 1.5f * k), b);
        }
    }

    // ---------------------------------------------------------------- shelf tab
    void LoadShelf() {
        shelf.clear();
        std::string raw;
        if (!ReadFileBytes(LocalDataDir() + L"\\shelf.json", raw)) return;
        Json arr = Json::parse(raw);
        if (arr.isArr()) for (auto& e : arr.arr) if (e.isStr()) {
            std::wstring pth = Widen(e.s);
            if (GetFileAttributesW(pth.c_str()) != INVALID_FILE_ATTRIBUTES) shelf.push_back(pth);
        }
    }
    void SaveShelf() {
        Json arr = Json::array();
        for (auto& f : shelf) arr.arr.push_back(Json(Narrow(f)));
        WriteFileBytes(LocalDataDir() + L"\\shelf.json", arr.dump());
    }
    void PruneShelf() {
        size_t n = shelf.size();
        shelf.erase(std::remove_if(shelf.begin(), shelf.end(), [](const std::wstring& f) { return GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES; }), shelf.end());
        if (shelf.size() != n) SaveShelf();
    }
    void AddShelf(const std::vector<std::wstring>& files) {
        int added = 0;
        for (auto& f : files) {
            if (std::find(shelf.begin(), shelf.end(), f) != shelf.end()) continue;
            if (shelf.size() >= 60) break;
            shelf.push_back(f); ++added;
        }
        if (added) SaveShelf();
        LogLine("Island shelf: %d added, %zu total", added, shelf.size());
    }
    ID2D1Bitmap* ShelfIcon(const std::wstring& path) {
        if (auto f = icons.find(path); f != icons.end()) return f->second.Get();
        ComPtr<ID2D1Bitmap> bmp;
        if (HBITMAP hb = ShellUtil::Icon(path, (int)(48 * k))) {
            ComPtr<IWICBitmap> wb;
            if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapUsePremultipliedAlpha, &wb))) {
                ComPtr<IWICFormatConverter> conv; wic->CreateFormatConverter(&conv);
                if (conv && SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
                    dc->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
            }
            DeleteObject(hb);
        }
        icons[path] = bmp;
        return bmp.Get();
    }
    void DrawShelf(D2D1_RECT_F c, ID2D1SolidColorBrush* b, float a) {
        if (!shelf.empty()) {
            D2D1_RECT_F cr = R(CARD_W - 72, 11, 56, 24, c);
            b->SetColor(Hex(hoverId == H_SHELFCLEAR ? 0x24272d : 0x1a1c21, a));
            dc->FillRoundedRectangle(D2D1::RoundedRect(cr, 12 * k, 12 * k), b);
            Text(L"Clear", cr, Font(12, DWRITE_FONT_WEIGHT_MEDIUM, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0xb3b7bf, a));
            hits.push_back({ cr, H_SHELFCLEAR });
        }
        float H = CardH();
        if (shelf.empty() || dropHover) {
            D2D1_RECT_F z = R(16, 48, CARD_W - 32, H - 64, c);
            if (!shelf.empty()) z = R(16, 48, CARD_W - 32, H - 64, c);
            ComPtr<ID2D1StrokeStyle> dash;
            float dashes[] = { 4, 3 };
            d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_CUSTOM, 0), dashes, 2, &dash);
            if (dropHover) { b->SetColor(Hex(0xd4f53c, a * 0.07f)); dc->FillRoundedRectangle(D2D1::RoundedRect(z, 16 * k, 16 * k), b); }
            b->SetColor(dropHover ? Hex(0xd4f53c, a) : Hex(0x3a3e46, a));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(z, 16 * k, 16 * k), b, 1.4f * k, dash.Get());
            float cy = (z.top + z.bottom) / 2;
            SmallIcon(5, D2D1::RectF((z.left + z.right) / 2 - 16 * k, cy - 44 * k, (z.left + z.right) / 2 + 16 * k, cy - 12 * k), b, dropHover ? Hex(0xd4f53c, a) : Hex(0xb3b7bf, a));
            Text(dropHover ? L"Release to park them here" : L"Drop files here to keep them handy",
                D2D1::RectF(z.left, cy - 6 * k, z.right, cy + 16 * k), Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0xf3f4f6, a));
            Text(L"Drag them back out into any app. The files stay where they are.",
                D2D1::RectF(z.left, cy + 18 * k, z.right, cy + 38 * k), Font(12, DWRITE_FONT_WEIGHT_REGULAR, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0x8c919a, a));
            return;
        }
        const int perRow = 5;
        const float tw = 84, th = 88;
        int rows = (int)((shelf.size() + perRow - 1) / perRow);
        shelfScroll = std::clamp(shelfScroll, 0, std::max(0, rows - 2));
        float x0 = (CARD_W - perRow * tw) / 2;
        auto fn = Font(11.5f, DWRITE_FONT_WEIGHT_REGULAR, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        for (size_t i = (size_t)shelfScroll * perRow; i < shelf.size() && i < (size_t)(shelfScroll + 2) * perRow; ++i) {
            int col = (int)(i % perRow), row = (int)(i / perRow) - shelfScroll;
            D2D1_RECT_F t = R(x0 + col * tw + 3, 48 + row * th, tw - 6, th - 6, c);
            int base = H_SHELF + (int)i * 2;
            bool hov = hoverId == base || hoverId == base + 1;
            if (hov) { b->SetColor(Hex(0x15171b, a)); dc->FillRoundedRectangle(D2D1::RoundedRect(t, 12 * k, 12 * k), b); }
            float icx = (t.left + t.right) / 2;
            D2D1_RECT_F ir = D2D1::RectF(icx - 20 * k, t.top + 8 * k, icx + 20 * k, t.top + 48 * k);
            if (ID2D1Bitmap* ic = ShelfIcon(shelf[i])) dc->DrawBitmap(ic, ir, a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            else SmallIcon(2, ir, b, Hex(0xb3b7bf, a));
            Text(PathFindFileNameW(shelf[i].c_str()), D2D1::RectF(t.left + 4 * k, t.top + 54 * k, t.right - 4 * k, t.top + 72 * k), fn.Get(), b, Hex(0xd6d8dc, a));
            D2D1_RECT_F xr = D2D1::RectF(t.right - 22 * k, t.top + 4 * k, t.right - 4 * k, t.top + 22 * k);
            if (hov) {
                Circle(D2D1::Point2F((xr.left + xr.right) / 2, (xr.top + xr.bottom) / 2), 9 * k, b, Hex(hoverId == base + 1 ? 0x3a3e46 : 0x24272d, a));
                SmallIcon(4, D2D1::RectF(xr.left + 4 * k, xr.top + 4 * k, xr.right - 4 * k, xr.bottom - 4 * k), b, Hex(0xd6d8dc, a));
            }
            hits.push_back({ xr, base + 1 });
            hits.push_back({ t, base });
        }
        if (rows > 2) {
            wchar_t more[32]; swprintf_s(more, L"%d of %d rows", shelfScroll + 1, rows - 1);
            Text(more, R(16, H - 22, CARD_W - 32, 14, c), Font(10.5f, DWRITE_FONT_WEIGHT_REGULAR, true, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0x5d626b, a));
        }
    }

    void DragOut(int i) {
        if (i < 0 || i >= (int)shelf.size()) return;
        ComPtr<IShellItem> item;
        if (FAILED(SHCreateItemFromParsingName(shelf[i].c_str(), nullptr, IID_PPV_ARGS(&item)))) { PruneShelf(); Render(); return; }
        ComPtr<IDataObject> obj;
        if (FAILED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&obj)))) return;
        DWORD eff = 0;
        dragOut = true;
        SHDoDragDrop(hwnd, obj.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &eff);
        dragOut = false;
        pressId = 0;
        PruneShelf();      // a move takes it off the shelf
        Render();
    }

    void OnDrag(bool on) {
        dropHover = on;
        if (on) { tab = 4; pinnedOpen = false; hoverSince = 0; if (!expanded) Expand(true); else UpdateTargets(); }
        Render();
    }

    // ---------------------------------------------------------------- behaviour
    void SetClickThrough(bool on) {
        if (on == clickThrough) return;
        clickThrough = on;
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, on ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT));
    }

    void Expand(bool on) {
        if (on == expanded) return;
        expanded = on;
        if (on) { volume = Audio::Volume(); micMuted = Audio::MicMuted(); if (tab == 4) PruneShelf(); }
        else { hoverId = 0; dropHover = false; }
        UpdateTargets();
    }

    void Poll() {
        if (!s.enabled || inGame || picker.catcher) return;
        POINT pt; GetCursorPos(&pt);
        RECT wr; GetWindowRect(hwnd, &wr);
        POINT c{ pt.x - wr.left, pt.y - wr.top };
        D2D1_RECT_F p = PillRect();
        ULONGLONG now = GetTickCount64();
        float m = 8 * k;
        bool over = c.x >= p.left - m && c.x <= p.right + m && c.y >= 0 && c.y <= p.bottom + m;
        // Hidden mode / tiny pill: touching the top edge near the middle also opens it.
        bool edge = pt.y <= mon.top + 1 && std::abs(pt.x - (mon.left + mon.right) / 2) < (LONG)(160 * k);
        bool button = (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
        if (button && !prevButton) pressOnPill = over || edge;   // a click meant for the app under the pill
        prevButton = button;
        if (!expanded) {
            if (button && !pressOnPill && (over || edge)) {
                // Mouse held over the island: maybe a file drag. Let OLE see the window; DragEnter opens the shelf.
                SetClickThrough(false);
                hoverSince = 0;
            } else {
                if (!button && ((over && p.right - p.left > 4) || edge)) { if (!hoverSince) hoverSince = now; if (now - hoverSince >= 140) Expand(true); }
                else hoverSince = 0;
                SetClickThrough(true);
            }
        } else {
            SetClickThrough(!over);
            if (!over && hoverId) { hoverId = 0; Render(); }
            if (pinnedOpen && over) pinnedOpen = false;                   // from here on, normal hover rules
            if (pinnedOpen && now - pinnedSince > 8000) pinnedOpen = false;
            if (over || dragging || pinnedOpen || dropHover || dragOut) leaveSince = 0;
            else { if (!leaveSince) leaveSince = now; if (now - leaveSince >= 380) { Expand(false); hoverSince = 0; } }
        }
        // keep the equalizer / progress / timers moving
        bool lively = (!expanded && ((mi.has && mi.playing) || TimerRunning() || use.mic)) || (expanded && (tab == 0 || tab == 2));
        if (lively && now - lastEq >= (expanded ? 250u : 70u)) { lastEq = now; SetTimer(hwnd, T_ANIM, 16, nullptr); }
        if (timerEnd && now >= timerEnd) {
            timerEnd = 0;
            Message(L"Focus session done", 0, 8000);
            MessageBeep(MB_ICONASTERISK);
        }
        if (msgUntil && now >= msgUntil) { msgUntil = 0; UpdateTargets(); }
        if (osdUntil && now >= osdUntil) { osdUntil = 0; UpdateTargets(); }
    }

    void Animate() {
        bool a1 = w.Step(), a2 = h.Step();
        Render();
        if (!a1 && !a2) KillTimer(hwnd, T_ANIM);
    }

    void UpdateArt() {
        if (mi.artVersion == artVersion) return;
        artVersion = mi.artVersion;
        art.Reset();
        if (mi.art.empty()) return;
        ComPtr<IStream> st; st.Attach(SHCreateMemStream(mi.art.data(), (UINT)mi.art.size()));
        ComPtr<IWICBitmapDecoder> dec;
        if (!st || FAILED(wic->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec))) return;
        ComPtr<IWICBitmapFrameDecode> fr; dec->GetFrame(0, &fr);
        ComPtr<IWICFormatConverter> conv; wic->CreateFormatConverter(&conv);
        if (fr && SUCCEEDED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            dc->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &art);
    }

    void SetSlider(int id, POINT c, bool final = false) {
        D2D1_RECT_F r{};
        bool found = false;
        for (auto& ht : hits) if (ht.id == id) { r = ht.r; found = true; break; }
        if (!found) return;
        float bx = r.left + 6 * k, bw = r.right - r.left - 12 * k;
        float v = std::clamp((c.x - bx) / bw, 0.0f, 1.0f);
        if (id == H_VOL) { volume = v; Audio::SetVolume(v); }
        else if (id == H_BRI) {
            brightness = (int)std::lround(v * 100);
            // WMI is slow: apply at most ~6 times a second while dragging, and once more on release.
            ULONGLONG now = GetTickCount64();
            if (final || now - lastBriSet > 160) { Brightness::Set(brightness); lastBriSet = now; briPending = false; }
            else briPending = true;
        }
        Render();
    }

    int HitAt(POINT c) {
        for (auto& ht : hits)
            if (c.x >= ht.r.left && c.x <= ht.r.right && c.y >= ht.r.top && c.y <= ht.r.bottom) return ht.id;
        return 0;
    }

    void Click(POINT c) {
        int hitId = 0;
        for (auto& ht : hits)
            if (c.x >= ht.r.left && c.x <= ht.r.right && c.y >= ht.r.top && c.y <= ht.r.bottom) { hitId = ht.id; break; }
        if (!hitId) return;
        {
            if (hitId >= H_TAB0 && hitId < H_TAB0 + kTabs) {
                tab = hitId - H_TAB0;
                if (tab == 1) { brightness = Brightness::Get(); volume = Audio::Volume(); micMuted = Audio::MicMuted(); }
                if (tab == 4) PruneShelf();
                clipScroll = 0;
            } else if (hitId >= H_CHIP0 && hitId < H_CHIP0 + 6) {
                clipFilter = hitId - H_CHIP0; clipScroll = 0;
            } else if (hitId == H_SHELFCLEAR) {
                shelf.clear(); icons.clear(); SaveShelf();
            } else if (hitId >= H_SHELF) {
                int i = (hitId - H_SHELF) / 2;
                if ((hitId - H_SHELF) % 2 == 1 && i < (int)shelf.size()) { icons.erase(shelf[i]); shelf.erase(shelf.begin() + i); SaveShelf(); }
            } else if (hitId >= H_CLIPROW && clip) {
                size_t row = (size_t)(hitId - H_CLIPROW) / 4;
                int part = (hitId - H_CLIPROW) % 4;
                if (row < clipIds.size()) {
                    uint64_t id = clipIds[row];
                    if (part == 1) clip->TogglePin(id);
                    else if (part == 2) clip->Remove(id);
                    else if (clip->CopyBack(id)) {
                        pinnedOpen = false;
                        Expand(false);
                        Message(L"Copied. Paste with Ctrl+V", 0, 1800);
                    }
                }
            }
            switch (hitId) {
            case H_PREV: media.Command(3); break;
            case H_PLAY: media.Command(1); mi.playing = !mi.playing; break;
            case H_NEXT: media.Command(2); break;
            case H_MIC: Audio::SetMicMuted(micMuted != 1); micMuted = Audio::MicMuted(); break;
            case H_PICK: Expand(false); picker.Start(); break;
            case H_SNIP: Expand(false); ShellUtil::SendKeys({ VK_LWIN, VK_SHIFT, 'S' }); break;
            case H_T5: case H_T15: case H_T25: case H_T45: {
                int mins[4] = { 5, 15, 25, 45 };
                timerTotal = (ULONGLONG)mins[hitId - H_T5] * 60000;
                timerEnd = GetTickCount64() + timerTotal;
                break;
            }
            case H_TSTOP: timerEnd = 0; break;
            }
            UpdateTargets();
            Render();
            return;
        }
    }

    LRESULT Handle(HWND hw, UINT m, WPARAM wp, LPARAM lp) {
        switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONDOWN: {
            POINT c{ (short)LOWORD(lp), (short)HIWORD(lp) };
            for (auto& ht : hits) if ((ht.id == H_VOL || ht.id == H_BRI) && c.x >= ht.r.left && c.x <= ht.r.right && c.y >= ht.r.top && c.y <= ht.r.bottom) {
                dragging = true; dragId = ht.id; SetCapture(hw); SetSlider(dragId, c); return 0;
            }
            int id = HitAt(c);
            pressId = (id >= H_SHELF && (id - H_SHELF) % 2 == 0) ? id : 0;
            pressPt = c;
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            int id = HitAt(POINT{ (short)LOWORD(lp), (short)HIWORD(lp) });
            if (id >= H_SHELF && (id - H_SHELF) % 2 == 0) {
                size_t i = (size_t)(id - H_SHELF) / 2;
                if (i < shelf.size()) ShellExecuteW(nullptr, L"open", shelf[i].c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT c{ (short)LOWORD(lp), (short)HIWORD(lp) };
            if (dragging) { SetSlider(dragId, c); return 0; }
            if (pressId && (wp & MK_LBUTTON) &&
                (std::abs(c.x - pressPt.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(c.y - pressPt.y) > GetSystemMetrics(SM_CYDRAG))) {
                int i = (pressId - H_SHELF) / 2;
                pressId = 0;
                DragOut(i);
                return 0;
            }
            int id = HitAt(c);
            if (id != hoverId) { hoverId = id; Render(); }
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hw, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (hoverId) { hoverId = 0; Render(); }
            return 0;
        case WM_MOUSEWHEEL: {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            if (tab == 3) { clipScroll -= d / 120.0f * CLIP_ROW * 1.5f; Render(); }
            else if (tab == 4) { shelfScroll += d < 0 ? 1 : -1; Render(); }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT c{ (short)LOWORD(lp), (short)HIWORD(lp) };
            if (dragging) {
                dragging = false; ReleaseCapture();
                if (dragId == H_BRI && briPending) SetSlider(H_BRI, c, true);
                return 0;
            }
            pressId = 0;
            Click(c);
            return 0;
        }
        case WM_TIMER:
            if (wp == T_POLL) Poll();
            else if (wp == T_ANIM) Animate();
            else if (wp == T_SLOW && !inGame) {
                Privacy::InUse u = Privacy::Poll();
                int mm = Audio::MicMuted();
                if (u.mic != use.mic || u.camera != use.camera || mm != micMuted) { use = u; micMuted = mm; UpdateTargets(); }
            }
            return 0;
        case WM_MEDIA_MSG: {
            {
                MediaInfo n = media.Get();
                if (n.title != mi.title || n.playing != mi.playing || n.has != mi.has)
                    LogLine("Island media: %s, %s - %s (%s)", n.has ? (n.playing ? "playing" : "paused") : "none",
                        Narrow(n.title).c_str(), Narrow(n.artist).c_str(), Narrow(n.app).c_str());
                mi = std::move(n);
            }
            UpdateArt();
            UpdateTargets();
            return 0;
        }
        case WM_VOLUME_MSG:
            volume = wp / 1000.0f;
            if (s.volumeOsd && !expanded) { osdValue = lp ? 0.0f : volume; osdUntil = GetTickCount64() + 1600; UpdateTargets(); }
            else Render();
            return 0;
        }
        return DefWindowProcW(hw, m, wp, lp);
    }
    static const UINT WM_MEDIA_MSG = WM_MEDIA, WM_VOLUME_MSG = WM_VOLUME;
};

// OLE drop target: files dragged onto the island go on the shelf (as references; nothing is copied or moved).
struct ShelfDrop : IDropTarget {
    std::atomic<ULONG> refs{ 1 };
    Island::Impl* I;
    ComPtr<IDropTargetHelper> helper;
    bool ok = false;
    explicit ShelfDrop(Island::Impl* i) : I(i) { CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper)); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == IID_IDropTarget) { *out = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --refs; if (!r) delete this; return r; }
    // Never MOVE: the source would delete the file, and the shelf only holds a reference.
    DWORD Effect(DWORD allowed) const {
        if (!ok) return DROPEFFECT_NONE;
        if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
        if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
        return DROPEFFECT_NONE;
    }
    static std::vector<std::wstring> Files(IDataObject* obj) {
        std::vector<std::wstring> out;
        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM sm{};
        if (SUCCEEDED(obj->GetData(&fe, &sm))) {
            if (auto d = (HDROP)GlobalLock(sm.hGlobal)) {
                UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n && i < 200; ++i) { wchar_t pth[MAX_PATH * 4]; if (DragQueryFileW(d, i, pth, ARRAYSIZE(pth))) out.push_back(pth); }
                GlobalUnlock(sm.hGlobal);
            }
            ReleaseStgMedium(&sm);
        }
        return out;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* obj, DWORD, POINTL pt, DWORD* eff) override {
        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        ok = obj->QueryGetData(&fe) == S_OK;
        *eff = Effect(*eff);
        POINT p{ pt.x, pt.y };
        if (helper) helper->DragEnter(I->hwnd, obj, &p, *eff);
        if (ok && !I->dragOut) I->OnDrag(true);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* eff) override {
        *eff = I->dragOut ? DROPEFFECT_NONE : Effect(*eff);
        POINT p{ pt.x, pt.y };
        if (helper) helper->DragOver(&p, *eff);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (helper) helper->DragLeave();
        if (I->dropHover) I->OnDrag(false);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* obj, DWORD, POINTL pt, DWORD* eff) override {
        *eff = I->dragOut ? DROPEFFECT_NONE : Effect(*eff);
        POINT p{ pt.x, pt.y };
        if (helper) helper->Drop(obj, &p, *eff);
        if (ok && !I->dragOut) I->AddShelf(Files(obj));
        I->OnDrag(false);
        return S_OK;
    }
};

void Island::Impl::RegisterDrop() {
    if (!oleInit) oleInit = SUCCEEDED(OleInitialize(nullptr));
    auto* t = new ShelfDrop(this);
    HRESULT hr = RegisterDragDrop(hwnd, t);
    t->Release();   // the system holds its own reference
    if (FAILED(hr)) LogLine("Island: RegisterDragDrop failed 0x%08X", (unsigned)hr);
}

Island::~Island() { if (impl_) { impl_->Destroy(); delete impl_; } }

void Island::Apply(const IslandSettings& s, bool inGame) {
    if (!impl_) impl_ = new Impl();
    Impl& I = *impl_;
    I.s = s;
    I.inGame = inGame;
    I.noExclude = noExclude;
    if (!s.enabled) { I.Destroy(); return; }
    if (!I.EnsureDevice()) return;
    I.Create();
    I.Layout();
    if (inGame) ShowWindow(I.hwnd, SW_HIDE);
    else { ShowWindow(I.hwnd, SW_SHOWNOACTIVATE); I.Render(); }
}

void Island::ShowMessage(const std::wstring& text, unsigned color, int ms) {
    if (impl_ && impl_->hwnd) impl_->Message(text, color, ms);
}
void Island::ToggleCard() {
    if (!impl_ || !impl_->hwnd) return;
    impl_->pinnedOpen = !impl_->expanded;
    impl_->pinnedSince = GetTickCount64();
    impl_->Expand(!impl_->expanded);
    if (!impl_->expanded) impl_->pinnedOpen = false;
}
void Island::ToggleMic() {
    int m = Audio::MicMuted();
    if (m < 0) { ShowMessage(L"No microphone found"); return; }
    Audio::SetMicMuted(m != 1);
    if (impl_) impl_->micMuted = Audio::MicMuted();
    ShowMessage(m != 1 ? L"Microphone muted" : L"Microphone on", 0, 2200);
}
void Island::OpenCard(int tab) {
    if (!impl_ || !impl_->hwnd) return;
    impl_->tab = std::clamp(tab, 0, kTabs - 1);
    if (impl_->tab == 1) { impl_->brightness = Brightness::Get(); impl_->volume = Audio::Volume(); }
    if (impl_->tab == 4) impl_->PruneShelf();
    impl_->clipScroll = 0;
    impl_->pinnedOpen = true;
    impl_->pinnedSince = GetTickCount64();
    if (impl_->expanded) { impl_->UpdateTargets(); impl_->Render(); }
    impl_->Expand(true);
}
void Island::PickColor() { if (impl_ && impl_->hwnd) impl_->picker.Start(); }
void Island::Reassert() {
    if (impl_ && impl_->hwnd) SetWindowPos(impl_->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}
HWND Island::Hwnd() const { return impl_ ? impl_->hwnd : nullptr; }
void Island::SetClipboard(ClipboardHistory* c) {
    if (!impl_) impl_ = new Impl();
    impl_->clip = c;
}
void Island::ClipboardChanged() {
    if (impl_ && impl_->hwnd && impl_->expanded && impl_->tab == 3) impl_->Render();
}
