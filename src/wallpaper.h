#pragma once
#include <windows.h>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>

struct WallpaperSettings {
    bool enabled = false;
    std::string source = "shader";      // shader | image | video
    std::string shader = "aurora";      // aurora | flow | grid | ink | mesh
    float speed = 100;                  // % (shaders)
    float dim = 0;                      // % darkening
    int fps = 30;                       // 15 | 30 | 60
    std::wstring image, video;
    bool pan = true;                    // slow drift on still images
    bool allDisplays = true;
    int monitor = 0;
    bool pauseBattery = true;
};

// Live wallpaper drawn in a window that sits behind the desktop icons (a child of the
// desktop's Progman/WorkerW). It runs on its own thread and only draws while the desktop
// can actually be seen: games, maximized/fullscreen windows, the lock screen and battery
// saver all pause it. The Windows wallpaper setting is never touched.
class Wallpaper {
public:
    ~Wallpaper() { Stop(); }
    void Apply(const WallpaperSettings& s, bool inGame);
    void OnExplorerRestart();          // desktop windows were recreated
    std::string Status();              // short text for the panel
    void RequestDump(const std::wstring& png) { std::lock_guard<std::mutex> lk(mu_); dump_ = png; }

private:
    void Stop();
    void ThreadMain();
    std::thread thread_;
    std::atomic<bool> stop_{ false };
    std::mutex mu_;
    WallpaperSettings s_;
    bool inGame_ = false;
    unsigned version_ = 0;
    std::atomic<bool> rehost_{ false };
    std::string status_ = "Off";
    std::wstring dump_;
    friend struct WallpaperSession;
};
