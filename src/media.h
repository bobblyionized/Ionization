#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

// "Now playing" from Windows' media controls (Spotify, browsers, Apple Music, VLC, ...).
struct MediaInfo {
    bool has = false, playing = false;
    std::wstring title, artist, app;
    double position = 0, duration = 0;   // seconds (position extrapolated while playing)
    std::vector<uint8_t> art;           // encoded image bytes (PNG/JPEG)
    unsigned artVersion = 0;
};

class MediaWatcher {
public:
    ~MediaWatcher() { Stop(); }
    void Start(HWND notify, UINT msg);   // posts msg when something changes
    void Stop();
    MediaInfo Get();
    void Command(int c);                 // 1 play/pause, 2 next, 3 previous
private:
    void Run();
    std::thread thread_;
    std::atomic<bool> stop_{ false };
    std::atomic<int> cmd_{ 0 };
    std::mutex mu_;
    MediaInfo info_;
    HWND notify_ = nullptr;
    UINT msg_ = 0;
};
