#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include "json.h"

// Desktop widgets: weather, world clocks, to-do, calendar and PC stats. Each one is a
// small Direct2D window that sits just above the wallpaper (or on top), can be dragged
// anywhere, and draws only when its content changes.
class Widgets {
public:
    ~Widgets();
    // `all` is the full settings object (reads widgets.*, display.monitor and clock.*).
    void Apply(const Json& all, bool inGame);
    void Reassert();
    void RefreshWeather();
    bool noExclude = false;
    std::function<void(const std::string& type, int x, int y)> onMoved;   // monitor-relative px
    std::function<void(int index)> onTodoToggle;
    std::function<void(const std::string& type)> onHide;
    std::function<void()> onOpenSettings;
    struct Impl;
private:
    Impl* impl_ = nullptr;
};
