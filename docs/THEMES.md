# Writing a UI theme

The menu, help screen, notices and on-screen controls are painted by a **theme** (Esc > Misc > Theme). Eleven are
built in: PS3, Classic 95-style, Metro-style, Glossy Aero-style, Material-style, Brushed Metal-style, Neumorphism,
Flat-style, Synthwave, Brutalist, Terminal-style and Adwaita-style. You can add your own as small text files.

## Where theme files go

Put `*.theme` files in a folder called `themes` next to `settings.ini`:

| System | Folder |
| --- | --- |
| Linux | `~/.local/share/ps3vis/PS3 Visualizer/themes/` |
| Linux (Flatpak) | `~/.var/app/io.github.ps3vis.Visualizer/data/ps3vis/PS3 Visualizer/themes/` |
| Windows | `%APPDATA%\ps3vis\PS3 Visualizer\themes\` |
| macOS | `~/Library/Application Support/ps3vis/PS3 Visualizer/themes/` |

Themes are read at start-up and again **every time the Esc menu opens**, so you can edit a file, close the menu,
reopen it and see the change. If a line can't be understood, a notice names the file and the line. The rest of
the file still loads.

The chosen theme is saved by its name. If its file goes away, the player falls back to the PS3 theme.

## Fonts

Each built-in theme lists the fonts it looks best with, in order of preference (characters a font lacks come from the next one). The player ships with free
fonts (in its `fonts` folder, with their licences), so every theme has a good font even if you install nothing:

| Theme | Prefers | Bundled stand-in |
| --- | --- | --- |
| PS3 | the PS3's own font (from your firmware) | - |
| Classic 95-style | Tahoma | DejaVu Sans |
| Metro-style, Glossy Aero-style | Segoe UI | Selawik (made by Microsoft as an open stand-in for Segoe UI) |
| Material-style | Roboto | Roboto |
| Brushed Metal-style | Lucida Grande | DejaVu Sans |
| Neumorphism | Inter | Selawik |
| Flat-style | SF Pro Text, Helvetica Neue | Roboto |
| Synthwave | Orbitron | Orbitron (it's the real thing: free and bundled) |
| Brutalist | Helvetica, Arial | DejaVu Sans |
| Terminal-style | Cascadia Mono, Consolas | DejaVu Sans Mono |
| Adwaita-style | Cantarell | Cantarell (GNOME's own font: free and bundled) |

To get the preferred fonts:

- **Windows** already has Tahoma and Segoe UI. **macOS** already has Lucida Grande.
- **Linux:** Tahoma, Segoe UI and Lucida Grande belong to Windows and macOS and aren't free to share, so the
  bundled stand-ins are used (Selawik is made to match Segoe UI; DejaVu Sans is a close relative of Tahoma).
- Any other installed font works in your own themes too: name it in `font =` (see below), by its family name
  ("Noto Sans") or its file name. Fonts are found in the `themes` folder, the system's font folders and the
  player's `fonts` folder (in that order). Install them with your system's
  font installer or package manager (for example Noto Sans: `noto-fonts` on Arch, `fonts-noto` on Debian/Ubuntu).

## A first theme

The easiest start: pick a theme and colours in Esc > Misc, then choose **Copy theme to a file** at the end of the
Colours section. It writes a new file (only what differs from its base, plus a list of all element names) and
switches to it; edit that.

While a theme of your own is chosen, Misc also shows an **Edit theme** section: pick an element and one of its
colours (fill, gradient end, text, border, edge line, shadow, tint) and change it on the spot. Each change is saved
into the theme's file straight away. The file is rewritten, so comments you added to it are lost.

```ini
; sunset.theme: comments start with ';'
[theme]
name = Sunset
base = Metro-style        ; start from a built-in theme (default: PS3)
accent = #ff8a3d
background = #2a1530
highlight = #ff8a3d

[panel]
radius = 10
shadow = black @0.5
shadow_blur = 18
shadow_y = 4

[row_selected]
radius = 6
```

A theme only needs the keys it changes: everything else comes from its `base`.

## [theme]

| Key | Meaning |
| --- | --- |
| `name` | Shown in the Theme row. If it's left out, the file name is used. |
| `base` | The built-in theme to start from: `PS3`, `Classic 95-style`, `Metro-style`, `Glossy Aero-style`, `Material-style`, `Brushed Metal-style`, `Neumorphism`, `Flat-style`, `Synthwave`, `Brutalist`, `Terminal-style` or `Adwaita-style`. |
| `accent`, `text`, `background`, `highlight` | The theme's own colours, used when a colour row in Misc > Colours is set to "Theme". Write them as `#rrggbb` or `r g b` (0-1). |
| `text_shadow` | `1` adds the soft dark shadow under text (the PS3 look); `0` turns it off. |
| `tab_size` | The size of the tab labels (1 = normal). |
| `font` | A list of fonts, separated by commas, like CSS `font-family`: `font = "Segoe UI", Selawik, "Noto Sans"`. Each is a font's family name (its regular face is used), a full face name (`"Segoe UI Semibold"`), or a font file (a name ending in `.ttf`, `.otf` or `.ttc`, or a full path). Every listed font that is found is used, in order: each character comes from the first font that has it, and the PS3 fonts come last. Quotes are optional, but a name containing a comma needs them: `"Roboto[wdth,wght].ttf"`. Each font is sized to match the PS3 font's capital letters. Left out, or none found: the PS3 font. |
| `font_scale` | Fine-tunes that font's size (1 = same capital height as the PS3 font). |

## Elements

There is one section per kind of element. `[a : b]` starts element `a` as a copy of `b` (for example
`[button_hot : button]`), then applies the keys that follow.

| Section | What it is |
| --- | --- |
| `backdrop` | the shade over the visuals behind the menu |
| `panel`, `header` | the menu panel; the band behind the tabs |
| `tab`, `tab_active` | menu tabs |
| `section_title` | group titles inside a tab. Its `fill` is the rule line next to the title. |
| `row`, `row_selected` | setting rows |
| `slot`, `slot_capture` | a key binding cell; while waiting for a key |
| `button`, `button_hot` | buttons in the menu (hot = under the mouse) |
| `field` | the text box for renaming a preset |
| `scroll_track`, `scroll_track_hot`, `scroll_thumb`, `scroll_thumb_hot`, `scroll_thumb_drag` | scroll bars |
| `hint` | the hint line at the bottom of the menu |
| `tooltip` | tooltips |
| `bar_button`, `bar_button_hot` | Reset / Quit under the menu |
| `dim`, `dialog`, `dialog_button`, `dialog_button_hot`, `danger_button`, `danger_button_hot` | confirmation dialogs |
| `arrow` | the < > arrows on the selected row |
| `control_bar`, `control_button`, `control_button_hot` | on-screen playback bar and preset bar |
| `track_info` | text over the visuals (track title, time). Only `text` is used. |
| `help`, `help_header`, `keycap` | the shortcuts screen (H), its title band, a key |
| `notice` | message boxes at the top of the window |

## Element keys

| Key | Meaning |
| --- | --- |
| `fill` | the background colour |
| `fill2` | a second colour: the fill becomes a top-to-bottom gradient from `fill` to `fill2` |
| `tint` | a colour laid over the fill |
| `radius` | corner radius |
| `border`, `border_width` | an outline |
| `bevel` | `raised N`, `sunken N` or `none`: 3D edges N pixels wide (1-4) |
| `bevel_light`, `bevel_dark` | the bevel's colours (default white and black) |
| `shadow`, `shadow_blur`, `shadow_y` | a drop shadow. With `shadow_blur = 0` it is a hard copy of the box moved down by `shadow_y` (a keycap's side). |
| `gloss` | a white sheen over the top half (0-1) |
| `edge`, `edge_side`, `edge_width`, `edge_inset` | a coloured line on one side (`top`, `bottom`, `left` or `none`) |
| `grow_x`, `grow_y` | draw the element this much bigger than its slot |
| `text` | the colour of text on the element |

Sizes are in UI units: one pixel in a 720-pixel-high window at UI scale 1; they grow with the window and the UI scale setting.

## Colours

A colour is a **role**, a fixed colour, or `none`, followed by optional modifiers:

```
accent        text        background        highlight        white        black        danger        none
#c0c0c0       0.5 0.5 0.5
background *1.6 +0.02 @0.75 translucent
```

- **Roles** follow the user's choices in Misc > Colours, so your theme keeps working when someone picks a
  different accent.
- `*k` multiplies the colour and `+k` adds to it (`-k` subtracts).
- `@a` is the opacity (0-1).
- `translucent` also multiplies the opacity by the Background opacity setting.
- `none` turns that part off, for example `border = none`.

**Readable text.** With *Keep text readable* on (Misc > Colours, the default), the player checks each text
and icon colour against what is behind it. When all four colours are on *Theme* and *UI colours from
visuals* is off, the check is skipped entirely: your theme is drawn exactly as designed. With other colours (a custom pick, or *UI colours from
visuals*), text that would be harder to read than in your design is lightened or darkened, keeping its hue,
until it reaches 4.5:1 contrast, or at least your design's own contrast. Text about as bright as what's
behind it becomes black or white.

The built-in themes are written in this same format; they are at the top of `src/uistyle.cpp`.
