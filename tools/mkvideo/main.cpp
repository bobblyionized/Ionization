// Test tool (not shipped): writes a short H.264 MP4 with Windows' built-in encoder, for
// checking the video wallpaper path. Usage: mkvideo out.mp4
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { fwprintf(stderr, L"usage: mkvideo out.mp4\n"); return 2; }
    const UINT32 W = 1280, H = 720, FPS = 30, FRAMES = 90;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION))) return 1;
    ComPtr<IMFSinkWriter> sink;
    if (FAILED(MFCreateSinkWriterFromURL(argv[1], nullptr, nullptr, &sink))) { fwprintf(stderr, L"sink failed\n"); return 1; }
    ComPtr<IMFMediaType> out, in;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, 4000000);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, W, H);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, FPS, 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD stream = 0;
    if (FAILED(sink->AddStream(out.Get(), &stream))) { fwprintf(stderr, L"stream failed\n"); return 1; }
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, W, H);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, FPS, 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(sink->SetInputMediaType(stream, in.Get(), nullptr))) { fwprintf(stderr, L"input type failed\n"); return 1; }
    sink->BeginWriting();
    const LONGLONG dur = 10000000LL / FPS;
    for (UINT32 f = 0; f < FRAMES; ++f) {
        ComPtr<IMFMediaBuffer> buf;
        MFCreateMemoryBuffer(W * H * 4, &buf);
        BYTE* p = nullptr;
        buf->Lock(&p, nullptr, nullptr);
        float t = f / (float)FPS;
        for (UINT32 y = 0; y < H; ++y) {
            BYTE* row = p + (size_t)(H - 1 - y) * W * 4;   // RGB32 is bottom-up
            for (UINT32 x = 0; x < W; ++x) {
                float u = x / (float)W, v = y / (float)H;
                float band = 0.5f + 0.5f * std::sin((u * 6.0f + t * 2.0f) + std::sin(v * 4.0f + t));
                row[x * 4 + 0] = (BYTE)(60 + 160 * band);          // B
                row[x * 4 + 1] = (BYTE)(30 + 60 * v);              // G
                row[x * 4 + 2] = (BYTE)(40 + 120 * (1 - band));    // R
                row[x * 4 + 3] = 255;
            }
        }
        // a white square sweeping across, so motion is obvious
        UINT32 sx = (UINT32)((W - 120) * (0.5f + 0.5f * std::sin(t * 2.0f)));
        for (UINT32 y = H / 2 - 60; y < H / 2 + 60; ++y) for (UINT32 x = sx; x < sx + 120; ++x) {
            BYTE* px = p + (size_t)(H - 1 - y) * W * 4 + x * 4; px[0] = px[1] = px[2] = 245;
        }
        buf->Unlock();
        buf->SetCurrentLength(W * H * 4);
        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(buf.Get());
        s->SetSampleTime(f * dur);
        s->SetSampleDuration(dur);
        if (FAILED(sink->WriteSample(stream, s.Get()))) { fwprintf(stderr, L"write failed\n"); return 1; }
    }
    HRESULT hr = sink->Finalize();
    MFShutdown();
    wprintf(L"%s: %s\n", argv[1], SUCCEEDED(hr) ? L"ok" : L"finalize failed");
    return SUCCEEDED(hr) ? 0 : 1;
}
