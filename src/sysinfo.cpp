#include "sysinfo.h"

#include <SDL3/SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace {

std::string env(const char *name)
{
    const char *v = std::getenv(name);
    return v ? v : "";
}

// First "key: value" line of a /proc style file whose key starts with `key`.
std::string procField(const char *file, const char *key)
{
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line))
        if (line.rfind(key, 0) == 0) {
            const size_t colon = line.find_first_of(":=");
            if (colon == std::string::npos)
                continue;
            std::string v = line.substr(colon + 1);
            v.erase(0, v.find_first_not_of(" \t\""));
            while (!v.empty() && (v.back() == '"' || v.back() == ' '))
                v.pop_back();
            return v;
        }
    return "";
}

std::string versionText(unsigned v)
{
    return std::to_string(v >> 16) + "." + std::to_string((v >> 8) & 0xff) + "." + std::to_string(v & 0xff);
}

} // namespace

int logicalCpus() { return SDL_GetNumLogicalCPUCores(); }

ResourceSample sampleResources()
{
    ResourceSample s;
#if defined(_WIN32)
    HANDLE p = GetCurrentProcess();
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(p, &created, &exited, &kernel, &user)) {
        auto secs = [](const FILETIME &f) {
            return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7;
        };
        s.cpuSeconds = secs(kernel) + secs(user);
        s.ok = true;
    }
    PROCESS_MEMORY_COUNTERS mem{};
    if (GetProcessMemoryInfo(p, &mem, sizeof mem))
        s.memoryBytes = mem.WorkingSetSize;
    IO_COUNTERS io{};
    if (GetProcessIoCounters(p, &io)) {
        s.readBytes = io.ReadTransferCount;
        s.writeBytes = io.WriteTransferCount;
    }
#elif defined(__linux__)
    // /proc/self/stat: fields after the ")" of the command name; utime/stime are fields 14/15,
    // the thread count field 20 (1-based), so tokens 11, 12 and 17 after the ")".
    std::ifstream stat("/proc/self/stat");
    std::string all((std::istreambuf_iterator<char>(stat)), {});
    const size_t close = all.rfind(')');
    if (close != std::string::npos) {
        std::istringstream rest(all.substr(close + 2));
        std::vector<std::string> f;
        for (std::string t; rest >> t;)
            f.push_back(t);
        if (f.size() > 17) {
            const double tick = double(sysconf(_SC_CLK_TCK));
            s.cpuSeconds = (std::atof(f[11].c_str()) + std::atof(f[12].c_str())) / tick;
            s.threads = std::atoi(f[17].c_str());
            s.ok = true;
        }
    }
    std::ifstream statm("/proc/self/statm");
    uint64_t size = 0, resident = 0;
    if (statm >> size >> resident)
        s.memoryBytes = resident * uint64_t(sysconf(_SC_PAGESIZE));
    s.readBytes = std::strtoull(procField("/proc/self/io", "rchar").c_str(), nullptr, 10);
    s.writeBytes = std::strtoull(procField("/proc/self/io", "wchar").c_str(), nullptr, 10);
#else
    rusage u{};
    if (getrusage(RUSAGE_SELF, &u) == 0) {
        s.cpuSeconds = double(u.ru_utime.tv_sec) + double(u.ru_utime.tv_usec) * 1e-6 + double(u.ru_stime.tv_sec) +
                       double(u.ru_stime.tv_usec) * 1e-6;
        s.memoryBytes = uint64_t(u.ru_maxrss); // bytes on macOS (peak, the best cheap figure)
        s.ok = true;
    }
#endif
    return s;
}

// Display names are often "make model serial (connector)". The serial is dropped when the last two
// words before the connector both contain digits (model + serial); "Dell U2720Q" stays as it is.
std::string withoutSerial(const std::string &name)
{
    const size_t paren = name.rfind(" (");
    const std::string head = paren == std::string::npos ? name : name.substr(0, paren);
    const std::string tail = paren == std::string::npos ? "" : name.substr(paren);
    const size_t last = head.rfind(' ');
    if (last == std::string::npos)
        return name;
    const size_t prev = head.rfind(' ', last - 1);
    auto digits = [](const std::string &w) { return w.find_first_of("0123456789") != std::string::npos; };
    const std::string serial = head.substr(last + 1), model = head.substr(prev == std::string::npos ? 0 : prev + 1,
                                                                          last - (prev == std::string::npos ? 0 : prev + 1));
    if (!digits(serial) || !digits(model))
        return name;
    return head.substr(0, last) + " <serial>" + tail;
}

std::string cpuName()
{
    std::string cpu;
#if defined(_WIN32)
    char name[256] = {};
    DWORD size = sizeof name;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS)
        cpu = name;
#elif defined(__linux__)
    cpu = procField("/proc/cpuinfo", "model name");
    if (cpu.empty())
        cpu = procField("/proc/cpuinfo", "Model"); // ARM boards
#elif defined(__APPLE__)
    char name[256] = {};
    size_t size = sizeof name;
    if (sysctlbyname("machdep.cpu.brand_string", name, &size, nullptr, 0) == 0)
        cpu = name;
#endif
    return cpu;
}

uint64_t memoryAvailableBytes()
{
#if defined(_WIN32)
    MEMORYSTATUSEX m{sizeof m};
    return GlobalMemoryStatusEx(&m) ? uint64_t(m.ullAvailPhys) : 0;
#elif defined(__linux__)
    return std::strtoull(procField("/proc/meminfo", "MemAvailable").c_str(), nullptr, 10) * 1024; // (in kB)
#else
    return 0;
#endif
}

std::string hardwareInfo(const char *glVendor, const char *glRenderer, bool hideSerials)
{
    std::ostringstream out;
    const std::string cpu = cpuName();
    out << "cpu = " << (cpu.empty() ? "?" : cpu) << ", " << SDL_GetNumLogicalCPUCores() << " logical cores, "
        << "SIMD:" << (SDL_HasSSE42() ? " SSE4.2" : "") << (SDL_HasAVX2() ? " AVX2" : "") << (SDL_HasAVX512F() ? " AVX-512" : "")
        << (SDL_HasNEON() ? " NEON" : "") << "\n";
    out << "ram = " << SDL_GetSystemRAM() << " MiB";
#if defined(__linux__)
    const std::string avail = procField("/proc/meminfo", "MemAvailable");
    if (!avail.empty())
        out << " (available: " << avail << ")";
#endif
    out << "\n";
    out << "gpu = " << (glRenderer ? glRenderer : "?") << " (" << (glVendor ? glVendor : "?") << ")\n";
    int count = 0;
    if (SDL_DisplayID *ds = SDL_GetDisplays(&count)) {
        for (int i = 0; i < count; i++) {
            const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(ds[i]);
            const char *name = SDL_GetDisplayName(ds[i]);
            char line[256];
            std::snprintf(line, sizeof line, "display %d = \"%s\", %dx%d @ %.2f Hz, scale %.2f, HDR %s\n", i,
                          name ? (hideSerials ? withoutSerial(name) : std::string(name)).c_str() : "?", m ? m->w : 0, m ? m->h : 0, m ? double(m->refresh_rate) : 0.0,
                          double(SDL_GetDisplayContentScale(ds[i])),
                          SDL_GetBooleanProperty(SDL_GetDisplayProperties(ds[i]), SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false)
                              ? "on"
                              : "off");
            out << line;
        }
        SDL_free(ds);
    }
    if (SDL_AudioDeviceID *devs = SDL_GetAudioPlaybackDevices(&count)) {
        for (int i = 0; i < count; i++) {
            const char *name = SDL_GetAudioDeviceName(devs[i]);
            out << "audio output " << i << " = " << (name ? name : "?") << "\n";
        }
        SDL_free(devs);
    }
    SDL_AudioSpec spec{};
    int frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &frames))
        out << "default audio output = " << spec.freq << " Hz, " << spec.channels << " channels, buffer " << frames
            << " frames\n";
    return out.str();
}

std::string softwareInfo(const char *glVersion, const char *glslVersion)
{
    std::ostringstream out;
#if defined(_WIN32)
    // GetVersionEx lies on Windows 10+; RtlGetVersion doesn't.
    using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    OSVERSIONINFOW v{sizeof v};
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))))
        fn(&v);
    out << "os = Windows " << v.dwMajorVersion << "." << v.dwMinorVersion << " build " << v.dwBuildNumber
        << (v.dwBuildNumber >= 22000 ? " (Windows 11)" : "") << "\n";
#else
    utsname u{};
    uname(&u);
    std::string pretty = procField("/etc/os-release", "PRETTY_NAME");
    out << "os = " << (pretty.empty() ? u.sysname : pretty) << ", kernel " << u.sysname << " " << u.release << " ("
        << u.machine << ")\n";
    out << "session = " << (env("XDG_SESSION_TYPE").empty() ? "?" : env("XDG_SESSION_TYPE"))
        << ", desktop " << (env("XDG_CURRENT_DESKTOP").empty() ? "?" : env("XDG_CURRENT_DESKTOP"))
        << ", Wayland " << (env("WAYLAND_DISPLAY").empty() ? "no" : "yes") << ", X11 "
        << (env("DISPLAY").empty() ? "no" : "yes") << "\n";
#endif
    const int sv = SDL_GetVersion();
    out << "sdl = " << SDL_VERSIONNUM_MAJOR(sv) << "." << SDL_VERSIONNUM_MINOR(sv) << "." << SDL_VERSIONNUM_MICRO(sv)
        << " (" << SDL_GetRevision() << "), platform " << SDL_GetPlatform() << ", video "
        << (SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?") << ", audio "
        << (SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?") << "\n";
    out << "opengl = " << (glVersion ? glVersion : "?") << ", GLSL " << (glslVersion ? glslVersion : "?") << "\n";
    out << "ffmpeg = " << av_version_info() << " (avformat " << versionText(avformat_version()) << ", avcodec "
        << versionText(avcodec_version()) << ", swresample " << versionText(swresample_version()) << ", avutil "
        << versionText(avutil_version()) << ")\n";
#if defined(__clang__)
    out << "compiler = clang " << __clang_version__;
#elif defined(__GNUC__)
    out << "compiler = gcc " << __VERSION__;
#elif defined(_MSC_VER)
    out << "compiler = MSVC " << _MSC_VER;
#endif
    out << ", C++ " << __cplusplus << ", built " << __DATE__ << "\n";
    int n = 0;
    if (SDL_Locale **locales = SDL_GetPreferredLocales(&n)) {
        out << "locales =";
        for (int i = 0; i < n && locales[i]; i++)
            out << " " << locales[i]->language << (locales[i]->country ? std::string("_") + locales[i]->country : "");
        out << "\n";
        SDL_free(locales);
    }
    return out.str();
}

std::string redactPersonal(const std::string &text)
{
    std::string s = text;
    auto escape = [](const std::string &v) {
        static const std::regex special(R"([.^$|()\[\]{}*+?\\])");
        return std::regex_replace(v, special, R"(\$&)");
    };
    auto replaceAll = [&s](const std::string &from, const std::string &to) {
        if (from.empty())
            return;
        for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size())
            s.replace(at, from.size(), to);
    };
    // E-mail addresses.
    s = std::regex_replace(s, std::regex(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})"), "<email>");
    // Home folder -> ~ (both slash styles on Windows).
    for (const char *var : {"HOME", "USERPROFILE"}) {
        std::string home = env(var);
        while (home.size() > 1 && (home.back() == '/' || home.back() == '\\'))
            home.pop_back();
        if (home.size() > 2) {
            replaceAll(home, "~");
            std::string other = home;
            for (char &c : other)
                c = c == '\\' ? '/' : c == '/' ? '\\' : c;
            replaceAll(other, "~");
        }
    }
    // MAC addresses, then IPv6 (8 groups, or shortened with "::" and at least two groups / ::1).
    s = std::regex_replace(s, std::regex(R"(\b[0-9A-Fa-f]{2}(?:[:-][0-9A-Fa-f]{2}){5}\b)"), "<mac>");
    s = std::regex_replace(s, std::regex(R"(\b(?:[0-9A-Fa-f]{1,4}:){7}[0-9A-Fa-f]{1,4}\b)"), "<ip>");
    s = std::regex_replace(
        s, std::regex(R"((?:\b[0-9A-Fa-f]{1,4}(?::[0-9A-Fa-f]{1,4})*::(?:[0-9A-Fa-f]{1,4}(?::[0-9A-Fa-f]{1,4})*)?\b)|::1\b)"),
        "<ip>");
    // IPv4 (each part at most 255, so version numbers like 10.0.22631.0 stay).
    {
        static const std::regex v4(R"(\b(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})\b)");
        std::string out;
        auto last = s.cbegin();
        for (std::sregex_iterator it(s.cbegin(), s.cend(), v4), end; it != end; ++it) {
            bool ip = true;
            for (int g = 1; g <= 4; g++)
                ip &= std::atoi((*it)[g].str().c_str()) <= 255;
            out.append(last, (*it)[0].first);
            out += ip ? "<ip>" : (*it)[0].str();
            last = (*it)[0].second;
        }
        out.append(last, s.cend());
        s = out;
    }
    // User and computer names, as whole words (case-insensitive; very short names are skipped,
    // they'd hit ordinary words).
    std::string host = env("COMPUTERNAME");
#if !defined(_WIN32)
    char name[256] = {};
    if (gethostname(name, sizeof name - 1) == 0)
        host = name;
#endif
    // (The computer name first: it often contains the user name.)
    for (auto [value, tag] : {std::pair{host, "<host>"}, std::pair{env("USER"), "<user>"},
                              std::pair{env("USERNAME"), "<user>"}, std::pair{env("LOGNAME"), "<user>"}}) {
        if (value.size() < 3)
            continue;
        const std::regex word("(^|[^A-Za-z0-9])" + escape(value) + "(?=$|[^A-Za-z0-9])", std::regex::icase);
        s = std::regex_replace(s, word, std::string("$1") + tag);
    }
    return s;
}
