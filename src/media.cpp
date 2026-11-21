// C++/WinRT lives in this file only.
#include "media.h"
#include "common.h"
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <chrono>

using namespace winrt;
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage::Streams;

void MediaWatcher::Start(HWND notify, UINT msg) {
    if (thread_.joinable()) return;
    notify_ = notify; msg_ = msg;
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
}

void MediaWatcher::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

MediaInfo MediaWatcher::Get() {
    std::lock_guard<std::mutex> lk(mu_);
    return info_;
}

void MediaWatcher::Command(int c) { cmd_ = c; }

void MediaWatcher::Run() {
    try { init_apartment(apartment_type::multi_threaded); } catch (...) {}
    GlobalSystemMediaTransportControlsSessionManager mgr{ nullptr };
    try { mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get(); }
    catch (...) { LogLine("Media: session manager unavailable"); return; }
    std::wstring lastArtKey;
    unsigned artVersion = 0;
    std::vector<uint8_t> art;
    int tick = 0;
    while (!stop_) {
        MediaInfo next;
        try {
            auto s = mgr.GetCurrentSession();
            if (s) {
                int c = cmd_.exchange(0);
                if (c == 1) s.TryTogglePlayPauseAsync().get();
                else if (c == 2) s.TrySkipNextAsync().get();
                else if (c == 3) s.TrySkipPreviousAsync().get();
                auto props = s.TryGetMediaPropertiesAsync().get();
                next.has = true;
                next.title = props.Title().c_str();
                next.artist = props.Artist().c_str();
                next.app = s.SourceAppUserModelId().c_str();
                auto pb = s.GetPlaybackInfo();
                next.playing = pb.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                auto tl = s.GetTimelineProperties();
                using secs = std::chrono::duration<double>;
                double pos = std::chrono::duration_cast<secs>(tl.Position()).count();
                double end = std::chrono::duration_cast<secs>(tl.EndTime() - tl.StartTime()).count();
                if (next.playing) {
                    double since = std::chrono::duration_cast<secs>(winrt::clock::now() - tl.LastUpdatedTime()).count();
                    if (since > 0 && since < 3600) pos += since;
                }
                next.position = end > 0 ? std::min(pos, end) : pos;
                next.duration = end;
                std::wstring artKey = next.title + L"\x1f" + next.artist;
                if (artKey != lastArtKey) {
                    lastArtKey = artKey;
                    art.clear();
                    if (auto th = props.Thumbnail()) {
                        auto stream = th.OpenReadAsync().get();
                        uint32_t size = (uint32_t)std::min<uint64_t>(stream.Size(), 8u * 1024 * 1024);
                        Buffer buf(size);
                        auto rd = stream.ReadAsync(buf, size, InputStreamOptions::None).get();
                        art.assign(rd.data(), rd.data() + rd.Length());
                    }
                    ++artVersion;
                }
                next.art = art;
                next.artVersion = artVersion;
            }
        } catch (...) {
            // Sessions come and go (apps closing); try again next tick.
        }
        bool changed;
        {
            std::lock_guard<std::mutex> lk(mu_);
            changed = next.has != info_.has || next.playing != info_.playing || next.title != info_.title ||
                next.artist != info_.artist || next.artVersion != info_.artVersion || (int)next.position != (int)info_.position;
            info_ = std::move(next);
        }
        if (changed && notify_) PostMessageW(notify_, msg_, 0, 0);
        // Poll quickly right after a command, otherwise once a second.
        for (int i = 0; i < 10 && !stop_ && cmd_ == 0; ++i) Sleep(100);
        ++tick;
    }
}
