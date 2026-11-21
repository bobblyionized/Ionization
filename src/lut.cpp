#include "lut.h"
#include "common.h"
#include <cmath>
#include <sstream>
#include <algorithm>
#include <functional>

namespace {

struct V3 { float r, g, b; };
inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
inline float luma(V3 c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }
inline V3 mix(V3 a, V3 b, float t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }
inline V3 sat(V3 c, float s) { float l = luma(c); return { l + (c.r - l) * s, l + (c.g - l) * s, l + (c.b - l) * s }; }
inline float scurve(float x, float k) { // k>0 adds contrast around 0.5
    float y = x - 0.5f;
    return 0.5f + y * (1 + k) / (1 + k * std::fabs(2 * y));
}
inline V3 curve(V3 c, float k) { return { scurve(c.r, k), scurve(c.g, k), scurve(c.b, k) }; }
inline V3 lift(V3 c, float lo, float hi) { return { lo + c.r * (hi - lo), lo + c.g * (hi - lo), lo + c.b * (hi - lo) }; }
inline V3 add(V3 c, V3 d, float w) { return { c.r + d.r * w, c.g + d.g * w, c.b + d.b * w }; }
inline V3 mul(V3 c, V3 m) { return { c.r * m.r, c.g * m.g, c.b * m.b }; }
inline V3 clampv(V3 c) { return { clamp01(c.r), clamp01(c.g), clamp01(c.b) }; }

struct Look { const char* id; const char* name; std::function<V3(V3)> fn; };

const std::vector<Look>& Looks() {
    static const std::vector<Look> looks = {
        { "teal-orange", "Teal & Orange", [](V3 c) {
            float l = luma(c);
            c = add(c, { -0.06f, 0.015f, 0.07f }, (1 - l) * 0.9f);
            c = add(c, { 0.09f, 0.025f, -0.08f }, l * 0.9f);
            c = sat(c, 1.12f);
            return curve(c, 0.18f); } },
        { "warm-film", "Warm Film", [](V3 c) {
            c = curve(c, 0.12f);
            c = mul(c, { 1.06f, 1.0f, 0.88f });
            c = sat(c, 0.9f);
            return lift(c, 0.035f, 0.97f); } },
        { "cold-steel", "Cold Steel", [](V3 c) {
            c = sat(c, 0.72f);
            c = mul(c, { 0.92f, 1.0f, 1.1f });
            return curve(c, 0.25f); } },
        { "faded-matte", "Faded Matte", [](V3 c) {
            c = sat(c, 0.82f);
            c = curve(c, -0.08f);
            return lift(c, 0.09f, 0.93f); } },
        { "bleach-bypass", "Bleach Bypass", [](V3 c) {
            float l = luma(c);
            V3 o = { l < 0.5f ? 2 * c.r * l : 1 - 2 * (1 - c.r) * (1 - l),
                     l < 0.5f ? 2 * c.g * l : 1 - 2 * (1 - c.g) * (1 - l),
                     l < 0.5f ? 2 * c.b * l : 1 - 2 * (1 - c.b) * (1 - l) };
            c = mix(c, o, 0.55f);
            c = sat(c, 0.55f);
            return curve(c, 0.15f); } },
        { "cross-process", "Cross Process", [](V3 c) {
            V3 o = { scurve(c.r, 0.35f), scurve(c.g, 0.2f), 0.12f + c.b * 0.76f };
            o = add(o, { 0.0f, 0.03f, -0.02f }, 1 - luma(c));
            o = add(o, { 0.04f, 0.0f, 0.05f }, luma(c));
            return sat(o, 1.08f); } },
        { "amber-night", "Amber Night", [](V3 c) {
            c = mul(c, { 1.0f, 0.84f, 0.56f });
            c = sat(c, 0.92f);
            return curve(c, 0.05f); } },
        { "mono", "Mono", [](V3 c) {
            float l = scurve(luma(c), 0.22f);
            return V3{ l, l, l }; } },
        { "vivid", "Vivid", [](V3 c) {
            c = sat(c, 1.35f);
            return curve(c, 0.14f); } },
    };
    return looks;
}

std::wstring LutDir() {
    std::wstring d = AppDataDir() + L"\\luts";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

} // namespace

std::vector<LutEntry> ListLuts() {
    std::vector<LutEntry> out;
    out.push_back({ "neutral", "Neutral", "built-in", 0 });
    for (auto& l : Looks()) out.push_back({ l.id, l.name, "built-in", 33 });
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((LutDir() + L"\\*.cube").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring fn = fd.cFileName;
            std::wstring stem = fn.substr(0, fn.size() - 5);
            LutEntry e;
            e.id = "file:" + Narrow(fn);
            e.name = Narrow(stem);
            e.kind = "imported";
            // Peek size from header (cheap).
            std::string text;
            if (ReadFileBytes(LutDir() + L"\\" + fn, text)) {
                size_t p = text.find("LUT_3D_SIZE");
                if (p != std::string::npos) e.size = atoi(text.c_str() + p + 11);
            }
            out.push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}

bool ParseCube(const std::string& text, LutData& out, std::string& error) {
    std::istringstream in(text);
    std::string line;
    int size = 0;
    float dmin[3] = { 0, 0, 0 }, dmax[3] = { 1, 1, 1 };
    std::vector<float> data;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos || line[s] == '#') continue;
        line = line.substr(s);
        if (line.rfind("TITLE", 0) == 0) continue;
        if (line.rfind("LUT_1D_SIZE", 0) == 0) { error = "1D LUTs are not supported"; return false; }
        if (line.rfind("LUT_3D_SIZE", 0) == 0) { size = atoi(line.c_str() + 11); continue; }
        if (line.rfind("DOMAIN_MIN", 0) == 0) { sscanf_s(line.c_str() + 10, "%f %f %f", &dmin[0], &dmin[1], &dmin[2]); continue; }
        if (line.rfind("DOMAIN_MAX", 0) == 0) { sscanf_s(line.c_str() + 10, "%f %f %f", &dmax[0], &dmax[1], &dmax[2]); continue; }
        if ((line[0] >= '0' && line[0] <= '9') || line[0] == '-' || line[0] == '.' || line[0] == '+') {
            float r, g, b;
            if (sscanf_s(line.c_str(), "%f %f %f", &r, &g, &b) == 3) {
                data.push_back((r - dmin[0]) / (dmax[0] - dmin[0]));
                data.push_back((g - dmin[1]) / (dmax[1] - dmin[1]));
                data.push_back((b - dmin[2]) / (dmax[2] - dmin[2]));
            }
        }
    }
    if (size < 2 || size > 129) { error = "Missing or invalid LUT_3D_SIZE"; return false; }
    if (data.size() != (size_t)size * size * size * 3) {
        error = "Expected " + std::to_string(size * size * size) + " entries, found " + std::to_string(data.size() / 3);
        return false;
    }
    out.size = size;
    out.rgb = std::move(data);
    return true;
}

bool LoadLut(const std::string& id, LutData& out) {
    if (id.rfind("file:", 0) == 0) {
        std::string text, err;
        if (!ReadFileBytes(LutDir() + L"\\" + Widen(id.substr(5)), text)) return false;
        if (!ParseCube(text, out, err)) { LogLine("LUT %s: %s", id.c_str(), err.c_str()); return false; }
        return true;
    }
    for (auto& l : Looks()) {
        if (id != l.id) continue;
        const int N = 33;
        out.size = N;
        out.rgb.resize(N * N * N * 3);
        size_t k = 0;
        for (int b = 0; b < N; ++b)
            for (int g = 0; g < N; ++g)
                for (int r = 0; r < N; ++r) {
                    V3 c = clampv(l.fn({ r / float(N - 1), g / float(N - 1), b / float(N - 1) }));
                    out.rgb[k++] = c.r; out.rgb[k++] = c.g; out.rgb[k++] = c.b;
                }
        return true;
    }
    return false;
}

bool ImportCube(const std::wstring& src, std::string& outId, std::string& error) {
    std::string text;
    if (!ReadFileBytes(src, text)) { error = "Could not read file"; return false; }
    LutData tmp;
    if (!ParseCube(text, tmp, error)) return false;
    size_t slash = src.find_last_of(L"\\/");
    std::wstring fn = slash == std::wstring::npos ? src : src.substr(slash + 1);
    if (fn.size() < 5 || _wcsicmp(fn.c_str() + fn.size() - 5, L".cube") != 0) fn += L".cube";
    if (!WriteFileBytes(LutDir() + L"\\" + fn, text)) { error = "Could not save LUT"; return false; }
    outId = "file:" + Narrow(fn);
    return true;
}
