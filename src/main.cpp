// Ionization: host process. Panel window (WebView2 UI), tray, hotkeys, settings,
// and wiring to the renderer (blur/grade overlay), clock overlay and updater.
#include "common.h"
#include "json.h"
#include "renderer.h"
#include "clock.h"
#include "lut.h"
#include "updater.h"
#include "gamemode.h"
#include "gpuload.h"
#include "memory.h"
#include "macro.h"
#include "dock.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <memory>

using namespace Microsoft::WRL;

namespace {

const wchar_t* kPanelClass = strstr(IONIZATION_VERSION, "dev") ? L"IonizationPanelDev" : L"IonizationPanel";
const wchar_t* kHost = L"https://ionization.local/";
enum { TIMER_STATS = 1, TIMER_SAVE, TIMER_UPDATE, TIMER_ZORDER, TIMER_DUMP, TIMER_EXIT, TIMER_SHOW,
       TIMER_GAME, TIMER_RAISECHECK, TIMER_UNLOAD, TIMER_MEM, TIMER_MACRO, TIMER_RECTEST };
enum { HK_BLUR = 1, HK_VISUALS, HK_CLOCK };
enum { IDM_SHOW = 100, IDM_BLUR, IDM_VISUALS, IDM_CLOCK, IDM_ADDGAME, IDM_QUIT };
#define WM_APP_SHOW (WM_APP + 10)

HWND g_hwnd = nullptr;
HICON g_icon = nullptr, g_iconSmall = nullptr;
Json g_settings;
Renderer g_renderer;
ClockOverlay g_clock;
std::string g_clockApplied;
ComPtr<ICoreWebView2Environment> g_env;
ComPtr<ICoreWebView2Controller> g_ctl;
ComPtr<ICoreWebView2> g_wv;
bool g_startHidden = false, g_navigated = false, g_quitting = false;
bool g_hotkeysOk[4] = {};
Json g_lastUpdate;
std::wstring g_dumpPath;
UINT g_taskbarCreated = 0;
bool g_panelTopmost = false;
std::string g_startView;
GameMode g_game;
GpuLoad g_gpuLoad;
MemoryOptimizer g_mem;
MacroEngine g_macros;
Dock g_dock;
std::string g_dockApplied;
ULONGLONG g_lastTrim = 0;
bool g_revealPending = false, g_raiseCheckPending = false;
HWINEVENTHOOK g_hookFg = nullptr, g_hookReorder = nullptr;
void CreateWebView();
void GameTick();
DWORD ForegroundPid();

// ------------------------------------------------------------------ settings

Json DefaultSettings() {
    return Json::parse(R"({
      "blur": {"enabled": true, "mode": "smooth", "strength": 45, "opacity": 100},
      "visuals": {"enabled": true},
      "color": {"exposure": 0, "contrast": 0, "saturation": 100, "vibrance": 0, "temperature": 0, "tint": 0},
      "lut": {"id": "neutral", "intensity": 100},
      "fx": {"vignette": 0, "vignetteSoft": 50, "grain": 0, "bloom": 0, "bloomThreshold": 70, "sharpen": 0, "chroma": 0, "warmth": 0},
      "clock": {"enabled": false, "style": "volt", "hour24": true, "seconds": true, "date": true, "backdrop": true,
                "size": 100, "opacity": 100, "layer": "top", "corner": "tr", "x": 0, "y": 0},
      "display": {"monitor": 0},
      "app": {"autoUpdate": true, "keepPanelSharp": false, "hotkeys": true, "trayHintShown": false},
      "dock": {"enabled": false, "mode": "hover", "size": 48, "magnify": 60, "hideTaskbar": true, "showClock": true,
               "pins": [], "imported": false, "tbHidden": false, "tbOrig": 0},
      "memory": {"lowPriority": false, "autoTrim": false, "autoTrimAt": 85, "gameBoost": true},
      "perf": {"gameMode": true, "quality": "full", "hideClockInGames": true,
               "games": [{"exe": "robloxplayerbeta.exe", "mode": "pause"}, {"exe": "hd-player.exe", "mode": "pause"},
                         {"exe": "minecraft.windows.exe", "mode": "pause"}, {"exe": "cs2.exe", "mode": "pause"},
                         {"exe": "valorant-win64-shipping.exe", "mode": "pause"}, {"exe": "fortniteclient-win64-shipping.exe", "mode": "pause"},
                         {"exe": "r5apex.exe", "mode": "pause"}, {"exe": "gta5.exe", "mode": "pause"}]},
      "profiles": [],
      "activeProfile": "Clean"
    })");
}

void DeepMerge(Json& base, const Json& over) {
    if (!over.isObj() || !base.isObj()) { base = over; return; }
    for (auto& kv : over.obj) {
        Json& slot = base[kv.first];
        if (slot.isObj() && kv.second.isObj()) DeepMerge(slot, kv.second);
        else slot = kv.second;
    }
}

std::wstring SettingsPath() { return AppDataDir() + L"\\settings.json"; }

void LoadSettings() {
    g_settings = DefaultSettings();
    std::string text;
    if (ReadFileBytes(SettingsPath(), text)) {
        bool ok = false;
        Json j = Json::parse(text, &ok);
        if (ok && j.isObj()) DeepMerge(g_settings, j);
    }
    // v1.1: Efficient (half-res) trails showed halos, and plain trails look like echoes at 60 Hz.
    // Move existing installs to the new Smooth mode and Full quality once; users can switch back.
    if (!g_settings.boolean("app.migratedSmooth", false)) {
        g_settings["app"]["migratedSmooth"] = true;
        if (g_settings.str("blur.mode", "smooth") == "trail") g_settings["blur"]["mode"] = "smooth";
        g_settings["perf"]["quality"] = "full";
    }
}

void SaveSettingsNow() { WriteFileBytes(SettingsPath(), g_settings.dump()); }
void SaveSoon() { SetTimer(g_hwnd, TIMER_SAVE, 400, nullptr); }

std::wstring MacrosPath() { return AppDataDir() + L"\\macros.json"; }
void LoadMacros() {
    std::string text;
    Json list = Json::array();
    if (ReadFileBytes(MacrosPath(), text)) {
        bool ok = false;
        Json j = Json::parse(text, &ok);
        if (ok && j.isArr()) list = j;
    }
    g_macros.SetMacros(list);
}
void SaveMacros() { WriteFileBytes(MacrosPath(), g_macros.MacrosJson().dump()); }

EffectParams ParamsFrom(const Json& s) {
    EffectParams p;
    p.blurOn = s.boolean("blur.enabled", true);
    std::string mode = s.str("blur.mode", "smooth");
    p.blurMode = mode == "blend" ? 2 : mode == "trail" ? 1 : 3;
    p.opacity = (float)s.num("blur.opacity", 100);
    p.strength = (float)s.num("blur.strength", 45);
    p.quality = s.str("perf.quality", "full") == "efficient" ? 0 : 1;
    p.visualsOn = s.boolean("visuals.enabled", true);
    p.exposure = (float)s.num("color.exposure", 0);
    p.contrast = (float)s.num("color.contrast", 0);
    p.saturation = (float)s.num("color.saturation", 100);
    p.vibrance = (float)s.num("color.vibrance", 0);
    p.temperature = (float)s.num("color.temperature", 0);
    p.tint = (float)s.num("color.tint", 0);
    p.lutId = s.str("lut.id", "neutral");
    p.lutIntensity = (float)s.num("lut.intensity", 100);
    p.vignette = (float)s.num("fx.vignette", 0);
    p.vignetteSoft = (float)s.num("fx.vignetteSoft", 50);
    p.grain = (float)s.num("fx.grain", 0);
    p.bloom = (float)s.num("fx.bloom", 0);
    p.bloomThreshold = (float)s.num("fx.bloomThreshold", 70);
    p.sharpen = (float)s.num("fx.sharpen", 0);
    p.chroma = (float)s.num("fx.chroma", 0);
    p.warmth = (float)s.num("fx.warmth", 0);
    p.monitor = (int)s.num("display.monitor", 0);
    return p;
}

ClockSettings ClockFrom(const Json& s) {
    ClockSettings c;
    c.enabled = s.boolean("clock.enabled", false);
    c.style = s.str("clock.style", "volt");
    c.hour24 = s.boolean("clock.hour24", true);
    c.seconds = s.boolean("clock.seconds", true);
    c.date = s.boolean("clock.date", true);
    c.backdrop = s.boolean("clock.backdrop", true);
    c.size = (float)s.num("clock.size", 100);
    c.opacity = (float)s.num("clock.opacity", 100);
    c.layer = s.str("clock.layer", "top");
    c.corner = s.str("clock.corner", "tr");
    c.x = (int)s.num("clock.x", 0);
    c.y = (int)s.num("clock.y", 0);
    c.monitor = (int)s.num("display.monitor", 0);
    return c;
}

// ------------------------------------------------------------------ bridge

void PostToUI(const Json& j) {
    if (g_wv) g_wv->PostWebMessageAsJson(Widen(j.dump()).c_str());
}

Json MonitorsJson() {
    Json arr = Json::array();
    for (auto& m : EnumerateMonitors()) {
        Json o = Json::object();
        o["name"] = Narrow(m.friendly);
        o["device"] = Narrow(m.device);
        o["width"] = (int)(m.rect.right - m.rect.left);
        o["height"] = (int)(m.rect.bottom - m.rect.top);
        o["refresh"] = m.refresh;
        o["primary"] = m.primary;
        arr.arr.push_back(o);
    }
    return arr;
}

Json LutsJson() {
    Json arr = Json::array();
    for (auto& l : ListLuts()) {
        Json o = Json::object();
        o["id"] = l.id; o["name"] = l.name; o["kind"] = l.kind; o["size"] = l.size;
        arr.arr.push_back(o);
    }
    return arr;
}

bool AutostartEnabled() {
    wchar_t buf[1024]; DWORD sz = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"Ionization",
        RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\" --tray";
        RegSetValueExW(k, L"Ionization", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"Ionization");
    }
    RegCloseKey(k);
}

Json HotkeysJson() {
    Json h = Json::object();
    h["blur"] = g_hotkeysOk[HK_BLUR];
    h["visuals"] = g_hotkeysOk[HK_VISUALS];
    h["clock"] = g_hotkeysOk[HK_CLOCK];
    return h;
}

Json GameJson() {
    const GameStatus& gs = g_game.Status();
    Json j = Json::object();
    j["inGame"] = gs.inGame;
    j["keep"] = gs.keep;
    j["exe"] = gs.exe;
    j["reason"] = gs.reason;
    j["lastExe"] = g_game.LastForeignExe();
    return j;
}

void SendGame() {
    Json j = Json::object();
    j["type"] = "game";
    j["value"] = GameJson();
    PostToUI(j);
}

void SendInit() {
    Json j = Json::object();
    j["type"] = "init";
    j["settings"] = g_settings;
    j["monitors"] = MonitorsJson();
    j["luts"] = LutsJson();
    j["version"] = IONIZATION_VERSION;
    j["dev"] = Updater::IsDevBuild();
    j["autostart"] = AutostartEnabled();
    j["hotkeys"] = HotkeysJson();
    j["clockEditing"] = g_clock.Editing();
    if (g_lastUpdate.isObj()) j["update"] = g_lastUpdate;
    if (!g_startView.empty()) j["view"] = g_startView;
    j["game"] = GameJson();
    j["focused"] = GetForegroundWindow() == g_hwnd;
    j["macros"] = g_macros.MacrosJson();
    j["macroStatus"] = g_macros.StatusJson();
    PostToUI(j);
}

void SendState() {
    Json j = Json::object();
    j["type"] = "state";
    j["settings"] = g_settings;
    j["clockEditing"] = g_clock.Editing();
    PostToUI(j);
}

void Toast(const std::string& text, const char* tone = "info") {
    Json j = Json::object();
    j["type"] = "toast"; j["text"] = text; j["tone"] = tone;
    PostToUI(j);
}

// ------------------------------------------------------------------ apply

void ApplyPanelSharp() {
    bool want = g_settings.boolean("app.keepPanelSharp", false) && g_renderer.IsRunning();
    if (want) {
        SetWindowDisplayAffinity(g_hwnd, WDA_EXCLUDEFROMCAPTURE);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        g_panelTopmost = true;
    } else if (g_panelTopmost) {
        SetWindowDisplayAffinity(g_hwnd, WDA_NONE);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        g_panelTopmost = false;
    }
}

void ApplyHotkeys() {
    for (int i = 1; i <= 3; ++i) { UnregisterHotKey(g_hwnd, i); g_hotkeysOk[i] = false; }
    if (!g_settings.boolean("app.hotkeys", true)) return;
    g_hotkeysOk[HK_BLUR] = RegisterHotKey(g_hwnd, HK_BLUR, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'B') != 0;
    g_hotkeysOk[HK_VISUALS] = RegisterHotKey(g_hwnd, HK_VISUALS, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'V') != 0;
    g_hotkeysOk[HK_CLOCK] = RegisterHotKey(g_hwnd, HK_CLOCK, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C') != 0;
}

void UpdateTrayTip() {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd; nid.uID = 1; nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    std::wstring tip = L"Ionization\nMotion blur ";
    tip += g_settings.boolean("blur.enabled", true) ? L"on" : L"off";
    tip += L", color ";
    tip += g_settings.boolean("visuals.enabled", true) ? L"on" : L"off";
    if (g_settings.boolean("clock.enabled", false)) tip += L", clock on";
    const GameStatus& gs = g_game.Status();
    if (gs.inGame) tip += gs.keep ? L"\nGame: effects kept (efficient)" : L"\nPaused for " + Widen(gs.exe);
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

std::vector<GameRule> GameRulesFrom(const Json& s) {
    std::vector<GameRule> rules;
    if (const Json* g = s.path("perf.games"); g && g->isArr()) {
        for (auto& e : g->arr) {
            GameRule r;
            r.exe = e.str("exe", "");
            for (auto& ch : r.exe) ch = (char)tolower((unsigned char)ch);
            r.keep = e.str("mode", "pause") == "keep";
            if (!r.exe.empty()) rules.push_back(r);
        }
    }
    return rules;
}

DockSettings DockFrom(const Json& s) {
    DockSettings d;
    d.enabled = s.boolean("dock.enabled", false);
    d.mode = s.str("dock.mode", "hover");
    d.size = (float)s.num("dock.size", 48);
    d.magnify = (float)s.num("dock.magnify", 60);
    d.hideTaskbar = s.boolean("dock.hideTaskbar", true);
    d.showClock = s.boolean("dock.showClock", true);
    d.monitor = (int)s.num("display.monitor", 0);
    if (const Json* pins = s.path("dock.pins"); pins && pins->isArr())
        for (auto& p : pins->arr) d.pins.push_back({ Widen(p.str("launch", "")), Widen(p.str("key", "")), Widen(p.str("name", "")) });
    return d;
}

Json PinsJson(const std::vector<DockPin>& pins) {
    Json arr = Json::array();
    for (auto& p : pins) {
        Json o = Json::object();
        o["launch"] = Narrow(p.launch); o["key"] = Narrow(p.key); o["name"] = Narrow(p.name);
        arr.arr.push_back(o);
    }
    return arr;
}

void ApplyDock() {
    // First time the dock is turned on: start from the apps pinned to the Windows taskbar.
    if (g_settings.boolean("dock.enabled", false) && !g_settings.boolean("dock.imported", false)) {
        g_settings["dock"]["pins"] = PinsJson(Dock::ImportTaskbarPins());
        g_settings["dock"]["imported"] = true;
        SaveSoon();
    }
    Json key = Json::object();
    if (const Json* d = g_settings.find("dock")) key["d"] = *d;
    key["g"] = g_game.Status().inGame;
    key["m"] = g_settings.num("display.monitor", 0);
    std::string k = key.dump();
    if (k == g_dockApplied) return;
    g_dockApplied = k;
    g_dock.Apply(DockFrom(g_settings), g_game.Status().inGame);
}

void ApplyMemory() {
    g_mem.SetLowPriority(g_settings.boolean("memory.lowPriority", false), ForegroundPid());
}

void ApplySettings() {
    g_game.Configure(g_settings.boolean("perf.gameMode", true), GameRulesFrom(g_settings));
    const GameStatus& gs = g_game.Status();
    EffectParams p = ParamsFrom(g_settings);
    if (gs.inGame && gs.keep) p.quality = 0;          // opted-in games always get the cheap path
    g_renderer.SetParams(p);
    // In a game, the overlay goes away completely so the game presents directly again.
    bool need = p.NeedsOverlay() && !(gs.inGame && !gs.keep);
    if (need && !g_renderer.IsRunning()) g_renderer.Start(g_hwnd);
    else if (!need && g_renderer.IsRunning()) g_renderer.Stop();

    ClockSettings cs = ClockFrom(g_settings);
    if (gs.inGame && g_settings.boolean("perf.hideClockInGames", true)) cs.enabled = false;
    Json clockKey = Json::object();
    if (const Json* c = g_settings.find("clock")) clockKey["c"] = *c;
    clockKey["m"] = g_settings.num("display.monitor", 0);
    clockKey["on"] = cs.enabled;
    std::string ck = clockKey.dump();
    if (ck != g_clockApplied) {
        g_clockApplied = ck;
        g_clock.Apply(cs);
    }
    ApplyPanelSharp();
    ApplyDock();
    UpdateTrayTip();
}

void ToggleSetting(const char* section, const char* key) {
    Json& sec = g_settings[section];
    sec[key] = !g_settings.boolean((std::string(section) + "." + key).c_str(), false);
    ApplySettings();
    SaveSoon();
    SendState();
}

// ------------------------------------------------------------------ panel window

void ResizeWebView();

void FocusPanel() {
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    if (g_ctl) { g_ctl->put_IsVisible(TRUE); ResizeWebView(); g_ctl->NotifyParentWindowPositionChanged(); }
}

void SetCloak(bool on) {
    BOOL v = on ? TRUE : FALSE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_CLOAK, &v, sizeof(v));
}

// The WebView is created lazily (and unloaded after the panel sits hidden), so the window is
// shown cloaked until the page reports its first painted frame: nobody ever sees it empty.
void ShowPanel() {
    KillTimer(g_hwnd, TIMER_UNLOAD);
    if (!g_ctl) {
        if (!g_revealPending) {
            g_revealPending = true;
            SetCloak(true);
            ShowWindow(g_hwnd, SW_SHOW);
            if (!g_env) CreateWebView();
        }
        return;
    }
    if (!IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, SW_SHOW);
    FocusPanel();
}

void RevealPanel() {
    if (!g_revealPending) return;
    g_revealPending = false;
    KillTimer(g_hwnd, TIMER_SHOW);
    SetCloak(false);
    FocusPanel();
}

void DestroyWebView() {
    if (g_ctl) g_ctl->Close();
    g_wv.Reset();
    g_ctl.Reset();
    g_env.Reset();
    g_navigated = false;
    LogLine("WebView unloaded (panel hidden)");
}

void HidePanel() {
    g_revealPending = false;
    ShowWindow(g_hwnd, SW_HIDE);
    if (g_ctl) g_ctl->put_IsVisible(FALSE);
    // Free the browser engine's memory if the panel stays closed for a minute.
    SetTimer(g_hwnd, TIMER_UNLOAD, 60 * 1000, nullptr);
    if (!g_settings.boolean("app.trayHintShown", false)) {
        g_settings["app"]["trayHintShown"] = true;
        SaveSoon();
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = g_hwnd; nid.uID = 1; nid.uFlags = NIF_INFO;
        wcscpy_s(nid.szInfoTitle, L"Ionization is still running");
        wcscpy_s(nid.szInfo, L"Effects stay on. Open the panel from the tray, or press Ctrl+Alt+B to toggle blur.");
        nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
        nid.hBalloonIcon = g_icon;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void AddTrayIcon() {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd; nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_iconSmall;
    wcscpy_s(nid.szTip, L"Ionization");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd; nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"Open Ionization");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_settings.boolean("blur.enabled", true) ? MF_CHECKED : 0), IDM_BLUR, L"Motion blur\tCtrl+Alt+B");
    AppendMenuW(m, MF_STRING | (g_settings.boolean("visuals.enabled", true) ? MF_CHECKED : 0), IDM_VISUALS, L"Color && effects\tCtrl+Alt+V");
    AppendMenuW(m, MF_STRING | (g_settings.boolean("clock.enabled", false) ? MF_CHECKED : 0), IDM_CLOCK, L"Clock\tCtrl+Alt+C");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::string last = g_game.LastForeignExe();
    if (!last.empty()) {
        std::wstring label = L"Pause effects in " + Widen(last);
        AppendMenuW(m, MF_STRING, IDM_ADDGAME, label.c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"Quit");
    SetMenuDefaultItem(m, IDM_SHOW, FALSE);
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
}

void Quit() {
    if (g_quitting) return;
    g_quitting = true;
    KillTimer(g_hwnd, TIMER_SAVE);
    SaveSettingsNow();
    g_renderer.Stop();
    g_clock.Apply(ClockSettings{});
    RemoveTrayIcon();
    g_mem.RestoreAll();
    g_macros.StopAll();
    g_dock.Apply(DockSettings{}, false);   // destroys the dock and gives the taskbar back
    g_settings["dock"]["tbHidden"] = false;
    SaveSettingsNow();
    if (g_hookFg) UnhookWinEvent(g_hookFg);
    if (g_hookReorder) UnhookWinEvent(g_hookReorder);
    if (g_ctl) g_ctl->Close();
    DestroyWindow(g_hwnd);
}

void AddGameRule(const std::string& exe, const char* mode) {
    if (exe.empty()) return;
    Json& games = g_settings["perf"]["games"];
    if (!games.isArr()) games = Json::array();
    for (auto& e : games.arr) if (e.str("exe", "") == exe) { e["mode"] = mode; goto done; }
    {
        Json r = Json::object();
        r["exe"] = exe; r["mode"] = mode;
        games.arr.push_back(r);
    }
done:
    g_game.Configure(g_settings.boolean("perf.gameMode", true), GameRulesFrom(g_settings));
    GameTick();
    SaveSoon();
    SendState();
}

DWORD ForegroundPid() {
    DWORD pid = 0;
    if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() ? 0 : pid;
}

Json MemJson(const MemorySnapshot& m) {
    Json j = Json::object();
    j["total"] = (double)m.total; j["available"] = (double)m.available;
    j["commit"] = (double)m.commit; j["commitLimit"] = (double)m.commitLimit; j["load"] = m.load;
    Json top = Json::array();
    for (auto& a : m.top) {
        Json o = Json::object();
        o["exe"] = a.exe; o["ws"] = (double)a.workingSet; o["n"] = a.instances;
        top.arr.push_back(o);
    }
    j["top"] = top;
    return j;
}

void ReportTrim(const TrimResult& r, const char* why) {
    g_lastTrim = GetTickCount64();
    Json j = Json::object();
    j["type"] = "memTrimmed";
    j["freed"] = (double)r.freed; j["trimmed"] = r.trimmed; j["why"] = why;
    PostToUI(j);
}

// Game Mode: called every 500 ms and on every foreground change.
void GameTick() {
    if (!g_game.Tick()) return;
    const GameStatus& gs = g_game.Status();
    if (gs.inGame) LogLine("Game detected: %s (%s) -> %s", gs.exe.c_str(), gs.reason.c_str(), gs.keep ? "keep effects" : "paused");
    if (gs.inGame && g_settings.boolean("memory.gameBoost", true)) {
        // Game boost: push background apps out of RAM so the game has room.
        ReportTrim(g_mem.TrimAll(ForegroundPid(), gs.exe), "game");
    }
    else LogLine("Game left, effects restored");
    ApplySettings();
    SendGame();
}

// Something changed z-order: only if a foreign window now sits above the overlay, ask it to rise.
void RaiseCheck() {
    HWND ov = g_renderer.Overlay();
    if (!ov || !g_renderer.IsRunning()) return;
    RECT orc; GetWindowRect(ov, &orc);
    DWORD me = GetCurrentProcessId();
    for (HWND h = GetWindow(ov, GW_HWNDPREV); h; h = GetWindow(h, GW_HWNDPREV)) {
        if (!IsWindowVisible(h)) continue;
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid == me) continue;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) continue;
        RECT r, x;
        if (GetWindowRect(h, &r) && IntersectRect(&x, &r, &orc)) { g_renderer.RequestRaise(); return; }
    }
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND) {
        GameTick();
        if (g_settings.boolean("memory.lowPriority", false)) ApplyMemory();
    }
    if (!g_raiseCheckPending && g_renderer.IsRunning()) {
        g_raiseCheckPending = true;
        SetTimer(g_hwnd, TIMER_RAISECHECK, 250, nullptr);   // coalesce bursts of z-order events
    }
}

void ResizeWebView() {
    if (!g_ctl) return;
    RECT rc; GetClientRect(g_hwnd, &rc);
    g_ctl->put_Bounds(rc);
}

void ImportLut() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = { { L"Cube LUT (*.cube)", L"*.cube" } };
    dlg->SetFileTypes(1, spec);
    dlg->SetTitle(L"Import a .cube LUT");
    if (FAILED(dlg->Show(g_hwnd))) return;
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    std::string id, err;
    bool ok = ImportCube(path, id, err);
    CoTaskMemFree(path);
    if (!ok) { Toast("Could not import LUT: " + err, "error"); return; }
    g_settings["lut"]["id"] = id;
    ApplySettings();
    SaveSoon();
    Json j = Json::object();
    j["type"] = "luts"; j["luts"] = LutsJson(); j["selected"] = id;
    PostToUI(j);
    SendState();
    Toast("LUT imported", "ok");
}

void HandleMessage(const Json& m) {
    std::string type = m.str("type", "");
    if (type == "ready") { SendInit(); return; }
    if (type == "painted") {
        KillTimer(g_hwnd, TIMER_SHOW);
        RevealPanel();
        return;
    }
    if (type == "settings") {
        if (const Json* v = m.find("value"); v && v->isObj()) {
            g_settings = DefaultSettings();
            if (const Json* games = v->path("perf.games")) g_settings["perf"]["games"] = *games;  // lists replace, not merge
            DeepMerge(g_settings, *v);
            ApplySettings();
            GameTick();
            ApplyMemory();
            SaveSoon();
        }
        return;
    }
    if (type == "dockImport") {
        g_settings["dock"]["pins"] = PinsJson(Dock::ImportTaskbarPins());
        g_settings["dock"]["imported"] = true;
        ApplySettings(); SaveSoon(); SendState();
        Toast("Pinned apps imported from the taskbar", "ok");
        return;
    }
    if (type == "macros") {
        if (const Json* v = m.find("value"); v && v->isArr()) { g_macros.SetMacros(*v); SaveMacros(); }
        return;
    }
    if (type == "macroRecord") {
        if (m.boolean("start", false)) {
            g_macros.StartRecording(m.boolean("mouse", true), m.boolean("relative", false));
            SetTimer(g_hwnd, TIMER_MACRO, 250, nullptr);
        } else {
            KillTimer(g_hwnd, TIMER_MACRO);
            Json j = Json::object(); j["type"] = "macroRecorded"; j["steps"] = g_macros.StopRecording(); PostToUI(j);
        }
        Json st = Json::object(); st["type"] = "macroStatus"; st["value"] = g_macros.StatusJson(); PostToUI(st);
        return;
    }
    if (type == "macroPlay") {
        std::string id = m.str("id", "");
        const Json& list = g_macros.MacrosJson();
        for (size_t i = 0; i < list.arr.size(); ++i) if (list.arr[i].str("id", "") == id) { g_macros.Play((int)i); break; }
        return;
    }
    if (type == "macroStop") { g_macros.StopAll(); return; }
    if (type == "memSnapshot") {
        Json j = Json::object(); j["type"] = "mem"; j["value"] = MemJson(g_mem.Snapshot(12)); PostToUI(j); return;
    }
    if (type == "memTrim") {
        // The app you were just using (before clicking the panel) is spared.
        ReportTrim(g_mem.TrimAll(0, g_game.LastForeignExe()), "manual");
        Json j = Json::object(); j["type"] = "mem"; j["value"] = MemJson(g_mem.Snapshot(12)); PostToUI(j); return;
    }
    if (type == "memTrimExe") {
        ReportTrim(g_mem.TrimExe(m.str("exe", "")), "app");
        Json j = Json::object(); j["type"] = "mem"; j["value"] = MemJson(g_mem.Snapshot(12)); PostToUI(j); return;
    }
    if (type == "addGame") { AddGameRule(m.str("exe", ""), m.str("mode", "pause") == "keep" ? "keep" : "pause"); return; }
    if (type == "drag") { ReleaseCapture(); SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0); return; }
    if (type == "minimize") { ShowWindow(g_hwnd, SW_MINIMIZE); return; }
    if (type == "close") { HidePanel(); return; }
    if (type == "quit") { Quit(); return; }
    if (type == "importLut") { ImportLut(); return; }
    if (type == "openLutFolder") {
        std::wstring d = AppDataDir() + L"\\luts";
        CreateDirectoryW(d.c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (type == "refreshLuts") {
        Json j = Json::object(); j["type"] = "luts"; j["luts"] = LutsJson(); PostToUI(j); return;
    }
    if (type == "refreshMonitors") {
        Json j = Json::object(); j["type"] = "monitors"; j["monitors"] = MonitorsJson(); PostToUI(j); return;
    }
    if (type == "checkUpdate") { Updater::CheckAsync(g_hwnd, true); return; }
    if (type == "clockEdit") {
        g_clock.SetEditing(m.boolean("value", false));
        Json j = Json::object(); j["type"] = "clockEditing"; j["value"] = g_clock.Editing(); PostToUI(j);
        return;
    }
    if (type == "autostart") {
        SetAutostart(m.boolean("value", false));
        Json j = Json::object(); j["type"] = "autostart"; j["value"] = AutostartEnabled(); PostToUI(j);
        return;
    }
    if (type == "openRepo") {
        ShellExecuteW(nullptr, L"open", L"https://github.com/bobblyionized/Ionization", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (type == "hotkeys") { ApplyHotkeys(); Json j = Json::object(); j["type"] = "hotkeys"; j["value"] = HotkeysJson(); PostToUI(j); return; }
}

const wchar_t* MimeFor(const std::wstring& path) {
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    if (!_wcsicmp(ext, L".html")) return L"text/html; charset=utf-8";
    if (!_wcsicmp(ext, L".css")) return L"text/css; charset=utf-8";
    if (!_wcsicmp(ext, L".js")) return L"text/javascript; charset=utf-8";
    if (!_wcsicmp(ext, L".svg")) return L"image/svg+xml";
    if (!_wcsicmp(ext, L".woff2")) return L"font/woff2";
    if (!_wcsicmp(ext, L".png")) return L"image/png";
    if (!_wcsicmp(ext, L".json")) return L"application/json";
    return L"application/octet-stream";
}

void ServeResource(ICoreWebView2WebResourceRequestedEventArgs* args) {
    ComPtr<ICoreWebView2WebResourceRequest> req;
    args->get_Request(&req);
    LPWSTR uriRaw = nullptr;
    req->get_Uri(&uriRaw);
    std::wstring uri = uriRaw ? uriRaw : L"";
    CoTaskMemFree(uriRaw);
    std::wstring rel = uri.substr(wcslen(kHost));
    size_t q = rel.find_first_of(L"?#");
    if (q != std::wstring::npos) rel = rel.substr(0, q);
    if (rel.empty()) rel = L"index.html";

    std::string bytes;
    bool found = false;
    wchar_t devDir[MAX_PATH];
    if (GetEnvironmentVariableW(L"IONIZATION_UI_DIR", devDir, MAX_PATH) && rel.find(L"..") == std::wstring::npos) {
        std::wstring p = std::wstring(devDir) + L"\\" + rel;
        for (auto& ch : p) if (ch == L'/') ch = L'\\';
        found = ReadFileBytes(p, bytes);
    }
    if (!found) {
        const void* data; DWORD size;
        if (LoadEmbedded(ResourceNameForPath(rel), &data, &size)) { bytes.assign((const char*)data, size); found = true; }
    }
    ComPtr<IStream> stream;
    ComPtr<ICoreWebView2WebResourceResponse> resp;
    if (found) {
        stream.Attach(SHCreateMemStream((const BYTE*)bytes.data(), (UINT)bytes.size()));
        std::wstring headers = std::wstring(L"Content-Type: ") + MimeFor(rel) + L"\r\nCache-Control: no-store";
        g_env->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &resp);
    } else {
        g_env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &resp);
    }
    args->put_Response(resp.Get());
}

void CreateWebView() {
    std::wstring udf = LocalDataDir() + L"\\WebView2";
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT res, ICoreWebView2Environment* env) -> HRESULT {
            LogLine("WebView2 env: 0x%08x", res);
            if (FAILED(res) || !env) {
                MessageBoxW(g_hwnd, L"Ionization needs the Microsoft Edge WebView2 Runtime, which ships with Windows 11.\nInstall it from microsoft.com/edge/webview2 and try again.",
                    L"Ionization", MB_ICONERROR);
                return S_OK;
            }
            g_env = env;
            env->CreateCoreWebView2Controller(g_hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [](HRESULT res, ICoreWebView2Controller* ctl) -> HRESULT {
                    LogLine("WebView2 controller: 0x%08x", res);
                    if (FAILED(res) || !ctl) return S_OK;
                    g_ctl = ctl;
                    ctl->get_CoreWebView2(&g_wv);
                    ComPtr<ICoreWebView2Controller2> ctl2;
                    if (SUCCEEDED(ctl->QueryInterface(IID_PPV_ARGS(&ctl2)))) ctl2->put_DefaultBackgroundColor({ 255, 7, 8, 10 });
                    ComPtr<ICoreWebView2Settings> st;
                    g_wv->get_Settings(&st);
                    bool dev = Updater::IsDevBuild();
                    st->put_AreDefaultContextMenusEnabled(dev);
                    st->put_AreDevToolsEnabled(dev);
                    st->put_IsStatusBarEnabled(FALSE);
                    st->put_IsZoomControlEnabled(FALSE);
                    ComPtr<ICoreWebView2Settings3> st3;
                    if (SUCCEEDED(st.As(&st3))) st3->put_AreBrowserAcceleratorKeysEnabled(dev);
                    ComPtr<ICoreWebView2Settings4> st4;
                    if (SUCCEEDED(st.As(&st4))) { st4->put_IsGeneralAutofillEnabled(FALSE); st4->put_IsPasswordAutosaveEnabled(FALSE); }

                    g_wv->AddWebResourceRequestedFilter((std::wstring(kHost) + L"*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                    EventRegistrationToken tok;
                    g_wv->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                            ServeResource(args);
                            return S_OK;
                        }).Get(), &tok);
                    g_wv->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            LPWSTR raw = nullptr;
                            if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
                                bool ok = false;
                                Json m = Json::parse(Narrow(raw), &ok);
                                CoTaskMemFree(raw);
                                if (ok) HandleMessage(m);
                            }
                            return S_OK;
                        }).Get(), &tok);
                    g_wv->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
                            BOOL ok = FALSE; a->get_IsSuccess(&ok);
                            COREWEBVIEW2_WEB_ERROR_STATUS st{}; a->get_WebErrorStatus(&st);
                            LogLine("Navigation completed ok=%d status=%d", ok, (int)st);
                            if (!g_navigated) {
                                g_navigated = true;
                                // Normally the page reports "painted"; this is only a fallback.
                                if (g_revealPending) SetTimer(g_hwnd, TIMER_SHOW, 2000, nullptr);
                            }
                            return S_OK;
                        }).Get(), &tok);
                    // Block navigation away from the app.
                    g_wv->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                            a->put_Handled(TRUE);
                            return S_OK;
                        }).Get(), &tok);
                    ResizeWebView();
                    g_wv->Navigate((std::wstring(kHost) + L"index.html").c_str());
                    return S_OK;
                }).Get());
            return S_OK;
        }).Get());
    if (FAILED(hr)) LogLine("CreateCoreWebView2Environment failed 0x%08x", hr);
}

void EnableDarkMenus() {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto setMode = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(2); // ForceDark
    if (flush) flush();
}

void OnUpdateStatus(std::string* text) {
    std::unique_ptr<std::string> own(text);
    Json j = Json::parse(*own);
    g_lastUpdate = j;
    Json msg = Json::object();
    msg["type"] = "update";
    msg["value"] = j;
    PostToUI(msg);
    if (j.str("status", "") == "restart") {
        SaveSettingsNow();
        bool visible = IsWindowVisible(g_hwnd) != 0;
        if (Updater::Relaunch(visible)) Quit();
    }
}

LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_taskbarCreated && g_taskbarCreated) { AddTrayIcon(); g_dock.OnTaskbarCreated(); return 0; }
    switch (m) {
    case WM_NCCALCSIZE:
        if (w) return 0; // client area = whole window (custom title bar in the UI)
        break;
    case WM_SIZE:
        ResizeWebView();
        if (g_ctl) g_ctl->put_IsVisible(w != SIZE_MINIMIZED && IsWindowVisible(h));
        return 0;
    case WM_ACTIVATE: {
        // The live preview only animates while the panel has focus.
        Json j = Json::object();
        j["type"] = "focus";
        j["value"] = LOWORD(w) != WA_INACTIVE;
        PostToUI(j);
        break;
    }
    case WM_MOVE:
        if (g_ctl) g_ctl->NotifyParentWindowPositionChanged();
        return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        HidePanel();
        return 0;
    case WM_HOTKEY:
        if (g_macros.OnHotkey((int)w)) return 0;
        if (w == HK_BLUR) ToggleSetting("blur", "enabled");
        else if (w == HK_VISUALS) ToggleSetting("visuals", "enabled");
        else if (w == HK_CLOCK) ToggleSetting("clock", "enabled");
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(l)) {
        case WM_LBUTTONUP: case NIN_SELECT: case NIN_KEYSELECT: ShowPanel(); break;
        case WM_RBUTTONUP: case WM_CONTEXTMENU: ShowTrayMenu(); break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDM_SHOW: ShowPanel(); break;
        case IDM_BLUR: ToggleSetting("blur", "enabled"); break;
        case IDM_VISUALS: ToggleSetting("visuals", "enabled"); break;
        case IDM_CLOCK: ToggleSetting("clock", "enabled"); break;
        case IDM_ADDGAME: AddGameRule(g_game.LastForeignExe(), "pause"); break;
        case IDM_QUIT: Quit(); break;
        }
        return 0;
    case WM_APP_SHOW:
        ShowPanel();
        return 0;
    case WM_APP_RENDERER:
        if (w == 3) {
            // Overlay just re-raised itself: lift the clock and (optionally) the panel above it.
            g_clock.Reassert();
            if (g_panelTopmost) SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else if (w == 1) {
            g_clock.Reassert();
            ApplyPanelSharp();
        }
        return 0;
    case WM_APP_UPDATE:
        OnUpdateStatus((std::string*)l);
        return 0;
    case WM_APP_MACRO:
        if (w == 1 || w == 2) g_macros.OnTrigger((int)l, w == 1);
        else {
            if (l == 2 && !g_macros.Recording()) KillTimer(h, TIMER_MACRO);   // emergency stop
            Json st = Json::object(); st["type"] = "macroStatus"; st["value"] = g_macros.StatusJson(); PostToUI(st);
            if (l == 2) Toast("All macros stopped", "ok");
        }
        return 0;
    case WM_TIMER:
        switch (w) {
        case TIMER_SAVE: KillTimer(h, TIMER_SAVE); SaveSettingsNow(); break;
        case TIMER_STATS: {
            if (!g_wv || !IsWindowVisible(h) || IsIconic(h)) break;   // nobody is looking
            RenderStats s = g_renderer.Stats();
            Json j = Json::object();
            j["type"] = "stats";
            j["running"] = g_renderer.IsRunning() && s.running;
            j["fps"] = (double)s.fps; j["captureFps"] = (double)s.captureFps; j["gpuMs"] = (double)s.gpuMs;
            j["skipped"] = (double)s.skippedFps; j["quality"] = s.quality;
            j["gpuLoad"] = g_gpuLoad.Sample();
            j["width"] = s.width; j["height"] = s.height; j["refresh"] = s.refresh;
            PostToUI(j);
            break;
        }
        case TIMER_GAME:
            GameTick();
            break;
        case TIMER_RECTEST: {
            KillTimer(h, TIMER_RECTEST);
            WriteFileBytes(LocalDataDir() + L"\\rectest.json", g_macros.StopRecording().dump());
            Quit();
            break;
        }
        case TIMER_MACRO: {
            Json st = Json::object(); st["type"] = "macroStatus"; st["value"] = g_macros.StatusJson(); PostToUI(st);
            break;
        }
        case TIMER_MEM: {
            ApplyMemory();
            if (g_settings.boolean("memory.autoTrim", false) && GetTickCount64() - g_lastTrim > 120000) {
                MEMORYSTATUSEX ms{ sizeof(ms) };
                if (GlobalMemoryStatusEx(&ms) && (double)ms.dwMemoryLoad >= g_settings.num("memory.autoTrimAt", 85))
                    ReportTrim(g_mem.TrimAll(ForegroundPid(), g_game.Status().exe), "auto");
            }
            break;
        }
        case TIMER_RAISECHECK:
            KillTimer(h, TIMER_RAISECHECK);
            g_raiseCheckPending = false;
            RaiseCheck();
            break;
        case TIMER_UNLOAD:
            KillTimer(h, TIMER_UNLOAD);
            if (!IsWindowVisible(h) && g_ctl) DestroyWebView();
            break;
        case TIMER_ZORDER:
            if (!g_renderer.IsRunning()) g_clock.Reassert();
            break;
        case TIMER_UPDATE:
            if (g_settings.boolean("app.autoUpdate", true)) Updater::CheckAsync(h, true);
            break;
        case TIMER_DUMP:
            KillTimer(h, TIMER_DUMP);
            g_renderer.RequestDump(g_dumpPath);
            break;
        case TIMER_EXIT:
            Quit();
            break;
        case TIMER_SHOW:
            KillTimer(h, TIMER_SHOW);
            RevealPanel();
            break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

std::wstring ArgValue(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i + 1 < argc; ++i) if (!wcscmp(argv[i], name)) return argv[i + 1];
    return {};
}
bool HasArg(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], name)) return true;
    return false;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // After an update the previous process may still be shutting down.
    std::wstring waitPid = ArgValue(argc, argv, L"--wait-pid");
    if (!waitPid.empty()) {
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)_wtoi(waitPid.c_str()));
        if (p) { WaitForSingleObject(p, 10000); CloseHandle(p); }
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, Updater::IsDevBuild() ? L"Local\\IonizationSingleInstanceDev" : L"Local\\IonizationSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(kPanelClass, nullptr);
        if (other) PostMessageW(other, WM_APP_SHOW, 0, 0);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (HasArg(argc, argv, L"--mem-test")) {
        // Self-check of the RAM optimizer on the live system (logs only, then exits).
        MemoryOptimizer mo;
        MemorySnapshot a = mo.Snapshot(5);
        LogLine("memtest: total %.1f GB, available %.2f GB, load %d%%", a.total / 1073741824.0, a.available / 1073741824.0, a.load);
        for (auto& t : a.top) LogLine("memtest: top %s %.0f MB x%d", t.exe.c_str(), t.workingSet / 1048576.0, t.instances);
        TrimResult r = mo.TrimAll(ForegroundPid(), "");
        Sleep(500);
        MemorySnapshot b = mo.Snapshot(1);
        LogLine("memtest: trimmed %d processes, working sets -%.0f MB, available %.2f -> %.2f GB",
            r.trimmed, r.freed / 1048576.0, a.available / 1073741824.0, b.available / 1073741824.0);
        mo.SetLowPriority(true, ForegroundPid());
        LogLine("memtest: lowered %zu processes, %d verified LOW", mo.LoweredCount(), mo.VerifyLowered());
        mo.RestoreAll();
        LogLine("memtest: after restore %zu tracked", mo.LoweredCount());
        return 0;
    }
    if (HasArg(argc, argv, L"--macro-selftest")) {
        LogLine("macro selftest: %s", g_macros.SelfTestConvert().dump().c_str());
        return 0;
    }
    Updater::CleanupOldExe();
    EnableDarkMenus();
    LoadSettings();
    LogLine("Ionization %s starting", IONIZATION_VERSION);

    g_startHidden = HasArg(argc, argv, L"--tray");
    g_renderer.feedbackTest = HasArg(argc, argv, L"--no-exclude");
    g_clock.noExclude = g_renderer.feedbackTest;
    g_dumpPath = ArgValue(argc, argv, L"--dump-frame");
    std::wstring exitAfter = ArgValue(argc, argv, L"--exit-after");

    g_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256, LR_DEFAULTCOLOR);
    g_iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = inst;
    wc.lpszClassName = kPanelClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = g_icon;
    wc.hIconSm = g_iconSmall;
    wc.hbrBackground = CreateSolidBrush(RGB(7, 8, 10));
    RegisterClassExW(&wc);

    // Size the panel for the monitor under the cursor.
    POINT pt; GetCursorPos(&pt);
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(hm, &mi);
    UINT dpi = 96, dpiY = 96;
    {
        using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        HMODULE shc = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (shc) if (auto f = (Fn)GetProcAddress(shc, "GetDpiForMonitor")) f(hm, 0, &dpi, &dpiY);
    }
    RECT wa = mi.rcWork;
    int w = std::min(MulDiv(1260, dpi, 96), (int)(wa.right - wa.left) - 40);
    int h = std::min(MulDiv(820, dpi, 96), (int)(wa.bottom - wa.top) - 40);
    int x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - h) / 2;

    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, kPanelClass, L"Ionization",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, x, y, w, h,
        nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF border = RGB(0x2b, 0x2e, 0x35);
    DwmSetWindowAttribute(g_hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    MARGINS margins{ 0, 0, 0, 1 };
    DwmExtendFrameIntoClientArea(g_hwnd, &margins);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    std::wstring view = ArgValue(argc, argv, L"--view");
    if (!view.empty()) g_startView = Narrow(view);
    // If a previous run hid the taskbar and never restored it (crash, kill), recover the saved state.
    if (g_settings.boolean("dock.tbHidden", false)) {
        Taskbar::SetRecovered(true, (UINT)g_settings.num("dock.tbOrig", ABS_ALWAYSONTOP));
        if (!g_settings.boolean("dock.enabled", false)) Taskbar::Restore();
    }
    // On a crash, hand the taskbar back before the process dies.
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*) -> LONG { Taskbar::Restore(); return EXCEPTION_CONTINUE_SEARCH; });
    g_dock.onTaskbarStateChanged = [] {
        bool hidden = Taskbar::IsHidden();
        if (g_settings.boolean("dock.tbHidden", false) != hidden || (hidden && (UINT)g_settings.num("dock.tbOrig", 0) != Taskbar::OriginalState())) {
            g_settings["dock"]["tbHidden"] = hidden;
            g_settings["dock"]["tbOrig"] = (int)Taskbar::OriginalState();
            SaveSettingsNow();   // immediately, so a crash can still be recovered
        }
    };
    g_dock.onPinsChanged = [](const std::vector<DockPin>& pins) {
        g_settings["dock"]["pins"] = PinsJson(pins);
        g_dockApplied.clear();
        SaveSoon();
        SendState();
    };
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();
    ApplyHotkeys();
    g_game.Configure(g_settings.boolean("perf.gameMode", true), GameRulesFrom(g_settings));
    g_game.Tick();
    g_gpuLoad.Open(GetCurrentProcessId());
    ApplySettings();
    // Started in the tray (e.g. at sign-in): don't load the browser engine until the panel opens.
    if (!g_startHidden) ShowPanel();

    // Foreground changes drive Game Mode instantly; z-order changes trigger an overlay check.
    g_hookFg = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_hookReorder = SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER, nullptr, WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    SetTimer(g_hwnd, TIMER_STATS, 500, nullptr);
    SetTimer(g_hwnd, TIMER_GAME, 500, nullptr);
    SetTimer(g_hwnd, TIMER_MEM, 15000, nullptr);
    ApplyMemory();
    g_macros.Init(g_hwnd);
    LoadMacros();
    if (std::wstring rt = ArgValue(argc, argv, L"--record-test"); !rt.empty()) {
        // Test harness: record for N ms, write the converted steps, exit.
        g_macros.StartRecording(true, false);
        SetTimer(g_hwnd, TIMER_RECTEST, (UINT)_wtoi(rt.c_str()), nullptr);
    }
    SetTimer(g_hwnd, TIMER_ZORDER, 2000, nullptr);
    SetTimer(g_hwnd, TIMER_UPDATE, 6 * 60 * 60 * 1000, nullptr);
    // Never check on the launch right after an update (guards against any update loop).
    if (g_settings.boolean("app.autoUpdate", true) && !Updater::IsDevBuild() && !HasArg(argc, argv, L"--updated"))
        Updater::CheckAsync(g_hwnd, true);
    if (!g_dumpPath.empty()) {
        std::wstring delay = ArgValue(argc, argv, L"--dump-delay");
        SetTimer(g_hwnd, TIMER_DUMP, delay.empty() ? 2500 : (UINT)_wtoi(delay.c_str()), nullptr);
    }
    if (!exitAfter.empty()) SetTimer(g_hwnd, TIMER_EXIT, (UINT)_wtoi(exitAfter.c_str()), nullptr);
    if (HasArg(argc, argv, L"--updated")) {
        Json j = Json::object(); j["status"] = "updated"; j["latest"] = IONIZATION_VERSION; g_lastUpdate = j;
    }
    LocalFree(argv);

    g_clock.onMoved = [](int cx, int cy) {
        Json& c = g_settings["clock"];
        c["corner"] = "custom"; c["x"] = cx; c["y"] = cy;
        g_clockApplied.clear();
        ApplySettings();
        SaveSoon();
        SendState();
    };

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_renderer.Stop();
    CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
