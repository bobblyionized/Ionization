#include "renderer.h"
#include "common.h"
#include "lut.h"
#include "shaders.h"
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <cmath>
#include <algorithm>
#include <vector>

using Microsoft::WRL::ComPtr;

bool EffectParams::VisualsActive() const {
    if (!visualsOn) return false;
    return exposure != 0 || contrast != 0 || saturation != 100 || vibrance != 0 || temperature != 0 ||
        tint != 0 || (lutId != "neutral" && lutIntensity > 0) || vignette > 0 || grain > 0 || bloom > 0 ||
        sharpen > 0 || chroma > 0 || warmth > 0;
}

void Renderer::Start(HWND notify) {
    if (thread_.joinable()) return;
    notify_ = notify;
    stop_ = false;
    thread_ = std::thread([this] { ThreadMain(); });
}

void Renderer::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    std::lock_guard<std::mutex> lk(mu_);
    stats_ = RenderStats{};
}

void Renderer::SetParams(const EffectParams& p) {
    std::lock_guard<std::mutex> lk(mu_);
    params_ = p;
    ++paramsVersion_;
}

RenderStats Renderer::Stats() {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
}

void Renderer::RequestDump(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(mu_);
    dumpPath_ = path;
}

// ---------------------------------------------------------------------------

namespace {

struct Constants {
    float decay, lutMix, blendCount, blendSub;
    float exposure, contrast, saturation, vibrance;
    float temperature, tint, warmth, lutSize;
    float vignette, vignetteSoft, grain, frameSeed;
    float bloom, bloomThresh, sharpen, chroma;
    float texelX, texelY, useLut, aspect;
    float srcTexelX, srcTexelY, opacity, shutter;
    float tapStep, maxTaps, jitter, pad0;
};
static_assert(sizeof(Constants) % 16 == 0, "cbuffer alignment");

LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

struct RT {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
    explicit operator bool() const { return tex != nullptr; }
    void Reset() { tex.Reset(); rtv.Reset(); srv.Reset(); w = h = 0; }
};

bool MakeRT(ID3D11Device* dev, int w, int h, DXGI_FORMAT fmt, RT& rt) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &rt.tex))) return false;
    if (FAILED(dev->CreateRenderTargetView(rt.tex.Get(), nullptr, &rt.rtv))) return false;
    if (FAILED(dev->CreateShaderResourceView(rt.tex.Get(), nullptr, &rt.srv))) return false;
    rt.w = w; rt.h = h;
    return true;
}

constexpr DXGI_FORMAT kAccFmt = DXGI_FORMAT_R10G10B10A2_UNORM;   // 4 B/px
constexpr DXGI_FORMAT kSumFmt = DXGI_FORMAT_R16G16B16A16_UINT;   // exact integer box sum
constexpr DXGI_FORMAT kSlotFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT kBloomFmt = DXGI_FORMAT_R11G11B10_FLOAT;
constexpr int kMaxBlend = 8;
constexpr DXGI_FORMAT kLumaFmt = DXGI_FORMAT_R16_FLOAT;
constexpr DXGI_FORMAT kVecFmt = DXGI_FORMAT_R16G16_FLOAT;

} // namespace

struct RenderSession {
    Renderer* owner;
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIOutputDuplication> dupl;
    ComPtr<IDXGISwapChain2> swap;
    HANDLE waitable = nullptr;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> dtarget;
    ComPtr<IDCompositionVisual> dvisual;
    ComPtr<ID3D11RenderTargetView> backRtv;
    ComPtr<ID3D11Texture2D> backTex;

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> psComp[6], psTrailHalf, psBlendHalf, psDown, psBlurH, psBlurV;
    ComPtr<ID3D11PixelShader> psLumaColor, psLumaDown, psMotionCoarse, psMotionFine;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> cbuf;

    // Our own copy of the desktop. It is updated only while we own a duplication frame, and only
    // in the rectangles Windows reports as changed, so it can never pick up a half-written frame.
    ComPtr<ID3D11Texture2D> cap;
    ComPtr<ID3D11ShaderResourceView> capSrv;
    ComPtr<ID3D11Texture2D> duplTex;
    bool holding = false;
    bool capFilled = false;
    std::vector<BYTE> meta;

    // Smooth (optical flow) resources.
    RT lumaQ[2], lumaS[2], vecS, vecQ;
    int lumaCur = 0;
    bool havePrevLuma = false, sharpDrawn = true;
    LARGE_INTEGER lastNewT{};

    // Mode-dependent resources (allocated lazily, freed when unused).
    RT acc[2];                 // trail accumulators (full or half)
    RT curH;                   // efficient trail: half-res current
    RT sum[2];                 // blend running sums (full or half)
    std::vector<RT> ring;      // blend ring slots (N + 1)
    RT avgH;                   // efficient blend: half-res average
    RT bloomA, bloomB;
    int cfgMode = -1, cfgQuality = -1, cfgN = -1;
    int cur = 0, head = 0, filled = 0;

    ComPtr<ID3D11ShaderResourceView> lutSrv;
    std::string lutLoaded = "neutral";
    int lutSize = 0;

    ComPtr<ID3D11Query> qDisjoint[4], qStart[4], qEnd[4];
    int qFrame = 0;

    int width = 0, height = 0, halfW = 0, halfH = 0, refresh = 60, monitorIndex = 0;
    bool haveFrame = false, shown = false, resetAccum = true, presentedLast = false;
    unsigned seenVersion = ~0u;
    int framesSinceNew = 0;
    // Frame-rate limit: a draw that comes too early waits (deferred) until the next allowed slot.
    LARGE_INTEGER nextDueT{};   // when the next frame is due under the cap (keeps the average exact)
    bool deferred = false, deferredNew = false;
    int fpsCap = 0;
    LARGE_INTEGER qpf{}, lastT{}, statT{}, lastGrainT{};
    int presents = 0, captures = 0, loops = 0;
    float gpuMs = 0;
    unsigned frameNo = 0;

    enum Result { Continue, Lost, Restart, Quit };

    ~RenderSession() {
        if (ctx) { ctx->ClearState(); ctx->Flush(); }
        if (dupl && holding) dupl->ReleaseFrame();
        dupl.Reset();
        if (hwnd) { owner->overlay_ = nullptr; DestroyWindow(hwnd); }
    }

    bool Compile(const char* entry, const char* target, ComPtr<ID3DBlob>& blob, const D3D_SHADER_MACRO* defs = nullptr) {
        ComPtr<ID3DBlob> err;
        HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "ionization.hlsl", defs, nullptr,
            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
        if (FAILED(hr)) {
            LogLine("Shader %s failed: %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        return true;
    }
    bool MakePS(const char* entry, ComPtr<ID3D11PixelShader>& ps, const D3D_SHADER_MACRO* defs = nullptr) {
        ComPtr<ID3DBlob> b;
        return Compile(entry, "ps_5_0", b, defs) && SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps));
    }

    bool Init(int monitor) {
        auto mons = EnumerateMonitors();
        if (mons.empty()) { LogLine("No monitors"); return false; }
        monitorIndex = std::clamp(monitor, 0, (int)mons.size() - 1);
        const MonitorInfo& mi = mons[monitorIndex];
        refresh = mi.refresh;

        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(mi.adapter, &adapter))) return false;
        ComPtr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(mi.output, &output))) return false;

        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        if (FAILED(hr)) { LogLine("D3D11CreateDevice failed 0x%08x", hr); return false; }

        // Our work is tiny; give it priority so the overlay never misses a vsync
        // (a missed present repeats a frame, which reads as low FPS).
        ComPtr<IDXGIDevice> dxgiDev;
        dev.As(&dxgiDev);
        int wantPrio = 2;
        wchar_t pv[8];
        if (GetEnvironmentVariableW(L"IONIZATION_GPU_PRIORITY", pv, 8)) wantPrio = _wtoi(pv);   // benchmarking only
        HRESULT prio = dxgiDev->SetGPUThreadPriority(wantPrio);
        if (FAILED(prio) && wantPrio > 2) prio = dxgiDev->SetGPUThreadPriority(2);

        ComPtr<IDXGIOutput5> out5;
        if (SUCCEEDED(output.As(&out5))) {
            DXGI_FORMAT fmts[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
            hr = out5->DuplicateOutput1(dev.Get(), 0, 1, fmts, &dupl);
        }
        if (!dupl) {
            ComPtr<IDXGIOutput1> out1;
            if (SUCCEEDED(output.As(&out1))) hr = out1->DuplicateOutput(dev.Get(), &dupl);
        }
        if (!dupl) { LogLine("DuplicateOutput failed 0x%08x", hr); return false; }
        DXGI_OUTDUPL_DESC dd{};
        dupl->GetDesc(&dd);
        width = (int)dd.ModeDesc.Width;
        height = (int)dd.ModeDesc.Height;
        halfW = (width + 1) / 2;
        halfH = (height + 1) / 2;

        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = OverlayProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"IonizationOverlay";
            RegisterClassW(&wc);
            registered = true;
        }
        hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED |
            WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP,
            L"IonizationOverlay", L"Ionization Overlay", WS_POPUP,
            // One pixel short of the monitor: a topmost window that exactly covers the screen is
            // treated by Windows as a fullscreen app (Focus Assist silences notifications, and
            // fullscreen detection fires). The swap chain stays full size; the last row is clipped.
            mi.rect.left, mi.rect.top, mi.rect.right - mi.rect.left, mi.rect.bottom - mi.rect.top - 1,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) return false;
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        if (!owner->feedbackTest) SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

        ComPtr<IDXGIFactory2> f2;
        factory.As(&f2);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width; sd.Height = height;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> sc1;
        hr = f2->CreateSwapChainForComposition(dev.Get(), &sd, nullptr, &sc1);
        if (FAILED(hr)) { LogLine("CreateSwapChainForComposition failed 0x%08x", hr); return false; }
        sc1.As(&swap);
        swap->SetMaximumFrameLatency(1);
        waitable = swap->GetFrameLatencyWaitableObject();

        hr = DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&dcomp));
        if (FAILED(hr)) return false;
        dcomp->CreateTargetForHwnd(hwnd, TRUE, &dtarget);
        dcomp->CreateVisual(&dvisual);
        dvisual->SetContent(swap.Get());
        dtarget->SetRoot(dvisual.Get());

        swap->GetBuffer(0, IID_PPV_ARGS(&backTex));
        dev->CreateRenderTargetView(backTex.Get(), nullptr, &backRtv);

        ComPtr<ID3DBlob> vsb;
        if (!Compile("VS", "vs_5_0", vsb)) return false;
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
        static const char* pathNames[6] = { "0", "1", "2", "3", "4", "5" };
        for (int i = 0; i < 6; ++i) {
            D3D_SHADER_MACRO defs[] = { { "PATH", pathNames[i] }, { nullptr, nullptr } };
            if (!MakePS("PSComposite", psComp[i], defs)) return false;
        }
        if (!MakePS("PSTrailHalf", psTrailHalf) || !MakePS("PSBlendHalf", psBlendHalf) || !MakePS("PSBloomDown", psDown) ||
            !MakePS("PSBlurH", psBlurH) || !MakePS("PSBlurV", psBlurV)) return false;
        if (!MakePS("PSLumaFromColor", psLumaColor) || !MakePS("PSLumaDown", psLumaDown) ||
            !MakePS("PSMotionCoarse", psMotionCoarse) || !MakePS("PSMotionFine", psMotionFine)) return false;

        D3D11_SAMPLER_DESC smp{};
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smp.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&smp, &sampler);

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(Constants);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&bd, nullptr, &cbuf);

        for (int i = 0; i < 4; ++i) {
            D3D11_QUERY_DESC q{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            dev->CreateQuery(&q, &qDisjoint[i]);
            q.Query = D3D11_QUERY_TIMESTAMP;
            dev->CreateQuery(&q, &qStart[i]);
            dev->CreateQuery(&q, &qEnd[i]);
        }

        QueryPerformanceFrequency(&qpf);
        QueryPerformanceCounter(&lastT);
        statT = lastGrainT = lastT;
        owner->overlay_ = hwnd;
        LogLine("Session up: monitor %d %dx%d @%dHz, gpu priority hr=0x%08x", monitorIndex, width, height, refresh, prio);
        return true;
    }

    bool EnsureCap() {
        if (cap) return true;
        D3D11_TEXTURE2D_DESC cd{};
        cd.Width = width; cd.Height = height; cd.MipLevels = 1; cd.ArraySize = 1;
        cd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; cd.SampleDesc.Count = 1;
        cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&cd, nullptr, &cap))) return false;
        return SUCCEEDED(dev->CreateShaderResourceView(cap.Get(), nullptr, &capSrv));
    }

    // Allocate exactly what the current mode needs and free the rest (memory is shared with games).
    bool Configure(int mode, int quality, int n) {
        if (mode == cfgMode && quality == cfgQuality && (mode != 2 || n == cfgN)) return true;
        for (auto& r : acc) r.Reset();
        for (auto& r : sum) r.Reset();
        curH.Reset(); avgH.Reset(); ring.clear();
        for (auto& r : lumaQ) r.Reset();
        for (auto& r : lumaS) r.Reset();
        vecS.Reset(); vecQ.Reset();
        havePrevLuma = false;
        cfgMode = mode; cfgQuality = quality; cfgN = n;
        resetAccum = true; head = 0; filled = 0; cur = 0;
        int w = quality == 0 ? halfW : width, h = quality == 0 ? halfH : height;
        bool ok = true;
        if (mode == 1) {
            ok = MakeRT(dev.Get(), w, h, kAccFmt, acc[0]) && MakeRT(dev.Get(), w, h, kAccFmt, acc[1]);
            if (ok && quality == 0) ok = MakeRT(dev.Get(), w, h, kAccFmt, curH);
        } else if (mode == 2) {
            ok = MakeRT(dev.Get(), w, h, kSumFmt, sum[0]) && MakeRT(dev.Get(), w, h, kSumFmt, sum[1]);
            ring.resize(n + 1);
            for (auto& r : ring) ok = ok && MakeRT(dev.Get(), w, h, kSlotFmt, r);
            if (ok && quality == 0) ok = MakeRT(dev.Get(), w, h, kAccFmt, avgH);
        } else if (mode == 3) {
            int qw = std::max(1, (width + 3) / 4), qh = std::max(1, (height + 3) / 4);
            int sw = std::max(1, (qw + 3) / 4), sh = std::max(1, (qh + 3) / 4);
            ok = MakeRT(dev.Get(), qw, qh, kLumaFmt, lumaQ[0]) && MakeRT(dev.Get(), qw, qh, kLumaFmt, lumaQ[1]) &&
                 MakeRT(dev.Get(), sw, sh, kLumaFmt, lumaS[0]) && MakeRT(dev.Get(), sw, sh, kLumaFmt, lumaS[1]) &&
                 MakeRT(dev.Get(), sw, sh, kVecFmt, vecS) && MakeRT(dev.Get(), qw, qh, kVecFmt, vecQ);
        }
        ok = ok && EnsureCap();
        if (!ok) LogLine("Configure(mode=%d quality=%d n=%d) failed", mode, quality, n);
        return ok;
    }

    void EnsureBloom(bool on) {
        if (!on) { bloomA.Reset(); bloomB.Reset(); return; }
        if (bloomA) return;
        int w = std::max(1, width / 4), h = std::max(1, height / 4);
        MakeRT(dev.Get(), w, h, kBloomFmt, bloomA);
        MakeRT(dev.Get(), w, h, kBloomFmt, bloomB);
    }

    void EnsureLut(const std::string& id) {
        if (id == lutLoaded) return;
        lutLoaded = id;
        lutSrv.Reset();
        lutSize = 0;
        LutData data;
        if (!LoadLut(id, data)) return;
        using namespace DirectX::PackedVector;
        std::vector<HALF> px((size_t)data.size * data.size * data.size * 4);
        for (size_t i = 0, n = px.size() / 4; i < n; ++i) {
            px[i * 4 + 0] = XMConvertFloatToHalf(data.rgb[i * 3 + 0]);
            px[i * 4 + 1] = XMConvertFloatToHalf(data.rgb[i * 3 + 1]);
            px[i * 4 + 2] = XMConvertFloatToHalf(data.rgb[i * 3 + 2]);
            px[i * 4 + 3] = XMConvertFloatToHalf(1.0f);
        }
        D3D11_TEXTURE3D_DESC d{};
        d.Width = d.Height = d.Depth = data.size;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{ px.data(), (UINT)(data.size * 8), (UINT)(data.size * data.size * 8) };
        ComPtr<ID3D11Texture3D> t;
        if (SUCCEEDED(dev->CreateTexture3D(&d, &init, &t))) {
            dev->CreateShaderResourceView(t.Get(), nullptr, &lutSrv);
            lutSize = data.size;
        }
    }

    void Pass(ID3D11PixelShader* ps, UINT nrt, ID3D11RenderTargetView* const* rtvs, int w, int h) {
        D3D11_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(nrt, rtvs, nullptr);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->Draw(3, 0);
        ID3D11RenderTargetView* none[3] = {};
        ctx->OMSetRenderTargets(3, none, nullptr);
    }

    void Bind(UINT slot, ID3D11ShaderResourceView* srv) { ctx->PSSetShaderResources(slot, 1, &srv); }
    void Unbind() { ID3D11ShaderResourceView* n[13] = {}; ctx->PSSetShaderResources(0, 13, n); }

    void Upload(const Constants& c) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(cbuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, &c, sizeof(c));
            ctx->Unmap(cbuf.Get(), 0);
        }
    }

    void DumpBackbuffer(const std::wstring& path) {
        D3D11_TEXTURE2D_DESC d{};
        backTex->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> st;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) return;
        ctx->CopyResource(st.Get(), backTex.Get());
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))) {
            frame->SetSize(d.Width, d.Height);
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            frame->SetPixelFormat(&fmt);
            frame->WritePixels(d.Height, m.RowPitch, m.RowPitch * d.Height, (BYTE*)m.pData);
            frame->Commit();
            enc->Commit();
            LogLine("Dumped frame to %s", Narrow(path).c_str());
        }
        ctx->Unmap(st.Get(), 0);
    }

    // Bring our copy up to date while we still own the frame: only the moved/dirty rectangles.
    void CopyChanged(const DXGI_OUTDUPL_FRAME_INFO& fi) {
        EnsureCap();
        auto full = [&] { ctx->CopyResource(cap.Get(), duplTex.Get()); capFilled = true; };
        if (!capFilled || fi.TotalMetadataBufferSize == 0) { full(); return; }
        meta.resize(fi.TotalMetadataBufferSize);
        std::vector<D3D11_BOX> boxes;
        UINT used = 0;
        if (FAILED(dupl->GetFrameMoveRects((UINT)meta.size(), (DXGI_OUTDUPL_MOVE_RECT*)meta.data(), &used))) { full(); return; }
        auto* mv = (DXGI_OUTDUPL_MOVE_RECT*)meta.data();
        for (UINT i = 0; i < used / sizeof(DXGI_OUTDUPL_MOVE_RECT); ++i) {
            const RECT& d = mv[i].DestinationRect;
            boxes.push_back({ (UINT)d.left, (UINT)d.top, 0, (UINT)d.right, (UINT)d.bottom, 1 });
        }
        if (FAILED(dupl->GetFrameDirtyRects((UINT)meta.size(), (RECT*)meta.data(), &used))) { full(); return; }
        auto* dr = (RECT*)meta.data();
        for (UINT i = 0; i < used / sizeof(RECT); ++i)
            boxes.push_back({ (UINT)dr[i].left, (UINT)dr[i].top, 0, (UINT)dr[i].right, (UINT)dr[i].bottom, 1 });
        double area = 0;
        for (auto& b : boxes) area += double(b.right - b.left) * double(b.bottom - b.top);
        if (area > 0.6 * width * height) { full(); return; }
        for (auto& b : boxes) {
            if (b.right <= b.left || b.bottom <= b.top) continue;
            b.right = std::min<UINT>(b.right, width); b.bottom = std::min<UINT>(b.bottom, height);
            ctx->CopySubresourceRegion(cap.Get(), 0, b.left, b.top, 0, duplTex.Get(), 0, &b);
        }
    }

    Result Frame() {
        // Only wait for the swap chain if we actually presented last time; otherwise the
        // duplication wait below is what paces us (and sleeps when the screen is still).
        if (presentedLast && waitable) WaitForSingleObjectEx(waitable, 100, TRUE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (owner->stop_) return Quit;

        EffectParams p;
        std::wstring dump;
        unsigned version;
        {
            std::lock_guard<std::mutex> lk(owner->mu_);
            p = owner->params_;
            version = owner->paramsVersion_;
            dump.swap(owner->dumpPath_);
        }
        if (p.monitor != monitorIndex) {
            auto mons = EnumerateMonitors();
            if (std::clamp(p.monitor, 0, std::max(0, (int)mons.size() - 1)) != monitorIndex) return Restart;
        }

        int mode = (p.blurOn && p.strength > 0) ? std::clamp(p.blurMode, 1, 3) : 0;
        float s = std::clamp(p.strength, 0.0f, 100.0f) / 100.0f;
        int n = 2 + (int)std::lround(s * (kMaxBlend - 2));
        int quality = (p.quality && p.blurQuality > 0) ? 1 : 0;
        fpsCap = p.fpsCap > 0 ? p.fpsCap : 0;
        // Only limit when the cap is meaningfully below the display's refresh.
        bool capped = fpsCap > 0 && fpsCap < refresh - 1;
        bool paramsChanged = version != seenVersion;
        seenVersion = version;

        // Has the picture settled? Then there's nothing to draw until the desktop changes.
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dtNominal = 1.0f / std::max(30, refresh);
        float tau = 0.004f + 0.32f * std::pow(s, 1.7f);
        bool converged;
        if (mode == 1) converged = std::pow(std::exp(-dtNominal / tau), (float)framesSinceNew) < 1e-3f;
        else if (mode == 2) converged = framesSinceNew >= n;
        else if (mode == 3) converged = sharpDrawn;   // smooth: once motion stops, draw one sharp frame
        else converged = framesSinceNew >= 1;
        // Smooth mode: no new frame for ~45 ms means things stopped moving.
        bool stillNow = mode == 3 && haveFrame && float(now.QuadPart - lastNewT.QuadPart) / float(qpf.QuadPart) > 0.045f;
        bool grainTick = p.visualsOn && p.grain > 0 &&
            float(now.QuadPart - lastGrainT.QuadPart) / float(qpf.QuadPart) >= 1.0f / 24.0f;
        bool idle = haveFrame && converged && !paramsChanged && !resetAccum && !owner->raise_ && dump.empty();

        // Release the frame we held, right before asking for the next one (as the docs advise).
        if (holding) { dupl->ReleaseFrame(); holding = false; }
        UINT timeout = !haveFrame ? 500 : (idle ? (p.visualsOn && p.grain > 0 ? 42 : 250) : 0);
        if (deferred && capped) {
            // Sleep (inside the duplication wait) until the next allowed frame instead of spinning.
            float wait = float(nextDueT.QuadPart - now.QuadPart) / float(qpf.QuadPart) - 0.5f / std::max(30, refresh);
            timeout = (UINT)std::clamp((int)std::ceil(wait * 1000.0f), 1, 50);
        }
        if (mode == 3 && !idle && !sharpDrawn && !paramsChanged && !resetAccum) timeout = 16;   // wake to draw the still frame
        if (GetEnvironmentVariableW(L"IONIZATION_DEBUG_CAPTUREONLY", nullptr, 0) > 0) timeout = 20;   // benchmarking only
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dupl->AcquireNextFrame(timeout, &fi, &res);
        bool newFrame = false;
        if (hr == S_OK) {
            holding = true;
            if (fi.LastPresentTime.QuadPart != 0 || !haveFrame) {
                res.As(&duplTex);
                newFrame = true;
                ++captures;
                CopyChanged(fi);          // while we own the frame
            }
        } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
            LogLine("AcquireNextFrame 0x%08x", hr);
            return Lost;
        }
        ++loops;

        if (owner->raise_.exchange(false) && hwnd) {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            PostMessageW(owner->notify_, WM_APP_RENDERER, 3, 0);
        }

        if (!Configure(mode, quality, n)) return Lost;
        if (newFrame) {
            framesSinceNew = 0;
            haveFrame = true;
            lastNewT = now;
            if (mode == 3) sharpDrawn = false;
        } else if (haveFrame) {
            ++framesSinceNew;
        }
        if (!haveFrame) return Continue;

        static const bool captureOnly = GetEnvironmentVariableW(L"IONIZATION_DEBUG_CAPTUREONLY", nullptr, 0) > 0;   // benchmarking only
        if (captureOnly) { presentedLast = false; QueryPerformanceCounter(&now); UpdateStats(now); return Continue; }
        QueryPerformanceCounter(&now);
        bool mustDraw = newFrame || deferred || paramsChanged || resetAccum || grainTick || !dump.empty() || !shown ||
            (mode == 3 ? (!sharpDrawn && stillNow) : !converged);
        if (!mustDraw) { presentedLast = false; UpdateStats(now); return Continue; }
        bool fresh = newFrame || deferredNew;   // a captured frame not drawn yet
        if (capped && shown && dump.empty() && !paramsChanged) {
            // Draw on the vsync nearest the frame's due time (e.g. 120 on 144 Hz: mostly every vsync, every 5th skipped).
            float toDue = float(nextDueT.QuadPart - now.QuadPart) / float(qpf.QuadPart);
            if (toDue > 0.5f / std::max(30, refresh)) {
                deferred = true;
                deferredNew = fresh;
                presentedLast = false;
                UpdateStats(now);
                return Continue;
            }
        }
        deferred = deferredNew = false;
        if (grainTick || fresh) lastGrainT = now;

        float dt = std::clamp(float(now.QuadPart - lastT.QuadPart) / float(qpf.QuadPart), 1.0f / 500, 1.0f / 20);
        lastT = now;

        Constants c{};
        c.decay = (mode == 1 && !resetAccum) ? std::exp(-dt / tau) : 0.0f;
        c.opacity = std::clamp(p.opacity, 0.0f, 100.0f) / 100.0f;
        // Smooth: how much of the frame-to-frame motion the "shutter" smears (CapCut-like at ~1).
        bool sharpFrame = mode == 3 && !fresh && stillNow;
        c.shutter = (mode == 3 && !sharpFrame) ? 0.2f + 1.3f * s : 0.0f;
        // Smooth-blur quality: sample spacing (px), tap limit and dither.
        static const float kStep[4] = { 2.5f, 1.75f, 1.25f, 0.8f }, kTaps[4] = { 8, 16, 32, 64 };
        int bq = std::clamp(p.blurQuality, 0, 3);
        c.tapStep = kStep[bq]; c.maxTaps = kTaps[bq]; c.jitter = bq <= 1 ? 1.0f : 0.0f;
        bool vis = p.visualsOn;
        c.exposure = vis ? p.exposure : 0;
        c.contrast = vis ? p.contrast / 100.0f : 0;
        c.saturation = vis ? p.saturation / 100.0f : 1;
        c.vibrance = vis ? p.vibrance / 100.0f : 0;
        c.temperature = vis ? p.temperature / 100.0f : 0;
        c.tint = vis ? p.tint / 100.0f : 0;
        c.warmth = vis ? p.warmth / 100.0f : 0;
        c.vignette = vis ? p.vignette / 100.0f : 0;
        c.vignetteSoft = p.vignetteSoft / 100.0f;
        c.grain = vis ? p.grain / 100.0f : 0;
        c.bloom = vis ? p.bloom / 100.0f : 0;
        c.bloomThresh = 0.35f + 0.6f * (p.bloomThreshold / 100.0f);
        c.sharpen = vis ? p.sharpen / 100.0f : 0;
        c.chroma = vis ? p.chroma / 100.0f : 0;
        if (vis && p.lutId != "neutral" && p.lutIntensity > 0) EnsureLut(p.lutId);
        c.useLut = (vis && lutSrv && p.lutId == lutLoaded && p.lutIntensity > 0) ? 1.0f : 0.0f;
        c.lutMix = p.lutIntensity / 100.0f;
        c.lutSize = (float)std::max(lutSize, 2);
        c.frameSeed = (float)(frameNo % 4096);
        c.texelX = 1.0f / width; c.texelY = 1.0f / height;
        c.aspect = (float)width / (float)height;
        ++frameNo;

        // Blend bookkeeping: ring holds N+1 slots so the slot being written never equals the one leaving.
        int slots = (int)ring.size();
        int writeSlot = 0, oldestSlot = 0;
        if (mode == 2) {
            if (resetAccum) {
                float z[4] = { 0, 0, 0, 0 };
                ctx->ClearRenderTargetView(sum[0].rtv.Get(), z);
                ctx->ClearRenderTargetView(sum[1].rtv.Get(), z);
                head = 0; filled = 0;
            }
            writeSlot = head;
            oldestSlot = (head + 1) % slots;     // written N frames ago
            c.blendSub = filled >= n ? 1.0f : 0.0f;
            c.blendCount = (float)std::min(filled + 1, n);
        }
        resetAccum = false;

        int qi = qFrame % 4;
        ctx->Begin(qDisjoint[qi].Get());
        ctx->End(qStart[qi].Get());

        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ID3D11SamplerState* smp = sampler.Get();
        ctx->PSSetSamplers(0, 1, &smp);
        ID3D11Buffer* cb = cbuf.Get();
        ctx->PSSetConstantBuffers(0, 1, &cb);

        ID3D11ShaderResourceView* curSrv = capSrv.Get();
        if (!curSrv) return Continue;

        // Smooth: build the luma pyramid for this frame and estimate motion against the last one.
        if (mode == 3 && fresh) {
            int lc = lumaCur, lp = lumaCur ^ 1;
            Constants m = c;
            m.srcTexelX = 1.0f / width; m.srcTexelY = 1.0f / height;
            Upload(m);
            Bind(8, curSrv);
            ID3D11RenderTargetView* r = lumaQ[lc].rtv.Get();
            Pass(psLumaColor.Get(), 1, &r, lumaQ[lc].w, lumaQ[lc].h);
            Unbind();
            m.srcTexelX = 1.0f / lumaQ[lc].w; m.srcTexelY = 1.0f / lumaQ[lc].h;
            Upload(m);
            Bind(8, lumaQ[lc].srv.Get()); r = lumaS[lc].rtv.Get();
            Pass(psLumaDown.Get(), 1, &r, lumaS[lc].w, lumaS[lc].h);
            Unbind();
            if (havePrevLuma && !resetAccum) {
                Bind(9, lumaS[lc].srv.Get()); Bind(10, lumaS[lp].srv.Get()); r = vecS.rtv.Get();
                Pass(psMotionCoarse.Get(), 1, &r, vecS.w, vecS.h);
                Unbind();
                Bind(9, lumaQ[lc].srv.Get()); Bind(10, lumaQ[lp].srv.Get()); Bind(11, vecS.srv.Get()); r = vecQ.rtv.Get();
                Pass(psMotionFine.Get(), 1, &r, vecQ.w, vecQ.h);
                Unbind();
            } else {
                float z[4] = { 0, 0, 0, 0 };
                ctx->ClearRenderTargetView(vecQ.rtv.Get(), z);
            }
            havePrevLuma = true;
            lumaCur ^= 1;
        }

        // Bloom from the current frame at quarter resolution (tiny).
        EnsureBloom(c.bloom > 0);
        if (c.bloom > 0) {
            Constants b = c;
            b.srcTexelX = 1.0f / width; b.srcTexelY = 1.0f / height;
            Upload(b);
            Bind(8, curSrv);
            ID3D11RenderTargetView* r = bloomA.rtv.Get();
            Pass(psDown.Get(), 1, &r, bloomA.w, bloomA.h);
            Unbind();
            b.srcTexelX = 1.0f / bloomA.w; b.srcTexelY = 1.0f / bloomA.h;
            Upload(b);
            Bind(8, bloomA.srv.Get()); r = bloomB.rtv.Get();
            Pass(psBlurH.Get(), 1, &r, bloomB.w, bloomB.h);
            Unbind();
            Bind(8, bloomB.srv.Get()); r = bloomA.rtv.Get();
            Pass(psBlurV.Get(), 1, &r, bloomA.w, bloomA.h);
            Unbind();
        }
        Upload(c);

        int prev = cur ^ 1;
        ID3D11ShaderResourceView* bloomSrv = c.bloom > 0 ? bloomA.srv.Get() : nullptr;
        if (mode == 0) {
            Bind(0, curSrv); Bind(6, bloomSrv); Bind(7, lutSrv.Get());
            ID3D11RenderTargetView* r = backRtv.Get();
            Pass(psComp[0].Get(), 1, &r, width, height);
        } else if (mode == 3) {
            Bind(0, curSrv); Bind(12, vecQ.srv.Get()); Bind(6, bloomSrv); Bind(7, lutSrv.Get());
            ID3D11RenderTargetView* r = backRtv.Get();
            Pass(psComp[5].Get(), 1, &r, width, height);
            if (sharpFrame || c.shutter == 0) sharpDrawn = true;
        } else if (mode == 1 && quality == 1) {
            Bind(0, curSrv); Bind(1, acc[prev].srv.Get()); Bind(6, bloomSrv); Bind(7, lutSrv.Get());
            ID3D11RenderTargetView* r[2] = { backRtv.Get(), acc[cur].rtv.Get() };
            Pass(psComp[1].Get(), 2, r, width, height);
        } else if (mode == 1) {
            Bind(0, curSrv); Bind(1, acc[prev].srv.Get());
            ID3D11RenderTargetView* r[2] = { acc[cur].rtv.Get(), curH.rtv.Get() };
            Pass(psTrailHalf.Get(), 2, r, halfW, halfH);
            Unbind();
            Bind(0, curSrv); Bind(4, acc[cur].srv.Get()); Bind(5, curH.srv.Get()); Bind(6, bloomSrv); Bind(7, lutSrv.Get());
            ID3D11RenderTargetView* o = backRtv.Get();
            Pass(psComp[2].Get(), 1, &o, width, height);
        } else if (quality == 1) {
            Bind(0, curSrv); Bind(2, sum[prev].srv.Get()); Bind(3, ring[oldestSlot].srv.Get()); Bind(6, bloomSrv); Bind(7, lutSrv.Get());
            ID3D11RenderTargetView* r[3] = { backRtv.Get(), sum[cur].rtv.Get(), ring[writeSlot].rtv.Get() };
            Pass(psComp[3].Get(), 3, r, width, height);
        } else {
            Bind(0, curSrv); Bind(2, sum[prev].srv.Get()); Bind(3, ring[oldestSlot].srv.Get());
            ID3D11RenderTargetView* r[3] = { sum[cur].rtv.Get(), ring[writeSlot].rtv.Get(), avgH.rtv.Get() };
            Pass(psBlendHalf.Get(), 3, r, halfW, halfH);
            Unbind();
            Bind(0, curSrv); Bind(4, avgH.srv.Get()); Bind(5, ring[writeSlot].srv.Get()); Bind(6, bloomSrv); Bind(7, lutSrv.Get());
            ID3D11RenderTargetView* o = backRtv.Get();
            Pass(psComp[4].Get(), 1, &o, width, height);
        }
        Unbind();
        if (mode == 1 || mode == 2) cur ^= 1;
        if (mode == 2) { head = (head + 1) % slots; filled = std::min(filled + 1, n); }

        ctx->End(qEnd[qi].Get());
        ctx->End(qDisjoint[qi].Get());
        ++qFrame;
        if (qFrame >= 4) {
            int ri = qFrame % 4;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 t0 = 0, t1 = 0;
            if (ctx->GetData(qDisjoint[ri].Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                ctx->GetData(qStart[ri].Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                ctx->GetData(qEnd[ri].Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                !dj.Disjoint && dj.Frequency) {
                float ms = float(double(t1 - t0) * 1000.0 / double(dj.Frequency));
                gpuMs = gpuMs == 0 ? ms : gpuMs * 0.9f + ms * 0.1f;
            }
        }

        if (!dump.empty()) {
            LogLine("dump: mode=%d quality=%d n=%d decay=%.3f vis=%d", mode, quality, n, c.decay, (int)vis);
            DumpBackbuffer(dump);
        }

        hr = swap->Present(1, 0);
        if (FAILED(hr)) { LogLine("Present 0x%08x", hr); return Lost; }
        presentedLast = true;
        if (fpsCap > 0) {
            LONGLONG period = qpf.QuadPart / fpsCap, slot = qpf.QuadPart / std::max(30, refresh);
            nextDueT.QuadPart = std::max(nextDueT.QuadPart + period, now.QuadPart + period - slot);
        }
        ++presents;
        if (!shown) {
            dcomp->Commit();
            static const bool noShow = GetEnvironmentVariableW(L"IONIZATION_DEBUG_NOSHOW", nullptr, 0) > 0;   // benchmarking only
            if (!noShow) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            shown = true;
            PostMessageW(owner->notify_, WM_APP_RENDERER, 1, 0);
        }
        UpdateStats(now);
        return Continue;
    }

    void UpdateStats(LARGE_INTEGER now) {
        float since = float(now.QuadPart - statT.QuadPart) / float(qpf.QuadPart);
        if (since < 0.5f) return;
        std::lock_guard<std::mutex> lk(owner->mu_);
        owner->stats_.running = true;
        owner->stats_.fps = presents / since;
        owner->stats_.captureFps = captures / since;
        int target = fpsCap > 0 ? std::min(fpsCap, refresh) : refresh;
        owner->stats_.skippedFps = std::max(0.0f, target - presents / since);
        owner->stats_.gpuMs = presents ? gpuMs : 0.0f;
        owner->stats_.width = width;
        owner->stats_.height = height;
        owner->stats_.refresh = refresh;
        owner->stats_.quality = cfgQuality < 0 ? 0 : cfgQuality;
        static const bool logStats = GetEnvironmentVariableW(L"IONIZATION_STATS_LOG", nullptr, 0) > 0;   // benchmarking only
        if (logStats) LogLine("stats: presents/s=%.1f captures/s=%.1f loops/s=%.1f gpu=%.2fms", presents / since, captures / since, loops / since, gpuMs);
        presents = captures = loops = 0;
        statT = now;
    }
};

void Renderer::ThreadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    int failures = 0;
    while (!stop_) {
        int monitor;
        { std::lock_guard<std::mutex> lk(mu_); monitor = params_.monitor; }
        RenderSession::Result r = RenderSession::Lost;
        {
            RenderSession s;
            s.owner = this;
            if (s.Init(monitor)) {
                failures = 0;
                while ((r = s.Frame()) == RenderSession::Continue) {}
            } else {
                ++failures;
            }
        }
        if (r == RenderSession::Quit) break;
        if (r == RenderSession::Lost || failures) {
            PostMessageW(notify_, WM_APP_RENDERER, 2, 0);
            for (int i = 0; i < 10 && !stop_; ++i) Sleep(failures > 5 ? 100 : 30);
        }
    }
    CoUninitialize();
}
