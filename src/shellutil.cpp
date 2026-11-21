#include "shellutil.h"
#include "common.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace ShellUtil {

std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

bool IsAppWindow(HWND h) {
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return false;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    if (ex & WS_EX_NOACTIVATE && !(ex & WS_EX_APPWINDOW)) return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Windows.UI.Core.CoreWindow")) return false;
    return true;
}

std::wstring WindowAumid(HWND h) {
    ComPtr<IPropertyStore> ps;
    if (FAILED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps)))) return {};
    PROPVARIANT v; PropVariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal) out = v.pwszVal;
    PropVariantClear(&v);
    return out;
}

std::wstring WindowExe(HWND h) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t buf[MAX_PATH * 2]; DWORD n = (DWORD)std::size(buf);
    std::wstring out;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) out.assign(buf, n);
    CloseHandle(p);
    return out;
}

bool ResolveLink(const std::wstring& lnk, Link& out) {
    ComPtr<IShellLinkW> sl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    ComPtr<IPersistFile> pf;
    if (FAILED(sl.As(&pf)) || FAILED(pf->Load(lnk.c_str(), STGM_READ))) return false;
    out.path = lnk;
    wchar_t buf[MAX_PATH * 2] = {};
    if (SUCCEEDED(sl->GetPath(buf, (int)std::size(buf), nullptr, SLGP_RAWPATH))) {
        wchar_t exp[MAX_PATH * 2] = {};
        ExpandEnvironmentStringsW(buf, exp, (DWORD)std::size(exp));
        out.target = exp;
    }
    ComPtr<IPropertyStore> ps;
    if (SUCCEEDED(sl.As(&ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal) out.aumid = v.pwszVal;
        PropVariantClear(&v);
    }
    std::wstring file = lnk.substr(lnk.find_last_of(L"\\/") + 1);
    out.name = file.size() > 4 ? file.substr(0, file.size() - 4) : file;
    return true;
}

std::vector<std::wstring> TaskbarPins() {
    std::vector<std::wstring> out;
    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) return out;
    std::wstring dir = std::wstring(roaming) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar\\";
    CoTaskMemFree(roaming);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"*.lnk").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do { out.push_back(dir + fd.cFileName); } while (FindNextFileW(f, &fd));
    FindClose(f);
    return out;
}

std::wstring DisplayName(const std::wstring& parsingName) {
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(parsingName.c_str(), nullptr, IID_PPV_ARGS(&item)))) return {};
    PWSTR name = nullptr;
    std::wstring out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name))) { out = name; CoTaskMemFree(name); }
    return out;
}

std::wstring FileDescription(const std::wstring& exe) {
    DWORD h = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &h);
    std::wstring out;
    if (size) {
        std::vector<BYTE> data(size);
        if (GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) {
            struct LangCp { WORD lang, cp; }* tr = nullptr; UINT len = 0;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) && len >= sizeof(LangCp)) {
                wchar_t key[64];
                swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].cp);
                wchar_t* desc = nullptr; UINT dl = 0;
                if (VerQueryValueW(data.data(), key, (void**)&desc, &dl) && dl > 1) out = desc;
            }
        }
    }
    if (out.empty()) {
        size_t s = exe.find_last_of(L"\\/");
        out = exe.substr(s + 1);
        if (out.size() > 4) out = out.substr(0, out.size() - 4);
    }
    return out;
}

HBITMAP Icon(const std::wstring& parsingName, int size) {
    ComPtr<IShellItemImageFactory> f;
    if (FAILED(SHCreateItemFromParsingName(parsingName.c_str(), nullptr, IID_PPV_ARGS(&f)))) return nullptr;
    HBITMAP bmp = nullptr;
    if (FAILED(f->GetImage({ size, size }, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bmp))) return nullptr;
    return bmp;
}

void Launch(const std::wstring& parsingName) {
    ShellExecuteW(nullptr, L"open", parsingName.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void SendKeys(std::initializer_list<WORD> vks) {
    std::vector<INPUT> in;
    for (WORD vk : vks) { INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = vk; in.push_back(i); }
    for (auto it = vks.end(); it != vks.begin();) { --it; INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i); }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

} // namespace ShellUtil
