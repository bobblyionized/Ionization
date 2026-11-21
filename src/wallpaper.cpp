#include "wallpaper.h"
#include "common.h"
#include <d3d11.h>
#include <d3d10.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <wtsapi32.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// Wallpaper shaders: one fullscreen triangle, one pixel shader per look. Colors are the
// Ionization palette (volt, violet, indigo on near-black) and every look is dithered so
// slow gradients don't band.
const char kWpHlsl[] = R"HLSL(
cbuffer W : register(b0) {
    float time; float dim; float aspect; float pad0;
    float2 res; float2 pad1;
    float2 uvScale; float2 uvOffset;
};
Texture2D tSrc : register(t0);
SamplerState sLin : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V VSMain(uint id : SV_VertexID) {
    V o; float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;
}
static const float3 VOLT = float3(0.831, 0.961, 0.235);
static const float3 VIOLET = float3(0.48, 0.30, 0.95);
static const float3 INDIGO = float3(0.16, 0.14, 0.45);
static const float3 INK = float3(0.027, 0.030, 0.040);
float hash(float2 p) { p = frac(p * float2(123.34, 456.21)); p += dot(p, p + 45.32); return frac(p.x * p.y); }
float noise(float2 p) {
    float2 i = floor(p), f = frac(p); float2 u = f * f * (3 - 2 * f);
    return lerp(lerp(hash(i), hash(i + float2(1, 0)), u.x), lerp(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}
float fbm(float2 p) { float v = 0, a = 0.5; [unroll] for (int i = 0; i < 5; i++) { v += a * noise(p); p = p * 2.03 + float2(1.7, 9.2); a *= 0.5; } return v; }
float3 Finish(float3 c, float2 pos) { c *= 1 - dim; c += (hash(pos + frac(time * 0.37) * 61.0) - 0.5) / 255.0; return saturate(c); }

float4 PSAurora(V i) : SV_Target {
    float2 uv = i.uv; float t = time;
    float y = 1 - uv.y;
    float3 col = INK + float3(0.020, 0.015, 0.050) * y;
    float st = hash(floor(uv * res / 2.0));
    col += step(0.9975, st) * (0.55 + 0.45 * sin(t * 1.7 + st * 40.0)) * 0.45 * y;
    [unroll] for (int k = 0; k < 3; k++) {
        float fk = k;
        float x = uv.x * aspect * (0.9 + fk * 0.35) + t * (0.020 + fk * 0.012);
        float curve = 0.60 - fk * 0.08 + 0.10 * sin(x * 1.6 + fk * 2.3 + t * 0.05) + 0.05 * sin(x * 4.1 - t * 0.09)
                    + (fbm(float2(x * 0.7, fk * 3.1 + t * 0.03)) - 0.5) * 0.18;
        float d = y - curve;
        float glow = exp(-max(d, 0) * (7.0 - fk * 1.5)) * exp(-max(-d, 0) * 42.0);
        float rays = 0.45 + 0.55 * noise(float2(x * 26.0, t * 0.25 + fk * 7.0));
        float3 mint = float3(0.30, 0.95, 0.62);
        float3 c = k == 0 ? lerp(VOLT, mint, 0.35 + 0.35 * sin(x * 0.7 + t * 0.05)) : lerp(mint, VIOLET, saturate(fk * 0.5 + 0.25 * sin(x * 0.8 + t * 0.04)));
        col += c * glow * rays * (k == 0 ? 0.50 : k == 1 ? 0.24 : 0.16);
    }
    return float4(Finish(col, i.pos.xy), 1);
}

float4 PSFlow(V i) : SV_Target {
    float2 p = (i.uv - 0.5) * float2(aspect, 1) * 2.4;
    float t = time * 0.6;
    float2 q = float2(fbm(p + t * 0.05), fbm(p + float2(5.2, 1.3) - t * 0.04));
    float2 r = float2(fbm(p + 3.2 * q + float2(1.7, 9.2) + t * 0.07), fbm(p + 3.2 * q + float2(8.3, 2.8) - t * 0.06));
    float f = fbm(p + 3.0 * r);
    float3 col = lerp(INK, INDIGO, saturate(f * f * 2.2));
    col = lerp(col, VIOLET * 0.8, saturate(length(q) - 0.45) * 0.9);
    col *= 0.55 + 0.6 * f;
    col += VOLT * pow(saturate((r.x - 0.45) * 2.2), 3.0) * 0.32;
    return float4(Finish(col, i.pos.xy), 1);
}

float4 PSGrid(V i) : SV_Target {
    float2 p = i.uv - 0.5; p.x *= aspect;
    float t = time, hz = 0.02;
    float3 col = INK + float3(0.04, 0.02, 0.09) * saturate(1 - abs(p.y - hz) * 3.0);
    col += VIOLET * 0.35 * exp(-abs(p.y - hz) * 40.0);
    if (p.y > hz) {
        float z = 0.32 / (p.y - hz);
        float2 g = float2(p.x * z * 1.1, z + t * 0.7);
        float2 w = max(fwidth(g), 1e-4);
        float2 d = (0.5 - abs(frac(g) - 0.5)) / w;
        float lines = 1.0 - saturate(min(d.x, d.y) * 0.7);
        float fade = saturate(1.0 - z / 24.0) * saturate((p.y - hz) * 30.0);
        float pulse = exp(-pow(frac(z * 0.06 + t * 0.12) - 0.5, 2.0) * 60.0);
        col += VIOLET * 0.42 * lines * fade + VOLT * lines * fade * pulse * 0.95;
        col += VIOLET * 0.05 * fade;
    } else {
        float st = hash(floor(i.uv * res / 2.0));
        col += step(0.998, st) * 0.35 * saturate((hz - p.y) * 4.0);
    }
    return float4(Finish(col, i.pos.xy), 1);
}

float4 PSInk(V i) : SV_Target {
    float2 p = (i.uv - 0.5) * float2(aspect, 1);
    float t = time * 0.35;
    float2 w = p + 0.08 * float2(fbm(p * 2.0 + t * 0.2), fbm(p * 2.0 + 7.3 - t * 0.2)) - 0.04;
    float field = 0;
    [unroll] for (int k = 0; k < 7; k++) {
        float fk = k;
        float2 c = float2(sin(t * (0.21 + fk * 0.037) + fk * 1.9) * 0.62 * aspect / 1.6, cos(t * (0.17 + fk * 0.041) + fk * 2.7) * 0.36);
        float r = 0.13 + 0.04 * sin(t * 0.5 + fk);
        float2 dd = w - c;
        field += r * r / (dot(dd, dd) + 1e-4);
    }
    float ink = smoothstep(0.9, 1.6, field);
    float edge = smoothstep(0.88, 1.0, field) * (1 - smoothstep(1.0, 1.18, field));
    float3 bg = INK + float3(0.012, 0.012, 0.018) * (1 - length(p));
    float3 inkCol = lerp(float3(0.07, 0.07, 0.10), INDIGO * 0.75, saturate(fbm(w * 3.0 + t * 0.3) * 1.4 - 0.2));
    float3 col = lerp(bg, inkCol, ink);
    col += VOLT * edge * 0.16 + VIOLET * edge * 0.10;
    return float4(Finish(col, i.pos.xy), 1);
}

float4 PSMesh(V i) : SV_Target {
    float2 p = i.uv * float2(aspect, 1);
    float t = time * 0.25;
    float3 cols[5] = { float3(0.30, 0.14, 0.62), float3(0.10, 0.12, 0.42), float3(0.80, 0.93, 0.25), float3(0.92, 0.50, 0.22), float3(0.05, 0.05, 0.08) };
    float3 acc = 0; float wsum = 0;
    [unroll] for (int k = 0; k < 5; k++) {
        float fk = k;
        float2 c = float2(0.5 * aspect + sin(t * (0.6 + fk * 0.13) + fk * 2.1) * 0.42 * aspect, 0.5 + cos(t * (0.5 + fk * 0.11) + fk * 1.3) * 0.42);
        float wgt = 1.0 / pow(length(p - c) + 0.08, 2.6) * (k == 2 ? 0.35 : k == 4 ? 1.4 : 1.0);
        acc += cols[k] * wgt; wsum += wgt;
    }
    return float4(Finish(acc / wsum * 0.62, i.pos.xy), 1);
}

float4 PSTex(V i) : SV_Target {
    return float4(Finish(tSrc.Sample(sLin, uvOffset + i.uv * uvScale).rgb, i.pos.xy), 1);
}
)HLSL";

struct CB { float time, dim, aspect, pad0; float resX, resY, pad1, pad2; float uvScaleX, uvScaleY, uvOffX, uvOffY; };
static_assert(sizeof(CB) % 16 == 0, "cbuffer alignment");

struct MediaNotify : IMFMediaEngineNotify {
    std::atomic<ULONG> refs{ 1 };
    std::atomic<long> error{ 0 };
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEngineNotify)) { *out = static_cast<IMFMediaEngineNotify*>(this); AddRef(); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --refs; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD ev, DWORD_PTR p1, DWORD p2) override {
        if (ev == MF_MEDIA_ENGINE_EVENT_ERROR) error = p2 ? (long)p2 : (long)p1 ? (long)p1 : 1;
        return S_OK;
    }
};

const wchar_t* kChildClass = L"IonizationWallpaper";
const wchar_t* kMsgClass = L"IonizationWallpaperMsg";

LRESULT CALLBACK ChildProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

// Where wallpaper windows go. Windows 11 24H2+: icons (SHELLDLL_DefView) and the wallpaper
// (WorkerW) are both children of Progman, so we slot in between them. Older builds: the
// WorkerW that Explorer spawns behind the icon layer.
HWND FindHost(HWND& insertAfter) {
    insertAfter = nullptr;
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    DWORD_PTR r = 0;
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &r);   // ask for the WorkerW behind the icons
    if (HWND defview = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr)) { insertAfter = defview; return progman; }
    HWND found = nullptr;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        if (FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr)) { *(HWND*)lp = FindWindowExW(nullptr, h, L"WorkerW", nullptr); return FALSE; }
        return TRUE;
    }, (LPARAM)&found);
    if (!found) {
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &r);
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            if (FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr)) { *(HWND*)lp = FindWindowExW(nullptr, h, L"WorkerW", nullptr); return FALSE; }
            return TRUE;
        }, (LPARAM)&found);
    }
    return found ? found : progman;
}

bool BatterySaver() {
    SYSTEM_POWER_STATUS ps{};
    return GetSystemPowerStatus(&ps) && ps.SystemStatusFlag == 1;
}
} // namespace

struct Surface {
    HWND hwnd = nullptr;
    RECT mon{};
    HMONITOR hmon = nullptr;
    int w = 0, h = 0, bw = 0, bh = 0;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<ID3D11RenderTargetView> rtv;
    bool covered = false, drawn = false;
};

struct WallpaperSession {
    Wallpaper* owner = nullptr;
    WallpaperSettings s;
    bool inGame = false, locked = false, first = true;
    unsigned seen = ~0u;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<ID3D11VertexShader> vs;
    std::map<std::string, ComPtr<ID3D11PixelShader>> ps;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11SamplerState> smp;
    ComPtr<IWICImagingFactory> wic;
    std::vector<Surface> surfaces;
    HWND msgWnd = nullptr;
    // image
    ComPtr<ID3D11ShaderResourceView> imgSrv; int imgW = 0, imgH = 0; std::wstring imgPath;
    // video
    bool mfStarted = false;
    ComPtr<IMFDXGIDeviceManager> mfMgr; UINT mfToken = 0;
    ComPtr<IMFMediaEngine> engine; MediaNotify* notify = nullptr;
    std::wstring vidPath;
    ComPtr<ID3D11Texture2D> vidTex; ComPtr<ID3D11ShaderResourceView> vidSrv; int vidW = 0, vidH = 0;
    bool vidPlaying = false, vidFrame = false;
    std::string error;
    LARGE_INTEGER qpf{}, qStart{};
    double lastFrame = -1, animTime = 0, panTime = 0;
    ULONGLONG lastCover = 0;
    std::wstring dump;

    double Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return double(t.QuadPart - qStart.QuadPart) / double(qpf.QuadPart); }
    std::string lastStatus;
    void SetStatus(const std::string& st) {
        if (st != lastStatus) { lastStatus = st; LogLine("Wallpaper: %s", st.c_str()); }
        std::lock_guard<std::mutex> lk(owner->mu_); owner->status_ = st;
    }

    static LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* S = (WallpaperSession*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (S) {
            if (m == WM_WTSSESSION_CHANGE) { if (w == WTS_SESSION_LOCK) S->locked = true; else if (w == WTS_SESSION_UNLOCK) S->locked = false; return 0; }
            if (m == WM_DISPLAYCHANGE || (m == WM_SETTINGCHANGE && w == SPI_SETWORKAREA)) { S->owner->rehost_ = true; return 0; }
        }
        return DefWindowProcW(h, m, w, l);
    }

    bool Init() {
        QueryPerformanceFrequency(&qpf); QueryPerformanceCounter(&qStart);
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)) &&
            FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
            return false;
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);   // Media Foundation uses the device from its own threads
        ComPtr<IDXGIDevice> dxgi; dev.As(&dxgi);
        dxgi->SetGPUThreadPriority(-4);                                   // apps and games come first
        ComPtr<IDXGIAdapter> ad; dxgi->GetAdapter(&ad);
        ad->GetParent(IID_PPV_ARGS(&factory));
        DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp));
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        // vertex shader now; pixel shaders when a look is first used (see Shader())
        ComPtr<ID3DBlob> b;
        if (!Compile("VSMain", "vs_5_0", b) || FAILED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs))) return false;
        D3D11_BUFFER_DESC bd{ sizeof(CB), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
        dev->CreateBuffer(&bd, nullptr, &cb);
        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &smp);
        WNDCLASSW wc{}; wc.lpfnWndProc = ChildProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kChildClass;
        RegisterClassW(&wc);
        WNDCLASSW mc{}; mc.lpfnWndProc = MsgProc; mc.hInstance = GetModuleHandleW(nullptr); mc.lpszClassName = kMsgClass;
        RegisterClassW(&mc);
        // A hidden top-level window: gets display-change broadcasts and lock/unlock notifications.
        msgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, kMsgClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowLongPtrW(msgWnd, GWLP_USERDATA, (LONG_PTR)this);
        WTSRegisterSessionNotification(msgWnd, NOTIFY_FOR_THIS_SESSION);
        return true;
    }

    bool Compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        ComPtr<ID3DBlob> err;
        HRESULT hr = D3DCompile(kWpHlsl, sizeof(kWpHlsl) - 1, "wallpaper.hlsl", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
        if (FAILED(hr)) LogLine("Wallpaper shader %s failed: %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
        return SUCCEEDED(hr);
    }
    ID3D11PixelShader* Shader(const std::string& name) {
        if (auto it = ps.find(name); it != ps.end()) return it->second.Get();
        static const std::map<std::string, const char*> entries = {
            { "aurora", "PSAurora" }, { "flow", "PSFlow" }, { "grid", "PSGrid" }, { "ink", "PSInk" }, { "mesh", "PSMesh" }, { "tex", "PSTex" } };
        auto e = entries.find(name);
        if (e == entries.end()) return name == "aurora" ? nullptr : Shader("aurora");
        ComPtr<ID3DBlob> b; ComPtr<ID3D11PixelShader> p;
        if (Compile(e->second, "ps_5_0", b)) dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &p);
        ps[name] = p;
        return p.Get();
    }

    void DestroySurfaces() {
        for (auto& sf : surfaces) {
            sf.rtv.Reset(); sf.swap.Reset(); sf.visual.Reset(); sf.target.Reset();
            if (sf.hwnd && IsWindow(sf.hwnd)) DestroyWindow(sf.hwnd);
        }
        surfaces.clear();
        if (dcomp) dcomp->Commit();
    }

    void CreateSurfaces() {
        DestroySurfaces();
        HWND after = nullptr;
        HWND host = FindHost(after);
        if (!host) { error = "Can't find the desktop window"; return; }
        auto mons = EnumerateMonitors();
        std::vector<RECT> rects;
        if (s.allDisplays) for (auto& m : mons) rects.push_back(m.rect);
        else if (!mons.empty()) rects.push_back(mons[std::clamp(s.monitor, 0, (int)mons.size() - 1)].rect);
        if (rects.empty()) rects.push_back(RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) });
        float scale = s.source == "shader" ? 0.5f : 1.0f;   // shaders render at half resolution and are upscaled
        for (auto& r : rects) {
            Surface sf;
            sf.mon = r; sf.hmon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
            sf.w = r.right - r.left; sf.h = r.bottom - r.top;
            sf.bw = std::max(1, (int)(sf.w * scale)); sf.bh = std::max(1, (int)(sf.h * scale));
            POINT pt{ r.left, r.top };
            ScreenToClient(host, &pt);
            sf.hwnd = CreateWindowExW(WS_EX_NOPARENTNOTIFY, kChildClass, L"Ionization Wallpaper", WS_CHILD | WS_CLIPSIBLINGS | WS_DISABLED,
                pt.x, pt.y, sf.w, sf.h, host, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!sf.hwnd) { LogLine("Wallpaper: CreateWindow under host failed (%lu)", GetLastError()); continue; }
            SetWindowPos(sf.hwnd, after ? after : HWND_TOP, pt.x, pt.y, sf.w, sf.h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = sf.bw; sd.Height = sf.bh; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.Scaling = DXGI_SCALING_STRETCH;
            bool viaDcomp = false;
            if (dcomp && SUCCEEDED(dcomp->CreateTargetForHwnd(sf.hwnd, TRUE, &sf.target))) {
                sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                if (SUCCEEDED(factory->CreateSwapChainForComposition(dev.Get(), &sd, nullptr, &sf.swap))) {
                    dcomp->CreateVisual(&sf.visual);
                    sf.visual->SetContent(sf.swap.Get());
                    if (scale != 1.0f) {
                        D2D_MATRIX_3X2_F m{ sf.w / (float)sf.bw, 0, 0, sf.h / (float)sf.bh, 0, 0 };
                        sf.visual->SetTransform(m);
                        sf.visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
                    }
                    sf.target->SetRoot(sf.visual.Get());
                    viaDcomp = true;
                }
            }
            if (!viaDcomp) {
                sf.target.Reset();
                sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
                if (FAILED(factory->CreateSwapChainForHwnd(dev.Get(), sf.hwnd, &sd, nullptr, nullptr, &sf.swap))) {
                    LogLine("Wallpaper: no swap chain for the desktop window");
                    DestroyWindow(sf.hwnd);
                    continue;
                }
            }
            ComPtr<ID3D11Texture2D> bb;
            sf.swap->GetBuffer(0, IID_PPV_ARGS(&bb));
            dev->CreateRenderTargetView(bb.Get(), nullptr, &sf.rtv);
            LogLine("Wallpaper: surface %dx%d (buffer %dx%d) under %s via %s", sf.w, sf.h, sf.bw, sf.bh,
                after ? "Progman (between icons and WorkerW)" : "WorkerW", viaDcomp ? "DirectComposition" : "HWND swap chain");
            surfaces.push_back(std::move(sf));
        }
        if (dcomp) dcomp->Commit();
        lastCover = 0;
    }

    // ---------------------------------------------------------------- sources
    void CloseVideo() {
        if (engine) { engine->Shutdown(); engine.Reset(); }
        if (notify) { notify->Release(); notify = nullptr; }
        vidTex.Reset(); vidSrv.Reset(); vidW = vidH = 0; vidPlaying = vidFrame = false; vidPath.clear();
    }
    bool OpenVideo(const std::wstring& path) {
        CloseVideo();
        if (path.empty()) { error = "Choose a video file"; return false; }
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) { error = "Video file not found"; return false; }
        if (!mfStarted) { if (FAILED(MFStartup(MF_VERSION))) { error = "Media Foundation is unavailable"; return false; } mfStarted = true; }
        if (!mfMgr) {
            if (FAILED(MFCreateDXGIDeviceManager(&mfToken, &mfMgr)) || FAILED(mfMgr->ResetDevice(dev.Get(), mfToken))) { mfMgr.Reset(); error = "Video decoding is unavailable"; return false; }
        }
        ComPtr<IMFMediaEngineClassFactory> f;
        if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) { error = "Video playback is unavailable"; return false; }
        ComPtr<IMFAttributes> a;
        MFCreateAttributes(&a, 3);
        notify = new MediaNotify();
        a->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, mfMgr.Get());
        a->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
        a->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
        if (FAILED(f->CreateInstance(0, a.Get(), &engine))) { CloseVideo(); error = "Video playback is unavailable"; return false; }
        engine->SetMuted(TRUE);
        engine->SetLoop(TRUE);
        engine->SetAutoPlay(FALSE);
        BSTR url = SysAllocString(path.c_str());
        HRESULT hr = engine->SetSource(url);
        SysFreeString(url);
        if (FAILED(hr)) { CloseVideo(); error = "Can't open this video"; return false; }
        vidPath = path;
        LogLine("Wallpaper: video %s", Narrow(path).c_str());
        return true;
    }
    bool PullVideoFrame() {
        if (!engine) return false;
        if (notify && notify->error) { error = "Can't play this video (unsupported format?)"; return false; }
        LONGLONG pts = 0;
        if (engine->OnVideoStreamTick(&pts) != S_OK) return vidFrame;
        DWORD w = 0, h = 0;
        engine->GetNativeVideoSize(&w, &h);
        if (!w || !h) return vidFrame;
        if (!vidTex || (int)w != vidW || (int)h != vidH) {
            vidTex.Reset(); vidSrv.Reset();
            D3D11_TEXTURE2D_DESC td{ w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE };
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &vidTex))) return false;
            dev->CreateShaderResourceView(vidTex.Get(), nullptr, &vidSrv);
            vidW = (int)w; vidH = (int)h;
            LogLine("Wallpaper: video frames %ux%u", w, h);
        }
        RECT dst{ 0, 0, (LONG)w, (LONG)h };
        MFARGB border{ 0, 0, 0, 255 };
        if (SUCCEEDED(engine->TransferVideoFrame(vidTex.Get(), nullptr, &dst, &border))) vidFrame = true;
        return vidFrame;
    }
    bool OpenImage(const std::wstring& path) {
        imgSrv.Reset(); imgW = imgH = 0; imgPath.clear();
        if (path.empty()) { error = "Choose an image"; return false; }
        if (!wic) { error = "Image decoding is unavailable"; return false; }
        ComPtr<IWICBitmapDecoder> d;
        if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &d))) { error = "Can't open this image"; return false; }
        ComPtr<IWICBitmapFrameDecode> fr;
        if (FAILED(d->GetFrame(0, &fr))) { error = "Can't open this image"; return false; }
        UINT w = 0, h = 0; fr->GetSize(&w, &h);
        ComPtr<IWICBitmapSource> src = fr;
        if (std::max(w, h) > 8192) {
            double k = 8192.0 / std::max(w, h);
            ComPtr<IWICBitmapScaler> sc; wic->CreateBitmapScaler(&sc);
            sc->Initialize(fr.Get(), (UINT)(w * k), (UINT)(h * k), WICBitmapInterpolationModeFant);
            src = sc; src->GetSize(&w, &h);
        }
        ComPtr<IWICFormatConverter> conv; wic->CreateFormatConverter(&conv);
        if (FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) { error = "Can't read this image"; return false; }
        std::vector<uint8_t> px((size_t)w * h * 4);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()))) { error = "Can't read this image"; return false; }
        D3D11_TEXTURE2D_DESC td{ w, h, 0, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, D3D11_RESOURCE_MISC_GENERATE_MIPS };
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) { error = "Image is too large"; return false; }
        ctx->UpdateSubresource(tex.Get(), 0, nullptr, px.data(), w * 4, 0);
        dev->CreateShaderResourceView(tex.Get(), nullptr, &imgSrv);
        ctx->GenerateMips(imgSrv.Get());
        imgW = (int)w; imgH = (int)h; imgPath = path;
        LogLine("Wallpaper: image %ux%u", w, h);
        return true;
    }

    // ---------------------------------------------------------------- frame
    void CheckCovered() {
        for (auto& sf : surfaces) sf.covered = false;
        struct Ctx { WallpaperSession* S; DWORD pid; } c{ this, GetCurrentProcessId() };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* c = (Ctx*)lp;
            if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            if (pid == c->pid) return TRUE;
            LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
            if ((ex & WS_EX_TOOLWINDOW) || (ex & WS_EX_TRANSPARENT)) return TRUE;
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (cloaked) return TRUE;
            wchar_t cls[64] = L""; GetClassNameW(h, cls, 64);
            if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcsncmp(cls, L"Shell_", 6)) return TRUE;
            RECT r;
            if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
            bool zoomed = IsZoomed(h) != 0;
            HMONITOR hm = zoomed ? MonitorFromWindow(h, MONITOR_DEFAULTTONULL) : nullptr;
            for (auto& sf : c->S->surfaces) {
                if (sf.covered) continue;
                const RECT& m = sf.mon;
                if ((zoomed && hm == sf.hmon) || (r.left <= m.left + 1 && r.top <= m.top + 1 && r.right >= m.right - 1 && r.bottom >= m.bottom - 1))
                    sf.covered = true;
            }
            return TRUE;
        }, (LPARAM)&c);
    }

    void SaveDump(Surface& sf) {
        ComPtr<ID3D11Texture2D> bb; sf.swap->GetBuffer(0, IID_PPV_ARGS(&bb));
        D3D11_TEXTURE2D_DESC td; bb->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> st;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) return;
        ctx->CopyResource(st.Get(), bb.Get());
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &map))) return;
        ComPtr<IWICBitmap> bmp;
        if (wic && SUCCEEDED(wic->CreateBitmapFromMemory(td.Width, td.Height, GUID_WICPixelFormat32bppBGRA, map.RowPitch, map.RowPitch * td.Height, (BYTE*)map.pData, &bmp))) {
            ComPtr<IWICStream> fs; wic->CreateStream(&fs);
            ComPtr<IWICBitmapEncoder> enc; wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
            ComPtr<IWICBitmapFrameEncode> fe;
            if (fs && SUCCEEDED(fs->InitializeFromFilename(dump.c_str(), GENERIC_WRITE)) && enc && SUCCEEDED(enc->Initialize(fs.Get(), WICBitmapEncoderNoCache)) &&
                SUCCEEDED(enc->CreateNewFrame(&fe, nullptr)) && SUCCEEDED(fe->Initialize(nullptr))) {
                fe->SetSize(td.Width, td.Height);
                WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA; fe->SetPixelFormat(&fmt);
                fe->WriteSource(bmp.Get(), nullptr); fe->Commit(); enc->Commit();
                LogLine("Wallpaper: dumped %s", Narrow(dump).c_str());
            }
        }
        ctx->Unmap(st.Get(), 0);
    }

    void Draw(Surface& sf, ID3D11PixelShader* p, ID3D11ShaderResourceView* srv, int srcW, int srcH, bool doDump) {
        CB c{};
        c.time = (float)animTime; c.dim = std::clamp(s.dim, 0.0f, 90.0f) / 100.0f;
        c.aspect = sf.w / (float)sf.h; c.resX = (float)sf.bw; c.resY = (float)sf.bh;
        c.uvScaleX = c.uvScaleY = 1;
        if (srv && srcW > 0 && srcH > 0) {
            float sa = srcW / (float)srcH, da = c.aspect, sx = 1, sy = 1;
            if (sa > da) sx = da / sa; else sy = sa / da;            // cover: crop the long side
            float dx = 0, dy = 0;
            if (s.source == "image" && s.pan) {
                const float tau = 6.2831853f;
                float zoom = 1.07f + 0.035f * std::sin((float)panTime * tau / 74.0f);
                sx /= zoom; sy /= zoom;
                dx = 0.8f * std::sin((float)panTime * tau / 97.0f); dy = 0.8f * std::cos((float)panTime * tau / 83.0f);
            }
            c.uvScaleX = sx; c.uvScaleY = sy;
            c.uvOffX = (1 - sx) * 0.5f * (1 + dx); c.uvOffY = (1 - sy) * 0.5f * (1 + dy);
        }
        ctx->UpdateSubresource(cb.Get(), 0, nullptr, &c, 0, 0);
        ID3D11RenderTargetView* rt = sf.rtv.Get();
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        D3D11_VIEWPORT vp{ 0, 0, (float)sf.bw, (float)sf.bh, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(p, nullptr, 0);
        ID3D11Buffer* b = cb.Get();
        ctx->PSSetConstantBuffers(0, 1, &b);
        ID3D11SamplerState* sm = smp.Get();
        ctx->PSSetSamplers(0, 1, &sm);
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
        if (doDump) SaveDump(sf);
        sf.swap->Present(0, 0);
        sf.drawn = true;
    }

    void ApplySettings(const WallpaperSettings& n) {
        bool rebuild = first || n.allDisplays != s.allDisplays || n.monitor != s.monitor ||
            ((n.source == "shader") != (s.source == "shader"));
        bool sourceChanged = first || n.source != s.source || n.image != s.image || n.video != s.video;
        s = n;
        first = false;
        if (rebuild) CreateSurfaces();
        if (sourceChanged) {
            error.clear();
            if (s.source != "video") CloseVideo();
            if (s.source != "image") { imgSrv.Reset(); imgPath.clear(); }
            if (s.source == "video" && s.video != vidPath) OpenVideo(s.video);
            if (s.source == "image" && s.image != imgPath) OpenImage(s.image);
        }
        for (auto& sf : surfaces) {
            sf.drawn = false;
            // On an error, step aside so the normal Windows wallpaper shows.
            ShowWindow(sf.hwnd, error.empty() ? SW_SHOWNA : SW_HIDE);
        }
    }

    void Run() {
        if (!Init()) { SetStatus("Can't start the GPU renderer"); return; }
        while (!owner->stop_) {
            WallpaperSettings n;
            unsigned v;
            {
                std::lock_guard<std::mutex> lk(owner->mu_);
                n = owner->s_; v = owner->version_; inGame = owner->inGame_;
                if (!owner->dump_.empty()) { dump.swap(owner->dump_); owner->dump_.clear(); }
            }
            if (v != seen) { seen = v; ApplySettings(n); }
            if (owner->rehost_.exchange(false)) { CreateSurfaces(); for (auto& sf : surfaces) ShowWindow(sf.hwnd, error.empty() ? SW_SHOWNA : SW_HIDE); }
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            if (owner->stop_) break;

            ULONGLONG nowMs = GetTickCount64();
            if (nowMs - lastCover >= 500) { CheckCovered(); lastCover = nowMs; }
            bool anyVisible = false;
            for (auto& sf : surfaces) if (!sf.covered) anyVisible = true;
            bool battery = s.pauseBattery && BatterySaver();
            std::string st;
            bool paused = true;
            if (!error.empty()) st = error;
            else if (surfaces.empty()) st = "Can't attach to the desktop";
            else if (inGame) st = "Paused while a game is running";
            else if (locked) st = "Paused while the PC is locked";
            else if (battery) st = "Paused for battery saver";
            else if (!anyVisible) st = "Paused: a window covers the desktop";
            else { paused = false; st = "Live"; }
            SetStatus(st);
            bool doDump = !dump.empty() && !surfaces.empty() && error.empty();
            if (engine) {
                bool play = !paused || doDump;   // a test dump needs frames even when the desktop is covered
                if (!play && vidPlaying) { engine->Pause(); vidPlaying = false; }
                else if (play && !vidPlaying) { engine->Play(); vidPlaying = true; }
            }
            if (paused && !doDump) { lastFrame = -1; MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT); continue; }

            bool animated = s.source == "shader" || s.source == "video" || (s.source == "image" && s.pan);
            bool allDrawn = true;
            for (auto& sf : surfaces) if (!sf.drawn && !sf.covered) allDrawn = false;
            if (!animated && allDrawn && !doDump) { MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT); continue; }
            double interval = 1.0 / std::clamp(s.fps, 5, 60);
            double now = Now();
            if (animated && lastFrame >= 0 && now - lastFrame < interval && !doDump) {
                DWORD ms = (DWORD)std::max(1.0, std::ceil((interval - (now - lastFrame)) * 1000.0));
                MsgWaitForMultipleObjects(0, nullptr, FALSE, ms, QS_ALLINPUT);
                continue;
            }
            double dt = lastFrame < 0 ? 0 : std::min(0.1, now - lastFrame);
            lastFrame = now;
            animTime += dt * std::clamp(s.speed, 10.0f, 300.0f) / 100.0f;
            panTime += dt;

            ID3D11PixelShader* p = nullptr;
            ID3D11ShaderResourceView* srv = nullptr;
            int sw = 0, sh = 0;
            if (s.source == "video") {
                if (!PullVideoFrame()) { if (!error.empty()) ApplySettings(s); MsgWaitForMultipleObjects(0, nullptr, FALSE, 15, QS_ALLINPUT); continue; }
                p = Shader("tex"); srv = vidSrv.Get(); sw = vidW; sh = vidH;
            } else if (s.source == "image") {
                if (!imgSrv) { MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT); continue; }
                p = Shader("tex"); srv = imgSrv.Get(); sw = imgW; sh = imgH;
            } else {
                p = Shader(s.shader);
            }
            if (!p) { MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT); continue; }
            for (size_t i = 0; i < surfaces.size(); ++i) {
                Surface& sf = surfaces[i];
                bool d = doDump && i == 0;
                if ((sf.covered || (!animated && sf.drawn)) && !d) continue;
                Draw(sf, p, srv, sw, sh, d);
            }
            if (doDump) dump.clear();
        }
        CloseVideo();
        HWND host = surfaces.empty() ? nullptr : GetParent(surfaces[0].hwnd);
        DestroySurfaces();
        // Let Explorer repaint its own wallpaper where ours was.
        if (host) RedrawWindow(host, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        if (HWND progman = FindWindowW(L"Progman", nullptr)) RedrawWindow(progman, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        if (msgWnd) { WTSUnRegisterSessionNotification(msgWnd); DestroyWindow(msgWnd); }
        if (mfMgr) mfMgr.Reset();
        if (mfStarted) MFShutdown();
    }
};

void Wallpaper::ThreadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    {
        WallpaperSession S;
        S.owner = this;
        S.Run();
    }
    CoUninitialize();
}

void Wallpaper::Apply(const WallpaperSettings& s, bool inGame) {
    if (!s.enabled) { Stop(); return; }
    {
        std::lock_guard<std::mutex> lk(mu_);
        s_ = s; inGame_ = inGame; ++version_;
    }
    if (!thread_.joinable()) {
        stop_ = false;
        { std::lock_guard<std::mutex> lk(mu_); status_ = "Starting"; }
        thread_ = std::thread([this] { ThreadMain(); });
        LogLine("Wallpaper on (%s)", s.source.c_str());
    }
}

void Wallpaper::Stop() {
    if (thread_.joinable()) {
        stop_ = true;
        thread_.join();
        LogLine("Wallpaper off");
    }
    std::lock_guard<std::mutex> lk(mu_);
    status_ = "Off";
}

void Wallpaper::OnExplorerRestart() { if (thread_.joinable()) rehost_ = true; }

std::string Wallpaper::Status() {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}
