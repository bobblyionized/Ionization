#define _CRT_SECURE_NO_WARNINGS
#include "common.h"
#include <shlobj.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <algorithm>

using Microsoft::WRL::ComPtr;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring KnownDir(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    out += L"\\Ionization";
    CreateDirectoryW(out.c_str(), nullptr);
    return out;
}

std::wstring AppDataDir() { static std::wstring d = KnownDir(FOLDERID_RoamingAppData); return d; }
std::wstring LocalDataDir() { static std::wstring d = KnownDir(FOLDERID_LocalAppData); return d; }

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    return std::wstring(buf, n);
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = out.empty() ? TRUE : ReadFile(h, out.data(), (DWORD)out.size(), &rd, nullptr);
    CloseHandle(h);
    return ok && rd == out.size();
}

bool WriteFileBytes(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
    if (!ok || wr != data.size()) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::wstring ResourceNameForPath(const std::wstring& rel) {
    std::wstring n;
    for (wchar_t c : rel) {
        if (iswalnum(c)) n += (wchar_t)towupper(c);
        else n += L'_';
    }
    return n;
}

bool LoadEmbedded(const std::wstring& name, const void** data, DWORD* size) {
    HRSRC r = FindResourceW(nullptr, name.c_str(), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return false;
    *data = LockResource(g);
    *size = SizeofResource(nullptr, r);
    return *data != nullptr;
}

void LogLine(const char* fmt, ...) {
    static std::mutex m;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    std::lock_guard<std::mutex> lk(m);
    static std::wstring path = LocalDataDir() + L"\\ionization.log";
    static bool checked = false;
    if (!checked) {  // keep history across restarts (useful after updates), cap at ~256 KB
        checked = true;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 256 * 1024) DeleteFileW(path.c_str());
    }
    FILE* f = _wfopen(path.c_str(), L"ab");
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "%02d:%02d:%02d.%03d %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fclose(f);
    }
}

static std::wstring FriendlyNameFor(const std::wstring& device) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return {};
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return {};
    for (UINT32 i = 0; i < np; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (device != src.viewGdiDeviceName) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof(tgt);
        tgt.header.adapterId = paths[i].targetInfo.adapterId;
        tgt.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;
        auto tech = tgt.outputTechnology;
        if (tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL || tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
            tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED)
            return L"Built-in display";
        if (tgt.monitorFriendlyDeviceName[0]) return tgt.monitorFriendlyDeviceName;
    }
    return {};
}

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC d{};
            output->GetDesc(&d);
            if (!d.AttachedToDesktop) continue;
            MonitorInfo m;
            m.adapter = (int)a; m.output = (int)o;
            m.device = d.DeviceName;
            m.rect = d.DesktopCoordinates;
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfoW(d.Monitor, &mi)) m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            DEVMODEW dm{}; dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(d.DeviceName, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
                m.refresh = (int)dm.dmDisplayFrequency;
            m.friendly = FriendlyNameFor(d.DeviceName);
            if (m.friendly.empty()) m.friendly = L"Display";
            out.push_back(m);
        }
    }
    // Primary first, then left-to-right.
    std::stable_sort(out.begin(), out.end(), [](const MonitorInfo& x, const MonitorInfo& y) {
        if (x.primary != y.primary) return x.primary;
        return x.rect.left < y.rect.left;
    });
    return out;
}
