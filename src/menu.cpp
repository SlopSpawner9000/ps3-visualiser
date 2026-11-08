#include "menu.h"
#include "uistyle.h"
#include "glyphs.h"
#include "presetlib.h"

#include "audio.h"
#include "overlay.h"
#include "playlist.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <ctime>
#include <set>
#include <sstream>

namespace {

// Playlist tab rows before the tracks themselves.
enum PlaylistAction { kAddFiles, kAddFolder, kClear, kActionCount };
const char *kActionLabels[kActionCount] = {"Add files...", "Add folder...", "Clear playlist"};

std::string fmt(float v, const char *suffix = "")
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f%s", double(v), suffix);
    return buf;
}

void step(float &v, int dir, float by, float lo, float hi)
{
    v = std::clamp(std::round((v + by * float(dir)) / by) * by, lo, hi);
}

} // namespace

// ---------------------------------------------------------------- keybinds

namespace {
struct ActionInfo {
    const char *id, *label;
    KeyBind a, b;
};
constexpr int S = Keybinds::kShift;
const ActionInfo kActions[Keybinds::kCount] = {
    {"play_pause", "Play / pause", {SDLK_SPACE, 0}, {SDLK_K, 0}},
    {"seek_forward", "Seek forward 10 s", {SDLK_RIGHT, 0}, {SDLK_L, 0}},
    {"seek_back", "Seek back 10 s", {SDLK_LEFT, 0}, {SDLK_J, 0}},
    {"seek_forward_long", "Seek forward 60 s", {SDLK_RIGHT, S}, {SDLK_L, S}},
    {"seek_back_long", "Seek back 60 s", {SDLK_LEFT, S}, {SDLK_J, S}},
    {"volume_up", "Volume up", {SDLK_UP, 0}, {}},
    {"volume_down", "Volume down", {SDLK_DOWN, 0}, {}},
    {"next_track", "Next track", {SDLK_N, 0}, {SDLK_PAGEUP, 0}},
    {"prev_track", "Previous track", {SDLK_P, 0}, {SDLK_PAGEDOWN, 0}},
    {"next_preset", "Next preset", {SDLK_V, 0}, {SDLK_RIGHTBRACKET, 0}},
    {"prev_preset", "Previous preset", {SDLK_V, S}, {SDLK_LEFTBRACKET, 0}},
    {"fullscreen", "Fullscreen", {SDLK_F, 0}, {SDLK_F11, 0}},
    {"track_info", "Keep track info visible", {SDLK_I, 0}, {}},
    {"album_art", "Album art (off / auto / always)", {SDLK_I, S}, {}},
    {"shuffle", "Shuffle", {SDLK_S, 0}, {}},
    {"repeat", "Repeat (off / playlist / one song)", {SDLK_R, 0}, {}},
    {"help", "Help", {SDLK_H, 0}, {SDLK_SLASH, 0}},
    {"menu", "Menu (Esc always works too)", {SDLK_ESCAPE, 0}, {SDLK_M, 0}},
    {"quit", "Quit", {SDLK_Q, 0}, {}},
    {"debug_dump", "Debug dump (state for bug reports)", {SDLK_F12, 0}, {}},
};
} // namespace

void Keybinds::reset()
{
    // Controller defaults, in Action order.
    static const PadButton pads[kCount] = {
        PadButton::South,        PadButton::DpadRight,   PadButton::DpadLeft,  PadButton::None,         PadButton::None,
        PadButton::DpadUp,       PadButton::DpadDown,    PadButton::RightShoulder, PadButton::LeftShoulder,
        PadButton::RightTrigger, PadButton::LeftTrigger, PadButton::LeftStick, PadButton::North,        PadButton::None,
        PadButton::West,         PadButton::RightStick,  PadButton::Back,      PadButton::Start,        PadButton::None,
        PadButton::None};
    for (int i = 0; i < kCount; i++) {
        slot[i][0] = kActions[i].a;
        slot[i][1] = kActions[i].b;
        pad[i] = pads[i];
    }
}

Action Keybinds::padMatch(PadButton b) const
{
    for (int i = 0; i < kCount; i++)
        if (b != PadButton::None && pad[i] == b)
            return Action(i);
    return Action::Count;
}

int Keybinds::modsOf(SDL_Keymod m)
{
    return ((m & SDL_KMOD_SHIFT) ? kShift : 0) | ((m & SDL_KMOD_CTRL) ? kCtrl : 0) | ((m & SDL_KMOD_ALT) ? kAlt : 0);
}

Action Keybinds::match(SDL_Keycode key, SDL_Keymod mod) const
{
    const int m = modsOf(mod);
    for (int i = 0; i < kCount; i++)
        for (auto &b : slot[i])
            if (b.key && b.key == key && b.mods == m)
                return Action(i);
    // Shift + a volume key: the same action in 1% steps (main checks Shift).
    if (m & kShift)
        for (Action a : {Action::VolumeUp, Action::VolumeDown})
            for (auto &b : slot[int(a)])
                if (b.key && b.key == key && b.mods == (m & ~kShift))
                    return a;
    return Action::Count;
}

const char *Keybinds::label(Action a) { return int(a) >= 0 && int(a) < kCount ? kActions[int(a)].label : "-"; }
const char *Keybinds::id(Action a) { return int(a) >= 0 && int(a) < kCount ? kActions[int(a)].id : "none"; }

std::string Keybinds::describe(const KeyBind &b)
{
    if (!b.key)
        return "-";
    std::string s;
    if (b.mods & kCtrl) s += "Ctrl+";
    if (b.mods & kAlt) s += "Alt+";
    if (b.mods & kShift) s += "Shift+";
    return s + SDL_GetKeyName(b.key);
}

// ---------------------------------------------------------------- settings

// Named choices of each UI colour; the last one is "Custom" (hue/saturation/brightness).
namespace {
struct ColourChoice {
    const char *name;
    float rgb[3];
};
struct ColourKind {
    const char *what, *key; // menu label start, settings key
    std::vector<ColourChoice> choices;
};
// rgb < 0 = derived: -1 the accent, -2 the text colour, -3 a tint of the accent, -4 the UI theme's own colour.
// (A function-local static: uiTheme() runs during static initialisation of other files, e.g. main's
// gTheme, when a namespace-scope table might not be constructed yet.)
const ColourKind *colourKinds()
{
    static const ColourKind kinds[4] = {
        {"Accent", "accent",
         {{"Theme", {-4, 0, 0}}, {"Blue", {0.55f, 0.78f, 1.0f}}, {"Purple", {0.74f, 0.62f, 1.0f}}, {"Pink", {1.0f, 0.56f, 0.82f}},
          {"Red", {1.0f, 0.47f, 0.47f}}, {"Orange", {1.0f, 0.66f, 0.36f}}, {"Gold", {1.0f, 0.86f, 0.42f}},
          {"Green", {0.52f, 0.92f, 0.58f}}, {"Teal", {0.42f, 0.9f, 0.86f}}, {"Custom", {}}}},
        {"Text", "text",
         {{"Theme", {-4, 0, 0}}, {"White", {1, 1, 1}}, {"Warm white", {1.0f, 0.95f, 0.86f}}, {"Cool white", {0.88f, 0.94f, 1.0f}},
          {"Accent", {-1, 0, 0}}, {"Custom", {}}}},
        {"Background", "bg",
         {{"Theme", {-4, 0, 0}}, {"Midnight", {0.025f, 0.03f, 0.055f}}, {"Black", {0.008f, 0.008f, 0.01f}}, {"Graphite", {0.07f, 0.07f, 0.075f}},
          {"Accent tint", {-3, 0, 0}}, {"Custom", {}}}},
        {"Highlight", "highlight",
         {{"Theme", {-4, 0, 0}}, {"White", {1, 1, 1}}, {"Accent", {-1, 0, 0}}, {"Text colour", {-2, 0, 0}}, {"Custom", {}}}},
    };
    return kinds;
}
UiColour &colourOf(Settings &s, int kind)
{
    return kind == 0 ? s.uiAccent : kind == 1 ? s.uiText : kind == 2 ? s.uiBackground : s.uiHighlight;
}
} // namespace

bool loadUiColour(const std::string &k, float f, Settings &s);
std::string saveUiColours(const Settings &s);

void Settings::load(const fs::path &file)
{
    std::ifstream in(file);
    if (in)
        configVersion = 1; // files without config_version are version 1
    std::string line;
    while (std::getline(in, line)) {
        const auto sep = line.find('=');
        if (sep == std::string::npos)
            continue;
        const std::string k = line.substr(0, sep), v = line.substr(sep + 1);
        const float f = std::strtof(v.c_str(), nullptr);
        if (k == "ui_theme") { // by name (a number in files from before user themes: a built-in one)
            const bool number = !v.empty() && v.find_first_not_of("0123456789") == std::string::npos;
            uiStyle = number ? std::min(std::atoi(v.c_str()), int(ui::builtinStyles().size()) - 1)
                             : std::max(0, ui::styleIndex(v));
            continue;
        }
        if (!std::isfinite(f)) // nan / inf (a damaged file): keep the default
            continue;
        if (k == "preset") preset = v;
        else if (k == "random_presets") randomPresets = v == "1";
        else if (k == "gain") gain = std::clamp(f, 0.25f, 12.0f);
        else if (k == "volume") volume = std::clamp(f, 0.0f, 1.5f);
        else if (k == "shuffle") shuffle = v == "1";
        else if (k == "repeat") repeat = std::clamp(int(f), 0, 2);
        else if (k == "wheel_volume") wheelVolume = v == "1";
        else if (k == "ui_scale") uiScale = std::clamp(f, 0.5f, 2.0f);
        else if (k == "info_position") infoPosition = std::clamp(int(f), 0, 3);
        else if (k == "controls_position") controlsPosition = std::clamp(int(f), 0, 2);
        else if (k == "preset_bar_position") presetBarPosition = std::clamp(int(f), 0, 1);
        else if (k == "info_scale") infoScale = std::clamp(f, 0.5f, 2.5f);
        else if (k == "controls_scale") controlsScale = std::clamp(f, 0.5f, 2.5f);
        else if (k == "preset_bar_scale") presetBarScale = std::clamp(f, 0.5f, 2.5f);
        else if (k == "controls_vertical") controlsVertical = v == "1";
        else if (k == "info_xy") std::sscanf(v.c_str(), "%f,%f", &infoXY[0], &infoXY[1]);
        else if (k == "controls_xy") std::sscanf(v.c_str(), "%f,%f", &controlsXY[0], &controlsXY[1]);
        else if (k == "preset_bar_xy") std::sscanf(v.c_str(), "%f,%f", &presetBarXY[0], &presetBarXY[1]);
        else if (k == "display_mode") displayMode = std::clamp(int(f), 0, 2);
        else if (k == "fullscreen_type") fullscreenType = std::clamp(int(f), 1, 2);
        else if (k == "hdr") hdr = std::clamp(int(f), 0, 2);
        else if (k == "hdr_boost") hdrBoost = std::clamp(f, 1.0f, 8.0f);
        else if (k == "info_always") infoAlways = v == "1";
        else if (k == "on_screen_controls") onScreenControls = v == "1";
        else if (k == "debug_key") debugKey = v == "1";
        else if (k == "debug_spectrum") debugSpectrum = v == "1";
        else if (k == "verbose_log") verboseLog = v == "1";
        else if (k == "album_colours") albumColours = v == "1";
        else if (k == "album_art") albumArtMode = v == "1" ? 2 : 0; // (older files: on/off)
        else if (k == "album_art_mode") albumArtMode = std::clamp(int(f), 0, 2);
        else if (k == "log_volume") logVolume = v == "1";
        else if (k == "config_version") configVersion = int(f);
        else if (k == "shuffle_gap") shuffleGap = std::clamp(f, 0.0f, 0.9f);
        else if (k == "menu_tall") menuTall = v == "1";
        else if (k == "dither") dither = std::clamp(int(f), 0, 3);
        else if (k == "perf_overlay") perfOverlay = std::clamp(int(f), 0, 2);
        else if (k == "perf_window") perfWindow = std::clamp(f, 1.0f, 60.0f);
        else if (k == "resource_overlay") resourceOverlay = std::clamp(int(f), 0, 2);
        else if (k == "dump_hardware") dumpHardware = v == "1";
        else if (k == "dump_software") dumpSoftware = v == "1";
        else if (k == "dump_redact") dumpRedact = v == "1";
        else if (k == "show_config_keys") showConfigKeys = v == "1";
        else if (k == "show_config_values") showConfigValues = v == "1";
        else if (k == "show_config_ranges") showConfigRanges = v == "1";
        else if (k == "hardware_panel") hardwarePanel = v == "1";
        else if (k == "pause_mode") pauseMode = std::clamp(int(f), 0, 2);
        else if (k == "visualizer") visualizer = std::clamp(int(f), 0, 1);
        else if (k == "earth_view") earthView = v;
        else if (k == "earth_flight_speed") earth.flightSpeed = std::clamp(f, 0.1f, 4.0f);
        else if (k == "earth_clouds") earth.clouds = v == "1";
        else if (k == "earth_cloud_speed") earth.cloudSpeed = std::clamp(f, 0.0f, 10.0f);
        else if (k == "earth_stars") earth.stars = std::clamp(f, 0.0f, 3.0f);
        else if (k == "earth_glare") earth.glare = std::clamp(f, 0.0f, 3.0f);
        else if (k == "earth_exposure") earth.exposure = std::clamp(f, 0.25f, 3.0f);
        else if (k == "earth_music") earth.music = std::clamp(int(f), 0, 2);
        else if (k == "earth_next_view") earth.nextAfterFlight = v == "1";
        else if (k == "earth_firmware_shaders") earth.firmwareShaders = v == "1";
        else if (k == "earth_real_time") earth.realTime = std::clamp(int(f), 0, 2);
        else if (k == "earth_anti_alias") earth.antiAlias = std::clamp(int(f), 0, 3);
        else if (k == "blur_hq") tweaks.blurHq = v == "1";
        else if (k == "ui_from_visuals") uiFromVisuals = v == "1";
        else if (k == "ui_readable") uiReadable = v == "1";
        else if (k.rfind("ui_", 0) == 0 && loadUiColour(k, f, *this)) {}
        else if (k == "ui_opacity") uiOpacity = std::clamp(f, 0.3f, 1.3f);
        else if (k == "song_fades") songFades = v == "1";
        else if (k == "info_crossfade") infoCrossfade = v == "1";
        else if (k == "fade_seconds") fadeSeconds = std::clamp(f, 0.01f, 10.0f);
        else if (k == "wave_height") tweaks.waveHeight = std::clamp(f, 0.1f, 4.0f);
        else if (k == "line_width") tweaks.lineWidth = std::clamp(f, 0.1f, 4.0f);
        else if (k == "speed") tweaks.speed = std::clamp(f, 0.1f, 3.0f);
        else if (k == "blur") tweaks.blur = std::clamp(f, 0.0f, 3.0f);
        else if (k == "glow") tweaks.glow = std::clamp(f, 0.0f, 3.0f);
        else if (k == "fov") tweaks.fov = std::clamp(f, 0.4f, 2.0f);
        else if (k == "blur_div") tweaks.blurDiv = std::clamp(int(f), 1, 8);
        else if (k == "drift") tweaks.drift = std::clamp(f, 0.0f, 3.0f);
        else if (k == "camera_motion") tweaks.cameraMotion = v == "1";
        else if (k == "keep_above_terrain") tweaks.keepAboveTerrain = v == "1";
        else if (k == "sync_ms") tweaks.syncMs = std::clamp(f, -500.0f, 500.0f);
        else if (k == "spectrum") tweaks.spectrumMode = v == "enhanced" ? 2 : v == "ps3_smooth_bass" ? 1 : 0;
        else if (k == "falling_tiles_mode") tweaks.fallingTiles = std::clamp(int(f), 0, 2);
        else if (k == "drift_speed") tweaks.driftSpeed = std::clamp(f, 0.05f, 4.0f);
        else if (k == "camera_smooth") tweaks.cameraSmooth = std::clamp(f, 0.0f, 5.0f);
        else if (k == "flow_morph") flowMorph = std::clamp(f, 0.5f, 30.0f);
        else if (k == "custom_in_rotation") customInRotation = v == "1";
        else if (k.rfind("custom.", 0) == 0) custom[k.substr(7)] = std::clamp(f, -100000.0f, 100000.0f);
        else if (k.rfind("pad.", 0) == 0) {
            for (int i = 0; i < Keybinds::kCount; i++)
                if (k.substr(4) == Keybinds::id(Action(i)))
                    keys.pad[i] = padButtonFromId(v);
        }
        else if (k == "controllers") controllers = v == "1";
        else if (k == "file_browser") fileBrowser = std::clamp(int(f), 0, 2);
        else if (k == "browse_dir") browseDir = v;
        else if (k == "pad_glyphs") padGlyphs = std::clamp(int(f), 0, 3);
        else if (k.rfind("key.", 0) == 0) {
            for (int i = 0; i < Keybinds::kCount; i++)
                if (k.substr(4) == Keybinds::id(Action(i))) {
                    // key.<action>=keycode:mods,keycode:mods
                    unsigned k0 = 0, k1 = 0;
                    int m0 = 0, m1 = 0;
                    if (std::sscanf(v.c_str(), "%u:%d,%u:%d", &k0, &m0, &k1, &m1) == 4) {
                        keys.slot[i][0] = {SDL_Keycode(k0), m0};
                        keys.slot[i][1] = {SDL_Keycode(k1), m1};
                    }
                }
        }
        else if (k == "eq_enabled") eq.enabled = v == "1";
        else if (k == "eq_target") eq.target = std::clamp(int(f), 0, 2);
        else if (k == "eq_visualizer") eq.target = v == "1" ? EqSettings::AudioAndVisualizer : EqSettings::AudioOnly; // older files
        else if (k == "eq_preamp") eq.preampDb = std::clamp(f, -EqSettings::kMaxDb, EqSettings::kMaxDb);
        else if (k == "eq_bands") {
            std::stringstream ss(v);
            std::string item;
            for (size_t b = 0; b < eq.gainDb.size() && std::getline(ss, item, ','); b++)
                eq.gainDb[b] = std::clamp(float(std::atof(item.c_str())), -EqSettings::kMaxDb, EqSettings::kMaxDb);
        }
        else if (k == "flow") flow = std::clamp(int(f), 0, 2);
        else if (k == "flow_seconds") flowSeconds = std::clamp(f, 10.0f, 300.0f);
    }
    // 2 -> 3: the colour choices got "Theme" (the UI theme's own colour) in front. The old first choice was the
    // default (the PS3 theme's colours, so it becomes "Theme" and follows the theme); the others move up one.
    if (configVersion < 3)
        for (int n = 0; n < 4; n++) {
            UiColour &c = colourOf(*this, n);
            if ((uiColoursRead & (1u << n)) && c.choice > 0)
                c.choice = std::min(c.choice + 1, int(colourKinds()[n].choices.size()) - 1);
        }
    // 1 -> 2: Next/Previous track swapped PageDown/PageUp; swap them in files still on the old defaults.
    if (configVersion < 2) {
        KeyBind &next = keys.slot[int(Action::NextTrack)][1], &prev = keys.slot[int(Action::PrevTrack)][1];
        if (next.key == SDLK_PAGEDOWN && !next.mods && prev.key == SDLK_PAGEUP && !prev.mods)
            std::swap(next, prev);
    }
}

void Settings::save(const fs::path &file) const { std::ofstream(file) << text(); }

std::string Settings::text() const
{
    std::ostringstream out;
    out << "config_version=" << kConfigVersion << "\npreset=" << preset << "\nrandom_presets=" << randomPresets << "\ngain=" << gain
        << "\nvolume=" << volume << "\nshuffle=" << shuffle << "\nrepeat=" << repeat << "\nwheel_volume=" << wheelVolume << "\nui_scale=" << uiScale
        << "\ninfo_position=" << infoPosition << "\ncontrols_position=" << controlsPosition
        << "\npreset_bar_position=" << presetBarPosition << "\ninfo_scale=" << infoScale << "\ncontrols_scale=" << controlsScale
        << "\npreset_bar_scale=" << presetBarScale
        << "\ncontrols_vertical=" << controlsVertical << "\ninfo_xy=" << infoXY[0] << "," << infoXY[1]
        << "\ncontrols_xy=" << controlsXY[0] << "," << controlsXY[1] << "\npreset_bar_xy=" << presetBarXY[0] << ","
        << presetBarXY[1]
        << "\ndisplay_mode=" << displayMode << "\nfullscreen_type=" << fullscreenType
        << "\nhdr=" << hdr << "\nhdr_boost=" << hdrBoost
        << "\ninfo_always=" << infoAlways << "\non_screen_controls=" << onScreenControls << "\ndebug_key=" << debugKey << "\ndebug_spectrum=" << debugSpectrum << "\nverbose_log=" << verboseLog << "\nalbum_colours=" << albumColours << "\nalbum_art_mode=" << albumArtMode << "\nui_opacity=" << uiOpacity << "\nui_from_visuals=" << uiFromVisuals << "\nui_readable=" << uiReadable << "\nlog_volume=" << logVolume << "\ncontrollers=" << controllers << "\nfile_browser=" << fileBrowser << "\nbrowse_dir=" << browseDir << "\npad_glyphs=" << padGlyphs << "\nshuffle_gap=" << shuffleGap
        << "\nmenu_tall=" << menuTall << "\nui_theme=" << ui::style(uiStyle).name << "\ndither=" << dither << "\nperf_overlay=" << perfOverlay << "\nperf_window=" << perfWindow << "\nresource_overlay=" << resourceOverlay
        << "\ndump_hardware=" << dumpHardware << "\ndump_software=" << dumpSoftware << "\ndump_redact=" << dumpRedact << "\nshow_config_keys=" << showConfigKeys << "\nshow_config_values=" << showConfigValues
        << "\nshow_config_ranges=" << showConfigRanges << "\nhardware_panel=" << hardwarePanel << "\npause_mode=" << pauseMode
        << "\nvisualizer=" << visualizer << "\nearth_view=" << earthView << "\nearth_flight_speed=" << earth.flightSpeed
        << "\nearth_clouds=" << earth.clouds << "\nearth_cloud_speed=" << earth.cloudSpeed << "\nearth_stars=" << earth.stars
        << "\nearth_glare=" << earth.glare << "\nearth_exposure=" << earth.exposure << "\nearth_music=" << earth.music
        << "\nearth_next_view=" << earth.nextAfterFlight << "\nearth_firmware_shaders=" << earth.firmwareShaders
        << "\nearth_real_time=" << earth.realTime << "\nearth_anti_alias=" << earth.antiAlias
        << "\nblur_hq=" << tweaks.blurHq
        << saveUiColours(*this) << "\nsong_fades=" << songFades << "\ninfo_crossfade=" << infoCrossfade << "\nfade_seconds=" << fadeSeconds << "\nwave_height=" << tweaks.waveHeight
        << "\nline_width=" << tweaks.lineWidth << "\nspeed=" << tweaks.speed << "\nblur=" << tweaks.blur
        << "\nglow=" << tweaks.glow << "\nfov=" << tweaks.fov << "\nblur_div=" << tweaks.blurDiv << "\ndrift=" << tweaks.drift
        << "\nflow=" << flow << "\nflow_seconds=" << flowSeconds
        << "\ncamera_motion=" << tweaks.cameraMotion << "\nkeep_above_terrain=" << tweaks.keepAboveTerrain << "\nsync_ms=" << tweaks.syncMs << "\nspectrum=" << (tweaks.spectrumMode == 2 ? "enhanced" : tweaks.spectrumMode == 1 ? "ps3_smooth_bass" : "ps3") << "\nfalling_tiles_mode=" << tweaks.fallingTiles << "\ndrift_speed=" << tweaks.driftSpeed
        << "\ncamera_smooth=" << tweaks.cameraSmooth << "\nflow_morph=" << flowMorph << "\n";
    out << "custom_in_rotation=" << customInRotation << "\n";
    out << "eq_enabled=" << eq.enabled << "\neq_target=" << eq.target << "\neq_preamp=" << eq.preampDb
        << "\neq_bands=";
    for (size_t b = 0; b < eq.gainDb.size(); b++)
        out << (b ? "," : "") << eq.gainDb[b];
    out << "\n";
    for (auto &[name, value] : custom)
        out << "custom." << name << "=" << value << "\n";
    for (int i = 0; i < Keybinds::kCount; i++) {
        out << "key." << Keybinds::id(Action(i)) << "=" << unsigned(keys.slot[i][0].key) << ":"
            << keys.slot[i][0].mods << "," << unsigned(keys.slot[i][1].key) << ":" << keys.slot[i][1].mods << "\n";
        out << "pad." << Keybinds::id(Action(i)) << "=" << padButtonId(keys.pad[i]) << "\n";
    }
    return out.str();
}

// ---------------------------------------------------------------- menu

void Menu::setOpen(bool o)
{
    open_ = o;
    dragging_ = false;
    barFocus_ = -1;
    confirmOpen_ = false;
    renaming_ = -1;
    capturingPad_ = false;
    browsing_ = false;
    if (o)
        SDL_ShowCursor();
}

std::vector<Menu::Row> Menu::settingsRows(Context &c)
{
    Settings &s = c.settings;
    auto changed = c.changed;
    std::vector<Row> rows;
    const auto &names = c.canyon.presets();
    // Which visualizer (the PS3 offers both): the rows below depend on it (see rowsFor()).
    static const char *visNames[] = {"Waves (canyon)", "Earth"};
    rows.push_back({"Visualizer", visNames[std::clamp(s.visualizer, 0, 1)],
                    [&s, changed](int d) { s.visualizer = (s.visualizer + d + 2) % 2; changed(); }, nullptr});
    {
        rows.push_back({"View", "View " + s.earthView, [&c](int d) { c.stepPreset(d); }, nullptr});
        rows.push_back({"Next view after each flight", s.earth.nextAfterFlight ? "On" : "Off",
                        [&s, changed](int) { s.earth.nextAfterFlight = !s.earth.nextAfterFlight; changed(); }, nullptr});
        static const char *realNames[] = {"Off", "Turn the Earth", "Move the sun"};
        rows.push_back({"Real time", realNames[std::clamp(s.earth.realTime, 0, 2)],
                        [&s, changed](int d) { s.earth.realTime = (s.earth.realTime + d + 3) % 3; changed(); }, nullptr});
        rows.push_back({"Flight speed", fmt(s.earth.flightSpeed, "x"),
                        [&s, changed](int d) { step(s.earth.flightSpeed, d, 0.1f, 0.1f, 4.0f); changed(); }, nullptr});
        rows.push_back({"Shaders", s.earth.firmwareShaders ? "Firmware (hybrid)" : "Lookalike",
                        [&s, changed](int) { s.earth.firmwareShaders = !s.earth.firmwareShaders; changed(); }, nullptr});
        static const char *aaNames[] = {"Off", "2x", "4x", "8x"};
        rows.push_back({"Anti-aliasing", aaNames[std::clamp(s.earth.antiAlias, 0, 3)],
                        [&s, changed](int d) { s.earth.antiAlias = std::clamp(s.earth.antiAlias + d, 0, 3); changed(); }, nullptr});
        rows.push_back({"Clouds", s.earth.clouds ? "On" : "Off",
                        [&s, changed](int) { s.earth.clouds = !s.earth.clouds; changed(); }, nullptr});
        rows.push_back({"Cloud drift", fmt(s.earth.cloudSpeed, "x"),
                        [&s, changed](int d) { step(s.earth.cloudSpeed, d, 0.25f, 0.0f, 10.0f); changed(); }, nullptr});
        rows.push_back({"Stars", s.earth.stars <= 0 ? "Off" : fmt(s.earth.stars, "x"),
                        [&s, changed](int d) { step(s.earth.stars, d, 0.25f, 0.0f, 3.0f); changed(); }, nullptr});
        rows.push_back({"Sun glare", s.earth.glare <= 0 ? "Off" : fmt(s.earth.glare, "x"),
                        [&s, changed](int d) { step(s.earth.glare, d, 0.1f, 0.0f, 3.0f); changed(); }, nullptr});
        rows.push_back({"Exposure", fmt(s.earth.exposure, "x"),
                        [&s, changed](int d) { step(s.earth.exposure, d, 0.05f, 0.25f, 3.0f); changed(); }, nullptr});
        static const char *musicNames[] = {"Off (like the PS3)", "Gentle", "Strong"};
        rows.push_back({"React to music", musicNames[std::clamp(s.earth.music, 0, 2)],
                        [&s, changed](int d) { s.earth.music = (s.earth.music + d + 3) % 3; changed(); }, nullptr});
    }
    rows.push_back({"Preset", names.empty() ? "-" : c.presetName(), [&c](int d) { c.stepPreset(d); }, nullptr});
    static const char *flowNames[3] = {"Off", "Within family", "All presets"};
    rows.push_back({"Preset flow", flowNames[std::clamp(s.flow, 0, 2)],
                    [&s, changed](int d) { s.flow = (s.flow + d + 3) % 3; changed(); }, nullptr});
    rows.push_back({"Flow interval", std::to_string(int(std::lround(s.flowSeconds))) + " s",
                    [&s, changed](int d) { step(s.flowSeconds, d, 5.0f, 10.0f, 300.0f); changed(); }, nullptr});
    rows.push_back({"Flow transition", fmt(s.flowMorph, " s"),
                    [&s, changed](int d) { step(s.flowMorph, d, 0.5f, 0.5f, 30.0f); changed(); }, nullptr});
    rows.push_back({"Keep camera above terrain", s.tweaks.keepAboveTerrain ? "On" : "Off",
                    [&s, changed](int) { s.tweaks.keepAboveTerrain = !s.tweaks.keepAboveTerrain; changed(); }, nullptr});
    rows.push_back({"Camera motion", s.tweaks.cameraMotion ? "On" : "Off",
                    [&s, changed](int) { s.tweaks.cameraMotion = !s.tweaks.cameraMotion; changed(); }, nullptr});
    rows.push_back({"Drift distance", fmt(s.tweaks.drift, "x"),
                    [&s, changed](int d) { step(s.tweaks.drift, d, 0.1f, 0.0f, 3.0f); changed(); }, nullptr});
    rows.push_back({"Drift speed", fmt(s.tweaks.driftSpeed, "x"),
                    [&s, changed](int d) { step(s.tweaks.driftSpeed, d, 0.05f, 0.05f, 4.0f); changed(); }, nullptr});
    rows.push_back({"Camera smoothing", s.tweaks.cameraSmooth <= 0 ? "Off" : fmt(s.tweaks.cameraSmooth, " s"),
                    [&s, changed](int d) { step(s.tweaks.cameraSmooth, d, 0.04f, 0.0f, 5.0f); changed(); }, nullptr});
    rows.push_back({"New preset for every track", s.randomPresets ? "On" : "Off",
                    [&s, changed](int) { s.randomPresets = !s.randomPresets; changed(); }, nullptr});
    static const char *tileNames[3] = {"Off", "Auto", "On"};
    rows.push_back({"Falling tiles effect", tileNames[std::clamp(s.tweaks.fallingTiles, 0, 2)],
                    [&s, changed](int d) { s.tweaks.fallingTiles = (s.tweaks.fallingTiles + d + 3) % 3; changed(); },
                    nullptr});
    static const char *responseNames[] = {"PS3", "PS3, smooth bass", "Enhanced"};
    rows.push_back({"Colours from album art", s.albumColours ? "On" : "Off",
                    [&s, changed](int) { s.albumColours = !s.albumColours; changed(); }, nullptr});
    rows.push_back({"Music response", responseNames[std::clamp(s.tweaks.spectrumMode, 0, 2)],
                    [&s, changed](int d) { s.tweaks.spectrumMode = (s.tweaks.spectrumMode + d + 3) % 3; changed(); },
                    nullptr});
    rows.push_back({"Visual sync", (s.tweaks.syncMs > 0 ? "+" : "") + std::to_string(int(std::lround(s.tweaks.syncMs))) + " ms",
                    [&s, changed](int d) { step(s.tweaks.syncMs, d, 10.0f, -500.0f, 500.0f); changed(); }, nullptr});
    rows.push_back({"Sensitivity", fmt(s.gain, "x"), [&s, changed](int d) { step(s.gain, d, 0.25f, 0.25f, 12.0f); changed(); },
                    nullptr});
    rows.push_back({"Wave height", fmt(s.tweaks.waveHeight, "x"),
                    [&s, changed](int d) { step(s.tweaks.waveHeight, d, 0.1f, 0.1f, 4.0f); changed(); }, nullptr});
    rows.push_back({"Line width", fmt(s.tweaks.lineWidth, "x"),
                    [&s, changed](int d) { step(s.tweaks.lineWidth, d, 0.1f, 0.1f, 4.0f); changed(); }, nullptr});
    rows.push_back({"Glow", s.tweaks.glow <= 0 ? "Off" : fmt(s.tweaks.glow, "x"),
                    [&s, changed](int d) { step(s.tweaks.glow, d, 0.1f, 0.0f, 3.0f); changed(); }, nullptr});
    rows.push_back({"Depth of field", s.tweaks.blur <= 0 ? "Off" : fmt(s.tweaks.blur, "x"),
                    [&s, changed](int d) { step(s.tweaks.blur, d, 0.1f, 0.0f, 3.0f); changed(); }, nullptr});
    static const int divs[4] = {1, 2, 4, 8};
    static const char *divNames[4] = {"Full (sharpest)", "1/2", "1/4 (default)", "1/8 (softest)"};
    int divIdx = 2;
    for (int k = 0; k < 4; k++)
        if (divs[k] == s.tweaks.blurDiv)
            divIdx = k;
    rows.push_back({"Blur resolution", divNames[divIdx], [&s, changed, divIdx](int d) {
                        s.tweaks.blurDiv = divs[std::clamp(divIdx + d, 0, 3)];
                        changed();
                    }, nullptr});
    rows.push_back({"High-quality blur", s.tweaks.blurHq ? "On" : "Off",
                    [&s, changed](int) { s.tweaks.blurHq = !s.tweaks.blurHq; changed(); }, nullptr});
    rows.push_back({"Field of view", fmt(s.tweaks.fov, "x"),
                    [&s, changed](int d) { step(s.tweaks.fov, d, 0.05f, 0.4f, 2.0f); changed(); }, nullptr});
    rows.push_back({"Speed", fmt(s.tweaks.speed, "x"),
                    [&s, changed](int d) { step(s.tweaks.speed, d, 0.1f, 0.1f, 3.0f); changed(); }, nullptr});
    rows.push_back({"Volume", volumeText(s), [&s, changed](int d) {
                        const bool fine = SDL_GetModState() & SDL_KMOD_SHIFT; // Shift: 1% steps
                        step(s.volume, d, fine ? 0.01f : 0.05f, 0.0f, 1.5f);
                        changed();
                    },
                    nullptr});
    rows.push_back({"Volume curve", s.logVolume ? "Logarithmic" : "Linear",
                    [&s, changed](int) { s.logVolume = !s.logVolume; changed(); }, nullptr});
    rows.push_back({"Shuffle", s.shuffle ? "On" : "Off", [&s, changed](int) { s.shuffle = !s.shuffle; changed(); }, nullptr});
    rows.push_back({"Shuffle repeat gap", std::to_string(int(std::lround(s.shuffleGap * 100))) + "% of the playlist",
                    [&s, changed](int d) {
                        step(s.shuffleGap, d, SDL_GetModState() & SDL_KMOD_SHIFT ? 0.01f : 0.05f, 0.0f, 0.9f);
                        changed();
                    }, nullptr});
    static const char *repeatNames[] = {"Off", "Whole playlist", "One song"};
    rows.push_back({"Repeat", repeatNames[std::clamp(s.repeat, 0, 2)],
                    [&s, changed](int d) { s.repeat = (s.repeat + d + 3) % 3; changed(); }, nullptr});
    rows.push_back({"Track info", s.infoAlways ? "Always" : "Auto",
                    [&s, changed](int) { s.infoAlways = !s.infoAlways; changed(); }, nullptr});
    rows.push_back({"Song fades", s.songFades ? "On" : "Off",
                    [&s, changed](int) { s.songFades = !s.songFades; changed(); }, nullptr});
    {
        // 10 ms .. 10 s in steps that feel even.
        static const float kFades[] = {0.01f, 0.025f, 0.05f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f,
                                       2.0f,  3.0f,   4.0f,  5.0f, 6.0f,  7.5f, 10.0f};
        constexpr int kN = int(sizeof kFades / sizeof kFades[0]);
        char label[32];
        if (s.fadeSeconds < 1.0f)
            std::snprintf(label, sizeof label, "%d ms", int(std::lround(s.fadeSeconds * 1000.0f)));
        else
            std::snprintf(label, sizeof label, "%g s", double(s.fadeSeconds));
        rows.push_back({"Fade duration", label, [&s, changed](int d) {
                            int i = 0;
                            while (i < kN - 1 && kFades[i] < s.fadeSeconds - 1e-4f)
                                i++;
                            s.fadeSeconds = kFades[std::clamp(i + d, 0, kN - 1)];
                            changed();
                        }, nullptr});
    }
    rows.push_back({"Crossfade track info", s.infoCrossfade ? "On" : "Off",
                    [&s, changed](int) { s.infoCrossfade = !s.infoCrossfade; changed(); }, nullptr});
    static const char *artNames[] = {"Off", "Auto (with track info)", "Always"};
    rows.push_back({"Album art", artNames[std::clamp(s.albumArtMode, 0, 2)],
                    [&s, changed](int d) { s.albumArtMode = (s.albumArtMode + d + 3) % 3; changed(); }, nullptr});
    {
        const int n = int(ui::styles().size());
        rows.push_back({"Theme", ui::style(s.uiStyle).name,
                        [&s, changed, n](int d) { s.uiStyle = (std::clamp(s.uiStyle, 0, n - 1) + d + n) % n; changed(); },
                        nullptr});
        rows.push_back({"Copy theme to a file", "", nullptr, [&c] {
                            if (c.exportTheme)
                                c.exportTheme();
                        }});
        // Edit theme: one colour of one element of a user theme, saved to its file on every change.
        if (ui::Style *us = ui::userStyle(s.uiStyle)) {
            const int index = s.uiStyle;
            auto save = [index, &c] {
                std::string error;
                if (!ui::saveUserStyle(index, &error) && !error.empty())
                    c.notify(error);
            };
            const int parts = int(ui::Part::Count);
            editPart_ = std::clamp(editPart_, 0, parts - 1);
            std::string partLabel = ui::partName(ui::Part(editPart_));
            for (char &ch : partLabel)
                if (ch == '_')
                    ch = ' ';
            partLabel[0] = char(std::toupper(static_cast<unsigned char>(partLabel[0])));
            rows.push_back({"Edit element", partLabel, [this, parts](int d) { editPart_ = (editPart_ + d + parts) % parts; },
                            nullptr});
            struct Prop {
                const char *name;
                ui::Colour ui::Paint::*member;
            };
            static const Prop props[] = {{"Fill", &ui::Paint::fill},     {"Gradient end", &ui::Paint::fill2},
                                         {"Text", &ui::Paint::text},     {"Border", &ui::Paint::border},
                                         {"Edge line", &ui::Paint::edge}, {"Shadow", &ui::Paint::shadow},
                                         {"Tint", &ui::Paint::tint}};
            const int nProps = int(std::size(props));
            editProp_ = std::clamp(editProp_, 0, nProps - 1);
            rows.push_back({"Edit property", props[editProp_].name,
                            [this, nProps](int d) { editProp_ = (editProp_ + d + nProps) % nProps; }, nullptr});
            ui::Paint &paint = us->paints[size_t(editPart_)];
            ui::Colour &col = paint.*(props[editProp_].member);
            static const char *roleNames[] = {"None", "Accent", "Text", "Background", "Highlight", "White", "Black",
                                              "Danger", "Custom"};
            const UiTheme pal = uiTheme(s);
            Row r{"Element colour", roleNames[int(col.role)],
                  [&col, &paint, pal, save](int d) {
                      const Rgba was = ui::resolve(col, pal); // (Custom starts from what was shown)
                      col.role = ui::Role((int(col.role) + d + 9) % 9);
                      if (col.role == ui::Role::Fixed) {
                          col.rgb[0] = was.r, col.rgb[1] = was.g, col.rgb[2] = was.b;
                          col.scale = 1, col.add = 0;
                      }
                      // A colour on a part that needs a size to show: give it one.
                      if (col.set() && &col == &paint.border && paint.borderWidth <= 0)
                          paint.borderWidth = 1;
                      if (col.set() && &col == &paint.edge && (paint.edgeWidth <= 0 || paint.edgeSide == ui::Side::None)) {
                          paint.edgeWidth = std::max(paint.edgeWidth, 2.0f);
                          if (paint.edgeSide == ui::Side::None)
                              paint.edgeSide = ui::Side::Bottom;
                      }
                      if (col.set() && &col == &paint.shadow && paint.shadowBlur <= 0 && paint.shadowY == 0)
                          paint.shadowBlur = 12, paint.shadowY = 3;
                      save();
                  },
                  nullptr};
            const Rgba shownRgb = ui::resolve(col, pal);
            r.swatch = col.set();
            r.swatchRgb[0] = shownRgb.r, r.swatchRgb[1] = shownRgb.g, r.swatchRgb[2] = shownRgb.b;
            rows.push_back(r);
            if (col.role == ui::Role::Fixed) {
                float hue, sat, val;
                rgbHsv(col.rgb, hue, sat, val);
                auto setHsv = [&col, save](float h, float sa, float v) {
                    hsvRgb(h, sa, v, col.rgb);
                    save();
                };
                Row h{"Element hue", "", [=](int d) { setHsv(std::fmod(hue + 5.0f * float(d) + 360.0f, 360.0f), sat, val); },
                      nullptr};
                h.picker = 1;
                h.pickPos = hue / 360.0f;
                h.pick = [=](float f) { setHsv(std::clamp(f, 0.0f, 1.0f) * 360.0f, sat, val); };
                rows.push_back(h);
                Row sa{"Element saturation", "", [=](int d) { setHsv(hue, std::clamp(sat + 0.05f * float(d), 0.0f, 1.0f), val); },
                       nullptr};
                sa.picker = 2;
                sa.pickPos = sat;
                sa.pickHue = hue;
                sa.pick = [=](float f) { setHsv(hue, std::clamp(f, 0.0f, 1.0f), val); };
                rows.push_back(sa);
                Row v{"Element brightness", "", [=](int d) { setHsv(hue, sat, std::clamp(val + 0.02f * float(d), 0.0f, 1.0f)); },
                      nullptr};
                v.picker = 3;
                v.pickPos = val;
                v.pickHue = hue;
                v.pickSat = sat;
                v.pick = [=](float f) { setHsv(hue, sat, std::clamp(f, 0.0f, 1.0f)); };
                rows.push_back(v);
            }
            if (col.set())
                rows.push_back({"Element opacity", std::to_string(int(std::lround(col.alpha * 100))) + "%",
                                [&col, save](int d) {
                                    col.alpha = std::clamp(std::round(col.alpha * 20 + float(d)) / 20, 0.0f, 1.0f);
                                    save();
                                },
                                nullptr});
            // The element's shape (whatever the property).
            auto number = [&](const char *label, float &v, float stepBy, float hi) {
                rows.push_back({label, fmt(v, ""), [&v, stepBy, hi, save](int d) {
                                    v = std::clamp(std::round((v + stepBy * float(d)) / stepBy) * stepBy, 0.0f, hi);
                                    save();
                                },
                                nullptr});
            };
            number("Element corner radius", paint.radius, 1, 40);
            number("Element border width", paint.borderWidth, 0.5f, 8);
            number("Element gloss", paint.gloss, 0.05f, 1);
            number("Element shadow blur", paint.shadowBlur, 2, 60);
        }
    }
    rows.push_back({"UI colours from visuals", s.uiFromVisuals ? "On" : "Off",
                    [&s, changed](int) { s.uiFromVisuals = !s.uiFromVisuals; changed(); }, nullptr});
    if (!s.uiFromVisuals) {
        const UiTheme theme = uiTheme(s);
        const float *shown[4] = {theme.accent, theme.text, theme.bg, theme.highlight};
        for (int n = 0; n < 4; n++) {
            UiColour &col = colourOf(s, n);
            const auto &kind = colourKinds()[n];
            const int count = int(kind.choices.size());
            Row r{std::string(kind.what) + " colour", kind.choices[size_t(std::clamp(col.choice, 0, count - 1))].name,
                  [&col, count, changed](int d) { col.choice = (col.choice + d + count) % count; changed(); }, nullptr};
            r.swatch = true;
            std::copy(shown[n], shown[n] + 3, r.swatchRgb);
            rows.push_back(r);
            if (col.choice != count - 1)
                continue;
            // Custom: hue, saturation and brightness strips.
            const std::string what = kind.what;
            Row h{what + " hue", "", [&col, changed](int d) {
                      col.hue = std::fmod(col.hue + 5.0f * float(d) + 360.0f, 360.0f);
                      changed();
                  }, nullptr};
            h.picker = 1;
            h.pickPos = col.hue / 360.0f;
            h.pick = [&col, changed](float f) { col.hue = std::clamp(f, 0.0f, 1.0f) * 360.0f; changed(); };
            rows.push_back(h);
            Row sat{what + " saturation", "", [&col, changed](int d) { step(col.sat, d, 0.05f, 0.0f, 1.0f); changed(); },
                    nullptr};
            sat.picker = 2;
            sat.pickPos = col.sat;
            sat.pickHue = col.hue;
            sat.pick = [&col, changed](float f) { col.sat = std::clamp(f, 0.0f, 1.0f); changed(); };
            rows.push_back(sat);
            Row val{what + " brightness", "", [&col, changed](int d) { step(col.val, d, 0.02f, 0.0f, 1.0f); changed(); },
                    nullptr};
            val.picker = 3;
            val.pickPos = col.val;
            val.pickHue = col.hue;
            val.pickSat = col.sat;
            val.pick = [&col, changed](float f) { col.val = std::clamp(f, 0.0f, 1.0f); changed(); };
            rows.push_back(val);
        }
    }
    rows.push_back({"Keep text readable", s.uiReadable ? "On" : "Off",
                    [&s, changed](int) { s.uiReadable = !s.uiReadable; changed(); }, nullptr});
    rows.push_back({"Background opacity", std::to_string(int(std::lround(s.uiOpacity * 100))) + "%",
                    [&s, changed](int d) { step(s.uiOpacity, d, 0.1f, 0.3f, 1.3f); changed(); }, nullptr});
    rows.push_back({"UI scale", fmt(s.uiScale, "x"),
                    [&s, changed](int d) { step(s.uiScale, d, 0.1f, 0.5f, 2.0f); changed(); }, nullptr});
    static const char *infoPos[] = {"Bottom left", "Bottom right", "Top left", "Top right"};
    rows.push_back({"Track info position", s.infoXY[0] >= 0 ? "Custom (dragged)" : infoPos[std::clamp(s.infoPosition, 0, 3)],
                    [&s, changed](int d) {
                        if (s.infoXY[0] < 0)
                            s.infoPosition = (s.infoPosition + d + 4) % 4;
                        s.infoXY[0] = s.infoXY[1] = -1;
                        changed();
                    }, nullptr});
    static const char *ctlPos[] = {"Bottom right", "Bottom centre", "Bottom left"};
    static const char *ctlPosV[] = {"Right edge", "Right edge", "Left edge"};
    rows.push_back({"Playback buttons position",
                    s.controlsXY[0] >= 0 ? "Custom (dragged)"
                    : s.controlsVertical ? ctlPosV[std::clamp(s.controlsPosition, 0, 2)]
                                         : ctlPos[std::clamp(s.controlsPosition, 0, 2)],
                    [&s, changed](int d) {
                        if (s.controlsXY[0] < 0) {
                            s.controlsPosition = (s.controlsPosition + d + 3) % 3;
                            if (s.controlsVertical && s.controlsPosition == 1) // (no centre on the edges)
                                s.controlsPosition = (s.controlsPosition + d + 3) % 3;
                        }
                        s.controlsXY[0] = s.controlsXY[1] = -1;
                        changed();
                    }, nullptr});
    rows.push_back({"Vertical playback buttons", s.controlsVertical ? "On" : "Off", [&s, changed](int) {
                        s.controlsVertical = !s.controlsVertical;
                        if (s.controlsVertical && s.controlsPosition == 1)
                            s.controlsPosition = 0;
                        s.controlsXY[0] = s.controlsXY[1] = -1;
                        changed();
                    }, nullptr});
    rows.push_back({"Preset bar position", s.presetBarXY[0] >= 0 ? "Custom (dragged)" : s.presetBarPosition ? "Bottom" : "Top",
                    [&s, changed](int) {
                        if (s.presetBarXY[0] < 0)
                            s.presetBarPosition ^= 1;
                        s.presetBarXY[0] = s.presetBarXY[1] = -1;
                        changed();
                    }, nullptr});
    rows.push_back({"Track info & art scale", fmt(s.infoScale, "x"),
                    [&s, changed](int d) { step(s.infoScale, d, 0.1f, 0.5f, 2.5f); changed(); }, nullptr});
    {
        // Dragging elements needs a mouse: greyed out while a controller is the input in use.
        const bool pad = c.padActive && c.padActive();
        Row arrange{"Arrange on-screen elements", pad ? "Not with a controller" : "", nullptr, [&c, pad] {
                        if (pad)
                            c.notify("Arranging the screen needs a mouse");
                        else
                            c.arrange();
                    }};
        arrange.disabled = pad;
        rows.push_back(arrange);
    }
    static const char *pauseNames[] = {"Glide to a stop", "Stop at once", "Keep moving"};
    rows.push_back({"When paused", pauseNames[std::clamp(s.pauseMode, 0, 2)],
                    [&s, changed](int d) { s.pauseMode = (s.pauseMode + d + 3) % 3; changed(); }, nullptr});
    static const char *browserNames[] = {"System dialogs", "Built-in", "Auto (built-in with a controller)"};
    rows.push_back({"File browser", browserNames[std::clamp(s.fileBrowser, 0, 2)],
                    [&s, changed](int d) { s.fileBrowser = (s.fileBrowser + d + 3) % 3; changed(); }, nullptr});
    rows.push_back({"Mouse wheel", s.wheelVolume ? "Volume" : "Off",
                    [&s, changed](int) { s.wheelVolume = !s.wheelVolume; changed(); }, nullptr});
    rows.push_back({"On-screen controls", s.onScreenControls ? "On" : "Off",
                    [&s, changed](int) { s.onScreenControls = !s.onScreenControls; changed(); }, nullptr});
    static const char *overlayNames[] = {"Off", "Numbers", "Numbers + graph"};
    rows.push_back({"Frame rate display", overlayNames[std::clamp(s.perfOverlay, 0, 2)],
                    [&s, changed](int d) { s.perfOverlay = (s.perfOverlay + d + 3) % 3; changed(); }, nullptr});
    {
        static const float windows[] = {1, 5, 10, 30, 60};
        int at = 0;
        for (int i = 0; i < 5; i++)
            if (std::fabs(s.perfWindow - windows[i]) < std::fabs(s.perfWindow - windows[at]))
                at = i;
        rows.push_back({"Frame stats window", std::to_string(int(windows[at])) + " s", [&s, changed, at](int d) {
                            s.perfWindow = windows[std::clamp(at + d, 0, 4)];
                            changed();
                        }, nullptr});
    }
    rows.push_back({"Resource usage display", overlayNames[std::clamp(s.resourceOverlay, 0, 2)],
                    [&s, changed](int d) { s.resourceOverlay = (s.resourceOverlay + d + 3) % 3; changed(); }, nullptr});
    rows.push_back({"Dump: hardware details", s.dumpHardware ? "On" : "Off",
                    [&s, changed](int) { s.dumpHardware = !s.dumpHardware; changed(); }, nullptr});
    rows.push_back({"Dump: software details", s.dumpSoftware ? "On" : "Off",
                    [&s, changed](int) { s.dumpSoftware = !s.dumpSoftware; changed(); }, nullptr});
    rows.push_back({"Dump: hide personal info", s.dumpRedact ? "On" : "Off",
                    [&s, changed](int) { s.dumpRedact = !s.dumpRedact; changed(); }, nullptr});
    {
        StressTest &st = s.stress;
        auto cycle = [changed](int &v, int n) { return [&v, n, changed](int d) { v = (v + d + n) % n; changed(); }; };
        static const char *fpsNames[] = {"VSync (normal)", "Unlocked", "Cap 30 fps", "Cap 60 fps", "Cap 120 fps", "Cap 240 fps"};
        rows.push_back({"Frame rate limit", fpsNames[std::clamp(st.fps, 0, 5)], cycle(st.fps, 6), nullptr});
        static const char *visNames[] = {"60 Hz (normal)", "Every frame", "120 Hz", "240 Hz"};
        rows.push_back({"Visualizer update rate", visNames[std::clamp(st.visRate, 0, 3)], cycle(st.visRate, 4), nullptr});
        rows.push_back({"Render passes per update", std::to_string(st.renderPasses) + "x",
                        [&st, changed](int d) { st.renderPasses = std::clamp(st.renderPasses + d, 1, 8); changed(); },
                        nullptr});
        rows.push_back({"Render scale", std::to_string(int(std::lround(st.renderScale * 100))) + "%",
                        [&st, changed](int d) { step(st.renderScale, d, 0.25f, 0.25f, 3.0f); changed(); }, nullptr});
        static const char *stormNames[] = {"Off", "Every 2 s", "Every 0.5 s", "Every frame"};
        rows.push_back({"Preset storm", stormNames[std::clamp(st.presetStorm, 0, 3)], cycle(st.presetStorm, 4), nullptr});
        static const char *seekNames[] = {"Off", "Every 5 s", "Every second"};
        rows.push_back({"Seek storm", seekNames[std::clamp(st.seekStorm, 0, 2)], cycle(st.seekStorm, 3), nullptr});
        static const char *hitchNames[] = {"Off", "Up to 30 ms", "Up to 100 ms"};
        rows.push_back({"Random frame stalls", hitchNames[std::clamp(st.hitches, 0, 2)], cycle(st.hitches, 3), nullptr});
        static const char *cpuNames[] = {"Off", "1 busy thread", "Half the cores", "All cores"};
        rows.push_back({"Background CPU load", cpuNames[std::clamp(st.cpuLoad, 0, 3)], cycle(st.cpuLoad, 4), nullptr});
        rows.push_back({"Stop all stress tests", "", nullptr, [&st, changed] { st = StressTest{}; changed(); }});
    }
    rows.push_back({"Show settings keys in tooltips", s.showConfigKeys ? "On" : "Off",
                    [&s, changed](int) { s.showConfigKeys = !s.showConfigKeys; changed(); }, nullptr});
    for (auto [label, flag] : {std::pair{"Show current values", &s.showConfigValues},
                               std::pair{"Show valid values", &s.showConfigRanges}}) {
        Row r{label, *flag ? "On" : "Off", [flag, changed](int) { *flag = !*flag; changed(); }, nullptr};
        r.disabled = !s.showConfigKeys; // (only with the keys shown)
        rows.push_back(r);
    }
    rows.push_back({"Hardware info panel", s.hardwarePanel ? "On" : "Off",
                    [&s, changed](int) { s.hardwarePanel = !s.hardwarePanel; changed(); }, nullptr});
    rows.push_back({"Debug dump key", s.debugKey ? "On" : "Off",
                    [&s, changed](int) { s.debugKey = !s.debugKey; changed(); }, nullptr});
    rows.push_back({"Debug spectrum", s.debugSpectrum ? "On" : "Off",
                    [&s, changed](int) { s.debugSpectrum = !s.debugSpectrum; changed(); }, nullptr});
    rows.push_back({"Extensive logging", s.verboseLog ? "On" : "Off",
                    [&s, changed](int) { s.verboseLog = !s.verboseLog; changed(); }, nullptr});
    rows.push_back({"Reset visual settings", "", nullptr, [&s, changed] { // (of the chosen visualizer)
                        if (s.visualizer == 1) {
                            s.earth = Earth::Tweaks{};
                        } else {
                            s.gain = 3.0f;
                            s.tweaks = Canyon::Tweaks{};
                        }
                        changed();
                    }});
    static const char *hdrNames[] = {"Auto", "On", "Off"};
    rows.push_back({"HDR", std::string(hdrNames[std::clamp(s.hdr, 0, 2)]) + ": " + c.hdrStatus(),
                    [&s, changed](int d) { s.hdr = (s.hdr + d + 3) % 3; changed(); }, nullptr});
    rows.push_back({"HDR highlight boost", fmt(s.hdrBoost, "x"),
                    [&s, changed](int d) { step(s.hdrBoost, d, 0.5f, 1.0f, 8.0f); changed(); }, nullptr});
    static const char *displayNames[] = {"Window", "Borderless fullscreen", "Exclusive fullscreen"};
    rows.push_back({"Display", displayNames[std::clamp(s.displayMode, 0, 2)],
                    [&s, &c](int d) { c.setDisplay((s.displayMode + d + 3) % 3); },
                    [&s, &c] { c.setDisplay((s.displayMode + 1) % 3); }});
    {
        std::string status = c.padStatus ? c.padStatus() : std::string();
        if (status.size() > 30) // (long device names)
            status = status.substr(0, 28) + "...";
        rows.push_back({"Controller support", s.controllers ? (status.empty() ? "On" : "On: " + status) : "Off",
                        [&s, changed](int) { s.controllers = !s.controllers; changed(); }, nullptr});
        static const char *glyphNames[] = {"Auto (from the controller)", "Xbox", "PlayStation", "Steam"};
        rows.push_back({"Button glyphs", glyphNames[std::clamp(s.padGlyphs, 0, 3)],
                        [&s, changed](int d) { s.padGlyphs = (s.padGlyphs + d + 4) % 4; changed(); }, nullptr});
    }
    rows.push_back({"Menu size", s.menuTall ? "Tall" : "Normal",
                    [&s, changed](int) { s.menuTall = !s.menuTall; changed(); }, nullptr});
    static const char *ditherNames[] = {"Off", "Low", "Medium", "High"};
    rows.push_back({"Dithering", ditherNames[std::clamp(s.dither, 0, 3)],
                    [&s, changed](int d) { s.dither = std::clamp(s.dither + d, 0, 3); changed(); }, nullptr});
    // Toggles also respond to Enter.
    for (auto &r : rows)
        if (!r.activate && r.adjust && (r.value == "On" || r.value == "Off" || r.value == "Always" || r.value == "Auto")) {
            auto adj = r.adjust;
            r.activate = [adj] { adj(1); };
        }
    return rows;
}

namespace {
// Step sizes and ranges for editing the Custom preset, grouped like the
// firmware's menus. Shift multiplies the step by 10.
struct ParamSpec {
    const char *group, *name;
    float step, lo, hi;
};
const ParamSpec kParamSpecs[] = {
    {"Camera", "POS X", 1, -300, 300}, {"Camera", "POS Y", 1, -100, 300}, {"Camera", "POS Z", 1, -100, 200},
    {"Camera", "TARGET X", 5, -1000, 1000}, {"Camera", "TARGET Y", 5, -1000, 1000},
    {"Camera", "TARGET Z", 5, -1000, 1500}, {"Camera", "ZOOM", 1, 10, 170}, {"Camera", "POS SPEED", 0.5f, 0, 80},
    {"Camera", "TIME AHEAD", 2, -100, 500},
    {"Terrain", "TERRAIN HEIGHT", 1, 0, 400}, {"Terrain", "TERRAIN WIDTH", 0.1f, -5, 25},
    {"Terrain", "TERRAIN SLIDE", 0.1f, -20, 20}, {"Terrain", "SCALE X", 0.05f, 0.1f, 6},
    {"Terrain", "TEXTURE SCALE", 0.5f, 0, 40}, {"Terrain", "BUMP SCALE", 0.01f, 0, 1},
    {"Terrain", "VALLEY HEIGHT", 0.1f, -20, 20}, {"Terrain", "VALLEY CENTRE", 0.02f, -1, 1},
    {"Terrain", "VALLEY WIDTH", 0.02f, 0, 1}, {"Terrain", "VALLEY SHARP", 0.02f, 0, 3},
    {"Line", "LINE AHEAD", 1, -50, 500}, {"Line", "LINE WIDTH", 1, 0.5f, 300}, {"Line", "LINE HEIGHT", 0.5f, -80, 80},
    {"Line", "LINE LENGTH", 0.01f, 0, 3}, {"Line", "LINE BIAS", 0.01f, -1, 2},
    {"Colour", "FADE COLOUR", 0.05f, 0, 1}, {"Colour", "COLOUR R", 0.1f, 0, 15}, {"Colour", "COLOUR G", 0.1f, 0, 15},
    {"Colour", "COLOUR B", 0.1f, 0, 15}, {"Colour", "COLOUR SCALE", 0.05f, 0, 5},
    {"Colour", "COLOUR BIAS", 0.02f, -1, 1}, {"Colour", "LINE R", 0.05f, 0, 2}, {"Colour", "LINE G", 0.05f, 0, 2},
    {"Colour", "LINE B", 0.05f, 0, 2},
    {"Fog", "FOG MIN", 5, -500, 500}, {"Fog", "FOG MAX", 5, -500, 800}, {"Fog", "FOG R", 0.02f, 0, 1},
    {"Fog", "FOG G", 0.02f, 0, 1}, {"Fog", "FOG B", 0.02f, 0, 1},
    {"Blur", "FOCAL DEPTH", 5, -500, 1000}, {"Blur", "FOCAL VARIANCE", 5, 1, 500}, {"Blur", "LOD", 0.02f, 0, 1},
    {"Blur", "SKY BLUR", 0.05f, 0, 1},
    // Video feedback: each frame mixes in the previous one (FEEDBACK = how much), stretched by
    // X/Y and turned by ROT (radians). The firmware files these under Blur.
    {"Feedback (video trails)", "FEEDBACK", 0.05f, 0, 1}, {"Feedback (video trails)", "FEEDBACK X", 0.01f, 0.5f, 1.5f},
    {"Feedback (video trails)", "FEEDBACK Y", 0.01f, 0.5f, 1.5f},
    {"Feedback (video trails)", "FEEDBACK ROT", 0.05f, -3.2f, 3.2f},
};

std::string num(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3g", double(v));
    return buf;
}
} // namespace


bool loadUiColour(const std::string &k, float f, Settings &s)
{
    for (int n = 0; n < 4; n++) {
        const std::string base = std::string("ui_") + colourKinds()[n].key;
        UiColour &c = colourOf(s, n);
        if (k == base) {
            c.choice = std::clamp(int(f), 0, int(colourKinds()[n].choices.size()) - 1);
            s.uiColoursRead |= 1u << n;
        }
        else if (k == base + "_hue") c.hue = std::clamp(f, 0.0f, 360.0f);
        else if (k == base + "_sat") c.sat = std::clamp(f, 0.0f, 1.0f);
        else if (k == base + "_val") c.val = std::clamp(f, 0.0f, 1.0f);
        else continue;
        return true;
    }
    return false;
}

std::string saveUiColours(const Settings &s)
{
    std::ostringstream out;
    for (int n = 0; n < 4; n++) {
        const UiColour &c = colourOf(const_cast<Settings &>(s), n);
        const std::string base = std::string("\nui_") + colourKinds()[n].key;
        out << base << "=" << c.choice << base << "_hue=" << c.hue << base << "_sat=" << c.sat << base << "_val=" << c.val;
    }
    return out.str();
}

void hsvRgb(float hue, float sat, float val, float rgb[3])
{
    static const float offset[3] = {5, 3, 1};
    for (int k = 0; k < 3; k++) {
        const float n = std::fmod(hue / 60.0f + offset[k], 6.0f);
        rgb[k] = val * (1.0f - sat * std::clamp(std::min(n, 4.0f - n), 0.0f, 1.0f));
    }
}

void rgbHsv(const float rgb[3], float &hue, float &sat, float &val)
{
    const float mx = std::max({rgb[0], rgb[1], rgb[2]}), mn = std::min({rgb[0], rgb[1], rgb[2]}), d = mx - mn;
    val = mx;
    sat = mx > 0 ? d / mx : 0;
    if (d <= 0)
        hue = 0;
    else if (mx == rgb[0])
        hue = 60.0f * std::fmod((rgb[1] - rgb[2]) / d + 6.0f, 6.0f);
    else if (mx == rgb[1])
        hue = 60.0f * ((rgb[2] - rgb[0]) / d + 2.0f);
    else
        hue = 60.0f * ((rgb[0] - rgb[1]) / d + 4.0f);
}

float volumeGain(const Settings &s)
{
    const float v = std::clamp(s.volume, 0.0f, 1.5f);
    if (!s.logVolume)
        return v;
    if (v <= 1.0f) {
        const float g = std::pow(10.0f, 50.0f * (v - 1.0f) / 20.0f);
        return v < 0.02f ? g * v / 0.02f : g; // the last 2% fade to true silence
    }
    return std::pow(10.0f, 3.52f * (v - 1.0f) / 0.5f / 20.0f); // 1.5 -> +3.52 dB = 1.5x
}

std::string volumeText(const Settings &s)
{
    std::string out = std::to_string(int(std::lround(s.volume * 100))) + "%";
    if (s.logVolume) {
        const float g = volumeGain(s);
        char db[32];
        std::snprintf(db, sizeof db, g <= 0.0f ? " (silent)" : " (%+.0f dB)", double(20.0f * std::log10(std::max(g, 1e-6f))));
        out += db;
    }
    return out;
}

UiTheme uiTheme(const Settings &s)
{
    UiTheme t{};
    float *out[4] = {t.accent, t.text, t.bg, t.highlight};
    for (int n = 0; n < 4; n++) { // in order: later colours may refer to the accent / text
        const UiColour &c = colourOf(const_cast<Settings &>(s), n);
        const auto &choices = colourKinds()[n].choices;
        const int ch = std::clamp(c.choice, 0, int(choices.size()) - 1);
        float rgb[3];
        if (ch == int(choices.size()) - 1) {
            hsvRgb(c.hue, c.sat, c.val, rgb);
        } else {
            const float *src = choices[size_t(ch)].rgb;
            if (src[0] == -4)
                std::copy(ui::style(s.uiStyle).defaults[n], ui::style(s.uiStyle).defaults[n] + 3, rgb);
            else if (src[0] == -1 || src[0] == -2)
                std::copy(src[0] == -1 ? t.accent : t.text, (src[0] == -1 ? t.accent : t.text) + 3, rgb);
            else if (src[0] == -3)
                for (int k = 0; k < 3; k++)
                    rgb[k] = t.accent[k] * 0.07f + 0.012f;
            else
                std::copy(src, src + 3, rgb);
        }
        std::copy(rgb, rgb + 3, out[n]);
    }
    t.opacity = s.uiOpacity;
    // All four colours left on "Theme": the theme is trusted as designed, no corrections at all. (The palette from the
    // visuals doesn't come through here; it sets `readable` from the setting alone.)
    t.readable = s.uiReadable && (s.uiAccent.choice != 0 || s.uiText.choice != 0 || s.uiBackground.choice != 0 ||
                                  s.uiHighlight.choice != 0);
    return t;
}

// Type and accepted values of a settings.ini key (for "Show valid values"). Prefix keys end in '.'.
static std::pair<std::string, std::string> keyTypeRange(const std::string &key)
{
    static const std::map<std::string, std::pair<const char *, const char *>> table = {
        {"preset", {"string", "a preset name"}}, {"random_presets", {"bool", "0-1"}}, {"gain", {"float", "0.25-12"}},
        {"volume", {"float", "0-1.5"}}, {"shuffle", {"bool", "0-1"}}, {"shuffle_gap", {"float", "0-0.9"}},
        {"repeat", {"int", "0-2 (off, playlist, one song)"}}, {"wheel_volume", {"bool", "0-1"}},
        {"ui_scale", {"float", "0.5-2"}}, {"info_position", {"int", "0-3 (corners)"}},
        {"controls_position", {"int", "0-2"}}, {"preset_bar_position", {"int", "0-1 (top, bottom)"}},
        {"info_scale", {"float", "0.5-2.5"}}, {"controls_scale", {"float", "0.5-2.5"}},
        {"preset_bar_scale", {"float", "0.5-2.5"}}, {"controls_vertical", {"bool", "0-1"}},
        {"info_xy", {"x,y", "-1 (auto) or 0-1 each"}}, {"controls_xy", {"x,y", "-1 (auto) or 0-1 each"}},
        {"preset_bar_xy", {"x,y", "-1 (auto) or 0-1 each"}}, {"display_mode", {"int", "0-2 (window, borderless, exclusive)"}},
        {"fullscreen_type", {"int", "1-2"}}, {"hdr", {"int", "0-2 (auto, on, off)"}}, {"hdr_boost", {"float", "1-8"}},
        {"info_always", {"bool", "0-1"}}, {"on_screen_controls", {"bool", "0-1"}}, {"debug_key", {"bool", "0-1"}},
        {"debug_spectrum", {"bool", "0-1"}}, {"verbose_log", {"bool", "0-1"}}, {"album_colours", {"bool", "0-1"}},
        {"album_art_mode", {"int", "0-2 (off, auto, always)"}}, {"log_volume", {"bool", "0-1"}},
        {"menu_tall", {"bool", "0-1"}}, {"dither", {"int", "0-3 (off, low, medium, high)"}},
        {"perf_overlay", {"int", "0-2"}}, {"perf_window", {"float", "1-60"}},
        {"resource_overlay", {"int", "0-2"}}, {"hardware_panel", {"bool", "0-1"}}, {"dump_hardware", {"bool", "0-1"}},
        {"dump_software", {"bool", "0-1"}}, {"dump_redact", {"bool", "0-1"}}, {"show_config_keys", {"bool", "0-1"}},
        {"show_config_values", {"bool", "0-1"}}, {"show_config_ranges", {"bool", "0-1"}}, {"blur_hq", {"bool", "0-1"}},
        {"ui_from_visuals", {"bool", "0-1"}}, {"ui_readable", {"bool", "0-1"}}, {"ui_theme", {"string", "a theme's name (PS3, Classic 95-style, Metro-style, Glossy Aero-style, Material-style, Brushed Metal-style, Neumorphism, Flat-style, Synthwave, Brutalist, Terminal-style, Adwaita-style or a user theme)"}}, {"ui_opacity", {"float", "0.3-1.3"}}, {"song_fades", {"bool", "0-1"}},
        {"info_crossfade", {"bool", "0-1"}}, {"fade_seconds", {"float", "0.01-10"}}, {"pause_mode", {"int", "0-2 (glide, stop, keep moving)"}},
        {"wave_height", {"float", "0.1-4"}}, {"line_width", {"float", "0.1-4"}}, {"speed", {"float", "0.1-3"}},
        {"blur", {"float", "0-3"}}, {"glow", {"float", "0-3"}}, {"fov", {"float", "0.4-2"}}, {"blur_div", {"int", "1, 2, 4, 8"}},
        {"drift", {"float", "0-3"}}, {"drift_speed", {"float", "0.05-4"}}, {"camera_motion", {"bool", "0-1"}},
        {"camera_smooth", {"float", "0-5"}}, {"keep_above_terrain", {"bool", "0-1"}}, {"sync_ms", {"float", "-500-500"}},
        {"spectrum", {"int", "0-2 (PS3, PS3 smooth bass, enhanced)"}}, {"falling_tiles_mode", {"int", "0-2 (off, auto, on)"}},
        {"flow", {"int", "0-2 (off, family, all)"}}, {"flow_seconds", {"float", "10-300"}}, {"flow_morph", {"float", "0.5-30"}},
        {"custom_in_rotation", {"bool", "0-1"}}, {"controllers", {"bool", "0-1"}}, {"file_browser", {"int", "0-2 (system, built-in, auto)"}},
        {"browse_dir", {"path", "a folder"}}, {"pad_glyphs", {"int", "0-3 (auto, Xbox, PlayStation, Steam)"}},
        {"eq_enabled", {"bool", "0-1"}}, {"eq_target", {"int", "0-2 (audio, audio + visualizer, visualizer)"}},
        {"eq_preamp", {"float", "-12-12 dB"}}, {"eq_bands", {"float list", "10 values, -12-12 dB each"}},
        {"ui_accent", {"int", "0-8 (8 = custom)"}}, {"ui_text", {"int", "0-4 (4 = custom)"}}, {"ui_bg", {"int", "0-4 (4 = custom)"}},
        {"ui_highlight", {"int", "0-3 (3 = custom)"}}, {"visualizer", {"int", "0-1 (waves, earth)"}},
        {"earth_view", {"string", "a view name: 0, 0a, 0b, 1 ... 9b"}}, {"earth_next_view", {"bool", "0-1"}},
        {"earth_flight_speed", {"float", "0.1-4"}}, {"earth_clouds", {"bool", "0-1"}}, {"earth_cloud_speed", {"float", "0-10"}},
        {"earth_stars", {"float", "0-3"}}, {"earth_glare", {"float", "0-3"}}, {"earth_exposure", {"float", "0.25-3"}},
        {"earth_music", {"int", "0-2 (off, gentle, strong)"}}, {"earth_firmware_shaders", {"bool", "0-1"}},
        {"earth_real_time", {"int", "0-2 (off, turn the earth, move the sun)"}},
        {"earth_anti_alias", {"int", "0-3 (off, 2x, 4x, 8x)"}}};
    if (auto it = table.find(key); it != table.end())
        return {it->second.first, it->second.second};
    for (const char *colour : {"ui_accent", "ui_text", "ui_bg", "ui_highlight"}) {
        const std::string c = colour;
        if (key == c + "_hue")
            return {"float", "0-360 (degrees)"};
        if (key == c + "_sat" || key == c + "_val")
            return {"float", "0-1"};
    }
    if (key.rfind("custom.", 0) == 0)
        return {"float", "any (the firmware parameter)"};
    if (key.rfind("key.", 0) == 0)
        return {"key:mods,key:mods", "SDL key codes; mods 1 Shift, 2 Ctrl, 4 Alt"};
    if (key.rfind("pad.", 0) == 0)
        return {"button", "south, east, west, north, back, start, leftshoulder, rightshoulder, lefttrigger, righttrigger, "
                          "dpup, dpdown, dpleft, dpright, leftstick, rightstick, none"};
    return {"", ""};
}

// The settings.ini key(s) behind a menu row, for the "Show settings keys in tooltips" option. Empty: none.
static std::string rowConfigKeys(int tab, const std::string &rawLabel, const Menu::Context &c)
{
    static const std::map<std::string, const char *> keys = {
        {"Preset", "preset"}, {"Preset flow", "flow"}, {"Flow interval", "flow_seconds"},
        {"Flow transition", "flow_morph"}, {"New preset for every track", "random_presets"},
        {"Falling tiles effect", "falling_tiles_mode"}, {"Keep camera above terrain", "keep_above_terrain"},
        {"Camera motion", "camera_motion"}, {"Drift distance", "drift"}, {"Drift speed", "drift_speed"},
        {"Camera smoothing", "camera_smooth"}, {"Field of view", "fov"}, {"Music response", "spectrum"},
        {"Visual sync", "sync_ms"}, {"Sensitivity", "gain"}, {"Wave height", "wave_height"},
        {"Line width", "line_width"}, {"Speed", "speed"}, {"Glow", "glow"}, {"Depth of field", "blur"},
        {"Blur resolution", "blur_div"}, {"Colours from album art", "album_colours"}, {"High-quality blur", "blur_hq"},
        {"HDR", "hdr"}, {"HDR highlight boost", "hdr_boost"},
        {"Volume", "volume"}, {"Volume curve", "log_volume"}, {"Shuffle", "shuffle"},
        {"Shuffle repeat gap", "shuffle_gap"}, {"Repeat", "repeat"}, {"Song fades", "song_fades"},
        {"Fade duration", "fade_seconds"}, {"Mouse wheel", "wheel_volume"}, {"File browser", "file_browser, browse_dir"},
        {"Track info", "info_always"}, {"Album art", "album_art_mode"}, {"Crossfade track info", "info_crossfade"},
        {"Track info & art scale", "info_scale"}, {"On-screen controls", "on_screen_controls"}, {"UI scale", "ui_scale"},
        {"Track info position", "info_position, info_xy"}, {"Playback buttons position", "controls_position, controls_xy"},
        {"Vertical playback buttons", "controls_vertical"}, {"Preset bar position", "preset_bar_position, preset_bar_xy"},
        {"Arrange on-screen elements", "info_xy, controls_xy, preset_bar_xy, info_scale, controls_scale, preset_bar_scale"},
        {"UI colours from visuals", "ui_from_visuals"}, {"Theme", "ui_theme"}, {"Keep text readable", "ui_readable"}, {"Background opacity", "ui_opacity"},
        {"Controller support", "controllers"}, {"Button glyphs", "pad_glyphs"}, {"Display", "display_mode, fullscreen_type"},
        {"Menu size", "menu_tall"}, {"Dithering", "dither"},
        {"Debug spectrum", "debug_spectrum"}, {"Frame rate display", "perf_overlay"}, {"Frame stats window", "perf_window"},
        {"Resource usage display", "resource_overlay"}, {"Show settings keys in tooltips", "show_config_keys"},
        {"Extensive logging", "verbose_log"}, {"Debug dump key", "debug_key"}, {"Dump: hardware details", "dump_hardware"},
        {"Dump: software details", "dump_software"}, {"Dump: hide personal info", "dump_redact"},
        {"Equalizer", "eq_enabled"}, {"Applies to", "eq_target"}, {"Preamp", "eq_preamp"}, {"Reset to flat", "eq_bands, eq_preamp"},
        {"Include in preset rotation", "custom_in_rotation"}, {"When paused", "pause_mode"},
        {"Show current values", "show_config_values"}, {"Show valid values", "show_config_ranges"},
        {"Hardware info panel", "hardware_panel"}, {"Visualizer", "visualizer"}, {"View", "earth_view"},
        {"Next view after each flight", "earth_next_view"}, {"Flight speed", "earth_flight_speed"},
        {"Clouds", "earth_clouds"}, {"Cloud drift", "earth_cloud_speed"}, {"Stars", "earth_stars"},
        {"Sun glare", "earth_glare"}, {"Exposure", "earth_exposure"}, {"React to music", "earth_music"}, {"Shaders", "earth_firmware_shaders"}, {"Real time", "earth_real_time"}, {"Anti-aliasing", "earth_anti_alias"}};
    static const char *const stress[] = {"Frame rate limit", "Visualizer update rate", "Render passes per update", "Render scale",
                                         "Preset storm", "Seek storm", "Random frame stalls", "Background CPU load"};
    std::string label = rawLabel;
    label.erase(0, label.find_first_not_of(' '));
    for (const char *st : stress)
        if (label == st)
            return "(not saved: a stress test)";
    // Colours: "<Kind> colour / hue / saturation / brightness" -> ui_<kind>[_hue|_sat|_val].
    for (auto [what, key] : {std::pair{"Accent", "ui_accent"}, std::pair{"Text", "ui_text"},
                             std::pair{"Background", "ui_bg"}, std::pair{"Highlight", "ui_highlight"}})
        for (auto [part, suffix] : {std::pair{" colour", ""}, std::pair{" hue", "_hue"}, std::pair{" saturation", "_sat"},
                                    std::pair{" brightness", "_val"}})
            if (label == std::string(what) + part)
                return std::string(key) + suffix;
    if (tab == 4 && label.size() > 3 && label.compare(label.size() - 2, 2, "Hz") == 0) {
        static const char *bands[] = {"31", "62", "125", "250", "500", "1", "2", "4", "8", "16"};
        for (int b = 0; b < 10; b++)
            if (label.rfind(bands[b], 0) == 0)
                return "eq_bands (value " + std::to_string(b + 1) + " of 10)";
        return "eq_bands";
    }
    if (tab == 3) { // Custom preset: its parameters
        const auto &names = c.canyon.paramNames();
        if (std::find(names.begin(), names.end(), label) != names.end())
            return "custom." + label;
    }
    const auto it = keys.find(label);
    return it == keys.end() ? std::string() : std::string(it->second);
}

// Tooltip text for a menu row (shown after hovering it for a second). Null: no tooltip.
static const char *rowHelp(int tab, const std::string &label)
{
    static const std::map<std::string, const char *> help = {
        // Visualizer
        {"Preset", "The current look of the canyon. Left/Right switches to the previous or next preset."},
        {"Preset flow", "Changes the preset by itself every so often: only to presets of the same colour family, or to any preset."},
        {"Flow interval", "How long each preset stays before Preset flow moves on to the next one."},
        {"Flow transition", "How long the blend from one preset to the next takes when Preset flow changes it."},
        {"New preset for every track", "Picks a random preset whenever a new track starts."},
        {"Keep camera above terrain", "Stops the camera from dipping into the canyon walls when the music gets loud."},
        {"Camera motion", "Lets the camera sway and drift like on the PS3. Off keeps it still."},
        {"Drift distance", "How far the camera wanders from its path."},
        {"Drift speed", "How fast the camera wanders."},
        {"Camera smoothing", "Smooths out sudden camera movements. Off is the original, more twitchy movement."},
        {"Falling tiles effect", "The blocks that rain down on strong beats. Auto uses the preset's own setting."},
        {"Music response", "How the music is turned into the canyon shape. PS3 copies the console exactly; smooth bass "
                           "calms very deep bass; Enhanced uses a finer, more detailed analysis."},
        {"Visual sync", "Shifts the picture earlier (-) or later (+) against the sound, if the two feel out of step "
                        "(for example with Bluetooth headphones)."},
        {"Sensitivity", "How strongly the canyon reacts to the music. Raise it for quiet tracks."},
        {"Wave height", "Height of the canyon walls."},
        {"Line width", "Thickness of the glowing lines."},
        {"Glow", "Strength of the bloom around bright lines. Off disables it."},
        {"Depth of field", "Blurs the far distance like a camera lens. Off keeps everything sharp."},
        {"Blur resolution", "Quality of the blur effects. Lower resolutions are faster on weak graphics cards."},
        {"Field of view", "How wide the camera sees. Higher values give a wider, more distorted view."},
        {"Speed", "How fast you fly through the canyon."},
        {"Colours from album art", "Picks the colours from the track's cover picture. Tracks without a cover keep the "
                                   "preset's own colours."},
        {"Reset visual settings", "Puts the chosen visualizer's settings on this tab back to their defaults."},
        {"HDR", "High dynamic range output for HDR screens: brighter highlights. Auto turns it on when the screen "
                "supports it; the text after the colon tells what is in use right now."},
        {"HDR highlight boost", "How much brighter than normal white the brightest lines get in HDR."},
        // Misc
        {"Volume", "Playback volume. Hold Shift for 1% steps (also with the arrow keys and the mouse wheel)."},
        {"Volume curve", "Linear changes the loudness by the same amount per step, which feels jumpy at the low "
                         "end. Logarithmic makes every step sound equally big (in decibels), giving finer control "
                         "of quiet levels."},
        {"Shuffle", "Plays the playlist in random order. Previous goes back to the track that played before."},
        {"Repeat", "Off stops after the last track, Whole playlist starts it over from the top, One song plays the current track again and again."},
        {"Song fades", "Crossfades into the next track instead of cutting over: the new track fades in while the old "
                       "one fades out."},
        {"Fade duration", "Length of the crossfade between tracks."},
        {"Mouse wheel", "Lets the mouse wheel change the volume while the menu is closed."},
        {"Track info", "Auto shows the title for a few seconds when a track starts or the mouse moves; Always keeps it "
                       "on screen."},
        {"Album art", "The cover picture next to the track info. Auto shows and hides it together with the track "
                      "info; Always keeps it on screen."},
        {"Crossfade track info", "During a song crossfade, fades the old title and cover into the new ones instead "
                                 "of switching at once."},
        {"Track info & art scale", "Size of the track info and cover, separate from the rest of the interface."},
        {"Track info position", "Which corner the track info and cover sit in. Custom means it was dragged in the arrange mode."},
        {"On-screen controls", "The playback buttons and preset bar that fade in when the mouse nears the edge of "
                               "the window."},
        {"Playback buttons position", "Where the play, pause and skip buttons sit."},
        {"Vertical playback buttons", "Stacks the playback buttons top to bottom, which fits the left and right "
                                      "edges of the screen."},
        {"Preset bar position", "Where the preset switcher bar sits."},
        {"UI scale", "Size of the menu and every on-screen element."},
        {"Arrange on-screen elements", "Needs a mouse (greyed out while a controller is in use). Drag the track info, buttons and preset bar anywhere. The mouse wheel "
                                       "resizes the element under it; elements snap to the edges and to each other "
                                       "(hold Shift to place freely). R resets, Enter or Esc finishes."},
        {"Accent colour", "Colour of highlights: the selection marker, section titles, keycaps and the progress bar. Custom lets you pick your own."},
        {"Text colour", "Colour of the menu and on-screen text. Custom lets you pick your own."},
        {"Background colour", "Colour of the menu panel, the help screen and the on-screen control bars. Custom "
                              "lets you pick your own (keep the brightness low)."},
        {"Highlight colour", "Colour of the selected row in the menu and of the on-screen buttons (drawn see-through)."},
        {"Theme", "The interface's design: PS3 (the original look), Classic 95-style (grey 3D bevels), Metro-style "
                  "(flat tiles), Glossy Aero-style (light glass), Material-style (dark, rounded), Brushed Metal-style "
                  "(skeuomorphic), Neumorphism (soft, monochrome, shadow-lifted), Flat-style (no bevels or shadows "
                  "at all), Synthwave (neon on near-black), Brutalist (raw black and white, thick square borders), "
                  "Terminal-style (green phosphor on black) or Adwaita-style (light GNOME look), plus your own "
                  "*.theme files from the \"themes\" folder next to settings.ini (re-read "
                  "whenever this menu opens; see docs/THEMES.md). Each brings its own colours; the colour rows below "
                  "change them (\"Theme\" = the theme's own)."},
        {"Edit element", "Which part of the interface to recolour (shown while a theme of your own is chosen; for "
                         "a built-in one, use Copy theme to a file first). Changes are saved into the theme's file at "
                         "once; comments in that file are not kept."},
        {"Edit property", "Which of the element's colours: its fill, the end of its top-to-bottom gradient, its text, "
                          "border, edge line, shadow or a tint over the fill."},
        {"Element colour", "One of the colour roles (they follow the colour rows above), a fixed Custom colour (hue, "
                           "saturation and brightness rows appear), or None to leave it out."},
        {"Element opacity", "How opaque that colour is."},
        {"Element corner radius", "How round the element's corners are (UI units, about pixels at 720p)."},
        {"Element border width", "The outline's width; it needs a Border colour (Edit property)."},
        {"Element gloss", "A white sheen over the element's top half."},
        {"Element shadow blur", "How soft the drop shadow is; it needs a Shadow colour (Edit property). 0 makes a hard "
                                "shadow, shifted down."},
        {"Copy theme to a file", "Writes the current theme, with the colours chosen above, as a new .theme file in "
                                 "the \"themes\" folder next to settings.ini and switches to it. Edit the file in "
                                 "any text editor (see docs/THEMES.md); reopen this menu to see the changes."},
        {"UI colours from visuals", "Themes the whole interface from what's on screen: the album art's colours when "
                                    "the track has a cover, otherwise the current preset's colours. It follows "
                                    "preset changes with a smooth fade."},
        {"Keep text readable", "Lightens or darkens text where its colour would be hard to read on what's behind it "
                               "(custom colours, colours from the visuals or album art). Only as much as needed, so the "
                               "colour keeps its hue. With all colours on Theme and UI colours from visuals off, the "
                               "theme is used exactly as designed. Off: colours are used exactly as chosen."},
        {"Background opacity", "How see-through the menu panel and the control bars are."},
        {"Display", "Window, borderless fullscreen or exclusive fullscreen."},
        // Debug
        {"Debug dump key", "Lets a key save a debug dump (a zip with the log, settings and a screenshot) for "
                           "reporting problems."},
        {"Debug spectrum", "Draws the music analysis the canyon is fed with on top of the picture."},
        {"Frame rate limit", "Unlocked turns vsync off so frames are drawn as fast as possible (tearing is "
                             "normal); the caps limit it instead. Stress test: not saved."},
        {"Visualizer update rate", "How often the visualizer itself steps. The PS3 runs it 60 times a second and its "
                                   "trails are tuned for that, so faster rates change the look. Stress test: not saved."},
        {"Render passes per update", "Renders the visualizer this many times per step (the extra results are thrown "
                                     "away, though trails build up faster). Pure graphics card load. Stress test: not saved."},
        {"Render scale", "Renders the visualizer at this share of the window resolution (above 100% = supersampling, "
                         "heavy on the graphics card and its memory). Stress test: not saved."},
        {"Preset storm", "Keeps switching presets on its own, to stress the preset blending. Stress test: not saved."},
        {"Seek storm", "Jumps to random points in the song, to stress decoding and the visualizer's sync. "
                       "Stress test: not saved."},
        {"Random frame stalls", "Pauses the main thread for a random time each frame, like a busy or slow system. "
                                "Stress test: not saved."},
        {"Background CPU load", "Keeps CPU cores busy in the background, to check that the audio and visuals cope. "
                                "Stress test: not saved."},
        {"Stop all stress tests", "Turns every stress test above off again (they also reset when the player restarts)."},
        {"Show settings keys in tooltips", "Adds the name each option has in settings.ini under its tooltip, for "
                                           "editing the file by hand or reading a debug dump."},
        {"Show current values", "With the settings keys shown: adds each key's current value and its type, e.g. "
                                "<int> 1 (only works while Show settings keys in tooltips is on)."},
        {"Show valid values", "With the settings keys shown: adds what the key accepts, e.g. 0-2, 0.25-12 or the "
                              "list of names (only works while Show settings keys in tooltips is on)."},
        {"Hardware info panel", "A panel listing the hardware in use: CPU, memory, graphics card and API, display, "
                                "audio output and controller (refreshed every 2 s)."},
        {"When paused", "What the visualizer does while the music is paused: Glide to a stop slows the flight "
                        "smoothly to a halt (and speeds up again on play), Stop at once freezes the picture, Keep "
                        "moving carries on flying over the last sound."},
        {"Visualizer", "Which PS3 visualizer to show: the wave canyon, or the Earth seen from orbit. The settings "
                       "below change with it."},
        {"View", "Which of the PS3's Earth flights to show (each is a slow camera flight of a few minutes)."},
        {"Next view after each flight", "When a flight ends, move on to the next view instead of flying the same one "
                                        "again."},
        {"Flight speed", "How fast the camera flies along its path."},
        {"Clouds", "The cloud layer (and its shadows)."},
        {"Shaders", "Lookalike: our own shaders tuned to look like the PS3 (fast). Firmware (hybrid): the PS3's own "
                    "Earth surface shader, fed with the PS3's light table and an atmosphere table we compute; "
                    "the outer glow, sun and tone mapping stay ours. Clouds don't drift with it."},
        {"Anti-aliasing", "Smooths the planet's jagged edge (multisampling). Higher costs more GPU time; the "
                          "graphics card may cap it."},
        {"Real time", "Day and night where they are right now (from the clock, UTC). Turn the Earth: each view keeps "
                      "its lighting and the planet is turned under it. Move the sun: the sun goes where it really is, "
                      "like the PS3's clock Earth in the XMB (views can then be dark)."},
        {"Cloud drift", "How fast the clouds move around the planet. 0 keeps them still."},
        {"Stars", "Brightness of the stars. Off hides them."},
        {"Sun glare", "Strength of the sun's glow, rays and bloom."},
        {"Exposure", "Overall brightness, on top of each view's own."},
        {"React to music", "The PS3's Earth doesn't react to music. Gentle makes the light and the sun's glare pulse "
                           "a little with the bass. Strong also catches the beat: on each kick the atmosphere's glow "
                           "swells and springs back with a small bounce."},
        {"Frame rate display", "Shows the display's refresh rate, the frame rate and the time per frame in the top "
                               "right corner, optionally with a graph of recent frame times (spikes = stutters)."},
        {"Frame stats window", "How far back the frame rate display's 1% low, minimum and maximum fps look. The 1% "
                               "low is the average frame rate of the slowest 1% of frames: it shows stutter that the "
                               "average hides."},
        {"Resource usage display", "Shows the player's CPU use, memory, disk/file reads and writes and thread count, "
                                   "optionally with graphs of the last half minute."},
        {"Dump: hardware details", "Adds the CPU, graphics card, memory, displays and audio devices to debug dumps."},
        {"Dump: software details", "Adds the operating system, desktop, drivers and library versions to debug dumps."},
        {"Dump: hide personal info", "Removes your user name, computer name, IP and MAC addresses and e-mail addresses "
                                     "from debug dumps (paths in your home folder become ~). Leave it on unless "
                                     "asked otherwise."},
        {"Shuffle repeat gap", "With shuffle on, a track that just played waits until this share of the playlist has "
                               "played before it can come up again. 0% only avoids playing the same track twice in a "
                               "row. Shift: 1% steps."},
        {"High-quality blur", "Runs the depth-of-field blur at full resolution with as many passes as needed to keep "
                              "the same blur size as Blur resolution would give: smooth instead of blocky, but "
                              "much heavier on the graphics card."},
        {"File browser", "What Add files / Add folder in the Playlist tab open: the system's file dialogs, or a "
                         "simple browser inside the menu (easy with a controller or on a Steam Deck). Auto uses the "
                         "built-in one while a controller is in use."},
        {"Controller support", "Lets a game controller (Xbox, PlayStation, Steam and most others) run the player and "
                               "the menu. Keyboards that pretend to be a controller are recognised and skipped. The "
                               "value shows the controller in use, or why none is."},
        {"Button glyphs", "Which button pictures to show while you use a controller. Auto picks them from the "
                          "controller itself."},
        {"Dithering", "Adds a faint, fixed grain (smaller than one colour step) to the picture so smooth "
                      "gradients like glows and fog don't show colour bands. Higher hides stronger banding but the "
                      "grain gets easier to see. Off shows the picture untouched."},
        {"Menu size", "Tall uses the whole screen height except 15% at the top and bottom, to show more rows at "
                      "once. It makes a difference with a UI scale below 1 (at 1 the menu is already that tall)."},
        {"Extensive logging", "Writes far more detail to the log. Turn it on before reproducing a problem, then "
                              "save a debug dump. It can slow things down a little."},
        // Equalizer
        {"Equalizer", "Turns the 10-band equalizer on or off."},
        {"Applies to", "What the equalizer changes: the sound you hear, the sound and the visualizer, or only what "
                       "the visualizer sees."},
        {"Preamp", "Overall gain before the bands. Lower it if boosting bands makes the sound distort."},
        {"Reset to flat", "Sets every band and the preamp back to 0 dB."},
        // Custom preset
        {"Include in preset rotation", "Lets Preset flow and random presets pick the custom preset too."},
        // Presets library
        {"Save custom preset to the list", "Adds the current custom preset to the library below."},
        {"Import from clipboard", "Adds a preset that someone shared (the PS3VIS-PRESET1 text) from the clipboard."},
        {"Import from file...", "Adds presets from .ps3vis files."},
        {"Export custom preset to clipboard", "Copies the custom preset as shareable text."},
        {"Export custom preset to file...", "Saves the custom preset as a .ps3vis file."},
    };
    auto endsWith = [&](const char *end) {
        const size_t n = std::strlen(end);
        return label.size() > n && label.compare(label.size() - n, n, end) == 0;
    };
    if (tab == 5 && endsWith(" hue"))
        return "Click or drag along the strip to pick the colour (Left/Right nudges it).";
    if (tab == 5 && endsWith(" saturation"))
        return "How strong the colour is: from grey/white on the left to the full colour on the right.";
    if (tab == 5 && endsWith(" brightness"))
        return "How light the colour is: from black on the left to full brightness on the right.";
    if (tab == 4 && label.size() > 3 && label.compare(label.size() - 2, 2, "Hz") == 0)
        return "Boosts (+) or lowers (-) this frequency range. Low numbers are bass, high numbers treble.";
    if (tab == 3 && label.rfind("Copy values from ", 0) == 0)
        return "Starts the custom preset from the values of the built-in preset shown last.";
    if (tab == 3 && (label == "Use custom preset now" || label == "Custom preset is active"))
        return "Switches the visualizer to the custom preset. Edits below show live while it is active.";
    const auto it = help.find(label);
    return it == help.end() ? nullptr : it->second;
}

// The Visualizer, Misc and Debug tabs share settingsRows(); each shows its own rows, grouped
// under section headers in this order (rows not listed go last).
// ---------------------------------------------------------------- built-in file browser

void Menu::openBrowser(Context &c)
{
    browsing_ = true;
    browsePlaces_ = false;
    std::error_code ec;
    fs::path start = pathFromUtf8(c.settings.browseDir);
    if (c.settings.browseDir.empty() || !fs::is_directory(start, ec)) {
        const char *music = SDL_GetUserFolder(SDL_FOLDER_MUSIC), *home = SDL_GetUserFolder(SDL_FOLDER_HOME);
        start = pathFromUtf8(music && fs::is_directory(pathFromUtf8(music), ec) ? music : home ? home : ".");
    }
    browseTo(start, c);
}

void Menu::browseTo(const fs::path &dir, Context &c)
{
    browsePlaces_ = false;
    browseError_.clear();
    browseEntries_.clear();
    std::error_code ec;
    const fs::path where = fs::weakly_canonical(dir, ec);
    browseDir_ = ec ? dir : where;
    // Folders and playable files; hidden ones skipped; unreadable entries ignored; at most 5000.
    for (auto it = fs::directory_iterator(browseDir_, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator() && browseEntries_.size() < 5000; it.increment(ec)) {
        const std::string name = utf8(it->path().filename());
        if (name.empty() || name[0] == '.')
            continue;
        std::error_code e2;
        const bool dir = it->is_directory(e2);
        if (e2 || (!dir && !(it->is_regular_file(e2) && isAudioFile(it->path()))))
            continue;
        browseEntries_.push_back({name, it->path(), dir});
    }
    if (ec)
        browseError_ = "Can't open this folder: " + ec.message();
    auto lower = [](std::string t) {
        for (char &ch : t)
            ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return t;
    };
    std::sort(browseEntries_.begin(), browseEntries_.end(), [&](const BrowseEntry &a, const BrowseEntry &b) {
        return a.dir != b.dir ? a.dir : lower(a.name) < lower(b.name); // folders first
    });
    c.settings.browseDir = utf8(browseDir_);
    c.changed();
    sel_ = 0;
    skipHeader(c, 1);
    scroll_ = 0; // (keep the folder's path, the first row, in view)
}

std::vector<Menu::Row> Menu::browserRows(Context &c)
{
    std::vector<Row> rows;
    auto header = [&rows](const std::string &title) {
        Row h{title, "", nullptr, nullptr};
        h.header = true;
        rows.push_back(h);
    };
    if (browsePlaces_) {
        header("Places");
        auto place = [&](const std::string &label, const fs::path &p) {
            std::error_code ec;
            if (fs::is_directory(p, ec))
                rows.push_back({label, utf8(p), nullptr, [this, &c, p] { browseTo(p, c); }});
        };
        for (auto [label, folder] : {std::pair{"Home", SDL_FOLDER_HOME}, std::pair{"Music", SDL_FOLDER_MUSIC},
                                     std::pair{"Desktop", SDL_FOLDER_DESKTOP}, std::pair{"Downloads", SDL_FOLDER_DOWNLOADS}})
            if (const char *p = SDL_GetUserFolder(folder))
                place(label, pathFromUtf8(p));
#if defined(_WIN32)
        for (char d = 'A'; d <= 'Z'; d++) { // drives
            const std::string root = std::string(1, d) + ":/";
            std::error_code ec;
            if (fs::exists(root, ec))
                place(std::string("Drive ") + d + ":", root);
        }
#else
        // Mounted drives and SD cards (/run/media/<user>/..., /media/..., /mnt/...).
        const char *user = std::getenv("USER");
        for (const fs::path &base : {fs::path("/run/media") / (user ? user : ""), fs::path("/media") / (user ? user : ""),
                                    fs::path("/media"), fs::path("/mnt")}) {
            std::error_code ec;
            for (auto it = fs::directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::directory_iterator(); it.increment(ec))
                if (std::error_code e2; it->is_directory(e2) && it->path().filename() != (user ? user : ""))
                    place("Drive " + utf8(it->path().filename()), it->path());
        }
        place("Computer (/)", "/");
#endif
        rows.push_back({"Back to the folder", "", nullptr, [this, &c] { browseTo(browseDir_, c); }});
        return rows;
    }
    std::string title = utf8(browseDir_);
    if (title.size() > 70)
        title = "..." + title.substr(title.size() - 67);
    header(title);
    rows.push_back({"Places...", "home, music, drives", nullptr, [this] {
                        browsePlaces_ = true;
                        sel_ = 1; // the first place (row 0 is the heading)
                        scroll_ = 0;
                    }});
    if (browseDir_.has_parent_path() && browseDir_.parent_path() != browseDir_)
        rows.push_back({"Up one folder", "", nullptr, [this, &c] { browseTo(browseDir_.parent_path(), c); }});
    int files = 0;
    for (auto &e : browseEntries_)
        files += !e.dir;
    rows.push_back({"Add everything in this folder", files ? std::to_string(files) + " here + subfolders" : "subfolders",
                    nullptr, [&c, dir = browseDir_] {
                        if (c.addPaths)
                            c.addPaths({utf8(dir)});
                    }});
    if (!browseError_.empty())
        header(browseError_);
    else if (browseEntries_.empty())
        header("No folders or music files here");
    for (auto &e : browseEntries_) {
        if (e.dir)
            rows.push_back({"   " + e.name + "/", "", nullptr, [this, &c, p = e.path] { browseTo(p, c); }});
        else
            rows.push_back({"   " + e.name, "", nullptr, [&c, p = e.path] {
                                if (c.addPaths)
                                    c.addPaths({utf8(p)});
                            }});
    }
    return rows;
}

// Labels of the Colours section, in order.
static std::vector<std::string> colourLabels()
{
    std::vector<std::string> out = {"Theme", "UI colours from visuals"};
    for (const char *what : {"Accent", "Text", "Background", "Highlight"})
        for (const char *part : {" colour", " hue", " saturation", " brightness"})
            out.push_back(std::string(what) + part);
    out.push_back("Keep text readable");
    out.push_back("Background opacity");
    out.push_back("Copy theme to a file");
    return out;
}

// Labels of the Edit theme section (only there while a user theme is chosen).
static std::vector<std::string> editThemeLabels()
{
    return {"Edit element", "Edit property", "Element colour", "Element hue", "Element saturation", "Element brightness",
            "Element opacity", "Element corner radius", "Element border width", "Element gloss", "Element shadow blur"};
}

std::vector<Menu::Row> Menu::rowsFor(Context &c)
{
    if (browsing_)
        return browserRows(c);
    if (tab_ == 3)
        return customRows(c);
    if (tab_ == 4)
        return eqRows(c);
    if (tab_ == 7)
        return presetRows(c);
    using Section = std::pair<const char *, std::vector<std::string>>;
    static const std::vector<Section> earthSections = {
        {"", {"Visualizer"}},
        {"Flight", {"View", "Next view after each flight", "Flight speed", "Real time"}},
        {"Look", {"Shaders", "Anti-aliasing", "Clouds", "Cloud drift", "Stars", "Sun glare", "Exposure"}},
        {"Music", {"React to music"}},
        {"HDR", {"HDR", "HDR highlight boost"}},
        {"", {"Reset visual settings"}},
    };
    static const std::vector<Section> visualizer = {
        {"", {"Visualizer"}},
        {"Presets", {"Preset", "Preset flow", "Flow interval", "Flow transition", "New preset for every track",
                     "Falling tiles effect"}},
        {"Camera", {"Keep camera above terrain", "Camera motion", "Drift distance", "Drift speed", "Camera smoothing",
                    "Field of view"}},
        {"Music", {"Music response", "Visual sync", "Sensitivity"}},
        {"Look", {"Wave height", "Line width", "Speed", "Glow", "Depth of field", "Blur resolution",
                  "Colours from album art", "High-quality blur"}},
        {"HDR", {"HDR", "HDR highlight boost"}},
        {"", {"Reset visual settings"}},
    };
    static const std::vector<Section> misc = {
        {"Playback", {"Volume", "Volume curve", "Shuffle", "Shuffle repeat gap", "Repeat", "Song fades", "Fade duration", "When paused", "Mouse wheel", "File browser"}},
        {"Track info & album art", {"Track info", "Album art", "Crossfade track info", "Track info & art scale"}},
        {"On-screen layout", {"On-screen controls", "UI scale", "Track info position", "Playback buttons position",
                              "Vertical playback buttons", "Preset bar position", "Arrange on-screen elements"}},
        {"Colours", colourLabels()},
        {"Edit theme", editThemeLabels()},
        {"Controller", {"Controller support", "Button glyphs"}},
        {"Window", {"Display", "Menu size", "Dithering"}},
    };
    static const std::vector<Section> debug = {
        {"Displays", {"Debug spectrum", "Frame rate display", "Frame stats window", "Resource usage display",
                      "Hardware info panel"}},
        {"Stress tests (not saved)", {"Frame rate limit", "Visualizer update rate", "Render passes per update",
                                      "Render scale", "Preset storm", "Seek storm", "Random frame stalls",
                                      "Background CPU load", "Stop all stress tests"}},
        {"Logging & debug dumps", {"Show settings keys in tooltips", "Show current values", "Show valid values",
                                   "Extensive logging", "Debug dump key", "Dump: hardware details",
                                   "Dump: software details", "Dump: hide personal info"}},
    };
    const bool earthMode = c.settings.visualizer == 1;
    const auto &sections = tab_ == 5 ? misc : tab_ == 6 ? debug : earthMode ? earthSections : visualizer;
    auto all = settingsRows(c);
    // Which tab owns a row: listed in misc/debug, else the visualizer.
    auto owner = [&](const std::string &label) {
        for (auto *list : {&misc, &debug})
            for (auto &sec : *list)
                if (std::find(sec.second.begin(), sec.second.end(), label) != sec.second.end())
                    return list == &misc ? 5 : 6;
        return 0;
    };
    std::vector<Row> out;
    std::vector<bool> used(all.size(), false);
    for (auto &[title, labels] : sections) {
        bool headerDone = false;
        for (auto &l : labels)
            for (size_t r = 0; r < all.size(); r++)
                if (!used[r] && all[r].label == l) {
                    if (!headerDone && *title) {
                        Row hdr{title, "", nullptr, nullptr};
                        hdr.header = true;
                        out.push_back(hdr);
                    }
                    headerDone = true;
                    used[r] = true;
                    out.push_back(all[r]);
                }
    }
    // Anything not categorised yet (the Visualizer tab only lists the chosen visualizer's own rows).
    auto inList = [](const std::vector<Section> &list, const std::string &label) {
        for (auto &sec : list)
            if (std::find(sec.second.begin(), sec.second.end(), label) != sec.second.end())
                return true;
        return false;
    };
    for (size_t r = 0; r < all.size(); r++)
        if (!used[r] && owner(all[r].label) == tab_ &&
            !(tab_ == 0 && inList(earthMode ? visualizer : earthSections, all[r].label)))
            out.push_back(all[r]);
    return out;
}

// Shareable presets: import/export (clipboard or file) and the saved-preset library.
std::vector<Menu::Row> Menu::presetRows(Context &c)
{
    Settings &s = c.settings;
    std::vector<Row> rows;
    auto current = [&c, &s] {
        SharedPreset p;
        p.name = c.canyon.presets()[size_t(c.canyon.lastBuiltin())] + " (custom)";
        p.values = s.custom;
        return p;
    };
    // Loading replaces the custom preset (missing values keep their current value) and shows it.
    auto apply = [&c, &s](const SharedPreset &p) {
        for (auto &[k, v] : p.values)
            s.custom[k] = v;
        c.canyon.setPreset(c.canyon.customIndex());
        s.preset = Canyon::kCustomName;
        c.changed();
        c.notify("Loaded preset \"" + p.name + "\"");
    };
    rows.push_back({"Save custom preset to the list", "", nullptr, [&c, current] {
                        SharedPreset p = current();
                        char stamp[32];
                        const std::time_t t = std::time(nullptr);
                        std::strftime(stamp, sizeof stamp, " %Y-%m-%d %H.%M", std::localtime(&t));
                        p.name = "Custom" + std::string(stamp);
                        presetlib::save(p);
                        c.notify("Saved as \"" + p.name + "\"");
                    }});
    rows.push_back({"Import from clipboard", "", nullptr, [&c] {
                        char *text = SDL_GetClipboardText();
                        SharedPreset p;
                        const bool ok = text && presetlib::decode(text, c.canyon.paramNames(), p);
                        SDL_free(text);
                        if (!ok) {
                            c.notify("The clipboard doesn't hold a PS3 Visualizer preset");
                            return;
                        }
                        presetlib::save(p);
                        c.notify("Imported \"" + p.name + "\" (Enter on it to use it)");
                    }});
    rows.push_back({"Import from file...", "", nullptr, [&c] { c.importPresetFiles(); }});
    rows.push_back({"Export custom preset to clipboard", "", nullptr, [&c, current] {
                        SDL_SetClipboardText(presetlib::encode(current()).c_str());
                        c.notify("Custom preset copied to the clipboard");
                    }});
    rows.push_back({"Export custom preset to file...", "", nullptr, [&c] { c.exportPresetFile(); }});
    const auto saved = presetlib::list(c.canyon.paramNames());
    {
        Row hdr{saved.empty() ? "No saved presets yet" : "Saved presets", "", nullptr, nullptr};
        hdr.header = true;
        rows.push_back(hdr);
    }
    for (const auto &e : saved) {
        const int index = int(rows.size());
        Row r{"   " + e.name, "", nullptr, [&c, apply, e] {
                  SharedPreset p;
                  if (presetlib::load(e.file, c.canyon.paramNames(), p))
                      apply(p);
                  else
                      c.notify("Can't read " + e.name);
              }};
        r.remove = [this, &c, e] {
            ask({"Delete \"" + e.name + "\"?", "The preset file is removed from the presets folder. This can't be undone.",
                 "Delete", [&c, e] {
                     if (presetlib::remove(e))
                         c.notify("Deleted \"" + e.name + "\"");
                     else
                         c.notify("Couldn't delete \"" + e.name + "\"");
                 }});
        };
        r.editName = e.name;
        r.renameTo = [&c, e](const std::string &name) {
            const std::string clean = presetlib::cleanName(name);
            if (clean.empty() || clean == e.name)
                return;
            if (presetlib::rename(e, clean, c.canyon.paramNames()))
                c.notify("Renamed to \"" + clean + "\"");
            else
                c.notify("Couldn't rename \"" + e.name + "\"");
        };
        r.buttons = {{"Rename", [this, &c, index] { beginRename(index, c); }}, {"Delete", r.remove}};
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<Menu::Row> Menu::eqRows(Context &c)
{
    EqSettings &eq = c.settings.eq;
    auto changed = c.changed;
    auto db = [](float v) {
        char b[16];
        std::snprintf(b, sizeof b, "%+.0f dB", v);
        return std::string(v == 0.0f ? "0 dB" : b);
    };
    std::vector<Row> rows;
    rows.push_back({"Equalizer", eq.enabled ? "On" : "Off",
                    [&eq, changed](int) { eq.enabled = !eq.enabled; changed(); }, nullptr});
    static const char *targetNames[] = {"Audio only", "Audio and visualizer", "Visualizer only"};
    rows.push_back({"Applies to", targetNames[std::clamp(eq.target, 0, 2)], [&eq, changed](int d) {
                        eq.target = (eq.target + d + 3) % 3;
                        changed();
                    }, nullptr});
    rows.push_back({"Preamp", db(eq.preampDb), [&eq, changed](int d) {
                        eq.preampDb = std::clamp(eq.preampDb + float(d), -EqSettings::kMaxDb, EqSettings::kMaxDb);
                        changed();
                    }, nullptr});
    for (int b = 0; b < EqSettings::kBands; b++) {
        const float f = EqSettings::kFreqs[b];
        char label[32];
        if (f >= 1000)
            std::snprintf(label, sizeof label, "   %g kHz", f / 1000.0f);
        else
            std::snprintf(label, sizeof label, "   %g Hz", f);
        rows.push_back({label, db(eq.gainDb[size_t(b)]), [&eq, changed, b](int d) {
                            float &g = eq.gainDb[size_t(b)];
                            g = std::clamp(g + float(d), -EqSettings::kMaxDb, EqSettings::kMaxDb);
                            changed();
                        }, nullptr});
    }
    rows.push_back({"Reset to flat", "", nullptr, [&eq, changed] {
                        eq.preampDb = 0;
                        eq.gainDb.fill(0);
                        changed();
                    }});
    for (auto &r : rows)
        if (!r.activate && r.adjust && (r.label == "Equalizer" || r.label == "Applies to")) {
            auto adj = r.adjust;
            r.activate = [adj] { adj(1); };
        }
    return rows;
}

std::vector<Menu::Row> Menu::customRows(Context &c)
{
    Settings &s = c.settings;
    auto changed = c.changed;
    std::vector<Row> rows;
    const bool active = c.canyon.preset() == c.canyon.customIndex();
    rows.push_back({active ? "Custom preset is active" : "Use custom preset now", "", nullptr, [&c, changed] {
                        c.canyon.setPreset(c.canyon.customIndex());
                        c.settings.preset = Canyon::kCustomName;
                        changed();
                    }});
    rows.push_back({"Include in preset rotation", s.customInRotation ? "On" : "Off",
                    [&s, changed](int) { s.customInRotation = !s.customInRotation; changed(); }, nullptr});
    const std::string from = c.canyon.presets()[size_t(c.canyon.lastBuiltin())];
    rows.push_back({"Copy values from " + from, "", nullptr, [&c, &s, changed] {
                        s.custom = c.canyon.presetValues(c.canyon.lastBuiltin());
                        changed();
                    }});
    const char *group = "";
    for (const auto &spec : kParamSpecs) {
        if (std::string(group) != spec.group) {
            group = spec.group;
            Row hdr{group, "", nullptr, nullptr};
            hdr.header = true;
            rows.push_back(hdr);
        }
        const std::string name = spec.name;
        auto it = s.custom.find(name);
        const float value = it != s.custom.end() ? it->second : 0.0f;
        rows.push_back({"   " + name, num(value), [&s, changed, spec, name](int d) {
                            const bool shift = SDL_GetModState() & SDL_KMOD_SHIFT;
                            float &v = s.custom[name];
                            v = std::clamp(v + spec.step * float(d) * (shift ? 10.0f : 1.0f), spec.lo, spec.hi);
                            v = std::round(v / spec.step) * spec.step;
                            changed();
                        }, nullptr});
    }
    // Toggles also respond to Enter.
    for (auto &r : rows)
        if (!r.activate && r.adjust && (r.value == "On" || r.value == "Off")) {
            auto adj = r.adjust;
            r.activate = [adj] { adj(1); };
        }
    return rows;
}

// Keyboard selection steps over section headers.
void Menu::skipHeader(Context &c, int dir)
{
    if (!rowTab())
        return;
    const auto rows = rowsFor(c);
    const int n = int(rows.size());
    for (int guard = 0; guard < n && sel_ >= 0 && sel_ < n && rows[size_t(sel_)].header; guard++)
        sel_ += dir;
    if (sel_ < 0 || sel_ >= n) // ran off an end: step back to the nearest real row
        for (sel_ = std::clamp(sel_, 0, n - 1); sel_ >= 0 && sel_ < n && rows[size_t(sel_)].header; sel_ -= dir)
            ;
}

int Menu::rowCount(Context &c)
{
    if (tab_ == 2)
        return Keybinds::kCount + 1; // actions + "reset"
    return rowTab() ? int(rowsFor(c).size()) : kActionCount + int(c.playlist.size());
}

void Menu::clampSelection(Context &c)
{
    const int n = rowCount(c);
    sel_ = std::clamp(sel_, 0, std::max(0, n - 1));
    if (sel_ < scroll_)
        scroll_ = sel_;
    if (sel_ >= scroll_ + visible_)
        scroll_ = sel_ - visible_ + 1;
    scroll_ = std::clamp(scroll_, 0, std::max(0, n - visible_));
}

void Menu::activate(Context &c, int dir)
{
    if (rowTab()) {
        auto rows = rowsFor(c);
        if (sel_ < 0 || sel_ >= int(rows.size()))
            return;
        if (dir == 0 && rows[size_t(sel_)].activate)
            rows[size_t(sel_)].activate();
        else if (dir != 0 && rows[size_t(sel_)].adjust)
            rows[size_t(sel_)].adjust(dir);
        return;
    }
    if (tab_ == 2) {
        if (dir != 0) {
            col_ = dir < 0 ? 0 : 1;
        } else if (sel_ == Keybinds::kCount) {
            c.settings.keys.reset();
            c.changed();
        } else if (c.padActive && c.padActive()) {
            capturingPad_ = true; // the controller column is shown: wait for a button
        } else {
            capturing_ = true;
        }
        return;
    }
    if (dir != 0)
        return;
    // The built-in browser instead of the system's dialogs: always, or (auto) while a controller is used.
    const bool builtIn = c.settings.fileBrowser == 1 || (c.settings.fileBrowser == 2 && c.padActive && c.padActive());
    if ((sel_ == kAddFiles || sel_ == kAddFolder) && builtIn) openBrowser(c);
    else if (sel_ == kAddFiles) c.addFiles();
    else if (sel_ == kAddFolder) c.addFolder();
    else if (sel_ == kClear) {
        c.audio.stop();
        c.playlist.clear();
    } else c.play(sel_ - kActionCount);
}

void Menu::barAction(int which, Context &c)
{
    if (which == 0) {
        if (c.quit)
            c.quit();
    } else {
        ask({"Reset all settings to their defaults?",
             "Every setting goes back to how it was on first start: visuals, layout, colours, keys, equalizer and "
             "debug options. Kept: the playlist, the Custom preset, your saved presets and the volume.",
             "Reset", [&c] {
                 if (c.resetAll)
                     c.resetAll();
             }});
    }
}

void Menu::padBind(PadButton b, Context &c)
{
    if (!capturingPad_)
        return;
    capturingPad_ = false;
    if (b == PadButton::Start || sel_ < 0 || sel_ >= Keybinds::kCount)
        return; // Start cancels (it opens and closes the menu, so it stays bound)
    auto &k = c.settings.keys;
    for (auto &p : k.pad) // a button belongs to one action only
        if (p == b)
            p = PadButton::None;
    k.pad[sel_] = b;
    c.changed();
}

void Menu::ask(Confirm q)
{
    confirm_ = std::move(q);
    confirmOpen_ = true;
    confirmFocus_ = 1;
}

void Menu::beginRename(int row, Context &c)
{
    const auto rows = rowsFor(c);
    if (row < 0 || row >= int(rows.size()) || !rows[size_t(row)].renameTo)
        return;
    renaming_ = row;
    sel_ = row;
    renameText_ = rows[size_t(row)].editName;
    if (c.textInput)
        c.textInput(true);
}

void Menu::endRename(bool commit, Context &c)
{
    if (renaming_ < 0)
        return;
    const auto rows = rowsFor(c);
    if (commit && renaming_ < int(rows.size()) && rows[size_t(renaming_)].renameTo)
        rows[size_t(renaming_)].renameTo(renameText_);
    renaming_ = -1;
    if (c.textInput)
        c.textInput(false);
}

void Menu::text(const char *utf8, Context &)
{
    if (renaming_ < 0 || !utf8)
        return;
    for (const char *p = utf8; *p; p++)
        if (static_cast<unsigned char>(*p) >= 0x20 && renameText_.size() < 60)
            renameText_ += *p;
}

bool Menu::key(const SDL_KeyboardEvent &e, Context &c)
{
    if (!open_)
        return false;
    if (capturingPad_) { // any key cancels waiting for a controller button
        capturingPad_ = false;
        return true;
    }
    if (capturing_) {
        switch (e.key) {
        case SDLK_LSHIFT: case SDLK_RSHIFT: case SDLK_LCTRL: case SDLK_RCTRL:
        case SDLK_LALT: case SDLK_RALT: case SDLK_LGUI: case SDLK_RGUI:
            return true; // wait for the real key
        case SDLK_ESCAPE:
            capturing_ = false;
            return true;
        default: {
            const KeyBind b{e.key, Keybinds::modsOf(e.mod)};
            auto &k = c.settings.keys;
            for (auto &row : k.slot) // a combination belongs to one action only
                for (auto &x : row)
                    if (x.key == b.key && x.mods == b.mods)
                        x = {};
            k.slot[sel_][col_] = b;
            capturing_ = false;
            c.changed();
            return true;
        }
        }
    }
    // Reset confirmation: Left/Right/Tab pick, Enter does it, Esc cancels; nothing else gets through.
    if (confirmOpen_) {
        switch (e.key) {
        case SDLK_LEFT: case SDLK_RIGHT: case SDLK_TAB: confirmFocus_ ^= 1; break;
        case SDLK_RETURN: case SDLK_KP_ENTER:
            confirmOpen_ = false;
            if (confirmFocus_ == 0 && confirm_.action)
                confirm_.action();
            clampSelection(c);
            break;
        case SDLK_ESCAPE: confirmOpen_ = false; break;
        default: break;
        }
        return true;
    }
    // Renaming a preset: typed text arrives through text(); Enter keeps it, Esc cancels.
    if (renaming_ >= 0) {
        switch (e.key) {
        case SDLK_RETURN: case SDLK_KP_ENTER: endRename(true, c); break;
        case SDLK_ESCAPE: endRename(false, c); break;
        case SDLK_BACKSPACE:
            while (!renameText_.empty() && (static_cast<unsigned char>(renameText_.back()) & 0xc0) == 0x80)
                renameText_.pop_back(); // continuation bytes of a UTF-8 character
            if (!renameText_.empty())
                renameText_.pop_back();
            break;
        default: break;
        }
        return true;
    }
    if (browsing_) { // file browser: Esc back to the playlist, Backspace / Left up one folder
        if (e.key == SDLK_ESCAPE) {
            browsing_ = false;
            sel_ = 0;
            scroll_ = 0;
            return true;
        }
        if (e.key == SDLK_BACKSPACE || e.key == SDLK_LEFT || e.key == SDLK_DELETE) {
            if (browsePlaces_)
                browseTo(browseDir_, c);
            else if (browseDir_.has_parent_path() && browseDir_.parent_path() != browseDir_)
                browseTo(browseDir_.parent_path(), c);
            return true;
        }
    }
    if (e.key == SDLK_F2 && rowTab() && !browsing_) {
        beginRename(sel_, c);
        return true;
    }
    // Button bar (reached with Down from the last row).
    if (barFocus_ >= 0) {
        switch (e.key) {
        case SDLK_LEFT: case SDLK_RIGHT: barFocus_ ^= 1; break;
        case SDLK_UP: barFocus_ = -1; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: barAction(barFocus_, c); break;
        case SDLK_ESCAPE: barFocus_ = -1; setOpen(false); break;
        case SDLK_TAB: barFocus_ = -1; return key(e, c);
        default: break;
        }
        return true;
    }
    const bool shift = e.mod & SDL_KMOD_SHIFT;
    const int track = sel_ - kActionCount;
    if (e.key == SDLK_DOWN && !shift && sel_ >= rowCount(c) - 1) { // past the last row: the buttons
        barFocus_ = 1; // Reset (the left one; it asks before doing anything)
        return true;
    }
    switch (e.key) {
    case SDLK_ESCAPE:
        setOpen(false);
        break;
    case SDLK_TAB:
        {
            int pos = 0;
            while (pos < kTabs && kOrder[pos] != tab_)
                pos++;
            setTab(kOrder[(pos + (shift ? kTabs - 1 : 1)) % kTabs]);
        }
        break;
    case SDLK_UP:
        if (tab_ == 1 && shift && track > 0)
            sel_ = c.playlist.move(track, -1) + kActionCount;
        else
            sel_--;
        skipHeader(c, -1);
        break;
    case SDLK_DOWN:
        if (tab_ == 1 && shift && track >= 0)
            sel_ = c.playlist.move(track, 1) + kActionCount;
        else
            sel_++;
        skipHeader(c, 1);
        break;
    case SDLK_PAGEUP: sel_ -= std::max(1, visible_ - 1); break;
    case SDLK_PAGEDOWN: sel_ += std::max(1, visible_ - 1); break;
    case SDLK_HOME: sel_ = 0; break;
    case SDLK_END: sel_ = rowCount(c) - 1; break;
    case SDLK_LEFT: activate(c, -1); break;
    case SDLK_RIGHT: activate(c, 1); break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: activate(c, 0); break;
    case SDLK_DELETE:
    case SDLK_BACKSPACE:
        if (tab_ == 1 && track >= 0)
            c.playlist.remove(track);
        if (tab_ == 7) { // (asks first)
            auto rows = rowsFor(c);
            if (sel_ >= 0 && sel_ < int(rows.size()) && rows[size_t(sel_)].remove)
                rows[size_t(sel_)].remove();
        }
        if (tab_ == 2 && sel_ < Keybinds::kCount) {
            if (c.padActive && c.padActive())
                c.settings.keys.pad[sel_] = PadButton::None;
            else
                c.settings.keys.slot[sel_][col_] = {};
            c.changed();
        }
        break;
    case SDLK_SPACE:
        return false; // play/pause still works with the menu open
    default:
        return true;
    }
    clampSelection(c);
    return true;
}

bool Menu::mouse(const SDL_Event &e, Context &c)
{
    if (!open_)
        return false;
    if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        scroll_ -= int(e.wheel.y * 3);
        scroll_ = std::clamp(scroll_, 0, std::max(0, rowCount(c) - visible_));
        sel_ = std::clamp(sel_, scroll_, scroll_ + visible_ - 1); // keep the selection on screen, or draw snaps back
        return true;
    }
    float x = 0, y = 0;
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        x = e.motion.x;
        y = e.motion.y;
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        x = e.button.x;
        y = e.button.y;
    } else {
        return false;
    }
    // Event coordinates are in window points; the layout is in pixels.
    float scale = 1.0f;
    if (SDL_Window *win = SDL_GetWindowFromID(e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.windowID : e.button.windowID))
        scale = SDL_GetWindowPixelDensity(win);
    x *= scale;
    y *= scale;
    mouseX_ = x;
    mouseY_ = y;
    if (confirmOpen_) { // the confirmation takes every click; outside it = cancel
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            if (confirmButtons_[0].contains(x, y)) {
                confirmOpen_ = false;
                if (confirm_.action)
                    confirm_.action();
                clampSelection(c);
            } else if (confirmButtons_[1].contains(x, y) || !panel_.contains(x, y)) {
                confirmOpen_ = false;
            }
        }
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        for (auto &[r, row, b] : rowButtons_) // buttons inside a row (Rename, Delete)
            if (r.contains(x, y)) {
                endRename(false, c);
                const auto rows = rowsFor(c);
                if (row < int(rows.size()) && b < int(rows[size_t(row)].buttons.size())) {
                    sel_ = row;
                    rows[size_t(row)].buttons[size_t(b)].second();
                }
                return true;
            }
        endRename(true, c); // clicking elsewhere keeps the typed name
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT)
        for (int b = 0; b < 2; b++)
            if (barButtons_[b].contains(x, y)) {
                barFocus_ = -1;
                barAction(b, c);
                return true;
            }

    // Scrollbar: drag the bar, or click the track to jump there (and keep dragging).
    auto dragTo = [&](float my) {
        const float range = std::max(1.0f, scrollTrackH_ - scrollBarH_);
        const float t = std::clamp((my - dragGrab_ - scrollTop_) / range, 0.0f, 1.0f);
        scroll_ = int(std::lround(t * float(scrollMax_)));
        sel_ = std::clamp(sel_, scroll_, scroll_ + visible_ - 1); // keep the selection on screen, or draw snaps back
    };
    // Colour picker strips: click or drag along them.
    auto pickAt = [&](int i) {
        const auto rows = rowsFor(c);
        if (i >= 0 && i < int(rows.size()) && rows[size_t(i)].pick && pickX1_ > pickX0_)
            rows[size_t(i)].pick((x - pickX0_) / (pickX1_ - pickX0_));
    };
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        const bool was = dragging_ || pickDrag_ >= 0;
        dragging_ = false;
        pickDrag_ = -1;
        return was;
    }
    if (pickDrag_ >= 0 && e.type == SDL_EVENT_MOUSE_MOTION) {
        pickAt(pickDrag_);
        return true;
    }
    if (dragging_ && e.type == SDL_EVENT_MOUSE_MOTION) {
        dragTo(y);
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && scrollMax_ > 0 &&
        scrollHit_.contains(x, y)) {
        const bool onBar = y >= scrollBarY_ && y < scrollBarY_ + scrollBarH_;
        dragGrab_ = onBar ? y - scrollBarY_ : scrollBarH_ * 0.5f; // track click: centre the bar there
        dragging_ = true;
        dragTo(y);
        return true;
    }
    for (auto &[r, i] : rowRects_)
        if (r.contains(x, y)) {
            sel_ = i;
            if (tab_ == 2 && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                capturing_ = false;
                if (i < Keybinds::kCount)
                    col_ = x >= slotX_[1] ? 1 : 0;
                activate(c, 0);
                clampSelection(c);
                return true;
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && rowTab() &&
                rowsFor(c)[size_t(i)].picker && x >= pickX0_ - 20 * (pickX1_ - pickX0_) / 400) {
                pickDrag_ = i;
                pickAt(i);
                return true;
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                const bool hasValue = rowTab() && rowsFor(c)[size_t(i)].adjust;
                if (hasValue)
                    activate(c, x < valueX_ + (r.x + r.w - valueX_) * 0.5f ? -1 : 1);
                else if (rowTab() || e.button.clicks >= 2 || i < kActionCount)
                    activate(c, 0);
            }
            clampSelection(c);
            return true;
        }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        for (int t = 0; t < kTabs; t++)
            if (tabs_[t].contains(x, y)) {
                setTab(t);
                return true;
            }
        if (!panel_.contains(x, y))
            setOpen(false);
    }
    return true;
}

void Menu::draw(Overlay &o, int w, int h, Context &c)
{
    if (!open_)
        return;
    // UI scale, but never larger than what fits the window.
    const float base = float(h) / 720.0f;
    const float u = std::min(base * c.settings.uiScale,
                             std::max(base, std::min((float(h) - 20.0f) / 620.0f, (float(w) - 20.0f) / 780.0f)));
    const float pw = std::min(float(w) - 40 * u, 860 * u);
    // Tall: everything but 15% at the top and bottom (never smaller than the normal size).
    // The button bar (Quit, Reset to defaults) sits under the panel.
    const float barH = 40 * u, barGap = 10 * u;
    const float ph = std::min(float(h) - 40 * u - barH - barGap,
                              c.settings.menuTall ? std::max(600 * u, float(h) * 0.7f) : 600 * u);
    const float px = (float(w) - pw) * 0.5f, py = (float(h) - ph - barH - barGap) * 0.5f;
    panel_ = {px, py, pw, ph};
    // Look: a dark, slightly blue panel with a thin border, a header band behind the tabs, and
    // one accent colour for the active tab, the selection and section titles.
    // Look: the UI theme (Misc > Theme) paints each element (ui::paint); the palette gives the colours.
    const UiTheme theme = themeSet_ ? theme_ : uiTheme(c.settings);
    const ui::Style &st = ui::style(c.settings.uiStyle);
    using ui::Part;
    auto paint = [&](Part part, float x, float y, float pw2, float ph2, float fade = 1) {
        ui::paint(o, st, part, x, y, pw2, ph2, u, theme, fade);
    };
    auto say = [&](const std::string &t, float x, float y, float size, const Rgba &col) {
        o.text(t, x, y, size, col.r, col.g, col.b, col.a, st.textShadow);
    };
    auto ink = [&](Part part, float alpha) { return ui::textOn(st, part, theme, alpha); };
    // Accent-coloured text (arrows, action rows): on a selection with its own text colour (a solid accent fill in
    // Classic / Metro) it takes that colour instead, or it would vanish.
    auto accentInk = [&](bool onSelection, float alpha) {
        return onSelection && st[Part::RowSelected].text.set() ? ink(Part::RowSelected, alpha) : ink(Part::Arrow, alpha);
    };
    const bool padMode = c.padActive && c.padActive();
    const PadStyle padStyle = c.padStyle ? c.padStyle() : PadStyle::Xbox;
    paint(Part::Backdrop, 0, 0, float(w), float(h));
    ui::paint(o, st, Part::Panel, px, py, pw, ph, u, theme, 1, ui::Body);
    paint(Part::Header, px, py, pw, 66 * u);
    ui::paint(o, st, Part::Panel, px, py, pw, ph, u, theme, 1, ui::Frame); // (the border over the header)

    // Tabs
    const char *tabNames[kTabs] = {"Visualizer", "Playlist", "Keys", "Custom", "EQ", "Misc", "Debug", "Presets"};
    float tx = px + 28 * u;
    const float ty = py + 44 * u;
    // Shrink the tab labels (and gaps) if they don't all fit across the panel.
    float tabSize = 26 * u * st.tabSize, gap = 36 * u, total = 0;
    for (int t = 0; t < kTabs; t++)
        total += o.measure(tabNames[kOrder[t]], tabSize) + (t ? gap : 0.0f);
    if (const float room = pw - 56 * u; total > room) {
        tabSize *= room / total;
        gap *= room / total;
    }
    for (int pos = 0; pos < kTabs; pos++) {
        const int t = kOrder[pos];
        const float tw = o.measure(tabNames[t], tabSize);
        const Part tp = t == tab_ ? Part::TabActive : Part::Tab;
        // The label's box (the PS3 look underlines its bottom edge): as wide as its advance and centred on its ink;
        // the bottom edge where it always was, the top edge as far above the capitals (which follow the label's size:
        // shrunk to fit, or the theme's tab_size), so the capitals sit in the middle.
        float ink0, ink1;
        o.inkSpan(tabNames[t], tabSize, ink0, ink1);
        const float below = 13 * u, top = ty - o.capHeight(tabSize) - below;
        paint(tp, std::round(tx + (ink0 + ink1 - tw) * 0.5f), top, tw, ty + below - top);
        say(tabNames[t], tx, ty, tabSize, ink(tp, 1));
        tabs_[t] = {tx - gap * 0.25f, ty - 30 * u, tw + gap * 0.5f, 46 * u};
        tx += tw + gap;
    }

    // Rows
    const float rowH = 34 * u, top = ty + 34 * u, bottom = py + ph - 44 * u;
    visible_ = std::max(1, int((bottom - top) / rowH));
    clampSelection(c);
    if (rowTab()) { // never rest on a section title
        const auto check = rowsFor(c);
        if (sel_ >= 0 && sel_ < int(check.size()) && check[size_t(sel_)].header)
            skipHeader(c, 1);
    }
    rowRects_.clear();
    rowButtons_.clear();
    valueX_ = px + pw * 0.58f;
    const int n = rowCount(c);
    std::vector<Row> rows;
    if (rowTab())
        rows = rowsFor(c);
    const fs::path playing = c.audio.isOpen() && c.playlist.index() >= 0 && c.playlist.index() < int(c.playlist.size())
                                 ? c.playlist.current()
                                 : fs::path();
    float keysLabelW = 0; // Keys tab: the longest action name
    if (tab_ == 2)
        for (int k = 0; k < Keybinds::kCount; k++)
            keysLabelW = std::max(keysLabelW, o.measure(Keybinds::label(Action(k)), 20 * u));
    for (int i = scroll_; i < n && i < scroll_ + visible_; i++) {
        const float y = top + float(i - scroll_) * rowH;
        const Rect r{px + 16 * u, y, pw - 32 * u, rowH};
        const bool header = rowTab() && i < int(rows.size()) && rows[size_t(i)].header;
        const float base = y + rowH * 0.7f, size = 20 * u;
        if (header) { // section title: accent text and a thin rule, not selectable
            const std::string &title = rows[size_t(i)].label;
            const float ts = 15 * u, tw2 = o.measure(title, ts);
            say(title, px + 24 * u, y + rowH * 0.78f, ts, ink(Part::SectionTitle, 1));
            paint(Part::SectionTitle, px + 34 * u + tw2, y + rowH * 0.6f, pw - 70 * u - tw2, 1 * u); // (the rule)
            continue;
        }
        rowRects_.push_back({r, i});
        const bool selected = i == sel_ && barFocus_ < 0;
        paint(selected ? Part::RowSelected : Part::Row, r.x, r.y, r.w, r.h);
        const Part rowPart = selected ? Part::RowSelected : Part::Row; // (text on the row)
        if (tab_ == 2) {
            if (i == Keybinds::kCount) {
                say("Reset keys to defaults", px + 32 * u, base, size, accentInk(selected, 1));
                continue;
            }
            say(Keybinds::label(Action(i)), px + 32 * u, base, size, ink(rowPart, 0.95f));
            if (padMode) { // the controller is in use: one column of button glyphs
                slotX_[0] = slotX_[1] = std::max(px + pw * 0.5f, px + 32 * u + keysLabelW + 24 * u);
                if (i == sel_)
                    paint(capturingPad_ ? Part::SlotCapture : Part::Slot, slotX_[0] - 6 * u, y + 3 * u,
                          px + pw - 36 * u - slotX_[0], rowH - 6 * u);
                if (i == sel_ && capturingPad_)
                    say("press a button... (Start cancels)", slotX_[0], base, 18 * u, ink(Part::SlotCapture, 0.95f));
                else {
                    const Rgba dash = ink(rowPart, 0.45f);
                    drawPadGlyph(o, c.settings.keys.pad[i], padStyle, slotX_[0], y + rowH * 0.5f, rowH * 0.7f, 1, &dash);
                }
                continue;
            }
            // Binding columns start after the longest action name.
            float labelW = 0;
            for (int k = 0; k < Keybinds::kCount; k++)
                labelW = std::max(labelW, o.measure(Keybinds::label(Action(k)), size));
            slotX_[0] = std::max(px + pw * 0.5f, px + 32 * u + labelW + 24 * u);
            slotX_[1] = slotX_[0] + (px + pw - 36 * u - slotX_[0]) * 0.5f;
            for (int k = 0; k < 2; k++) {
                const bool active = i == sel_ && k == col_;
                std::string t = active && capturing_ ? "press a key..." : Keybinds::describe(c.settings.keys.slot[i][k]);
                if (active)
                    paint(capturing_ ? Part::SlotCapture : Part::Slot, slotX_[k] - 6 * u, y + 3 * u,
                          slotX_[1] - slotX_[0] - 8 * u, rowH - 6 * u);
                say(t, slotX_[k], base, 18 * u,
                    ink(active ? (capturing_ ? Part::SlotCapture : Part::Slot) : rowPart, t == "-" ? 0.4f : 0.95f));
            }
        } else if (rowTab()) {
            const Row &row = rows[size_t(i)];
            // Buttons on the right (Rename, Delete).
            float buttonsLeft = px + pw - 36 * u;
            for (int b = int(row.buttons.size()) - 1; b >= 0; b--) {
                const std::string &bl = row.buttons[size_t(b)].first;
                const float bs = 15 * u, bw = o.measure(bl, bs) + 22 * u, bx = buttonsLeft - bw;
                const Rect br{bx, y + 5 * u, bw, rowH - 10 * u};
                rowButtons_.push_back({br, i, b});
                const Part bp = br.contains(mouseX_, mouseY_) ? Part::ButtonHot : Part::Button;
                paint(bp, br.x, br.y, br.w, br.h);
                say(bl, bx + 11 * u, br.y + br.h * 0.7f, bs, ink(bp, 1));
                buttonsLeft = bx - 8 * u;
            }
            if (i == renaming_) { // text field with a blinking caret
                const float fx = px + 28 * u, fw = buttonsLeft - 8 * u - fx;
                paint(Part::Field, fx, y + 4 * u, fw, rowH - 8 * u);
                const Rgba fieldInk = ink(Part::Field, 1);
                say(renameText_, fx + 6 * u, base, size, fieldInk);
                if ((SDL_GetTicks() / 500) % 2 == 0)
                    o.rect(fx + 7 * u + o.measure(renameText_, size), y + 8 * u, 1.5f * u, rowH - 16 * u, fieldInk.r,
                           fieldInk.g, fieldInk.b, 0.9f * fieldInk.a);
                continue;
            }
            std::string label = row.label;
            if (!row.buttons.empty() || (row.value.empty() && !row.picker)) // shorten long names before the buttons / edge
                while (label.size() > 4 && px + 32 * u + o.measure(label + "...", size) > buttonsLeft - 10 * u) {
                    label.pop_back();
                    while (!label.empty() && (static_cast<unsigned char>(label.back()) & 0xc0) == 0x80)
                        label.pop_back();
                }
            if (label != row.label)
                label += "...";
            say(label, px + 32 * u, base, size, ink(rowPart, row.disabled ? 0.35f : 0.95f));
            if (row.picker) { // gradient strip with a marker
                const float right = px + pw - 36 * u;
                pickX0_ = valueX_ + 22 * u;
                pickX1_ = right - 22 * u;
                const float sy = y + rowH * 0.5f - 6 * u, sh = 12 * u, segW = (pickX1_ - pickX0_) / 48.0f;
                for (int k = 0; k < 48; k++) {
                    float rgb[3];
                    const float f = (float(k) + 0.5f) / 48.0f;
                    if (row.picker == 1)
                        hsvRgb(f * 360.0f, 1.0f, 1.0f, rgb);
                    else if (row.picker == 2)
                        hsvRgb(row.pickHue, f, 1.0f, rgb);
                    else
                        hsvRgb(row.pickHue, row.pickSat, f, rgb);
                    o.rect(pickX0_ + segW * float(k), sy, segW + 0.5f, sh, rgb[0], rgb[1], rgb[2], 1);
                }
                const float mx = pickX0_ + (pickX1_ - pickX0_) * std::clamp(row.pickPos, 0.0f, 1.0f);
                o.rect(mx - 3 * u, sy - 4 * u, 6 * u, sh + 8 * u, 0, 0, 0, 0.8f);
                o.rect(mx - 1.5f * u, sy - 3 * u, 3 * u, sh + 6 * u, 1, 1, 1, 1);
                if (i == sel_ && row.adjust) {
                    say("<", valueX_, base, size, accentInk(true, 1));
                    say(">", right - o.measure(">", size), base, size, accentInk(true, 1));
                }
            } else if (!row.value.empty()) {
                const std::string v = row.value;
                const float vw = o.measure(v, size), right = px + pw - 36 * u;
                say(v, (valueX_ + right - vw) * 0.5f, base, size, ink(rowPart, row.disabled ? 0.35f : 0.95f));
                if (row.swatch) {
                    const float sw = 16 * u, sx = (valueX_ + right - vw) * 0.5f - sw - 10 * u, sy = y + (rowH - sw) * 0.5f;
                    o.rect(sx - 1 * u, sy - 1 * u, sw + 2 * u, sw + 2 * u, 1, 1, 1, 0.35f);
                    o.rect(sx, sy, sw, sw, row.swatchRgb[0], row.swatchRgb[1], row.swatchRgb[2], 1);
                }
                if (i == sel_ && row.adjust) { // (only rows ←/→ can change)
                    say("<", valueX_, base, size, accentInk(true, 1));
                    say(">", right - o.measure(">", size), base, size, accentInk(true, 1));
                }
            }
        } else if (i < kActionCount) {
            say(kActionLabels[i], px + 32 * u, base, size, accentInk(selected, 0.95f / 0.9f));
        } else {
            const auto &path = c.playlist.tracks()[size_t(i - kActionCount)];
            const bool now = !playing.empty() && path == playing && i - kActionCount == c.playlist.index();
            std::string name = utf8(path.stem());
            const float maxW = pw - 110 * u;
            while (name.size() > 4 && o.measure(name, size) > maxW) {
                name.pop_back();
                while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xc0) == 0x80)
                    name.pop_back();
            }
            if (name != utf8(path.stem()))
                name += "...";
            const std::string num = std::to_string(i - kActionCount + 1);
            say(num, px + 72 * u - o.measure(num, 16 * u), base, 16 * u, ink(rowPart, 0.5f));
            if (now)
                say(">", px + 30 * u, base, size, ink(rowPart, 1));
            say(name, px + 86 * u, base, size, ink(rowPart, now ? 1.0f : 0.85f));
        }
    }
    scrollMax_ = 0;
    if (n > visible_) { // scrollbar (click and drag; see mouse())
        const float trackH = bottom - top, barH = std::max(24 * u, trackH * float(visible_) / float(n));
        const float barY = top + (trackH - barH) * float(scroll_) / float(std::max(1, n - visible_));
        const float barX = px + pw - 14 * u, barW = 6 * u;
        scrollHit_ = {barX - 8 * u, top, barW + 16 * u, trackH};
        scrollTop_ = top;
        scrollTrackH_ = trackH;
        scrollBarY_ = barY;
        scrollBarH_ = barH;
        scrollMax_ = n - visible_;
        const bool hot = dragging_ || scrollHit_.contains(mouseX_, mouseY_);
        paint(hot ? Part::ScrollTrackHot : Part::ScrollTrack, barX, top, barW, trackH);
        paint(dragging_ ? Part::ScrollThumbDrag : hot ? Part::ScrollThumbHot : Part::ScrollThumb, barX, barY, barW, barH);
    } else {
        dragging_ = false;
    }
    if (tab_ == 1 && c.playlist.empty() && !browsing_)
        say("The playlist is empty: add files or drop them onto the window.", px + 32 * u,
            top + rowH * float(kActionCount) + rowH * 0.9f, 18 * u, ink(Part::Row, 0.6f));

    const char *hint = browsing_ ? "Enter: open folder / add track    Backspace: up one folder    Esc: back to the playlist"
                       : tab_ == 7   ? "Tab: next tab    Up/Down: select    Enter: run / use preset    F2: rename    Del: delete    Esc: close"
                       : tab_ == 4 ? "Tab: next tab    Up/Down: select    Left/Right: change (1 dB)    Esc: close"
                       : tab_ == 3 ? "Left/Right: change (Shift: x10)    Edits apply live while Custom is active    Esc: close"
                       : (tab_ == 0 || tab_ >= 5) ? "Tab: next tab    Up/Down: select    Left/Right: change    Esc: close"
                       : tab_ == 1 ? "Tab: next tab    Enter: play    Del: remove    Shift+Up/Down: move    Esc: close"
                       : capturing_ ? "Press the new key (with Shift/Ctrl/Alt if wanted)    Esc: cancel"
                                    : "Tab: next tab    Left/Right: slot    Enter: rebind    Del: clear    Esc: close";
    if (padMode) { // controller in use: button glyphs instead of keys
        const PadStyle st = padStyle;
        std::vector<std::pair<std::vector<PadButton>, std::string>> hints = {
            {{PadButton::DpadUp, PadButton::DpadDown}, "select"},
            {{PadButton::South}, browsing_ ? "open / add" : tab_ == 2 ? "rebind" : tab_ == 1 ? "play" : "use"},
            {{PadButton::East}, browsing_ ? "back to the playlist" : "back"},
            {{PadButton::LeftShoulder, PadButton::RightShoulder}, "tab"}};
        if (browsing_)
            hints.insert(hints.begin() + 1, {{PadButton::DpadLeft}, "up one folder"});
        else if (rowTab() && tab_ != 7)
            hints.insert(hints.begin() + 1, {{PadButton::DpadLeft, PadButton::DpadRight}, "change"});
        if (!browsing_ && (tab_ == 1 || tab_ == 2 || tab_ == 7))
            hints.push_back({{PadButton::North}, tab_ == 2 ? "clear" : tab_ == 1 ? "remove" : "delete"});
        float hx = px + 28 * u;
        const float hy = py + ph - 24 * u, gs = 20 * u;
        for (auto &[buttons, what] : hints) {
            for (PadButton b : buttons)
                hx += drawPadGlyph(o, b, st, hx, hy, gs, 0.9f) + 3 * u;
            say(what, hx + 4 * u, hy + 5 * u, 15 * u, ink(Part::Hint, 0.6f / 0.55f));
            hx += o.measure(what, 15 * u) + 22 * u;
        }
    } else {
        say(hint, px + 28 * u, py + ph - 18 * u, 15 * u, ink(Part::Hint, 1));
    }

    // Tooltip for the row under the mouse, after it has been hovered for a second.
    // (PS3VIS_SHOT_TIP: show it for the selected row at once, for screenshots.)
    static const bool shotTip = std::getenv("PS3VIS_SHOT_TIP") != nullptr;
    const Rect *tipRow = nullptr;
    int tipIdx = -1;
    // With a controller there's no pointer: the selected row gets the tooltip (after the same second).
    const bool bySelection = shotTip || (padMode && barFocus_ < 0 && !confirmOpen_ && renaming_ < 0);
    for (auto &[r, i] : rowRects_)
        if ((bySelection && i == sel_) || (!bySelection && !dragging_ && r.contains(mouseX_, mouseY_))) {
            tipRow = &r;
            tipIdx = i;
        }
    const char *tip = tipRow && rowTab() && tipIdx < int(rows.size()) ? rowHelp(tab_, rows[size_t(tipIdx)].label) : nullptr;
    // Debug option: the settings.ini key under the description (Keys tab: key.<action> / pad.<action>).
    std::string cfg;
    if (tipRow && c.settings.showConfigKeys) {
        if (rowTab() && tipIdx < int(rows.size()))
            cfg = rowConfigKeys(tab_, rows[size_t(tipIdx)].label, c);
        else if (tab_ == 2 && tipIdx >= 0 && tipIdx < Keybinds::kCount)
            cfg = std::string(padMode ? "pad." : "key.") + Keybinds::id(Action(tipIdx));
        else if (tab_ == 2 && tipIdx == Keybinds::kCount)
            cfg = "key.* and pad.*";
    }
    const std::string key = tip || !cfg.empty() ? std::to_string(tab_) + "/" + std::to_string(tipIdx) : std::string();
    const double now = double(SDL_GetTicks()) / 1000.0;
    if (key != hoverKey_) {
        hoverKey_ = key;
        hoverSince_ = now;
    }
    if ((tip || !cfg.empty()) && (shotTip || now - hoverSince_ >= 1.0)) {
        const float ts = 16 * u, pad = 10 * u, lineH = 21 * u;
        float boxW = std::min(pw - 60 * u, 460 * u);
        const float textW = boxW - 2 * pad;
        std::vector<std::string> lines; // word wrap
        std::string line, word;
        std::istringstream words(tip ? tip : "");
        while (words >> word) {
            const std::string next = line.empty() ? word : line + " " + word;
            if (!line.empty() && o.measure(next, ts) > textW) {
                lines.push_back(line);
                line = word;
            } else {
                line = next;
            }
        }
        if (!line.empty())
            lines.push_back(line);
        // The settings key(s): smaller, in the accent colour, under a thin rule; one line per key, with the
        // current value and the accepted values when those options are on.
        std::vector<std::string> cfgLines;
        if (!cfg.empty()) {
            std::map<std::string, std::string> current; // from the settings.ini text
            if (c.settings.showConfigValues) {
                std::istringstream ini(c.settings.text());
                for (std::string l; std::getline(ini, l);)
                    if (const size_t eq = l.find('='); eq != std::string::npos)
                        current[l.substr(0, eq)] = l.substr(eq + 1);
            }
            std::string rest = cfg;
            const bool single = cfg.find('(') != std::string::npos; // "eq_bands (value 2 of 10)", "(not saved...)"
            while (!rest.empty()) {
                const size_t comma = single ? std::string::npos : rest.find(", ");
                std::string k = rest.substr(0, comma);
                rest = comma == std::string::npos ? "" : rest.substr(comma + 2);
                std::string line = "[settings.ini] -> " + k;
                const std::string bare = k.substr(0, k.find(' '));
                auto [type, range] = keyTypeRange(bare);
                if (bare == "eq_bands" && k.find("value ") != std::string::npos) // one band of the list
                    type = "float", range = "-12-12 dB";
                std::string extra;
                if (c.settings.showConfigValues && current.count(bare)) {
                    std::string v = current[bare];
                    if (bare == "eq_bands" && k.find("value ") != std::string::npos) { // just this band's value
                        const int n = std::atoi(k.c_str() + k.find("value ") + 6);
                        std::istringstream list(v);
                        std::string item;
                        for (int b = 0; b < n && std::getline(list, item, ','); b++)
                            v = item;
                    }
                    extra = (type.empty() ? "" : "<" + type + "> ") + v;
                } else if (c.settings.showConfigRanges && !type.empty()) {
                    extra = "<" + type + ">";
                }
                if (c.settings.showConfigRanges && !range.empty())
                    extra += (extra.empty() ? "" : "; ") + range;
                if (!extra.empty())
                    line += "  " + extra;
                cfgLines.push_back(line);
            }
        }
        const float cs = 14 * u, cfgLH = cs + 5 * u;
        for (auto &l : cfgLines) // widen for long key lines (up to the panel)
            boxW = std::max(boxW, std::min(pw - 60 * u, o.measure(l, cs) + 2 * pad));
        const float cfgH = cfgLines.empty() ? 0.0f : cfgLH * float(cfgLines.size()) + (lines.empty() ? 1 * u : 11 * u);
        const float boxH = 2 * pad + (lines.empty() ? 0.0f : lineH * float(lines.size()) - 4 * u) + cfgH;
        const float anchorX = bySelection ? tipRow->x + 24 * u : std::max(tipRow->x + 24 * u, mouseX_ - boxW * 0.3f);
        const float bx = std::clamp(anchorX, px + 8 * u, px + pw - boxW - 8 * u);
        float by = tipRow->y + tipRow->h + 4 * u; // below the row, or above it if that runs off the panel
        if (by + boxH > py + ph - 4 * u)
            by = tipRow->y - boxH - 4 * u;
        const float fade = shotTip ? 1.0f : float(std::clamp((now - hoverSince_ - 1.0) / 0.15, 0.0, 1.0));
        paint(Part::Tooltip, bx - 1 * u, by - 1 * u, boxW + 2 * u, boxH + 2 * u, fade);
        for (size_t k = 0; k < lines.size(); k++)
            say(lines[k], bx + pad, by + pad + lineH * float(k) + ts * 0.8f, ts, ink(Part::Tooltip, fade));
        if (!cfgLines.empty()) {
            float cy = by + boxH - pad - cfgLH * float(cfgLines.size() - 1) - cs * 0.25f;
            if (!lines.empty()) // (a rule like the section titles')
                paint(Part::SectionTitle, bx + pad, cy - cs - 6 * u, boxW - 2 * pad, 1, fade * 1.2f);
            // Section-title colour, unless it's too close to the tooltip's own fill (light tooltips): then dimmed text.
            Rgba cfgInk = ink(Part::SectionTitle, fade);
            if (st[Part::Tooltip].fill.set()) {
                auto luma = [](const Rgba &k) { return 0.2126f * k.r + 0.7152f * k.g + 0.0722f * k.b; };
                if (std::abs(luma(cfgInk) - luma(ui::resolve(st[Part::Tooltip].fill, theme))) < 0.35f)
                    cfgInk = ink(Part::Tooltip, 0.7f * fade);
            }
            for (auto &l : cfgLines) {
                std::string shown = l; // (cut to the box; long value lists end in "...")
                if (o.measure(shown, cs) > boxW - 2 * pad) {
                    while (shown.size() > 8 && o.measure(shown + "...", cs) > boxW - 2 * pad)
                        shown.pop_back();
                    shown += "...";
                }
                say(shown, bx + pad, cy, cs, cfgInk);
                cy += cfgLH;
            }
        }
    }

    // Button bar under the panel.
    {
        const char *labels[2] = {"Quit", "Reset to defaults"};
        const float bs = 18 * u, bw0 = o.measure(labels[0], bs) + 48 * u, bw1 = o.measure(labels[1], bs) + 48 * u;
        const float by = py + ph + barGap, gap = 12 * u;
        const float bx0 = px + pw - bw0, bx1 = px + pw - bw0 - gap - bw1; // Quit in the corner, Reset beside it
        barButtons_[0] = {bx0, by, bw0, barH};
        barButtons_[1] = {bx1, by, bw1, barH};
        for (int b = 0; b < 2; b++) {
            const Rect &r = barButtons_[b];
            const bool focus = barFocus_ == b || (!confirmOpen_ && r.contains(mouseX_, mouseY_));
            const Part bp = focus ? Part::BarButtonHot : Part::BarButton;
            paint(bp, r.x, r.y, r.w, r.h);
            say(labels[b], r.x + (r.w - o.measure(labels[b], bs)) * 0.5f, r.y + r.h * 0.66f, bs, ink(bp, 1));
        }
    }

    // Reset confirmation, over everything.
    if (confirmOpen_) {
        paint(Part::Dim, px, py, pw, ph + barH + barGap);
        const float bw = std::min(pw - 40 * u, 520 * u), ts = 17 * u, lh = 23 * u, pad = 20 * u;
        const std::string &title = confirm_.title, &body = confirm_.body;
        std::vector<std::string> lines; // word wrap
        std::string line, word;
        std::istringstream words(body);
        while (words >> word) {
            const std::string next = line.empty() ? word : line + " " + word;
            if (!line.empty() && o.measure(next, ts) > bw - 2 * pad) {
                lines.push_back(line);
                line = word;
            } else {
                line = next;
            }
        }
        lines.push_back(line);
        const float btnH = 38 * u, bh = pad * 2 + 40 * u + lh * float(lines.size()) + 16 * u + btnH;
        const float bx = px + (pw - bw) * 0.5f, by = py + (ph - bh) * 0.5f;
        paint(Part::Dialog, bx - 1 * u, by - 1 * u, bw + 2 * u, bh + 2 * u);
        say(title, bx + pad, by + pad + 20 * u, 21 * u, ink(Part::Dialog, 1 / 0.85f));
        for (size_t k = 0; k < lines.size(); k++)
            say(lines[k], bx + pad, by + pad + 40 * u + lh * float(k) + ts, ts, ink(Part::Dialog, 1));
        const char *labels[2] = {confirm_.yes.c_str(), "Cancel"};
        const float btnW = 130 * u, btnY = by + bh - pad - btnH;
        for (int b = 0; b < 2; b++) {
            const Rect r{bx + bw - pad - btnW * float(2 - b) - 12 * u * float(1 - b), btnY, btnW, btnH};
            confirmButtons_[b] = r;
            const bool focus = confirmFocus_ == b || r.contains(mouseX_, mouseY_);
            const Part bp = b == 0 ? (focus ? Part::DangerButtonHot : Part::DangerButton) // (the destructive one: red)
                                   : (focus ? Part::DialogButtonHot : Part::DialogButton);
            paint(bp, r.x, r.y, r.w, r.h);
            say(labels[b], r.x + (r.w - o.measure(labels[b], 18 * u)) * 0.5f, r.y + r.h * 0.66f, 18 * u, ink(bp, 1));
        }
    }
}
