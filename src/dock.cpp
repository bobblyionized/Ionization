#include "dock.h"
#include "common.h"
#include "shellutil.h"
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <map>

using Microsoft::WRL::ComPtr;

// Defined in clock.cpp.
ComPtr<IDWriteFontCollection1> LoadEmbeddedFonts(IDWriteFactory5* f);

// ======================================================================== taskbar
namespace {
bool g_tbHidden = false, g_tbTemp = false;
UINT g_tbOrig = ABS_ALWAYSONTOP;

void ForEachTray(const std::function<void(HWND)>& fn) {
    if (HWND t = FindWindowW(L"Shell_TrayWnd", nullptr)) fn(t);
    HWND s = nullptr;
    while ((s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) fn(s);
}
} // namespace

namespace Taskbar {
void Hide() {
    if (!g_tbHidden) {
        APPBARDATA abd{ sizeof(abd) };
        g_tbOrig = (UINT)SHAppBarMessage(ABM_GETSTATE, &abd);
        abd.lParam = ABS_AUTOHIDE;          // gives apps the full screen height
        SHAppBarMessage(ABM_SETSTATE, &abd);
        g_tbHidden = true;
        LogLine("Taskbar hidden (original state %u)", g_tbOrig);
    }
    if (!g_tbTemp) ForEachTray([](HWND h) { if (IsWindowVisible(h)) ShowWindow(h, SW_HIDE); });
}
void Restore() {
    if (!g_tbHidden) return;
    ForEachTray([](HWND h) { ShowWindow(h, SW_SHOWNA); });
    APPBARDATA abd{ sizeof(abd) };
    abd.lParam = g_tbOrig;
    SHAppBarMessage(ABM_SETSTATE, &abd);
    g_tbHidden = false;
    g_tbTemp = false;
    LogLine("Taskbar restored (state %u)", g_tbOrig);
}
bool IsHidden() { return g_tbHidden; }
UINT OriginalState() { return g_tbOrig; }
void SetRecovered(bool hidden, UINT original) { g_tbHidden = hidden; g_tbOrig = original; }
void ShowTemporarily(bool show) {
    g_tbTemp = show;
    if (!g_tbHidden) return;
    ForEachTray([show](HWND h) { ShowWindow(h, show ? SW_SHOWNA : SW_HIDE); });
}
} // namespace Taskbar

// ======================================================================== dock
namespace {

D2D1_COLOR_F Hex(UINT rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

enum ItemType { ItStart, ItApp, ItSep, ItClock, ItBattery, ItQuick, ItTaskbar };
enum { T_POLL = 1, T_REFRESH = 2, T_ANIM = 3 };

struct App {
    std::wstring key, launch, name, iconSource;
    std::vector<HWND> windows;
    bool pinned = false;
};

struct Item {
    ItemType type;
    int app = -1;
    float baseW = 0, baseCx = 0;   // unmagnified layout
    float scale = 1, x = 0, w = 0; // current
};

} // namespace

struct Dock::Impl {
    Dock* owner;
    DockSettings s;
    bool inGame = false;
    HWND hwnd = nullptr;
    UINT shellMsg = 0;

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
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> icons;

    std::vector<App> apps;
    std::vector<Item> items;
    int winW = 0, winH = 0;
    float k = 1, S = 48, pad = 10, bodyH = 0;
    RECT mon{}, work{};

    // state
    bool visible = false, menuOpen = false, clickThrough = false;
    float slide = 0, slideTarget = 0, hover = 0, hoverTarget = 0, cursorX = -1;
    int hot = -1;
    ULONGLONG edgeSince = 0, leaveSince = 0, lastTick = 0;
    std::wstring lastClock;
    HWND lastFg = nullptr;

    // ---------------------------------------------------------------- setup
    bool EnsureDevice() {
        if (dev) return true;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, &dev, nullptr, nullptr))) return false;
        ComPtr<IDXGIDevice> dxgi; dev.As(&dxgi);
        if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp)))) return false;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, (void**)d2d.GetAddressOf()))) return false;
        if (FAILED(d2d->CreateDevice(dxgi.Get(), &d2dDev))) return false;
        d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), (IUnknown**)dw.GetAddressOf());
        fonts = LoadEmbeddedFonts(dw.Get());
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        return dc && dw && wic;
    }

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* self = (Impl*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (self) return self->Handle(h, m, w, l);
        return DefWindowProcW(h, m, w, l);
    }

    void CreateWindowOnce() {
        if (hwnd) return;
        static bool reg = false;
        if (!reg) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"IonizationDock";
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&wc);
            reg = true;
        }
        hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP,
            L"IonizationDock", L"Ionization Dock", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        dcomp->CreateTargetForHwnd(hwnd, TRUE, &target);
        dcomp->CreateVisual(&visual);
        target->SetRoot(visual.Get());
        shellMsg = RegisterWindowMessageW(L"SHELLHOOK");
        RegisterShellHookWindow(hwnd);
        SetTimer(hwnd, T_POLL, 50, nullptr);
        SetTimer(hwnd, T_REFRESH, 2000, nullptr);
    }

    void Destroy() {
        if (hwnd) { DeregisterShellHookWindow(hwnd); DestroyWindow(hwnd); hwnd = nullptr; }
        bitmap.Reset(); swap.Reset(); visual.Reset(); target.Reset();
        winW = winH = 0;
        visible = false;
    }

    // ---------------------------------------------------------------- apps
    ComPtr<ID2D1Bitmap> IconFor(const std::wstring& source, const std::wstring& fallback) {
        auto it = icons.find(source);
        if (it != icons.end()) return it->second;
        int px = (int)std::lround(S * 2.0f);
        HBITMAP hb = ShellUtil::Icon(source, px);
        if (!hb && !fallback.empty()) hb = ShellUtil::Icon(fallback, px);
        ComPtr<ID2D1Bitmap> bmp;
        if (hb) {
            ComPtr<IWICBitmap> wb;
            if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapUsePremultipliedAlpha, &wb))) {
                ComPtr<IWICFormatConverter> conv;
                wic->CreateFormatConverter(&conv);
                if (SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
                    dc->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
            }
            DeleteObject(hb);
        }
        icons[source] = bmp;
        return bmp;
    }

    static std::wstring KeyFor(const std::wstring& aumid, const std::wstring& exe) {
        return aumid.empty() ? ShellUtil::Lower(exe) : L"aumid:" + ShellUtil::Lower(aumid);
    }

    bool RefreshApps() {
        std::vector<App> next;
        for (auto& p : s.pins) {
            App a; a.key = p.key; a.launch = p.launch; a.name = p.name; a.iconSource = p.launch; a.pinned = true;
            next.push_back(a);
        }
        struct Ctx { std::vector<App>* apps; } ctx{ &next };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* c = (Ctx*)lp;
            if (!ShellUtil::IsAppWindow(h)) return TRUE;
            std::wstring aumid = ShellUtil::WindowAumid(h), exe = ShellUtil::WindowExe(h);
            if (exe.empty()) return TRUE;
            std::wstring key = KeyFor(aumid, exe), exeKey = ShellUtil::Lower(exe);
            // Match a pin by AUMID or by exe (pins made from plain shortcuts have no AUMID).
            for (auto& a : *c->apps) if (a.key == key || a.key == exeKey) { a.windows.push_back(h); return TRUE; }
            App a;
            a.key = key;
            if (!aumid.empty()) { a.launch = L"shell:AppsFolder\\" + aumid; a.name = ShellUtil::DisplayName(a.launch); }
            else { a.launch = exe; }
            if (a.name.empty()) a.name = ShellUtil::FileDescription(exe);
            a.iconSource = aumid.empty() || ShellUtil::Lower(exe).find(L"applicationframehost") == std::wstring::npos ? (aumid.empty() ? exe : a.launch) : a.launch;
            a.windows.push_back(h);
            c->apps->push_back(a);
            return TRUE;
        }, (LPARAM)&ctx);
        bool changed = next.size() != apps.size();
        for (size_t i = 0; !changed && i < next.size(); ++i)
            changed = next[i].key != apps[i].key || next[i].windows != apps[i].windows;
        HWND fg = GetForegroundWindow();
        if (fg != lastFg) { lastFg = fg; changed = true; }
        apps = std::move(next);
        return changed;
    }

    // ---------------------------------------------------------------- layout
    void Layout() {
        auto mons = EnumerateMonitors();
        RECT mr{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        if (!mons.empty()) mr = mons[std::clamp(s.monitor, 0, (int)mons.size() - 1)].rect;
        HMONITOR hm = MonitorFromRect(&mr, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(hm, &mi);
        mon = mi.rcMonitor; work = mi.rcWork;
        UINT dx = 96, dy = 96; GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
        k = dx / 96.0f;
        S = std::clamp(s.size, 32.0f, 80.0f) * k;
        pad = 9 * k;
        bodyH = S + pad * 2 + 6 * k;

        items.clear();
        items.push_back({ ItStart });
        for (size_t i = 0; i < apps.size(); ++i) items.push_back({ ItApp, (int)i });
        items.push_back({ ItSep });
        if (s.showClock) {
            items.push_back({ ItClock });
            SYSTEM_POWER_STATUS ps{};
            if (GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryFlag != 255) items.push_back({ ItBattery });
        }
        items.push_back({ ItQuick });
        if (s.hideTaskbar) items.push_back({ ItTaskbar });
        float total = 0;
        for (auto& it : items) {
            it.baseW = it.type == ItSep ? 12 * k : it.type == ItClock ? S * 1.45f : S;
            total += it.baseW;
        }
        float gap = 6 * k;
        total += gap * (items.size() - 1);
        float mag = 1 + std::clamp(s.magnify, 0.0f, 100.0f) / 100.0f;
        int w = (int)std::ceil(total + pad * 2 + S * (mag - 1) * 3.2f + 40 * k);
        int h = (int)std::ceil(bodyH + S * (mag - 1) + 44 * k);
        w = std::min(w, (int)(mon.right - mon.left));
        // base positions, centred in the window
        float x = (w - total) / 2;
        for (auto& it : items) { it.baseCx = x + it.baseW / 2; x += it.baseW + gap; }
        bool bottomTaskbar = !s.hideTaskbar;
        int bottom = bottomTaskbar ? work.bottom : mon.bottom;
        int wx = mon.left + ((mon.right - mon.left) - w) / 2;
        int wy = bottom - h - (int)(8 * k);
        SetWindowPos(hwnd, HWND_TOPMOST, wx, wy, w, h, SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : 0));
        if (w != winW || h != winH) {
            bitmap.Reset();
            dc->SetTarget(nullptr);
            if (swap) swap->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
            else {
                ComPtr<IDXGIDevice> dxgi; dev.As(&dxgi);
                ComPtr<IDXGIAdapter> ad; dxgi->GetAdapter(&ad);
                ComPtr<IDXGIFactory2> f; ad->GetParent(IID_PPV_ARGS(&f));
                DXGI_SWAP_CHAIN_DESC1 sd{};
                sd.Width = w; sd.Height = h; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
                f->CreateSwapChainForComposition(dev.Get(), &sd, nullptr, &swap);
                visual->SetContent(swap.Get());
            }
            ComPtr<IDXGISurface> surf;
            swap->GetBuffer(0, IID_PPV_ARGS(&surf));
            auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &bitmap);
            dc->SetTarget(bitmap.Get());
            winW = w; winH = h;
        }
    }

    // Magnified positions for the current hover state.
    void Magnify() {
        float mag = std::clamp(s.magnify, 0.0f, 100.0f) / 100.0f;
        float total = 0, gap = 6 * k;
        for (auto& it : items) {
            float sc = 1;
            if (it.type != ItSep && it.type != ItClock && cursorX >= 0 && hover > 0) {
                float d = (cursorX - it.baseCx) / (S * 1.6f);
                sc = 1 + mag * hover * std::exp(-d * d);
            }
            it.scale = sc;
            it.w = it.baseW * (it.type == ItClock || it.type == ItSep ? 1 : sc);
            total += it.w;
        }
        total += gap * (items.size() - 1);
        float x = (winW - total) / 2;
        for (auto& it : items) { it.x = x; x += it.w + gap; }
    }

    D2D1_RECT_F BodyRect() {
        float l = items.empty() ? 0 : items.front().x - pad, r = items.empty() ? 0 : items.back().x + items.back().w + pad;
        return D2D1::RectF(l, winH - bodyH, r, (float)winH);
    }

    // ---------------------------------------------------------------- drawing
    ComPtr<IDWriteTextFormat> Font(float size, DWRITE_FONT_WEIGHT wgt, bool mono = false) {
        ComPtr<IDWriteTextFormat> f;
        dw->CreateTextFormat(mono ? L"JetBrains Mono" : L"Inter Tight", fonts.Get(), wgt, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
        if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
        return f;
    }

    void Glyph(ItemType t, D2D1_RECT_F r, ID2D1SolidColorBrush* br) {
        float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, u = (r.right - r.left) / 48.0f;
        ComPtr<ID2D1SolidColorBrush> b = br;
        // tile
        D2D1_ROUNDED_RECT tile = D2D1::RoundedRect(D2D1::RectF(r.left + 3 * u, r.top + 3 * u, r.right - 3 * u, r.bottom - 3 * u), 11 * u, 11 * u);
        br->SetColor(Hex(0x15171b, 0.95f)); dc->FillRoundedRectangle(tile, br);
        br->SetColor(Hex(0x2b2e35)); dc->DrawRoundedRectangle(tile, br, 1.0f);
        float sw = 2.2f * u;
        if (t == ItStart) {   // launcher grid
            br->SetColor(Hex(0xd4f53c));
            for (int i = 0; i < 4; ++i) {
                float ox = (i % 2 ? 1 : -1) * 6.5f * u, oy = (i / 2 ? 1 : -1) * 6.5f * u;
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx + ox - 4.6f * u, cy + oy - 4.6f * u, cx + ox + 4.6f * u, cy + oy + 4.6f * u), 2.2f * u, 2.2f * u), br);
            }
        } else if (t == ItQuick) {   // wifi arcs + speaker
            br->SetColor(Hex(0xf3f4f6));
            ComPtr<ID2D1PathGeometry> g; d2d->CreatePathGeometry(&g);
            ComPtr<ID2D1GeometrySink> sk; g->Open(&sk);
            for (int i = 1; i <= 2; ++i) {
                float rr = 5.5f * u * i;
                D2D1_POINT_2F a = D2D1::Point2F(cx - 6 * u - rr * 0.7071f, cy + 2 * u - rr * 0.7071f);
                D2D1_POINT_2F e = D2D1::Point2F(cx - 6 * u + rr * 0.7071f, cy + 2 * u - rr * 0.7071f);
                sk->BeginFigure(a, D2D1_FIGURE_BEGIN_HOLLOW);
                sk->AddArc(D2D1::ArcSegment(e, D2D1::SizeF(rr, rr), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sk->EndFigure(D2D1_FIGURE_END_OPEN);
            }
            sk->Close();
            dc->DrawGeometry(g.Get(), br, sw);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 6 * u, cy + 3 * u), 1.8f * u, 1.8f * u), br);
            // speaker
            ComPtr<ID2D1PathGeometry> sp; d2d->CreatePathGeometry(&sp);
            ComPtr<ID2D1GeometrySink> s2; sp->Open(&s2);
            s2->BeginFigure(D2D1::Point2F(cx + 4 * u, cy - 2 * u), D2D1_FIGURE_BEGIN_FILLED);
            s2->AddLine(D2D1::Point2F(cx + 7 * u, cy - 2 * u)); s2->AddLine(D2D1::Point2F(cx + 11 * u, cy - 6 * u));
            s2->AddLine(D2D1::Point2F(cx + 11 * u, cy + 8 * u)); s2->AddLine(D2D1::Point2F(cx + 7 * u, cy + 4 * u));
            s2->AddLine(D2D1::Point2F(cx + 4 * u, cy + 4 * u));
            s2->EndFigure(D2D1_FIGURE_END_CLOSED); s2->Close();
            dc->FillGeometry(sp.Get(), br);
        } else if (t == ItTaskbar) {   // a bar with dots: "show Windows taskbar"
            br->SetColor(Hex(0xb3b7bf));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 11 * u, cy + 1 * u, cx + 11 * u, cy + 8 * u), 2.5f * u, 2.5f * u), br, sw * 0.8f);
            for (int i = -1; i <= 1; ++i) dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + i * 6 * u, cy + 4.5f * u), 1.4f * u, 1.4f * u), br);
            dc->DrawLine(D2D1::Point2F(cx, cy - 9 * u), D2D1::Point2F(cx, cy - 2 * u), br, sw);
            dc->DrawLine(D2D1::Point2F(cx - 3.5f * u, cy - 5.5f * u), D2D1::Point2F(cx, cy - 9 * u), br, sw);
            dc->DrawLine(D2D1::Point2F(cx + 3.5f * u, cy - 5.5f * u), D2D1::Point2F(cx, cy - 9 * u), br, sw);
        } else if (t == ItBattery) {
            SYSTEM_POWER_STATUS ps{}; GetSystemPowerStatus(&ps);
            int pct = ps.BatteryLifePercent <= 100 ? ps.BatteryLifePercent : 100;
            br->SetColor(Hex(0xf3f4f6));
            D2D1_RECT_F body = D2D1::RectF(cx - 11 * u, cy - 6 * u, cx + 9 * u, cy + 6 * u);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(body, 2.5f * u, 2.5f * u), br, 1.6f * u);
            dc->FillRectangle(D2D1::RectF(cx + 10 * u, cy - 2.5f * u, cx + 12 * u, cy + 2.5f * u), br);
            br->SetColor(pct <= 20 ? Hex(0xff6b5e) : ps.ACLineStatus == 1 ? Hex(0xd4f53c) : Hex(0xf3f4f6));
            float fw = (body.right - body.left - 4 * u) * pct / 100.0f;
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(body.left + 2 * u, body.top + 2 * u, body.left + 2 * u + fw, body.bottom - 2 * u), 1.2f * u, 1.2f * u), br);
            auto f = Font(9.5f * u, DWRITE_FONT_WEIGHT_MEDIUM, true);
            wchar_t txt[8]; swprintf_s(txt, L"%d%%", pct);
            br->SetColor(Hex(0x8c919a));
            dc->DrawTextW(txt, (UINT32)wcslen(txt), f.Get(), D2D1::RectF(r.left, cy + 8 * u, r.right, r.bottom + 2 * u), br);
        }
    }

    void Render() {
        if (!dc || !bitmap) return;
        Magnify();
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        ComPtr<ID2D1SolidColorBrush> br;
        dc->CreateSolidColorBrush(Hex(0xffffff), &br);
        float off = (1 - slide) * (bodyH + 16 * k);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(0, off));

        // glass body
        D2D1_RECT_F body = BodyRect();
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(body, 20 * k, 20 * k);
        br->SetColor(Hex(0x0b0c0f, 0.9f)); dc->FillRoundedRectangle(rr, br.Get());
        br->SetColor(Hex(0x2b2e35, 0.9f)); dc->DrawRoundedRectangle(rr, br.Get(), 1.0f);
        br->SetColor(Hex(0xffffff, 0.07f));
        dc->DrawLine(D2D1::Point2F(body.left + 20 * k, body.top + 1.5f), D2D1::Point2F(body.right - 20 * k, body.top + 1.5f), br.Get(), 1.0f);

        HWND fg = GetForegroundWindow();
        float baseY = winH - pad - 6 * k;   // icon bottom
        for (size_t i = 0; i < items.size(); ++i) {
            Item& it = items[i];
            float size = S * it.scale;
            D2D1_RECT_F ir = D2D1::RectF(it.x + (it.w - size) / 2, baseY - size, it.x + (it.w + size) / 2, baseY);
            if (it.type == ItSep) {
                br->SetColor(Hex(0x2b2e35));
                dc->DrawLine(D2D1::Point2F(it.x + it.w / 2, baseY - S * 0.8f), D2D1::Point2F(it.x + it.w / 2, baseY - S * 0.1f), br.Get(), 1.0f);
            } else if (it.type == ItClock) {
                SYSTEMTIME st; GetLocalTime(&st);
                wchar_t t1[16], t2[24];
                swprintf_s(t1, L"%02d:%02d", st.wHour, st.wMinute);
                static const wchar_t* days[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
                swprintf_s(t2, L"%s %d", days[st.wDayOfWeek], st.wDay);
                auto f1 = Font(S * 0.36f, DWRITE_FONT_WEIGHT_MEDIUM);
                auto f2 = Font(S * 0.22f, DWRITE_FONT_WEIGHT_REGULAR, true);
                br->SetColor(Hex(0xf3f4f6));
                dc->DrawTextW(t1, (UINT32)wcslen(t1), f1.Get(), D2D1::RectF(it.x, baseY - S, it.x + it.w, baseY - S * 0.42f), br.Get());
                br->SetColor(Hex(0x8c919a));
                dc->DrawTextW(t2, (UINT32)wcslen(t2), f2.Get(), D2D1::RectF(it.x, baseY - S * 0.46f, it.x + it.w, baseY - S * 0.08f), br.Get());
            } else if (it.type == ItApp) {
                App& a = apps[it.app];
                auto icon = IconFor(a.iconSource, a.launch);
                if (icon) dc->DrawBitmap(icon.Get(), ir, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                else {
                    br->SetColor(Hex(0x15171b)); dc->FillRoundedRectangle(D2D1::RoundedRect(ir, 10 * k, 10 * k), br.Get());
                }
                if (!a.windows.empty()) {
                    bool active = std::find(a.windows.begin(), a.windows.end(), fg) != a.windows.end();
                    br->SetColor(active ? Hex(0xd4f53c) : Hex(0xb3b7bf, 0.8f));
                    float cx = (ir.left + ir.right) / 2;
                    if (active) dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 6 * k, baseY + 3 * k, cx + 6 * k, baseY + 5.5f * k), 1.5f * k, 1.5f * k), br.Get());
                    else dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, baseY + 4.2f * k), 2.2f * k, 2.2f * k), br.Get());
                }
            } else {
                Glyph(it.type, ir, br.Get());
                if (it.type == ItTaskbar && Taskbar::IsHidden() && g_tbTemp) {
                    br->SetColor(Hex(0xd4f53c));
                    float cx = (ir.left + ir.right) / 2;
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, baseY + 4.2f * k), 2.2f * k, 2.2f * k), br.Get());
                }
            }
        }
        // tooltip for the hovered item
        if (hot >= 0 && hot < (int)items.size() && hover > 0.6f) {
            Item& it = items[hot];
            std::wstring label;
            switch (it.type) {
            case ItStart: label = L"Start"; break;
            case ItApp: label = apps[it.app].name; break;
            case ItClock: label = L"Notifications"; break;
            case ItBattery: label = L"Battery"; break;
            case ItQuick: label = L"Quick settings"; break;
            case ItTaskbar: label = g_tbTemp ? L"Hide Windows taskbar" : L"Show Windows taskbar"; break;
            default: break;
            }
            if (!label.empty()) {
                auto f = Font(12.5f * k, DWRITE_FONT_WEIGHT_MEDIUM);
                ComPtr<IDWriteTextLayout> lay;
                dw->CreateTextLayout(label.c_str(), (UINT32)label.size(), f.Get(), 600 * k, 40 * k, &lay);
                DWRITE_TEXT_METRICS tm{}; lay->GetMetrics(&tm);
                float tw = tm.width + 20 * k, th = 26 * k;
                float cx = it.x + it.w / 2, top = baseY - S * it.scale - th - 10 * k;
                D2D1_RECT_F tr = D2D1::RectF(cx - tw / 2, top, cx + tw / 2, top + th);
                br->SetColor(Hex(0x0d0e11, 0.94f)); dc->FillRoundedRectangle(D2D1::RoundedRect(tr, 8 * k, 8 * k), br.Get());
                br->SetColor(Hex(0x2b2e35)); dc->DrawRoundedRectangle(D2D1::RoundedRect(tr, 8 * k, 8 * k), br.Get(), 1.0f);
                br->SetColor(Hex(0xf3f4f6));
                dc->DrawTextW(label.c_str(), (UINT32)label.size(), f.Get(), tr, br.Get());
            }
        }
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->EndDraw();
        swap->Present(1, 0);
        dcomp->Commit();
    }

    // ---------------------------------------------------------------- behaviour
    int HitItem(POINT client) {
        float off = (1 - slide) * (bodyH + 16 * k);
        float y = client.y - off;
        if (y < winH - bodyH - S * 0.8f || y > winH) return -1;
        for (size_t i = 0; i < items.size(); ++i)
            if (client.x >= items[i].x - 3 * k && client.x <= items[i].x + items[i].w + 3 * k) return items[i].type == ItSep ? -1 : (int)i;
        return -1;
    }

    void Show(bool on) {
        if (on == visible) return;
        visible = on;
        slideTarget = on ? 1.0f : 0.0f;
        if (on) {
            RefreshApps();
            Layout();
            slide = 0;
            Render();
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        SetTimer(hwnd, T_ANIM, 16, nullptr);
    }

    void SetClickThrough(bool on) {
        if (on == clickThrough) return;
        clickThrough = on;
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, on ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT));
    }

    void Poll() {
        if (!s.enabled || inGame) return;
        POINT pt; GetCursorPos(&pt);
        ULONGLONG now = GetTickCount64();
        RECT wr; GetWindowRect(hwnd, &wr);
        bool always = s.mode == "always";
        if (!visible) {
            bool atEdge = pt.y >= mon.bottom - 2 && pt.x >= mon.left && pt.x < mon.right && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000);
            if (always) { Show(true); return; }
            if (atEdge) { if (!edgeSince) edgeSince = now; if (now - edgeSince >= 120) { Show(true); edgeSince = 0; } }
            else edgeSince = 0;
            return;
        }
        POINT c{ pt.x - wr.left, pt.y - wr.top };
        D2D1_RECT_F body = BodyRect();
        float off = (1 - slide) * (bodyH + 16 * k);
        float topZone = winH - bodyH - S * (std::clamp(s.magnify, 0.0f, 100.0f) / 100.0f) * hover - 12 * k + off;
        bool over = c.x >= body.left - 12 * k && c.x <= body.right + 12 * k && c.y >= topZone && c.y <= winH + 4;
        SetClickThrough(!over);
        float newCursor = over ? (float)c.x : -1;
        int newHot = over ? HitItem(c) : -1;
        hoverTarget = over ? 1.0f : 0.0f;
        bool dirty = (over && std::fabs(newCursor - cursorX) > 0.5f) || newHot != hot;
        cursorX = over ? newCursor : cursorX;
        hot = newHot;
        if (over || menuOpen) leaveSince = 0;
        else if (!always) {
            if (!leaveSince) leaveSince = now;
            if (now - leaveSince >= 600) { Show(false); leaveSince = 0; return; }
        }
        // minute tick for the clock
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t ck[8]; swprintf_s(ck, L"%02d%02d", st.wHour, st.wMinute);
        if (lastClock != ck) { lastClock = ck; dirty = true; }
        if (dirty || std::fabs(hover - hoverTarget) > 0.001f) SetTimer(hwnd, T_ANIM, 16, nullptr);
    }

    void Animate() {
        float sp = 0.22f;
        slide += (slideTarget - slide) * 0.28f;
        hover += (hoverTarget - hover) * sp;
        bool done = std::fabs(slide - slideTarget) < 0.004f && std::fabs(hover - hoverTarget) < 0.004f;
        if (done) { slide = slideTarget; hover = hoverTarget; }
        if (hover < 0.01f) cursorX = -1;
        Render();
        if (done) {
            KillTimer(hwnd, T_ANIM);
            if (!visible) ShowWindow(hwnd, SW_HIDE);
        }
    }

    void Activate(App& a) {
        if (a.windows.empty()) { ShellUtil::Launch(a.launch); return; }
        HWND fg = GetForegroundWindow();
        auto it = std::find(a.windows.begin(), a.windows.end(), fg);
        if (it != a.windows.end()) {
            if (a.windows.size() == 1) { ShowWindow(fg, SW_MINIMIZE); return; }
            ++it;
            if (it == a.windows.end()) it = a.windows.begin();
            fg = *it;
        } else fg = a.windows.front();
        if (IsIconic(fg)) ShowWindow(fg, SW_RESTORE);
        SetForegroundWindow(fg);
    }

    void SavePins() {
        std::vector<DockPin> pins;
        for (auto& a : apps) if (a.pinned) pins.push_back({ a.launch, a.key, a.name });
        s.pins = pins;
        if (owner->onPinsChanged) owner->onPinsChanged(pins);
    }

    void ContextMenu(int idx) {
        if (idx < 0 || items[idx].type != ItApp) return;
        App& a = apps[items[idx].app];
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, a.windows.empty() ? L"Open" : L"New window");
        AppendMenuW(m, MF_STRING, 2, a.pinned ? L"Unpin from dock" : L"Pin to dock");
        if (!a.windows.empty()) {
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 3, a.windows.size() > 1 ? L"Close all windows" : L"Close window");
        }
        POINT pt; GetCursorPos(&pt);
        menuOpen = true;
        SetForegroundWindow(hwnd);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_CENTERALIGN, pt.x, pt.y - (int)(10 * k), 0, hwnd, nullptr);
        menuOpen = false;
        DestroyMenu(m);
        if (cmd == 1) ShellUtil::Launch(a.launch);
        else if (cmd == 2) {
            a.pinned = !a.pinned;
            SavePins();
            RefreshApps(); Layout(); Render();
        } else if (cmd == 3) {
            for (HWND w : a.windows) PostMessageW(w, WM_CLOSE, 0, 0);
        }
    }

    void Click(int idx, bool middle) {
        if (idx < 0) return;
        switch (items[idx].type) {
        case ItStart: ShellUtil::SendKeys({ VK_LWIN }); break;
        case ItClock: ShellUtil::SendKeys({ VK_LWIN, 'N' }); break;
        case ItBattery: case ItQuick: ShellUtil::SendKeys({ VK_LWIN, 'A' }); break;
        case ItTaskbar:
            Taskbar::ShowTemporarily(!g_tbTemp);
            Render();
            break;
        case ItApp: {
            App& a = apps[items[idx].app];
            if (middle) ShellUtil::Launch(a.launch); else Activate(a);
            break;
        }
        default: break;
        }
    }

    LRESULT Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == shellMsg && shellMsg) {
            if (visible && RefreshApps()) { Layout(); Render(); }
            return 0;
        }
        switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONUP: { POINT c{ (short)LOWORD(l), (short)HIWORD(l) }; Click(HitItem(c), false); return 0; }
        case WM_MBUTTONUP: { POINT c{ (short)LOWORD(l), (short)HIWORD(l) }; Click(HitItem(c), true); return 0; }
        case WM_RBUTTONUP: { POINT c{ (short)LOWORD(l), (short)HIWORD(l) }; ContextMenu(HitItem(c)); return 0; }
        case WM_TIMER:
            if (w == T_POLL) Poll();
            else if (w == T_ANIM) Animate();
            else if (w == T_REFRESH) {
                if (s.enabled && s.hideTaskbar && !inGame) Taskbar::Hide();   // Explorer sometimes brings it back
                if (visible && RefreshApps()) { Layout(); Render(); }
            }
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

Dock::~Dock() {
    if (impl_) { impl_->Destroy(); delete impl_; }
    Taskbar::Restore();
}

void Dock::Apply(const DockSettings& s, bool inGame) {
    if (!impl_) { impl_ = new Impl(); impl_->owner = this; }
    Impl& I = *impl_;
    bool pinsChanged = s.pins.size() != I.s.pins.size();
    for (size_t i = 0; !pinsChanged && i < s.pins.size(); ++i) pinsChanged = s.pins[i].key != I.s.pins[i].key;
    I.s = s;
    I.inGame = inGame;
    if (!s.enabled) {
        I.Destroy();
        Taskbar::Restore();
        if (onTaskbarStateChanged) onTaskbarStateChanged();
        return;
    }
    if (!I.EnsureDevice()) return;
    I.CreateWindowOnce();
    if (s.hideTaskbar) Taskbar::Hide(); else Taskbar::Restore();
    if (onTaskbarStateChanged) onTaskbarStateChanged();
    if (pinsChanged) I.icons.clear();
    I.RefreshApps();
    I.Layout();
    if (inGame) { I.visible = false; ShowWindow(I.hwnd, SW_HIDE); return; }
    if (s.mode == "always") I.Show(true);
    else if (I.visible) I.Render();
}

void Dock::OnTaskbarCreated() {
    // Explorer restarted and re-showed its taskbar: hide the windows again (keep the saved state).
    if (impl_ && impl_->s.enabled && impl_->s.hideTaskbar) Taskbar::Hide();
}

std::vector<DockPin> Dock::ImportTaskbarPins() {
    std::vector<DockPin> pins;
    for (auto& lnk : ShellUtil::TaskbarPins()) {
        ShellUtil::Link L;
        if (!ShellUtil::ResolveLink(lnk, L)) continue;
        if (L.target.empty() && L.aumid.empty()) continue;
        DockPin p;
        p.launch = lnk;
        p.key = L.aumid.empty() ? ShellUtil::Lower(L.target) : L"aumid:" + ShellUtil::Lower(L.aumid);
        p.name = L.name;
        pins.push_back(p);
    }
    return pins;
}
