#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <cstdint>

struct AppMemory {
    std::string exe;
    uint64_t workingSet = 0;   // bytes, summed over instances
    int instances = 0;
};

struct MemorySnapshot {
    uint64_t total = 0, available = 0, commit = 0, commitLimit = 0;
    int load = 0;                      // percent in use
    std::vector<AppMemory> top;        // largest apps first
};

struct TrimResult {
    uint64_t freed = 0;    // drop in summed working sets (bytes)
    int trimmed = 0;       // processes trimmed
};

// Keeps background apps from hogging RAM: trims working sets on demand and can mark
// background processes as low memory priority so Windows reclaims their pages first.
class MemoryOptimizer {
public:
    MemorySnapshot Snapshot(int topN = 12);
    // Trim every process we may touch, except `keepPid` (the app in use) and `keepExe` (a game).
    TrimResult TrimAll(DWORD keepPid, const std::string& keepExe);
    TrimResult TrimExe(const std::string& exe);
    // Low memory priority for background processes; the foreground app is always restored.
    void SetLowPriority(bool on, DWORD foregroundPid);
    void RestoreAll();
    size_t LoweredCount() const { return lowered_.size(); }
    // Self-check for --mem-test: returns how many lowered processes really report LOW priority.
    int VerifyLowered() const;

private:
    std::map<DWORD, ULONG> lowered_;   // pid -> original memory priority
};
