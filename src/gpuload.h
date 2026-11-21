#pragma once
#include <windows.h>
#include <pdh.h>

// 3D-engine utilisation of one process, from Windows' "GPU Engine" performance counters.
// Unlike GPU timestamps, this isn't inflated when a game is sharing the GPU.
class GpuLoad {
public:
    ~GpuLoad();
    bool Open(DWORD pid);
    double Sample();   // percent of the 3D engine, averaged since the previous call
private:
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
};
