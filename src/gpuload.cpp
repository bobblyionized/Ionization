#include "gpuload.h"
#include <pdhmsg.h>
#include <string>
#include <vector>

GpuLoad::~GpuLoad() { if (query_) PdhCloseQuery(query_); }

bool GpuLoad::Open(DWORD pid) {
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) return false;
    std::wstring path = L"\\GPU Engine(pid_" + std::to_wstring(pid) + L"_*engtype_3D)\\Utilization Percentage";
    if (PdhAddEnglishCounterW(query_, path.c_str(), 0, &counter_) != ERROR_SUCCESS) {
        PdhCloseQuery(query_);
        query_ = nullptr;
        return false;
    }
    PdhCollectQueryData(query_);
    return true;
}

double GpuLoad::Sample() {
    if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS) return 0;
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &size, &count, nullptr) != PDH_MORE_DATA) return 0;
    std::vector<BYTE> buf(size);
    auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
    if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) return 0;
    double sum = 0;
    for (DWORD i = 0; i < count; ++i) if (items[i].FmtValue.CStatus == ERROR_SUCCESS) sum += items[i].FmtValue.doubleValue;
    return sum;
}
