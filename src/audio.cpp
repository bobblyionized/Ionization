#include "audio.h"
#include "common.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wbemidl.h>
#include <wrl/client.h>
#include <atomic>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<IAudioEndpointVolume> Endpoint(EDataFlow flow, ERole role) {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return nullptr;
    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(flow, role, &dev))) return nullptr;
    ComPtr<IAudioEndpointVolume> vol;
    if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)vol.GetAddressOf()))) return nullptr;
    return vol;
}

struct VolumeCallback : IAudioEndpointVolumeCallback {
    std::atomic<ULONG> refs{ 1 };
    HWND notify = nullptr; UINT msg = 0;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --refs; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioEndpointVolumeCallback)) { *out = this; AddRef(); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
        if (notify) PostMessageW(notify, msg, (WPARAM)(d->fMasterVolume * 1000), d->bMuted);
        return S_OK;
    }
};
ComPtr<IAudioEndpointVolume> g_watched;
VolumeCallback* g_cb = nullptr;

} // namespace

namespace Audio {
float Volume() {
    auto v = Endpoint(eRender, eConsole);
    float f = -1;
    if (v) v->GetMasterVolumeLevelScalar(&f);
    return f;
}
void SetVolume(float f) {
    if (auto v = Endpoint(eRender, eConsole)) {
        v->SetMasterVolumeLevelScalar(f < 0 ? 0 : f > 1 ? 1 : f, nullptr);
        v->SetMute(FALSE, nullptr);
    }
}
bool SpeakerMuted() {
    BOOL m = FALSE;
    if (auto v = Endpoint(eRender, eConsole)) v->GetMute(&m);
    return m != FALSE;
}
int MicMuted() {
    auto v = Endpoint(eCapture, eCommunications);
    if (!v) v = Endpoint(eCapture, eConsole);
    if (!v) return -1;
    BOOL m = FALSE;
    v->GetMute(&m);
    return m ? 1 : 0;
}
void SetMicMuted(bool m) {
    // Mute both default capture roles so every app is covered.
    for (ERole role : { eCommunications, eConsole })
        if (auto v = Endpoint(eCapture, role)) v->SetMute(m ? TRUE : FALSE, nullptr);
}
void WatchVolume(HWND notify, UINT msg) {
    StopWatching();
    g_watched = Endpoint(eRender, eConsole);
    if (!g_watched) return;
    g_cb = new VolumeCallback();
    g_cb->notify = notify; g_cb->msg = msg;
    g_watched->RegisterControlChangeNotify(g_cb);
}
void StopWatching() {
    if (g_watched && g_cb) g_watched->UnregisterControlChangeNotify(g_cb);
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    g_watched.Reset();
}
} // namespace Audio

namespace Brightness {
static ComPtr<IWbemServices> Wmi() {
    ComPtr<IWbemLocator> loc;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&loc)))) return nullptr;
    ComPtr<IWbemServices> svc;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    HRESULT hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
    SysFreeString(ns);
    if (FAILED(hr)) return nullptr;
    CoSetProxyBlanket(svc.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return svc;
}
int Get() {
    auto svc = Wmi();
    if (!svc) return -1;
    ComPtr<IEnumWbemClassObject> en;
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(L"SELECT CurrentBrightness FROM WmiMonitorBrightness");
    HRESULT hr = svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(lang); SysFreeString(q);
    if (FAILED(hr)) return -1;
    ComPtr<IWbemClassObject> obj; ULONG n = 0;
    int out = -1;
    if (en->Next(1000, 1, &obj, &n) == S_OK && n) {
        VARIANT v; VariantInit(&v);
        if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr))) out = v.bVal;
        VariantClear(&v);
    }
    return out;
}
void Set(int value) {
    auto svc = Wmi();
    if (!svc) return;
    ComPtr<IEnumWbemClassObject> en;
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessMethods");
    HRESULT hr = svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(lang); SysFreeString(q);
    if (FAILED(hr)) return;
    ComPtr<IWbemClassObject> inst; ULONG n = 0;
    if (en->Next(1000, 1, &inst, &n) != S_OK || !n) return;
    VARIANT path; VariantInit(&path);
    inst->Get(L"__PATH", 0, &path, nullptr, nullptr);
    ComPtr<IWbemClassObject> cls, inDef, args;
    BSTR cname = SysAllocString(L"WmiMonitorBrightnessMethods"), mname = SysAllocString(L"WmiSetBrightness");
    if (SUCCEEDED(svc->GetObject(cname, 0, nullptr, &cls, nullptr)) && SUCCEEDED(cls->GetMethod(mname, 0, &inDef, nullptr)) &&
        SUCCEEDED(inDef->SpawnInstance(0, &args))) {
        VARIANT t; VariantInit(&t); t.vt = VT_I4; t.lVal = 0;
        VARIANT b; VariantInit(&b); b.vt = VT_UI1; b.bVal = (BYTE)(value < 0 ? 0 : value > 100 ? 100 : value);
        args->Put(L"Timeout", 0, &t, 0);
        args->Put(L"Brightness", 0, &b, 0);
        svc->ExecMethod(path.bstrVal, mname, 0, nullptr, args.Get(), nullptr, nullptr);
    }
    SysFreeString(cname); SysFreeString(mname);
    VariantClear(&path);
}
} // namespace Brightness

namespace Privacy {
// An app is using the device if its record has a start time and no stop time yet.
static bool Scan(HKEY root, const wchar_t* path, ULONGLONG* since, int depth = 0) {
    HKEY k;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool used = false;
    ULONGLONG start = 0, stop = 0; DWORD sz = sizeof(ULONGLONG);
    if (RegQueryValueExW(k, L"LastUsedTimeStart", nullptr, nullptr, (BYTE*)&start, &sz) == ERROR_SUCCESS) {
        sz = sizeof(ULONGLONG);
        RegQueryValueExW(k, L"LastUsedTimeStop", nullptr, nullptr, (BYTE*)&stop, &sz);
        if (start && !stop) {
            used = true;
            if (since) {
                // FILETIME -> ms ago -> tick count at start
                FILETIME now; GetSystemTimeAsFileTime(&now);
                ULONGLONG n = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
                ULONGLONG ago = n > start ? (n - start) / 10000 : 0;
                ULONGLONG t = GetTickCount64() > ago ? GetTickCount64() - ago : 0;
                if (!*since || t < *since) *since = t;
            }
        }
    }
    if (depth < 2) {
        wchar_t name[512];
        for (DWORD i = 0;; ++i) {
            DWORD len = 512;
            if (RegEnumKeyExW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::wstring sub = std::wstring(path) + L"\\" + name;
            if (Scan(root, sub.c_str(), since, depth + 1)) used = true;
        }
    }
    RegCloseKey(k);
    return used;
}
InUse Poll() {
    InUse u;
    const wchar_t* base = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";
    u.mic = Scan(HKEY_CURRENT_USER, (std::wstring(base) + L"microphone").c_str(), &u.micSinceMs);
    u.camera = Scan(HKEY_CURRENT_USER, (std::wstring(base) + L"webcam").c_str(), nullptr);
    return u;
}
} // namespace Privacy
