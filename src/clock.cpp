#include "clock.h"
#include "common.h"
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <d2d1_1.h>
#include <dwrite_3.h>
#include <shellscalingapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {

D2D1_COLOR_F Hex(UINT rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

} // namespace

// Load the embedded woff2 fonts into a private DirectWrite collection (shared with the dock).
ComPtr<IDWriteFontCollection1> LoadEmbeddedFonts(IDWriteFactory5* f) {
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    if (FAILED(f->CreateInMemoryFontFileLoader(&loader))) return nullptr;
    f->RegisterFontFileLoader(loader.Get());
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(f->CreateFontSetBuilder(&builder))) return nullptr;
    const wchar_t* names[] = { L"FONTS_INTERTIGHT_WOFF2", L"FONTS_JETBRAINSMONO_WOFF2" };
    for (auto n : names) {
        const void* data; DWORD size;
        if (!LoadEmbedded(n, &data, &size)) continue;
        ComPtr<IDWriteFontFileStream> stream;
        if (FAILED(f->UnpackFontFile(DWRITE_CONTAINER_TYPE_WOFF2, data, size, &stream))) continue;
        UINT64 len = 0;
        stream->GetFileSize(&len);
        const void* frag; void* ctx;
        if (FAILED(stream->ReadFileFragment(&frag, 0, len, &ctx))) continue;
        ComPtr<IDWriteFontFile> file;
        HRESULT hr = loader->CreateInMemoryFontFileReference(f, frag, (UINT32)len, nullptr, &file);
        stream->ReleaseFileFragment(ctx);
        if (SUCCEEDED(hr)) builder->AddFontFile(file.Get());
    }
    ComPtr<IDWriteFontSet> set;
    if (FAILED(builder->CreateFontSet(&set))) return nullptr;
    ComPtr<IDWriteFontCollection1> coll;
    if (FAILED(f->CreateFontCollectionFromFontSet(set.Get(), &coll))) return nullptr;
    return coll;
}

struct ClockOverlay::Impl {
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
    std::wstring sans = L"Inter Tight", mono = L"JetBrains Mono";
};

static const wchar_t* kClockClass = L"IonizationClock";

LRESULT CALLBACK ClockProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = (ClockOverlay*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_NCHITTEST:
        return (self && self->editing_) ? HTCAPTION : HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_TIMER:
        if (self) self->Tick();
        return 0;
    case WM_EXITSIZEMOVE:
        if (self && self->editing_) {
            RECT r; GetWindowRect(h, &r);
            HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
            if (self->onMoved) self->onMoved(r.left - mi.rcMonitor.left, r.top - mi.rcMonitor.top);
        }
        return 0;
    case WM_NCRBUTTONUP:
        if (self && self->editing_) self->SetEditing(false);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

ClockOverlay::~ClockOverlay() { Destroy(); delete impl_; }

void ClockOverlay::Destroy() {
    if (hwnd_) { KillTimer(hwnd_, 1); DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (impl_) { impl_->bitmap.Reset(); impl_->dc.Reset(); impl_->visual.Reset(); impl_->target.Reset(); impl_->swap.Reset(); }
    w_ = h_ = 0;
}

bool ClockOverlay::EnsureDevice() {
    if (!impl_) impl_ = new Impl();
    Impl& I = *impl_;
    if (I.dev) return true;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &I.dev, nullptr, nullptr);
    if (FAILED(hr)) return false;
    ComPtr<IDXGIDevice> dxgi; I.dev.As(&dxgi);
    if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&I.dcomp)))) return false;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, (void**)I.d2d.GetAddressOf()))) return false;
    if (FAILED(I.d2d->CreateDevice(dxgi.Get(), &I.d2dDev))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), (IUnknown**)I.dw.GetAddressOf()))) return false;
    I.fonts = LoadEmbeddedFonts(I.dw.Get());
    if (!I.fonts) { I.sans = L"Segoe UI Variable Display"; I.mono = L"Cascadia Mono"; }
    return true;
}

void ClockOverlay::Apply(const ClockSettings& s) {
    bool layerChanged = s.layer != s_.layer;
    s_ = s;
    if (!s_.enabled) { editing_ = false; Destroy(); return; }
    if (!EnsureDevice()) return;
    if (hwnd_ && layerChanged) Destroy();
    if (!hwnd_) {
        static bool reg = false;
        if (!reg) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = ClockProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kClockClass;
            wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
            RegisterClassW(&wc);
            reg = true;
        }
        DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP;
        if (s_.layer != "desktop") ex |= WS_EX_TOPMOST;
        hwnd_ = CreateWindowExW(ex, kClockClass, L"Ionization Clock", WS_POPUP, 0, 0, 10, 10,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd_) return;
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        // On top: keep it out of the blur overlay's capture so it stays crisp.
        // On desktop: let it be captured, so it shows (blurred) through the overlay.
        SetWindowDisplayAffinity(hwnd_, (s_.layer == "desktop" || noExclude) ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE);
        impl_->dcomp->CreateTargetForHwnd(hwnd_, TRUE, &impl_->target);
        impl_->dcomp->CreateVisual(&impl_->visual);
        impl_->target->SetRoot(impl_->visual.Get());
    }
    Layout();
    Render();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    ArmTimer();
}

void ClockOverlay::SetEditing(bool e) {
    if (!hwnd_) { editing_ = false; return; }
    editing_ = e;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (e) ex &= ~WS_EX_TRANSPARENT; else ex |= WS_EX_TRANSPARENT;
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    Render();
}

void ClockOverlay::ArmTimer() {
    SYSTEMTIME st; GetLocalTime(&st);
    UINT ms = 1000 - st.wMilliseconds + 5;
    SetTimer(hwnd_, 1, ms, nullptr);
}

void ClockOverlay::Reassert() {
    if (!hwnd_) return;
    if (s_.layer == "desktop") {
        HWND progman = FindWindowW(L"Progman", nullptr);
        HWND above = progman ? GetWindow(progman, GW_HWNDPREV) : nullptr;
        if (above && above != hwnd_)
            SetWindowPos(hwnd_, above, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    } else {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

void ClockOverlay::Tick() {
    if (!hwnd_) return;
    Render();
    ArmTimer();
}

void ClockOverlay::Layout() {
    auto mons = EnumerateMonitors();
    RECT mr{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (!mons.empty()) mr = mons[std::clamp(s_.monitor, 0, (int)mons.size() - 1)].rect;
    HMONITOR hm = MonitorFromRect(&mr, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(hm, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    dpiScale_ = dx / 96.0f;
    float k = dpiScale_ * std::clamp(s_.size, 40.0f, 250.0f) / 100.0f;
    int w = (int)std::lround((s_.style == "mono" ? 380 : 340) * k);
    int h = (int)std::lround((s_.date ? 138 : 110) * k);
    RECT wa = mi.rcWork;
    int m = (int)std::lround(28 * dpiScale_);
    int x = wa.right - w - m, y = wa.top + m;
    if (s_.corner == "tl") { x = wa.left + m; y = wa.top + m; }
    else if (s_.corner == "bl") { x = wa.left + m; y = wa.bottom - h - m; }
    else if (s_.corner == "br") { x = wa.right - w - m; y = wa.bottom - h - m; }
    else if (s_.corner == "tc") { x = wa.left + (wa.right - wa.left - w) / 2; y = wa.top + m; }
    else if (s_.corner == "custom") { x = mi.rcMonitor.left + s_.x; y = mi.rcMonitor.top + s_.y; }
    HWND after = HWND_TOPMOST;
    if (s_.layer == "desktop") {
        // Sit directly above the desktop (Progman) so it is behind every app window.
        HWND progman = FindWindowW(L"Progman", nullptr);
        HWND above = progman ? GetWindow(progman, GW_HWNDPREV) : nullptr;
        after = (above && above != hwnd_) ? above : HWND_BOTTOM;
    }
    SetWindowPos(hwnd_, after, x, y, w, h, SWP_NOACTIVATE);
    if (w != w_ || h != h_) {
        Impl& I = *impl_;
        I.bitmap.Reset();
        if (I.dc) I.dc->SetTarget(nullptr);
        if (I.swap) {
            I.swap->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        } else {
            ComPtr<IDXGIDevice> dxgi; I.dev.As(&dxgi);
            ComPtr<IDXGIAdapter> ad; dxgi->GetAdapter(&ad);
            ComPtr<IDXGIFactory2> f; ad->GetParent(IID_PPV_ARGS(&f));
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = w; sd.Height = h; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            f->CreateSwapChainForComposition(I.dev.Get(), &sd, nullptr, &I.swap);
            I.visual->SetContent(I.swap.Get());
        }
        if (!I.dc) I.d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &I.dc);
        ComPtr<IDXGISurface> surf;
        I.swap->GetBuffer(0, IID_PPV_ARGS(&surf));
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        I.dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &I.bitmap);
        I.dc->SetTarget(I.bitmap.Get());
        w_ = w; h_ = h;
        I.dcomp->Commit();
    }
}

void ClockOverlay::Render() {
    if (!hwnd_ || !impl_ || !impl_->dc || !impl_->bitmap) return;
    Impl& I = *impl_;
    SYSTEMTIME st; GetLocalTime(&st);
    float k = dpiScale_ * std::clamp(s_.size, 40.0f, 250.0f) / 100.0f;
    float op = std::clamp(s_.opacity, 5.0f, 100.0f) / 100.0f;
    bool right = s_.corner == "tr" || s_.corner == "br";
    bool center = s_.corner == "tc";

    int hour = st.wHour;
    const wchar_t* ampm = L"";
    if (!s_.hour24) { ampm = hour < 12 ? L"AM" : L"PM"; hour = hour % 12; if (hour == 0) hour = 12; }
    wchar_t hm[16], sec[8], date[64];
    swprintf_s(hm, s_.hour24 ? L"%02d:%02d" : L"%d:%02d", hour, st.wMinute);
    swprintf_s(sec, L"%02d", st.wSecond);
    static const wchar_t* days[] = { L"SUN", L"MON", L"TUE", L"WED", L"THU", L"FRI", L"SAT" };
    static const wchar_t* months[] = { L"JAN", L"FEB", L"MAR", L"APR", L"MAY", L"JUN", L"JUL", L"AUG", L"SEP", L"OCT", L"NOV", L"DEC" };
    swprintf_s(date, L"%s  %02d %s %d", days[st.wDayOfWeek], st.wDay, months[st.wMonth - 1], st.wYear);

    I.dc->BeginDraw();
    I.dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    D2D1_RECT_F full = D2D1::RectF(0, 0, (float)w_, (float)h_);

    ComPtr<ID2D1SolidColorBrush> br;
    I.dc->CreateSolidColorBrush(Hex(0xffffff), &br);

    float pad = 22 * k;
    if (s_.backdrop) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, w_ - 0.5f, h_ - 0.5f), 16 * k, 16 * k);
        br->SetColor(Hex(0x0d0e11, 0.88f * op)); I.dc->FillRoundedRectangle(rr, br.Get());
        br->SetColor(Hex(0xffffff, 0.06f * op));
        I.dc->DrawLine(D2D1::Point2F(16 * k, 1.5f), D2D1::Point2F(w_ - 16 * k, 1.5f), br.Get(), 1.0f);
        br->SetColor(Hex(0x2b2e35, op)); I.dc->DrawRoundedRectangle(rr, br.Get(), 1.0f);
    }

    auto fmt = [&](const std::wstring& family, float size, DWRITE_FONT_WEIGHT wgt) {
        ComPtr<IDWriteTextFormat> f;
        I.dw->CreateTextFormat(family.c_str(), I.fonts.Get(), wgt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
        if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return f;
    };
    auto layout = [&](const wchar_t* text, IDWriteTextFormat* f, float tracking) {
        ComPtr<IDWriteTextLayout> l;
        I.dw->CreateTextLayout(text, (UINT32)wcslen(text), f, 4000, 1000, &l);
        ComPtr<IDWriteTextLayout1> l1;
        if (l && tracking != 0 && SUCCEEDED(l.As(&l1))) l1->SetCharacterSpacing(tracking / 2, tracking / 2, 0, { 0, (UINT32)wcslen(text) });
        return l;
    };
    auto width = [](IDWriteTextLayout* l) { DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); return m.widthIncludingTrailingWhitespace; };
    auto baseline = [](IDWriteTextLayout* l) { DWRITE_LINE_METRICS lm{}; UINT32 n = 0; l->GetLineMetrics(&lm, 1, &n); return lm.baseline; };
    auto shadowed = [&](IDWriteTextLayout* l, float x, float y, D2D1_COLOR_F col) {
        if (!s_.backdrop) {
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.28f * op));
            for (int i = 0; i < 4; ++i) {
                float ox = (i & 1 ? 1.0f : -1.0f) * k, oy = (i & 2 ? 2.0f : 0.5f) * k;
                I.dc->DrawTextLayout(D2D1::Point2F(x + ox, y + oy), l, br.Get());
            }
        }
        br->SetColor(col);
        I.dc->DrawTextLayout(D2D1::Point2F(x, y), l, br.Get());
    };

    const bool mono = s_.style == "mono", minimal = s_.style == "minimal";
    float timeSize = (mono ? 54.0f : minimal ? 66.0f : 70.0f) * k;
    auto timeFmt = fmt(mono ? I.mono : I.sans, timeSize, minimal ? DWRITE_FONT_WEIGHT_LIGHT : DWRITE_FONT_WEIGHT_MEDIUM);
    auto secFmt = fmt(mono ? I.mono : I.mono, (mono ? 54.0f : 22.0f) * k, DWRITE_FONT_WEIGHT_REGULAR);
    auto dateFmt = fmt(I.mono, 12.5f * k, DWRITE_FONT_WEIGHT_REGULAR);

    std::wstring timeText = hm;
    if (mono && s_.seconds) timeText += std::wstring(L":") + sec;
    auto tl = layout(timeText.c_str(), timeFmt.Get(), (mono ? 0.0f : -0.035f) * timeSize);
    ComPtr<IDWriteTextLayout> sl, al, dl;
    if (!mono && s_.seconds) sl = layout(sec, secFmt.Get(), 0);
    if (!s_.hour24) al = layout(ampm, dateFmt.Get(), 1.2f * k);
    if (s_.date) dl = layout(date, dateFmt.Get(), 1.4f * k);

    float gap = 8 * k;
    float tw = width(tl.Get());
    float extraW = 0;
    if (sl) extraW = std::max(extraW, width(sl.Get()));
    if (al) extraW = std::max(extraW, width(al.Get()));
    float rowW = tw + (extraW > 0 ? gap + extraW : 0);
    float x0 = right ? w_ - pad - rowW : center ? (w_ - rowW) / 2 : pad;
    float y0 = pad - 6 * k;

    D2D1_COLOR_F fg = Hex(0xf3f4f6, op), volt = Hex(0xd4f53c, op), dim = Hex(0x8c919a, op);
    if (minimal) fg = Hex(0xffffff, 0.92f * op);
    shadowed(tl.Get(), x0, y0, fg);
    if (mono && s_.seconds) {
        // Repaint the colons in volt for the mono style.
        DWRITE_HIT_TEST_METRICS hmx{};
        float px, py;
        for (UINT32 i = 0; i < timeText.size(); ++i) {
            if (timeText[i] != L':') continue;
            tl->HitTestTextPosition(i, FALSE, &px, &py, &hmx);
            I.dc->PushAxisAlignedClip(D2D1::RectF(x0 + hmx.left, y0, x0 + hmx.left + hmx.width, y0 + hmx.height), D2D1_ANTIALIAS_MODE_ALIASED);
            br->SetColor(volt);
            I.dc->DrawTextLayout(D2D1::Point2F(x0, y0), tl.Get(), br.Get());
            I.dc->PopAxisAlignedClip();
        }
    }
    float tb = baseline(tl.Get());
    float ex = x0 + tw + gap;
    if (sl) shadowed(sl.Get(), ex, y0 + tb - baseline(sl.Get()) - (al ? 18 * k : 0), minimal ? dim : volt);
    if (al) shadowed(al.Get(), ex, y0 + tb - baseline(al.Get()), dim);

    if (dl) {
        float dw = width(dl.Get());
        float dot = 6 * k;
        float rowD = dot + 9 * k + dw;
        float dx = right ? w_ - pad - rowD : center ? (w_ - rowD) / 2 : pad;
        float dy = y0 + tb + 16 * k;
        if (!minimal) {
            br->SetColor(Hex(0xd4f53c, 0.25f * op));
            I.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dx + dot / 2, dy + 9 * k), dot, dot), br.Get());
            br->SetColor(volt);
            I.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dx + dot / 2, dy + 9 * k), dot / 2, dot / 2), br.Get());
        }
        shadowed(dl.Get(), minimal ? dx + (right ? rowD - dw : 0) - (right ? 0 : 0) : dx + dot + 9 * k, dy, dim);
    }

    if (editing_) {
        ComPtr<ID2D1StrokeStyle> dash;
        float dashes[] = { 4, 3 };
        I.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
            D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM, 0), dashes, 2, &dash);
        br->SetColor(Hex(0xd4f53c, 1));
        I.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(1, 1, w_ - 1.0f, h_ - 1.0f), 16 * k, 16 * k), br.Get(), 1.5f, dash.Get());
    }
    (void)full;
    I.dc->EndDraw();
    I.swap->Present(1, 0);
}
