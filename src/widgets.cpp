#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include "widgets.h"
#include "common.h"
#include "net.h"
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <d2d1_1.h>
#include <dwrite_3.h>
#include <shellscalingapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
ComPtr<IDWriteFontCollection1> LoadEmbeddedFonts(IDWriteFactory5* f);   // clock.cpp

namespace {
D2D1_COLOR_F Hex(UINT rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}
const UINT WM_WEATHER = WM_APP + 60;
enum { T_MINUTE = 1, T_STATS = 2, T_WEATHER = 3 };
const char* kTypes[5] = { "weather", "clocks", "calendar", "todo", "stats" };
const float W = 300;   // widget width (DIP)

struct HourCast { std::wstring label; double t = 0; int code = 0; bool day = true; };
struct Weather {
    bool has = false;
    double temp = 0, feels = 0, hi = 0, lo = 0;
    int code = 0; bool day = true;
    HourCast hours[5]; int nh = 0;
    std::string key, err;
    ULONGLONG at = 0;
    bool busy = false;
};

std::wstring Condition(int c) {
    if (c == 0) return L"Clear";
    if (c == 1) return L"Mostly clear";
    if (c == 2) return L"Partly cloudy";
    if (c == 3) return L"Overcast";
    if (c == 45 || c == 48) return L"Fog";
    if (c >= 51 && c <= 57) return L"Drizzle";
    if (c >= 61 && c <= 67) return L"Rain";
    if (c >= 71 && c <= 77) return L"Snow";
    if (c >= 80 && c <= 82) return L"Showers";
    if (c == 85 || c == 86) return L"Snow showers";
    if (c >= 95) return L"Thunderstorm";
    return L"";
}

struct Sampler {
    ULONGLONG idle0 = 0, total0 = 0, net0In = 0, net0Out = 0, t0 = 0;
    PDH_HQUERY q = nullptr; PDH_HCOUNTER gpuC = nullptr;
    double cpu = 0, gpu = -1, ram = 0, ramUsed = 0, ramTotal = 0, down = 0, up = 0;
    ~Sampler() { if (q) PdhCloseQuery(q); }
    static ULONGLONG U(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }
    void Net(ULONGLONG& in, ULONGLONG& out) {
        in = out = 0;
        MIB_IF_TABLE2* t = nullptr;
        if (GetIfTable2(&t) != NO_ERROR || !t) return;
        for (ULONG i = 0; i < t->NumEntries; ++i) {
            const MIB_IF_ROW2& r = t->Table[i];
            if (!r.InterfaceAndOperStatusFlags.HardwareInterface || r.OperStatus != IfOperStatusUp || r.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            in += r.InOctets; out += r.OutOctets;
        }
        FreeMibTable(t);
    }
    void Sample() {
        FILETIME fi, fk, fu;
        ULONGLONG now = GetTickCount64();
        if (GetSystemTimes(&fi, &fk, &fu)) {
            ULONGLONG idle = U(fi), total = U(fk) + U(fu);   // kernel time includes idle
            if (total0 && total > total0) cpu = 100.0 * (1.0 - (double)(idle - idle0) / (double)(total - total0));
            idle0 = idle; total0 = total;
        }
        MEMORYSTATUSEX ms{ sizeof(ms) };
        if (GlobalMemoryStatusEx(&ms)) {
            ram = ms.dwMemoryLoad;
            ramTotal = ms.ullTotalPhys / 1073741824.0;
            ramUsed = (ms.ullTotalPhys - ms.ullAvailPhys) / 1073741824.0;
        }
        if (!q && PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS)
            if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &gpuC) != ERROR_SUCCESS) gpuC = nullptr;
        if (q && gpuC && PdhCollectQueryData(q) == ERROR_SUCCESS) {
            DWORD size = 0, count = 0;
            if (PdhGetFormattedCounterArrayW(gpuC, PDH_FMT_DOUBLE, &size, &count, nullptr) == PDH_MORE_DATA && size) {
                std::vector<BYTE> buf(size);
                auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
                if (PdhGetFormattedCounterArrayW(gpuC, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
                    double sum = 0;
                    for (DWORD i = 0; i < count; ++i) if (items[i].FmtValue.CStatus == ERROR_SUCCESS) sum += items[i].FmtValue.doubleValue;
                    gpu = std::min(100.0, sum);
                }
            }
        }
        ULONGLONG in, out;
        Net(in, out);
        if (t0 && now > t0 && in >= net0In && out >= net0Out) {
            double s = (now - t0) / 1000.0;
            down = (in - net0In) / s; up = (out - net0Out) / s;
        }
        net0In = in; net0Out = out; t0 = now;
        cpu = std::clamp(cpu, 0.0, 100.0);
    }
};

std::wstring Rate(double bps) {
    wchar_t t[32];
    if (bps >= 1048576) swprintf_s(t, L"%.1f MB/s", bps / 1048576);
    else if (bps >= 1024) swprintf_s(t, L"%.0f KB/s", bps / 1024);
    else swprintf_s(t, L"%.0f B/s", bps);
    return t;
}
} // namespace

struct Widget {
    std::string type;
    HWND hwnd = nullptr;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<ID2D1Bitmap1> bitmap;
    int w = 0, h = 0;
    std::vector<std::pair<D2D1_RECT_F, int>> hits;
    int hover = -1;
    bool zAllowed = false;
    Widgets::Impl* owner = nullptr;
};

struct Widgets::Impl {
    Widgets* pub = nullptr;
    Json cfg = Json::object();
    bool inGame = false, hour24 = true, clockTR = false;
    std::string layer = "desktop", theme = "glass", units = "c";
    float scale = 100, clockSize = 100;
    int monitor = 0;
    float k = 1;
    RECT mon{}, work{};

    ComPtr<ID3D11Device> dev;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<ID2D1Factory1> d2d;
    ComPtr<ID2D1Device> d2dDev;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<IDWriteFactory5> dw;
    ComPtr<IDWriteFontCollection1> fonts;
    std::map<uint64_t, ComPtr<IDWriteTextFormat>> fontCache;
    HWND msg = nullptr;
    std::map<std::string, std::unique_ptr<Widget>> list;
    Weather weather;
    Sampler stats;

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
        static bool reg = false;
        if (!reg) {
            WNDCLASSW mc{}; mc.lpfnWndProc = MsgProc; mc.hInstance = GetModuleHandleW(nullptr); mc.lpszClassName = L"IonizationWidgetsMsg";
            RegisterClassW(&mc);
            WNDCLASSW wc{}; wc.lpfnWndProc = WidgetProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"IonizationWidget";
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&wc);
            reg = true;
        }
        msg = CreateWindowExW(0, L"IonizationWidgetsMsg", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowLongPtrW(msg, GWLP_USERDATA, (LONG_PTR)this);
        return dc && dw;
    }

    static LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* I = (Impl*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (I) {
            if (m == WM_TIMER) { I->OnTimer((int)w); return 0; }
            if (m == WM_WEATHER) { auto* s = (std::string*)l; I->OnWeather(*s); delete s; return 0; }
        }
        return DefWindowProcW(h, m, w, l);
    }
    static LRESULT CALLBACK WidgetProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* wd = (Widget*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (wd && wd->owner) return wd->owner->Handle(*wd, m, w, l);
        return DefWindowProcW(h, m, w, l);
    }

    const Json* Sec(const std::string& type) const { return cfg.find(type); }
    bool On(const std::string& type) const { const Json* s = Sec(type); return s && s->boolean("on", false); }

    void CreateWidget(const std::string& type) {
        auto wd = std::make_unique<Widget>();
        wd->type = type; wd->owner = this;
        DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP;
        if (layer == "top") ex |= WS_EX_TOPMOST;
        wd->hwnd = CreateWindowExW(ex, L"IonizationWidget", L"Ionization Widget", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!wd->hwnd) return;
        SetWindowLongPtrW(wd->hwnd, GWLP_USERDATA, (LONG_PTR)wd.get());
        SetLayeredWindowAttributes(wd->hwnd, 0, 255, LWA_ALPHA);
        // On top: keep out of the blur overlay's capture. On the desktop: part of the scene like the wallpaper.
        SetWindowDisplayAffinity(wd->hwnd, (layer == "top" && !pub->noExclude) ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
        dcomp->CreateTargetForHwnd(wd->hwnd, TRUE, &wd->target);
        dcomp->CreateVisual(&wd->visual);
        wd->target->SetRoot(wd->visual.Get());
        list[type] = std::move(wd);
    }
    void DestroyWidget(const std::string& type) {
        auto it = list.find(type);
        if (it == list.end()) return;
        if (it->second->hwnd) DestroyWindow(it->second->hwnd);
        list.erase(it);
    }
    void DestroyAll() {
        for (auto& kv : list) if (kv.second->hwnd) DestroyWindow(kv.second->hwnd);
        list.clear();
        if (msg) { KillTimer(msg, T_MINUTE); KillTimer(msg, T_STATS); KillTimer(msg, T_WEATHER); }
    }

    // ---------------------------------------------------------------- layout
    float HeightOf(const std::string& type) const {
        if (type == "weather") return 178;
        if (type == "calendar") return 264;
        if (type == "stats") return 186;
        if (type == "clocks") {
            const Json* z = Sec("clocks") ? Sec("clocks")->find("zones") : nullptr;
            int n = z && z->isArr() ? (int)z->arr.size() : 0;
            return 50.0f + std::clamp(n, 1, 6) * 48.0f + 6;
        }
        if (type == "todo") {
            const Json* t = Sec("todo") ? Sec("todo")->find("items") : nullptr;
            int n = t && t->isArr() ? (int)t->arr.size() : 0;
            return n ? 50.0f + std::min(n, 10) * 34.0f + 10 : 128.0f;
        }
        return 160;
    }
    void Layout() {
        auto mons = EnumerateMonitors();
        RECT mr{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        if (!mons.empty()) mr = mons[std::clamp(monitor, 0, (int)mons.size() - 1)].rect;
        HMONITOR hm = MonitorFromRect(&mr, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(hm, &mi);
        mon = mi.rcMonitor; work = mi.rcWork;
        UINT dx = 96, dy = 96; GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
        float nk = dx / 96.0f * std::clamp(scale, 70.0f, 140.0f) / 100.0f;
        if (nk != k) fontCache.clear();
        k = nk;
        float dpi = dx / 96.0f;
        int m = (int)std::lround(28 * dpi), gap = (int)std::lround(14 * dpi);
        int startY = work.top + m;
        if (clockTR) startY += (int)std::lround(138 * dpi * clockSize / 100.0f) + gap;   // keep clear of a top-right clock
        int colX = work.right - m, y = startY;
        for (const char* t : kTypes) {
            auto it = list.find(t);
            if (it == list.end()) continue;
            Widget& wd = *it->second;
            int w = (int)std::ceil(W * k), h = (int)std::ceil(HeightOf(t) * k);
            int x, yy;
            const Json* s = Sec(t);
            double sx = s ? s->num("x", -1) : -1, sy = s ? s->num("y", -1) : -1;
            if (sx >= 0 && sy >= 0) {
                x = std::clamp((int)(mon.left + sx), (int)mon.left, (int)mon.right - w);
                yy = std::clamp((int)(mon.top + sy), (int)mon.top, (int)mon.bottom - h);
            } else {
                if (y + h > work.bottom - m && y != startY) { colX -= w + gap; y = startY; }
                x = colX - w; yy = y;
                y += h + gap;
            }
            Place(wd, x, yy, w, h);
        }
    }
    HWND DesktopAnchor() {
        HWND progman = FindWindowW(L"Progman", nullptr);
        HWND above = progman ? GetWindow(progman, GW_HWNDPREV) : nullptr;
        return above;
    }
    void Place(Widget& wd, int x, int y, int w, int h) {
        HWND after = HWND_TOPMOST;
        if (layer != "top") { HWND a = DesktopAnchor(); after = (a && a != wd.hwnd) ? a : HWND_BOTTOM; }
        wd.zAllowed = true;
        SetWindowPos(wd.hwnd, after, x, y, w, h, SWP_NOACTIVATE);
        wd.zAllowed = false;
        if (w == wd.w && h == wd.h && wd.bitmap) return;
        wd.bitmap.Reset();
        dc->SetTarget(nullptr);
        if (wd.swap) wd.swap->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        else {
            ComPtr<IDXGIDevice> dxgi; dev.As(&dxgi);
            ComPtr<IDXGIAdapter> ad; dxgi->GetAdapter(&ad);
            ComPtr<IDXGIFactory2> f; ad->GetParent(IID_PPV_ARGS(&f));
            DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = w; sd.Height = h; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            if (FAILED(f->CreateSwapChainForComposition(dev.Get(), &sd, nullptr, &wd.swap))) return;
            wd.visual->SetContent(wd.swap.Get());
        }
        ComPtr<IDXGISurface> surf; wd.swap->GetBuffer(0, IID_PPV_ARGS(&surf));
        auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &wd.bitmap);
        wd.w = w; wd.h = h;
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
    D2D1_RECT_F R(float x, float y, float w, float h) const { return D2D1::RectF(x * k, y * k, (x + w) * k, (y + h) * k); }
    void Text(const std::wstring& t, D2D1_RECT_F r, IDWriteTextFormat* f, ID2D1SolidColorBrush* b, D2D1_COLOR_F c) {
        b->SetColor(c);
        dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    std::wstring Time(int hh, int mm, std::wstring* ampm = nullptr) const {
        wchar_t t[16];
        if (hour24) { swprintf_s(t, L"%02d:%02d", hh, mm); if (ampm) ampm->clear(); }
        else { swprintf_s(t, L"%d:%02d", hh % 12 ? hh % 12 : 12, mm); if (ampm) *ampm = hh < 12 ? L"AM" : L"PM"; }
        return t;
    }
    std::wstring Hour(int hh) const {
        wchar_t t[16];
        if (hour24) swprintf_s(t, L"%02d:00", hh);
        else swprintf_s(t, L"%d %s", hh % 12 ? hh % 12 : 12, hh < 12 ? L"AM" : L"PM");
        return t;
    }
    std::wstring Deg(double v) const { wchar_t t[16]; swprintf_s(t, L"%d°", (int)std::lround(v)); return t; }

    // Weather glyphs, drawn (no font icons).
    void Cloud(float cx, float cy, float u, ID2D1SolidColorBrush* b, D2D1_COLOR_F c) {
        b->SetColor(c);
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 4.5f * u, cy + 1 * u), 5 * u, 5 * u), b);
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 1.5f * u, cy - 2.5f * u), 6.5f * u, 6.5f * u), b);
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 6.5f * u, cy + 1.5f * u), 4.5f * u, 4.5f * u), b);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 9.5f * u, cy + 1 * u, cx + 11 * u, cy + 6 * u), 2.5f * u, 2.5f * u), b);
    }
    void Sun(float cx, float cy, float u, ID2D1SolidColorBrush* b, D2D1_COLOR_F c) {
        b->SetColor(c);
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 4.6f * u, 4.6f * u), b);
        for (int i = 0; i < 8; ++i) {
            float a = i * 3.14159f / 4;
            dc->DrawLine(D2D1::Point2F(cx + std::cos(a) * 7 * u, cy + std::sin(a) * 7 * u), D2D1::Point2F(cx + std::cos(a) * 9.5f * u, cy + std::sin(a) * 9.5f * u), b, 1.6f * u, nullptr);
        }
    }
    void Moon(float cx, float cy, float u, ID2D1SolidColorBrush* b, D2D1_COLOR_F c) {
        ComPtr<ID2D1EllipseGeometry> a, d;
        d2d->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), 7 * u, 7 * u), &a);
        d2d->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx + 4 * u, cy - 3 * u), 6 * u, 6 * u), &d);
        ComPtr<ID2D1PathGeometry> p; d2d->CreatePathGeometry(&p);
        ComPtr<ID2D1GeometrySink> s; p->Open(&s);
        a->CombineWithGeometry(d.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, s.Get());
        s->Close();
        b->SetColor(c);
        dc->FillGeometry(p.Get(), b);
    }
    void WeatherIcon(int code, bool day, D2D1_RECT_F r, ID2D1SolidColorBrush* b) {
        float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, u = (r.right - r.left) / 24.0f;
        D2D1_COLOR_F sun = Hex(0xf5c542), moon = Hex(0xc9d2e8), cloud = Hex(0xd6d8dc), dark = Hex(0x9aa0aa), rain = Hex(0x5aa9ff);
        auto Drops = [&](D2D1_COLOR_F c, bool dots) {
            b->SetColor(c);
            for (int i = 0; i < 3; ++i) {
                float x = cx - 5 * u + i * 5 * u, y = cy + 8.5f * u;
                if (dots) dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y + 1.5f * u), 1.4f * u, 1.4f * u), b);
                else dc->DrawLine(D2D1::Point2F(x + 1 * u, y), D2D1::Point2F(x - 1 * u, y + 3.5f * u), b, 1.6f * u);
            }
        };
        if (code == 0) { if (day) Sun(cx, cy, u, b, sun); else Moon(cx, cy, u, b, moon); return; }
        if (code == 1 || code == 2) {
            if (day) Sun(cx - 4 * u, cy - 4 * u, u * 0.8f, b, sun); else Moon(cx - 4 * u, cy - 4 * u, u * 0.8f, b, moon);
            Cloud(cx + 2 * u, cy + 2.5f * u, u * 0.82f, b, cloud);
            return;
        }
        if (code == 45 || code == 48) {
            b->SetColor(dark);
            for (int i = 0; i < 3; ++i) dc->DrawLine(D2D1::Point2F(cx - 9 * u + (i % 2) * 2 * u, cy - 4 * u + i * 4.5f * u), D2D1::Point2F(cx + 9 * u - (i % 2) * 3 * u, cy - 4 * u + i * 4.5f * u), b, 1.8f * u);
            return;
        }
        bool wet = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
        bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
        bool storm = code >= 95;
        Cloud(cx, cy - (wet || snow || storm ? 2.5f * u : 0), u * 0.92f, b, wet || storm ? dark : cloud);
        if (wet) Drops(rain, false);
        if (snow) Drops(Hex(0xf3f4f6), true);
        if (storm) {
            ComPtr<ID2D1PathGeometry> g; d2d->CreatePathGeometry(&g); ComPtr<ID2D1GeometrySink> s; g->Open(&s);
            s->BeginFigure(D2D1::Point2F(cx + 1 * u, cy + 3 * u), D2D1_FIGURE_BEGIN_FILLED);
            s->AddLine(D2D1::Point2F(cx - 3 * u, cy + 9 * u)); s->AddLine(D2D1::Point2F(cx, cy + 9 * u));
            s->AddLine(D2D1::Point2F(cx - 1.5f * u, cy + 13 * u)); s->AddLine(D2D1::Point2F(cx + 4 * u, cy + 7 * u));
            s->AddLine(D2D1::Point2F(cx + 1 * u, cy + 7 * u));
            s->EndFigure(D2D1_FIGURE_END_CLOSED); s->Close();
            b->SetColor(Hex(0xd4f53c)); dc->FillGeometry(g.Get(), b);
        }
    }
    void Arrow(float cx, float cy, float u, bool down, ID2D1SolidColorBrush* b, D2D1_COLOR_F c) {
        b->SetColor(c);
        float d = down ? 1.0f : -1.0f;
        dc->DrawLine(D2D1::Point2F(cx, cy - 5 * u * d), D2D1::Point2F(cx, cy + 5 * u * d), b, 1.6f * u);
        dc->DrawLine(D2D1::Point2F(cx - 3.5f * u, cy + 1.5f * u * d), D2D1::Point2F(cx, cy + 5 * u * d), b, 1.6f * u);
        dc->DrawLine(D2D1::Point2F(cx + 3.5f * u, cy + 1.5f * u * d), D2D1::Point2F(cx, cy + 5 * u * d), b, 1.6f * u);
    }
    void Header(const std::wstring& label, ID2D1SolidColorBrush* b, const std::wstring& right = L"") {
        Text(label, R(18, 12, W - 36, 20), Font(10.5f, DWRITE_FONT_WEIGHT_MEDIUM, true).Get(), b, Hex(0x8c919a));
        if (!right.empty()) Text(right, R(18, 12, W - 36, 20), Font(10.5f, DWRITE_FONT_WEIGHT_REGULAR, true, DWRITE_TEXT_ALIGNMENT_TRAILING).Get(), b, Hex(0x5d626b));
    }
    void Centered(const std::wstring& t1, const std::wstring& t2, float y, ID2D1SolidColorBrush* b) {
        Text(t1, R(18, y, W - 36, 22), Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0xf3f4f6));
        Text(t2, R(18, y + 22, W - 36, 18), Font(12, DWRITE_FONT_WEIGHT_REGULAR, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0x8c919a));
    }

    // ---------------------------------------------------------------- render
    void Render(Widget& wd) {
        if (!wd.bitmap || !dc) return;
        wd.hits.clear();
        dc->SetTarget(wd.bitmap.Get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        ComPtr<ID2D1SolidColorBrush> b;
        dc->CreateSolidColorBrush(Hex(0xffffff), &b);
        float hDip = wd.h / k;
        D2D1_RECT_F card = D2D1::RectF(0.5f, 0.5f, wd.w - 0.5f, wd.h - 0.5f);
        float rad = 18 * k;
        bool glass = theme != "solid";
        b->SetColor(glass ? Hex(0x0b0c0f, 0.80f) : Hex(0x111317));
        dc->FillRoundedRectangle(D2D1::RoundedRect(card, rad, rad), b.Get());
        b->SetColor(glass ? Hex(0xffffff, 0.09f) : Hex(0x2b2e35));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(card, rad, rad), b.Get(), 1.0f);
        if (wd.type == "weather") DrawWeather(b.Get());
        else if (wd.type == "clocks") DrawClocks(b.Get());
        else if (wd.type == "calendar") DrawCalendar(b.Get());
        else if (wd.type == "todo") DrawTodo(wd, b.Get(), hDip);
        else if (wd.type == "stats") DrawStats(b.Get());
        dc->EndDraw();
        dc->SetTarget(nullptr);
        wd.swap->Present(1, 0);
        dcomp->Commit();
    }
    void RenderAll() { for (auto& kv : list) Render(*kv.second); }
    void RenderType(const char* t) { if (auto it = list.find(t); it != list.end()) Render(*it->second); }

    void DrawWeather(ID2D1SolidColorBrush* b) {
        const Json* s = Sec("weather");
        std::wstring place = s ? Widen(s->str("place", "")) : L"";
        if (place.empty()) {
            WeatherIcon(2, true, R(W / 2 - 22, 36, 44, 44), b);
            Centered(L"Set a place for weather", L"Ionization  >  Widgets  >  Weather", 96, b);
            return;
        }
        Text(place, R(18, 12, W - 120, 20), Font(13, DWRITE_FONT_WEIGHT_MEDIUM).Get(), b, Hex(0xd6d8dc));
        if (!weather.has) {
            Text(weather.err.empty() ? L"Loading" : L"Offline", R(18, 12, W - 36, 20), Font(10.5f, DWRITE_FONT_WEIGHT_REGULAR, true, DWRITE_TEXT_ALIGNMENT_TRAILING).Get(), b, Hex(0x5d626b));
            Centered(weather.err.empty() ? L"Getting the forecast" : L"Can't reach the weather service",
                weather.err.empty() ? L"Open-Meteo" : L"It will try again shortly", 76, b);
            return;
        }
        Text(L"Feels " + Deg(weather.feels), R(18, 12, W - 36, 20), Font(10.5f, DWRITE_FONT_WEIGHT_REGULAR, true, DWRITE_TEXT_ALIGNMENT_TRAILING).Get(), b, Hex(0x8c919a));
        Text(Deg(weather.temp), R(16, 32, 170, 60), Font(50, DWRITE_FONT_WEIGHT_SEMI_BOLD).Get(), b, Hex(0xf3f4f6));
        Text(Condition(weather.code) + L"   H " + Deg(weather.hi) + L"  L " + Deg(weather.lo), R(18, 92, W - 36, 20), Font(12.5f, DWRITE_FONT_WEIGHT_REGULAR).Get(), b, Hex(0x8c919a));
        WeatherIcon(weather.code, weather.day, R(W - 18 - 62, 34, 62, 62), b);
        b->SetColor(Hex(0xffffff, 0.07f));
        dc->DrawLine(D2D1::Point2F(18 * k, 120 * k), D2D1::Point2F((W - 18) * k, 120 * k), b, 1.0f);
        float cw = (W - 36) / 5.0f;
        for (int i = 0; i < weather.nh; ++i) {
            float x = 18 + i * cw;
            Text(weather.hours[i].label, R(x, 126, cw, 14), Font(10, DWRITE_FONT_WEIGHT_REGULAR, true, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0x8c919a));
            WeatherIcon(weather.hours[i].code, weather.hours[i].day, R(x + cw / 2 - 10, 141, 20, 20), b);
            Text(Deg(weather.hours[i].t), R(x, 160, cw, 14), Font(12, DWRITE_FONT_WEIGHT_MEDIUM, false, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b, Hex(0xd6d8dc));
        }
    }

    void DrawClocks(ID2D1SolidColorBrush* b) {
        Header(L"WORLD CLOCK", b);
        const Json* z = Sec("clocks") ? Sec("clocks")->find("zones") : nullptr;
        if (!z || !z->isArr() || z->arr.empty()) { Centered(L"No cities yet", L"Add some in Ionization  >  Widgets", 46, b); return; }
        using namespace std::chrono;
        auto now = system_clock::now();
        seconds localOff{ 0 };
        try { localOff = current_zone()->get_info(now).offset; }
        catch (...) { TIME_ZONE_INFORMATION tzi; DWORD r = GetTimeZoneInformation(&tzi); localOff = seconds(-(tzi.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? tzi.DaylightBias : 0)) * 60); }
        auto localDays = floor<days>(now + localOff);
        int row = 0;
        for (auto& e : z->arr) {
            if (row >= 6) break;
            float y = 46 + row * 48.0f;
            std::wstring name = Widen(e.str("name", ""));
            std::string tzName = e.str("tz", "");
            std::wstring timeStr = L"--:--", ampm, sub = L"Unknown time zone";
            bool day = true;
            try {
                const time_zone* tz = locate_zone(tzName);
                auto off = tz->get_info(now).offset;
                auto lt = now + off;
                auto dd = floor<days>(lt);
                hh_mm_ss tod{ floor<minutes>(lt - dd) };
                int hh = (int)tod.hours().count(), mm = (int)tod.minutes().count();
                timeStr = Time(hh, mm, &ampm);
                day = hh >= 6 && hh < 18;
                long long diffMin = duration_cast<minutes>(off - localOff).count();
                std::wstring when = dd == localDays ? L"Today" : dd > localDays ? L"Tomorrow" : L"Yesterday";
                wchar_t d[32];
                if (diffMin == 0) swprintf_s(d, L"%s, same time", when.c_str());
                else if (diffMin % 60 == 0) swprintf_s(d, L"%s, %+lldh", when.c_str(), diffMin / 60);
                else swprintf_s(d, L"%s, %+lldh %02lldm", when.c_str(), diffMin / 60, std::llabs(diffMin % 60));
                sub = d;
            } catch (...) {}
            if (row) { b->SetColor(Hex(0xffffff, 0.06f)); dc->DrawLine(D2D1::Point2F(18 * k, y * k), D2D1::Point2F((W - 18) * k, y * k), b, 1.0f); }
            b->SetColor(day ? Hex(0xf5c542) : Hex(0x6f86c9));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(24 * k, (y + 17) * k), 3.2f * k, 3.2f * k), b);
            Text(name, R(36, y + 6, 150, 22), Font(14, DWRITE_FONT_WEIGHT_MEDIUM).Get(), b, Hex(0xf3f4f6));
            Text(sub, R(36, y + 26, 160, 16), Font(10.5f, DWRITE_FONT_WEIGHT_REGULAR, true).Get(), b, Hex(0x8c919a));
            float right = W - 18 - (ampm.empty() ? 0 : 26);
            Text(timeStr, R(150, y + 6, right - 150, 34), Font(24, DWRITE_FONT_WEIGHT_MEDIUM, false, DWRITE_TEXT_ALIGNMENT_TRAILING).Get(), b, Hex(0xf3f4f6));
            if (!ampm.empty()) Text(ampm, R(W - 18 - 24, y + 14, 24, 20), Font(10.5f, DWRITE_FONT_WEIGHT_MEDIUM, true, DWRITE_TEXT_ALIGNMENT_TRAILING).Get(), b, Hex(0x8c919a));
            ++row;
        }
    }

    void DrawCalendar(ID2D1SolidColorBrush* b) {
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t month[64] = L"";
        SYSTEMTIME first = st; first.wDay = 1;
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &first, L"MMMM", month, 64, nullptr);
        Text(month, R(18, 12, 180, 24), Font(16, DWRITE_FONT_WEIGHT_SEMI_BOLD).Get(), b, Hex(0xf3f4f6));
        Text(std::to_wstring(st.wYear), R(18, 12, W - 36, 24), Font(12, DWRITE_FONT_WEIGHT_REGULAR, true, DWRITE_TEXT_ALIGNMENT_TRAILING).Get(), b, Hex(0x8c919a));
        DWORD fdw = 0;
        GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER, (LPWSTR)&fdw, sizeof(fdw) / sizeof(wchar_t));
        int firstDow = ((int)fdw + 1) % 7;   // 0=Sunday based (LOCALE: 0=Monday)
        const wchar_t* names[7] = { L"Su", L"Mo", L"Tu", L"We", L"Th", L"Fr", L"Sa" };
        float cw = 36, x0 = (W - 7 * cw) / 2;
        for (int i = 0; i < 7; ++i) {
            int dow = (firstDow + i) % 7;
            Text(names[dow], R(x0 + i * cw, 44, cw, 18), Font(10.5f, DWRITE_FONT_WEIGHT_MEDIUM, true, DWRITE_TEXT_ALIGNMENT_CENTER).Get(), b,
                dow == 0 || dow == 6 ? Hex(0x5d626b) : Hex(0x8c919a));
        }
        // day of week of the 1st
        FILETIME ft; SystemTimeToFileTime(&first, &ft); FileTimeToSystemTime(&ft, &first);
        int lead = (first.wDayOfWeek - firstDow + 7) % 7;
        static const int dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        auto daysIn = [](int y, int m) { return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : dim[m - 1]; };
        int n = daysIn(st.wYear, st.wMonth);
        int pm = st.wMonth == 1 ? 12 : st.wMonth - 1, py = st.wMonth == 1 ? st.wYear - 1 : st.wYear;
        int prevN = daysIn(py, pm);
        auto fd = Font(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        for (int cell = 0; cell < 42; ++cell) {
            int r = cell / 7, c = cell % 7, day = cell - lead + 1;
            float x = x0 + c * cw, y = 66 + r * 31.0f;
            int dow = (firstDow + c) % 7;
            D2D1_COLOR_F col = Hex(0xd6d8dc);
            int shown = day;
            if (day < 1) { shown = prevN + day; col = Hex(0x3a3e46); }
            else if (day > n) { shown = day - n; col = Hex(0x3a3e46); }
            else if (dow == 0 || dow == 6) col = Hex(0x9aa0aa);
            if (day == st.wDay) {
                b->SetColor(Hex(0xd4f53c));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((x + cw / 2) * k, (y + 14) * k), 13 * k, 13 * k), b);
                col = Hex(0x0b0d02);
            }
            Text(std::to_wstring(shown), R(x, y, cw, 28), fd.Get(), b, col);
        }
    }

    void DrawTodo(Widget& wd, ID2D1SolidColorBrush* b, float hDip) {
        const Json* t = Sec("todo") ? Sec("todo")->find("items") : nullptr;
        int n = t && t->isArr() ? (int)t->arr.size() : 0, left = 0;
        for (int i = 0; i < n; ++i) if (!t->arr[i].boolean("done", false)) ++left;
        Header(L"TO-DO", b, n ? std::to_wstring(left) + L" left" : L"");
        if (!n) { Centered(L"Nothing to do", L"Add tasks in Ionization  >  Widgets", 48, b); return; }
        auto f = Font(13.5f, DWRITE_FONT_WEIGHT_REGULAR);
        for (int i = 0; i < n && i < 10; ++i) {
            float y = 44 + i * 34.0f;
            bool done = t->arr[i].boolean("done", false);
            D2D1_RECT_F row = R(8, y, W - 16, 32);
            if (wd.hover == i) { b->SetColor(Hex(0xffffff, 0.05f)); dc->FillRoundedRectangle(D2D1::RoundedRect(row, 9 * k, 9 * k), b); }
            D2D1_POINT_2F c = D2D1::Point2F(27 * k, (y + 16) * k);
            if (done) {
                b->SetColor(Hex(0xd4f53c)); dc->FillEllipse(D2D1::Ellipse(c, 8.5f * k, 8.5f * k), b);
                b->SetColor(Hex(0x0b0d02));
                dc->DrawLine(D2D1::Point2F(c.x - 4 * k, c.y + 0.3f * k), D2D1::Point2F(c.x - 1.2f * k, c.y + 3 * k), b, 1.8f * k);
                dc->DrawLine(D2D1::Point2F(c.x - 1.2f * k, c.y + 3 * k), D2D1::Point2F(c.x + 4.2f * k, c.y - 3 * k), b, 1.8f * k);
            } else {
                b->SetColor(wd.hover == i ? Hex(0xb3b7bf) : Hex(0x5d626b));
                dc->DrawEllipse(D2D1::Ellipse(c, 8 * k, 8 * k), b, 1.5f * k);
            }
            std::wstring text = Widen(t->arr[i].str("text", ""));
            ComPtr<IDWriteTextLayout> l;
            D2D1_RECT_F tr = R(44, y, W - 44 - 16, 32);
            dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), f.Get(), tr.right - tr.left, tr.bottom - tr.top, &l);
            if (l) {
                if (done) l->SetStrikethrough(TRUE, DWRITE_TEXT_RANGE{ 0, (UINT32)text.size() });
                b->SetColor(done ? Hex(0x5d626b) : Hex(0xe6e7ea));
                dc->DrawTextLayout(D2D1::Point2F(tr.left, tr.top), l.Get(), b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            wd.hits.push_back({ row, i });
        }
        (void)hDip;
    }

    void DrawStats(ID2D1SolidColorBrush* b) {
        Header(L"SYSTEM", b);
        struct Row { const wchar_t* name; double v; std::wstring sub; };
        wchar_t ramSub[48]; swprintf_s(ramSub, L"%.1f of %.1f GB", stats.ramUsed, stats.ramTotal);
        Row rows[3] = { { L"CPU", stats.cpu, L"" }, { L"GPU", stats.gpu, L"" }, { L"RAM", stats.ram, ramSub } };
        for (int i = 0; i < 3; ++i) {
            float y = 42 + i * 34.0f;
            Text(rows[i].name, R(18, y, 40, 24), Font(11, DWRITE_FONT_WEIGHT_MEDIUM, true).Get(), b, Hex(0x8c919a));
            double v = rows[i].v;
            wchar_t val[16];
            if (v < 0) wcscpy_s(val, L"-"); else swprintf_s(val, L"%d%%", (int)std::lround(v));
            Text(val, R(W - 18 - 52, y, 52, 24), Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_TEXT_ALIGNMENT_TRAILING).Get(), b, Hex(0xf3f4f6));
            D2D1_RECT_F track = R(62, y + 10, W - 62 - 82, 5);
            b->SetColor(Hex(0xffffff, 0.08f)); dc->FillRoundedRectangle(D2D1::RoundedRect(track, 2.5f * k, 2.5f * k), b);
            if (v > 0) {
                D2D1_COLOR_F col = v >= 92 ? Hex(0xff6b5e) : v >= 80 ? Hex(0xf5a23c) : Hex(0xd4f53c);
                b->SetColor(col);
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(track.left, track.top, track.left + (track.right - track.left) * (float)std::min(v, 100.0) / 100, track.bottom), 2.5f * k, 2.5f * k), b);
            }
            if (!rows[i].sub.empty()) Text(rows[i].sub, R(62, y + 16, W - 62 - 82, 14), Font(9.5f, DWRITE_FONT_WEIGHT_REGULAR, true).Get(), b, Hex(0x5d626b));
        }
        b->SetColor(Hex(0xffffff, 0.07f));
        dc->DrawLine(D2D1::Point2F(18 * k, 148 * k), D2D1::Point2F((W - 18) * k, 148 * k), b, 1.0f);
        Arrow(24 * k, 166 * k, k, true, b, Hex(0x5aa9ff));
        Text(Rate(stats.down), R(34, 156, 110, 20), Font(12.5f, DWRITE_FONT_WEIGHT_MEDIUM).Get(), b, Hex(0xd6d8dc));
        Arrow((W / 2 + 6) * k, 166 * k, k, false, b, Hex(0xd4f53c));
        Text(Rate(stats.up), R(W / 2 + 16, 156, 110, 20), Font(12.5f, DWRITE_FONT_WEIGHT_MEDIUM).Get(), b, Hex(0xd6d8dc));
    }

    // ---------------------------------------------------------------- data
    std::string WeatherKey() const {
        const Json* s = Sec("weather");
        if (!s) return {};
        char buf[160];
        snprintf(buf, sizeof(buf), "%.4f,%.4f,%s", s->num("lat", 0), s->num("lon", 0), units.c_str());
        return s->str("place", "").empty() ? std::string() : std::string(buf);
    }
    void FetchWeather() {
        std::string key = WeatherKey();
        if (key.empty() || weather.busy || !msg) return;
        const Json* s = Sec("weather");
        wchar_t url[512];
        swprintf_s(url, L"https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
            L"&current=temperature_2m,apparent_temperature,weather_code,is_day"
            L"&hourly=temperature_2m,weather_code,is_day&daily=temperature_2m_max,temperature_2m_min"
            L"&timezone=auto&forecast_days=2%s", s->num("lat", 0), s->num("lon", 0), units == "f" ? L"&temperature_unit=fahrenheit" : L"");
        weather.busy = true;
        HWND target = msg;
        std::wstring u = url;
        std::thread([target, u, key] {
            std::string body, err;
            bool ok = HttpGetText(u, body, err);
            auto* r = new std::string(key + "\n" + (ok ? body : "ERR " + err));
            if (!PostMessageW(target, WM_WEATHER, 0, (LPARAM)r)) delete r;
        }).detach();
    }
    void OnWeather(const std::string& payload) {
        weather.busy = false;
        size_t nl = payload.find('\n');
        std::string key = payload.substr(0, nl), body = nl == std::string::npos ? "" : payload.substr(nl + 1);
        if (key != WeatherKey()) { FetchWeather(); return; }   // settings changed while it was in flight
        if (body.rfind("ERR ", 0) == 0) {
            weather.err = body.substr(4);
            LogLine("Weather: %s", weather.err.c_str());
            if (weather.key != key) weather.has = false;
            RenderType("weather");
            return;
        }
        Json j = Json::parse(body);
        const Json* cur = j.find("current");
        if (!cur) { weather.err = "bad response"; RenderType("weather"); return; }
        weather.key = key; weather.err.clear(); weather.has = true; weather.at = GetTickCount64();
        weather.temp = cur->num("temperature_2m", 0);
        weather.feels = cur->num("apparent_temperature", weather.temp);
        weather.code = (int)cur->num("weather_code", 0);
        weather.day = cur->num("is_day", 1) != 0;
        if (const Json* d = j.find("daily")) {
            const Json* mx = d->find("temperature_2m_max"); const Json* mn = d->find("temperature_2m_min");
            if (mx && mx->isArr() && !mx->arr.empty()) weather.hi = mx->arr[0].n;
            if (mn && mn->isArr() && !mn->arr.empty()) weather.lo = mn->arr[0].n;
        }
        weather.nh = 0;
        if (const Json* hj = j.find("hourly")) {
            const Json* times = hj->find("time"); const Json* temps = hj->find("temperature_2m");
            const Json* codes = hj->find("weather_code"); const Json* days = hj->find("is_day");
            std::string now = cur->str("time", "");
            if (times && temps && codes && times->isArr()) {
                for (size_t i = 0; i < times->arr.size() && weather.nh < 5; ++i) {
                    const std::string& ts = times->arr[i].s;
                    if (ts <= now) continue;
                    HourCast& hc = weather.hours[weather.nh++];
                    int hh = ts.size() >= 13 ? atoi(ts.substr(11, 2).c_str()) : 0;
                    hc.label = Hour(hh);
                    hc.t = i < temps->arr.size() ? temps->arr[i].n : 0;
                    hc.code = i < codes->arr.size() ? (int)codes->arr[i].n : 0;
                    hc.day = days && i < days->arr.size() ? days->arr[i].n != 0 : true;
                }
            }
        }
        LogLine("Weather: %.1f, code %d, %d hours", weather.temp, weather.code, weather.nh);
        RenderType("weather");
    }

    bool Covered() {
        if (layer == "top") return false;
        HWND fg = GetForegroundWindow();
        if (!fg || IsIconic(fg)) return false;
        wchar_t cls[64] = L""; GetClassNameW(fg, cls, 64);
        if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
        if (IsZoomed(fg)) return true;
        RECT r; GetWindowRect(fg, &r);
        return r.left <= mon.left && r.top <= mon.top && r.right >= mon.right && r.bottom >= mon.bottom;
    }

    void ArmMinute() {
        SYSTEMTIME st; GetLocalTime(&st);
        SetTimer(msg, T_MINUTE, (UINT)((60 - st.wSecond) * 1000 - st.wMilliseconds + 60), nullptr);
    }
    void OnTimer(int id) {
        if (id == T_MINUTE) {
            ArmMinute();
            if (inGame) return;
            RenderType("clocks"); RenderType("calendar");
            if (On("weather") && (!weather.at || GetTickCount64() - weather.at > 30 * 60 * 1000)) FetchWeather();
        } else if (id == T_STATS) {
            stats.Sample();
            if (!inGame && !Covered()) RenderType("stats");
        } else if (id == T_WEATHER) {
            FetchWeather();
        }
    }

    // ---------------------------------------------------------------- input
    LRESULT Handle(Widget& wd, UINT m, WPARAM wp, LPARAM lp) {
        switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_NCHITTEST: {
            POINT p{ (short)LOWORD(lp), (short)HIWORD(lp) };
            ScreenToClient(wd.hwnd, &p);
            for (auto& ht : wd.hits)
                if (p.x >= ht.first.left && p.x <= ht.first.right && p.y >= ht.first.top && p.y <= ht.first.bottom) return HTCLIENT;
            return HTCAPTION;   // drag anywhere else to move it
        }
        case WM_NCLBUTTONDBLCLK: return 0;
        case WM_WINDOWPOSCHANGING: {
            // Desktop widgets stay just above the wallpaper, whatever gets clicked.
            auto* pos = (WINDOWPOS*)lp;
            if (layer != "top" && !wd.zAllowed) pos->flags |= SWP_NOZORDER;
            break;
        }
        case WM_EXITSIZEMOVE: {
            RECT r; GetWindowRect(wd.hwnd, &r);
            if (pub->onMoved) pub->onMoved(wd.type, (int)(r.left - mon.left), (int)(r.top - mon.top));
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT p{ (short)LOWORD(lp), (short)HIWORD(lp) };
            int hov = -1;
            for (auto& ht : wd.hits)
                if (p.x >= ht.first.left && p.x <= ht.first.right && p.y >= ht.first.top && p.y <= ht.first.bottom) hov = ht.second;
            if (hov != wd.hover) { wd.hover = hov; Render(wd); }
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, wd.hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (wd.hover != -1) { wd.hover = -1; Render(wd); }
            return 0;
        case WM_LBUTTONUP: {
            POINT p{ (short)LOWORD(lp), (short)HIWORD(lp) };
            for (auto& ht : wd.hits)
                if (p.x >= ht.first.left && p.x <= ht.first.right && p.y >= ht.first.top && p.y <= ht.first.bottom) {
                    if (wd.type == "todo" && pub->onTodoToggle) pub->onTodoToggle(ht.second);
                    break;
                }
            return 0;
        }
        case WM_NCRBUTTONUP: case WM_RBUTTONUP: {
            POINT pt; GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Widget settings");
            if (wd.type == "weather") AppendMenuW(menu, MF_STRING, 3, L"Refresh weather");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 2, L"Hide this widget");
            SetForegroundWindow(wd.hwnd);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, wd.hwnd, nullptr);
            PostMessageW(wd.hwnd, WM_NULL, 0, 0);
            DestroyMenu(menu);
            std::string type = wd.type;   // the widget may be destroyed by the callbacks below
            Reassert();
            if (cmd == 1 && pub->onOpenSettings) pub->onOpenSettings();
            else if (cmd == 2 && pub->onHide) pub->onHide(type);
            else if (cmd == 3) { weather.at = 0; FetchWeather(); }
            return 0;
        }
        case WM_DISPLAYCHANGE: case WM_DPICHANGED:
            Layout(); RenderAll();
            return 0;
        }
        return DefWindowProcW(wd.hwnd, m, wp, lp);
    }

    void Reassert() {
        for (auto& kv : list) {
            Widget& wd = *kv.second;
            if (!wd.hwnd || !IsWindowVisible(wd.hwnd)) continue;
            wd.zAllowed = true;
            if (layer == "top") SetWindowPos(wd.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            else if (HWND a = DesktopAnchor(); a && a != wd.hwnd)
                SetWindowPos(wd.hwnd, a, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            wd.zAllowed = false;
        }
    }
};

Widgets::~Widgets() {
    if (impl_) { impl_->DestroyAll(); if (impl_->msg) DestroyWindow(impl_->msg); delete impl_; }
}

void Widgets::Apply(const Json& all, bool inGame) {
    const Json* w = all.find("widgets");
    bool any = false;
    if (w) for (const char* t : kTypes) if (const Json* s = w->find(t); s && s->boolean("on", false)) any = true;
    if (!impl_) {
        if (!any) return;
        impl_ = new Impl();
        impl_->pub = this;
    }
    Impl& I = *impl_;
    std::string oldLayer = I.layer, oldUnits = I.units;
    bool oldHour24 = I.hour24;
    I.cfg = w ? *w : Json::object();
    I.layer = I.cfg.str("layer", "desktop");
    I.theme = I.cfg.str("theme", "glass");
    I.units = I.cfg.str("units", "c");
    I.hour24 = I.cfg.boolean("hour24", true);
    I.scale = (float)I.cfg.num("size", 100);
    I.monitor = (int)all.num("display.monitor", 0);
    I.clockTR = all.boolean("clock.enabled", false) && all.str("clock.corner", "tr") == "tr";
    I.clockSize = (float)all.num("clock.size", 100);
    I.inGame = inGame;
    if (!any) { I.DestroyAll(); return; }
    if (!I.EnsureDevice()) return;
    if (oldLayer != I.layer) I.DestroyAll();          // window styles depend on the layer
    if (oldHour24 != I.hour24) I.weather.at = 0;      // hour labels are baked into the forecast
    for (const char* t : kTypes) {
        bool on = I.On(t);
        if (on && !I.list.count(t)) I.CreateWidget(t);
        else if (!on && I.list.count(t)) I.DestroyWidget(t);
    }
    if (inGame) {
        for (auto& kv : I.list) ShowWindow(kv.second->hwnd, SW_HIDE);
        KillTimer(I.msg, T_STATS);
        return;
    }
    if (I.On("stats")) I.stats.Sample();   // so the first frame has real numbers
    I.Layout();
    I.RenderAll();
    for (auto& kv : I.list) ShowWindow(kv.second->hwnd, SW_SHOWNOACTIVATE);
    I.ArmMinute();
    if (I.On("stats")) { I.stats.Sample(); SetTimer(I.msg, T_STATS, 1000, nullptr); }
    else KillTimer(I.msg, T_STATS);
    if (I.On("weather")) {
        std::string key = I.WeatherKey();
        if (key != I.weather.key || oldUnits != I.units || !I.weather.at || GetTickCount64() - I.weather.at > 30 * 60 * 1000) {
            if (key != I.weather.key) { I.weather.has = false; I.weather.err.clear(); I.RenderType("weather"); }
            I.FetchWeather();
        }
        SetTimer(I.msg, T_WEATHER, 30 * 60 * 1000, nullptr);
    } else KillTimer(I.msg, T_WEATHER);
}

void Widgets::Reassert() { if (impl_ && !impl_->inGame) impl_->Reassert(); }
void Widgets::RefreshWeather() { if (impl_) { impl_->weather.at = 0; impl_->FetchWeather(); } }
