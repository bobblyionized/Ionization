// Minimal JSON value + parser + serializer (UTF-8). Enough for settings, bridge
// messages and the GitHub releases API.
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdlib>
#include <cstdio>

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    Json() = default;
    Json(bool v) : type(Bool), b(v) {}
    Json(int v) : type(Number), n(v) {}
    Json(double v) : type(Number), n(v) {}
    Json(const char* v) : type(String), s(v) {}
    Json(std::string v) : type(String), s(std::move(v)) {}
    static Json object() { Json j; j.type = Object; return j; }
    static Json array() { Json j; j.type = Array; return j; }

    bool isObj() const { return type == Object; }
    bool isArr() const { return type == Array; }
    bool isNum() const { return type == Number; }
    bool isStr() const { return type == String; }
    bool isBool() const { return type == Bool; }

    const Json* find(const std::string& k) const {
        if (type != Object) return nullptr;
        for (auto& p : obj) if (p.first == k) return &p.second;
        return nullptr;
    }
    Json& operator[](const std::string& k) {
        if (type != Object) { type = Object; obj.clear(); }
        for (auto& p : obj) if (p.first == k) return p.second;
        obj.emplace_back(k, Json());
        return obj.back().second;
    }
    // Dotted-path getters with defaults.
    const Json* path(const char* p) const {
        const Json* cur = this;
        std::string key;
        for (const char* c = p;; ++c) {
            if (*c == '.' || *c == 0) {
                if (!cur) return nullptr;
                cur = cur->find(key);
                key.clear();
                if (*c == 0) break;
            } else key += *c;
        }
        return cur;
    }
    double num(const char* p, double def) const {
        const Json* j = path(p);
        if (j && j->type == Number) return j->n;
        if (j && j->type == Bool) return j->b ? 1 : 0;
        return def;
    }
    bool boolean(const char* p, bool def) const {
        const Json* j = path(p);
        if (j && j->type == Bool) return j->b;
        if (j && j->type == Number) return j->n != 0;
        return def;
    }
    std::string str(const char* p, const char* def) const {
        const Json* j = path(p);
        return (j && j->type == String) ? j->s : std::string(def);
    }

    // ---------------- serialize ----------------
    static void escape(std::string& out, const std::string& v) {
        out += '"';
        for (unsigned char c : v) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char buf[8]; snprintf(buf, 8, "\\u%04x", c); out += buf; }
                else out += (char)c;
            }
        }
        out += '"';
    }
    void dumpTo(std::string& out) const {
        switch (type) {
        case Null: out += "null"; break;
        case Bool: out += b ? "true" : "false"; break;
        case Number: {
            char buf[64];
            if (n == (long long)n && n < 1e15 && n > -1e15) snprintf(buf, 64, "%lld", (long long)n);
            else snprintf(buf, 64, "%.6g", n);
            out += buf; break;
        }
        case String: escape(out, s); break;
        case Array:
            out += '[';
            for (size_t i = 0; i < arr.size(); ++i) { if (i) out += ','; arr[i].dumpTo(out); }
            out += ']'; break;
        case Object:
            out += '{';
            for (size_t i = 0; i < obj.size(); ++i) {
                if (i) out += ',';
                escape(out, obj[i].first); out += ':'; obj[i].second.dumpTo(out);
            }
            out += '}'; break;
        }
    }
    std::string dump() const { std::string o; dumpTo(o); return o; }

    // ---------------- parse ----------------
    static Json parse(const std::string& text, bool* ok = nullptr) {
        size_t i = 0; bool good = true;
        Json j = parseValue(text, i, good, 0);
        skipWs(text, i);
        if (i != text.size()) good = false;
        if (ok) *ok = good;
        return good ? j : Json();
    }
private:
    static void skipWs(const std::string& t, size_t& i) {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
    }
    static void putUtf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    static unsigned hex4(const std::string& t, size_t i, bool& good) {
        if (i + 4 > t.size()) { good = false; return 0; }
        unsigned v = 0;
        for (size_t k = 0; k < 4; ++k) {
            char c = t[i + k]; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else { good = false; return 0; }
        }
        return v;
    }
    static std::string parseString(const std::string& t, size_t& i, bool& good) {
        std::string o;
        ++i; // opening quote
        while (i < t.size()) {
            char c = t[i++];
            if (c == '"') return o;
            if (c == '\\') {
                if (i >= t.size()) break;
                char e = t[i++];
                switch (e) {
                case 'n': o += '\n'; break; case 't': o += '\t'; break;
                case 'r': o += '\r'; break; case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'u': {
                    unsigned cp = hex4(t, i, good); i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        unsigned lo = hex4(t, i + 2, good); i += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    putUtf8(o, cp); break;
                }
                default: o += e;
                }
            } else o += c;
        }
        good = false; return o;
    }
    static Json parseValue(const std::string& t, size_t& i, bool& good, int depth) {
        skipWs(t, i);
        if (i >= t.size() || depth > 64) { good = false; return Json(); }
        char c = t[i];
        if (c == '{') {
            Json j = object(); ++i; skipWs(t, i);
            if (i < t.size() && t[i] == '}') { ++i; return j; }
            while (good) {
                skipWs(t, i);
                if (i >= t.size() || t[i] != '"') { good = false; break; }
                std::string k = parseString(t, i, good);
                skipWs(t, i);
                if (i >= t.size() || t[i] != ':') { good = false; break; }
                ++i;
                j.obj.emplace_back(std::move(k), parseValue(t, i, good, depth + 1));
                skipWs(t, i);
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == '}') { ++i; break; }
                good = false;
            }
            return j;
        }
        if (c == '[') {
            Json j = array(); ++i; skipWs(t, i);
            if (i < t.size() && t[i] == ']') { ++i; return j; }
            while (good) {
                j.arr.push_back(parseValue(t, i, good, depth + 1));
                skipWs(t, i);
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == ']') { ++i; break; }
                good = false;
            }
            return j;
        }
        if (c == '"') return Json(parseString(t, i, good));
        if (t.compare(i, 4, "true") == 0) { i += 4; return Json(true); }
        if (t.compare(i, 5, "false") == 0) { i += 5; return Json(false); }
        if (t.compare(i, 4, "null") == 0) { i += 4; return Json(); }
        const char* start = t.c_str() + i; char* end = nullptr;
        double v = strtod(start, &end);
        if (end == start) { good = false; return Json(); }
        i += end - start;
        return Json(v);
    }
};
