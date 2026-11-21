#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <set>
#include "json.h"

#define WM_APP_MACRO (WM_APP + 20)   // wParam: 1 trigger down, 2 trigger up, 3 status changed; lParam: macro index

struct MacroStep {
    std::string kind;   // kd ku md mu mv rm wh tx dl
    int vk = 0, sc = 0, button = 0, dx = 0, dy = 0, delta = 0;
    bool ext = false, horizontal = false;
    double x = 0, y = 0;
    std::wstring text;
    int wait = 0;       // ms before this step
};

struct Macro {
    std::string id, name;
    bool enabled = true;
    int trigVk = 0, trigMods = 0, trigMouse = 0;   // trigMouse: 0 none, 3 middle, 4 X1, 5 X2
    std::string trigMode = "press";                // press | toggle | hold
    bool block = true;
    std::string scope;                             // exe name or "" for everywhere
    std::string repeat = "once";                   // once | times | forever
    int count = 1;
    double speed = 1.0, humanize = 0.0;
    std::vector<MacroStep> steps;
    bool NeedsHook() const;
};

// Records and plays keyboard/mouse macros. Low-level hooks run on their own thread and
// only while something needs them (recording, playing, or a trigger that hotkeys can't do).
class MacroEngine {
public:
    ~MacroEngine();
    void Init(HWND notify);
    void SetMacros(const Json& list);
    const Json& MacrosJson() const { return json_; }
    bool StartRecording(bool mouseMoves, bool relative);
    Json StopRecording();
    int RecordedCount() const { return (int)recCount_.load(); }
    bool Recording() const { return recording_; }
    void Play(int index, bool loopUntilStopped = false);
    void StopAll();
    // Main-window plumbing.
    bool OnHotkey(int id);                 // returns true if the id belongs to macros
    void OnTrigger(int index, bool down);  // from the hook (posted)
    Json StatusJson();
    static const int kHotkeyBase = 1000, kStopHotkey = 999;

    // Hook entry points (static thunks call these).
    LRESULT OnKeyboard(int code, WPARAM w, LPARAM l);
    LRESULT OnMouse(int code, WPARAM w, LPARAM l);

private:
    void RegisterTriggers();
    void UpdateHooks();
    void HookThread();
    void PlayThread(int index, bool loopUntilStopped, unsigned token);
    bool ScopeOk(const Macro& m);
    bool ModsDown(int mods);

    HWND notify_ = nullptr;
    Json json_;
    std::vector<Macro> macros_;
    std::mutex mu_;

    // hooks
    std::thread hookThread_;
    std::atomic<DWORD> hookThreadId_{ 0 };
    std::atomic<bool> hooksOn_{ false };
    std::atomic<bool> suspendHooks_{ false };

    // recording
    struct RawEvent { int type; int vk, sc, button, delta; bool ext, down, horizontal; POINT pt; LONGLONG t; };
    std::atomic<bool> recording_{ false };
    bool recMouse_ = true, recRelative_ = false;
    std::vector<RawEvent> rec_;
    std::atomic<size_t> recCount_{ 0 };
    std::mutex recMu_;

    // playback
    std::vector<std::thread> players_;
    std::atomic<unsigned> playToken_{ 0 };
    std::vector<int> playing_;                 // index -> active players
    std::vector<char> held_;                   // index -> trigger held (hold mode)
    std::set<int> registered_;
};
