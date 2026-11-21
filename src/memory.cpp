#include "memory.h"
#include "common.h"
#include <psapi.h>
#include <algorithm>
#include <cwctype>
#include <set>

namespace {

// Processes we never touch: core Windows, security, audio and the compositor.
const std::set<std::string>& Protected() {
    static const std::set<std::string> s = {
        "system", "registry", "smss.exe", "csrss.exe", "wininit.exe", "winlogon.exe", "services.exe", "lsass.exe",
        "lsaiso.exe", "dwm.exe", "audiodg.exe", "msmpeng.exe", "nissrv.exe", "fontdrvhost.exe", "memory compression",
        "secure system", "sgrmbroker.exe", "searchindexer.exe", "explorer.exe", "ctfmon.exe", "sihost.exe",
        "textinputhost.exe", "startmenuexperiencehost.exe", "shellexperiencehost.exe", "ionization.exe", "ionization-dev.exe" };
    return s;
}

std::string ExeName(HANDLE p) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = (DWORD)std::size(buf);
    if (!QueryFullProcessImageNameW(p, 0, buf, &n)) return {};
    std::wstring w(buf, n);
    size_t slash = w.find_last_of(L"\\/");
    if (slash != std::wstring::npos) w = w.substr(slash + 1);
    for (auto& c : w) c = (wchar_t)towlower(c);
    return Narrow(w);
}

std::vector<DWORD> AllPids() {
    std::vector<DWORD> pids(2048);
    DWORD bytes = 0;
    while (true) {
        if (!K32EnumProcesses(pids.data(), (DWORD)(pids.size() * sizeof(DWORD)), &bytes)) return {};
        if (bytes < pids.size() * sizeof(DWORD)) break;
        pids.resize(pids.size() * 2);
    }
    pids.resize(bytes / sizeof(DWORD));
    return pids;
}

uint64_t WorkingSet(HANDLE p) {
    PROCESS_MEMORY_COUNTERS pmc{ sizeof(pmc) };
    return K32GetProcessMemoryInfo(p, &pmc, sizeof(pmc)) ? pmc.WorkingSetSize : 0;
}

} // namespace

MemorySnapshot MemoryOptimizer::Snapshot(int topN) {
    MemorySnapshot s;
    MEMORYSTATUSEX ms{ sizeof(ms) };
    if (GlobalMemoryStatusEx(&ms)) {
        s.total = ms.ullTotalPhys;
        s.available = ms.ullAvailPhys;
        s.load = (int)ms.dwMemoryLoad;
    }
    PERFORMANCE_INFORMATION pi{ sizeof(pi) };
    if (K32GetPerformanceInfo(&pi, sizeof(pi))) {
        s.commit = (uint64_t)pi.CommitTotal * pi.PageSize;
        s.commitLimit = (uint64_t)pi.CommitLimit * pi.PageSize;
    }
    std::map<std::string, AppMemory> byExe;
    for (DWORD pid : AllPids()) {
        if (pid == 0) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!p) continue;
        std::string exe = ExeName(p);
        uint64_t ws = WorkingSet(p);
        CloseHandle(p);
        if (exe.empty() || !ws) continue;
        AppMemory& a = byExe[exe];
        a.exe = exe;
        a.workingSet += ws;
        a.instances++;
    }
    for (auto& kv : byExe) s.top.push_back(kv.second);
    std::sort(s.top.begin(), s.top.end(), [](const AppMemory& a, const AppMemory& b) { return a.workingSet > b.workingSet; });
    if ((int)s.top.size() > topN) s.top.resize(topN);
    return s;
}

static TrimResult TrimWhere(DWORD keepPid, const std::string& keepExe, const std::string& keepExe2, const std::string& onlyExe) {
    TrimResult r;
    DWORD self = GetCurrentProcessId();
    for (DWORD pid : AllPids()) {
        if (pid == 0 || pid == 4 || pid == self || pid == keepPid) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA, FALSE, pid);
        if (!p) continue;
        std::string exe = ExeName(p);
        bool skip = exe.empty() || Protected().count(exe) || (!keepExe.empty() && exe == keepExe) ||
            (!keepExe2.empty() && exe == keepExe2) ||
            (!onlyExe.empty() && exe != onlyExe);
        if (!skip) {
            uint64_t before = WorkingSet(p);
            if (K32EmptyWorkingSet(p)) {
                uint64_t after = WorkingSet(p);
                if (before > after) r.freed += before - after;
                r.trimmed++;
            }
        }
        CloseHandle(p);
    }
    return r;
}

TrimResult MemoryOptimizer::TrimAll(DWORD keepPid, const std::string& keepExe) {
    // Also spare every instance of the foreground app (e.g. all of a browser's processes).
    std::string fgExe;
    if (keepPid) {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, keepPid);
        if (p) { fgExe = ExeName(p); CloseHandle(p); }
    }
    TrimResult r = TrimWhere(keepPid, keepExe, fgExe, "");
    LogLine("Memory: trimmed %d processes, %.0f MB released", r.trimmed, r.freed / 1048576.0);
    return r;
}

TrimResult MemoryOptimizer::TrimExe(const std::string& exe) {
    TrimResult r = TrimWhere(0, "", "", exe);
    LogLine("Memory: trimmed %s (%d processes), %.0f MB released", exe.c_str(), r.trimmed, r.freed / 1048576.0);
    return r;
}

void MemoryOptimizer::SetLowPriority(bool on, DWORD foregroundPid) {
    if (!on) { RestoreAll(); return; }
    DWORD self = GetCurrentProcessId();
    std::string fgExe;
    if (foregroundPid) {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, foregroundPid);
        if (p) { fgExe = ExeName(p); CloseHandle(p); }
    }
    std::set<DWORD> alive;
    for (DWORD pid : AllPids()) {
        if (pid == 0 || pid == 4 || pid == self) continue;
        alive.insert(pid);
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE, pid);
        if (!p) continue;
        std::string exe = ExeName(p);
        bool foreground = pid == foregroundPid || (!fgExe.empty() && exe == fgExe);
        if (exe.empty() || Protected().count(exe)) { CloseHandle(p); continue; }
        auto it = lowered_.find(pid);
        if (foreground) {
            if (it != lowered_.end()) {   // the app you're using gets its normal priority back
                MEMORY_PRIORITY_INFORMATION mp{ it->second };
                SetProcessInformation(p, ProcessMemoryPriority, &mp, sizeof(mp));
                lowered_.erase(it);
            }
        } else if (it == lowered_.end()) {
            MEMORY_PRIORITY_INFORMATION cur{};
            if (GetProcessInformation(p, ProcessMemoryPriority, &cur, sizeof(cur)) && cur.MemoryPriority > MEMORY_PRIORITY_LOW) {
                MEMORY_PRIORITY_INFORMATION mp{ MEMORY_PRIORITY_LOW };
                if (SetProcessInformation(p, ProcessMemoryPriority, &mp, sizeof(mp))) lowered_[pid] = cur.MemoryPriority;
            }
        }
        CloseHandle(p);
    }
    for (auto it = lowered_.begin(); it != lowered_.end();) it = alive.count(it->first) ? std::next(it) : lowered_.erase(it);
}

int MemoryOptimizer::VerifyLowered() const {
    int ok = 0;
    for (auto& kv : lowered_) {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, kv.first);
        if (!p) continue;
        MEMORY_PRIORITY_INFORMATION mp{};
        if (GetProcessInformation(p, ProcessMemoryPriority, &mp, sizeof(mp)) && mp.MemoryPriority == MEMORY_PRIORITY_LOW) ok++;
        CloseHandle(p);
    }
    return ok;
}

void MemoryOptimizer::RestoreAll() {
    for (auto& kv : lowered_) {
        HANDLE p = OpenProcess(PROCESS_SET_INFORMATION, FALSE, kv.first);
        if (!p) continue;
        MEMORY_PRIORITY_INFORMATION mp{ kv.second };
        SetProcessInformation(p, ProcessMemoryPriority, &mp, sizeof(mp));
        CloseHandle(p);
    }
    lowered_.clear();
}
