#include "updater.h"
#include "common.h"
#include "json.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <atomic>
#include <thread>
#include <functional>
#include <vector>

namespace {

const wchar_t* kApi = L"https://api.github.com/repos/bobblyionized/Ionization/releases/latest";
std::atomic<bool> g_busy{ false };

void Post(HWND notify, Json j) {
    auto* s = new std::string(j.dump());
    if (!PostMessageW(notify, WM_APP_UPDATE, 0, (LPARAM)s)) delete s;
}

Json Status(const char* status) { Json j = Json::object(); j["status"] = status; return j; }

bool HttpGet(const std::wstring& url, std::string& body, std::string& err,
             const std::function<void(unsigned long long, unsigned long long)>& progress = {}) {
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    wchar_t extra[2048] = {};
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = "bad url"; return false; }
    std::wstring fullPath = std::wstring(path) + extra;

    HINTERNET s = WinHttpOpen(L"Ionization-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!s) { err = "WinHttpOpen failed"; return false; }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", fullPath.c_str(), nullptr, nullptr, nullptr,
        uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = false;
    if (r) {
        const wchar_t* hdrs = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
        if (WinHttpSendRequest(r, hdrs, (DWORD)-1L, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD code = 0, sz = sizeof(code);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
            unsigned long long total = 0;
            wchar_t lenBuf[32] = {}; DWORD lsz = sizeof(lenBuf);
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, lenBuf, &lsz, nullptr)) total = _wcstoui64(lenBuf, nullptr, 10);
            body.clear();
            if (total) body.reserve((size_t)total);
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(r, &avail) && avail) {
                size_t off = body.size();
                body.resize(off + avail);
                DWORD rd = 0;
                if (!WinHttpReadData(r, body.data() + off, avail, &rd)) break;
                body.resize(off + rd);
                if (progress) progress(body.size(), total);
            }
            if (code == 200) ok = true;
            else err = "HTTP " + std::to_string(code);
        } else err = "network error " + std::to_string(GetLastError());
    } else err = "connect failed";
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

std::vector<int> ParseVersion(const std::string& v) {
    std::vector<int> out;
    int cur = -1;
    for (char ch : v) {
        if (ch >= '0' && ch <= '9') cur = (cur < 0 ? 0 : cur * 10) + (ch - '0');
        else if (cur >= 0) { out.push_back(cur); cur = -1; if (ch != '.') break; }
    }
    if (cur >= 0) out.push_back(cur);
    return out;
}

bool Newer(const std::string& remote, const std::string& local) {
    auto a = ParseVersion(remote), b = ParseVersion(local);
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

std::string Sha256Hex(const std::string& data) {
    unsigned char hash[32] = {};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), hash, 32) != 0) return {};
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char b : hash) { out += hex[b >> 4]; out += hex[b & 15]; }
    return out;
}

std::wstring ExeDir() {
    std::wstring p = ExePath();
    return p.substr(0, p.find_last_of(L'\\'));
}

void Run(HWND notify, bool install) {
    Post(notify, Status("checking"));
    std::string body, err;
    if (!HttpGet(kApi, body, err)) {
        Json j = Status("error"); j["error"] = err == "HTTP 404" ? "No releases published yet" : err;
        Post(notify, j); return;
    }
    Json rel = Json::parse(body);
    std::string tag = rel.str("tag_name", "");
    std::string exeUrl, shaUrl;
    if (const Json* assets = rel.find("assets"); assets && assets->isArr()) {
        for (auto& a : assets->arr) {
            std::string n = a.str("name", "");
            if (n == "Ionization.exe") exeUrl = a.str("browser_download_url", "");
            if (n == "Ionization.exe.sha256") shaUrl = a.str("browser_download_url", "");
        }
    }
    if (tag.empty() || exeUrl.empty() || shaUrl.empty()) {
        Json j = Status("error"); j["error"] = "Release is missing assets"; Post(notify, j); return;
    }
    // A version string without three numeric parts is a broken stamp: never self-update from it.
    if (ParseVersion(IONIZATION_VERSION).size() < 3) {
        Json j = Status("error"); j["error"] = "This build has no valid version stamp; download the latest release manually";
        Post(notify, j); return;
    }
    if (!Newer(tag, IONIZATION_VERSION)) {
        Json j = Status("current"); j["latest"] = tag; Post(notify, j); return;
    }
    if (!install) { Json j = Status("available"); j["latest"] = tag; Post(notify, j); return; }

    std::string shaText;
    if (!HttpGet(Widen(shaUrl), shaText, err)) { Json j = Status("error"); j["error"] = "checksum: " + err; Post(notify, j); return; }
    std::string expected;
    for (char ch : shaText) {
        if (isxdigit((unsigned char)ch)) expected += (char)tolower((unsigned char)ch);
        else if (!expected.empty()) break;
    }
    if (expected.size() != 64) { Json j = Status("error"); j["error"] = "Bad checksum file"; Post(notify, j); return; }

    std::string exe;
    int lastPct = -1;
    bool got = HttpGet(Widen(exeUrl), exe, err, [&](unsigned long long done, unsigned long long total) {
        int pct = total ? (int)(done * 100 / total) : 0;
        if (pct != lastPct && pct % 5 == 0) {
            lastPct = pct;
            Json j = Status("downloading"); j["latest"] = tag; j["progress"] = pct; Post(notify, j);
        }
    });
    if (!got) { Json j = Status("error"); j["error"] = "download: " + err; Post(notify, j); return; }
    if (Sha256Hex(exe) != expected) { Json j = Status("error"); j["error"] = "Checksum mismatch, update skipped"; Post(notify, j); return; }

    std::wstring dir = LocalDataDir() + L"\\update";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring staged = dir + L"\\Ionization.new.exe";
    if (!WriteFileBytes(staged, exe)) { Json j = Status("error"); j["error"] = "Could not save update"; Post(notify, j); return; }

    std::wstring cur = ExePath();
    std::wstring old = ExeDir() + L"\\Ionization.old.exe";
    DeleteFileW(old.c_str());
    if (!MoveFileExW(cur.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Json j = Status("error"); j["error"] = "Folder is read-only, move Ionization.exe somewhere writable"; Post(notify, j); return;
    }
    if (!MoveFileExW(staged.c_str(), cur.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old.c_str(), cur.c_str(), MOVEFILE_REPLACE_EXISTING);
        Json j = Status("error"); j["error"] = "Could not replace exe"; Post(notify, j); return;
    }
    LogLine("Updated %s -> %s", IONIZATION_VERSION, tag.c_str());
    Json j = Status("restart"); j["latest"] = tag; Post(notify, j);
}

} // namespace

namespace Updater {

bool IsDevBuild() { return strstr(IONIZATION_VERSION, "dev") != nullptr; }

void CheckAsync(HWND notify, bool install) {
    if (IsDevBuild()) { Json j = Status("dev"); Post(notify, j); return; }
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return;
    std::thread([notify, install] {
        Run(notify, install);
        g_busy = false;
    }).detach();
}

void CleanupOldExe() {
    std::wstring old = ExeDir() + L"\\Ionization.old.exe";
    for (int i = 0; i < 20 && GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES; ++i) {
        if (DeleteFileW(old.c_str())) break;
        Sleep(150);
    }
}

bool Relaunch(bool showPanel) {
    std::wstring cmd = L"\"" + ExePath() + L"\" --updated --wait-pid " + std::to_wstring(GetCurrentProcessId());
    if (!showPanel) cmd += L" --tray";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace Updater
