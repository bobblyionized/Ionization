#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct GameRule {
    std::string exe;   // lowercase file name, e.g. "robloxplayerbeta.exe"
    bool keep = false; // true = keep effects running in this game
};

struct GameStatus {
    bool inGame = false;
    bool keep = false;
    std::string exe;
    std::string reason;  // "game list", "fullscreen", "exclusive fullscreen"
};

// Watches the foreground app and decides whether a game is being played.
class GameMode {
public:
    void Configure(bool enabled, const std::vector<GameRule>& rules) { enabled_ = enabled; rules_ = rules; }
    // Re-evaluates; returns true when the status changed.
    bool Tick();
    const GameStatus& Status() const { return status_; }
    // Last foreground app that is neither ours nor the shell (for "Add current app").
    const std::string& LastForeignExe() const { return lastForeign_; }
    static std::string ExeOfWindow(HWND h);

private:
    bool enabled_ = true;
    std::vector<GameRule> rules_;
    GameStatus status_;
    std::string lastForeign_;
    int missTicks_ = 0;
};
