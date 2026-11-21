#pragma once
#include <windows.h>
#include <string>
#include <functional>

class ClipboardHistory;

struct IslandSettings {
    bool enabled = false;
    std::string shape = "island";   // island (floating) | notch (attached to the top edge)
    std::string theme = "black";    // black | graphite | glass
    float size = 100;               // %
    bool hidden = false;            // stay out of sight until the cursor touches the top edge
    bool volumeOsd = true;
    bool alwaysOnTop = false;       // stay in front of everything, even games and snapped windows
    int monitor = 0;
};

// A Dynamic-Island-style pill at the top of the screen: now playing, controls, timers,
// mic/camera indicators, a color picker and a message line other programs can write to.
class Island {
public:
    ~Island();
    void Apply(const IslandSettings& s, bool inGame);
    void ShowMessage(const std::wstring& text, unsigned color = 0, int ms = 5000);
    void ToggleCard();          // Ctrl+Alt+N
    void ToggleMic();           // Ctrl+Alt+M
    void PickColor();           // Ctrl+Alt+P
    void OpenCard(int tab);     // 0 now playing, 1 controls, 2 timer, 3 clipboard, 4 shelf
    void SetClipboard(ClipboardHistory* c);
    void ClipboardChanged();
    void Reassert();            // stay above the blur overlay
    HWND Hwnd() const;
    bool noExclude = false;     // debug: let screen capture see the island
    struct Impl;
private:
    Impl* impl_ = nullptr;
};
