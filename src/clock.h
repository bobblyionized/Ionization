#pragma once
#include <windows.h>
#include <string>
#include <functional>

struct ClockSettings {
    bool enabled = false;
    std::string style = "volt";      // volt | mono | minimal
    bool hour24 = true, seconds = true, date = true, backdrop = true;
    float size = 100;                // 50..200 (%)
    float opacity = 100;             // 10..100 (%)
    std::string layer = "top";       // top | desktop
    std::string corner = "tr";       // tl | tr | bl | br | tc | custom
    int x = 0, y = 0;                // custom position (physical px, relative to monitor top-left)
    int monitor = 0;
};

class ClockOverlay {
public:
    ~ClockOverlay();
    void Apply(const ClockSettings& s);
    void SetEditing(bool editing);
    bool Editing() const { return editing_; }
    HWND Hwnd() const { return hwnd_; }
    // Called when the user finishes dragging the clock in edit mode.
    std::function<void(int x, int y)> onMoved;
    void Tick();
    // Re-apply z-order (topmost above the blur overlay, or just above the desktop).
    void Reassert();

private:
    friend LRESULT CALLBACK ClockProc(HWND, UINT, WPARAM, LPARAM);
    bool EnsureDevice();
    void Destroy();
    void Layout();
    void Render();
    void ArmTimer();

    ClockSettings s_;
    bool editing_ = false;
    HWND hwnd_ = nullptr;
    int w_ = 0, h_ = 0;
    float dpiScale_ = 1;
    struct Impl;
    Impl* impl_ = nullptr;
};
