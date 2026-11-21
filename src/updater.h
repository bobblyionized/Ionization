#pragma once
#include <windows.h>

// Checks GitHub Releases for a newer build, downloads + verifies it, swaps the exe
// in place and asks the main window to restart. Status is posted to `notify` as
// WM_APP_UPDATE with a heap-allocated JSON string in lParam.
namespace Updater {
    bool IsDevBuild();
    void CheckAsync(HWND notify, bool install);
    void CleanupOldExe();
    // Relaunch the (new) exe and let the caller exit.
    bool Relaunch(bool showPanel);
}
