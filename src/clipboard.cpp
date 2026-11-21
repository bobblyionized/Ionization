#include "clipboard.h"
#include "common.h"
#include <wincrypt.h>
#include <dpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace {
const wchar_t* kClass = L"IonizationClipboard";
enum { T_CAPTURE = 1, T_SAVE = 2 };
const size_t kMaxText = 200000;              // chars kept per text item
const size_t kMaxPng = 8u * 1024 * 1024;     // bigger images are skipped
const int kMaxImages = 12;                   // unpinned images kept

long long NowMs() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (long long)((t - 116444736000000000ULL) / 10000);
}

uint64_t Fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ULL) {
    auto* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string B64(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
        out += kB64[v >> 18]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += kB64[v & 63];
    }
    if (i < in.size()) {
        uint32_t v = (uint8_t)in[i] << 16;
        if (i + 1 < in.size()) v |= (uint8_t)in[i + 1] << 8;
        out += kB64[v >> 18]; out += kB64[(v >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}
std::string UnB64(const std::string& in) {
    static int8_t map[256];
    static bool init = false;
    if (!init) { memset(map, -1, sizeof(map)); for (int i = 0; i < 64; ++i) map[(uint8_t)kB64[i]] = (int8_t)i; init = true; }
    std::string out;
    out.reserve(in.size() / 4 * 3);
    uint32_t v = 0; int bits = 0;
    for (char c : in) {
        int8_t d = map[(uint8_t)c];
        if (d < 0) continue;
        v = (v << 6) | (uint32_t)d; bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((v >> bits) & 0xFF); }
    }
    return out;
}

IWICImagingFactory* Wic() {
    static ComPtr<IWICImagingFactory> f;
    if (!f) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f.Get();
}

ComPtr<IWICBitmapSource> DecodeBytes(const void* data, size_t n) {
    if (!Wic() || !n) return nullptr;
    ComPtr<IWICStream> st;
    if (FAILED(Wic()->CreateStream(&st)) || FAILED(st->InitializeFromMemory((BYTE*)data, (DWORD)n))) return nullptr;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(Wic()->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec))) return nullptr;
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, &fr))) return nullptr;
    // Materialize: the stream points at memory the caller may free.
    ComPtr<IWICBitmap> bmp;
    if (FAILED(Wic()->CreateBitmapFromSource(fr.Get(), WICBitmapCacheOnLoad, &bmp))) return nullptr;
    return bmp;
}

ComPtr<IWICBitmapSource> Scaled(IWICBitmapSource* src, int maxSide) {
    UINT w = 0, h = 0; src->GetSize(&w, &h);
    if (!maxSide || (int)std::max(w, h) <= maxSide) return src;
    double s = (double)maxSide / std::max(w, h);
    ComPtr<IWICBitmapScaler> sc;
    if (FAILED(Wic()->CreateBitmapScaler(&sc)) ||
        FAILED(sc->Initialize(src, std::max(1u, (UINT)(w * s)), std::max(1u, (UINT)(h * s)), WICBitmapInterpolationModeFant))) return src;
    return sc;
}

std::string EncodePng(IWICBitmapSource* src) {
    ComPtr<IStream> mem;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &mem))) return {};
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(Wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(mem.Get(), WICBitmapEncoderNoCache))) return {};
    ComPtr<IWICBitmapFrameEncode> fr;
    ComPtr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(&fr, &props)) || FAILED(fr->Initialize(props.Get()))) return {};
    UINT w = 0, h = 0; src->GetSize(&w, &h);
    fr->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    fr->SetPixelFormat(&fmt);
    ComPtr<IWICFormatConverter> conv;
    Wic()->CreateFormatConverter(&conv);
    IWICBitmapSource* feed = src;
    if (conv && SUCCEEDED(conv->Initialize(src, fmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) feed = conv.Get();
    if (FAILED(fr->WriteSource(feed, nullptr)) || FAILED(fr->Commit()) || FAILED(enc->Commit())) return {};
    STATSTG stat{}; mem->Stat(&stat, STATFLAG_NONAME);
    std::string out((size_t)stat.cbSize.QuadPart, '\0');
    LARGE_INTEGER zero{}; mem->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG rd = 0; mem->Read(out.data(), (ULONG)out.size(), &rd);
    out.resize(rd);
    return out;
}

// CF_DIB (BITMAPINFO + bits) -> a .bmp file in memory, which WIC can decode.
std::string DibToBmp(const uint8_t* dib, size_t n) {
    if (n < sizeof(BITMAPINFOHEADER)) return {};
    auto* bi = (const BITMAPINFOHEADER*)dib;
    size_t head = bi->biSize;
    if (bi->biSize == sizeof(BITMAPINFOHEADER) && bi->biCompression == BI_BITFIELDS) head += 12;
    size_t colors = bi->biClrUsed ? bi->biClrUsed : (bi->biBitCount <= 8 ? (1u << bi->biBitCount) : 0);
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfSize = (DWORD)(sizeof(fh) + n);
    fh.bfOffBits = (DWORD)(sizeof(fh) + head + colors * 4);
    std::string out((const char*)&fh, sizeof(fh));
    out.append((const char*)dib, n);
    return out;
}

void SetBytes(UINT fmt, const void* data, size_t n) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!g) return;
    memcpy(GlobalLock(g), data, n);
    GlobalUnlock(g);
    if (!SetClipboardData(fmt, g)) GlobalFree(g);
}

bool OpenClip(HWND h) {
    for (int i = 0; i < 10; ++i) { if (OpenClipboard(h)) return true; Sleep(15); }
    return false;
}

std::wstring Trimmed(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}
} // namespace

bool ImageUtil::DecodePng(const std::string& png, int maxSide, std::vector<uint8_t>& bgra, int& w, int& h) {
    auto src = DecodeBytes(png.data(), png.size());
    if (!src) return false;
    auto sc = Scaled(src.Get(), maxSide);
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(Wic()->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(sc.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    UINT uw = 0, uh = 0; conv->GetSize(&uw, &uh);
    w = (int)uw; h = (int)uh;
    bgra.resize((size_t)uw * uh * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, (UINT)bgra.size(), bgra.data()));
}

ClipboardHistory::~ClipboardHistory() { Enable(false, max_); }

LRESULT CALLBACK ClipboardHistory::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = (ClipboardHistory*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (self) {
        if (m == WM_CLIPBOARDUPDATE) { SetTimer(h, T_CAPTURE, 120, nullptr); return 0; }   // apps often write in bursts
        if (m == WM_TIMER && w == T_CAPTURE) { KillTimer(h, T_CAPTURE); self->Capture(); return 0; }
        if (m == WM_TIMER && w == T_SAVE) { KillTimer(h, T_SAVE); self->SaveNow(); return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}

void ClipboardHistory::Enable(bool on, int maxItems) {
    max_ = std::clamp(maxItems, 10, 500);
    if (on && !hwnd_) {
        if (!loaded_) Load();
        static bool reg = false;
        if (!reg) {
            WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kClass;
            RegisterClassW(&wc); reg = true;
        }
        hwnd_ = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
        AddClipboardFormatListener(hwnd_);
        LogLine("Clipboard history on (%zu items stored)", items_.size());
    } else if (!on && hwnd_) {
        if (dirty_) SaveNow();
        RemoveClipboardFormatListener(hwnd_);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        LogLine("Clipboard history off");
    }
    if (on) Trim();
}

void ClipboardHistory::Capture() {
    static const UINT fExclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const UINT fCanInclude = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    static const UINT fIgnore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    static const UINT fPng = RegisterClipboardFormatW(L"PNG");
    if (GetClipboardOwner() == hwnd_) return;                   // our own copy-back
    // Password managers mark their copies private; respect that.
    if (IsClipboardFormatAvailable(fExclude) || IsClipboardFormatAvailable(fIgnore)) return;
    if (!OpenClip(hwnd_)) return;
    ClipItem it;
    std::string rawPng, rawDib;
    bool skip = false;
    if (HANDLE h = GetClipboardData(fCanInclude)) {
        if (auto* p = (const DWORD*)GlobalLock(h)) { if (GlobalSize(h) >= sizeof(DWORD) && *p == 0) skip = true; GlobalUnlock(h); }
    }
    if (!skip) {
        if (HANDLE h = GetClipboardData(CF_HDROP)) {
            if (auto drop = (HDROP)GlobalLock(h)) {
                UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n && i < 500; ++i) {
                    wchar_t path[MAX_PATH * 4];
                    if (DragQueryFileW(drop, i, path, ARRAYSIZE(path))) it.files.push_back(path);
                }
                GlobalUnlock(h);
            }
            if (!it.files.empty()) {
                it.kind = "files";
                for (auto& f : it.files) { if (!it.text.empty()) it.text += L"\n"; it.text += f; }
            }
        }
        if (it.kind.empty()) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = (const wchar_t*)GlobalLock(h)) {
                    size_t n = wcsnlen(p, GlobalSize(h) / sizeof(wchar_t));
                    it.text.assign(p, std::min(n, kMaxText));
                    GlobalUnlock(h);
                }
                std::wstring t = Trimmed(it.text);
                if (!t.empty()) {
                    bool url = (t.rfind(L"http://", 0) == 0 || t.rfind(L"https://", 0) == 0 || t.rfind(L"www.", 0) == 0) &&
                        t.find_first_of(L" \t\r\n") == std::wstring::npos;
                    it.kind = url ? "link" : "text";
                    if (url) it.text = t;
                }
            }
        }
        if (it.kind.empty()) {
            if (HANDLE h = GetClipboardData(fPng)) {
                if (auto* p = GlobalLock(h)) { size_t n = GlobalSize(h); if (n <= kMaxPng) rawPng.assign((const char*)p, n); GlobalUnlock(h); }
            }
            if (rawPng.empty()) if (HANDLE h = GetClipboardData(CF_DIB)) {
                if (auto* p = GlobalLock(h)) { size_t n = GlobalSize(h); if (n <= 64u * 1024 * 1024) rawDib.assign((const char*)p, n); GlobalUnlock(h); }
            }
        }
    }
    CloseClipboard();
    if (skip) return;
    if (it.kind.empty() && (!rawPng.empty() || !rawDib.empty())) {
        // Heavy work only after the clipboard is closed again.
        std::string bmp = rawPng.empty() ? DibToBmp((const uint8_t*)rawDib.data(), rawDib.size()) : std::string();
        auto src = rawPng.empty() ? DecodeBytes(bmp.data(), bmp.size()) : DecodeBytes(rawPng.data(), rawPng.size());
        if (src) {
            UINT w = 0, h = 0; src->GetSize(&w, &h);
            if ((ULONGLONG)w * h <= 40000000ULL) {
                it.png = rawPng.empty() ? EncodePng(src.Get()) : rawPng;
                it.thumb = EncodePng(Scaled(src.Get(), 160).Get());
                it.w = (int)w; it.h = (int)h;
                if (!it.png.empty() && it.png.size() <= kMaxPng) it.kind = "image";
            }
        }
    }
    if (it.kind.empty()) return;
    Add(std::move(it));
}

void ClipboardHistory::Add(ClipItem it) {
    uint64_t h = Fnv(it.kind.data(), it.kind.size());
    if (it.kind == "image") h = Fnv(it.png.data(), it.png.size(), h);
    else h = Fnv(it.text.data(), it.text.size() * sizeof(wchar_t), h);
    it.hash = h;
    it.time = NowMs();
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].hash == h) {
            ClipItem old = std::move(items_[i]);
            items_.erase(items_.begin() + i);
            old.time = it.time;
            items_.insert(items_.begin(), std::move(old));
            Changed();
            return;
        }
    }
    it.id = nextId_++;
    items_.insert(items_.begin(), std::move(it));
    Trim();
    Changed();
}

void ClipboardHistory::Trim() {
    int unpinned = 0, images = 0;
    for (size_t i = 0; i < items_.size();) {
        auto& it = items_[i];
        if (!it.pinned) {
            ++unpinned;
            if (it.kind == "image") ++images;
            if (unpinned > max_ || (it.kind == "image" && images > kMaxImages)) { items_.erase(items_.begin() + i); dirty_ = true; continue; }
        }
        ++i;
    }
}

void ClipboardHistory::Changed() {
    ++version_;
    dirty_ = true;
    if (hwnd_) SetTimer(hwnd_, T_SAVE, 1500, nullptr);
    else SaveNow();
    if (onChanged) onChanged();
}

bool ClipboardHistory::CopyBack(uint64_t id) {
    auto itp = std::find_if(items_.begin(), items_.end(), [&](const ClipItem& c) { return c.id == id; });
    if (itp == items_.end()) return false;
    HWND owner = hwnd_;
    if (!owner) return false;
    if (!OpenClip(owner)) return false;
    EmptyClipboard();
    const ClipItem& it = *itp;
    if (it.kind == "text" || it.kind == "link") {
        SetBytes(CF_UNICODETEXT, it.text.c_str(), (it.text.size() + 1) * sizeof(wchar_t));
    } else if (it.kind == "files") {
        std::wstring list;
        for (auto& f : it.files) { list += f; list += L'\0'; }
        list += L'\0';
        std::string buf(sizeof(DROPFILES) + list.size() * sizeof(wchar_t), '\0');
        auto* df = (DROPFILES*)buf.data();
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        memcpy(buf.data() + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
        SetBytes(CF_HDROP, buf.data(), buf.size());
        DWORD effect = DROPEFFECT_COPY;
        SetBytes(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), &effect, sizeof(effect));
    } else if (it.kind == "image") {
        SetBytes(RegisterClipboardFormatW(L"PNG"), it.png.data(), it.png.size());
        // Plus a classic bottom-up 32bpp DIB for apps that don't read PNG.
        if (auto src = DecodeBytes(it.png.data(), it.png.size())) {
            ComPtr<IWICFormatConverter> conv;
            if (SUCCEEDED(Wic()->CreateFormatConverter(&conv)) &&
                SUCCEEDED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
                UINT w = 0, h = 0; conv->GetSize(&w, &h);
                std::vector<uint8_t> px((size_t)w * h * 4);
                if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()))) {
                    std::string dib(sizeof(BITMAPINFOHEADER) + px.size(), '\0');
                    auto* bi = (BITMAPINFOHEADER*)dib.data();
                    bi->biSize = sizeof(BITMAPINFOHEADER); bi->biWidth = (LONG)w; bi->biHeight = (LONG)h;
                    bi->biPlanes = 1; bi->biBitCount = 32; bi->biCompression = BI_RGB; bi->biSizeImage = (DWORD)px.size();
                    for (UINT y = 0; y < h; ++y)
                        memcpy(dib.data() + sizeof(BITMAPINFOHEADER) + (size_t)(h - 1 - y) * w * 4, px.data() + (size_t)y * w * 4, (size_t)w * 4);
                    SetBytes(CF_DIB, dib.data(), dib.size());
                }
            }
        }
    }
    CloseClipboard();
    ClipItem moved = std::move(*itp);
    items_.erase(itp);
    moved.time = NowMs();
    items_.insert(items_.begin(), std::move(moved));
    Changed();
    return true;
}

void ClipboardHistory::TogglePin(uint64_t id) {
    for (auto& it : items_) if (it.id == id) { it.pinned = !it.pinned; Changed(); return; }
}
void ClipboardHistory::Remove(uint64_t id) {
    for (size_t i = 0; i < items_.size(); ++i) if (items_[i].id == id) { items_.erase(items_.begin() + i); Changed(); return; }
}
void ClipboardHistory::Clear() {
    items_.erase(std::remove_if(items_.begin(), items_.end(), [](const ClipItem& c) { return !c.pinned; }), items_.end());
    Changed();
}

Json ClipboardHistory::ListJson(size_t max) const {
    Json arr = Json::array();
    for (size_t i = 0; i < items_.size() && i < max; ++i) {
        const ClipItem& it = items_[i];
        Json o = Json::object();
        o["id"] = (double)it.id;
        o["kind"] = it.kind;
        std::wstring t = it.text.substr(0, 400);
        o["text"] = Narrow(t);
        o["chars"] = (double)it.text.size();
        o["files"] = (double)it.files.size();
        o["w"] = it.w; o["h"] = it.h;
        o["time"] = (double)it.time;
        o["pinned"] = it.pinned;
        if (!it.thumb.empty()) o["thumb"] = "data:image/png;base64," + B64(it.thumb);
        arr.arr.push_back(std::move(o));
    }
    return arr;
}

Json ClipboardHistory::SelfTest() {
    Json r = Json::object();
    items_.clear();
    ClipItem t; t.kind = "text"; t.text = L"Selftest text, line one\nline two";
    Add(t);
    Add(t);                                           // a repeat moves to the top instead of duplicating
    ClipItem l; l.kind = "link"; l.text = L"https://open-meteo.com/en/docs";
    Add(l);
    std::vector<uint8_t> px(320 * 200 * 4);
    for (int y = 0; y < 200; ++y) for (int x = 0; x < 320; ++x) {
        uint8_t* p = &px[(y * 320 + x) * 4];
        p[0] = (uint8_t)(x * 255 / 319); p[1] = (uint8_t)(y * 255 / 199); p[2] = 0xd4; p[3] = 255;
    }
    ComPtr<IWICBitmap> bmp;
    if (Wic() && SUCCEEDED(Wic()->CreateBitmapFromMemory(320, 200, GUID_WICPixelFormat32bppBGRA, 320 * 4, (UINT)px.size(), px.data(), &bmp))) {
        ClipItem im; im.kind = "image"; im.png = EncodePng(bmp.Get()); im.thumb = EncodePng(Scaled(bmp.Get(), 160).Get()); im.w = 320; im.h = 200;
        r["pngBytes"] = (double)im.png.size();
        Add(im);
    }
    ClipItem f; f.kind = "files"; f.files = { L"C:\\Windows\\notepad.exe", L"C:\\Windows\\win.ini" };
    f.text = f.files[0] + L"\n" + f.files[1];
    Add(f);
    TogglePin(items_.back().id);                      // pin the oldest (the text)
    r["count"] = (double)items_.size();
    dirty_ = true; SaveNow();
    items_.clear(); loaded_ = false; nextId_ = 1;
    Load();
    r["reloaded"] = (double)items_.size();
    std::string kinds;
    for (auto& it : items_) { if (!kinds.empty()) kinds += ","; kinds += it.kind; if (it.pinned) kinds += "*"; }
    r["kinds"] = kinds;
    for (auto& it : items_) if (it.kind == "image") {
        std::vector<uint8_t> dec; int w = 0, h = 0;
        r["thumb"] = ImageUtil::DecodePng(it.thumb, 0, dec, w, h) ? std::to_string(w) + "x" + std::to_string(h) : std::string("fail");
    }
    for (auto& it : items_) if (it.kind == "text") r["textOk"] = it.text == L"Selftest text, line one\nline two";
    return r;
}

// ---------------------------------------------------------------- storage (DPAPI, this Windows user only)
void ClipboardHistory::SaveNow() {
    if (!dirty_) return;
    dirty_ = false;
    Json arr = Json::array();
    for (auto& it : items_) {
        Json o = Json::object();
        o["id"] = (double)it.id; o["kind"] = it.kind; o["text"] = Narrow(it.text);
        Json files = Json::array();
        for (auto& f : it.files) files.arr.push_back(Json(Narrow(f)));
        o["filesList"] = files;
        if (!it.png.empty()) o["png"] = B64(it.png);
        if (!it.thumb.empty()) o["thumb"] = B64(it.thumb);
        o["w"] = it.w; o["h"] = it.h; o["time"] = (double)it.time; o["pinned"] = it.pinned;
        o["hash"] = std::to_string(it.hash);
        arr.arr.push_back(std::move(o));
    }
    std::string plain = arr.dump();
    DATA_BLOB in{ (DWORD)plain.size(), (BYTE*)plain.data() }, out{};
    std::wstring path = LocalDataDir() + L"\\clipboard.bin";
    if (items_.empty()) { DeleteFileW(path.c_str()); return; }
    if (CryptProtectData(&in, L"Ionization clipboard", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        WriteFileBytes(path, std::string((const char*)out.pbData, out.cbData));
        SecureZeroMemory(plain.data(), plain.size());
        LocalFree(out.pbData);
    }
}

void ClipboardHistory::Load() {
    loaded_ = true;
    std::string enc;
    if (!ReadFileBytes(LocalDataDir() + L"\\clipboard.bin", enc) || enc.empty()) return;
    DATA_BLOB in{ (DWORD)enc.size(), (BYTE*)enc.data() }, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return;
    std::string plain((const char*)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    Json arr = Json::parse(plain);
    if (!arr.isArr()) return;
    for (auto& o : arr.arr) {
        ClipItem it;
        it.id = (uint64_t)o.num("id", 0);
        it.kind = o.str("kind", "");
        it.text = Widen(o.str("text", ""));
        if (const Json* f = o.find("filesList"); f && f->isArr()) for (auto& p : f->arr) if (p.isStr()) it.files.push_back(Widen(p.s));
        it.png = UnB64(o.str("png", ""));
        it.thumb = UnB64(o.str("thumb", ""));
        it.w = (int)o.num("w", 0); it.h = (int)o.num("h", 0);
        it.time = (long long)o.num("time", 0);
        it.pinned = o.boolean("pinned", false);
        it.hash = _strtoui64(o.str("hash", "0").c_str(), nullptr, 10);
        if (it.kind.empty() || !it.id) continue;
        nextId_ = std::max(nextId_, it.id + 1);
        items_.push_back(std::move(it));
    }
}
