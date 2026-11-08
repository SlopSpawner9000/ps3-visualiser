// Esc menu: visualizer settings and playlist management, plus the settings
// file they persist to.
#pragma once

#include "canyon.h"
#include "earth.h"
#include "eq.h"
#include "firmware.h"
#include "gamepad.h"

#include <SDL3/SDL.h>

#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

class AudioEngine;

// Colours of the menu and on-screen elements (Misc tab -> Colours).
struct UiTheme {
    float accent[3];
    float text[3];
    float bg[3];        // panel / bar background
    float highlight[3]; // selection and button highlight (drawn see-through)
    float opacity;      // multiplies background opacities
    bool readable = true; // text colours are adjusted where they'd be hard to read (ui::textOn)
};
// One themable colour: a named choice, or (the last choice) custom hue/saturation/brightness.
struct UiColour {
    int choice = 0;
    float hue = 0, sat = 0, val = 1;
};
struct Settings;
UiTheme uiTheme(const Settings &s);
// Gain for the volume slider: linear, or (logVolume) a decibel curve: 0..100% spans -50..0 dB
// (fading to silence below 2%), 100..150% goes up to +3.5 dB like the linear maximum.
float volumeGain(const Settings &s);
std::string volumeText(const Settings &s); // "60%", or "60% (-20 dB)" with the log curve
void hsvRgb(float hue, float sat, float val, float rgb[3]); // hue in degrees, sat/val 0..1
void rgbHsv(const float rgb[3], float &hue, float &sat, float &val);
class Overlay;
class Playlist;

// Rebindable actions. Each has two slots; a binding is a key plus an exact
// set of Shift/Ctrl/Alt modifiers.
enum class Action {
    PlayPause, SeekForward, SeekBack, SeekForwardLong, SeekBackLong, VolumeUp, VolumeDown,
    NextTrack, PrevTrack, NextPreset, PrevPreset, Fullscreen, TrackInfo, AlbumArt, Shuffle, Repeat, Help, Menu, Quit,
    DebugDump,
    Count
};

struct KeyBind {
    SDL_Keycode key = 0;
    int mods = 0; // kShift | kCtrl | kAlt
};

struct Keybinds {
    static constexpr int kShift = 1, kCtrl = 2, kAlt = 4;
    static constexpr int kCount = int(Action::Count);
    KeyBind slot[kCount][2];
    PadButton pad[kCount];      // controller button per action (PadButton::None = unbound)

    Keybinds() { reset(); }
    void reset();
    static int modsOf(SDL_Keymod m);
    // Returns Action::Count when nothing matches.
    Action match(SDL_Keycode key, SDL_Keymod mod) const;
    Action padMatch(PadButton b) const;
    static const char *label(Action a);
    static const char *id(Action a);
    static std::string describe(const KeyBind &b);
};

// settings.ini format version. A file from a newer version is read but never overwritten.
// 2: Next/Previous track defaults moved to PageUp/PageDown (swapped in older files).
// 3: the UI colour choices start with "Theme" (older choices move up one).
constexpr int kConfigVersion = 3;

// Debug tab stress tests. Session only: never loaded or saved (they can make things unstable).
struct StressTest {
    int fps = 0;            // window frame rate: 0 vsync, 1 unlocked, 2..5 capped at 30/60/120/240
    int visRate = 0;        // visualizer updates: 0 60 Hz (normal), 1 every frame, 2 120 Hz, 3 240 Hz
    int renderPasses = 1;   // visualizer renders per update (1..8; extra ones are thrown away)
    float renderScale = 1;  // visualizer resolution vs the window (0.25..3)
    int presetStorm = 0;    // preset changes: 0 off, 1 every 2 s, 2 every 0.5 s, 3 every frame
    int seekStorm = 0;      // random seeks: 0 off, 1 every 5 s, 2 every second
    int hitches = 0;        // random stalls per frame: 0 off, 1 up to 30 ms, 2 up to 100 ms
    int cpuLoad = 0;        // busy background threads: 0 off, 1 one, 2 half the cores, 3 all cores
    bool active() const { return fps || heavy(); }
    // Anything beyond the frame rate limit (which alone is harmless): these get the warning banner.
    bool heavy() const
    {
        return visRate || renderPasses != 1 || renderScale != 1 || presetStorm || seekStorm || hitches || cpuLoad;
    }
};

struct Settings {
    std::string preset = "Blue02"; // what the PS3 opened with in the user's reference recording
    bool randomPresets = false;
    float gain = 3.0f;
    float volume = 1.0f;          // volume slider position (0..1.5); see volumeGain()
    bool logVolume = false;       // logarithmic volume curve (even loudness steps) instead of linear
    bool shuffle = false;
    float shuffleGap = 0.33f;     // shuffle: a track can't come back until this share of the playlist has played
    int repeat = 1;               // 0 off, 1 whole playlist, 2 the current song
    bool wheelVolume = true;      // mouse wheel over the visualizer changes the volume
    float uiScale = 1.0f;         // size of on-screen text and controls
    int infoPosition = 0;         // track info: 0 bottom left, 1 bottom right, 2 top left, 3 top right
    int controlsPosition = 0;     // playback buttons: 0 bottom right, 1 bottom centre, 2 bottom left
    int presetBarPosition = 1;    // preset arrows: 0 top, 1 bottom
    float infoScale = 1.0f;       // track info & album art size (on top of the UI scale)
    float controlsScale = 1.0f;   // playback buttons size (arrange mode: mouse wheel)
    float presetBarScale = 1.0f;  // preset bar size (arrange mode: mouse wheel)
    bool controlsVertical = false; // playback buttons stacked on the right/left edge
    // Dragged positions (top-left as a fraction of the window); x < 0 = use the setting above.
    float infoXY[2] = {-1, -1}, controlsXY[2] = {-1, -1}, presetBarXY[2] = {-1, -1};
    int displayMode = 0;          // 0 window, 1 borderless fullscreen, 2 exclusive fullscreen
    int fullscreenType = 1;       // what F / double-click switch to: 1 borderless, 2 exclusive
    int hdr = 0;                  // 0 auto (HDR if a display has it on), 1 on, 2 off
    float hdrBoost = 3.0f;        // HDR: how much brighter than SDR white the glow highlights get
    bool infoAlways = false;
    bool onScreenControls = true; // preset and transport buttons while the mouse moves
    bool debugKey = false;        // allow the Debug dump action (F12); for troubleshooting
    bool debugSpectrum = false;   // draw what the terrain is fed this frame
    bool albumColours = false;
    int albumArtMode = 0;         // cover next to the track info: 0 off, 1 auto (with the info), 2 always
    // UI colours (Misc -> Colours); the choice names are in menu.cpp (kColourChoices).
    UiColour uiAccent{0, 210, 0.45f, 1}, uiText{0, 40, 0.1f, 1}, uiBackground{0, 225, 0.55f, 0.06f},
        uiHighlight{0, 210, 0.15f, 1};
    bool uiReadable = true;       // keep UI text readable whatever the colours (ui::readableOn)
    bool uiFromVisuals = false;   // theme the UI from the visualizer's colours (or the album art)
    float uiOpacity = 1.0f;       // background opacity of the menu and control bars (0.3..1.3)
    bool songFades = false;       // fade songs in and out
    bool infoCrossfade = true;    // track info/cover crossfade along with the songs
    float fadeSeconds = 1.5f;     // 0.01 .. 10    // cover next to the track info (shown even when the info hides)    // shift preset colours to the track's embedded album art
    int fileBrowser = 2;          // Add files/folder: 0 system dialogs, 1 built-in, 2 auto (built-in with a controller)
    std::string browseDir;        // built-in file browser: last folder
    bool controllers = true;      // use game controllers (keyboards posing as one are skipped)
    int padGlyphs = 0;            // controller glyphs: 0 auto (from the controller), 1 Xbox, 2 PlayStation, 3 Steam
    bool menuTall = false;        // taller menu: the screen height minus 15% at the top and bottom
    int uiStyle = 0;              // UI theme (ui::builtinStyles(): 0 = the PS3 look)
    int dither = 1;               // dithering of the final image against colour banding: 0 off, 1 low, 2 medium, 3 high
    int perfOverlay = 0;          // refresh rate & frame time display: 0 off, 1 numbers, 2 numbers + graph
    float perfWindow = 10.0f;     // seconds the 1% low / min / max fps cover (1..60)
    int resourceOverlay = 0;      // CPU, RAM, I/O display: 0 off, 1 numbers, 2 numbers + graphs
    bool dumpHardware = false;    // debug dumps: add CPU, GPU, RAM, display and audio device details
    bool dumpSoftware = false;    // debug dumps: add OS, desktop, driver and library versions
    bool dumpRedact = true;
    bool showConfigKeys = false;  // tooltips also name the settings.ini key of each option
    bool showConfigValues = false; // ... with its current value
    bool showConfigRanges = false; // ... and its type and valid values
    bool hardwarePanel = false;   // debug panel: the hardware in use (CPU, RAM, GPU, display, audio)
    int pauseMode = 2;            // when paused: 0 glide to a stop, 1 stop at once, 2 keep moving       // debug dumps: hide user/host names, IP/MAC and e-mail addresses
    int configVersion = kConfigVersion; // version of the file this was loaded from
    unsigned uiColoursRead = 0;         // (load: which ui_<colour> choices the file had, bit per colour)
    bool verboseLog = false;      // extensive logging (trace lines + SDL verbose), kept for debug dumps
    // Preset flow, as seen in the reference recording of the PS3: in order through the
    // family (Blue02 -> 03 -> 04 -> 01 ...), every 17-28 s, blending over ~8 s.
    int flow = 1;               // 0 off, 1 within the preset's family, 2 all presets
    float flowSeconds = 22.0f;  // average time between flow steps (randomised +-25%)
    float flowMorph = 8.0f;     // seconds each flow transition takes
    StressTest stress;           // (not saved)
    Canyon::Tweaks tweaks;
    int visualizer = 0;           // 0 waves (canyon), 1 earth
    std::string earthView = "0";  // the earth's view (preset) name
    Earth::Tweaks earth;
    Keybinds keys;
    std::map<std::string, float> custom; // the Custom preset's parameter values
    bool customInRotation = false;       // let V / arrows / flow / random pick Custom
    EqSettings eq;

    void load(const fs::path &file);
    void save(const fs::path &file) const;
    std::string text() const; // the settings.ini contents
};

class Menu {
public:
    struct Context {
        Settings &settings;
        Canyon &canyon;
        AudioEngine &audio;
        Playlist &playlist;
        std::function<void(int)> play;      // start playlist entry
        std::function<void()> addFiles;     // open a file dialog
        std::function<void()> addFolder;    // open a folder dialog
        std::function<void()> toggleFullscreen;
        std::function<void(int)> setDisplay; // 0 window, 1 borderless, 2 exclusive fullscreen
        std::function<void()> quit;
        std::function<void()> changed;      // settings changed (apply + save)
        std::function<void(int)> stepPreset;  // -1 / +1, applied after a short settle delay
        std::function<std::string()> presetName; // the selected (possibly still pending) preset
        std::function<std::string()> hdrStatus;  // what the display output is doing right now
        std::function<void()> importPresetFiles;  // file dialog: add .ps3vis files to the preset library
        std::function<void()> exportPresetFile;   // file dialog: save the custom preset as a .ps3vis file
        std::function<void(const std::string &)> notify; // short on-screen message
        std::function<void()> arrange;                   // drag the on-screen elements into place
        std::function<void()> resetAll = {};             // every setting back to its default (after confirming)
        std::function<void(bool)> textInput = {};        // start/stop typing text (renaming a preset)
        std::function<std::string()> padStatus = {};     // the controller in use, or why none is
        std::function<bool()> padActive = {};            // a controller is the input in use: show its glyphs
        std::function<PadStyle()> padStyle = {};         // which glyphs
        std::function<void(const std::vector<std::string> &)> addPaths = {}; // add files/folders to the playlist
        std::function<std::vector<std::string>()> earthViews = {};          // the earth's view names (empty until loaded)
        std::function<void()> exportTheme = {}; // write the current theme into the themes folder and switch to it
        std::function<void()> openDataFolder = {}; // open settings.ini's folder in the OS's file browser
    };

    bool isOpen() const { return open_; }
    void setOpen(bool o);
    // Colours to draw with (the main loop's, which may follow the visuals); unset: from the settings.
    void setTheme(const UiTheme &t) { theme_ = t; themeSet_ = true; }
    void setTab(int t) { tab_ = ((t % kTabs) + kTabs) % kTabs; sel_ = scroll_ = 0; capturing_ = dragging_ = browsing_ = false; }
    // Tabs as shown, left to right (internal tab numbers stay the same).
    static constexpr int kOrder[] = {1, 0, 3, 7, 4, 5, 2, 6};
    // Return true when the event was consumed.
    bool key(const SDL_KeyboardEvent &e, Context &c);
    bool mouse(const SDL_Event &e, Context &c);
    void text(const char *utf8, Context &c); // typed text (SDL_EVENT_TEXT_INPUT)
    // Keys tab, controller shown: waiting for a button to bind. padBind() takes it (Start cancels).
    bool capturingPad() const { return open_ && capturingPad_; }
    void padBind(PadButton b, Context &c);
    void draw(Overlay &o, int w, int h, Context &c);

private:
    struct Row {
        std::string label, value;
        std::function<void(int)> adjust; // -1 / +1, may be null
        std::function<void()> activate;  // Enter / click, may be null
        std::function<void()> remove = {}; // Delete key, may be null
        bool header = false;               // a section title (not selectable)
        bool disabled = false;             // greyed out: shown, but doesn't react (activate explains why)
        // Colour picker strip instead of a value: 1 hue, 2 saturation (of `pickHue`), 3 brightness. `pick` sets
        // the position 0..1 (click or drag on the strip).
        int picker = 0;
        float pickPos = 0, pickHue = 0, pickSat = 0;
        std::function<void(float)> pick = {};
        bool swatch = false; // a colour sample next to the value
        float swatchRgb[3] = {};
        // Small buttons on the right of the row (e.g. Rename, Delete), and renaming in place.
        std::vector<std::pair<std::string, std::function<void()>>> buttons = {};
        std::function<void(const std::string &)> renameTo = {};
        std::string editName = {}; // starting text when renaming
    };
    std::vector<Row> settingsRows(Context &c);
    std::vector<Row> customRows(Context &c);
    std::vector<Row> eqRows(Context &c);
    std::vector<Row> presetRows(Context &c);
    std::vector<Row> rowsFor(Context &c);
    bool rowTab() const { return browsing_ || tab_ == 0 || tab_ >= 3; }
    int rowCount(Context &c);
    void clampSelection(Context &c);
    void skipHeader(Context &c, int dir);
    void activate(Context &c, int dir);

    bool open_ = false;
    static constexpr int kTabs = 8;
    int tab_ = 0;     // 0 visualizer, 1 playlist, 2 keys, 3 custom preset, 4 equalizer, 5 misc, 6 debug, 7 presets
    int col_ = 0;     // keys tab: which binding slot is selected
    bool capturing_ = false; // keys tab: waiting for a key press
    bool capturingPad_ = false; // keys tab: waiting for a controller button
    int sel_ = 0;     // selected row in the current tab
    int editPart_ = 1, editProp_ = 0; // Misc > Edit theme: the element (ui::Part, 1 = panel) and which of its colours
    int scroll_ = 0;  // first visible row
    int visible_ = 12; // rows that fit (from the last draw; a typical value until the first one)

    // Layout from the last draw, for mouse hit tests.
    struct Rect {
        float x, y, w, h;
        bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    };
    Rect panel_{}, tabs_[kTabs]{};
    float slotX_[2] = {}; // keys tab: left edges of the two binding columns
    std::vector<std::pair<Rect, int>> rowRects_;
    float valueX_ = 0; // left edge of the value column
    // Scrollbar, from the last draw: grab area (wider than what's drawn), bar position/size.
    Rect scrollHit_{};
    float scrollTop_ = 0, scrollTrackH_ = 0, scrollBarY_ = 0, scrollBarH_ = 0;
    int scrollMax_ = 0;       // 0 = no scrollbar
    bool dragging_ = false;   // scrollbar grabbed with the mouse
    float dragGrab_ = 0;      // where on the bar it was grabbed
    float mouseX_ = -1, mouseY_ = -1;
    float pickX0_ = 0, pickX1_ = 0; // colour picker strips: left/right edge (from the last draw)
    int pickDrag_ = -1;             // picker row being dragged
    UiTheme theme_{};
    bool themeSet_ = false;
    // Button bar under the panel (Quit, Reset to defaults) and the reset confirmation.
    Rect barButtons_[2]{};    // from the last draw
    int barFocus_ = -1;       // keyboard focus on a bar button (0 quit, 1 reset), -1 = the rows
    // A yes/cancel question over the menu (reset to defaults, deleting a preset).
    struct Confirm {
        std::string title, body, yes;
        std::function<void()> action;
    };
    bool confirmOpen_ = false;
    Confirm confirm_;
    int confirmFocus_ = 1;    // 0 yes, 1 Cancel (the default, so a stray Enter does nothing)
    Rect confirmButtons_[2]{};
    void ask(Confirm q);
    // Row buttons from the last draw (rect, row, button), and renaming a row in place.
    std::vector<std::tuple<Rect, int, int>> rowButtons_;
    int renaming_ = -1;
    std::string renameText_;
    void beginRename(int row, Context &c);
    // Built-in file browser (Playlist tab: Add files / Add folder), shown as rows in place of the playlist.
    struct BrowseEntry {
        std::string name;
        std::filesystem::path path;
        bool dir;
    };
    bool browsing_ = false, browsePlaces_ = false;
    std::filesystem::path browseDir_;
    std::vector<BrowseEntry> browseEntries_;
    std::string browseError_;
    void openBrowser(Context &c);
    void browseTo(const std::filesystem::path &dir, Context &c);
    std::vector<Row> browserRows(Context &c);
    void endRename(bool commit, Context &c);
    void barAction(int which, Context &c);
    // Tooltip: the row under the mouse and since when (help appears after 1 s).
    std::string hoverKey_;
    double hoverSince_ = 0;
};
