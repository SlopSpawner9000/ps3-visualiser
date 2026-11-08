// UI themes ("design languages"): how each kind of UI element is painted, as data.
//
// A theme is a small INI-style text (the built-in ones are in uistyle.cpp, written in the same format a user theme
// file would use): a [theme] section with its name, metrics and default colours, then one section per element
// ([panel], [button], ...) with its paint: fill (or a top-to-bottom gradient), corner radius, border, bevel,
// shadow, gloss, an accent edge, and the colour of text on it. Colours are roles of the palette (accent, text,
// background, highlight; set in Misc > Colours, "Theme" = the theme's own default) or fixed ones, with modifiers:
//   accent              background *1.6 +0.02 @0.75 translucent         #c0c0c0 @0.5         white @0.9
//   (*k scales, +k adds, @a is the alpha, "translucent" also multiplies by the UI opacity setting)
// Sections can start from another one: [button_hot : button]. Comments start with ';'.
// User themes: *.theme files in <data dir>/themes/, read at start-up and whenever the menu opens. Each starts from the
// PS3 theme, or from another built-in one named by `base = Classic 95-style` in [theme].
#pragma once

#include "overlay.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

struct UiTheme;

namespace ui {

enum class Role { None, Accent, Text, Background, Highlight, White, Black, Danger, Fixed };

struct Colour {
    Role role = Role::None; // None = not drawn
    float rgb[3] = {0, 0, 0}; // Fixed
    float scale = 1, add = 0, alpha = 1;
    bool translucent = false;
    bool set() const { return role != Role::None; }
};

// Where an edge (a solid strip in its own colour) goes: the PS3 look's accent underline / selection marker.
enum class Side { None, Bottom, Left, Top };

struct Paint {
    Colour fill, fill2;       // fill2 set: a vertical gradient from fill (top) to fill2 (bottom)
    Colour tint;              // a second flat layer over the fill (e.g. a highlight wash)
    float growX = 0, growY = 0; // paint this much (UI units) outside the element's rectangle
    float radius = 0;         // corner radius (UI units)
    Colour border;            // outline
    float borderWidth = 0;    // (UI units; 0 = none)
    int bevel = 0;            // 3D edges: 1 raised, -1 sunken (light top-left, dark bottom-right), in whole pixels
    Colour bevelLight, bevelDark;
    Colour shadow;            // drop shadow (shadow_blur 0: a hard copy of the box, shifted by shadow_y)
    float shadowBlur = 0, shadowY = 0;
    float gloss = 0;          // white sheen over the top half (glossy styles), 0..1
    Colour edge;              // accent strip on one side
    Side edgeSide = Side::None;
    float edgeWidth = 0, edgeInset = 0;
    Colour text;              // text drawn on this element (None: the palette's text colour)
};

// The UI's element kinds (one paint each; "Hot" = under the pointer or focused).
enum class Part {
    Backdrop, Panel, Header, Tab, TabActive, SectionTitle, Row, RowSelected, Slot, SlotCapture, Button, ButtonHot,
    Field, ScrollTrack, ScrollTrackHot, ScrollThumb, ScrollThumbHot, ScrollThumbDrag, Hint, Tooltip, BarButton,
    BarButtonHot, Dim, Dialog, DialogButton, DialogButtonHot, DangerButton, DangerButtonHot, Arrow,
    ControlBar, ControlButton, ControlButtonHot, // the on-screen playback bar and preset bar
    TrackInfo, // text over the visuals (track title, time): its text colour
    Help, HelpHeader, Keycap, // the shortcuts screen (H): panel, its title band, a key
    Notice,    // the message box at the top (errors, "saved", ...)
    Count
};

struct Style {
    std::string name;
    std::string base;          // (user themes) the built-in theme it starts from; empty = PS3
    std::filesystem::path file; // (user themes) the file it came from
    float defaults[4][3] = {}; // the "Theme" colour choice: accent, text, background, highlight
    float tabSize = 1.0f;      // tab label size factor
    bool textShadow = true;    // the soft dark shadow under text (the PS3 look); flat styles turn it off
    std::string font;          // font files to try, comma-separated (findFont); empty: the PS3's
    float fontScale = 1;       // its size against the PS3 font's
    std::array<Paint, size_t(Part::Count)> paints{};
    const Paint &operator[](Part p) const { return paints[size_t(p)]; }
};

// The built-in themes, in menu order (the first is the PS3 look).
const std::vector<Style> &builtinStyles();
// All themes: the built-in ones, then the user's (loadUserStyles).
const std::vector<Style> &styles();
const Style &style(int index); // (clamped)
int styleIndex(const std::string &name); // -1 if there's none by that name
// (Re)reads the user themes from `dir` (*.theme, sorted by file name). Returns one message per broken file
// ("name.theme: line N: ..."); a broken file still loads, minus the bad lines.
std::vector<std::string> loadUserStyles(const std::filesystem::path &dir);
// A theme as a theme file: the differences from the built-in theme it starts from, named `name`, with `colours`
// (accent, text, background, highlight) as its own colours. Parsing it gives the same theme back. `base`: list the
// differences from this built-in theme instead.
std::string styleText(const Style &s, const std::string &name, const float colours[4][3], const Style *base = nullptr);
// The fonts of a theme's `font` list that exist, in its order (like CSS font-family: comma-separated, a name in
// quotes may contain commas). Each is a font family or full face name ("Segoe UI", "Segoe UI Semibold": the
// family's regular face), or a font file (name ending in .ttf/.otf/.ttc/.otc, or a full path). Looked for in
// `themesDir`, then the system's font folders, then the bundled ones.
std::vector<std::filesystem::path> findFonts(const std::string &list, const std::filesystem::path &themesDir);
// The names in a `font` list (quotes removed).
std::vector<std::string> fontNames(const std::string &list);
// Folders with fonts that ship with the player (searched after the themes folder, before the system's).
void setBundledFontDirs(const std::vector<std::filesystem::path> &dirs);
// A user theme to change in place (the menu's theme editor), or null for a built-in one.
Style *userStyle(int index);
// Writes a user theme back to its file (styleText). False (and `error`) if it can't be written.
bool saveUserStyle(int index, std::string *error = nullptr);
// An element's name in theme files ("row_selected").
const char *partName(Part p);
// Parses a theme text (see the top of this file) onto `base` (unset keys keep base's). Errors go to `error`
// ("line N: ..."), the rest still applies.
Style parseStyle(const std::string &text, const Style &base, std::string *error = nullptr);

// A role colour resolved against the palette: rgb + alpha (alpha includes `fade`).
Rgba resolve(const Colour &c, const UiTheme &pal, float fade = 1);
// Paints one element into the rectangle (pixels); u = UI unit in pixels (radius, widths are in units).
// layers: what to draw (Body = shadow, fill, tint, gloss; Frame = bevel, border, edge), so something can go between
// (the menu's header band sits on the panel's body, under its border).
enum Layers { Body = 1, Frame = 2, All = 3 };
void paint(Overlay &o, const Style &s, Part part, float x, float y, float w, float h, float u, const UiTheme &pal,
           float fade = 1, int layers = All);
// The colour for text on an element: its own text colour (alpha multiplying it), or the palette's text at `alpha`.
// With pal.readable it's made readable on the element first (readableOn): at least as readable as the theme is
// with its own colours, up to 4.5:1 (so a theme with its own colours is unchanged).
Rgba textOn(const Style &s, Part part, const UiTheme &pal, float alpha);
// A colour for text or icons on an element, made readable on it like textOn's.
Rgba colourOn(const Style &s, Part part, const UiTheme &pal, const Colour &colour, float alpha);
// The WCAG contrast ratio (1..21) of text in `ink` on an element, against what's behind it there (its fill over
// what it sits on; the worse of the upper and lower part of a gradient).
float textContrast(const Style &s, Part part, const UiTheme &pal, const Rgba &ink);
// `ink`, moved towards white or black just far enough for `want` contrast on the element.
// Palettes from custom colours or the visuals can otherwise put e.g. white text on a light theme's button.
// `need` is the least it accepts: it goes as far as it can on its own side (light text lighter, dark text darker)
// and only flips to the other side if that can't reach `need`.
Rgba readableOn(const Style &s, Part part, const UiTheme &pal, Rgba ink, float want = 4.5f, float need = 4.5f);

} // namespace ui
