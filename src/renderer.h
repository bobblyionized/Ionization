#pragma once
#include <windows.h>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>

// Raw UI values (same units as the panel sliders).
struct EffectParams {
    bool blurOn = true;
    int blurMode = 3;            // 1 = trail (EMA), 2 = blend (frame average), 3 = smooth (optical flow)
    float opacity = 100;         // 0..100: blur strength against the sharp frame
    float strength = 45;         // 0..100
    int quality = 1;             // 0 = forced efficient (games that keep effects), 1 = follow blurQuality
    int blurQuality = 2;         // 0 low (half-res trails, 8 taps), 1 medium, 2 high, 3 ultra (64 taps)
    int fpsCap = 120;            // max overlay frames per second; 0 = as fast as the display refreshes
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
    float fps = 0, captureFps = 0, gpuMs = 0, skippedFps = 0;
    int width = 0, height = 0, refresh = 0, quality = 0;
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
    // Ask the render thread to put the overlay back on top (it owns the window).
    void RequestRaise() { raise_ = true; }
    bool feedbackTest = false;   // --no-exclude: overlay is NOT excluded from capture

private:
    void ThreadMain();
    std::thread thread_;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> raise_{ false };
    std::atomic<HWND> overlay_{ nullptr };
    HWND notify_ = nullptr;
    std::mutex mu_;
    EffectParams params_;
    unsigned paramsVersion_ = 0;
    RenderStats stats_;
    std::wstring dumpPath_;
    friend struct RenderSession;
};
