// Access to assets inside an installed PS3 firmware (dev_flash), e.g. the one
// RPCS3 creates when it installs PS3UPDAT.PUP.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using Bytes = std::vector<uint8_t>;

std::optional<Bytes> readFile(const fs::path &path);

// SDL hands us UTF-8 (argv via SDL_main, drag & drop); std::filesystem would
// read plain char strings in the ANSI code page on Windows.
inline fs::path pathFromUtf8(const std::string &s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}
inline std::string utf8(const fs::path &p)
{
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// A QGL resource container (dev_flash/vsh/resource/qgl/*.qrc).
// Layout: optional "QRCC" zlib wrapper around a "QRCF" tree whose file
// elements carry an id (path) and a src (offset/size into the data block).
class QrcArchive {
public:
    bool load(const fs::path &path);
    bool contains(const std::string &name) const { return files_.count(name) != 0; }
    std::optional<Bytes> get(const std::string &name) const;
    std::vector<std::string> list(const std::string &prefix = {}) const;

private:
    Bytes data_;
    std::map<std::string, std::pair<uint32_t, uint32_t>> files_;
};

// "#MNU_1.0" parameter files: lines of NAME:type:value.
using MnuValues = std::map<std::string, float>;
MnuValues parseMnu(const Bytes &text);

// Locates dev_flash. Accepts dev_flash itself, a directory containing it
// (e.g. an RPCS3 config dir or PS3-HDD-DATA), or nothing to search defaults.
std::optional<fs::path> findDevFlash(const std::string &hint, const fs::path &exeDir);
