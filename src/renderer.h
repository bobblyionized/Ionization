#pragma once
#include <windows.h>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>

// Raw UI values (same units as the panel sliders).
struct EffectParams {
    bool blurOn = true;
    int blurMode = 1;            // 1 = trail (EMA), 2 = blend (frame average)
    float strength = 45;         // 0..100
    bool visualsOn = false;
    float exposure = 0;          // -2..2 EV
    float contrast = 0;          // -100..100
    float saturation = 100;      // 0..200
    float vibrance = 0;          // -100..100
    float temperature = 0;       // -100..100
    float tint = 0;              // -100..100
    std::string lutId = "neutral";
    float lutIntensity = 100;    // 0..100
    float vignette = 0, vignetteSoft = 50, grain = 0, bloom = 0, bloomThreshold = 70;
    float sharpen = 0, chroma = 0, warmth = 0;
    int monitor = 0;

    bool VisualsActive() const;
    bool NeedsOverlay() const { return (blurOn && strength > 0) || VisualsActive(); }
};

struct RenderStats {
    bool running = false;
    float fps = 0, captureFps = 0, gpuMs = 0;
    int width = 0, height = 0, refresh = 0;
};

class Renderer {
public:
    ~Renderer() { Stop(); }
    void Start(HWND notify);
    void Stop();
    bool IsRunning() const { return thread_.joinable(); }
    void SetParams(const EffectParams& p);
    RenderStats Stats();
    HWND Overlay() const { return overlay_.load(); }
    void RequestDump(const std::wstring& path);
    bool feedbackTest = false;   // --no-exclude: overlay is NOT excluded from capture

private:
    void ThreadMain();
    std::thread thread_;
    std::atomic<bool> stop_{ false };
    std::atomic<HWND> overlay_{ nullptr };
    HWND notify_ = nullptr;
    std::mutex mu_;
    EffectParams params_;
    unsigned paramsVersion_ = 0;
    RenderStats stats_;
    std::wstring dumpPath_;
    friend struct RenderSession;
};
