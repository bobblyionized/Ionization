#include "net.h"
#include <windows.h>
#include <winhttp.h>

bool HttpGetText(const std::wstring& url, std::string& body, std::string& err) {
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = "bad url"; return false; }
    std::wstring full = std::wstring(path) + extra;
    HINTERNET s = WinHttpOpen(L"Ionization", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!s) { err = "WinHttpOpen failed"; return false; }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(s, 8000, 8000, 10000, 15000);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", full.c_str(), nullptr, nullptr, nullptr,
        uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = false;
    if (r && WinHttpSendRequest(r, L"Accept: application/json\r\n", (DWORD)-1L, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
        body.clear();
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(r, &avail) && avail && body.size() < 4u * 1024 * 1024) {
            size_t off = body.size();
            body.resize(off + avail);
            DWORD rd = 0;
            if (!WinHttpReadData(r, body.data() + off, avail, &rd)) break;
            body.resize(off + rd);
        }
        ok = code == 200;
        if (!ok) err = "HTTP " + std::to_string(code);
    } else err = "network error " + std::to_string(GetLastError());
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

std::wstring UrlEncode(const std::string& utf8) {
    std::wstring out;
    const char* hex = "0123456789ABCDEF";
    for (unsigned char ch : utf8) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') out += (wchar_t)ch;
        else { out += L'%'; out += (wchar_t)hex[ch >> 4]; out += (wchar_t)hex[ch & 15]; }
    }
    return out;
}
