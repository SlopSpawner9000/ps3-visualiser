#include "presetlib.h"
#include "firmware.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace presetlib {
namespace {

constexpr const char *kPrefix = "PS3VIS-PRESET1:";
constexpr const char *kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const std::string &in)
{
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8 | uint8_t(in[i + 2]);
        for (int k = 18; k >= 0; k -= 6)
            out += kAlphabet[(v >> k) & 63];
    }
    if (i < in.size()) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16;
        if (i + 1 < in.size())
            v |= uint32_t(uint8_t(in[i + 1])) << 8;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < in.size() ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

bool unbase64(const std::string &in, std::string &out)
{
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        if (ch == '=' || ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t')
            continue;
        const char *p = std::strchr(kAlphabet, ch);
        if (!p || !ch)
            return false;
        acc = acc << 6 | uint32_t(p - kAlphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((acc >> bits) & 0xFF);
        }
    }
    return true;
}

std::string trim(std::string s)
{
    const auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// File names: keep letters, digits, spaces, - and _.
std::string safeName(const std::string &name)
{
    std::string out;
    for (unsigned char c : name)
        out += (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c >= 0x80) ? char(c) : '_';
    out = trim(out);
    return out.empty() ? "Preset" : out.substr(0, 60);
}

// Library cache (list()).
std::vector<Entry> gCache;
Uint64 gCacheAt = 0;
bool gCacheValid = false;
void invalidate() { gCacheValid = false; }

} // namespace

std::string cleanName(const std::string &name)
{
    std::string out;
    for (unsigned char ch : name)
        if (ch >= 0x20 && ch != 0x7f)
            out += char(ch);
    out = trim(out);
    if (out.size() > 60) {
        out.resize(60);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xc0) == 0x80)
            out.pop_back(); // don't cut a character in half
        if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xc0)
            out.pop_back();
    }
    return out;
}

std::string encode(const SharedPreset &p)
{
    std::ostringstream text;
    text << "name=" << p.name << "\n";
    for (auto &[k, v] : p.values) {
        char num[32];
        std::snprintf(num, sizeof num, "%.6g", double(v));
        text << k << "=" << num << "\n";
    }
    return kPrefix + base64(text.str());
}

bool decode(const std::string &input, const std::vector<std::string> &known, SharedPreset &out)
{
    const std::string text = trim(input);
    const size_t at = text.find(kPrefix);
    if (at == std::string::npos)
        return false;
    std::string body;
    if (!unbase64(text.substr(at + std::strlen(kPrefix)), body))
        return false;
    const std::set<std::string> names(known.begin(), known.end());
    out = {};
    std::istringstream lines(body);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "name") {
            out.name = cleanName(value);
        } else if (names.count(key)) {
            // Sanity: numbers only, finite, and within a generous range.
            char *end = nullptr;
            const float v = std::strtof(value.c_str(), &end);
            if (end != value.c_str() && std::isfinite(v))
                out.values[key] = std::clamp(v, -100000.0f, 100000.0f);
        }
    }
    if (out.name.empty())
        out.name = "Imported preset";
    return !out.values.empty();
}

std::filesystem::path folder()
{
    std::filesystem::path dir;
    if (char *pref = SDL_GetPrefPath("ps3vis", "PS3 Visualizer")) {
        dir = pathFromUtf8(pref) / "presets";
        SDL_free(pref);
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::vector<Entry> list(const std::vector<std::string> &known)
{
    const Uint64 now = SDL_GetTicks();
    if (gCacheValid && now - gCacheAt < 1500)
        return gCache;
    std::vector<Entry> out;
    std::error_code ec;
    int seen = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             folder(), std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator() && seen < 500; it.increment(ec)) {
        if (it.depth() > 2) {
            it.disable_recursion_pending();
            continue;
        }
        const auto &f = it->path();
        std::string ext = f.extension().string();
        for (char &ch : ext)
            ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (!it->is_regular_file(ec) || (ext != ".ps3vis" && ext != ".txt" && !ext.empty()))
            continue;
        if (it->file_size(ec) > 64 * 1024 || ec)
            continue;
        seen++;
        SharedPreset p;
        if (load(f, known, p)) // decodes, with at least one known parameter
            out.push_back({p.name, f});
    }
    std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
    gCache = out;
    gCacheAt = now;
    gCacheValid = true;
    return out;
}

std::filesystem::path save(const SharedPreset &p)
{
    const std::string base = safeName(p.name);
    std::filesystem::path file = folder() / pathFromUtf8(base + ".ps3vis");
    for (int n = 2; std::filesystem::exists(file); n++)
        file = folder() / pathFromUtf8(base + " " + std::to_string(n) + ".ps3vis");
    std::ofstream(file) << encode(p) << "\n";
    invalidate();
    return file;
}

bool rename(const Entry &e, const std::string &newName, const std::vector<std::string> &known)
{
    SharedPreset p;
    const std::string name = cleanName(newName);
    if (name.empty() || !load(e.file, known, p))
        return false;
    p.name = name;
    const std::filesystem::path file = save(p);
    std::error_code ec;
    if (!std::filesystem::equivalent(file, e.file, ec))
        std::filesystem::remove(e.file, ec);
    invalidate();
    return true;
}

bool remove(const Entry &e)
{
    std::error_code ec;
    const bool ok = std::filesystem::remove(e.file, ec);
    invalidate();
    return ok;
}

bool load(const std::filesystem::path &file, const std::vector<std::string> &known, SharedPreset &out)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || std::filesystem::file_size(file, ec) > 64 * 1024 || ec)
        return false; // not a preset file (sanity: they're well under 2 KB)
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    return decode(text.str(), known, out);
}

} // namespace presetlib
