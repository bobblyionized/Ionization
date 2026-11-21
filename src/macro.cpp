#include "macro.h"
#include "common.h"
#include "gamemode.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace {

MacroEngine* g_engine = nullptr;
HHOOK g_kb = nullptr, g_ms = nullptr;

LRESULT CALLBACK KbThunk(int c, WPARAM w, LPARAM l) { return g_engine ? g_engine->OnKeyboard(c, w, l) : CallNextHookEx(nullptr, c, w, l); }
LRESULT CALLBACK MsThunk(int c, WPARAM w, LPARAM l) { return g_engine ? g_engine->OnMouse(c, w, l) : CallNextHookEx(nullptr, c, w, l); }

LONGLONG Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
double Freq() { static double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (double)q.QuadPart; }(); return f; }

bool IsModifier(int vk) {
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL || vk == VK_LCONTROL ||
        vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}

// Never call WindowFromPoint here: it can send WM_NCHITTEST to our own (blocked) UI thread
// while the hook thread is being stopped, which deadlocks. A rectangle test sends nothing.
std::atomic<HWND> g_panel{ nullptr };
bool OverOurWindow(POINT pt) {
    HWND panel = g_panel.load();
    if (!panel || !IsWindowVisible(panel) || IsIconic(panel)) return false;
    RECT r;
    return GetWindowRect(panel, &r) && PtInRect(&r, pt);
}

// Sleep with sub-millisecond precision, waking early if the run was cancelled.
bool WaitMs(double ms, const std::atomic<unsigned>& token, unsigned mine) {
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LONGLONG end = Now() + (LONGLONG)(ms * Freq() / 1000.0);
    while (true) {
        if (token.load() != mine) return false;
        double left = (end - Now()) * 1000.0 / Freq();
        if (left <= 0) return true;
        double chunk = std::min(left, 25.0);
        if (timer) {
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(chunk * 10000.0);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep((DWORD)std::max(1.0, chunk));
        }
    }
}

int MouseFlag(int button, bool down) {
    switch (button) {
    case 0: return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    case 1: return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    case 2: return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    default: return down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
    }
}

void SendKey(int vk, int sc, bool ext, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)sc;
    in.ki.dwFlags = (sc ? KEYEVENTF_SCANCODE : 0) | (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(in));
}

void SendButton(int button, bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MouseFlag(button, down);
    if (button >= 3) in.mi.mouseData = button == 3 ? XBUTTON1 : XBUTTON2;
    SendInput(1, &in, sizeof(in));
}

MacroStep StepFromJson(const Json& j) {
    MacroStep s;
    s.kind = j.str("k", "dl");
    s.vk = (int)j.num("vk", 0); s.sc = (int)j.num("sc", 0); s.ext = j.boolean("e", false);
    s.button = (int)j.num("b", 0);
    s.x = j.num("x", 0); s.y = j.num("y", 0);
    s.dx = (int)j.num("dx", 0); s.dy = (int)j.num("dy", 0);
    s.delta = (int)j.num("d", 120); s.horizontal = j.boolean("h", false);
    s.text = Widen(j.str("s", ""));
    s.wait = std::max(0, (int)j.num("w", 0));
    return s;
}

Json StepToJson(const MacroStep& s) {
    Json j = Json::object();
    j["k"] = s.kind;
    if (s.kind == "kd" || s.kind == "ku") { j["vk"] = s.vk; j["sc"] = s.sc; if (s.ext) j["e"] = true; }
    if (s.kind == "md" || s.kind == "mu") j["b"] = s.button;
    if (s.kind == "mv") { j["x"] = std::round(s.x * 10000) / 10000; j["y"] = std::round(s.y * 10000) / 10000; }
    if (s.kind == "rm") { j["dx"] = s.dx; j["dy"] = s.dy; }
    if (s.kind == "wh") { j["d"] = s.delta; if (s.horizontal) j["h"] = true; }
    if (s.kind == "tx") j["s"] = Narrow(s.text);
    j["w"] = s.wait;
    return j;
}

} // namespace

bool Macro::NeedsHook() const {
    if (!enabled || (!trigVk && !trigMouse)) return false;
    return trigMouse != 0 || trigMode == "hold" || !block;
}

MacroEngine::~MacroEngine() {
    StopAll();
    if (hookThreadId_) { PostThreadMessageW(hookThreadId_, WM_QUIT, 0, 0); if (hookThread_.joinable()) hookThread_.join(); }
}

void MacroEngine::Init(HWND notify) {
    notify_ = notify;
    g_engine = this;
    g_panel = notify;
    // The emergency stop is always available.
    RegisterHotKey(notify_, kStopHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_ESCAPE);
}

void MacroEngine::SetMacros(const Json& list) {
    std::vector<Macro> next;
    if (list.isArr()) {
        for (auto& j : list.arr) {
            Macro m;
            m.id = j.str("id", "");
            m.name = j.str("name", "Macro");
            m.enabled = j.boolean("enabled", true);
            m.trigVk = (int)j.num("trigger.vk", 0);
            m.trigMods = (int)j.num("trigger.mods", 0);
            m.trigMouse = (int)j.num("trigger.mouse", 0);
            m.trigMode = j.str("trigger.mode", "press");
            m.block = j.boolean("trigger.block", true);
            m.scope = j.str("scope", "");
            for (auto& c : m.scope) c = (char)tolower((unsigned char)c);
            m.repeat = j.str("repeat", "once");
            m.count = std::max(1, (int)j.num("count", 1));
            m.speed = std::clamp(j.num("speed", 1.0), 0.1, 10.0);
            m.humanize = std::clamp(j.num("humanize", 0.0), 0.0, 100.0);
            if (const Json* st = j.find("steps"); st && st->isArr())
                for (auto& s : st->arr) m.steps.push_back(StepFromJson(s));
            next.push_back(std::move(m));
        }
    }
    StopAll();
    // The hook thread reads the macro list: take it down while the list changes.
    suspendHooks_ = true;
    UpdateHooks();
    {
        std::lock_guard<std::mutex> lk(mu_);
        json_ = list.isArr() ? list : Json::array();
        macros_ = std::move(next);
        playing_.assign(macros_.size(), 0);
        held_.assign(macros_.size(), 0);
    }
    RegisterTriggers();
    suspendHooks_ = false;
    UpdateHooks();
}

void MacroEngine::RegisterTriggers() {
    for (int id : registered_) UnregisterHotKey(notify_, id);
    registered_.clear();
    for (size_t i = 0; i < macros_.size(); ++i) {
        const Macro& m = macros_[i];
        if (!m.enabled || !m.trigVk || m.NeedsHook()) continue;
        int id = kHotkeyBase + (int)i;
        if (RegisterHotKey(notify_, id, (UINT)m.trigMods | MOD_NOREPEAT, (UINT)m.trigVk)) registered_.insert(id);
        else LogLine("Macro '%s': hotkey taken, falling back to hook", m.name.c_str());
    }
}

void MacroEngine::UpdateHooks() {
    bool need = recording_;
    for (size_t i = 0; i < macros_.size() && !suspendHooks_; ++i) {
        const Macro& m = macros_[i];
        if (m.NeedsHook()) need = true;
        if (m.enabled && m.trigVk && !m.NeedsHook() && !registered_.count(kHotkeyBase + (int)i)) need = true;
        if (i < playing_.size() && playing_[i] && m.trigMode == "hold") need = true;
    }
    if (suspendHooks_ && !recording_) need = false;
    if (need && !hooksOn_) {
        hooksOn_ = true;
        hookThread_ = std::thread([this] { HookThread(); });
        LogLine("Macro hooks installed");
    } else if (!need && hooksOn_) {
        for (int i = 0; i < 400 && !hookThreadId_; ++i) Sleep(5);   // thread still starting
        for (int i = 0; i < 100 && hookThreadId_ && !PostThreadMessageW(hookThreadId_, WM_QUIT, 0, 0); ++i) Sleep(5);
        if (hookThread_.joinable()) hookThread_.join();
        hookThreadId_ = 0;
        hooksOn_ = false;
        LogLine("Macro hooks removed");
    }
}

void MacroEngine::HookThread() {
    MSG peek;
    PeekMessageW(&peek, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // create the queue first
    hookThreadId_ = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    g_kb = SetWindowsHookExW(WH_KEYBOARD_LL, KbThunk, GetModuleHandleW(nullptr), 0);
    g_ms = SetWindowsHookExW(WH_MOUSE_LL, MsThunk, GetModuleHandleW(nullptr), 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_kb) UnhookWindowsHookEx(g_kb);
    if (g_ms) UnhookWindowsHookEx(g_ms);
    g_kb = g_ms = nullptr;
}

bool MacroEngine::ModsDown(int mods) {
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool ctrl = down(VK_CONTROL), alt = down(VK_MENU), shift = down(VK_SHIFT), win = down(VK_LWIN) || down(VK_RWIN);
    return ctrl == !!(mods & MOD_CONTROL) && alt == !!(mods & MOD_ALT) && shift == !!(mods & MOD_SHIFT) && win == !!(mods & MOD_WIN);
}

bool MacroEngine::ScopeOk(const Macro& m) {
    if (m.scope.empty()) return true;
    return GameMode::ExeOfWindow(GetForegroundWindow()) == m.scope;
}

LRESULT MacroEngine::OnKeyboard(int code, WPARAM w, LPARAM l) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, w, l);
    auto* k = (KBDLLHOOKSTRUCT*)l;
    bool injected = (k->flags & LLKHF_INJECTED) != 0;
    bool down = w == WM_KEYDOWN || w == WM_SYSKEYDOWN;
    static const bool recordInjected = GetEnvironmentVariableW(L"IONIZATION_RECORD_INJECTED", nullptr, 0) > 0;   // tests only
    if (injected && !(recording_ && recordInjected)) return CallNextHookEx(nullptr, code, w, l);
    if (recording_) {
        std::lock_guard<std::mutex> lk(recMu_);
        rec_.push_back({ 0, (int)k->vkCode, (int)k->scanCode, 0, 0, (k->flags & LLKHF_EXTENDED) != 0, down, false, {}, Now() });
        recCount_ = rec_.size();
        return CallNextHookEx(nullptr, code, w, l);
    }
    bool swallow = false;
    for (size_t i = 0; i < macros_.size(); ++i) {
        const Macro& m = macros_[i];
        if (!m.enabled || m.trigMouse || (int)k->vkCode != m.trigVk) continue;
        bool hookTrigger = m.NeedsHook() || !registered_.count(kHotkeyBase + (int)i);
        if (!hookTrigger) continue;
        if (down) {
            if (held_[i]) { if (m.block) swallow = true; continue; }   // auto-repeat
            if (!ModsDown(m.trigMods)) continue;
            held_[i] = 1;
            PostMessageW(notify_, WM_APP_MACRO, 1, (LPARAM)i);
        } else if (held_[i]) {
            held_[i] = 0;
            PostMessageW(notify_, WM_APP_MACRO, 2, (LPARAM)i);
        }
        if (m.block) swallow = true;
    }
    return swallow ? 1 : CallNextHookEx(nullptr, code, w, l);
}

LRESULT MacroEngine::OnMouse(int code, WPARAM w, LPARAM l) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, w, l);
    auto* m = (MSLLHOOKSTRUCT*)l;
    static const bool recordInjected = GetEnvironmentVariableW(L"IONIZATION_RECORD_INJECTED", nullptr, 0) > 0;   // tests only
    if ((m->flags & LLMHF_INJECTED) && !(recording_ && recordInjected)) return CallNextHookEx(nullptr, code, w, l);
    int button = -1; bool down = false; int delta = 0; bool horiz = false; bool move = false;
    switch (w) {
    case WM_LBUTTONDOWN: button = 0; down = true; break;  case WM_LBUTTONUP: button = 0; break;
    case WM_RBUTTONDOWN: button = 1; down = true; break;  case WM_RBUTTONUP: button = 1; break;
    case WM_MBUTTONDOWN: button = 2; down = true; break;  case WM_MBUTTONUP: button = 2; break;
    case WM_XBUTTONDOWN: button = HIWORD(m->mouseData) == XBUTTON1 ? 3 : 4; down = true; break;
    case WM_XBUTTONUP: button = HIWORD(m->mouseData) == XBUTTON1 ? 3 : 4; break;
    case WM_MOUSEWHEEL: delta = (short)HIWORD(m->mouseData); break;
    case WM_MOUSEHWHEEL: delta = (short)HIWORD(m->mouseData); horiz = true; break;
    case WM_MOUSEMOVE: move = true; break;
    }
    if (recording_) {
        if (!OverOurWindow(m->pt)) {
            std::lock_guard<std::mutex> lk(recMu_);
            if (button >= 0) rec_.push_back({ 2, 0, 0, button, 0, false, down, false, m->pt, Now() });
            else if (delta) rec_.push_back({ 3, 0, 0, 0, delta, false, false, horiz, m->pt, Now() });
            else if (move && recMouse_) rec_.push_back({ 1, 0, 0, 0, 0, false, false, false, m->pt, Now() });
            recCount_ = rec_.size();
        }
        return CallNextHookEx(nullptr, code, w, l);
    }
    // Mouse-button triggers: middle (3), X1 (4), X2 (5) map from button 2/3/4.
    bool swallow = false;
    if (button >= 2) {
        int trig = button + 1;
        for (size_t i = 0; i < macros_.size(); ++i) {
            const Macro& mc = macros_[i];
            if (!mc.enabled || mc.trigMouse != trig) continue;
            if (down) {
                if (!ModsDown(mc.trigMods)) continue;
                held_[i] = 1;
                PostMessageW(notify_, WM_APP_MACRO, 1, (LPARAM)i);
            } else if (held_[i]) {
                held_[i] = 0;
                PostMessageW(notify_, WM_APP_MACRO, 2, (LPARAM)i);
            }
            if (mc.block) swallow = true;
        }
    }
    return swallow ? 1 : CallNextHookEx(nullptr, code, w, l);
}

bool MacroEngine::StartRecording(bool mouseMoves, bool relative) {
    if (recording_) return false;
    StopAll();
    {
        std::lock_guard<std::mutex> lk(recMu_);
        rec_.clear();
        recCount_ = 0;
    }
    recMouse_ = mouseMoves;
    recRelative_ = relative;
    recording_ = true;
    UpdateHooks();
    LogLine("Macro recording started");
    return true;
}

Json MacroEngine::StopRecording() {
    recording_ = false;
    std::vector<RawEvent> ev;
    {
        std::lock_guard<std::mutex> lk(recMu_);
        ev.swap(rec_);
        recCount_ = 0;
    }
    UpdateHooks();
    return Convert(ev);
}

Json MacroEngine::SelfTestConvert() {
    // Synthetic events in the hook's own format: a dragged path, a click, A, scroll, Ctrl left over.
    std::vector<RawEvent> ev;
    LONGLONG t = Now(), f = (LONGLONG)Freq();
    auto ms = [&](double v) { return t + (LONGLONG)(v * f / 1000.0); };
    for (int i = 0; i <= 40; ++i) ev.push_back({ 1, 0, 0, 0, 0, false, false, false, { 300 + i * 5, 400 }, ms(i * 2.0) });   // 2 ms apart
    ev.push_back({ 2, 0, 0, 0, 0, false, true, false, { 500, 400 }, ms(100) });
    ev.push_back({ 2, 0, 0, 0, 0, false, false, false, { 500, 400 }, ms(140) });
    ev.push_back({ 0, 0x41, 0x1E, 0, 0, false, true, false, {}, ms(200) });
    ev.push_back({ 0, 0x41, 0x1E, 0, 0, false, false, false, {}, ms(260) });
    ev.push_back({ 3, 0, 0, 0, -120, false, false, false, { 500, 400 }, ms(320) });
    ev.push_back({ 0, VK_LCONTROL, 0x1D, 0, 0, false, true, false, {}, ms(400) });   // trailing modifier: dropped
    recMouse_ = true; recRelative_ = false;
    return Convert(ev);
}

Json MacroEngine::Convert(std::vector<RawEvent> ev) {
    // Drop trailing modifier presses (from a stop shortcut) and anything after the last real event.
    while (!ev.empty() && ev.back().type == 0 && IsModifier(ev.back().vk)) ev.pop_back();

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1), vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    Json steps = Json::array();
    LONGLONG last = ev.empty() ? 0 : ev.front().t;
    LONGLONG lastMoveT = 0;
    POINT lastPt{ LONG_MIN, LONG_MIN }, pendingPt{};
    bool pending = false;
    LONGLONG pendingT = 0;
    auto emit = [&](MacroStep s, LONGLONG t) {
        s.wait = (int)std::lround((t - last) * 1000.0 / Freq());
        last = t;
        steps.arr.push_back(StepToJson(s));
    };
    auto emitMove = [&](POINT pt, LONGLONG t) {
        MacroStep s;
        if (recRelative_ && lastPt.x != LONG_MIN) { s.kind = "rm"; s.dx = pt.x - lastPt.x; s.dy = pt.y - lastPt.y; }
        else { s.kind = "mv"; s.x = double(pt.x - vx) / vw; s.y = double(pt.y - vy) / vh; }
        emit(s, t);
        lastPt = pt; lastMoveT = t;
    };
    for (auto& e : ev) {
        if (e.type == 1) {
            // Thin the path: keep a point every 8 ms or after 24 px.
            double ms = (e.t - lastMoveT) * 1000.0 / Freq();
            double dist = lastPt.x == LONG_MIN ? 1e9 : std::hypot(double(e.pt.x - lastPt.x), double(e.pt.y - lastPt.y));
            if (ms >= 8 || dist >= 24) { emitMove(e.pt, e.t); pending = false; }
            else { pending = true; pendingPt = e.pt; pendingT = e.t; }
            continue;
        }
        if (pending) { emitMove(pendingPt, pendingT); pending = false; }
        MacroStep s;
        if (e.type == 0) { s.kind = e.down ? "kd" : "ku"; s.vk = e.vk; s.sc = e.sc; s.ext = e.ext; }
        else if (e.type == 2) {
            if (!recMouse_ || lastPt.x == LONG_MIN || lastPt.x != e.pt.x || lastPt.y != e.pt.y) {
                if (!recRelative_) emitMove(e.pt, e.t);   // clicks always land where they were recorded
            }
            s.kind = e.down ? "md" : "mu"; s.button = e.button;
        }
        else { s.kind = "wh"; s.delta = e.delta; s.horizontal = e.horizontal; }
        emit(s, e.t);
    }
    if (pending) emitMove(pendingPt, pendingT);
    if (!steps.arr.empty()) steps.arr[0]["w"] = 0;
    LogLine("Macro recording stopped: %zu raw events -> %zu steps", ev.size(), steps.arr.size());
    return steps;
}

void MacroEngine::Play(int index, bool loopUntilStopped) {
    if (index < 0 || index >= (int)macros_.size()) return;
    if (macros_[index].steps.empty() || recording_) return;
    unsigned token = playToken_.load();
    {
        std::lock_guard<std::mutex> lk(mu_);
        playing_[index]++;
    }
    UpdateHooks();
    PostMessageW(notify_, WM_APP_MACRO, 3, 0);
    std::thread([this, index, loopUntilStopped, token] { PlayThread(index, loopUntilStopped, token); }).detach();
}

void MacroEngine::PlayThread(int index, bool loopUntilStopped, unsigned token) {
    Macro m;
    { std::lock_guard<std::mutex> lk(mu_); m = macros_[index]; }
    std::mt19937 rng(GetTickCount());
    std::uniform_real_distribution<double> jitter(-1.0, 1.0);
    std::set<int> keys, buttons;
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    (void)vx; (void)vy;
    int iteration = 0;
    bool ok = true;
    while (ok) {
        for (const MacroStep& s : m.steps) {
            double wait = s.wait / m.speed;
            if (m.humanize > 0 && wait > 0) wait *= 1.0 + jitter(rng) * m.humanize / 100.0 * 0.5;
            if (wait > 0 && !WaitMs(wait, playToken_, token)) { ok = false; break; }
            if (playToken_.load() != token) { ok = false; break; }
            if (s.kind == "kd") { SendKey(s.vk, s.sc, s.ext, true); keys.insert(s.vk); }
            else if (s.kind == "ku") { SendKey(s.vk, s.sc, s.ext, false); keys.erase(s.vk); }
            else if (s.kind == "md") { SendButton(s.button, true); buttons.insert(s.button); }
            else if (s.kind == "mu") { SendButton(s.button, false); buttons.erase(s.button); }
            else if (s.kind == "mv") {
                INPUT in{}; in.type = INPUT_MOUSE;
                in.mi.dx = (LONG)std::lround(s.x * 65535.0);
                in.mi.dy = (LONG)std::lround(s.y * 65535.0);
                in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                SendInput(1, &in, sizeof(in));
            } else if (s.kind == "rm") {
                INPUT in{}; in.type = INPUT_MOUSE; in.mi.dx = s.dx; in.mi.dy = s.dy; in.mi.dwFlags = MOUSEEVENTF_MOVE;
                SendInput(1, &in, sizeof(in));
            } else if (s.kind == "wh") {
                INPUT in{}; in.type = INPUT_MOUSE; in.mi.mouseData = (DWORD)s.delta;
                in.mi.dwFlags = s.horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
                SendInput(1, &in, sizeof(in));
            } else if (s.kind == "tx") {
                for (wchar_t ch : s.text) {
                    INPUT in[2]{};
                    in[0].type = in[1].type = INPUT_KEYBOARD;
                    in[0].ki.wScan = in[1].ki.wScan = ch;
                    in[0].ki.dwFlags = KEYEVENTF_UNICODE;
                    in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
                    SendInput(2, in, sizeof(INPUT));
                    if (!WaitMs(8 / m.speed, playToken_, token)) { ok = false; break; }
                }
            }
        }
        if (!ok) break;
        ++iteration;
        bool again;
        if (loopUntilStopped || m.repeat == "forever") again = true;
        else if (m.trigMode == "hold") again = held_.size() > (size_t)index && held_[index];
        else if (m.repeat == "times") again = iteration < m.count;
        else again = false;
        if (!again) break;
        if (playToken_.load() != token) break;
        if (m.steps.front().wait == 0 && !WaitMs(1, playToken_, token)) break;   // never spin
    }
    // Never leave keys or buttons stuck down.
    for (int vk : keys) SendKey(vk, MapVirtualKeyW(vk, MAPVK_VK_TO_VSC), false, false);
    for (int b : buttons) SendButton(b, false);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (index < (int)playing_.size() && playing_[index] > 0) playing_[index]--;
    }
    PostMessageW(notify_, WM_APP_MACRO, 3, 1);
}

void MacroEngine::StopAll() {
    playToken_++;
    for (int i = 0; i < 100; ++i) {
        bool any = false;
        { std::lock_guard<std::mutex> lk(mu_); for (int p : playing_) if (p) any = true; }
        if (!any) break;
        Sleep(5);
    }
}

bool MacroEngine::OnHotkey(int id) {
    if (id == kStopHotkey) {
        bool wasRecording = recording_;
        if (wasRecording) StopRecording();
        StopAll();
        LogLine("Macro emergency stop");
        PostMessageW(notify_, WM_APP_MACRO, 3, 2);
        return true;
    }
    if (id < kHotkeyBase || id >= kHotkeyBase + (int)macros_.size()) return false;
    OnTrigger(id - kHotkeyBase, true);
    return true;
}

void MacroEngine::OnTrigger(int index, bool down) {
    if (index < 0 || index >= (int)macros_.size()) return;
    const Macro& m = macros_[index];
    if (!m.enabled || recording_) return;
    bool active = false;
    { std::lock_guard<std::mutex> lk(mu_); active = playing_[index] > 0; }
    if (m.trigMode == "hold") {
        if (down && !active && ScopeOk(m)) Play(index);
        return;   // releasing the trigger ends the loop (checked between iterations)
    }
    if (!down) return;
    if (m.trigMode == "toggle") {
        if (active) { StopAll(); PostMessageW(notify_, WM_APP_MACRO, 3, 0); }
        else if (ScopeOk(m)) Play(index, true);
        return;
    }
    if (!active && ScopeOk(m)) Play(index);
}

Json MacroEngine::StatusJson() {
    Json j = Json::object();
    j["recording"] = (bool)recording_;
    j["events"] = (int)recCount_.load();
    Json playing = Json::array();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (size_t i = 0; i < playing_.size() && i < macros_.size(); ++i) if (playing_[i]) playing.arr.push_back(Json(macros_[i].id));
    }
    j["playing"] = playing;
    j["hooks"] = (bool)hooksOn_;
    return j;
}
