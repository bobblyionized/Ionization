// fpsbench: a tiny borderless-fullscreen D3D11 "game" for measuring what Ionization costs.
// Each frame does tunable shader math plus a full-resolution read/write (like a real game's
// bandwidth), then presents. Prints average FPS and 1% lows.
//   fpsbench.exe [--seconds 10] [--warmup 2] [--load 48] [--vsync 0|1] [--out result.txt]
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

static const char* kHlsl = R"(
cbuffer C : register(b0) { float t; float load; float2 inv; };
Texture2D prev : register(t0);
SamplerState s : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V VS(uint id : SV_VertexID) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }
float4 PSScene(V i) : SV_Target {
    float3 c = prev.SampleLevel(s, i.uv + float2(sin(t) * 0.002, 0), 0).rgb * 0.92;
    float2 p = i.uv * 6.0 + t * 0.3;
    float a = 0;
    [loop] for (int k = 0; k < (int)load; k++) { p = float2(p.x * 1.01 + sin(p.y + k), p.y * 0.99 + cos(p.x - k)); a += sin(p.x * p.y) * 0.02; }
    float x = frac(i.uv.x - t * 0.25);
    float bar = smoothstep(0.02, 0.0, abs(x - 0.5));
    return float4(c * 0.85 + float3(0.1 + a * 0.2, 0.2 + bar, 0.35) * 0.15 + bar * float3(0.8, 1, 0.2) * 0.2, 1);
}
float4 PSBlit(V i) : SV_Target { return prev.SampleLevel(s, i.uv, 0); }
)";

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_ESCAPE) PostQuitMessage(0);
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

static std::wstring Arg(int argc, wchar_t** argv, const wchar_t* n, const wchar_t* d) {
    for (int i = 1; i + 1 < argc; ++i) if (!wcscmp(argv[i], n)) return argv[i + 1];
    return d;
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    double seconds = _wtof(Arg(argc, argv, L"--seconds", L"10").c_str());
    double warmup = _wtof(Arg(argc, argv, L"--warmup", L"2").c_str());
    float load = (float)_wtof(Arg(argc, argv, L"--load", L"48").c_str());
    int vsync = _wtoi(Arg(argc, argv, L"--vsync", L"0").c_str());
    std::wstring out = Arg(argc, argv, L"--out", L"");
    bool hidden = false;
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], L"--hidden")) hidden = true;

    HMONITOR mon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    int W = mi.rcMonitor.right - mi.rcMonitor.left, H = mi.rcMonitor.bottom - mi.rcMonitor.top;

    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"fpsbench";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND hwnd;
    if (hidden) {
        // Behind every other window, never activated: measures GPU/memory contention without
        // taking over the screen. Renders at full monitor resolution like the fullscreen run.
        hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"fpsbench", L"fpsbench", WS_POPUP, mi.rcMonitor.left, mi.rcMonitor.top, W, H,
            nullptr, nullptr, wc.hInstance, nullptr);
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        hwnd = CreateWindowExW(0, L"fpsbench", L"fpsbench", WS_POPUP | WS_VISIBLE, mi.rcMonitor.left, mi.rcMonitor.top, W, H,
            nullptr, nullptr, wc.hInstance, nullptr);
        SetForegroundWindow(hwnd);
    }

    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return 1;
    ComPtr<IDXGIDevice> dx; dev.As(&dx); ComPtr<IDXGIAdapter> ad; dx->GetAdapter(&ad);
    ComPtr<IDXGIFactory2> f; ad->GetParent(IID_PPV_ARGS(&f));
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = W; sd.Height = H; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc;
    if (FAILED(f->CreateSwapChainForHwnd(dev.Get(), hwnd, &sd, nullptr, nullptr, &sc))) return 2;
    ComPtr<ID3D11Texture2D> bb; sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    ComPtr<ID3D11RenderTargetView> bbRtv; dev->CreateRenderTargetView(bb.Get(), nullptr, &bbRtv);

    // Ping-pong scene targets (RGBA16F, full res) = game-like memory traffic.
    ComPtr<ID3D11Texture2D> st[2]; ComPtr<ID3D11RenderTargetView> sr[2]; ComPtr<ID3D11ShaderResourceView> ss[2];
    for (int i = 0; i < 2; ++i) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        dev->CreateTexture2D(&d, nullptr, &st[i]); dev->CreateRenderTargetView(st[i].Get(), nullptr, &sr[i]); dev->CreateShaderResourceView(st[i].Get(), nullptr, &ss[i]);
    }
    auto compile = [&](const char* e, const char* t) { ComPtr<ID3DBlob> b, err; D3DCompile(kHlsl, strlen(kHlsl), nullptr, nullptr, nullptr, e, t, 0, 0, &b, &err); return b; };
    auto vb = compile("VS", "vs_5_0"), pb = compile("PSScene", "ps_5_0"), bl = compile("PSBlit", "ps_5_0");
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps, psb;
    dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps);
    dev->CreatePixelShader(bl->GetBufferPointer(), bl->GetBufferSize(), nullptr, &psb);
    D3D11_SAMPLER_DESC smp{}; smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> sampler; dev->CreateSamplerState(&smp, &sampler);
    struct { float t, load, ix, iy; } cbData{ 0, load, 1.0f / W, 1.0f / H };
    D3D11_BUFFER_DESC bd{ sizeof(cbData), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    ComPtr<ID3D11Buffer> cb; dev->CreateBuffer(&bd, nullptr, &cb);

    LARGE_INTEGER qpf, t0, last; QueryPerformanceFrequency(&qpf); QueryPerformanceCounter(&t0); last = t0;
    std::vector<double> frames;
    int cur = 0;
    MSG msg{};
    bool quit = false;
    while (!quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) quit = true; TranslateMessage(&msg); DispatchMessageW(&msg); }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double el = double(now.QuadPart - t0.QuadPart) / qpf.QuadPart;
        if (el > warmup + seconds) break;
        cbData.t = (float)el; ctx->UpdateSubresource(cb.Get(), 0, nullptr, &cbData, 0, 0);
        D3D11_VIEWPORT vp{ 0, 0, (float)W, (float)H, 0, 1 }; ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ID3D11Buffer* cbs = cb.Get(); ctx->PSSetConstantBuffers(0, 1, &cbs);
        ID3D11SamplerState* sp = sampler.Get(); ctx->PSSetSamplers(0, 1, &sp);
        ID3D11ShaderResourceView* nul = nullptr;
        ctx->PSSetShaderResources(0, 1, &nul);
        ctx->OMSetRenderTargets(1, sr[cur].GetAddressOf(), nullptr);
        ID3D11ShaderResourceView* in = ss[cur ^ 1].Get(); ctx->PSSetShaderResources(0, 1, &in);
        ctx->PSSetShader(ps.Get(), nullptr, 0); ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &nul);
        ctx->OMSetRenderTargets(1, bbRtv.GetAddressOf(), nullptr);
        in = ss[cur].Get(); ctx->PSSetShaderResources(0, 1, &in);
        ctx->PSSetShader(psb.Get(), nullptr, 0); ctx->Draw(3, 0);
        sc->Present(vsync ? 1 : 0, 0);
        cur ^= 1;
        QueryPerformanceCounter(&now);
        double ft = double(now.QuadPart - last.QuadPart) / qpf.QuadPart; last = now;
        if (el > warmup) frames.push_back(ft);
    }
    DestroyWindow(hwnd);
    if (frames.empty()) return 3;
    double total = 0; for (double x : frames) total += x;
    std::vector<double> sorted = frames; std::sort(sorted.begin(), sorted.end(), std::greater<double>());
    size_t n1 = std::max<size_t>(1, sorted.size() / 100);
    double worst = 0; for (size_t i = 0; i < n1; ++i) worst += sorted[i];
    double avg = frames.size() / total, low1 = n1 / worst;
    char line[256];
    snprintf(line, sizeof(line), "frames=%zu avg_fps=%.1f low1_fps=%.1f load=%.0f vsync=%d res=%dx%d", frames.size(), avg, low1, load, vsync, W, H);
    printf("%s\n", line);
    if (!out.empty()) { FILE* fp = _wfopen(out.c_str(), L"wb"); if (fp) { fputs(line, fp); fclose(fp); } }
    return 0;
}
