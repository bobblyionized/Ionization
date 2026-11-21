#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <functional>

struct DockPin {
    std::wstring launch;   // .lnk path, exe path or shell:AppsFolder\<AUMID>
    std::wstring key;      // identity used to match running windows
    std::wstring name;
};

struct DockSettings {
    bool enabled = false;
    std::string mode = "hover";   // hover | always
    float size = 48;              // icon size (DIP)
    float magnify = 60;           // 0..100 -> up to 2x
    bool hideTaskbar = true;
    bool showClock = true;
    int monitor = 0;
    std::vector<DockPin> pins;
};

// The Windows taskbar is hidden/restored through these (also used by the crash handler).
namespace Taskbar {
    void Hide();
    void Restore();
    bool IsHidden();
    UINT OriginalState();
    void SetRecovered(bool hidden, UINT original);   // from settings after a crash
    void ShowTemporarily(bool show);
}

// A macOS-style floating dock: pinned + running apps, magnification, Start and a system cluster.
class Dock {
public:
    ~Dock();
    void Apply(const DockSettings& s, bool inGame);
    void OnTaskbarCreated();
    static std::vector<DockPin> ImportTaskbarPins();
    std::function<void(const std::vector<DockPin>&)> onPinsChanged;
    std::function<void()> onTaskbarStateChanged;
    struct Impl;
private:
    Impl* impl_ = nullptr;
};
