#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include "json.h"

struct ClipItem {
    uint64_t id = 0;
    std::string kind;                 // text | link | image | files
    std::wstring text;                // text and links (files: the paths, one per line)
    std::vector<std::wstring> files;
    std::string png, thumb;           // images: full PNG and a small preview PNG
    int w = 0, h = 0;
    long long time = 0;               // unix ms
    bool pinned = false;
    uint64_t hash = 0;
};

// Clipboard history. Watches the clipboard with AddClipboardFormatListener, skips
// anything marked private (password managers), keeps the newest items and stores
// them encrypted with DPAPI so only this Windows account can read them.
class ClipboardHistory {
public:
    ~ClipboardHistory();
    void Enable(bool on, int maxItems);
    bool Enabled() const { return hwnd_ != nullptr; }
    const std::vector<ClipItem>& Items() const { return items_; }
    unsigned Version() const { return version_; }
    bool CopyBack(uint64_t id);        // put an item back on the clipboard (and move it to the top)
    void TogglePin(uint64_t id);
    void Remove(uint64_t id);
    void Clear();                      // everything except pinned items
    Json ListJson(size_t max = 60) const;   // for the panel (text clipped, thumbnails as data URLs)
    void SaveNow();
    Json SelfTest();                   // dev builds: dedupe, PNG thumbnails, DPAPI save + reload (no system clipboard)
    std::function<void()> onChanged;

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    void Capture();
    void Add(ClipItem it);
    void Trim();
    void Changed();
    void Load();
    HWND hwnd_ = nullptr;
    std::vector<ClipItem> items_;
    int max_ = 60;
    unsigned version_ = 0;
    uint64_t nextId_ = 1;
    bool loaded_ = false, dirty_ = false;
};

namespace ImageUtil {
    // PNG bytes -> 32bpp premultiplied BGRA scaled to fit maxSide (0 = original size).
    bool DecodePng(const std::string& png, int maxSide, std::vector<uint8_t>& bgra, int& w, int& h);
}
