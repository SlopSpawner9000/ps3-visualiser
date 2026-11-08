#include "firmware.h"

#include <SDL3/SDL_log.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

// stb_image: its zlib decoder unpacks QRCC containers (so no zlib dependency), and its PNG/JPEG
// decoders read album art.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG // album art (see albumart.cpp)
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

std::optional<Bytes> readFile(const fs::path &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::nullopt;
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static uint32_t be32(const Bytes &d, size_t o)
{
    if (o + 4 > d.size())
        return 0xffffffff;
    return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

static std::string cstr(const Bytes &d, size_t o)
{
    std::string s;
    while (o < d.size() && d[o])
        s += char(d[o++]);
    return s;
}

bool QrcArchive::load(const fs::path &path)
{
    auto raw = readFile(path);
    if (!raw || raw->size() < 8)
        return false;
    if (std::memcmp(raw->data(), "QRCC", 4) == 0) {
        const uint32_t size = be32(*raw, 4);
        data_.resize(size);
        const int got = stbi_zlib_decode_buffer(reinterpret_cast<char *>(data_.data()), int(size),
                                                reinterpret_cast<const char *>(raw->data() + 8),
                                                int(raw->size() - 8));
        if (got < 0)
            return false;
        data_.resize(size_t(got));
    } else {
        data_ = std::move(*raw);
    }
    if (data_.size() < 0x40 || std::memcmp(data_.data(), "QRCF", 4) != 0)
        return false;

    const uint32_t treeOff = be32(data_, 0x08);
    const uint32_t idOff = be32(data_, 0x10);
    const uint32_t nameOff = be32(data_, 0x18);
    const uint32_t dataOff = be32(data_, 0x30);

    // Each element: tag, attrCount, parent, prev, next, firstChild, lastChild,
    // followed by attrCount * {name, type, v1, v2}.
    std::vector<uint32_t> stack{0};
    int guard = 0;
    while (!stack.empty() && guard++ < 1000000) {
        uint32_t e = stack.back();
        stack.pop_back();
        while (e != 0xffffffff) {
            const size_t b = treeOff + e;
            const uint32_t nattr = be32(data_, b + 4);
            const uint32_t next = be32(data_, b + 16);
            const uint32_t child = be32(data_, b + 20);
            std::string id;
            uint32_t srcOff = 0, srcSize = 0;
            bool hasSrc = false;
            for (uint32_t i = 0; i < nattr && i < 64; i++) {
                const size_t a = b + 28 + 16 * i;
                const std::string an = cstr(data_, nameOff + be32(data_, a));
                if (an == "src") {
                    srcOff = be32(data_, a + 8);
                    srcSize = be32(data_, a + 12);
                    hasSrc = true;
                } else if (an == "id") {
                    id = cstr(data_, idOff + be32(data_, a + 8) + 4);
                }
            }
            if (hasSrc && !id.empty() && size_t(dataOff) + srcOff + srcSize <= data_.size())
                files_[id] = {dataOff + srcOff, srcSize};
            if (child != 0xffffffff)
                stack.push_back(child);
            e = next;
        }
    }
    return !files_.empty();
}

std::optional<Bytes> QrcArchive::get(const std::string &name) const
{
    auto it = files_.find(name);
    if (it == files_.end())
        return std::nullopt;
    return Bytes(data_.begin() + it->second.first, data_.begin() + it->second.first + it->second.second);
}

std::vector<std::string> QrcArchive::list(const std::string &prefix) const
{
    std::vector<std::string> out;
    for (auto &[k, v] : files_)
        if (k.compare(0, prefix.size(), prefix) == 0)
            out.push_back(k);
    return out;
}

MnuValues parseMnu(const Bytes &text)
{
    MnuValues out;
    std::istringstream in(std::string(text.begin(), text.end()));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto a = line.find(':');
        if (a == std::string::npos || line[0] == '#')
            continue;
        const auto b = line.find(':', a + 1);
        if (b == std::string::npos)
            continue;
        out[line.substr(0, a)] = std::strtof(line.c_str() + b + 1, nullptr);
    }
    return out;
}

static bool isDevFlash(const fs::path &p)
{
    std::error_code ec;
    return fs::exists(p / "vsh" / "resource" / "qgl" / "canyon.qrc", ec);
}

// RPCS3 can keep dev_flash elsewhere (e.g. EmuDeck on an SD card): its vfs.yml (in the RPCS3 folder or
// its config/) maps "/dev_flash/" to a folder, possibly through "$(EmulatorDir)" (empty = the RPCS3
// folder itself). Returns that folder, if the file names one.
static std::optional<fs::path> vfsDevFlash(const fs::path &rpcs3)
{
    for (const fs::path &file : {rpcs3 / "config" / "vfs.yml", rpcs3 / "vfs.yml"}) {
        std::ifstream in(file);
        if (!in)
            continue;
        std::string line, emulatorDir, devFlash;
        auto value = [](const std::string &l, size_t colon) {
            std::string v = l.substr(colon + 1);
            v.erase(0, v.find_first_not_of(" \t\""));
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '"' || v.back() == '\r'))
                v.pop_back();
            return v;
        };
        while (std::getline(in, line)) {
            const size_t a = line.find_first_not_of(" \t");
            if (a == std::string::npos)
                continue;
            if (line.compare(a, 15, "$(EmulatorDir):") == 0)
                emulatorDir = value(line, line.find(':', a));
            else if (line.compare(a, 12, "/dev_flash/:") == 0)
                devFlash = value(line, a + 11);
        }
        if (devFlash.empty())
            continue;
        const std::string base = emulatorDir.empty() ? (rpcs3.string() + "/") : emulatorDir;
        if (const size_t at = devFlash.find("$(EmulatorDir)"); at != std::string::npos)
            devFlash.replace(at, 14, base);
        return pathFromUtf8(devFlash);
    }
    return std::nullopt;
}

std::optional<fs::path> findDevFlash(const std::string &hint, const fs::path &exeDir)
{
    std::vector<fs::path> candidates;
    auto addRoot = [&](const fs::path &root) {
        if (auto moved = vfsDevFlash(root)) // a dev_flash moved through RPCS3's vfs.yml
            candidates.push_back(*moved);
        candidates.push_back(root);
        candidates.push_back(root / "dev_flash");
        candidates.push_back(root / "PS3-HDD-DATA" / "dev_flash");
    };
    if (!hint.empty()) {
        addRoot(pathFromUtf8(hint));
    } else {
        if (const char *env = std::getenv("PS3VIS_FIRMWARE"))
            addRoot(env);
        addRoot(fs::current_path());
        addRoot(exeDir);
        addRoot(exeDir.parent_path());
#if defined(_WIN32)
        // RPCS3 for Windows is a portable folder: dev_flash sits next to rpcs3.exe.
        if (const char *appdata = std::getenv("APPDATA"))
            addRoot(fs::path(appdata) / "rpcs3");
        if (const char *local = std::getenv("LOCALAPPDATA"))
            addRoot(fs::path(local) / "rpcs3");
        if (const char *user = std::getenv("USERPROFILE")) {
            addRoot(fs::path(user) / "rpcs3");
            addRoot(fs::path(user) / "RPCS3");
            addRoot(fs::path(user) / "Downloads" / "rpcs3");
            addRoot(fs::path(user) / "Desktop" / "rpcs3");
        }
        if (const char *pf = std::getenv("ProgramFiles"))
            addRoot(fs::path(pf) / "RPCS3");
        addRoot("C:/rpcs3");
        addRoot("C:/RPCS3");
        addRoot("C:/Games/rpcs3");
#elif defined(__APPLE__)
        if (const char *home = std::getenv("HOME"))
            addRoot(fs::path(home) / "Library" / "Application Support" / "rpcs3");
#else
        if (const char *xdg = std::getenv("XDG_CONFIG_HOME"))
            addRoot(fs::path(xdg) / "rpcs3");
        if (const char *home = std::getenv("HOME")) {
            addRoot(fs::path(home) / ".config" / "rpcs3");
            addRoot(fs::path(home) / ".var" / "app" / "net.rpcs3.RPCS3" / "config" / "rpcs3");
        }
#endif
    }
    for (auto &c : candidates)
        if (isDevFlash(c))
            return fs::weakly_canonical(c);
    std::string tried; // (for startup.log)
    for (auto &c : candidates)
        tried += "\n  " + c.string();
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "no dev_flash (vsh/resource/qgl/canyon.qrc) in:%s", tried.c_str());
    return std::nullopt;
}
