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
    float decay, ringCount, blurMode, lutMix;
    float exposure, contrast, saturation, vibrance;
    float temperature, tint, warmth, lutSize;
    float vignette, vignetteSoft, grain, frameSeed;
    float bloom, bloomThresh, sharpen, chroma;
    float texelX, texelY, useLut, aspect;
    float srcTexelX, srcTexelY, ringHead, ringCap;
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
    ComPtr<ID3D11PixelShader> psAccum, psDown, psBlurH, psBlurV, psGrade;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> cbuf;

    ComPtr<ID3D11Texture2D> cap;
    ComPtr<ID3D11ShaderResourceView> capSrv;
    RT accum[2];
    RT bloomA, bloomB;
    ComPtr<ID3D11Texture2D> ring;
    ComPtr<ID3D11ShaderResourceView> ringSrv;
    int ringCap = 0, ringHead = 0, ringFilled = 0;
    ComPtr<ID3D11ShaderResourceView> lutSrv;
    std::string lutLoaded = "neutral";
    int lutSize = 0;

    ComPtr<ID3D11Query> qDisjoint[4], qStart[4], qEnd[4];
    int qFrame = 0;

    int width = 0, height = 0, refresh = 60, monitorIndex = 0;
    int cur = 0;
    bool haveFrame = false, shown = false, resetAccum = true;
    int lastMode = 0;
    LARGE_INTEGER qpf{}, lastT{}, statT{}, raiseT{};
    int presents = 0, captures = 0;
    float gpuMs = 0;
    unsigned frameNo = 0;

    enum Result { Continue, Lost, Restart, Quit };

    ~RenderSession() {
        if (ctx) { ctx->ClearState(); ctx->Flush(); }
        if (dupl) dupl.Reset();
        if (hwnd) { owner->overlay_ = nullptr; DestroyWindow(hwnd); }
    }

    bool Compile(const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
        ComPtr<ID3DBlob> err;
        HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "ionization.hlsl", nullptr, nullptr,
            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
        if (FAILED(hr)) {
            LogLine("Shader %s failed: %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        return true;
    }
    bool MakePS(const char* entry, ComPtr<ID3D11PixelShader>& ps) {
        ComPtr<ID3DBlob> b;
        return Compile(entry, "ps_5_0", b) && SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps));
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

        // Overlay window covering the monitor, click-through, excluded from capture.
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
            mi.rect.left, mi.rect.top, mi.rect.right - mi.rect.left, mi.rect.bottom - mi.rect.top,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) return false;
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        if (!owner->feedbackTest) SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

        ComPtr<IDXGIDevice> dxgiDev;
        dev.As(&dxgiDev);
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

        // Shaders + state.
        ComPtr<ID3DBlob> vsb;
        if (!Compile("VS", "vs_5_0", vsb)) return false;
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
        if (!MakePS("PSAccum", psAccum) || !MakePS("PSBloomDown", psDown) || !MakePS("PSBlurH", psBlurH) ||
            !MakePS("PSBlurV", psBlurV) || !MakePS("PSGrade", psGrade)) return false;

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

        D3D11_TEXTURE2D_DESC cd{};
        cd.Width = width; cd.Height = height; cd.MipLevels = 1; cd.ArraySize = 1;
        cd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; cd.SampleDesc.Count = 1;
        cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&cd, nullptr, &cap))) return false;
        dev->CreateShaderResourceView(cap.Get(), nullptr, &capSrv);

        if (!MakeRT(dev.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, accum[0]) ||
            !MakeRT(dev.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, accum[1]) ||
            !MakeRT(dev.Get(), std::max(1, width / 4), std::max(1, height / 4), DXGI_FORMAT_R16G16B16A16_FLOAT, bloomA) ||
            !MakeRT(dev.Get(), std::max(1, width / 4), std::max(1, height / 4), DXGI_FORMAT_R16G16B16A16_FLOAT, bloomB))
            return false;

        for (int i = 0; i < 4; ++i) {
            D3D11_QUERY_DESC q{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            dev->CreateQuery(&q, &qDisjoint[i]);
            q.Query = D3D11_QUERY_TIMESTAMP;
            dev->CreateQuery(&q, &qStart[i]);
            dev->CreateQuery(&q, &qEnd[i]);
        }

        QueryPerformanceFrequency(&qpf);
        QueryPerformanceCounter(&lastT);
        statT = lastT;
        owner->overlay_ = hwnd;
        LogLine("Session up: monitor %d %dx%d @%dHz", monitorIndex, width, height, refresh);
        return true;
    }

    bool EnsureRing(int n) {
        if (ringCap >= n) return true;
        ring.Reset(); ringSrv.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = width; d.Height = height; d.MipLevels = 1; d.ArraySize = n;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &ring))) { ringCap = 0; return false; }
        dev->CreateShaderResourceView(ring.Get(), nullptr, &ringSrv);
        ringCap = n; ringHead = 0; ringFilled = 0;
        return true;
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

    void Pass(ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, int w, int h) {
        D3D11_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->Draw(3, 0);
        ID3D11RenderTargetView* none = nullptr;
        ctx->OMSetRenderTargets(1, &none, nullptr);
    }

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

    Result Frame() {
        if (waitable) WaitForSingleObjectEx(waitable, 100, TRUE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (owner->stop_) return Quit;

        EffectParams p;
        std::wstring dump;
        {
            std::lock_guard<std::mutex> lk(owner->mu_);
            p = owner->params_;
            dump.swap(owner->dumpPath_);
        }
        if (p.monitor != monitorIndex) {
            auto mons = EnumerateMonitors();
            if (std::clamp(p.monitor, 0, std::max(0, (int)mons.size() - 1)) != monitorIndex) return Restart;
        }

        // Capture.
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dupl->AcquireNextFrame(haveFrame ? 0 : 200, &fi, &res);
        if (hr == S_OK) {
            if (fi.LastPresentTime.QuadPart != 0 || !haveFrame) {
                ComPtr<ID3D11Texture2D> tex;
                res.As(&tex);
                ctx->CopyResource(cap.Get(), tex.Get());
                haveFrame = true;
                ++captures;
            }
            dupl->ReleaseFrame();
        } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
            LogLine("AcquireNextFrame 0x%08x", hr);
            return Lost;
        }
        if (!haveFrame) return Continue;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = std::clamp(float(now.QuadPart - lastT.QuadPart) / float(qpf.QuadPart), 1.0f / 500, 1.0f / 20);
        lastT = now;

        // Blur parameters.
        int mode = (p.blurOn && p.strength > 0) ? std::clamp(p.blurMode, 1, 2) : 0;
        if (mode != lastMode) { resetAccum = true; ringFilled = 0; lastMode = mode; }
        float s = std::clamp(p.strength, 0.0f, 100.0f) / 100.0f;
        Constants c{};
        c.blurMode = (float)mode;
        if (mode == 1) {
            float tau = 0.004f + 0.32f * std::pow(s, 1.7f);
            c.decay = resetAccum ? 0.0f : std::exp(-dt / tau);
        } else if (mode == 2) {
            int n = 2 + (int)std::lround(s * 10);
            if (EnsureRing(12)) {
                ctx->CopySubresourceRegion(ring.Get(), D3D11CalcSubresource(0, ringHead, 1), 0, 0, 0, cap.Get(), 0, nullptr);
                ringHead = (ringHead + 1) % ringCap;
                ringFilled = std::min(ringFilled + 1, ringCap);
                // Average the newest min(n, filled) slices: walk back from head.
                c.ringCount = (float)std::min(n, ringFilled);
                c.ringHead = (float)ringHead;   // newest slice is ringHead - 1
                c.ringCap = (float)ringCap;
            } else {
                c.blurMode = 0;
            }
        }
        resetAccum = false;

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

        // Pass 1: accumulate into accum[cur].
        Upload(c);
        int prev = cur ^ 1;
        ID3D11ShaderResourceView* srvA[3] = { capSrv.Get(), accum[prev].srv.Get(), ringSrv.Get() };
        ctx->PSSetShaderResources(0, 3, srvA);
        Pass(psAccum.Get(), accum[cur].rtv.Get(), width, height);
        ID3D11ShaderResourceView* nulls[7] = {};
        ctx->PSSetShaderResources(0, 7, nulls);

        // Bloom passes (quarter res).
        if (c.bloom > 0) {
            Constants b = c;
            b.srcTexelX = 1.0f / width; b.srcTexelY = 1.0f / height;
            Upload(b);
            ID3D11ShaderResourceView* s0 = accum[cur].srv.Get();
            ctx->PSSetShaderResources(6, 1, &s0);
            Pass(psDown.Get(), bloomA.rtv.Get(), bloomA.w, bloomA.h);
            b.srcTexelX = 1.0f / bloomA.w; b.srcTexelY = 1.0f / bloomA.h;
            Upload(b);
            s0 = bloomA.srv.Get();
            ctx->PSSetShaderResources(6, 1, &s0);
            Pass(psBlurH.Get(), bloomB.rtv.Get(), bloomB.w, bloomB.h);
            s0 = bloomB.srv.Get();
            ctx->PSSetShaderResources(6, 1, &s0);
            Pass(psBlurV.Get(), bloomA.rtv.Get(), bloomA.w, bloomA.h);
            ctx->PSSetShaderResources(0, 7, nulls);
            Upload(c);
        }

        // Final grade into the backbuffer.
        ID3D11ShaderResourceView* srvG[3] = { accum[cur].srv.Get(), bloomA.srv.Get(), lutSrv.Get() };
        ctx->PSSetShaderResources(3, 3, srvG);
        Pass(psGrade.Get(), backRtv.Get(), width, height);
        ctx->PSSetShaderResources(0, 7, nulls);

        ctx->End(qEnd[qi].Get());
        ctx->End(qDisjoint[qi].Get());
        ++qFrame;
        if (qFrame >= 4) {
            int ri = qFrame % 4; // oldest
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
            LogLine("dump params: blur=%d mode=%d s=%.0f vis=%d exp=%.2f sat=%.0f vig=%.0f -> cMode=%.0f decay=%.3f cSat=%.2f",
                p.blurOn, p.blurMode, p.strength, p.visualsOn, p.exposure, p.saturation, p.vignette, c.blurMode, c.decay, c.saturation);
            DumpBackbuffer(dump);
        }

        hr = swap->Present(1, 0);
        if (FAILED(hr)) { LogLine("Present 0x%08x", hr); return Lost; }
        ++presents;
        if (!shown) {
            dcomp->Commit();
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            shown = true;
            PostMessageW(owner->notify_, WM_APP_RENDERER, 1, 0);
        }
        cur ^= 1;

        // Periodically re-assert topmost so other always-on-top windows are covered too;
        // the main thread then lifts the clock / sharp panel back above us.
        if (float(now.QuadPart - raiseT.QuadPart) / float(qpf.QuadPart) > 0.75f) {
            raiseT = now;
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            PostMessageW(owner->notify_, WM_APP_RENDERER, 3, 0);
        }

        float since = float(now.QuadPart - statT.QuadPart) / float(qpf.QuadPart);
        if (since >= 0.5f) {
            std::lock_guard<std::mutex> lk(owner->mu_);
            owner->stats_.running = true;
            owner->stats_.fps = presents / since;
            owner->stats_.captureFps = captures / since;
            owner->stats_.gpuMs = gpuMs;
            owner->stats_.width = width;
            owner->stats_.height = height;
            owner->stats_.refresh = refresh;
            presents = captures = 0;
            statT = now;
        }
        return Continue;
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
