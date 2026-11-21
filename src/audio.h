#pragma once
#include <windows.h>
#include <cstdint>

// System audio, screen brightness and "in use" privacy indicators.
namespace Audio {
    float Volume();                  // 0..1, -1 if unavailable
    void SetVolume(float v);
    bool SpeakerMuted();
    int MicMuted();                  // 1 muted, 0 live, -1 no microphone
    void SetMicMuted(bool m);
    // Posts `msg` to `notify` whenever the speaker volume or mute changes.
    void WatchVolume(HWND notify, UINT msg);
    void StopWatching();
}

namespace Brightness {
    int Get();                       // 0..100, -1 if the display doesn't support it
    void Set(int v);
}

namespace Privacy {
    struct InUse { bool mic = false, camera = false; ULONGLONG micSinceMs = 0; };
    InUse Poll();                    // from Windows' capability-access records (no permissions needed)
}
