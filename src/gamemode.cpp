#include "gamemode.h"
#include "common.h"
#include <shellapi.h>
#include <algorithm>
#include <cwctype>

std::string GameMode::ExeOfWindow(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return {};
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD n = (DWORD)std::size(buf);
    std::wstring name;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
        name.assign(buf, n);
        size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
        for (auto& c : name) c = (wchar_t)towlower(c);
    }
    CloseHandle(p);
    return Narrow(name);
}

static bool IsShellWindow(HWND h) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    static const wchar_t* shell[] = { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow", L"TopLevelWindowForOverflowXamlIsland",
        L"NotifyIconOverflowWindow", L"ForegroundStaging", L"MultitaskingViewFrame", L"TaskSwitcherWnd" };
    for (auto s : shell) if (!wcscmp(cls, s)) return true;
    return false;
}

static bool CoversMonitor(HWND h) {
    // A maximized window with a title bar covers the monitor when the taskbar is hidden
    // (e.g. with the Ionization dock): that's a normal app, not a fullscreen game.
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) return false;
    RECT r;
    if (!GetWindowRect(h, &r)) return false;
    HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(m, &mi)) return false;
    const RECT& b = mi.rcMonitor;
    return r.left <= b.left && r.top <= b.top && r.right >= b.right && r.bottom >= b.bottom;
}

bool GameMode::Tick() {
    GameStatus next;
    HWND fg = GetForegroundWindow();
    bool decided = false;
    if (fg) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId() || IsShellWindow(fg)) {
            // Our panel or the shell (Start, Alt+Tab, taskbar): keep the current state.
            return false;
        }
        std::string exe = ExeOfWindow(fg);
        if (!exe.empty()) lastForeign_ = exe;
        if (enabled_) {
            for (auto& r : rules_) {
                if (r.exe == exe) { next.inGame = true; next.keep = r.keep; next.exe = exe; next.reason = "game list"; decided = true; break; }
            }
            if (!decided) {
                QUERY_USER_NOTIFICATION_STATE q{};
                // Only exclusive fullscreen: QUNS_BUSY also fires for any topmost screen-sized
                // window (including overlays), so it can't tell a game from Ionization itself.
                bool d3dFull = SUCCEEDED(SHQueryUserNotificationState(&q)) && q == QUNS_RUNNING_D3D_FULL_SCREEN;
                if (d3dFull || CoversMonitor(fg)) {
                    next.inGame = true;
                    next.exe = exe;
                    next.reason = q == QUNS_RUNNING_D3D_FULL_SCREEN ? "exclusive fullscreen" : "fullscreen";
                }
            }
        }
    }
    if (!next.inGame && status_.inGame) {
        // Leave only after 1.5 s without a game (avoids flapping on quick focus changes).
        if (++missTicks_ < 3) return false;
    }
    missTicks_ = 0;
    bool changed = next.inGame != status_.inGame || next.keep != status_.keep || next.exe != status_.exe;
    status_ = next;
    return changed;
}
