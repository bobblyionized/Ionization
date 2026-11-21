#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

#ifndef IONIZATION_VERSION
#define IONIZATION_VERSION "0.0.0-dev"
#endif

#define APP_NAME L"Ionization"
#define WM_APP_TRAY        (WM_APP + 1)
#define WM_APP_POSTJSON    (WM_APP + 2)   // lParam = std::string* (owned by receiver)
#define WM_APP_RENDERER    (WM_APP + 3)   // wParam = event code
#define WM_APP_UPDATE      (WM_APP + 4)   // lParam = std::string* json (owned by receiver)

std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);
std::wstring AppDataDir();      // %APPDATA%\Ionization (created)
std::wstring LocalDataDir();    // %LOCALAPPDATA%\Ionization (created)
std::wstring ExePath();
bool ReadFileBytes(const std::wstring& path, std::string& out);
bool WriteFileBytes(const std::wstring& path, const std::string& data);
// Embedded RCDATA resource by name; returns false if missing.
bool LoadEmbedded(const std::wstring& name, const void** data, DWORD* size);
std::wstring ResourceNameForPath(const std::wstring& relPath);
void LogLine(const char* fmt, ...);

struct MonitorInfo {
    int adapter = 0, output = 0;
    std::wstring device;     // \\.\DISPLAY1
    std::wstring friendly;   // e.g. "DELL U2720Q"
    RECT rect{};             // desktop coordinates (physical px)
    int refresh = 60;
    bool primary = false;
};
std::vector<MonitorInfo> EnumerateMonitors();
