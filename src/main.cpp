// PS3 Visualizer: a music player driven by the PlayStation 3 "canyon" music
// visualizer, rebuilt from the user's own PS3 firmware files.

#include "albumart.h"
#include "audio.h"
#include "canyon.h"
#include "firmware.h"
#include "gl.h"
#include "menu.h"
#include "overlay.h"
#include "playlist.h"
#include "presetlib.h"
#include "spectrum.h"
#include "dither.h"
#include "uistyle.h"
#include "earth.h"
#include "glyphs.h"
#include "sysinfo.h"
#include "vkpresent.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <ctime>
#include <atomic>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

namespace {

constexpr double kVisStep = 1.0 / 60.0; // the visualizer's feedback effects are tuned per frame

struct Options {
    std::string firmware;
    std::string preset;
    bool fullscreen = false;
    bool shuffle = false;
    bool randomPresets = false;
    float gain = -1.0f;   // < 0: use the saved setting
    float volume = -1.0f;
    std::vector<std::string> inputs;
    // Offline rendering (for testing / thumbnails)
    std::string screenshot;
    int frames = 600;
    int every = 0; // with a "%d" in --screenshot: also save every N frames
    bool overlay = false;
    bool listPresets = false;
    double start = 0;
    int width = 1280, height = 720;
};

void usage()
{
    std::printf(
        "Usage: ps3vis [options] [files or folders...]\n"
        "\n"
        "  --firmware PATH   dev_flash of an installed PS3 firmware (or a folder containing it,\n"
        "                    e.g. your RPCS3 config directory). Searched automatically if omitted.\n"
        "  --preset NAME     visualizer preset to use (default: last used, Blue02 at first)\n"
        "  --random-presets  pick a random preset for every track\n"
        "  --list-presets    print the presets found in the firmware and exit\n"
        "  --shuffle         shuffle the playlist\n"
        "  --fullscreen      start in fullscreen\n"
        "  --volume X        volume, 0..1.5\n"
        "  --gain X          sensitivity: scales the spectrum fed to the visualizer (default 3)\n"
        "                    (settings changed in the Esc menu are remembered between runs)\n"
        "  --screenshot FILE render offline instead of playing: write a PNG after --frames N\n"
        "                    (default 600) frames of the first file, starting at --start SECONDS,\n"
        "                    at --size WxH (default 1280x720); --overlay adds the track info\n"
        "\n"
        "Keys: Esc menu, Space pause, Left/Right seek, Up/Down volume, N/P next/previous track,\n"
        "      V/Shift+V next/previous visual preset, F fullscreen, I track info, Shift+I album art,\n"
        "      S shuffle, R repeat (off / playlist / one song), mouse wheel volume, H help, Q quit\n");
}

bool parseArgs(int argc, char **argv, Options &o)
{
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            std::exit(0);
        } else if (a == "--firmware") {
            o.firmware = value();
        } else if (a == "--preset") {
            o.preset = value();
        } else if (a == "--random-presets") {
            o.randomPresets = true;
        } else if (a == "--shuffle") {
            o.shuffle = true;
        } else if (a == "--fullscreen") {
            o.fullscreen = true;
        } else if (a == "--volume") {
            o.volume = std::strtof(value().c_str(), nullptr);
        } else if (a == "--gain") {
            o.gain = std::strtof(value().c_str(), nullptr);
        } else if (a == "--screenshot") {
            o.screenshot = value();
        } else if (a == "--frames") {
            o.frames = std::max(1, std::atoi(value().c_str()));
        } else if (a == "--list-presets") {
            o.listPresets = true;
        } else if (a == "--overlay") {
            o.overlay = true;
        } else if (a == "--every") {
            o.every = std::max(1, std::atoi(value().c_str()));
        } else if (a == "--start") {
            o.start = std::strtod(value().c_str(), nullptr);
        } else if (a == "--size") {
            const std::string v = value();
            if (std::sscanf(v.c_str(), "%dx%d", &o.width, &o.height) != 2) {
                std::fprintf(stderr, "bad --size %s\n", v.c_str());
                return false;
            }
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return false;
        } else {
            o.inputs.push_back(a);
        }
    }
    return true;
}

fs::path exeDir()
{
    const char *base = SDL_GetBasePath();
    return base ? pathFromUtf8(base) : fs::current_path();
}

std::vector<fs::path> fontFiles(const fs::path &devFlash)
{
    std::vector<fs::path> out;
    for (const char *f : {"SCE-PS3-RD-R-LATIN.TTF", "SCE-PS3-RD-R-LATIN2.TTF", "SCE-PS3-NR-R-JPN.TTF",
                          "SCE-PS3-YG-R-KOR.TTF", "SCE-PS3-NR-R-EXT.TTF", "SCE-PS3-DH-R-CGB.TTF"})
        out.push_back(devFlash / "data" / "font" / f);
    return out;
}

void fail(const std::string &msg)
{
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", msg.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "PS3 Visualizer", msg.c_str(), nullptr);
}

// Short FFT frames are far noisier than the spectrum the PS3 audio player
// supplies; a little temporal smoothing keeps neighbouring terrain strips close.
void columnsFrom(Spectrum &spec, const float *left, const float *right, float gain, float dt, int srcRate,
                 int mode, float *columns)
{
    static float lb[Spectrum::kBins], rb[Spectrum::kBins];
    float now[Canyon::kCols];
    if (mode != 2) {
        // The PS3's own feed; Sensitivity scales the samples (3 = as on the PS3).
        static const Ps3Feed feed;
        // PS3VIS_FEEDSCALE=x: sample level the PS3 feed sees (testing)
        static const float fs = SDL_getenv("PS3VIS_FEEDSCALE") ? float(std::atof(SDL_getenv("PS3VIS_FEEDSCALE"))) : 1.0f;
        gain *= fs;
        float l[Ps3Feed::kOut], r[Ps3Feed::kOut];
        // The PS3 converts other rates to 48 kHz by sample-and-hold (soundvisualizer
        // sub_f10), whose imaging puts energy into the high (centre) columns.
        // Rebuild the source-rate samples from our properly resampled history
        // and hold them the same way.
        float hl[Ps3Feed::kSamples], hr[Ps3Feed::kSamples];
        const int off = Spectrum::kSize - Ps3Feed::kSamples;
        const double ratio = srcRate > 0 ? double(srcRate) / AudioEngine::kRate : 1.0;
        for (int n = 0; n < Ps3Feed::kSamples; n++) {
            const double m = std::floor((n + 0.5) * ratio);   // held source sample
            const double t = off + m / ratio;                   // its time in history samples
            const int i0 = std::clamp(int(t), 0, Spectrum::kSize - 2);
            const float f = float(t - i0);
            hl[n] = left[i0] + (left[i0 + 1] - left[i0]) * f;
            hr[n] = right[i0] + (right[i0 + 1] - right[i0]) * f;
        }
        feed.analyse(hl, gain / 3.0f, l);
        feed.analyse(hr, gain / 3.0f, r);
        Ps3Feed::toColumns(l, r, columns);
        if (mode == 1) {
            // Smooth bass: the PS3's ~1 ms window catches a different point of each
            // 25-60 ms bass cycle every frame, so the bass columns jitter. Average the
            // lowest columns over ~50 ms, fading out by ~800 Hz (column 8).
            static float ema[Canyon::kCols];
            static bool primed = false;
            const float k = primed ? 1.0f - std::exp(-dt / 0.05f) : 1.0f;
            primed = true;
            for (int i = 0; i < 9; i++) {
                const float w = std::clamp((8.0f - float(i)) / 5.0f, 0.0f, 1.0f);
                for (const int c : {i, Canyon::kCols - 1 - i}) {
                    ema[c] += (columns[c] - ema[c]) * k;
                    columns[c] += (ema[c] - columns[c]) * w;
                }
            }
        }
        return;
    }
    spec.analyse(left, lb);
    spec.analyse(right, rb);
    // Test switches while calibrating against the PS3 recording:
    //   PS3VIS_SPECMODE=linear  PS3 column layout instead of log bands
    //   PS3VIS_SMOOTH=seconds   temporal smoothing time constant (default 0.06)
    static const bool linear = SDL_getenv("PS3VIS_SPECMODE") && !std::strcmp(SDL_getenv("PS3VIS_SPECMODE"), "linear");
    static const float tau = SDL_getenv("PS3VIS_SMOOTH") ? float(std::atof(SDL_getenv("PS3VIS_SMOOTH"))) : 0.06f;
    if (linear)
        Spectrum::toColumnsLinear(lb, rb, now, float(AudioEngine::kRate),
                                  SDL_getenv("PS3VIS_HIGHHZ") ? float(std::atof(SDL_getenv("PS3VIS_HIGHHZ"))) : 22050.0f);
    else
    {
        static const float hi = SDL_getenv("PS3VIS_HIGHHZ") ? float(std::atof(SDL_getenv("PS3VIS_HIGHHZ"))) : Spectrum::kHighHz;
        Spectrum::toColumns(lb, rb, now, float(AudioEngine::kRate), Spectrum::kLowHz, hi);
    }
    // PS3VIS_REVERSE=1: bass in the centre, treble at the edges (test only)
    static const bool reverse = SDL_getenv("PS3VIS_REVERSE") != nullptr;
    if (reverse) {
        std::reverse(now, now + 64);
        std::reverse(now + 64, now + 128);
    }
    const float k = tau > 0 ? 1.0f - std::exp(-dt / tau) : 1.0f;
    for (int i = 0; i < Canyon::kCols; i++)
        columns[i] += (now[i] * gain - columns[i]) * k;
}

// On-screen layout, from the settings (UI scale and positions).
struct UiLayout {
    float scale = 1.0f;
    int infoPos = 0;     // 0 bottom left, 1 bottom right, 2 top left, 3 top right
    int controlsPos = 0; // 0 bottom right, 1 bottom centre, 2 bottom left (vertical: right / left edge)
    int presetBarPos = 1; // 0 top, 1 bottom
    float infoScale = 1.0f;  // track info & album art, on top of `scale`
    float controlsScale = 1.0f, presetScale = 1.0f; // playback buttons / preset bar, likewise
    bool vertical = false;   // playback buttons stacked vertically
    // Custom positions (top-left corner as a fraction of the window), x < 0 = automatic.
    float info[2] = {-1, -1}, controls[2] = {-1, -1}, presetBar[2] = {-1, -1};
};
UiLayout gLayout;
void drawHelp(Overlay &o, int w, int h, float unit, const Settings &settings, bool pad = false,
              PadStyle padStyle = PadStyle::Xbox);
// A UI theme from two colours of the visuals (main: backgrounds/text tint, accent: highlights).
UiTheme themeFromHues(float mainHue, float mainSat, float accHue, float accSat, float opacity)
{
    UiTheme t{};
    hsvRgb(accHue, std::min(0.65f, 0.15f + accSat * 0.6f), 1.0f, t.accent);
    hsvRgb(mainHue, std::min(0.08f, mainSat * 0.12f), 1.0f, t.text);
    hsvRgb(mainHue, std::min(0.7f, mainSat), 0.065f, t.bg);
    hsvRgb(accHue, std::min(0.3f, accSat * 0.4f), 1.0f, t.highlight);
    t.opacity = opacity;
    return t;
}
// PS3VIS_UI_COLOURS=accent,text,opacity[,hue,sat,val[,bg,highlight]] (test renders; custom = last choice,
// the hue/sat/val apply to every custom colour).
void uiColoursFromEnv(const char *text, Settings &s)
{
    float hue = 210, sat = 0.5f, val = 1;
    std::sscanf(text, "%d,%d,%f,%f,%f,%f,%d,%d", &s.uiAccent.choice, &s.uiText.choice, &s.uiOpacity, &hue, &sat, &val,
                &s.uiBackground.choice, &s.uiHighlight.choice);
    for (UiColour *c : {&s.uiAccent, &s.uiText, &s.uiBackground, &s.uiHighlight}) {
        c->hue = hue;
        c->sat = sat;
        c->val = c == &s.uiBackground ? val * 0.08f : val;
    }
}
UiTheme gTheme = uiTheme(Settings{}); // set from the settings (Misc -> Colours), or following the visuals
int gUiStyle = 0;                     // the UI theme (Misc -> Theme): how the on-screen controls are painted
float uiUnit(int h) { return float(h) / 720.0f * gLayout.scale; }

UiLayout layoutFrom(const Settings &s)
{
    UiLayout l;
    l.scale = s.uiScale;
    l.infoPos = s.infoPosition;
    l.controlsPos = s.controlsPosition;
    l.presetBarPos = s.presetBarPosition;
    l.infoScale = s.infoScale;
    l.controlsScale = s.controlsScale;
    l.presetScale = s.presetBarScale;
    l.vertical = s.controlsVertical;
    for (int k = 0; k < 2; k++) {
        l.info[k] = s.infoXY[k];
        l.controls[k] = s.controlsXY[k];
        l.presetBar[k] = s.presetBarXY[k];
    }
    return l;
}

// A custom position (fractions) to pixels, kept inside the window.
std::array<float, 2> customPos(const float *frac, float ew, float eh, int w, int h)
{
    return {std::clamp(frac[0] * float(w), 0.0f, std::max(0.0f, float(w) - ew)),
            std::clamp(frac[1] * float(h), 0.0f, std::max(0.0f, float(h) - eh))};
}
// Last drawn bounds of the track info block (x, y, w, h), for arranging and dragging, and the
// last computed preset bar (the track info keeps clear of it).
std::array<float, 4> gInfoRect{0, 0, 0, 0}, gPresetRect{0, 0, 0, 0};

// Where the playback (transport) bar goes: x, y, width, height.
std::array<float, 4> transportRect(int w, int h)
{
    const float margin = 48 * uiUnit(h), unit = uiUnit(h) * gLayout.controlsScale;
    const float tb = 52 * unit, gap = 10 * unit, pad = 10 * unit;
    const float along = tb * 7 + gap * 6 + pad * 2, across = tb + pad * 2;
    const float tw = gLayout.vertical ? across : along, th = gLayout.vertical ? along : across;
    if (gLayout.controls[0] >= 0) {
        const auto p = customPos(gLayout.controls, tw, th, w, h);
        return {p[0], p[1], tw, th};
    }
    if (gLayout.vertical) // on the right (or left) edge, centred vertically
        return {gLayout.controlsPos == 2 ? margin * 0.5f : float(w) - margin * 0.5f - tw, (float(h) - th) * 0.5f, tw, th};
    const float tx = gLayout.controlsPos == 1 ? (float(w) - tw) * 0.5f
                     : gLayout.controlsPos == 2 ? margin
                                                : float(w) - margin - tw;
    return {tx, float(h) - margin - th + 12 * unit, tw, th};
}

// "Now playing" block (bottom left by default).
// `barOut` (if given) receives the progress bar's clickable area (x, y, w, h in pixels).
void drawTrackInfo(Overlay &overlay, int w, int h, const TrackInfo &info, double pos, bool paused, int index,
                   int count, float alpha, float mouseX = -1, float mouseY = -1, float *barOut = nullptr,
                   GLuint artTex = 0, float artAspect = 1.0f, float artAlpha = 1.0f)
{
    const Rgba ink = ui::textOn(ui::style(gUiStyle), ui::Part::TrackInfo, gTheme, 1); // (text over the visuals)
    const float unit = uiUnit(h) * gLayout.infoScale, margin = 48 * uiUnit(h);
    std::string sub = info.artist;
    if (!info.album.empty())
        sub += (sub.empty() ? "" : "  -  ") + info.album;
    const double dur = info.duration;
    std::string times = formatTime(pos);
    if (dur > 0)
        times += " / " + formatTime(dur);
    if (paused)
        times += "   II";
    const float barW = 360 * unit, barH = 3 * unit;
    // Block size, so it can sit in any corner.
    const float blockW = std::max({overlay.measure(info.title, 34 * unit), overlay.measure(sub, 22 * unit),
                                   barW + 14 * unit + overlay.measure(times, 18 * unit)});
    const bool right = gLayout.infoPos == 1 || gLayout.infoPos == 3, top = gLayout.infoPos >= 2;
    // Album art (if any) sits to the left of the text, which moves right to make room.
    const float artSide = artTex ? 124 * unit : 0.0f, artGap = artTex ? 22 * unit : 0.0f;
    const float fullW = artSide + artGap + blockW;
    const float textH = (count > 1 ? 42 * unit : 0.0f) + 34 * unit + (sub.empty() ? 0.0f : 30 * unit) + 18 * unit;
    const float fullH = std::max(textH, artTex ? artSide : 0.0f);
    const bool custom = gLayout.info[0] >= 0;
    float left = right ? std::max(margin, float(w) - margin - fullW) : margin;
    std::array<float, 2> cp{0, 0};
    if (custom) {
        cp = customPos(gLayout.info, fullW, fullH, w, h);
        left = cp[0];
    }
    const float x = left + artSide + artGap;
    float y; // title baseline
    if (custom) { // block top at the custom position (text bottom-aligned with the art)
        y = cp[1] + fullH - 18 * unit - (sub.empty() ? 0.0f : 30 * unit) - 34 * unit + 28 * unit;
    } else if (top) {
        y = margin + 28 * unit + (count > 1 ? 42 * unit : 0.0f);
    } else {
        y = float(h) - margin - 46 * unit - (sub.empty() ? 0.0f : 30 * unit);
        // Sit above the playback bar if they would overlap.
        const auto t = transportRect(w, h);
        if (!gLayout.vertical && gLayout.controls[0] < 0 && left < t[0] + t[2] && x + blockW > t[0])
            y -= float(h) - margin - (t[1] - 12 * uiUnit(h)) + 12 * uiUnit(h);
        // ... and above an automatically placed bottom preset bar.
        const auto &p = gPresetRect;
        const float bottomNow = y + (sub.empty() ? 0.0f : 30 * unit) + 34 * unit + 18 * unit;
        if (gLayout.presetBar[0] < 0 && gLayout.presetBarPos == 1 && p[2] > 0 && left < p[0] + p[2] &&
            left + fullW > p[0] && bottomNow > p[1] - 8 * uiUnit(h))
            y -= bottomNow - (p[1] - 8 * uiUnit(h));
    }
    {
        const float blockBottom = y + (sub.empty() ? 0.0f : 30 * unit) + 34 * unit + 18 * unit;
        gInfoRect = {left, blockBottom - fullH, fullW, fullH};
    }
    if (artTex) { // square, bottom-aligned with the text block (top-aligned in the top corners)
        const float blockBottom = y + (sub.empty() ? 0.0f : 30 * unit) + 34 * unit + 12 * unit;
        const float ay = (top && !custom) ? margin : blockBottom - artSide;
        const float aw = artAspect >= 1.0f ? artSide : artSide * artAspect;
        const float ah = artAspect >= 1.0f ? artSide / artAspect : artSide;
        overlay.rect(left - 2 * unit, ay - 2 * unit, artSide + 4 * unit, artSide + 4 * unit, 0, 0, 0, 0.35f * artAlpha);
        overlay.image(artTex, left + (artSide - aw) * 0.5f, ay + (artSide - ah) * 0.5f, aw, ah, artAlpha);
    }
    if (alpha <= 0.0f) { // only the art is showing
        if (barOut)
            std::fill(barOut, barOut + 4, 0.0f);
        return;
    }
    if (count > 1)
        overlay.text(std::to_string(index + 1) + " / " + std::to_string(count), x, y - 42 * unit, 16 * unit, ink.r, ink.g, ink.b,
                     0.7f * alpha);
    overlay.text(info.title, x, y, 34 * unit, ink.r, ink.g, ink.b, alpha);
    y += 34 * unit;
    if (!sub.empty()) {
        overlay.text(sub, x, y, 22 * unit, 0.85f, 0.85f, 0.85f, alpha);
        y += 30 * unit;
    }
    // Progress bar: click or drag to seek, so it gets a taller hit area and grows when hovered.
    const float hitX = x - 6 * unit, hitY = y + 6 * unit - 10 * unit, hitW = barW + 12 * unit, hitH = barH + 20 * unit;
    const bool hover = dur > 0 && mouseX >= hitX && mouseX < hitX + hitW && mouseY >= hitY && mouseY < hitY + hitH;
    if (barOut) {
        const float r[4] = {hitX, hitY, hitW, hitH};
        std::copy(r, r + 4, barOut);
    }
    const float bh = hover ? barH * 2.0f : barH, by = y + 6 * unit + barH * 0.5f - bh * 0.5f;
    const float frac = dur > 0 ? float(std::clamp(pos / dur, 0.0, 1.0)) : 0.0f;
    overlay.rect(x, by, barW, bh, 1, 1, 1, (hover ? 0.35f : 0.25f) * alpha);
    if (dur > 0)
        overlay.rect(x, by, barW * frac, bh, gTheme.accent[0], gTheme.accent[1], gTheme.accent[2], 0.95f * alpha);
    if (hover) { // knob at the play position
        const float k = 10 * unit;
        overlay.rect(x + barW * frac - k * 0.5f, by + bh * 0.5f - k * 0.5f, k, k, 1, 1, 1, alpha);
    }
    overlay.text(times, x + barW + 14 * unit, y + 12 * unit, 18 * unit, ink.r, ink.g, ink.b, alpha);
}

// Keeps the most recent log lines for debug dumps (logging can come from any thread).
// Extensive logging (Debug tab): trace lines for every user action and program event.
bool gTrace = false;
#define TRACE(...)                                                                                   \
    do {                                                                                             \
        if (gTrace)                                                                                  \
            SDL_Log("trace: " __VA_ARGS__);                                                          \
    } while (0)

struct LogRing {
    std::mutex mtx;
    std::deque<std::string> lines;
    std::vector<std::string> startup; // the first lines, kept for good (HDR/Vulkan/GL decisions)
    std::FILE *startupFile = nullptr; // startup.log next to the settings: the same first lines, written at once
    SDL_LogOutputFunction next = nullptr;
    void *nextUser = nullptr;
    static void SDLCALL output(void *user, int category, SDL_LogPriority priority, const char *message)
    {
        auto *self = static_cast<LogRing *>(user);
        {
            std::lock_guard lock(self->mtx);
            char stamp[32];
            std::snprintf(stamp, sizeof stamp, "%10.3f ", double(SDL_GetTicksNS()) * 1e-9);
            self->lines.push_back(stamp + std::string(message));
            if (self->startup.size() < 400) {
                self->startup.push_back(self->lines.back());
                if (self->startupFile) { // flushed per line, so a start that dies early still leaves it all
                    std::fprintf(self->startupFile, "%s\n", self->lines.back().c_str());
                    std::fflush(self->startupFile);
                }
            }
            if (self->lines.size() > (gTrace ? 20000u : 400u))
                self->lines.pop_front();
        }
        if (self->next)
            self->next(self->nextUser, category, priority, message);
    }
};
LogRing gLog;

struct Ui {
    double infoUntil = 0;
    bool help = false;
    std::string osd;
    double osdUntil = 0;
    std::string notice; // longer message (e.g. HDR switched off), shown in a box at the top
    double noticeUntil = 0;
    double lastMouse = -10;
    float mouseX = -1, mouseY = -1; // pixels
    // On-screen buttons from the last frame (pixels: x, y, w, h).
    struct Button {
        float r[4];
        Action action;
        bool contains(float x, float y) const { return x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]; }
    };
    std::vector<Button> buttons;
    // Progress bar from the last frame (empty when hidden), and an ongoing click-drag seek.
    float bar[4] = {0, 0, 0, 0};
    bool seeking = false;
    double seekPreview = -1; // position shown while dragging
    double lastSeekAt = 0;
    // On-screen controls fade in when the mouse nears their edge of the window.
    float presetAlpha = 0, transportAlpha = 0;
    // "Arrange on-screen elements": drag the track info, playback buttons and preset bar.
    bool arranging = false;
    int dragging = -1; // 0 track info, 1 playback buttons, 2 preset bar
    float dragOffset[2] = {0, 0};
};

// Media icons drawn from shapes (the PS3 fonts have no transport glyphs).
// `state`: paused for play/pause, on/off for shuffle, 0/1/2 (off/playlist/one) for repeat. `ink`: its colour (iconInk).
void drawIcon(Overlay &o, Action a, int state, float cx, float cy, float s, float alpha, const Rgba &ink)
{
    const float tc[3] = {ink.r, ink.g, ink.b};
    const bool paused = state != 0;
    // A thick line from (x0, y0) to (x1, y1).
    auto seg = [&](float x0, float y0, float x1, float y1, float t, float al) {
        const float dx = x1 - x0, dy = y1 - y0, l = std::max(std::sqrt(dx * dx + dy * dy), 1e-3f);
        const float nx = -dy / l * t * 0.5f, ny = dx / l * t * 0.5f;
        o.tri(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, tc[0], tc[1], tc[2], al);
        o.tri(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, tc[0], tc[1], tc[2], al);
    };
    auto right = [&](float x, float hw) { o.tri(x - hw, cy - s, x - hw, cy + s, x + hw, cy, tc[0], tc[1], tc[2], alpha); };
    auto left = [&](float x, float hw) { o.tri(x + hw, cy - s, x + hw, cy + s, x - hw, cy, tc[0], tc[1], tc[2], alpha); };
    switch (a) {
    case Action::PlayPause:
        if (paused) {
            right(cx + s * 0.1f, s * 0.8f);
        } else {
            o.rect(cx - s * 0.7f, cy - s, s * 0.5f, s * 2, tc[0], tc[1], tc[2], alpha);
            o.rect(cx + s * 0.2f, cy - s, s * 0.5f, s * 2, tc[0], tc[1], tc[2], alpha);
        }
        break;
    case Action::NextTrack:
        right(cx - s * 0.2f, s * 0.6f);
        o.rect(cx + s * 0.45f, cy - s, s * 0.3f, s * 2, tc[0], tc[1], tc[2], alpha);
        break;
    case Action::PrevTrack:
        o.rect(cx - s * 0.75f, cy - s, s * 0.3f, s * 2, tc[0], tc[1], tc[2], alpha);
        left(cx + s * 0.2f, s * 0.6f);
        break;
    case Action::SeekForward:
        right(cx - s * 0.45f, s * 0.5f);
        right(cx + s * 0.5f, s * 0.5f);
        break;
    case Action::SeekBack:
        left(cx - s * 0.5f, s * 0.5f);
        left(cx + s * 0.45f, s * 0.5f);
        break;
    case Action::PrevPreset: left(cx, s * 0.5f); break;
    case Action::NextPreset: right(cx, s * 0.5f); break;
    case Action::Shuffle: { // two crossing arrows; dim when off
        const float al = state ? alpha : alpha * 0.35f, t = s * 0.2f;
        seg(cx - s, cy + s * 0.6f, cx + s * 0.45f, cy - s * 0.6f, t, al);
        seg(cx - s, cy - s * 0.6f, cx + s * 0.45f, cy + s * 0.6f, t, al);
        o.tri(cx + s * 0.35f, cy - s, cx + s * 0.35f, cy - s * 0.2f, cx + s * 1.05f, cy - s * 0.6f, tc[0], tc[1], tc[2], al);
        o.tri(cx + s * 0.35f, cy + s * 0.2f, cx + s * 0.35f, cy + s, cx + s * 1.05f, cy + s * 0.6f, tc[0], tc[1], tc[2], al);
        break;
    }
    case Action::Repeat: { // a loop of arrows; "1" inside for one song; dim when off
        const float al = state ? alpha : alpha * 0.35f, t = s * 0.2f;
        o.rect(cx - s, cy - s * 0.6f, s * 1.3f, t, tc[0], tc[1], tc[2], al);              // top
        o.rect(cx - s * 0.3f, cy + s * 0.6f - t, s * 1.3f, t, tc[0], tc[1], tc[2], al);   // bottom
        o.rect(cx - s, cy - s * 0.6f, t, s * 0.9f, tc[0], tc[1], tc[2], al);              // left
        o.rect(cx + s - t, cy - s * 0.3f, t, s * 0.9f, tc[0], tc[1], tc[2], al);          // right
        o.tri(cx + s * 0.3f, cy - s * 0.95f, cx + s * 0.3f, cy - s * 0.15f + t, cx + s * 0.85f, cy - s * 0.5f, 1, 1,
              1, al);
        o.tri(cx - s * 0.3f, cy + s * 0.15f - t, cx - s * 0.3f, cy + s * 0.95f, cx - s * 0.85f, cy + s * 0.5f, 1, 1,
              1, al);
        if (state == 2) { // "one song": a small 1 in the lower right of the button
            const float ts = s * 1.3f;
            o.text("1", cx + s * 1.25f, cy + s * 1.7f, ts, tc[0], tc[1], tc[2], al);
        }
        break;
    }
    default: break;
    }
}

// Preset bar (top centre) and transport bar (bottom right). Fills `buttons`.
// Audio fetched for analysis includes this much earlier audio, so the visualizer's
// equalizer filters have settled by the analysed part.
constexpr int kEqWarmup = 4096;

// Applies the EQ (if it should affect the visualizer) to `wide` (kEqWarmup + Spectrum::kSize
// frames) and copies the last Spectrum::kSize frames to left/right.
void analysisWindow(const EqSettings &eq, std::vector<float> &wideL, std::vector<float> &wideR, float *left,
                    float *right)
{
    if (eq.affectsVisualizer()) {
        Equalizer e;
        e.configure(eq, float(AudioEngine::kRate));
        e.process(wideL.data(), wideR.data(), int(wideL.size()));
    }
    std::copy(wideL.end() - Spectrum::kSize, wideL.end(), left);
    std::copy(wideR.end() - Spectrum::kSize, wideR.end(), right);
}

// RMS of the last 512 frames: the audio the spectrum was taken from.
float audioLevel(const float *left, const float *right)
{
    float sum = 0;
    for (int i = Spectrum::kSize - 512; i < Spectrum::kSize; i++)
        sum += left[i] * left[i] + right[i] * right[i];
    return std::sqrt(sum / 1024.0f);
}

// Debug panel (top left): the 128 columns fed to the terrain this frame, laid out like
// the terrain (left channel low->high, right channel high->low), the row values after the
// preset's boost/scaling as ticks, and the level of the audio they came from.
// Frequency (Hz) shown by terrain column `i` in the given music response mode.
float columnHz(int i, int mode)
{
    const int k = i < 64 ? i : 127 - i; // both halves run bass -> treble towards the middle
    if (mode != 2) {
        const int j = int(float(k + 1) * (1.0f / 64.0f) * 255.0f);
        const float pos = (std::pow(2.0f, (float(j) + 1.0f / 64.0f) / 64.0f) - 1.0f) * 255.0f /
                          (std::pow(2.0f, 3.98462f) - 1.0f);
        return std::max(pos, 0.5f) * float(AudioEngine::kRate) / float(Ps3Feed::kSamples);
    }
    return Spectrum::kLowHz * std::pow(Spectrum::kHighHz / Spectrum::kLowHz, (float(k) + 0.5f) / 64.0f);
}

// earth: when the Earth is on, its own input is shown (the dots = what its music reaction uses, plus a beat light).
void drawSpectrum(Overlay &overlay, int w, int h, const Canyon &canyon, float level, const EqSettings &eq,
                  const Earth *earth = nullptr)
{
    const float unit = uiUnit(h), margin = 24 * unit;
    const float pw = std::min(float(w) - 2 * margin, 560 * unit), ph = (eq.enabled ? 172 : 150) * unit;
    const float x0 = margin, y0 = margin, meterW = 18 * unit, gap = 10 * unit;
    const float extra = eq.enabled ? 22 * unit : 0.0f; // room for the EQ text line
    const float bw = (pw - meterW - gap - 16 * unit) / float(Canyon::kCols);
    const float bx = x0 + 8 * unit, by = y0 + 26 * unit, bh = ph - 52 * unit - extra;
    overlay.rect(x0, y0, pw, ph, 0, 0, 0, 0.55f);
    char title[64];
    static const char *musicNames[] = {"off", "gentle", "strong"};
    if (earth)
        std::snprintf(title, sizeof title, "Earth input (music: %s)", musicNames[std::clamp(earth->tweaks.music, 0, 2)]);
    else
        std::snprintf(title, sizeof title, "Terrain input (%d ms ahead)",
                      int(std::lround((canyon.prefetchSeconds() + canyon.tweaks.syncMs / 1000.0f) * 1000.0f)));
    overlay.text(title, x0 + 8 * unit, y0 + 18 * unit, 15 * unit, 1, 1, 1, 0.9f);
    const float *in = canyon.inputColumns(), *row = earth ? earth->appliedColumns() : canyon.rowValues();
    float peak = 0;
    for (int i = 0; i < Canyon::kCols; i++) {
        peak = std::max(peak, in[i]);
        const float v = std::clamp(in[i] / 1.5f, 0.0f, 1.0f); // PS3 feed: ~0..1.5 for loud music
        overlay.rect(bx + i * bw, by + bh * (1 - v), std::max(bw - 1, 1.0f), bh * v, 0.45f, 0.75f, 1.0f, 0.9f);
        if (row[i] < 0) // (a column the Earth doesn't use)
            continue;
        const float r = std::clamp(earth ? row[i] / 1.5f : row[i], 0.0f, 1.0f); // (the Earth's are in input units)
        overlay.rect(bx + i * bw, by + bh * (1 - r) - 1.5f * unit, std::max(bw - 1, 1.0f), 3 * unit, 1.0f, 0.85f,
                     0.2f, 0.95f);
    }
    overlay.rect(bx + 64 * bw - 0.5f * unit, by, 1 * unit, bh, 1, 1, 1, 0.35f);
    // EQ: red squares at each column's boost/cut (middle line = 0 dB, panel edges = +-12 dB);
    // dimmer when the EQ only changes the sound and not what the terrain sees.
    if (eq.enabled) {
        Equalizer e;
        e.configure(eq, float(AudioEngine::kRate));
        const float a = eq.affectsVisualizer() ? 0.95f : 0.45f, mid = by + bh * 0.5f, sq = std::max(3 * unit, 2.0f);
        overlay.rect(bx, mid - 0.5f * unit, 128 * bw, 1 * unit, 1.0f, 0.3f, 0.3f, 0.3f);
        for (int i = 0; i < Canyon::kCols; i++) {
            const float db = std::clamp(e.responseDb(columnHz(i, canyon.tweaks.spectrumMode), float(AudioEngine::kRate)),
                                        -EqSettings::kMaxDb, EqSettings::kMaxDb);
            const float y = mid - db / EqSettings::kMaxDb * bh * 0.5f;
            overlay.rect(bx + i * bw + bw * 0.5f - sq * 0.5f, y - sq * 0.5f, sq, sq, 1.0f, 0.2f, 0.2f, a);
        }
    }
    const float ty = by + bh + 16 * unit, ts = 12 * unit;
    overlay.text("L bass", bx, ty, ts, 0.8f, 0.8f, 0.8f, 0.9f);
    const std::string mid = "highs";
    overlay.text(mid, bx + 64 * bw - overlay.measure(mid, ts) * 0.5f, ty, ts, 0.8f, 0.8f, 0.8f, 0.9f);
    const std::string rb = "bass R";
    overlay.text(rb, bx + 128 * bw - overlay.measure(rb, ts), ty, ts, 0.8f, 0.8f, 0.8f, 0.9f);
    if (eq.enabled) {
        static const char *targets[] = {"audio only", "audio + visualizer", "visualizer only"};
        std::string line = std::string("EQ (") + targets[std::clamp(eq.target, 0, 2)] + ")";
        char part[32];
        if (eq.preampDb != 0.0f) {
            std::snprintf(part, sizeof part, "   pre %+.0f", eq.preampDb);
            line += part;
        }
        bool any = false;
        for (int b = 0; b < EqSettings::kBands; b++) {
            const float g = eq.gainDb[size_t(b)];
            if (g == 0.0f)
                continue;
            const float f = EqSettings::kFreqs[b];
            if (f >= 1000)
                std::snprintf(part, sizeof part, "   %gk %+.0f", f / 1000.0f, g);
            else
                std::snprintf(part, sizeof part, "   %g %+.0f", f, g);
            line += part;
            any = true;
        }
        if (!any)
            line += "   flat";
        overlay.text(line, bx, by + bh + 16 * unit + extra, ts, 1.0f, 0.45f, 0.45f, 0.95f);
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, earth ? "max %.2f   bars: input   ticks: applied" : "max %.2f   bars: input   ticks: row", peak);
    overlay.text(buf, x0 + pw - overlay.measure("audio", ts) - overlay.measure(buf, ts) - 24 * unit, y0 + 18 * unit, ts,
                 0.8f, 0.8f, 0.8f, 0.9f);
    // Audio level meter: what is playing now, so it should pulse with what you hear.
    const float mx = x0 + pw - meterW - 8 * unit, lv = std::clamp(level * 3.0f, 0.0f, 1.0f);
    overlay.rect(mx, by, meterW, bh, 1, 1, 1, 0.12f);
    overlay.rect(mx, by + bh * (1 - lv), meterW, bh * lv, 0.4f, 1.0f, 0.5f, 0.9f);
    overlay.text("audio", x0 + pw - overlay.measure("audio", ts) - 8 * unit, y0 + 18 * unit, ts, 0.4f, 1.0f, 0.5f, 0.9f);
    if (earth && earth->tweaks.music == 2) { // beat light (flashes on each detected beat) and the glow's bounce
        const float b = std::clamp(earth->beat(), 0.0f, 1.0f), lx = bx + 64 * bw + 10 * unit, ly = by + 4 * unit, s = 10 * unit;
        overlay.rect(lx, ly, s, s, 1.0f, 0.85f, 0.2f, 0.15f + 0.8f * b);
        char g[32];
        std::snprintf(g, sizeof g, "beat   glow %+.2f", double(earth->glowBounce()));
        overlay.text(g, lx + s + 6 * unit, ly + s - 1 * unit, ts, 1.0f, 0.85f, 0.2f, 0.9f);
    }
}

// Where the preset bar goes (top centre, or bottom centre above a centred transport bar).
// The preset bar: x, y, width, height (its width follows the preset name).
std::array<float, 4> presetBarRect(Overlay &overlay, int w, int h, const std::string &name, bool playing)
{
    // Same button size and padding as the playback bar (52 + 2 x 10).
    const float unit = uiUnit(h) * gLayout.presetScale, bh = 72 * unit, bw = 52 * unit;
    const float nameW = std::max(overlay.measure(name, 22 * unit), 140 * unit) + 32 * unit;
    const float total = bw * 2 + nameW + 40 * unit;
    if (gLayout.presetBar[0] >= 0) {
        const auto p = customPos(gLayout.presetBar, total, bh, w, h);
        return {p[0], p[1], total, bh};
    }
    float y = 24 * uiUnit(h);
    if (gLayout.presetBarPos == 1) {
        const auto t = transportRect(w, h);
        const bool stack = playing && !gLayout.vertical && gLayout.controls[0] < 0 && gLayout.controlsPos == 1;
        y = (stack ? t[1] - 12 * uiUnit(h) : float(h) - 36 * uiUnit(h)) - bh; // (bottom in line with the playback bar)
    }
    gPresetRect = {(float(w) - total) * 0.5f, y, total, bh};
    return gPresetRect;
}

void drawControls(Overlay &overlay, int w, int h, const std::string &name, bool playing, bool paused, bool shuffle,
                  int repeat, float mouseX, float mouseY, float presetAlpha, float transportAlpha,
                  std::vector<Ui::Button> &buttons)
{
    const float unitP = uiUnit(h) * gLayout.presetScale, unitT = uiUnit(h) * gLayout.controlsScale;
    // Buttons only take clicks while they're (mostly) visible.
    // Icons: the palette's text colour, kept readable on the button (ui::colourOn).
    auto iconInk = [&](float x, float y, float bw, float bh) {
        const bool hover = Ui::Button{{x, y, bw, bh}, Action::Menu}.contains(mouseX, mouseY);
        return ui::colourOn(ui::style(gUiStyle), hover ? ui::Part::ControlButtonHot : ui::Part::ControlButton, gTheme,
                            ui::Colour{ui::Role::Text}, 1);
    };
    auto button = [&](float x, float y, float bw, float bh, Action a, float alpha) {
        const Ui::Button b{{x, y, bw, bh}, a};
        const bool hover = b.contains(mouseX, mouseY);
        ui::paint(overlay, ui::style(gUiStyle), hover ? ui::Part::ControlButtonHot : ui::Part::ControlButton, x, y, bw, bh,
                  uiUnit(h), gTheme, alpha);
        if (alpha > 0.5f)
            buttons.push_back(b);
    };
    if (presetAlpha > 0.01f) {
        const float size = 22 * unitP, bw = 52 * unitP, pad = 10 * unitP; // buttons like the playback bar's
        const float nameW = std::max(overlay.measure(name, size), 140 * unitP) + 32 * unitP;
        const auto pr = presetBarRect(overlay, w, h, name, playing);
        const float x0 = pr[0], y0 = pr[1], bh = pr[3];
        const float px0 = x0 + pad, nx = x0 + pr[2] - pad - bw, by = y0 + pad;
        ui::paint(overlay, ui::style(gUiStyle), ui::Part::ControlBar, x0, y0, pr[2], bh, unitP, gTheme, presetAlpha);
        button(px0, by, bw, bw, Action::PrevPreset, presetAlpha);
        button(nx, by, bw, bw, Action::NextPreset, presetAlpha);
        drawIcon(overlay, Action::PrevPreset, 0, px0 + bw * 0.5f, by + bw * 0.5f, 9 * unitP, presetAlpha,
                 iconInk(px0, by, bw, bw));
        drawIcon(overlay, Action::NextPreset, 0, nx + bw * 0.5f, by + bw * 0.5f, 9 * unitP, presetAlpha,
                 iconInk(nx, by, bw, bw));
        const Rgba ink = ui::textOn(ui::style(gUiStyle), ui::Part::ControlBar, gTheme, presetAlpha);
        overlay.text(name, px0 + bw + 10 * unitP + (nameW - overlay.measure(name, size)) * 0.5f, y0 + bh * 0.5f + size * 0.35f, size,
                     ink.r, ink.g, ink.b, ink.a, ui::style(gUiStyle).textShadow);
    }
    if (!playing || transportAlpha <= 0.01f)
        return;
    // Transport bar: shuffle, previous, back, play/pause, forward, next, repeat.
    const Action transport[7] = {Action::Shuffle, Action::PrevTrack, Action::SeekBack, Action::PlayPause,
                                 Action::SeekForward, Action::NextTrack, Action::Repeat};
    const auto [tx, ty, tw, th] = transportRect(w, h);
    const float tb = 52 * unitT, gap = 10 * unitT, pad = 10 * unitT;
    ui::paint(overlay, ui::style(gUiStyle), ui::Part::ControlBar, tx, ty, tw, th, unitT, gTheme, transportAlpha);
    for (int k = 0; k < 7; k++) {
        const float step = float(k) * (tb + gap);
        const float bx = tx + pad + (gLayout.vertical ? 0.0f : step), by = ty + pad + (gLayout.vertical ? step : 0.0f);
        button(bx, by, tb, tb, transport[k], transportAlpha);
        const int state = transport[k] == Action::PlayPause ? int(paused)
                          : transport[k] == Action::Shuffle ? int(shuffle)
                          : transport[k] == Action::Repeat  ? repeat
                                                            : 0;
        drawIcon(overlay, transport[k], state, bx + tb * 0.5f, by + tb * 0.5f,
                 (transport[k] == Action::PlayPause ? 11 : 9) * unitT, transportAlpha, iconInk(bx, by, tb, tb));
    }
}

// Debug displays (Debug tab): frame timing and this process's resource use.
struct Stats {
    std::deque<float> frameMs; // intervals of the last 240 frames (graph, average)
    std::deque<std::pair<double, float>> timed; // (end time, ms) over the stats window
    float low1 = 0, minFps = 0, maxFps = 0;       // over the window, refreshed 4x a second
    double lastFrame = 0, lastSample = 0, prevAt = 0, lastWindowCalc = 0;
    ResourceSample prev;
    std::deque<float> cpu, memMb, readKb, writeKb; // every 0.5 s, the last 60 (30 s)
    float cpuPct = 0, readRate = 0, writeRate = 0;  // % of one core, bytes/s
    uint64_t mem = 0;
    int threads = 0;
    bool haveRes = false;

    void frame(double t)
    {
        if (lastFrame > 0)
            frameMs.push_back(float((t - lastFrame) * 1000.0));
        if (frameMs.size() > 240)
            frameMs.pop_front();
        lastFrame = t;
    }
    // Window statistics (`seconds` = how far back).
    void window(double t, double seconds)
    {
        if (!frameMs.empty())
            timed.push_back({t, frameMs.back()});
        while (!timed.empty() && t - timed.front().first > seconds)
            timed.pop_front();
        if (t - lastWindowCalc < 0.25 || timed.empty())
            return;
        lastWindowCalc = t;
        std::vector<float> ms;
        ms.reserve(timed.size());
        for (auto &f : timed)
            ms.push_back(f.second);
        // 1% low: the average frame rate over the slowest 1% of frames (at least one frame).
        const size_t n = std::max<size_t>(1, ms.size() / 100);
        std::nth_element(ms.begin(), ms.begin() + long(n - 1), ms.end(), std::greater<float>());
        float slow = 0;
        for (size_t i = 0; i < n; i++)
            slow += ms[i];
        slow /= float(n);
        const float longest = *std::max_element(ms.begin(), ms.begin() + long(n));
        const float shortest = *std::min_element(ms.begin(), ms.end());
        low1 = slow > 0 ? 1000.0f / slow : 0;
        minFps = longest > 0 ? 1000.0f / longest : 0;
        maxFps = shortest > 0 ? 1000.0f / shortest : 0;
    }
    void sample(double t)
    {
        if (t - lastSample < 0.5)
            return;
        lastSample = t;
        const ResourceSample s = sampleResources();
        if (prev.ok && s.ok && t > prevAt) {
            const double dt = t - prevAt;
            cpuPct = float((s.cpuSeconds - prev.cpuSeconds) / dt * 100.0);
            readRate = float(double(s.readBytes - std::min(s.readBytes, prev.readBytes)) / dt);
            writeRate = float(double(s.writeBytes - std::min(s.writeBytes, prev.writeBytes)) / dt);
            mem = s.memoryBytes;
            threads = s.threads;
            haveRes = true;
            for (auto [list, v] : {std::pair{&cpu, cpuPct}, std::pair{&memMb, float(double(mem) / 1048576.0)},
                                   std::pair{&readKb, readRate / 1024.0f}, std::pair{&writeKb, writeRate / 1024.0f}}) {
                list->push_back(v);
                if (list->size() > 60)
                    list->pop_front();
            }
        }
        prev = s;
        prevAt = t;
    }
};

std::string formatBytes(double b)
{
    char t[32];
    if (b < 1024)
        std::snprintf(t, sizeof t, "%.0f B", b);
    else if (b < 1048576)
        std::snprintf(t, sizeof t, "%.1f KB", b / 1024);
    else if (b < 1073741824.0)
        std::snprintf(t, sizeof t, "%.1f MB", b / 1048576);
    else
        std::snprintf(t, sizeof t, "%.2f GB", b / 1073741824.0);
    return t;
}

// Bars of `values` (newest on the right) scaled to `top`, with an optional marker line.
void drawGraph(Overlay &o, float x, float y, float gw, float gh, const std::deque<float> &values, size_t slots, float top,
               const float rgb[3], float marker = -1)
{
    o.rect(x, y, gw, gh, 1, 1, 1, 0.06f);
    if (top <= 0)
        top = 1;
    const float bw = gw / float(slots);
    for (size_t i = 0; i < values.size(); i++) {
        const float v = std::clamp(values[i] / top, 0.0f, 1.0f), bx = x + gw - bw * float(values.size() - i);
        o.rect(bx, y + gh * (1 - v), std::max(bw - 0.5f, 1.0f), gh * v, rgb[0], rgb[1], rgb[2], 0.8f);
    }
    if (marker > 0 && marker < top)
        o.rect(x, y + gh * (1 - marker / top), gw, 1, 1, 1, 1, 0.45f);
}

// Frame rate (perfMode) and resource (resMode) panels in the top right corner, under the OSD line.
void drawStats(Overlay &o, int w, int h, float unit, int perfMode, int resMode, const Stats &st, float refreshHz,
               double windowSeconds, const std::vector<std::string> *hardware = nullptr)
{
    const UiTheme &th = gTheme;
    const float pad = 10 * unit, ts = 16 * unit, lh = 21 * unit, gh = 46 * unit;
    char buf[160];
    // The text first: the panels are as wide as their longest line (at least 300 units) and stay
    // inside the window.
    std::vector<std::string> perfLines, resLines;
    float worst = 0;
    if (perfMode > 0) {
        float avg = 0;
        for (float f : st.frameMs) {
            avg += f;
            worst = std::max(worst, f);
        }
        avg = st.frameMs.empty() ? 0 : avg / float(st.frameMs.size());
        std::snprintf(buf, sizeof buf, "%.0f fps   display %.2f Hz", avg > 0 ? 1000.0 / avg : 0.0, double(refreshHz));
        perfLines.push_back(buf);
        std::snprintf(buf, sizeof buf, "frame %.2f ms   worst %.2f ms", double(avg), double(worst));
        perfLines.push_back(buf);
        std::snprintf(buf, sizeof buf, "1%% low %.0f fps (last %.0f s)", double(st.low1), windowSeconds);
        perfLines.push_back(buf);
        std::snprintf(buf, sizeof buf, "min %.0f   max %.0f fps", double(st.minFps), double(st.maxFps));
        perfLines.push_back(buf);
    }
    if (resMode > 0) {
        if (!st.haveRes) {
            resLines.push_back("measuring...");
        } else {
            std::snprintf(buf, sizeof buf, "CPU %.1f%% of one core (%d cores)", double(st.cpuPct), logicalCpus());
            resLines.push_back(buf);
            resLines.push_back("RAM " + formatBytes(double(st.mem)) +
                               (st.threads ? "   threads " + std::to_string(st.threads) : ""));
            resLines.push_back("I/O read " + formatBytes(st.readRate) + "/s");
            resLines.push_back("I/O write " + formatBytes(st.writeRate) + "/s");
        }
    }
    float textW = 0;
    if (hardware)
        for (auto &l : *hardware)
            textW = std::max(textW, o.measure(l, ts));
    for (auto *lines : {&perfLines, &resLines})
        for (auto &l : *lines)
            textW = std::max(textW, o.measure(l, ts));
    const float pw = std::min(float(w) - 16 * unit, std::max(300 * unit, textW + 2 * pad + 8 * unit));
    const float x = std::max(8 * unit, float(w) - 48 * uiUnit(h) - pw);
    float y = 48 * uiUnit(h) + 18 * unit;
    auto panel = [&](int lines, int graphs) {
        const float ph = pad * 2 + lh * float(lines) + float(graphs) * (gh + 6 * unit);
        o.rect(x, y, pw, ph, th.bg[0], th.bg[1], th.bg[2], std::min(1.0f, 0.75f * th.opacity));
        o.rect(x, y, 2 * unit, ph, th.accent[0], th.accent[1], th.accent[2], 0.9f);
        return ph;
    };
    auto text = [&](const std::string &t, float ty, float a = 0.92f) {
        o.text(t, x + pad + 4 * unit, ty, ts, th.text[0], th.text[1], th.text[2], a);
    };
    if (perfMode > 0) {
        const float ph = panel(int(perfLines.size()), perfMode == 2 ? 1 : 0);
        for (size_t k = 0; k < perfLines.size(); k++)
            text(perfLines[k], y + pad + lh * float(k) + ts, k == 0 ? 0.92f : 0.75f);
        if (perfMode == 2) {
            const float period = refreshHz > 0 ? 1000.0f / refreshHz : 16.67f;
            drawGraph(o, x + pad + 4 * unit, y + pad + lh * float(perfLines.size()) + 4 * unit, pw - 2 * pad - 4 * unit,
                      gh, st.frameMs, 240, std::max(period * 2.5f, worst * 1.1f), th.accent, period);
        }
        y += ph + 8 * unit;
    }
    if (hardware && !hardware->empty()) { // the hardware in use
        const float ph = panel(int(hardware->size()), 0);
        for (size_t k = 0; k < hardware->size(); k++)
            text((*hardware)[k], y + pad + lh * float(k) + ts, k == 0 ? 0.92f : 0.8f);
        y += ph + 8 * unit;
    }
    if (resMode > 0) {
        const float ph = panel(int(resLines.size()), resMode == 2 && st.haveRes ? 3 : 0);
        for (size_t k = 0; k < resLines.size(); k++)
            text(resLines[k], y + pad + lh * float(k) + ts, !st.haveRes ? 0.6f : k < 2 ? 0.92f : 0.8f);
        if (!st.haveRes)
            return;
        const float gy = y + pad + lh * 4 + 4 * unit;
        if (resMode == 2) {
            const float gw = pw - 2 * pad - 4 * unit, gx = x + pad + 4 * unit;
            float top = 0;
            for (float v : st.cpu)
                top = std::max(top, v);
            drawGraph(o, gx, gy, gw, gh, st.cpu, 60, std::max(25.0f, top * 1.2f), th.accent);
            float memTop = 0;
            for (float v : st.memMb)
                memTop = std::max(memTop, v);
            static const float memRgb[3] = {0.55f, 0.85f, 0.6f}, ioRgb[3] = {1.0f, 0.75f, 0.4f};
            drawGraph(o, gx, gy + gh + 6 * unit, gw, gh, st.memMb, 60, memTop * 1.15f, memRgb);
            std::deque<float> io;
            float ioTop = 0;
            for (size_t i = 0; i < st.readKb.size(); i++) {
                io.push_back(st.readKb[i] + st.writeKb[i]);
                ioTop = std::max(ioTop, io.back());
            }
            drawGraph(o, gx, gy + (gh + 6 * unit) * 2, gw, gh, io, 60, std::max(64.0f, ioTop * 1.2f), ioRgb);
            o.text("CPU", gx + 4 * unit, gy + 14 * unit, 13 * unit, 1, 1, 1, 0.6f);
            o.text("RAM", gx + 4 * unit, gy + gh + 20 * unit, 13 * unit, 1, 1, 1, 0.6f);
            o.text("I/O", gx + 4 * unit, gy + gh * 2 + 26 * unit, 13 * unit, 1, 1, 1, 0.6f);
        }
        (void)ph;
    }
}

// A longer message in a box at the top centre (word-wrapped).
void drawNotice(Overlay &o, int w, int h, float unit, const std::string &msg, float alpha)
{
    const UiTheme &th = gTheme;
    const float ts = 18 * unit, lh = 24 * unit, pad = 14 * unit, maxW = std::min(float(w) - 80 * unit, 640 * unit);
    std::vector<std::string> lines;
    std::string line, word;
    std::istringstream words(msg);
    while (words >> word) {
        const std::string next = line.empty() ? word : line + " " + word;
        if (!line.empty() && o.measure(next, ts) > maxW - 2 * pad) {
            lines.push_back(line);
            line = word;
        } else {
            line = next;
        }
    }
    if (!line.empty())
        lines.push_back(line);
    float bw = 0;
    for (auto &l : lines)
        bw = std::max(bw, o.measure(l, ts));
    bw += 2 * pad;
    const float bh = 2 * pad + lh * float(lines.size()) - 4 * unit;
    const float bx = (float(w) - bw) * 0.5f, by = 40 * uiUnit(h);
    const ui::Style &st = ui::style(gUiStyle);
    ui::paint(o, st, ui::Part::Notice, bx - 1 * unit, by - 1 * unit, bw + 2 * unit, bh + 2 * unit, unit, th, alpha);
    const Rgba ink = ui::textOn(st, ui::Part::Notice, th, alpha);
    for (size_t k = 0; k < lines.size(); k++)
        o.text(lines[k], bx + pad, by + pad + lh * float(k) + ts * 0.8f, ts, ink.r, ink.g, ink.b, ink.a, st.textShadow);
}

// Background CPU load for the stress tests: `set(n)` keeps n threads spinning.
struct CpuBurner {
    std::vector<std::thread> threads;
    std::atomic<int> want{0};
    std::atomic<bool> quit{false};
    void set(int n)
    {
        want = n;
        while (int(threads.size()) < n) {
            const int index = int(threads.size());
            threads.emplace_back([this, index] {
                volatile double sink = 0;
                while (!quit) {
                    if (index < want)
                        for (int i = 0; i < 200000; i++)
                            sink = sink + std::sqrt(double(i));
                    else
                        SDL_Delay(50);
                }
            });
        }
    }
    ~CpuBurner()
    {
        quit = true;
        for (auto &t : threads)
            t.join();
    }
};

std::string describeStress(const StressTest &st)
{
    static const char *fps[] = {"", "fps unlocked", "30 fps cap", "60 fps cap", "120 fps cap", "240 fps cap"};
    static const char *vis[] = {"", "visualizer every frame", "visualizer 120 Hz", "visualizer 240 Hz"};
    static const char *storm[] = {"", "preset storm 2 s", "preset storm 0.5 s", "preset storm every frame"};
    static const char *seek[] = {"", "seek storm 5 s", "seek storm 1 s"};
    static const char *hitch[] = {"", "stalls <=30 ms", "stalls <=100 ms"};
    static const char *cpu[] = {"", "CPU load 1 thread", "CPU load half the cores", "CPU load all cores"};
    std::string out;
    auto add = [&](const std::string &t) {
        if (!t.empty())
            out += (out.empty() ? "" : ", ") + t;
    };
    add(fps[std::clamp(st.fps, 0, 5)]);
    add(vis[std::clamp(st.visRate, 0, 3)]);
    if (st.renderPasses != 1)
        add(std::to_string(st.renderPasses) + " render passes");
    if (st.renderScale != 1)
        add("render scale " + std::to_string(int(std::lround(st.renderScale * 100))) + "%");
    add(storm[std::clamp(st.presetStorm, 0, 3)]);
    add(seek[std::clamp(st.seekStorm, 0, 2)]);
    add(hitch[std::clamp(st.hitches, 0, 2)]);
    add(cpu[std::clamp(st.cpuLoad, 0, 3)]);
    return out;
}

// The UI theme's font on the overlay, when it changed. `applied` remembers what this overlay has.
void syncThemeFont(Overlay &o, const fs::path &themesDir, std::string &applied)
{
    const ui::Style &st = ui::style(gUiStyle);
    const std::string key = st.font + "|" + std::to_string(st.fontScale) + "|" + utf8(themesDir);
    if (key == applied)
        return;
    applied = key;
    const std::vector<fs::path> files = st.font.empty() ? std::vector<fs::path>() : ui::findFonts(st.font, themesDir);
    o.setThemeFonts(files, st.fontScale);
    if (!st.font.empty()) {
        std::string list;
        for (const fs::path &f : files)
            list += (list.empty() ? "" : ", ") + utf8(f);
        SDL_Log("theme fonts: %s", list.empty() ? "none of the theme's fonts found (using the PS3 font)" : list.c_str());
    }
}

int renderOffline(const Options &opt, float gain, const EqSettings &eq, Canyon &canyon, Overlay *overlay,
                  const fs::path &devFlash)
{
    // PS3VIS_VISUALIZER=earth renders the Earth instead (PS3VIS_EARTH_VIEW=name, PS3VIS_EARTH_TIME=seconds,
    // PS3VIS_EARTH_FIRMWARE=1: the firmware shader, PS3VIS_EARTH_REALTIME=1/2 with PS3VIS_EARTH_UTC="YYYY-MM-DD HH:MM").
    // PS3VIS_DITHER=0..3: the Dithering setting's level (default low, like the setting).
    const float offlineDither = dither::amountFor(SDL_getenv("PS3VIS_DITHER") ? std::atoi(SDL_getenv("PS3VIS_DITHER")) : 1);
    Earth earth;
    earth.tweaks.firmwareShaders = SDL_getenv("PS3VIS_EARTH_FIRMWARE") != nullptr;
    if (const char *aa = SDL_getenv("PS3VIS_EARTH_AA")) // 0 off, 1 2x, 2 4x, 3 8x
        earth.tweaks.antiAlias = std::clamp(std::atoi(aa), 0, 3);
    if (const char *mu = SDL_getenv("PS3VIS_EARTH_MUSIC")) // 0 off, 1 gentle, 2 strong
        earth.tweaks.music = std::clamp(std::atoi(mu), 0, 2);
    if (const char *rt = SDL_getenv("PS3VIS_EARTH_REALTIME")) // 1 turn the Earth, 2 move the sun
        earth.tweaks.realTime = std::clamp(std::atoi(rt), 0, 2);
    const bool useEarth = SDL_getenv("PS3VIS_VISUALIZER") && !std::strcmp(SDL_getenv("PS3VIS_VISUALIZER"), "earth");
    if (useEarth) {
        std::string why;
        if (!earth.init(devFlash, why)) {
            std::fprintf(stderr, "earth: %s\n", why.c_str());
            return 1;
        }
        if (const char *v = SDL_getenv("PS3VIS_EARTH_VIEW")) {
            const auto &n = earth.presets();
            if (auto it = std::find(n.begin(), n.end(), std::string(v)); it != n.end())
                earth.setPreset(int(it - n.begin()));
        }
        if (const char *et = SDL_getenv("PS3VIS_EARTH_TIME"))
            earth.update(float(std::atof(et)), 0, nullptr);
    }
    if (opt.inputs.empty()) {
        std::fprintf(stderr, "--screenshot needs an audio file\n");
        return 2;
    }
    std::vector<float> pcm;
    TrackInfo info;
    if (!AudioEngine::decodeFile(pathFromUtf8(opt.inputs[0]), pcm, &info)) {
        std::fprintf(stderr, "cannot decode %s\n", opt.inputs[0].c_str());
        return 1;
    }
    const size_t frames = pcm.size() / 2;
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, opt.width, opt.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    float level = 0; // audio level for the debug spectrum panel
    // PS3VIS_ALBUM=1: colours from the input's album art (testing the Visualizer-tab option).
    if (SDL_getenv("PS3VIS_ALBUM") && !opt.inputs.empty()) {
        AlbumPalette pal;
        const bool ok = albumPalette(pathFromUtf8(opt.inputs[0]), pal);
        canyon.setPalette(ok ? &pal : nullptr);
        SDL_Log("album colours: %s", ok ? "from album art" : "no usable art, preset colours");
    }
    auto saveFrame = [&](int frame) {
        if (useEarth)
            earth.present(opt.width, opt.height, fbo, offlineDither);
        else
            canyon.present(opt.width, opt.height, fbo, offlineDither);
        // PS3VIS_UI_VISUALS=1: UI colours from the scene's colours (no album art).
        if (SDL_getenv("PS3VIS_UI_VISUALS")) {
            float fog[3], line[3], v, mh, ms, ah, as;
            canyon.sceneColours(fog, line);
            rgbHsv(fog, mh, ms, v);
            rgbHsv(line, ah, as, v);
            if (ms < 0.12f && as >= 0.12f)
                mh = ah, ms = as;
            if (as < 0.12f && ms >= 0.12f)
                ah = mh, as = ms;
            gTheme = themeFromHues(mh, ms, ah, as, 1.0f);
            if (SDL_getenv("PS3VIS_THEME_LOG"))
                std::fprintf(stderr, "theme %d fog %.3f %.3f %.3f line %.3f %.3f %.3f accent %.3f %.3f %.3f\n", frame,
                             fog[0], fog[1], fog[2], line[0], line[1], line[2], gTheme.accent[0], gTheme.accent[1],
                             gTheme.accent[2]);
        }
        if (const char *r = SDL_getenv("PS3VIS_UI_READABLE")) // =0: text colours exactly as given (Keep text readable off)
            gTheme.readable = r[0] != '0';
        if (overlay) {
            overlay->begin(opt.width, opt.height);
            // PS3VIS_SHOT_MENU=0|1 draws that menu tab instead (for checking the layout).
            if (const char *tab = SDL_getenv("PS3VIS_SHOT_MENU")) {
                Settings settings;
                settings.custom = canyon.presetValues(canyon.lastBuiltin());
                // PS3VIS_SHOT_CFGKEYS=1: tooltips with settings keys; =2: also values and valid values.
                settings.visualizer = SDL_getenv("PS3VIS_VISUALIZER") && !std::strcmp(SDL_getenv("PS3VIS_VISUALIZER"), "earth");
                settings.showConfigKeys = SDL_getenv("PS3VIS_SHOT_CFGKEYS") != nullptr;
                settings.showConfigValues = settings.showConfigRanges =
                    SDL_getenv("PS3VIS_SHOT_CFGKEYS") && std::atoi(SDL_getenv("PS3VIS_SHOT_CFGKEYS")) >= 2;
                // PS3VIS_SHOT_TALL=uiScale: the tall menu at that UI scale.
                if (const char *tall = SDL_getenv("PS3VIS_SHOT_TALL")) {
                    settings.menuTall = true;
                    settings.uiScale = std::max(0.5f, float(std::atof(tall)));
                }
                if (const char *col = SDL_getenv("PS3VIS_UI_COLOURS"))
                    uiColoursFromEnv(col, settings);
                if (const char *th = SDL_getenv("PS3VIS_UI_THEME")) // a UI theme index (Misc > Theme)
                    settings.uiStyle = std::atoi(th);
                settings.uiReadable = gTheme.readable;
                AudioEngine audio;
                Playlist playlist;
                // PS3VIS_SHOT_PLAYLIST=n: add the inputs n times (long-playlist tests).
                const int copies = SDL_getenv("PS3VIS_SHOT_PLAYLIST") ? std::atoi(SDL_getenv("PS3VIS_SHOT_PLAYLIST")) : 1;
                for (int k = 0; k < std::max(1, copies); k++)
                    for (auto &in : opt.inputs)
                        playlist.add(pathFromUtf8(in));
                playlist.select(0);
                Menu menu;
                if (SDL_getenv("PS3VIS_UI_VISUALS"))
                    menu.setTheme(gTheme);
                Menu::Context ctx{settings, canyon, audio, playlist, [](int) {}, [] {}, [] {}, [] {}, [](int) {}, [] {}, [] {},
                                  [](int) {}, [&] { return canyon.presets()[size_t(canyon.preset())]; },
                                  [] { return std::string("SDR"); }, [] {}, [] {},
                                  [](const std::string &msg) { SDL_Log("notify: %s", msg.c_str()); }, [] {}};
                // PS3VIS_SHOT_PAD=0|1|2: menu as seen with an Xbox / PlayStation / Steam controller.
                if (const char *ps = SDL_getenv("PS3VIS_SHOT_PAD")) {
                    const PadStyle st = PadStyle(std::clamp(std::atoi(ps), 0, 2));
                    ctx.padActive = [] { return true; };
                    ctx.padStyle = [st] { return st; };
                }
                menu.setOpen(true);
                menu.setTab(std::atoi(tab));
                // PS3VIS_SHOT_MENU=tab,row selects (and scrolls to) that row.
                if (const char *comma = std::strchr(tab, ','))
                    for (int i = std::atoi(comma + 1); i > 0; i--) {
                        SDL_KeyboardEvent down{};
                        down.key = SDLK_DOWN;
                        down.down = true;
                        menu.key(down, ctx);
                    }
                // PS3VIS_SHOT_MENUTEXT=text: type this (after the keys below), e.g. while renaming.
                // PS3VIS_SHOT_MENUKEYS=Right,Return,...: then press these keys (SDL key names).
                if (const char *keys = SDL_getenv("PS3VIS_SHOT_MENUKEYS")) {
                    std::stringstream list(keys);
                    for (std::string name; std::getline(list, name, ',');) {
                        SDL_KeyboardEvent down{};
                        down.key = SDL_GetKeyFromName(name.c_str());
                        down.down = true;
                        menu.key(down, ctx);
                        if (name == "F2")
                            if (const char *typed = SDL_getenv("PS3VIS_SHOT_MENUTEXT"))
                                menu.text(typed, ctx);
                    }
                }
                menu.draw(*overlay, opt.width, opt.height, ctx);
                // PS3VIS_SHOT_DRAG=fromY,toY (fractions of the window height): press the scrollbar
                // at the right edge of the panel, drag, release, and draw again (testing only).
                if (const char *drag = SDL_getenv("PS3VIS_SHOT_DRAG")) {
                    float fy = 0, ty = 0;
                    std::sscanf(drag, "%f,%f", &fy, &ty);
                    const float bx = float(opt.width) * 0.5f + std::min(float(opt.width) - 40 * float(opt.height) / 720,
                                                                     860.0f * float(opt.height) / 720) * 0.5f -
                                     11.0f * float(opt.height) / 720;
                    SDL_Event ev{};
                    ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                    ev.button.button = SDL_BUTTON_LEFT;
                    ev.button.x = bx;
                    ev.button.y = fy * float(opt.height);
                    menu.mouse(ev, ctx);
                    ev = {};
                    ev.type = SDL_EVENT_MOUSE_MOTION;
                    ev.motion.x = bx;
                    ev.motion.y = ty * float(opt.height);
                    menu.mouse(ev, ctx);
                    ev = {};
                    ev.type = SDL_EVENT_MOUSE_BUTTON_UP;
                    ev.button.button = SDL_BUTTON_LEFT;
                    ev.button.x = bx;
                    ev.button.y = ty * float(opt.height);
                    menu.mouse(ev, ctx);
                    overlay->end();
                    overlay->begin(opt.width, opt.height);
                    menu.draw(*overlay, opt.width, opt.height, ctx);
                }
            } else {
                // PS3VIS_SHOT_ART=1: with the input's album art; PS3VIS_SHOT_ART=only: art without the text.
                static GLuint shotArt = 0;
                static float shotAspect = 1.0f;
                if (const char *sa = SDL_getenv("PS3VIS_SHOT_ART"); sa && !shotArt && !opt.inputs.empty()) {
                    std::vector<unsigned char> rgba;
                    int aw = 0, ah = 0;
                    if (albumImage(pathFromUtf8(opt.inputs[0]), rgba, aw, ah)) {
                        glGenTextures(1, &shotArt);
                        glBindTexture(GL_TEXTURE_2D, shotArt);
                        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, aw, ah, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                        glGenerateMipmap(GL_TEXTURE_2D);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        shotAspect = float(aw) / float(ah);
                    }
                }
                presetBarRect(*overlay, opt.width, opt.height, canyon.presets()[size_t(canyon.preset())], true);
                const bool artOnly = SDL_getenv("PS3VIS_SHOT_ART") && !std::strcmp(SDL_getenv("PS3VIS_SHOT_ART"), "only");
                drawTrackInfo(*overlay, opt.width, opt.height, info, opt.start + frame * kVisStep, false, 0, 1,
                              artOnly ? 0.0f : 1.0f, -1, -1, nullptr, shotArt, shotAspect);
                if (SDL_getenv("PS3VIS_SHOT_SPECTRUM"))
                    drawSpectrum(*overlay, opt.width, opt.height, canyon, level, eq, useEarth ? &earth : nullptr);
                // PS3VIS_SHOT_STATS=perf,res (0-2 each): the debug displays with made-up numbers.
                if (const char *sm = SDL_getenv("PS3VIS_SHOT_STATS")) {
                    int perf = 2, res = 2;
                    std::sscanf(sm, "%d,%d", &perf, &res);
                    Stats st;
                    for (int i = 0; i < 240; i++)
                        st.frameMs.push_back(6.94f + 0.4f * std::sin(float(i) * 0.37f) + (i % 61 == 0 ? 9.0f : 0.0f));
                    for (int i = 0; i < 60; i++) {
                        st.cpu.push_back(18 + 6 * std::sin(float(i) * 0.3f));
                        st.memMb.push_back(410 + float(i) * 0.5f);
                        st.readKb.push_back(i % 17 == 0 ? 900.0f : 12.0f);
                        st.writeKb.push_back(i % 23 == 0 ? 300.0f : 0.0f);
                    }
                    st.cpuPct = 21.4f, st.mem = 440u << 20, st.readRate = 12288, st.writeRate = 0, st.threads = 23;
                    st.haveRes = true;
                    st.low1 = 2950, st.minFps = 1210, st.maxFps = 3604; // (wide numbers, as when unlocked)
                    drawStats(*overlay, opt.width, opt.height, uiUnit(opt.height), perf, res, st, 144.0f, 10.0);
                }
                // PS3VIS_SHOT_GLYPHS=1: every controller glyph in the three styles (checking them).
                if (SDL_getenv("PS3VIS_SHOT_GLYPHS")) {
                    const float sz = 44 * uiUnit(opt.height);
                    overlay->rect(0, 0, float(opt.width), float(opt.height), 0.05f, 0.05f, 0.07f, 0.9f);
                    const char *names[3] = {"Xbox", "PlayStation", "Steam"};
                    for (int st = 0; st < 3; st++) {
                        float x = 30 * uiUnit(opt.height), y = (90 + 190 * float(st)) * uiUnit(opt.height);
                        overlay->text(names[st], x, y - sz, 22 * uiUnit(opt.height), 1, 1, 1, 1);
                        for (int b = 0; b < int(PadButton::Count); b++) {
                            if (b == 9) { // second row
                                x = 30 * uiUnit(opt.height);
                                y += sz * 1.7f;
                            }
                            x += drawPadGlyph(*overlay, PadButton(b), PadStyle(st), x, y, sz, 1) + 14 * uiUnit(opt.height);
                        }
                    }
                }
                if (const char *msg = SDL_getenv("PS3VIS_SHOT_NOTICE"))
                    drawNotice(*overlay, opt.width, opt.height, uiUnit(opt.height), msg, 1.0f);
                if (SDL_getenv("PS3VIS_SHOT_HELP")) { // the H key help
                    Settings help;
                    help.debugKey = true;
                    const char *ps = SDL_getenv("PS3VIS_SHOT_PAD");
                    drawHelp(*overlay, opt.width, opt.height, uiUnit(opt.height), help, ps != nullptr,
                             PadStyle(ps ? std::clamp(std::atoi(ps), 0, 2) : 0));
                }
                if (SDL_getenv("PS3VIS_SHOT_CONTROLS")) {
                    std::vector<Ui::Button> buttons;
                    int shuf = 1, rep = 2; // PS3VIS_SHOT_CONTROLS=shuffle,repeat (default 1,2)
                    std::sscanf(SDL_getenv("PS3VIS_SHOT_CONTROLS"), "%d,%d", &shuf, &rep);
                    drawControls(*overlay, opt.width, opt.height, canyon.presets()[size_t(canyon.preset())], true,
                                 false, shuf != 0, rep, -1, -1, 1.0f, 1.0f, buttons);
                }
            }
            overlay->end();
        }
        std::vector<uint8_t> px(size_t(opt.width) * opt.height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, opt.width, opt.height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        for (size_t i = 3; i < px.size(); i += 4)
            px[i] = 255;
        char name[1024];
        std::snprintf(name, sizeof name, opt.screenshot.c_str(), frame);
        stbi_flip_vertically_on_write(1);
        const bool ok = stbi_write_png(name, opt.width, opt.height, 4, px.data(), opt.width * 4);
        std::printf("%s %s (%s, preset %s, t=%.2fs)\n", ok ? "wrote" : "FAILED to write", name, info.title.c_str(),
                    canyon.presets()[size_t(canyon.preset())].c_str(), opt.start + frame * kVisStep);
        return ok;
    };
    Spectrum spec(SDL_getenv("PS3VIS_FFTWIN") ? std::atoi(SDL_getenv("PS3VIS_FFTWIN")) : Spectrum::kSize);
    std::vector<float> l(Spectrum::kSize), r(Spectrum::kSize);
    std::vector<float> wl(kEqWarmup + Spectrum::kSize), wr(kEqWarmup + Spectrum::kSize);
    float columns[Canyon::kCols] = {};
    for (int f = 0; f < opt.frames; f++) {
        const double t = opt.start + f * kVisStep;
        const double ahead = std::max(0.0, double(canyon.prefetchSeconds() + canyon.tweaks.syncMs / 1000.0f));
        const long end = long((t + ahead) * AudioEngine::kRate);
        const int wide = int(wl.size());
        for (int i = 0; i < wide; i++) {
            const long s = end - wide + i;
            const bool in = s >= 0 && size_t(s) < frames;
            wl[size_t(i)] = in ? pcm[size_t(s) * 2] : 0.0f;
            wr[size_t(i)] = in ? pcm[size_t(s) * 2 + 1] : 0.0f;
        }
        analysisWindow(eq, wl, wr, l.data(), r.data());
        // PS3VIS_OFFLINE_DT simulates a slower frame rate (testing only).
        static const float simDt = SDL_getenv("PS3VIS_OFFLINE_DT") ? float(std::atof(SDL_getenv("PS3VIS_OFFLINE_DT")))
                                                                     : float(kVisStep);
        columnsFrom(spec, l.data(), r.data(), gain, simDt, info.sampleRate, canyon.tweaks.spectrumMode, columns);
        {   // meter: the audio playing at t, not the look-ahead
            float sum = 0;
            const long now = long(t * AudioEngine::kRate);
            for (long s = now - 512; s < now; s++)
                if (s >= 0 && size_t(s) < frames)
                    sum += pcm[size_t(s) * 2] * pcm[size_t(s) * 2] + pcm[size_t(s) * 2 + 1] * pcm[size_t(s) * 2 + 1];
            level = std::sqrt(sum / 1024.0f);
        }
        // PS3VIS_ALBUM_SWITCH=frame,track: switch to that track's album colours mid-render (testing).
        if (const char *as = SDL_getenv("PS3VIS_ALBUM_SWITCH")) {
            const char *comma = std::strchr(as, ',');
            if (comma && f == std::atoi(as)) {
                AlbumPalette pal;
                const bool ok = albumPalette(pathFromUtf8(comma + 1), pal);
                canyon.setPalette(ok ? &pal : nullptr);
            }
        }
        // PS3VIS_OFFLINE_SWITCH=frame,preset switches presets mid-run (testing only).
        if (const char *sw = SDL_getenv("PS3VIS_OFFLINE_SWITCH")) {
            const char *comma = std::strchr(sw, ',');
            if (comma && f == std::atoi(sw)) {
                auto &ps = canyon.presets();
                auto it = std::find(ps.begin(), ps.end(), std::string(comma + 1));
                if (it != ps.end())
                    canyon.setPreset(int(it - ps.begin()));
            }
        }
        // PS3VIS_OFFLINE_CYCLE=every,until steps to the next preset every N frames (testing only).
        if (const char *cy = SDL_getenv("PS3VIS_OFFLINE_CYCLE")) {
            const int every = std::max(1, std::atoi(cy));
            const char *comma = std::strchr(cy, ',');
            const int until = comma ? std::atoi(comma + 1) : 0;
            // PS3VIS_OFFLINE_BURST=n: n switches per step (key repeat is faster than frames)
            const int burst = SDL_getenv("PS3VIS_OFFLINE_BURST") ? std::atoi(SDL_getenv("PS3VIS_OFFLINE_BURST")) : 1;
            if (f > 0 && f < until && f % every == 0)
                for (int b = 0; b < burst; b++)
                    canyon.setPreset(canyon.step(canyon.preset(), 1));
        }
        if (useEarth) {
            canyon.update(simDt, columns); // (still analysing underneath, like live: the debug spectrum shows it)
            earth.update(simDt, level, columns);
            earth.render(opt.width, opt.height);
        } else {
            canyon.update(simDt, columns);
            canyon.render(opt.width, opt.height);
        }
        if (opt.every && (f + 1) % opt.every == 0 && f + 1 < opt.frames)
            saveFrame(f + 1);
    }
    return saveFrame(opt.frames) ? 0 : 1;
}


// Key help (H): a panel like the menu, actions grouped in two columns, keys drawn as keycaps.
void drawHelp(Overlay &o, int w, int h, float unit, const Settings &settings, bool padMode, PadStyle padStyle)
{
    const UiTheme &th = gTheme;
    const float u = std::min(unit * 1.2f, std::min(float(w) / 900.0f, float(h) / 560.0f));
    const float ts = 18 * u, ks = 15 * u, rowH = 30 * u, capH = 22 * u, capPad = 7 * u;
    struct Section {
        const char *title;
        std::vector<Action> actions;
    };
    const std::vector<Section> columns[2] = {
        {{"Playback", {Action::PlayPause, Action::SeekForward, Action::SeekBack, Action::SeekForwardLong,
                       Action::SeekBackLong, Action::VolumeUp, Action::VolumeDown, Action::NextTrack,
                       Action::PrevTrack, Action::Shuffle, Action::Repeat}}},
        {{"Visuals & window", {Action::NextPreset, Action::PrevPreset, Action::TrackInfo, Action::AlbumArt,
                               Action::Fullscreen}},
         {"App", {Action::Menu, Action::Help, Action::Quit, Action::DebugDump}}}};

    // A binding split into keycaps: "Shift+Right" -> {"Shift", "Right"}.
    auto caps = [](const KeyBind &b) {
        std::vector<std::string> out;
        std::string d = Keybinds::describe(b);
        for (size_t at; (at = d.find('+')) != std::string::npos && at + 1 < d.size();) {
            out.push_back(d.substr(0, at));
            d = d.substr(at + 1);
        }
        out.push_back(d);
        return out;
    };
    // A controller button travels as "\x01" + a letter; it's drawn as its glyph instead of a keycap.
    auto padOf = [](const std::string &k) { return k.size() == 2 && k[0] == '\x01' ? PadButton(k[1] - 'A') : PadButton::None; };
    auto capW = [&](const std::string &k) {
        if (const PadButton b = padOf(k); b != PadButton::None)
            return padGlyphWidth(o, b, padStyle, capH * 1.2f);
        return o.measure(k, ks) + 2 * capPad;
    };
    auto bindings = [&](Action a) {
        std::vector<std::vector<std::string>> out;
        if (a == Action::DebugDump && !settings.debugKey)
            return out;
        if (padMode) { // the controller is in use: its button
            if (const PadButton b = settings.keys.pad[int(a)]; b != PadButton::None)
                out.push_back({std::string("\x01") + char('A' + int(b))});
            return out;
        }
        for (auto &b : settings.keys.slot[int(a)])
            if (b.key)
                out.push_back(caps(b));
        return out;
    };
    const float plusW = o.measure("+", ks) + 6 * u, orW = o.measure("or", ks) + 14 * u;
    auto keysWidth = [&](const std::vector<std::vector<std::string>> &bs) {
        float x = 0;
        for (size_t n = 0; n < bs.size(); n++) {
            if (n)
                x += orW;
            for (size_t c = 0; c < bs[n].size(); c++)
                x += capW(bs[n][c]) + (c ? plusW : 0);
        }
        return x;
    };

    // Column sizes.
    float labelW[2] = {}, keyW[2] = {}, colH[2] = {};
    for (int c = 0; c < 2; c++)
        for (auto &sec : columns[c]) {
            colH[c] += rowH * 1.2f;
            for (Action a : sec.actions) {
                const auto bs = bindings(a);
                if (bs.empty())
                    continue;
                labelW[c] = std::max(labelW[c], o.measure(Keybinds::label(a), ts));
                keyW[c] = std::max(keyW[c], keysWidth(bs));
                colH[c] += rowH;
            }
        }
    const float gap = 24 * u, pad = 26 * u, colGap = 44 * u;
    const float colW[2] = {labelW[0] + gap + keyW[0], labelW[1] + gap + keyW[1]};
    const float headH = 54 * u, footH = 56 * u;
    const float pw = pad * 2 + colW[0] + colGap + colW[1];
    const float ph = headH + std::max(colH[0], colH[1]) + footH + 10 * u;
    // Too big for the window (long bindings, small windows): lay it out again smaller.
    if (const float k = std::min(float(w) * 0.96f / pw, float(h) * 0.96f / ph); k < 0.99f)
        return drawHelp(o, w, h, u * k / 1.2f, settings, padMode, padStyle);
    const float px = (float(w) - pw) * 0.5f, py = (float(h) - ph) * 0.5f;
    using ui::Part;
    const ui::Style &st = ui::style(gUiStyle);
    const bool sh = st.textShadow;
    auto say = [&](const std::string &t, float x, float y, float size, const Rgba &c) { o.text(t, x, y, size, c.r, c.g, c.b, c.a, sh); };
    auto ink = [&](Part part, float alpha) { return ui::textOn(st, part, th, alpha); };
    auto line = [&](float x, float y, float lw, float lh, const Rgba &c) { o.rect(x, y, lw, lh, c.r, c.g, c.b, c.a); };

    // Panel, title band (on the panel's body, under its border).
    ui::paint(o, st, Part::Help, px, py, pw, ph, u, th, 1, ui::Body);
    ui::paint(o, st, Part::HelpHeader, px, py, pw, headH, u, th, 1);
    ui::paint(o, st, Part::Help, px, py, pw, ph, u, th, 1, ui::Frame);
    say(padMode ? "Controller buttons" : "Keyboard shortcuts", px + pad, py + headH * 0.66f, 24 * u, ink(Part::HelpHeader, 1));
    const std::string close = padMode ? "rebind buttons in the menu > Keys" : "H closes  -  rebind keys in Esc > Keys";
    say(close, px + pw - pad - o.measure(close, ks), py + headH * 0.64f, ks, ink(Part::HelpHeader, 0.5f));

    // A keycap: tinted face, a darker accent "edge" underneath, the key name in the accent colour.
    auto cap = [&](const std::string &k, float x, float baseY) {
        if (const PadButton b = padOf(k); b != PadButton::None)
            return drawPadGlyph(o, b, padStyle, x, baseY, capH * 1.2f, 1);
        const float cw = capW(k), top = baseY - capH * 0.5f;
        ui::paint(o, st, Part::Keycap, x, top, cw, capH, u, th, 1);
        const Rgba c = ink(Part::Keycap, 1);
        o.text(k, x + capPad, top + capH * 0.73f, ks, c.r, c.g, c.b, c.a, false);
        return cw;
    };

    // Columns, with a divider between them.
    const float top = py + headH + 10 * u;
    line(px + pad + colW[0] + colGap * 0.5f, top + 6 * u, 1 * u, std::max(colH[0], colH[1]) - 12 * u, ink(Part::Help, 0.1f));
    for (int c = 0; c < 2; c++) {
        const float cx = px + pad + (c ? colW[0] + colGap : 0), keyX = cx + labelW[c] + gap;
        float y = top;
        for (auto &sec : columns[c]) {
            const float tw = o.measure(sec.title, 15 * u);
            const Rgba title = ink(Part::SectionTitle, 1);
            say(sec.title, cx, y + rowH * 0.85f, 15 * u, title);
            line(cx + tw + 10 * u, y + rowH * 0.66f, colW[c] - tw - 10 * u, 1 * u, Rgba{title.r, title.g, title.b, 0.25f});
            y += rowH * 1.2f;
            bool first = true;
            for (Action a : sec.actions) {
                const auto bs = bindings(a);
                if (bs.empty())
                    continue;
                if (!first) // row divider
                    line(cx, y, colW[c], 1 * u, ink(Part::Help, 0.06f));
                first = false;
                const float mid = y + rowH * 0.5f;
                say(Keybinds::label(a), cx, mid + ts * 0.35f, ts, ink(Part::Help, 0.92f));
                float x = keyX;
                for (size_t n = 0; n < bs.size(); n++) {
                    if (n) {
                        say("or", x + 7 * u, mid + ks * 0.35f, ks, ink(Part::Help, 0.4f));
                        x += orW;
                    }
                    for (size_t k = 0; k < bs[n].size(); k++) {
                        if (k) {
                            say("+", x + 3 * u, mid + ks * 0.35f, ks, ink(Part::Help, 0.5f));
                            x += plusW;
                        }
                        x += cap(bs[n][k], x, mid);
                    }
                }
                y += rowH;
            }
        }
    }

    // Footer: mouse.
    const float fy = py + ph - footH * 0.5f;
    line(px + pad, py + ph - footH, pw - 2 * pad, 1 * u, ink(Part::Help, 0.1f));
    float x = px + pad;
    std::vector<std::pair<std::string, std::string>> footer = {
        {"Right-click", "menu"}, {"Double-click", "fullscreen"}, {"Wheel", "volume"}, {"Drop files", "add to playlist"}};
    if (padMode) // controller: the menu buttons
        footer = {{std::string("\x01") + char('A' + int(PadButton::South)), "select (in the menu)"},
                  {std::string("\x01") + char('A' + int(PadButton::East)), "back / close"},
                  {std::string("\x01") + char('A' + int(PadButton::LeftShoulder)), "/"},
                  {std::string("\x01") + char('A' + int(PadButton::RightShoulder)), "menu tabs"}};
    for (auto &[k, what] : footer) {
        x += cap(k, x, fy) + 8 * u;
        say(what, x, fy + ks * 0.35f, ks, ink(Part::Help, 0.7f));
        x += o.measure(what, ks) + 22 * u;
    }
}

} // namespace

int main(int argc, char **argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt))
        return 2;
    const bool offline = !opt.screenshot.empty() || opt.listPresets;

    if (!SDL_Init(SDL_INIT_VIDEO | (offline ? 0 : SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetAppMetadata("PS3 Visualizer", "1.0", "io.github.ps3vis");
    gLog.next = SDL_GetDefaultLogOutputFunction();
    SDL_SetLogOutputFunction(&LogRing::output, &gLog);

    // Where the user last pointed us to the firmware (see the "Locate" dialog below).
    fs::path firmwareNote;
    if (char *pref = SDL_GetPrefPath("ps3vis", "PS3 Visualizer")) {
        firmwareNote = pathFromUtf8(pref) / "firmware.txt";
        // startup.log: every start's first log lines (for starts that fail where nothing can be shown,
        // e.g. the Steam Deck's Game Mode).
        if (!offline)
            gLog.startupFile = std::fopen(utf8(pathFromUtf8(pref) / "startup.log").c_str(), "w");
        SDL_free(pref);
    }
    {
        auto env = [](const char *n) { const char *v = SDL_getenv(n); return v ? v : "-"; };
        SDL_Log("PS3 Visualizer starting: build %s (%s), %s", PS3VIS_BUILD_TARGET, PS3VIS_BUILD_TYPE, __DATE__);
        SDL_Log("session: desktop %s, type %s, wayland %s, x11 %s, gamescope %s, Steam Game Mode %s, AppImage %s",
                env("XDG_CURRENT_DESKTOP"), env("XDG_SESSION_TYPE"), env("WAYLAND_DISPLAY"), env("DISPLAY"),
                env("GAMESCOPE_WAYLAND_DISPLAY"), env("SteamGamepadUI"), SDL_getenv("APPIMAGE") ? "yes" : "no");
    }
    std::string firmwareHint = opt.firmware;
    if (firmwareHint.empty() && !firmwareNote.empty()) {
        std::ifstream in(firmwareNote);
        std::getline(in, firmwareHint);
    }
    SDL_Log("firmware: remembered folder \"%s\"", firmwareHint.c_str());
    auto devFlash = findDevFlash(firmwareHint, exeDir());
    if (!devFlash && !firmwareHint.empty() && opt.firmware.empty())
        devFlash = findDevFlash("", exeDir()); // remembered folder gone: search the usual places
    // Not found: let the user pick their RPCS3 (or dev_flash) folder, and remember it.
    while (!devFlash && !offline) {
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Locate RPCS3 folder..."},
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
        };
        const SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, nullptr, "PS3 Visualizer",
                                     "The PS3 system software files (dev_flash) weren't found.\n\n"
                                     "Install PS3UPDAT.PUP in RPCS3 (File > Install Firmware), then choose "
                                     "the RPCS3 folder (the one with RPCS3 installed, or dev_flash, in it).",
                                     2, buttons, nullptr};
        int choice = 0;
        if (!SDL_ShowMessageBox(&box, &choice)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "firmware not found, and no message box could be shown: %s",
                         SDL_GetError());
            break;
        }
        if (choice != 1)
            break;
        struct Pick {
            bool done = false;
            std::string path;
        } pick;
        // macOS: unlike the message box above, SDL_ShowOpenFolderDialog needs a real parent window to
        // actually display - without one it fails silently and the callback fires immediately with
        // nothing picked. No window exists yet this early, so make a throwaway hidden one just for this.
        SDL_Window *dialogWindow = SDL_CreateWindow("PS3 Visualizer", 1, 1, SDL_WINDOW_HIDDEN);
        SDL_ShowOpenFolderDialog(
            [](void *user, const char *const *list, int) {
                auto *p = static_cast<Pick *>(user);
                if (list && list[0])
                    p->path = list[0];
                p->done = true;
            },
            &pick, dialogWindow, nullptr, false);
        while (!pick.done) {
            SDL_PumpEvents();
            SDL_Delay(20);
        }
        if (dialogWindow)
            SDL_DestroyWindow(dialogWindow);
        if (pick.path.empty())
            continue;
        devFlash = findDevFlash(pick.path, exeDir());
        if (devFlash && !firmwareNote.empty()) {
            std::ofstream out(firmwareNote);
            out << utf8(*devFlash) << "\n";
        }
    }
    if (!devFlash) {
        fail("Could not find PS3 firmware files (dev_flash).\n\n"
             "Install the official PS3 system software (PS3UPDAT.PUP) in RPCS3, then either run this from a "
             "folder that contains dev_flash or pass --firmware /path/to/dev_flash.");
        SDL_Quit();
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    // Saved settings, overridden by the command line (loaded early: the HDR choice
    // decides how the window is created).
    fs::path settingsFile, themesDir;
    if (char *pref = SDL_GetPrefPath("ps3vis", "PS3 Visualizer")) {
        settingsFile = pathFromUtf8(pref) / "settings.ini";
        themesDir = pathFromUtf8(pref) / "themes";
        SDL_free(pref);
    }
    // The themes' bundled free fonts: next to the program (Windows, a build folder's ../fonts) or installed.
    ui::setBundledFontDirs({exeDir() / "fonts", exeDir() / ".." / "fonts", exeDir() / ".." / "share" / "ps3vis" / "fonts"});
    // User UI themes first: settings.ini names its theme.
    std::string themeErrors;
    if (!themesDir.empty())
        for (const std::string &e : ui::loadUserStyles(themesDir))
            themeErrors += (themeErrors.empty() ? "Theme file " : "\nTheme file ") + e;
    if (!themeErrors.empty())
        SDL_Log("%s", themeErrors.c_str());
    // PS3VIS_THEME_ROUNDTRIP=dir (testing): writes every theme as a file into dir, reads them back and checks each
    // comes back the same. Exits.
    if (const char *rt = SDL_getenv("PS3VIS_THEME_ROUNDTRIP")) {
        const fs::path dir = pathFromUtf8(rt);
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::vector<ui::Style> originals = ui::styles();
        for (size_t i = 0; i < originals.size(); i++) { // (on its own base, and spelled out from PS3)
            std::ofstream(dir / pathFromUtf8("t" + std::to_string(i) + ".theme"))
                << ui::styleText(originals[i], "RT " + originals[i].name, originals[i].defaults);
            std::ofstream(dir / pathFromUtf8("p" + std::to_string(i) + ".theme"))
                << ui::styleText(originals[i], "RP " + originals[i].name, originals[i].defaults, &ui::builtinStyles()[0]);
        }
        for (const std::string &e : ui::loadUserStyles(dir))
            std::printf("error: %s\n", e.c_str());
        int bad = 0;
        for (const ui::Style &o : originals) {
            for (const char *prefix : {"RT ", "RP "}) {
                const int at = ui::styleIndex(prefix + o.name);
                const ui::Style *ps3 = &ui::builtinStyles()[0];
                const bool same = at >= 0 && ui::styleText(ui::style(at), "x", o.defaults, ps3) == ui::styleText(o, "x", o.defaults, ps3);
                std::printf("%s %s%s\n", same ? "same" : "DIFFERENT", prefix, o.name.c_str());
                bad += !same;
            }
        }
        return bad ? 1 : 0;
    }
    // PS3VIS_CONTRAST_REPORT=1 (testing): text contrast of every theme's elements under its own colours and palettes
    // from the visuals, before and after ui::readableOn ("fixed" = the guard changed it). Exits.
    if (SDL_getenv("PS3VIS_CONTRAST_REPORT")) {
        using ui::Part;
        struct Pal {
            const char *name;
            UiTheme t;
        };
        std::vector<Pal> pals;
        for (const float hue : {0.0f, 60.0f, 120.0f, 210.0f, 280.0f})
            pals.push_back({"visuals", themeFromHues(hue, 0.9f, hue + 30.0f, 0.9f, 1.0f)});
        UiTheme dark = uiTheme(Settings{}); // a user's dark text on the PS3 panel
        hsvRgb(220, 0.6f, 0.25f, dark.text);
        pals.push_back({"dark text", dark});
        UiTheme pale = uiTheme(Settings{}); // a pale accent
        hsvRgb(60, 0.3f, 1.0f, pale.accent);
        pals.push_back({"pale accent", pale});
        int adjusted = 0;
        for (size_t i = 0; i < ui::builtinStyles().size(); i++) {
            const ui::Style &st = ui::builtinStyles()[i];
            UiTheme own{};
            std::copy(st.defaults[0], st.defaults[0] + 3, own.accent);
            std::copy(st.defaults[1], st.defaults[1] + 3, own.text);
            std::copy(st.defaults[2], st.defaults[2] + 3, own.bg);
            std::copy(st.defaults[3], st.defaults[3] + 3, own.highlight);
            own.opacity = 1;
            std::vector<Pal> all = {{"own", own}};
            all.insert(all.end(), pals.begin(), pals.end());
            for (const Pal &pal : all)
                for (int p = 0; p < int(Part::Count); p++) {
                    UiTheme raw = pal.t;
                    raw.readable = false;
                    const Rgba before = ui::textOn(st, Part(p), raw, 1), after = ui::textOn(st, Part(p), pal.t, 1);
                    const float c0 = ui::textContrast(st, Part(p), raw, before), c1 = ui::textContrast(st, Part(p), raw, after);
                    const bool fixed = std::fabs(after.r - before.r) + std::fabs(after.g - before.g) +
                                           std::fabs(after.b - before.b) + std::fabs(after.a - before.a) > 1e-4f;
                    adjusted += fixed;
                    if (fixed || c0 < 4.5f)
                        std::printf("%-22s %-11s %-18s %5.2f -> %5.2f %s\n", st.name.c_str(), pal.name,
                                    ui::partName(Part(p)), double(c0), double(c1), fixed ? "fixed" : "");
                }
        }
        std::printf("%d adjusted\n", adjusted);
        return 0;
    }
    Settings settings;
    if (!settingsFile.empty())
        settings.load(settingsFile);

    // HDR: present through Vulkan (OpenGL can't output HDR); Auto does so only when a display
    // has HDR switched on. PS3VIS_HDR=auto|on|off overrides it for this run.
    int hdrMode = settings.hdr;
    bool hdrSwitchedOff = false; // HDR failed at start-up and the setting was set to Off
    bool hdrProbe = false;      // Auto: try Vulkan, keep it only if the display offers HDR output
    std::string startupNotice;  // shown in a box once the window is up
    if (const char *e = SDL_getenv("PS3VIS_HDR"))
        hdrMode = !std::strcmp(e, "on") ? 1 : !std::strcmp(e, "off") ? 2 : 0;
    bool useVulkan = !offline && hdrMode != 2;
    if (useVulkan && hdrMode == 0) {
        bool anyHdr = false;
        int count = 0;
        if (SDL_DisplayID *displays = SDL_GetDisplays(&count)) {
            for (int i = 0; i < count; i++)
                anyHdr |= SDL_GetBooleanProperty(SDL_GetDisplayProperties(displays[i]),
                                                 SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
            SDL_free(displays);
        }
        useVulkan = anyHdr;
        SDL_Log("HDR auto: %s", anyHdr ? "a display has HDR on, using the Vulkan presenter" : "no HDR display");
#if defined(_WIN32)
        // SDL rarely reports HDR on Windows: ask Vulkan whether the display offers HDR output.
        if (!anyHdr) {
            useVulkan = hdrProbe = true;
            SDL_Log("HDR auto: probing the display through Vulkan");
        }
#endif
    }

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    // PS3VIS_TEST_FIXED=1: fixed-size window, which tiling compositors such as
    // Hyprland float instead of retiling (used for test runs).
    if (SDL_getenv("PS3VIS_TEST_FIXED"))
        flags &= ~SDL_WINDOW_RESIZABLE;
    if (offline)
        flags |= SDL_WINDOW_HIDDEN;
    if (opt.fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;
    const int winW = SDL_getenv("PS3VIS_TEST_FIXED") ? 960 : 1280, winH = SDL_getenv("PS3VIS_TEST_FIXED") ? 540 : 720;
    SDL_Window *window = nullptr, *glWindow = nullptr; // glWindow: hidden, holds the GL context (Vulkan mode)
    SDL_GLContext context = nullptr;
    std::unique_ptr<VkPresenter> presenter;
    if (useVulkan) {
        // The visible window is a Vulkan one; OpenGL renders off-screen on a hidden helper window.
        window = SDL_CreateWindow("PS3 Visualizer", winW, winH, (flags & ~SDL_WINDOW_OPENGL) | SDL_WINDOW_VULKAN);
        glWindow = window ? SDL_CreateWindow("ps3vis gl", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN) : nullptr;
        context = glWindow ? SDL_GL_CreateContext(glWindow) : nullptr;
        std::string why = SDL_GetError();
        if (context && gl::load()) {
            presenter = std::make_unique<VkPresenter>();
            presenter->setBoost(settings.hdrBoost);
            presenter->setPreferHdr(hdrMode == 1);
            // PS3VIS_TEST_HDR_FAIL=1: act as if the HDR presenter failed (tests the fallback).
            if (SDL_getenv("PS3VIS_TEST_HDR_FAIL") || !presenter->init(window, hdrMode != 2, why)) {
                if (SDL_getenv("PS3VIS_TEST_HDR_FAIL"))
                    why = "simulated failure";
                presenter.reset();
            } else if (!presenter->offersHdr()) {
                if (hdrProbe) {
                    SDL_Log("HDR auto: the display offers no HDR output; using OpenGL");
                    presenter.reset();
                    why = "no HDR display";
                } else if (hdrMode == 1) {
                    startupNotice = "HDR is set to On, but this display offers no HDR output, so the picture is in SDR. "
                                    "Is HDR switched on in your system's display settings?";
                }
            }
        }
        if (!presenter && !hdrProbe) {
            // HDR was wanted but doesn't work here at all: switch it off (saved below) and say so.
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "HDR output doesn't work (%s); switching HDR off", why.c_str());
            if (!SDL_getenv("PS3VIS_HDR")) {
                settings.hdr = 2;
                hdrSwitchedOff = true;
            }
            startupNotice = "HDR output doesn't work on this system (" + why +
                            "), so HDR has been switched off. You can try again in Esc > Visualizer > HDR "
                            "(needs a restart).";
        }
        if (!presenter) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "HDR/Vulkan output unavailable (%s); using OpenGL", why.c_str());
            if (context)
                SDL_GL_DestroyContext(context);
            context = nullptr;
            if (glWindow)
                SDL_DestroyWindow(glWindow);
            if (window)
                SDL_DestroyWindow(window);
            window = glWindow = nullptr;
        }
    }
    if (!window) {
        window = SDL_CreateWindow("PS3 Visualizer", winW, winH, flags);
        if (!window) {
            fail(std::string("Cannot create window: ") + SDL_GetError());
            return 1;
        }
        context = SDL_GL_CreateContext(window);
        if (!context || !gl::load()) {
            fail(std::string("OpenGL 3.3 is required: ") + SDL_GetError());
            return 1;
        }
        SDL_GL_SetSwapInterval(1);
    }
    SDL_Log("firmware: %s", utf8(*devFlash).c_str());
    SDL_Log("GL: %s / %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    Canyon canyon;
    std::string error;
    if (!canyon.init(*devFlash, error)) {
        fail("Cannot start the visualizer: " + error);
        return 1;
    }
    std::mt19937 rng{std::random_device{}()};
    auto presetIndex = [&](const std::string &name) {
        auto &ps = canyon.presets();
        auto it = std::find(ps.begin(), ps.end(), name);
        return it == ps.end() ? -1 : int(it - ps.begin());
    };
    if (opt.listPresets) {
        for (auto &n : canyon.presets())
            std::printf("%s\n", n.c_str());
        canyon.shutdown();
        SDL_Quit();
        return 0;
    }
    // (settings were loaded before the window was created; see above)
    // Command-line options apply to this run only: remember the saved values so
    // they're written back instead of the overrides (unless changed in the app).
    const Settings fromFile = settings;
    if (opt.gain >= 0)
        settings.gain = opt.gain;
    if (opt.volume >= 0)
        settings.volume = std::clamp(opt.volume, 0.0f, 1.5f);
    if (opt.shuffle)
        settings.shuffle = true;
    if (opt.randomPresets)
        settings.randomPresets = true;
    // Test-only overrides (not saved): PS3VIS_TILES=0|1|2, PS3VIS_LIFT=0|1
    if (const char *e = SDL_getenv("PS3VIS_TILES"))
        settings.tweaks.fallingTiles = std::atoi(e);
    // PS3VIS_STRESS=fps,visRate,passes,scale,presetStorm,seekStorm,hitches,cpu: stress tests for this run.
    if (const char *e = SDL_getenv("PS3VIS_STRESS")) {
        StressTest &st = settings.stress;
        std::sscanf(e, "%d,%d,%d,%f,%d,%d,%d,%d", &st.fps, &st.visRate, &st.renderPasses, &st.renderScale,
                    &st.presetStorm, &st.seekStorm, &st.hitches, &st.cpuLoad);
    }
    if (SDL_getenv("PS3VIS_BLUR_HQ")) // high-quality blur for this run (not saved)
        settings.tweaks.blurHq = true;
    if (const char *e = SDL_getenv("PS3VIS_LIFT"))
        settings.tweaks.keepAboveTerrain = std::atoi(e) != 0;
    if (const char *e = SDL_getenv("PS3VIS_WAVE"))
        settings.tweaks.waveHeight = float(std::atof(e));
    if (const char *e = SDL_getenv("PS3VIS_DRIFT"))
        settings.tweaks.drift = float(std::atof(e));
    // PS3VIS_EQ=g31,g62,...,g16k (dB): enable the EQ for audio and visualizer (testing).
    if (const char *e = SDL_getenv("PS3VIS_EQ")) {
        settings.eq.enabled = true;
        settings.eq.target = EqSettings::AudioAndVisualizer;
        std::stringstream ss(e);
        std::string item;
        for (size_t b = 0; b < settings.eq.gainDb.size() && std::getline(ss, item, ','); b++)
            settings.eq.gainDb[b] = float(std::atof(item.c_str()));
    }
    if (const char *e = SDL_getenv("PS3VIS_SPECMODE")) // ps3 | log | linear
        settings.tweaks.spectrumMode = !std::strcmp(e, "ps3") ? 0 : !std::strcmp(e, "ps3_smooth_bass") ? 1 : 2;
    const Settings overridden = settings; // values right after the command line
    auto saveSettings = [&] {
        if (settingsFile.empty())
            return;
        // A settings file from a newer version of the player is never overwritten.
        if (fromFile.configVersion > kConfigVersion) {
            static bool told = false;
            if (!told)
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "settings.ini is format %d, this build writes %d: not saving",
                            fromFile.configVersion, kConfigVersion);
            told = true;
            return;
        }
        Settings out = settings;
        if (out.gain == overridden.gain) out.gain = fromFile.gain;
        if (out.volume == overridden.volume) out.volume = fromFile.volume;
        if (out.shuffle == overridden.shuffle) out.shuffle = fromFile.shuffle;
        if (out.randomPresets == overridden.randomPresets) out.randomPresets = fromFile.randomPresets;
        if (!opt.preset.empty() && out.preset == opt.preset) out.preset = fromFile.preset;
        // Test-only environment overrides (PS3VIS_LIFT, _DRIFT, _EQ, ...) are never saved.
        if (out.tweaks == overridden.tweaks) out.tweaks = fromFile.tweaks;
        if (out.eq == overridden.eq) out.eq = fromFile.eq;
        out.save(settingsFile);
    };
    canyon.tweaks = settings.tweaks;
    // The Custom preset starts as a copy of canyon01.
    if (settings.custom.empty())
        settings.custom = canyon.presetValues(std::max(0, presetIndex("canyon01")));
    canyon.setCustom(settings.custom);
    canyon.customInRotation = settings.customInRotation;
    if (opt.preset.empty() && presetIndex(settings.preset) >= 0)
        canyon.setPreset(presetIndex(settings.preset), 0);
    if (!opt.preset.empty()) {
        const int idx = presetIndex(opt.preset);
        if (idx < 0) {
            std::fprintf(stderr, "unknown preset %s; available:", opt.preset.c_str());
            for (auto &n : canyon.presets())
                std::fprintf(stderr, " %s", n.c_str());
            std::fprintf(stderr, "\n");
            return 2;
        }
        canyon.setPreset(idx, 0);
        settings.preset = opt.preset;
    }

    if (!opt.screenshot.empty()) {
        Overlay overlay;
        const bool withInfo = opt.overlay && overlay.init(fontFiles(*devFlash));
        gLayout = layoutFrom(settings);
        // PS3VIS_UI=scale,info,controls,presetbar overrides the layout (testing only).
        if (const char *ui = SDL_getenv("PS3VIS_UI"))
        {
            int vertical = gLayout.vertical;
            std::sscanf(ui, "%f,%d,%d,%d,%f,%d", &gLayout.scale, &gLayout.infoPos, &gLayout.controlsPos,
                        &gLayout.presetBarPos, &gLayout.infoScale, &vertical);
            gLayout.vertical = vertical != 0;
        }
        // PS3VIS_UI_THEME=n: the UI theme for this render (Misc -> Theme).
        if (const char *th = SDL_getenv("PS3VIS_UI_THEME"))
            settings.uiStyle = std::clamp(std::atoi(th), 0, int(ui::styles().size()) - 1);
        gTheme = uiTheme(settings);
        gUiStyle = settings.uiStyle;
        std::string fontApplied;
        if (withInfo)
            syncThemeFont(overlay, themesDir, fontApplied);
        // PS3VIS_UI_COLOURS=accent,text,opacity: UI colours for this render (Misc -> Colours).
        if (const char *col = SDL_getenv("PS3VIS_UI_COLOURS")) {
            Settings tmp;
            tmp.uiStyle = settings.uiStyle;
            uiColoursFromEnv(col, tmp);
            gTheme = uiTheme(tmp);
        }
        const int rc = renderOffline(opt, settings.gain, settings.eq, canyon, withInfo ? &overlay : nullptr, *devFlash);
        if (withInfo)
            overlay.shutdown();
        canyon.shutdown();
        dither::shutdown();
        SDL_GL_DestroyContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return rc;
    }

    Overlay overlay;
    overlay.init(fontFiles(*devFlash));
    std::string overlayFont; // (syncThemeFont)
    AudioEngine audio;
    const bool haveAudio = audio.init();

    Playlist playlist;
    CpuBurner cpuBurner; // stress test: background CPU load
    Earth earth;         // the PS3's other visualizer (loaded when first chosen)
    std::string earthError;
    Gamepads gamepads;   // game controllers (Misc -> Controller)
    // PS3VIS_TEST_VIRTUAL_PAD=xbox|playstation|steam: a virtual controller (tests; see PS3VIS_LIVE_PAD).
    if (const char *vp = SDL_getenv("PS3VIS_TEST_VIRTUAL_PAD"))
        SDL_Log("virtual controller: %s", gamepads.attachVirtual(vp) ? "attached" : SDL_GetError());
    auto padStyleNow = [&] { return settings.padGlyphs == 0 ? gamepads.style() : PadStyle(settings.padGlyphs - 1); };
    auto applySettings = [&] {
        // PS3VIS_TRACE=1 turns it on for a run without saving it.
        if (const bool want = settings.verboseLog || SDL_getenv("PS3VIS_TRACE"); want != gTrace) {
            gTrace = want;
            if (gTrace)
                SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);
            else
                SDL_ResetLogPriorities();
            SDL_Log("extensive logging %s", gTrace ? "on" : "off");
        }
        if (presenter)
            presenter->setTrace(gTrace);
        TRACE("settings applied: preset %s, gain %.2f, volume %.2f, spectrum %d, hdr %d, display %d",
              settings.preset.c_str(), double(settings.gain), double(settings.volume), settings.tweaks.spectrumMode,
              settings.hdr, settings.displayMode);
        canyon.tweaks = settings.tweaks;
        earth.tweaks = settings.earth;
        if (settings.visualizer == 1 && !earth.ready()) {
            std::string why;
            if (earth.init(*devFlash, why)) {
                SDL_Log("earth visualizer loaded");
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "earth visualizer unavailable: %s", why.c_str());
                settings.visualizer = 0;
                earthError = "The Earth visualizer couldn't load (" + why + ")"; // (shown by the main loop)
            }
        }
        if (earth.ready() && !earth.presets().empty() && earth.presets()[size_t(earth.preset())] != settings.earthView) {
            const auto &n = earth.presets();
            if (auto it = std::find(n.begin(), n.end(), settings.earthView); it != n.end())
                earth.setPreset(int(it - n.begin()));
            else
                settings.earthView = n[size_t(earth.preset())];
        }
        canyon.setCustom(settings.custom);
        canyon.customInRotation = settings.customInRotation;
        audio.setVolume(volumeGain(settings));
        audio.setFade(settings.songFades ? settings.fadeSeconds : 0.0f);
        audio.setEq(settings.eq);
        playlist.shuffle = settings.shuffle;
        playlist.gap = settings.shuffleGap;
        playlist.repeat = settings.repeat != 0; // "one song" still wraps when skipping by hand
        // Stress tests: vsync off for an unlocked or capped frame rate; background CPU threads.
        if (!presenter) {
            static int swap = -1;
            const int want = settings.stress.fps == 0 ? 1 : 0;
            if (want != swap) {
                SDL_GL_SetSwapInterval(want);
                swap = want;
            }
        }
        gamepads.setEnabled(settings.controllers);
        cpuBurner.set(settings.stress.cpuLoad == 0   ? 0
                      : settings.stress.cpuLoad == 1 ? 1
                      : settings.stress.cpuLoad == 2 ? std::max(1, logicalCpus() / 2)
                                                     : logicalCpus());
        gLayout = layoutFrom(settings);
        gUiStyle = settings.uiStyle;
        if (settings.uiFromVisuals) { // (glides towards the visuals' colours every frame)
            gTheme.opacity = settings.uiOpacity;
            gTheme.readable = settings.uiReadable;
        } else
            gTheme = uiTheme(settings);
        if (presenter) {
            presenter->setAllowHdr(settings.hdr != 2);
            presenter->setVsync(settings.stress.fps == 0);
            presenter->setPreferHdr(settings.hdr == 1);
            presenter->setBoost(settings.hdrBoost);
        }
        saveSettings();
    };
    applySettings();
    for (auto &in : opt.inputs)
        if (!playlist.add(pathFromUtf8(in)))
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "no playable audio in %s", in.c_str());

    Ui ui;
    if (fromFile.configVersion > kConfigVersion)
        startupNotice += std::string(startupNotice.empty() ? "" : "\n") +
                         "Your settings were saved by a newer version of the player. They are used, but changes "
                         "made now won't be saved (so the newer version's settings stay intact).";
    if (!themeErrors.empty())
        startupNotice += std::string(startupNotice.empty() ? "" : "\n") + themeErrors;
    if (!startupNotice.empty()) {
        SDL_Log("notice: %s", startupNotice.c_str());
        ui.notice = startupNotice;
        ui.noticeUntil = 12.0; // seconds after start (now() starts near 0)
    }
    if (hdrSwitchedOff)
        saveSettings();
    Stats stats;
    auto now = [] { return double(SDL_GetTicksNS()) * 1e-9; };
    auto showOsd = [&](const std::string &s) {
        ui.osd = s;
        ui.osdUntil = now() + 2.0;
    };

    // Tries tracks starting at the playlist's current one until one opens.
    // Album-art colours for the current track (preset colours when off or no embedded art).
    std::string paletteFor; // track the palette was computed for ("" = none)
    // "UI colours from visuals": the current track's cover colours (if any).
    std::string themeArtFor;
    bool themeArtOk = false;
    AlbumPalette themeArt;
    double themeTime = 0;
    float lastThemeTarget[4][3] = {};
    auto updatePalette = [&] {
        if (!settings.albumColours && paletteFor.empty())
            return;
        const std::string want = settings.albumColours && audio.isOpen() ? utf8(playlist.current()) : std::string();
        if (want == paletteFor)
            return;
        paletteFor = want;
        AlbumPalette pal;
        const bool ok = !want.empty() && albumPalette(playlist.current(), pal);
        canyon.setPalette(ok ? &pal : nullptr);
        TRACE("album colours: %s", want.empty() ? "off" : ok ? "from album art" : "no usable art, preset colours");
    };
    // Album art next to the track info (Misc tab), loaded when the track changes.
    GLuint artTex = 0;
    float artAspect = 1.0f;
    std::string artFor;
    auto updateArt = [&] {
        const std::string want = settings.albumArtMode != 0 && audio.isOpen() ? utf8(playlist.current()) : std::string();
        if (want == artFor)
            return;
        artFor = want;
        std::vector<unsigned char> rgba;
        int aw = 0, ah = 0;
        if (!want.empty() && albumImage(playlist.current(), rgba, aw, ah) && aw > 0 && ah > 0) {
            if (!artTex)
                glGenTextures(1, &artTex);
            glBindTexture(GL_TEXTURE_2D, artTex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, aw, ah, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            artAspect = float(aw) / float(ah);
            TRACE("album art %dx%d shown", aw, ah);
        } else if (artTex) {
            glDeleteTextures(1, &artTex);
            artTex = 0;
        }
    };
    // `crossfade`: the new track fades in over the current one (song fades); otherwise it starts fresh.
    struct PrevShown { // the track a crossfade is leaving, shown fading out
        bool active = false;
        TrackInfo info;
        double pos = 0, since = 0;
        int index = 0;
        GLuint art = 0;
        float aspect = 1.0f;
    } prevShown;
    auto startCurrent = [&](bool forward, bool crossfade = false, bool fadeIn = true) {
        for (size_t tries = 0; tries < playlist.size(); tries++) {
            TRACE("%s track %d: %s", crossfade ? "crossfading to" : "opening", playlist.index(),
                  utf8(playlist.current()).c_str());
            if (crossfade ? audio.crossfadeTo(playlist.current(), settings.fadeSeconds)
                          : audio.open(playlist.current(), fadeIn)) {
                audio.setPaused(false);
                ui.infoUntil = now() + 6.0;
                const auto &info = audio.info();
                updatePalette();
                SDL_SetWindowTitle(window, (info.title + " - PS3 Visualizer").c_str());
                if (settings.randomPresets && tries == 0 && canyon.presets().size() > 2) {
                    const int last = int(canyon.presets().size()) - (canyon.customInRotation ? 1 : 2);
                    canyon.setPreset(std::uniform_int_distribution<int>(0, last)(rng));
                }
                return true;
            }
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "cannot play %s", utf8(playlist.current()).c_str());
            if (!(forward ? playlist.next() : playlist.prev()))
                break;
        }
        return false;
    };
    // Crossfade into whatever the playlist now points at, keeping the old track's info/cover on
    // screen to fade out. `before*`: the track being left.
    auto crossfadeFrom = [&](int before, const TrackInfo &beforeInfo, double beforePos, bool forward) {
        if (prevShown.art)
            glDeleteTextures(1, &prevShown.art);
        prevShown = {true, beforeInfo, beforePos, now(), before, artTex, artAspect};
        artTex = 0;
        artFor.clear(); // load the new cover
        if (!startCurrent(forward, true))
            audio.stop();
    };
    // Next/previous by hand: crossfade when song fades are on; but a skip during a crossfade, or
    // within 3 s of such a snap, cuts straight to the track (someone scrolling through songs).
    double noFadeUntil = 0;
    auto skipTrack = [&](bool forward) {
        const int before = playlist.index();
        const TrackInfo beforeInfo = audio.info();
        const double beforePos = audio.position();
        const bool snap = audio.crossfading() || now() < noFadeUntil;
        if (!(forward ? playlist.next() : playlist.prev()))
            return;
        if (settings.songFades && audio.isOpen() && !audio.paused() && !snap) {
            TRACE("skip: crossfade from track %d", before);
            crossfadeFrom(before, beforeInfo, beforePos, forward);
            return;
        }
        if (settings.songFades && snap) {
            noFadeUntil = now() + 3.0;
            TRACE("skip: snap (no fades for 3 s)");
        }
        startCurrent(forward, false, !snap);
    };
    if (!playlist.empty() && haveAudio) {
        playlist.start();
        startCurrent(true);
    }

    Spectrum spectrum(SDL_getenv("PS3VIS_FFTWIN") ? std::atoi(SDL_getenv("PS3VIS_FFTWIN")) : Spectrum::kSize);
    std::vector<float> left(Spectrum::kSize), right(Spectrum::kSize);
    std::vector<float> heardL(Spectrum::kSize), heardR(Spectrum::kSize);
    std::vector<float> wideL(kEqWarmup + Spectrum::kSize), wideR(kEqWarmup + Spectrum::kSize);
    float audioLevelNow = 0;
    float columns[Canyon::kCols] = {};
    double last = now(), lastVis = last, visAcc = kVisStep;
    float visSpeed = 1.0f; // visualizer speed (pause behaviour)

    // Debug dump: rolling history of the last ~15 s of visual frames.
    struct HistoryFrame {
        double t, audioPos;
        float visDt, frameDt;
        int preset;
        bool paused;
        Canyon::Stats st;
    };
    std::deque<HistoryFrame> history;
    bool dumpRequested = false;
    float lastFrameDt = 0;
    bool running = true;

    // Adds paths (from drag & drop or the file dialogs); starts playback if idle.
    auto addPaths = [&](const std::vector<std::string> &paths) {
        const bool idle = playlist.empty() || !audio.isOpen();
        const size_t before = playlist.size();
        size_t n = 0;
        for (auto &p : paths)
            n += playlist.add(pathFromUtf8(p));
        if (n == 0) {
            showOsd("No playable audio there");
            return;
        }
        showOsd("Added " + std::to_string(n) + (n == 1 ? " track" : " tracks"));
        if (idle && haveAudio) {
            playlist.select(int(before));
            startCurrent(true);
        }
    };
    // File dialogs answer on another thread; hand results to the main loop.
    struct DialogResults {
        std::mutex mtx;
        std::vector<std::string> paths;
    } dialogResults;
    auto dialogCallback = [](void *user, const char *const *list, int) {
        if (!list)
            return;
        auto *r = static_cast<DialogResults *>(user);
        std::lock_guard lock(r->mtx);
        for (; *list; list++)
            r->paths.push_back(*list);
    };
    // Preset import/export dialogs (results handled in the main loop, like the ones above).
    DialogResults presetImports, presetExports;
    const SDL_DialogFileFilter presetFilter{"PS3 Visualizer presets", "ps3vis;txt"};
    const SDL_DialogFileFilter audioFilter{"Audio files",
                                           "mp3;flac;ogg;opus;wav;m4a;aac;wma;aiff;aif;ape;wv;mka;ac3;at3;oma;mpc"};
    // Window, borderless fullscreen (SDL3's default "desktop" fullscreen) or exclusive
    // fullscreen (a real display mode change, at the desktop resolution).
    auto setDisplay = [&](int mode) {
        mode = std::clamp(mode, 0, 2);
        if (mode == 0) {
            SDL_SetWindowFullscreen(window, false);
        } else {
            const SDL_DisplayMode *dm =
                mode == 2 ? SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window)) : nullptr;
            if (!SDL_SetWindowFullscreenMode(window, dm))
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "fullscreen mode: %s", SDL_GetError());
            SDL_SetWindowFullscreen(window, true);
            settings.fullscreenType = mode;
        }
        settings.displayMode = mode;
        applySettings();
    };
    auto toggleFullscreen = [&] {
        setDisplay(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN ? 0 : settings.fullscreenType);
    };
    // Start in the saved display mode (--fullscreen already opened the window fullscreen and
    // isn't saved, like other command-line options).
    // (test runs with PS3VIS_TEST_FIXED stay in a small floating window)
    if (!opt.fullscreen && settings.displayMode != 0 && !SDL_getenv("PS3VIS_TEST_FIXED"))
        setDisplay(settings.displayMode);
    // Preset flow (not from the firmware): every flowSeconds of playback, morph
    // slowly to the next preset, so the camera glides to a new viewpoint.
    double flowClock = 0;
    // Like the original, the time between steps varies (17-28 s around a 22 s average).
    std::uniform_real_distribution<double> flowJitter(0.8, 1.3);
    double flowNext = settings.flowSeconds * flowJitter(rng);
    auto flowStep = [&] {
        TRACE("preset flow step from %s", canyon.presets()[size_t(canyon.preset())].c_str());
        const auto &names = canyon.presets();
        if (names.size() < 2)
            return;
        auto family = [](std::string n) {
            while (!n.empty() && std::isdigit(static_cast<unsigned char>(n.back())))
                n.pop_back();
            return n;
        };
        const int cur = canyon.preset();
        int next = canyon.step(cur, 1);
        if (settings.flow == 1) {
            const std::string fam = family(names[size_t(cur)]);
            next = cur;
            for (int k = 1; k <= int(names.size()); k++) {
                const int i = (cur + k) % int(names.size());
                if (family(names[size_t(i)]) == fam) {
                    next = i;
                    break;
                }
            }
        }
        if (next != cur)
            canyon.setPreset(next, settings.flowMorph);
    };
    // Seek to where the progress bar was clicked/dragged (x in pixels). While dragging, real
    // seeks are limited to ~10 per second; the bar follows the mouse in between.
    auto seekToBar = [&](float x, bool force) {
        const double dur = audio.duration();
        if (dur <= 0 || ui.bar[2] <= 0)
            return;
        // ui.bar is the hit area: the bar is 360 units wide inside 6-unit margins on each side.
        const float u = ui.bar[2] / 372.0f, barX = ui.bar[0] + 6 * u, barW = 360 * u;
        const double frac = std::clamp(double(x - barX) / double(barW), 0.0, 1.0);
        ui.seekPreview = frac * dur;
        const double t = now();
        if (force || t - ui.lastSeekAt > 0.1) {
            audio.seek(ui.seekPreview);
            ui.lastSeekAt = t;
        }
    };
    // Preset steps (keys, on-screen arrows, menu) only pick a pending preset; it is applied
    // once no further step arrives for kPresetSettle, so clicking through or holding the key
    // doesn't restart the visualizer for every preset passed on the way.
    constexpr double kPresetSettle = 0.08;
    int pendingPreset = -1;
    double pendingSince = 0;
    auto shownPreset = [&] { return pendingPreset >= 0 ? pendingPreset : canyon.preset(); };
    // The Earth visualizer (loaded on first use; see Settings::visualizer).
    auto earthOn = [&] { return settings.visualizer == 1 && earth.ready(); };
    auto earthViewIndex = [&] {
        const auto &n = earth.presets();
        const auto it = std::find(n.begin(), n.end(), settings.earthView);
        return it == n.end() ? 0 : int(it - n.begin());
    };
    // What the preset bar shows: the canyon preset, or the Earth's view.
    auto presetLabel = [&]() -> std::string {
        if (settings.visualizer == 1)
            return "Earth " + settings.earthView;
        return canyon.presets()[size_t(shownPreset())];
    };
    auto stepPreset = [&](int dir) {
        if (settings.visualizer == 1) {
            if (!earth.ready())
                return;
            const int n = int(earth.presets().size());
            const int next = ((earthViewIndex() + dir) % n + n) % n;
            settings.earthView = earth.presets()[size_t(next)];
            earth.setPreset(next);
            saveSettings();
            showOsd("Earth view " + settings.earthView);
            return;
        }
        pendingPreset = canyon.step(shownPreset(), dir);
        pendingSince = now();
        showOsd("Preset " + canyon.presets()[size_t(pendingPreset)]);
    };
    auto settlePreset = [&](double t) {
        if (pendingPreset < 0 || t - pendingSince < kPresetSettle)
            return;
        if (pendingPreset != canyon.preset()) {
            canyon.setPreset(pendingPreset);
            SDL_Log("preset %s", canyon.presets()[size_t(pendingPreset)].c_str());
        }
        pendingPreset = -1;
        settings.preset = canyon.presets()[size_t(canyon.preset())];
        applySettings();
    };
    Menu menu;
    Menu::Context menuCtx{settings,
                          canyon,
                          audio,
                          playlist,
                          [&](int i) {
                              playlist.select(i);
                              startCurrent(true);
                          },
                          [&] { SDL_ShowOpenFileDialog(dialogCallback, &dialogResults, window, &audioFilter, 1, nullptr, true); },
                          [&] { SDL_ShowOpenFolderDialog(dialogCallback, &dialogResults, window, nullptr, true); },
                          toggleFullscreen,
                          setDisplay,
                          [&] { running = false; },
                          applySettings,
                          stepPreset,
                          [&] { return presetLabel(); },
                          [&]() -> std::string {
                              // Vulkan mode can switch HDR live; OpenGL mode needs a restart.
                              if (presenter)
                                  return settings.hdr == 2 ? std::string("SDR (restart for OpenGL)") : presenter->describe();
                              return settings.hdr == 1 ? std::string("SDR (restart to use HDR)") : std::string("SDR");
                          },
                          [&] {
                              SDL_ShowOpenFileDialog(dialogCallback, &presetImports, window, &presetFilter, 1, nullptr,
                                                     true);
                          },
                          [&] {
                              const std::string def = utf8(presetlib::folder() / "custom.ps3vis");
                              SDL_ShowSaveFileDialog(dialogCallback, &presetExports, window, &presetFilter, 1,
                                                     def.c_str());
                          },
                          [&](const std::string &msg) { showOsd(msg); },
                          [&] {
                              menu.setOpen(false);
                              ui.arranging = true;
                              ui.dragging = -1;
                          },
                          [&] {
                              // Reset to defaults (menu button, after confirming). Kept: the playlist (not a
                              // setting), the Custom preset, the saved preset library (files) and the volume.
                              Settings fresh;
                              fresh.custom = settings.custom;
                              fresh.customInRotation = settings.customInRotation;
                              fresh.volume = settings.volume;
                              const int display = settings.displayMode;
                              settings = fresh;
                              canyon.setPreset(std::max(0, presetIndex(settings.preset)));
                              canyon.setCustom(settings.custom);
                              applySettings();
                              if (display != settings.displayMode)
                                  setDisplay(settings.displayMode);
                              SDL_Log("settings reset to defaults");
                              showOsd("Settings reset to defaults");
                          },
                          [&](bool on) { // typing a preset name
                              if (on)
                                  SDL_StartTextInput(window);
                              else
                                  SDL_StopTextInput(window);
                          },
                          [&] { return gamepads.describe(); },
                          [&] { return gamepads.active(); },
                          [&] { return padStyleNow(); },
                          [&](const std::vector<std::string> &paths) { addPaths(paths); },
                          [&] { return earth.presets(); },
                          [&] { // Misc > Copy theme to a file
                              if (themesDir.empty())
                                  return;
                              const ui::Style &from = ui::style(settings.uiStyle);
                              // A free name: "<theme> copy", "<theme> copy 2", ... (also as a file name)
                              auto fileFor = [&](const std::string &name) {
                                  std::string f = name;
                                  for (char &ch : f)
                                      if (std::strchr("/\\:*?\"<>|", ch))
                                          ch = '-';
                                  return themesDir / pathFromUtf8(f + ".theme");
                              };
                              std::string name = from.name + " copy";
                              std::error_code ec;
                              for (int n = 2; ui::styleIndex(name) >= 0 || fs::exists(fileFor(name), ec); n++)
                                  name = from.name + " copy " + std::to_string(n);
                              const UiTheme pal = uiTheme(settings);
                              const float colours[4][3] = {{pal.accent[0], pal.accent[1], pal.accent[2]},
                                                           {pal.text[0], pal.text[1], pal.text[2]},
                                                           {pal.bg[0], pal.bg[1], pal.bg[2]},
                                                           {pal.highlight[0], pal.highlight[1], pal.highlight[2]}};
                              const fs::path file = fileFor(name);
                              fs::create_directories(themesDir, ec);
                              std::ofstream out(file, std::ios::binary);
                              out << ui::styleText(from, name, colours);
                              out.close();
                              if (!out) {
                                  ui.notice = "Couldn't write " + utf8(file);
                                  ui.noticeUntil = now() + 8.0;
                                  return;
                              }
                              themeErrors.clear();
                              for (const std::string &e : ui::loadUserStyles(themesDir))
                                  themeErrors += (themeErrors.empty() ? "Theme file " : "\nTheme file ") + e;
                              settings.uiStyle = std::max(0, ui::styleIndex(name));
                              applySettings();
                              SDL_Log("theme written: %s", utf8(file).c_str());
                              ui.notice = "Saved the theme as " + utf8(file) + ". Edit it in a text editor, then reopen "
                                          "this menu to see the changes.";
                              ui.noticeUntil = now() + 12.0;
                          }};

    auto runAction = [&](Action a) {
        TRACE("action %s (menu %d, track %d at %.2f s)", Keybinds::id(a), int(menu.isOpen()), playlist.index(),
              audio.position());
        switch (a) {
        case Action::Menu:
            ui.help = false;
            menu.setOpen(true);
            break;
        case Action::Quit: running = false; break;
        case Action::PlayPause:
            if (audio.isOpen()) {
                audio.setPaused(!audio.paused());
                showOsd(audio.paused() ? "Paused" : "Play");
            }
            break;
        case Action::SeekForward: audio.seek(audio.position() + 10); break;
        case Action::SeekBack: audio.seek(audio.position() - 10); break;
        case Action::SeekForwardLong: audio.seek(audio.position() + 60); break;
        case Action::SeekBackLong: audio.seek(audio.position() - 60); break;
        case Action::VolumeUp:
        case Action::VolumeDown: {
            const float stepV = SDL_GetModState() & SDL_KMOD_SHIFT ? 0.01f : 0.05f; // Shift: 1% steps
            settings.volume = std::clamp(settings.volume + (a == Action::VolumeUp ? stepV : -stepV), 0.0f, 1.5f);
            applySettings();
            showOsd("Volume " + volumeText(settings));
            break;
        }
        case Action::NextTrack:
            skipTrack(true);
            break;
        case Action::PrevTrack:
            if (audio.position() > 3.0 && !audio.crossfading())
                audio.seek(0);
            else
                skipTrack(false);
            break;
        case Action::NextPreset: stepPreset(1); break;
        case Action::PrevPreset: stepPreset(-1); break;
        case Action::Fullscreen: toggleFullscreen(); break;
        case Action::TrackInfo:
            settings.infoAlways = !settings.infoAlways;
            applySettings();
            break;
        case Action::AlbumArt: {
            static const char *names[] = {"Album art off", "Album art: auto (with track info)", "Album art: always"};
            settings.albumArtMode = (settings.albumArtMode + 1) % 3;
            applySettings();
            showOsd(names[settings.albumArtMode]);
            break;
        }
        case Action::Shuffle:
            settings.shuffle = !settings.shuffle;
            applySettings();
            showOsd(settings.shuffle ? "Shuffle on" : "Shuffle off");
            break;
        case Action::Repeat: {
            static const char *names[] = {"Repeat off", "Repeat playlist", "Repeat one song"};
            settings.repeat = (settings.repeat + 1) % 3;
            applySettings();
            showOsd(names[settings.repeat]);
            break;
        }
        case Action::Help: ui.help = !ui.help; break;
        case Action::DebugDump:
            if (settings.debugKey)
                dumpRequested = true;
            break;
        case Action::Count: break;
        }
    };

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            gamepads.event(ev, now()); // controllers, and key/mouse use (for which glyphs to show)
            // Arranging: the mouse drags elements (snapping to margins and the centre); Enter/Esc
            // finish, R resets every position. Nothing else reacts meanwhile.
            if (ui.arranging) {
                const float d = SDL_GetWindowPixelDensity(window);
                int ww = 0, wh = 0;
                SDL_GetWindowSizeInPixels(window, &ww, &wh);
                auto elementRect = [&](int e) -> std::array<float, 4> {
                    return e == 0   ? gInfoRect
                           : e == 1 ? transportRect(ww, wh)
                                    : presetBarRect(overlay, ww, wh, presetLabel(), true);
                };
                float *slots[3] = {settings.infoXY, settings.controlsXY, settings.presetBarXY};
                bool used = true;
                if (ev.type == SDL_EVENT_KEY_DOWN) {
                    if (ev.key.key == SDLK_ESCAPE || ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) {
                        ui.arranging = false;
                        showOsd("Layout saved");
                    } else if (ev.key.key == SDLK_R) {
                        for (float *xy : slots)
                            xy[0] = xy[1] = -1;
                        settings.infoScale = settings.controlsScale = settings.presetBarScale = 1.0f;
                        showOsd("Positions and sizes reset");
                    }
                    applySettings();
                } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
                    const float mx = ev.button.x * d, my = ev.button.y * d;
                    for (int e = 2; e >= 0 && ui.dragging < 0; e--) { // (topmost first)
                        const auto r = elementRect(e);
                        if (mx >= r[0] && mx < r[0] + r[2] && my >= r[1] && my < r[1] + r[3]) {
                            ui.dragging = e;
                            ui.dragOffset[0] = mx - r[0];
                            ui.dragOffset[1] = my - r[1];
                        }
                    }
                } else if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                    // Wheel over an element resizes it.
                    float *scales[3] = {&settings.infoScale, &settings.controlsScale, &settings.presetBarScale};
                    for (int e = 2; e >= 0; e--) {
                        const auto r = elementRect(e);
                        if (ui.mouseX >= r[0] && ui.mouseX < r[0] + r[2] && ui.mouseY >= r[1] && ui.mouseY < r[1] + r[3]) {
                            // Shift: fine steps (0.01). Some systems turn Shift+wheel into a sideways wheel.
                            const float dir = ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
                            const bool fine = SDL_GetModState() & SDL_KMOD_SHIFT;
                            const float amount = ev.wheel.y != 0.0f ? ev.wheel.y : -ev.wheel.x;
                            const float stepSize = fine ? 0.01f : 0.1f;
                            *scales[e] = std::clamp(std::round((*scales[e] + stepSize * amount * dir) / stepSize) * stepSize,
                                                    0.5f, 2.5f);
                            applySettings();
                            break;
                        }
                    }
                } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                    ui.mouseX = ev.motion.x * d;
                    ui.mouseY = ev.motion.y * d;
                    if (ui.dragging >= 0) {
                        const auto r = elementRect(ui.dragging);
                        float x = ui.mouseX - ui.dragOffset[0], y = ui.mouseY - ui.dragOffset[1];
                        // Snap to the window (margins, centre) and to the other elements (aligned edges or
                        // centres, or flush side by side). Shift: no snapping.
                        if (!(SDL_GetModState() & SDL_KMOD_SHIFT)) {
                            const float m = 48 * uiUnit(wh), g = 0.0f, snap = 20.0f; // g: gap when side by side (flush)
                            std::vector<float> cx = {m * 0.5f, float(ww) - m * 0.5f - r[2], (float(ww) - r[2]) * 0.5f};
                            std::vector<float> cy = {m * 0.5f, float(wh) - m * 0.5f - r[3], (float(wh) - r[3]) * 0.5f};
                            for (int e = 0; e < 3; e++) {
                                if (e == ui.dragging)
                                    continue;
                                const auto o = elementRect(e);
                                if (o[2] <= 0)
                                    continue;
                                for (float c : {o[0], o[0] + o[2] - r[2], o[0] + (o[2] - r[2]) * 0.5f, o[0] + o[2] + g,
                                                o[0] - r[2] - g})
                                    cx.push_back(c);
                                for (float c : {o[1], o[1] + o[3] - r[3], o[1] + (o[3] - r[3]) * 0.5f, o[1] + o[3] + g,
                                                o[1] - r[3] - g})
                                    cy.push_back(c);
                            }
                            float bestX = snap, bestY = snap, sx = x, sy = y;
                            for (float c : cx)
                                if (std::fabs(x - c) < bestX)
                                    bestX = std::fabs(x - c), sx = c;
                            for (float c : cy)
                                if (std::fabs(y - c) < bestY)
                                    bestY = std::fabs(y - c), sy = c;
                            x = sx;
                            y = sy;
                        }
                        slots[ui.dragging][0] = std::clamp(x, 0.0f, float(ww) - r[2]) / float(ww);
                        slots[ui.dragging][1] = std::clamp(y, 0.0f, float(wh) - r[3]) / float(wh);
                        gLayout = layoutFrom(settings);
                    }
                } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT) {
                    if (ui.dragging >= 0)
                        applySettings(); // saves
                    ui.dragging = -1;
                } else {
                    used = false;
                }
                if (used)
                    continue;
            }
            if (gTrace)
                switch (ev.type) {
                case SDL_EVENT_KEY_DOWN:
                    TRACE("key %s%s mods 0x%x", SDL_GetKeyName(ev.key.key), ev.key.repeat ? " (repeat)" : "",
                          unsigned(ev.key.mod));
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    TRACE("mouse %d %s at %.0f,%.0f (clicks %d, menu %d)", int(ev.button.button),
                          ev.button.down ? "down" : "up", double(ev.button.x), double(ev.button.y),
                          int(ev.button.clicks), int(menu.isOpen()));
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    TRACE("wheel %.2f (menu %d)", double(ev.wheel.y), int(menu.isOpen()));
                    break;
                case SDL_EVENT_DROP_FILE:
                    TRACE("dropped %s", ev.drop.data ? ev.drop.data : "?");
                    break;
                default:
                    if (ev.type >= SDL_EVENT_WINDOW_FIRST && ev.type <= SDL_EVENT_WINDOW_LAST)
                        TRACE("window event 0x%x (data %d, %d), flags 0x%llx", unsigned(ev.type), ev.window.data1,
                              ev.window.data2, (unsigned long long)SDL_GetWindowFlags(window));
                    else if (ev.type >= SDL_EVENT_AUDIO_DEVICE_ADDED && ev.type < SDL_EVENT_SENSOR_UPDATE)
                        TRACE("audio device event 0x%x", unsigned(ev.type));
                    break;
                }
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE: // controls fade out when the mouse leaves the window
                ui.mouseX = ui.mouseY = -1;
                break;
            case SDL_EVENT_WINDOW_HDR_STATE_CHANGED: // HDR switched on/off, or moved to another display
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                if (presenter)
                    presenter->refresh();
                break;
            case SDL_EVENT_DROP_FILE:
                addPaths({ev.drop.data});
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                menu.mouse(ev, menuCtx); // ends a scrollbar drag
                if (ui.seeking && ev.button.button == SDL_BUTTON_LEFT) {
                    seekToBar(ev.button.x * SDL_GetWindowPixelDensity(window), true);
                    ui.seeking = false;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_WHEEL:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                ui.lastMouse = now();
                SDL_ShowCursor();
                if (menu.mouse(ev, menuCtx))
                    break;
                if (ev.type == SDL_EVENT_MOUSE_WHEEL && settings.wheelVolume) {
                    // Shift: 1% steps (some systems turn Shift+wheel into a sideways wheel).
                    const bool fineWheel = SDL_GetModState() & SDL_KMOD_SHIFT;
                    const float wheelAmount = ev.wheel.y != 0.0f ? ev.wheel.y : fineWheel ? -ev.wheel.x : 0.0f;
                    if (wheelAmount != 0.0f) {
                        const float dir = ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
                        settings.volume =
                            std::clamp(settings.volume + (fineWheel ? 0.01f : 0.05f) * wheelAmount * dir, 0.0f, 1.5f);
                        applySettings();
                        showOsd("Volume " + volumeText(settings));
                        break;
                    }
                }
                if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                    const float d = SDL_GetWindowPixelDensity(window);
                    ui.mouseX = ev.motion.x * d;
                    ui.mouseY = ev.motion.y * d;
                    if (settings.onScreenControls) // show the track info (and its seek bar) too
                        ui.infoUntil = std::max(ui.infoUntil, now() + 3.0);
                    if (ui.seeking)
                        seekToBar(ui.mouseX, false);
                }
                if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT && ui.bar[2] > 0) {
                    const float d = SDL_GetWindowPixelDensity(window);
                    const float bx = ev.button.x * d, by = ev.button.y * d;
                    if (bx >= ui.bar[0] && bx < ui.bar[0] + ui.bar[2] && by >= ui.bar[1] && by < ui.bar[1] + ui.bar[3]) {
                        ui.seeking = true;
                        seekToBar(bx, true);
                        break;
                    }
                }
                if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
                    const float d = SDL_GetWindowPixelDensity(window);
                    bool hit = false;
                    for (auto &b : ui.buttons)
                        if (b.contains(ev.button.x * d, ev.button.y * d)) {
                            runAction(b.action);
                            hit = true;
                            break;
                        }
                    if (hit)
                        break;
                }
                if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT &&
                    ev.button.clicks == 2)
                    toggleFullscreen();
                if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_RIGHT)
                    menu.setOpen(true);
                break;
            case SDL_EVENT_TEXT_INPUT:
                menu.text(ev.text.text, menuCtx);
                break;
            case SDL_EVENT_KEY_DOWN: {
                if (menu.key(ev.key, menuCtx))
                    break;
                ui.infoUntil = std::max(ui.infoUntil, now() + 3.0);
                Action a = settings.keys.match(ev.key.key, ev.key.mod);
                if (a == Action::Count && ev.key.key == SDLK_ESCAPE)
                    a = Action::Menu; // Esc always opens the menu, whatever the bindings
                runAction(a);
                break;
            }
            default:
                break;
            }
        }

        // Controller presses: menu navigation while the menu is open, else the bound actions.
        for (PadButton b : gamepads.poll(now())) {
            TRACE("controller %s (menu %d)", padButtonId(b), int(menu.isOpen()));
            if (menu.capturingPad()) {
                menu.padBind(b, menuCtx);
                continue;
            }
            if (ui.arranging) { // (mouse-only) a controller can at least leave it: Circle/B or Start
                if (b == PadButton::East || b == PadButton::Start) {
                    ui.arranging = false;
                    ui.dragging = -1;
                    applySettings();
                    showOsd("Layout saved");
                }
                continue;
            }
            if (menu.isOpen()) {
                SDL_KeyboardEvent k{};
                k.down = true;
                switch (b) {
                case PadButton::DpadUp: k.key = SDLK_UP; break;
                case PadButton::DpadDown: k.key = SDLK_DOWN; break;
                case PadButton::DpadLeft: k.key = SDLK_LEFT; break;
                case PadButton::DpadRight: k.key = SDLK_RIGHT; break;
                case PadButton::South: k.key = SDLK_RETURN; break;
                case PadButton::East: case PadButton::Start: k.key = SDLK_ESCAPE; break;
                case PadButton::LeftShoulder: k.key = SDLK_TAB, k.mod = SDL_KMOD_LSHIFT; break;
                case PadButton::RightShoulder: k.key = SDLK_TAB; break;
                case PadButton::North: k.key = SDLK_DELETE; break;
                default: break;
                }
                if (k.key)
                    menu.key(k, menuCtx);
                continue;
            }
            if (ui.help && b == PadButton::East) {
                ui.help = false;
                continue;
            }
            ui.infoUntil = std::max(ui.infoUntil, now() + 3.0);
            runAction(settings.keys.padMatch(b));
        }

        {
            std::vector<std::string> picked;
            {
                std::lock_guard lock(dialogResults.mtx);
                picked.swap(dialogResults.paths);
            }
            if (!picked.empty())
                addPaths(picked);
            // Preset files picked for import / the export destination.
            std::vector<std::string> imports, exports;
            {
                std::lock_guard a(presetImports.mtx);
                imports.swap(presetImports.paths);
            }
            {
                std::lock_guard b(presetExports.mtx);
                exports.swap(presetExports.paths);
            }
            int imported = 0;
            for (auto &f : imports) {
                SharedPreset p;
                if (presetlib::load(pathFromUtf8(f), canyon.paramNames(), p)) {
                    presetlib::save(p);
                    imported++;
                } else {
                    showOsd("Not a PS3 Visualizer preset: " + utf8(pathFromUtf8(f).filename()));
                }
            }
            if (imported)
                showOsd(imported == 1 ? "Imported 1 preset into the list" : "Imported " + std::to_string(imported) + " presets into the list");
            for (auto &f : exports) {
                SharedPreset p;
                p.name = canyon.presets()[size_t(canyon.lastBuiltin())] + " (custom)";
                p.values = settings.custom;
                fs::path out = pathFromUtf8(f);
                if (out.extension().empty())
                    out += ".ps3vis";
                std::ofstream(out) << presetlib::encode(p) << "\n";
                showOsd("Exported to " + utf8(out.filename()));
                TRACE("preset exported to %s", utf8(out).c_str());
            }
        }

        audio.update(); // retires the old track once a crossfade is over
        // The previous track's info/cover fade out while a crossfade runs.
        if (prevShown.active && !audio.crossfading()) {
            prevShown.active = false;
            if (prevShown.art)
                glDeleteTextures(1, &prevShown.art);
            prevShown.art = 0;
        }
        // Song fades: start crossfading into the next track this long before the current one ends.
        if (settings.songFades && audio.isOpen() && !audio.paused() && !audio.crossfading()) {
            const double left = audio.remaining();
            if (left <= double(settings.fadeSeconds) && left > 0.0 &&
                audio.position() > double(settings.fadeSeconds)) { // (not right after a seek to the end)
                const int before = playlist.index();
                const TrackInfo beforeInfo = audio.info();
                const double beforePos = audio.position();
                if (settings.repeat == 2 || playlist.next()) {
                    TRACE("crossfade: %.2f s left of track %d", left, before);
                    crossfadeFrom(before, beforeInfo, beforePos, true);
                }
            }
        }
        if (audio.finished()) {
            TRACE("track finished (repeat %d, shuffle %d)", settings.repeat, int(settings.shuffle));
            if (settings.repeat == 2)
                startCurrent(true); // repeat one song
            else if (playlist.next())
                startCurrent(true);
            else
                audio.stop();
        }

        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        const double t = now();
        settlePreset(t);
        updatePalette(); // follows the setting and track changes (no work when nothing changed)
        updateArt();
        lastFrameDt = float(t - last);
        if (gTrace && lastFrameDt > 0.05f)
            TRACE("slow frame: %.1f ms", double(lastFrameDt) * 1000.0);
        { // User themes are re-read whenever the menu opens (theme authors see their edits).
            static bool menuWasOpen = false;
            if (menu.isOpen() && !menuWasOpen && !themesDir.empty()) {
                const std::string current = ui::style(settings.uiStyle).name;
                std::string errors;
                for (const std::string &e : ui::loadUserStyles(themesDir))
                    errors += (errors.empty() ? "Theme file " : "\nTheme file ") + e;
                settings.uiStyle = std::max(0, ui::styleIndex(current));
                gUiStyle = settings.uiStyle;
                if (!errors.empty() && errors != themeErrors) { // (each new problem once)
                    SDL_Log("notice: %s", errors.c_str());
                    ui.notice = errors;
                    ui.noticeUntil = t + 10.0;
                }
                themeErrors = errors;
            }
            menuWasOpen = menu.isOpen();
        }
        if (gTrace) { // menu open/close and tab changes
            static int lastMenu = -1;
            if (int(menu.isOpen()) != lastMenu) {
                lastMenu = int(menu.isOpen());
                TRACE("menu %s", lastMenu ? "opened" : "closed");
            }
        }
        visAcc += t - last;
        last = t;
        // Stress tests: storms and stalls, and another visualizer update rate.
        {
            const StressTest &st = settings.stress;
            static double nextStorm = 0, nextSeek = 0;
            if (st.presetStorm && t >= nextStorm) {
                canyon.setPreset(canyon.step(canyon.preset(), 1));
                nextStorm = t + (st.presetStorm == 1 ? 2.0 : st.presetStorm == 2 ? 0.5 : 0.0);
            }
            if (st.seekStorm && t >= nextSeek && audio.isOpen() && audio.duration() > 10) {
                audio.seek(std::uniform_real_distribution<double>(0.0, audio.duration() - 8.0)(rng));
                nextSeek = t + (st.seekStorm == 1 ? 5.0 : 1.0);
            }
            if (st.hitches)
                SDL_Delay(Uint32(std::uniform_int_distribution<int>(0, st.hitches == 1 ? 30 : 100)(rng)));
        }
        // When paused (Misc -> When paused): glide to a stop (friction, ~0.35 s time constant; the same back
        // up on play), stop at once, or keep moving. visSpeed scales the visualizer's time step; at 0 the
        // picture holds (no updates or renders, so the feedback trails don't keep smearing).
        {
            static double speedAt = t;
            const bool pausedNow = audio.isOpen() && audio.paused();
            const float target = pausedNow && settings.pauseMode != 2 ? 0.0f : 1.0f;
            if (settings.pauseMode == 0) {
                visSpeed += (target - visSpeed) * float(1.0 - std::exp(-std::min(t - speedAt, 0.1) / 0.35));
                if (std::fabs(target - visSpeed) < 0.01f)
                    visSpeed = target;
            } else {
                visSpeed = target;
            }
            speedAt = t;
        }
        const double visStep = settings.stress.visRate == 1   ? 0.0
                               : settings.stress.visRate == 2 ? 1.0 / 120
                               : settings.stress.visRate == 3 ? 1.0 / 240
                                                              : kVisStep;
        if (w > 0 && h > 0 && visAcc >= visStep * 0.9) {
            const float dt = float(std::min(t - lastVis, 0.1)) * visSpeed;
            lastVis = t;
            visAcc = std::min(std::max(0.0, visAcc - visStep), visStep);
            // Look ahead like the PS3 does (per preset), plus the user's sync offset.
            const float aheadS = std::max(0.0f, canyon.prefetchSeconds() + settings.tweaks.syncMs / 1000.0f);
            audio.upcomingSamples(wideL.data(), wideR.data(), int(wideL.size()), int(aheadS * AudioEngine::kRate));
            analysisWindow(settings.eq, wideL, wideR, left.data(), right.data());
            audio.recentSamples(heardL.data(), heardR.data(), Spectrum::kSize); // what's playing now
            audioLevelNow = audioLevel(heardL.data(), heardR.data());
            if (static const bool dbg = SDL_getenv("PS3VIS_DEBUG_SPECTRUM"); dbg) {
                float peak = 0;
                for (float v : left)
                    peak = std::max(peak, std::fabs(v));
                static int n = 0;
                if (++n % 60 == 0)
                    SDL_Log("live: pos %.2f  sample peak %.4f", audio.position(), peak);
            }
            columnsFrom(spectrum, left.data(), right.data(), settings.gain, dt, audio.info().sampleRate,
                        settings.tweaks.spectrumMode, columns);
            if (settings.flow && audio.isOpen() && !audio.paused() && pendingPreset < 0) {
                flowClock += dt;
                if (flowClock >= flowNext) {
                    flowClock = 0;
                    flowNext = settings.flowSeconds * flowJitter(rng);
                    flowStep();
                }
            }
            // Test hooks for live runs:
            //   PS3VIS_LIVE_CYCLE=every,until  step presets every N visual frames until frame M
            //   PS3VIS_LIVE_QUIT=seconds       quit after that long
            static int liveFrame = 0;
            liveFrame++;
            if (const char *cy = SDL_getenv("PS3VIS_LIVE_CYCLE")) {
                const int every = std::max(1, std::atoi(cy));
                const char *comma = std::strchr(cy, ',');
                if (comma && liveFrame < std::atoi(comma + 1) && liveFrame % every == 0)
                    stepPreset(1); // like a held key / rapid clicks
            }
            //   PS3VIS_LIVE_SEEKCLICK=secs,fraction  click the progress bar there once (with info shown)
            if (const char *sc = SDL_getenv("PS3VIS_LIVE_SEEKCLICK")) {
                static int stage = 0;
                const double at = std::atof(sc);
                const char *comma = std::strchr(sc, ',');
                const float frac = comma ? float(std::atof(comma + 1)) : 0.5f;
                if (stage == 0 && t > at - 1.0) { // make sure the bar is visible
                    ui.infoUntil = t + 5.0;
                    stage = 1;
                } else if (stage == 1 && t > at && ui.bar[2] > 0) {
                    const float d = SDL_GetWindowPixelDensity(window), u = ui.bar[2] / 372.0f;
                    SDL_Event e{};
                    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                    e.button.windowID = SDL_GetWindowID(window);
                    e.button.button = SDL_BUTTON_LEFT;
                    e.button.down = true;
                    e.button.clicks = 1;
                    e.button.x = (ui.bar[0] + 6 * u + 360 * u * frac) / d;
                    e.button.y = (ui.bar[1] + ui.bar[3] * 0.5f) / d;
                    SDL_PushEvent(&e);
                    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
                    e.button.down = false;
                    SDL_PushEvent(&e);
                    SDL_Log("seek-click test: clicked %.0f%% at %.2f s (duration %.1f s)", frac * 100.0, audio.position(),
                            audio.duration());
                    stage = 2;
                } else if (stage == 2 && t > at + 1.0) {
                    SDL_Log("seek-click test: position now %.2f s", audio.position());
                    stage = 3;
                }
            }
            //   PS3VIS_LIVE_ARRANGE=secs: enter "Arrange on-screen elements" then (tests)
            if (const char *la = SDL_getenv("PS3VIS_LIVE_ARRANGE")) {
                static bool entered = false;
                if (!entered && t > std::atof(la)) {
                    entered = true;
                    menuCtx.arrange();
                }
            }
            //   PS3VIS_LIVE_MOUSE=secs:fx:fy[:d|u],...  move (or press/release at) that fraction of the window
            if (const char *lm = SDL_getenv("PS3VIS_LIVE_MOUSE")) {
                static size_t moved = 0;
                std::vector<std::array<double, 4>> plan; // time, x, y, 'm'ove / 'd'own / 'u'p
                std::stringstream ss(lm);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    double a = 0, b = 0, c = 0;
                    char act = 'm';
                    if (std::sscanf(item.c_str(), "%lf:%lf:%lf:%c", &a, &b, &c, &act) >= 3)
                        plan.push_back({a, b, c, double(act)});
                }
                while (moved < plan.size() && t > plan[moved][0]) {
                    SDL_Event e{};
                    const float d = SDL_GetWindowPixelDensity(window);
                    const char act = char(plan[moved][3]);
                    if (act == 'w') { // wheel up one notch at that point (moves the mouse there first)
                        SDL_Event mv{};
                        mv.type = SDL_EVENT_MOUSE_MOTION;
                        mv.motion.windowID = SDL_GetWindowID(window);
                        mv.motion.x = float(plan[moved][1] * w / d);
                        mv.motion.y = float(plan[moved][2] * h / d);
                        SDL_PushEvent(&mv);
                        e.type = SDL_EVENT_MOUSE_WHEEL;
                        e.wheel.windowID = SDL_GetWindowID(window);
                        e.wheel.y = 1.0f;
                    } else if (act == 'm') {
                        e.type = SDL_EVENT_MOUSE_MOTION;
                        e.motion.windowID = SDL_GetWindowID(window);
                        e.motion.x = float(plan[moved][1] * w / d);
                        e.motion.y = float(plan[moved][2] * h / d);
                    } else {
                        e.type = act == 'd' ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                        e.button.windowID = SDL_GetWindowID(window);
                        e.button.button = SDL_BUTTON_LEFT;
                        e.button.down = act == 'd';
                        e.button.clicks = 1;
                        e.button.x = float(plan[moved][1] * w / d);
                        e.button.y = float(plan[moved][2] * h / d);
                    }
                    SDL_PushEvent(&e);
                    moved++;
                }
            }
            //   PS3VIS_LIVE_KEYS=secs:key,secs:key,...  press those keys (letters, or SDL key names such as Return,
            //   Down, Tab) at those times (tests);
            //   plain PS3VIS_LIVE_KEYS=secs presses V, Space, Space, Escape, Escape then.
            if (const char *lk = SDL_getenv("PS3VIS_LIVE_KEYS")) {
                static size_t done = 0;
                std::vector<std::pair<double, SDL_Keycode>> plan;
                if (std::strchr(lk, ':')) {
                    std::stringstream ss(lk);
                    std::string item;
                    while (std::getline(ss, item, ','))
                        if (const size_t c = item.find(':'); c != std::string::npos && c + 1 < item.size())
                            plan.push_back({std::atof(item.c_str()), item.size() == c + 2
                                                                         ? SDL_Keycode(item[c + 1])
                                                                         : SDL_GetKeyFromName(item.c_str() + c + 1)});
                } else {
                    for (SDL_Keycode k : {SDLK_V, SDLK_SPACE, SDLK_SPACE, SDLK_ESCAPE, SDLK_ESCAPE})
                        plan.push_back({std::atof(lk), k});
                }
                while (done < plan.size() && t > plan[done].first) {
                    SDL_Event e{};
                    e.type = SDL_EVENT_KEY_DOWN;
                    e.key.windowID = SDL_GetWindowID(window);
                    e.key.key = plan[done].second;
                    e.key.down = true;
                    SDL_PushEvent(&e);
                    done++;
                }
            }
            //   PS3VIS_LIVE_PAD=secs:button,...  press virtual controller buttons (ids as in settings, e.g. south)
            if (const char *lp = SDL_getenv("PS3VIS_LIVE_PAD")) {
                static std::vector<std::pair<double, PadButton>> plan;
                static size_t next = 0;
                static std::vector<std::pair<double, PadButton>> release;
                if (plan.empty()) {
                    std::stringstream list(lp);
                    for (std::string item; std::getline(list, item, ',');)
                        if (const size_t c = item.find(':'); c != std::string::npos)
                            plan.push_back({std::atof(item.c_str()), padButtonFromId(item.substr(c + 1))});
                }
                while (next < plan.size() && t > plan[next].first) {
                    gamepads.pressVirtual(plan[next].second, true);
                    release.push_back({t + 0.12, plan[next].second});
                    next++;
                }
                for (auto it = release.begin(); it != release.end();)
                    if (t > it->first) {
                        gamepads.pressVirtual(it->second, false);
                        it = release.erase(it);
                    } else {
                        ++it;
                    }
            }
            if (const char *q = SDL_getenv("PS3VIS_LIVE_QUIT"); q && t > std::atof(q))
                running = false;
            //   PS3VIS_LIVE_DUMP=seconds       trigger a debug dump once, at that time
            if (const char *dd = SDL_getenv("PS3VIS_LIVE_DUMP")) {
                static bool done = false;
                if (!done && t > std::atof(dd)) {
                    done = true;
                    runAction(Action::DebugDump);
                }
            }
            // (The canyon keeps analysing the music under the Earth too: the debug spectrum panel shows it.)
            if (visSpeed > 0.0f)
                canyon.update(dt, columns);
            if (earthOn()) {
                earth.update(dt, audioLevelNow, columns);
                if (earth.presets()[size_t(earth.preset())] != settings.earthView) // (moved on after a flight)
                    settings.earthView = earth.presets()[size_t(earth.preset())];
            }
            history.push_back({t, audio.position(), dt, lastFrameDt, canyon.preset(), audio.paused(), canyon.stats()});
            if (history.size() > 900)
                history.pop_front();
            // (Stress tests: extra passes and another render resolution.)
            const int rw = std::max(1, int(float(w) * settings.stress.renderScale));
            const int rh = std::max(1, int(float(h) * settings.stress.renderScale));
            for (int pass = 0; visSpeed > 0.0f && pass < std::clamp(settings.stress.renderPasses, 1, 8); pass++) {
                if (earthOn())
                    earth.render(rw, rh);
                else
                    canyon.render(rw, rh);
            }
        }
        // Vulkan/HDR mode: render into the presenter's float scene image instead of the window.
        GLuint frameFbo = 0;
        if (presenter) {
            frameFbo = presenter->beginFrame(w, h);
            canyon.hdrOutput = earth.hdrOutput = presenter->output() != VkPresenter::Output::Sdr;
        }
        const float ditherAmount = dither::amountFor(settings.dither);
        if (earthOn())
            earth.present(w, h, frameFbo, ditherAmount);
        else
            canyon.present(w, h, frameFbo, ditherAmount);

        // Overlay
        // The pointer hides after 5 s without mouse movement (any mouse event shows it again).
        if (!menu.isOpen() && !ui.arranging && t - ui.lastMouse > 5.0)
            SDL_HideCursor();
        // UI colours from the visuals: the cover's colours if the track has usable art, else the
        // scene's fog/line colours; glides so preset changes and morphs fade smoothly.
        if (settings.uiFromVisuals) {
            const std::string want = audio.isOpen() ? utf8(playlist.current()) : std::string();
            if (want != themeArtFor) {
                themeArtFor = want;
                themeArtOk = !want.empty() && albumPalette(playlist.current(), themeArt);
            }
            float mainHue, mainSat, accHue, accSat;
            if (themeArtOk) {
                mainHue = themeArt.hue;
                accHue = themeArt.accent;
                mainSat = accSat = themeArt.sat;
            } else {
                float fog[3], line[3], v;
                if (earthOn())
                    earth.sceneColours(fog, line);
                else
                    canyon.sceneColours(fog, line);
                rgbHsv(fog, mainHue, mainSat, v);
                rgbHsv(line, accHue, accSat, v);
                if (mainSat < 0.12f && accSat >= 0.12f) // grey fog: the line carries the colour
                    mainHue = accHue, mainSat = accSat;
                if (accSat < 0.12f && mainSat >= 0.12f)
                    accHue = mainHue, accSat = mainSat;
            }
            const UiTheme target = themeFromHues(mainHue, mainSat, accHue, accSat, settings.uiOpacity);
            const float k = 1.0f - std::exp(-float(std::min(t - themeTime, 0.1)) / 0.5f);
            float *cur[4] = {gTheme.accent, gTheme.text, gTheme.bg, gTheme.highlight};
            const float *to[4] = {target.accent, target.text, target.bg, target.highlight};
            // At most a small step per frame, so a one-frame spike in the source can't flash the UI.
            // Extensive logging notes large target jumps (to trace where one came from).
            for (int n = 0; n < 4; n++)
                for (int c = 0; c < 3; c++) {
                    if (gTrace && std::fabs(to[n][c] - lastThemeTarget[n][c]) > 0.2f && themeTime > 0)
                        TRACE("ui theme: colour %d target jumped %.2f -> %.2f (art %d)", n, double(lastThemeTarget[n][c]),
                              double(to[n][c]), int(themeArtOk));
                    lastThemeTarget[n][c] = to[n][c];
                    cur[n][c] += std::clamp((to[n][c] - cur[n][c]) * k, -0.03f, 0.03f);
                }
            gTheme.opacity = settings.uiOpacity;
        }
        themeTime = t;
        menu.setTheme(gTheme);
        syncThemeFont(overlay, themesDir, overlayFont);
        overlay.begin(w, h);
        const float unit = uiUnit(h);
        const float margin = 48 * unit;
        if (ui.arranging) { // every element fully visible (sample text if nothing plays), outlined
            TrackInfo sample;
            sample.title = "Track title";
            sample.artist = "Artist";
            sample.album = "Album";
            sample.duration = 200;
            const bool playing = audio.isOpen();
            drawTrackInfo(overlay, w, h, playing ? audio.info() : sample, playing ? audio.position() : 60.0, false,
                          playlist.index(), int(playlist.size()), 1.0f, -1, -1, nullptr,
                          settings.albumArtMode != 0 ? artTex : 0, artAspect);
            ui.buttons.clear();
            drawControls(overlay, w, h, presetLabel(), true, audio.paused(), settings.shuffle,
                         settings.repeat, -1, -1, 1.0f, 1.0f, ui.buttons);
            ui.buttons.clear();
            const std::array<float, 4> rects[3] = {
                gInfoRect, transportRect(w, h), presetBarRect(overlay, w, h, presetLabel(), true)};
            const float sizes[3] = {settings.infoScale, settings.controlsScale, settings.presetBarScale};
            for (int e = 0; e < 3; e++) {
                const auto &r = rects[e];
                const float a = ui.dragging == e ? 0.95f : 0.55f, b = 2 * unit;
                char label[32];
                std::snprintf(label, sizeof label, "%.2fx", double(sizes[e]));
                const float ls = 14 * unit, ly = r[1] > 22 * unit ? r[1] - 8 * unit : r[1] + r[3] + 18 * unit;
                overlay.text(label, r[0], ly, ls, 0.55f, 0.78f, 1.0f, 0.95f);
                overlay.rect(r[0] - b, r[1] - b, r[2] + 2 * b, b, 0.55f, 0.78f, 1.0f, a);
                overlay.rect(r[0] - b, r[1] + r[3], r[2] + 2 * b, b, 0.55f, 0.78f, 1.0f, a);
                overlay.rect(r[0] - b, r[1], b, r[3], 0.55f, 0.78f, 1.0f, a);
                overlay.rect(r[0] + r[2], r[1], b, r[3], 0.55f, 0.78f, 1.0f, a);
            }
            const std::string hint =
                "Drag: move (snaps; Shift: free)    Wheel: resize (Shift: fine)    R: reset    Enter / Esc: done";
            overlay.rect(0, float(h) * 0.5f - 26 * unit, float(w), 40 * unit, 0, 0, 0, 0.45f);
            overlay.text(hint, (float(w) - overlay.measure(hint, 20 * unit)) * 0.5f, float(h) * 0.5f, 20 * unit, 1, 1,
                         1, 0.95f);
        } else if (menu.isOpen()) {
            // the menu draws over everything below
        } else if (playlist.empty() || !audio.isOpen()) {
            const std::string msg =
                haveAudio ? "Drop music files or folders here, or press Esc" : "No audio device available";
            const float size = 30 * unit;
            overlay.text(msg, (float(w) - overlay.measure(msg, size)) * 0.5f, float(h) * 0.5f, size, 1, 1, 1, 0.85f);
        } else {
            const float alpha = settings.infoAlways ? 1.0f : float(std::clamp((ui.infoUntil - t) / 0.8, 0.0, 1.0));
            std::fill(ui.bar, ui.bar + 4, 0.0f);
            // Album art: "always" ignores the info's auto-hide, "auto" fades with it.
            const bool art = settings.albumArtMode != 0 && artTex;
            const float artVis = settings.albumArtMode == 2 ? 1.0f : alpha;
            // During a crossfade the old track's info/cover fade out as the new one's fade in.
            // ("Crossfade track info" off: the new track's info just replaces the old one.)
            const bool fadeInfo = prevShown.active && settings.infoCrossfade;
            const float in = fadeInfo ? audio.crossfadeProgress() : 1.0f;
            if (fadeInfo) {
                const bool oldArt = settings.albumArtMode != 0 && prevShown.art;
                if (alpha > 0 || (oldArt && artVis > 0))
                    drawTrackInfo(overlay, w, h, prevShown.info, prevShown.pos + (t - prevShown.since), false,
                                  prevShown.index, int(playlist.size()), alpha * (1.0f - in), -1, -1, nullptr,
                                  oldArt ? prevShown.art : 0, prevShown.aspect, (1.0f - in) * artVis);
            }
            if (alpha > 0 || (art && artVis > 0))
                drawTrackInfo(overlay, w, h, audio.info(), ui.seeking ? ui.seekPreview : audio.position(),
                              audio.paused(), playlist.index(), int(playlist.size()), alpha * in, ui.mouseX, ui.mouseY,
                              ui.bar, art ? artTex : 0, artAspect, in * artVis);
        }
        if (settings.debugSpectrum && !menu.isOpen())
            drawSpectrum(overlay, w, h, canyon, audioLevelNow, settings.eq, earthOn() ? &earth : nullptr);
        if (t < ui.osdUntil) {
            const float a = float(std::clamp((ui.osdUntil - t) / 0.5, 0.0, 1.0));
            const float size = 22 * unit;
            const Rgba ink = ui::colourOn(ui::style(gUiStyle), ui::Part::TrackInfo, gTheme, ui::Colour{ui::Role::Text}, a);
            overlay.text(ui.osd, float(w) - margin - overlay.measure(ui.osd, size), margin, size, ink.r, ink.g, ink.b, ink.a);
        }
        if (!earthError.empty()) {
            showOsd(earthError);
            earthError.clear();
        }
        stats.frame(t);
        if (settings.perfOverlay > 0)
            stats.window(t, settings.perfWindow);
        if (settings.resourceOverlay > 0)
            stats.sample(t);
        // Hardware info panel (Debug tab): what's in use, refreshed every 2 s.
        static std::vector<std::string> hardwareLines;
        static double hardwareAt = -10;
        if (settings.hardwarePanel && t - hardwareAt > 2.0) {
            hardwareAt = t;
            hardwareLines.clear();
            char b[256];
            const std::string cpu = cpuName();
            std::snprintf(b, sizeof b, "CPU  %s (%d threads)", cpu.empty() ? "?" : cpu.c_str(), logicalCpus());
            hardwareLines.push_back(b);
            const uint64_t avail = memoryAvailableBytes();
            std::snprintf(b, sizeof b, "RAM  %.1f GB%s", double(SDL_GetSystemRAM()) / 1024.0,
                          avail ? (" (" + formatBytes(double(avail)) + " free)").c_str() : "");
            hardwareLines.push_back(b);
            std::snprintf(b, sizeof b, "GPU  %s", reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
            hardwareLines.push_back(b);
            std::snprintf(b, sizeof b, "Output  OpenGL %.12s%s", reinterpret_cast<const char *>(glGetString(GL_VERSION)),
                          presenter ? (std::string(" -> Vulkan, ") + presenter->describe()).c_str() : "");
            hardwareLines.push_back(b);
            const SDL_DisplayID did = SDL_GetDisplayForWindow(window);
            const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(did);
            const char *dn = SDL_GetDisplayName(did);
            std::snprintf(b, sizeof b, "Display  %s, %dx%d @ %.0f Hz", dn ? withoutSerial(dn).c_str() : "?", dm ? dm->w : 0,
                          dm ? dm->h : 0, dm ? double(dm->refresh_rate) : 0.0);
            hardwareLines.push_back(b);
            SDL_AudioSpec spec{};
            int frames = 0;
            const bool haveSpec = SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &frames);
            std::snprintf(b, sizeof b, "Audio  %s, %d Hz, %d ch", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
                          haveSpec ? spec.freq : 0, haveSpec ? spec.channels : 0);
            hardwareLines.push_back(b);
            std::snprintf(b, sizeof b, "Window system  %s", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
            hardwareLines.push_back(b);
            hardwareLines.push_back("Controller  " + gamepads.describe());
        }
        if (settings.perfOverlay > 0 || settings.resourceOverlay > 0 || settings.hardwarePanel) {
            const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
            drawStats(overlay, w, h, unit, settings.perfOverlay, settings.resourceOverlay, stats,
                      mode ? mode->refresh_rate : 0.0f, settings.perfWindow, settings.hardwarePanel ? &hardwareLines : nullptr);
        }
        // A reminder while a stress test runs (the frame rate limit alone doesn't count, but is listed
        // along with the others).
        if (settings.stress.heavy()) {
            const std::string text = "STRESS TEST: " + describeStress(settings.stress);
            overlay.rect(margin * 0.5f - 6 * unit, margin * 0.25f, overlay.measure(text, 16 * unit) + 12 * unit, 24 * unit,
                         0.6f, 0.05f, 0.05f, 0.75f);
            overlay.text(text, margin * 0.5f, margin * 0.25f + 18 * unit, 16 * unit, 1, 0.85f, 0.85f, 1);
        }
        // On-screen controls while the mouse is in use: preset bar at the top,
        // transport bar bottom-right.
        ui.buttons.clear();
        {
            // Each control group shows while the mouse is near its edge (and has moved in the
            // last 3 s), fading in quickly and out a little slower.
            const bool active = !menu.isOpen() && settings.onScreenControls && ui.mouseY >= 0 && t - ui.lastMouse < 3.0;
            const float reach = 70 * uiUnit(h); // how close counts as "near"
            // Near = inside the element's area grown by `reach`, and towards the window edge it sits on.
            auto near = [&](std::array<float, 4> r) {
                float x0 = r[0] - reach, y0 = r[1] - reach, x1 = r[0] + r[2] + reach, y1 = r[1] + r[3] + reach;
                if (r[1] + r[3] > float(h) * 0.75f) y1 = float(h); // bottom edge: all the way down
                if (r[1] < float(h) * 0.25f) y0 = 0;                // top edge
                if (r[0] + r[2] > float(w) * 0.85f) x1 = float(w);  // right edge
                if (r[0] < float(w) * 0.15f) x0 = 0;                // left edge
                return ui.mouseX >= x0 && ui.mouseX <= x1 && ui.mouseY >= y0 && ui.mouseY <= y1;
            };
            const bool nearPreset =
                active && near(presetBarRect(overlay, w, h, presetLabel(), audio.isOpen()));
            const bool nearTransport = active && audio.isOpen() && near(transportRect(w, h));
            auto ease = [&](float &a, bool on) {
                a += ((on ? 1.0f : 0.0f) - a) * (1.0f - std::exp(-std::max(lastFrameDt, 0.0f) / (on ? 0.12f : 0.3f)));
            };
            if (SDL_getenv("PS3VIS_SHOT_CONTROLS")) { // (testing: show the on-screen controls at full opacity)
                ui.presetAlpha = 1;
                ui.transportAlpha = 1;
            } else {
                ease(ui.presetAlpha, nearPreset);
                ease(ui.transportAlpha, nearTransport);
            }
        }
        if (!ui.arranging && (ui.presetAlpha > 0.01f || ui.transportAlpha > 0.01f))
            drawControls(overlay, w, h, presetLabel(), audio.isOpen(), audio.paused(),
                         settings.shuffle, settings.repeat, ui.mouseX, ui.mouseY, ui.presetAlpha, ui.transportAlpha,
                         ui.buttons);
        if (ui.help)
            drawHelp(overlay, w, h, unit, settings, gamepads.active(), padStyleNow());
        menu.draw(overlay, w, h, menuCtx);
        if (!ui.notice.empty() && t < ui.noticeUntil) // (on top: some come up while the menu is open)
            drawNotice(overlay, w, h, unit, ui.notice, float(std::clamp((ui.noticeUntil - t) / 0.6, 0.0, 1.0)));
        overlay.end();
        if (dumpRequested) {
            dumpRequested = false;
            // Folder next to settings.ini: dumps/dump-YYYYMMDD-HHMMSS/
            char stamp[32];
            const std::time_t tt = std::time(nullptr);
            std::strftime(stamp, sizeof stamp, "dump-%Y%m%d-%H%M%S", std::localtime(&tt));
            const fs::path dir = (settingsFile.empty() ? fs::current_path() : settingsFile.parent_path()) / "dumps" / stamp;
            std::error_code ec;
            fs::create_directories(dir, ec);

            std::string st;
            char buf[1024];
            auto line = [&](const char *fmt, auto... args) {
                std::snprintf(buf, sizeof buf, fmt, args...);
                st += buf;
                st += '\n';
            };
            line("PS3 Visualizer debug dump, %s", stamp);
            line("[app]");
            {
                // How it was built (the build script, see CMakeLists) and how it's running now.
                const char *running = SDL_getenv("APPIMAGE")                ? "AppImage"
                                      : SDL_getenv("FLATPAK_ID") || fs::exists("/.flatpak-info") ? "Flatpak"
                                                                                                     : "plain executable";
                line("build = %s, %s, built %s %s; running as %s", PS3VIS_BUILD_TARGET, PS3VIS_BUILD_TYPE, __DATE__,
                     __TIME__, running);
            }
            line("GL = %s / %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
            line("firmware = %s", utf8(*devFlash).c_str());
            line("window = %dx%d px, density %.2f, fullscreen %d, menu open %d", w, h,
                 double(SDL_GetWindowPixelDensity(window)), int((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0),
                 int(menu.isOpen()));
            line("SDL %d.%d.%d, video driver %s, platform %s", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
                 SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
                 SDL_GetCurrentVideoDriver(), SDL_GetPlatform());
            line("[hdr]");
            line("setting = %d (0 auto, 1 on, 2 off), this run = %d%s, boost %.2f", settings.hdr, hdrMode,
                 SDL_getenv("PS3VIS_HDR") ? " (PS3VIS_HDR)" : "", double(settings.hdrBoost));
            line("path = %s, window flags 0x%llx", presenter ? "Vulkan presenter" : "OpenGL window",
                 (unsigned long long)SDL_GetWindowFlags(window));
            if (presenter)
                line("output = %s, SDR white %.3f (scRGB), headroom %.3f, glow excess %s", presenter->describe(),
                     double(presenter->sdrWhite()), double(presenter->headroom()), canyon.hdrOutput ? "on" : "off");
            {
                const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
                line("window HDR %d, SDR white %.3f, headroom %.3f",
                     int(SDL_GetBooleanProperty(wp, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false)),
                     double(SDL_GetFloatProperty(wp, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 0)),
                     double(SDL_GetFloatProperty(wp, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 0)));
                int nd = 0;
                if (SDL_DisplayID *ds = SDL_GetDisplays(&nd)) {
                    for (int i = 0; i < nd; i++) {
                        const char *raw = SDL_GetDisplayName(ds[i]);
                        const std::string shown = !raw ? "?" : settings.dumpRedact ? withoutSerial(raw) : raw;
                        const char *name = shown.c_str();
                        line("display %d \"%s\": HDR %d%s", int(ds[i]), name,
                             int(SDL_GetBooleanProperty(SDL_GetDisplayProperties(ds[i]), SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN,
                                                        false)),
                             ds[i] == SDL_GetDisplayForWindow(window) ? " (this window)" : "");
                    }
                    SDL_free(ds);
                }
            }
            if (!history.empty()) {
                double sum = 0, worst = 0;
                for (auto &hf : history) {
                    sum += hf.frameDt;
                    worst = std::max(worst, double(hf.frameDt));
                }
                line("frame time over last %zu visual frames: mean %.2f ms, worst %.2f ms", history.size(),
                     sum / double(history.size()) * 1000.0, worst * 1000.0);
            }
            line("uptime = %.1f s", t);
            line("[audio]");
            line("device ok %d, open %d, paused %d, finished %d, position %.2f / %.2f s, volume %.2f", int(haveAudio),
                 int(audio.isOpen()), int(audio.paused()), int(audio.finished()), audio.position(), audio.duration(),
                 double(audio.volume()));
            if (audio.isOpen())
                line("track = %s | %s | %s", audio.info().title.c_str(), audio.info().artist.c_str(),
                     audio.info().album.c_str());
            line("[playlist]");
            line("index %d of %zu, shuffle %d, repeat %d", playlist.index(), playlist.size(), int(playlist.shuffle),
                 int(playlist.repeat));
            for (int i = std::max(0, playlist.index() - 3); i < std::min(int(playlist.size()), playlist.index() + 4); i++)
                line("%s %d: %s", i == playlist.index() ? ">" : " ", i + 1, utf8(playlist.tracks()[size_t(i)]).c_str());
            canyon.dumpState(st);
            if (earth.ready())
                earth.dumpState(st);
            if (settings.dumpHardware)
                st += "[hardware]\n" + hardwareInfo(reinterpret_cast<const char *>(glGetString(0x1F00 /* GL_VENDOR */)),
                                                    reinterpret_cast<const char *>(glGetString(GL_RENDERER)),
                                                    settings.dumpRedact);
            if (settings.dumpSoftware)
                st += "[software]\n" + softwareInfo(reinterpret_cast<const char *>(glGetString(GL_VERSION)),
                                                    reinterpret_cast<const char *>(glGetString(0x8B8C /* GL_SHADING_LANGUAGE_VERSION */)));
            // Personal details (user/host names, addresses) are removed unless turned off.
            auto clean = [&](const std::string &text) { return settings.dumpRedact ? redactPersonal(text) : text; };
            auto cleanFile = [&](const fs::path &f) {
                std::ifstream in(f);
                std::stringstream text;
                text << in.rdbuf();
                in.close();
                std::ofstream(f) << clean(text.str());
            };
            if (settings.stress.active())
                st += "[stress]\nactive = " + describeStress(settings.stress) + "\n";
            st += std::string("[dump]\nredacted = ") + (settings.dumpRedact ? "yes" : "no") + "\n";
            std::ofstream(dir / "state.txt") << clean(st);

            std::ofstream csv(dir / "history.csv");
            csv << "t,audio_pos,vis_dt,frame_dt,preset,paused,morph,scroll,row_base,ring,peak,peak_env,mean_norm,"
                   "max_norm,lift,want_lift,catch_up,feedback_frames\n";
            for (auto &hf : history) {
                std::snprintf(buf, sizeof buf,
                              "%.4f,%.3f,%.5f,%.5f,%s,%d,%.3f,%.3f,%.3f,%d,%.5g,%.5g,%.4f,%.4f,%.3f,%.3f,%.2f,%d\n",
                              hf.t, hf.audioPos, double(hf.visDt), double(hf.frameDt),
                              canyon.presets()[size_t(hf.preset)].c_str(), int(hf.paused), double(hf.st.morph),
                              double(hf.st.scroll), double(hf.st.rowBase), hf.st.ring, double(hf.st.peak),
                              double(hf.st.peakEnv), double(hf.st.meanNorm), double(hf.st.maxNorm), double(hf.st.lift),
                              double(hf.st.wantLift), double(hf.st.catchUp), hf.st.feedbackFrames);
                csv << buf;
            }
            {
                std::lock_guard lock(gLog.mtx);
                std::string logText = "--- start-up ---\n";
                for (auto &l : gLog.startup)
                    logText += l + "\n";
                logText += "--- most recent ---\n";
                for (auto &l : gLog.lines)
                    logText += l + "\n";
                std::ofstream(dir / "log.txt") << clean(logText);
            }
            settings.save(dir / "settings-in-memory.ini");
            cleanFile(dir / "settings-in-memory.ini");
            if (!settingsFile.empty()) {
                fs::copy_file(settingsFile, dir / "settings-file.ini", fs::copy_options::overwrite_existing, ec);
                cleanFile(dir / "settings-file.ini");
            }

            std::vector<uint8_t> px(size_t(w) * h * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, frameFbo);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (size_t i = 3; i < px.size(); i += 4)
                px[i] = 255;
            stbi_flip_vertically_on_write(1);
            stbi_write_png(utf8(dir / "frame.png").c_str(), w, h, 4, px.data(), w * 4);
            SDL_Log("debug dump written to %s", utf8(dir).c_str());
            showOsd(std::string("Debug dump saved: ") + stamp);
        }

        // PS3VIS_LIVE_SHOT=path%d,every_seconds saves the window periodically (test hook).
        if (const char *shot = SDL_getenv("PS3VIS_LIVE_SHOT")) {
            static double nextShot = 0;
            static int shotIndex = 0;
            const char *comma = std::strrchr(shot, ',');
            const double every = comma ? std::atof(comma + 1) : 5.0;
            if (nextShot == 0)
                nextShot = t + every;
            if (t >= nextShot) {
                nextShot += every;
                std::vector<uint8_t> px(size_t(w) * h * 4);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, frameFbo);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                for (size_t i = 3; i < px.size(); i += 4)
                    px[i] = 255;
                char name[1024];
                const std::string pattern(shot, comma ? size_t(comma - shot) : std::strlen(shot));
                std::snprintf(name, sizeof name, pattern.c_str(), ++shotIndex);
                stbi_flip_vertically_on_write(1);
                stbi_write_png(name, w, h, 4, px.data(), w * 4);
                SDL_Log("live shot %s (preset %s)", name, canyon.presets()[size_t(canyon.preset())].c_str());
            }
        }
        if (presenter)
            presenter->endFrame();
        else
            SDL_GL_SwapWindow(window);
        // Stress test: capped frame rate (vsync is off then).
        if (settings.stress.fps >= 2) {
            static const double caps[] = {0, 0, 30, 60, 120, 240};
            static double frameEnd = 0;
            const double target = 1.0 / caps[std::clamp(settings.stress.fps, 2, 5)];
            frameEnd = std::max(frameEnd + target, now());
            const double wait = frameEnd - now();
            if (wait > 0)
                SDL_DelayPrecise(Uint64(wait * 1e9));
        }
    }

    saveSettings();
    audio.shutdown();
    overlay.shutdown();
    canyon.shutdown();
    dither::shutdown();
    if (presenter)
        presenter->shutdown();
    SDL_GL_DestroyContext(context);
    if (glWindow)
        SDL_DestroyWindow(glWindow);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
