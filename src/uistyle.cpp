#include "uistyle.h"

#include "firmware.h"
#include "menu.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace ui {
namespace {

// The PS3 look (the menu as it was before themes): a dark, slightly see-through panel with a thin accent border,
// a lighter header band behind the tabs, and one accent colour for the active tab, the selection marker and
// section titles. Every other theme starts from this one.
const char *kPs3 = R"(
[theme]
name = PS3
accent = 0.55 0.78 1.0
text = 1 1 1
background = 0.025 0.03 0.055
highlight = 1 1 1

[backdrop]
fill = black @0.40
[panel]
fill = background @0.86 translucent
border = accent @0.22
border_width = 1
[header]
fill = background *1.6 +0.02 @0.75 translucent
[tab]
text = text @0.5
[tab_active]
text = text @1
edge = accent @0.95
edge_side = bottom
edge_width = 3
[section_title]
fill = accent @0.25
text = accent @0.95
[row_selected]
fill = highlight @0.10
edge = accent @0.95
edge_side = left
edge_width = 3
edge_inset = 4
[slot]
fill = highlight @0.15
[slot_capture]
fill = highlight @0.30
[button]
fill = highlight @0.08
text = text @0.8
[button_hot : button]
fill = highlight @0.22
text = text @1
[field]
fill = black @0.45
edge = accent @0.9
edge_side = bottom
edge_width = 1.5
[scroll_track]
fill = white @0.05
[scroll_track_hot]
fill = white @0.10
[scroll_thumb]
fill = white @0.4
[scroll_thumb_hot]
fill = white @0.65
[scroll_thumb_drag]
fill = white @0.85
[hint]
text = text @0.55
[tooltip]
fill = background *1.3 +0.015 @0.97
border = accent @0.55
border_width = 1
text = text @0.92
[bar_button]
fill = background @0.86 translucent
tint = highlight @0.05
edge = accent @0.35
edge_side = bottom
edge_width = 2
text = text @0.85
[bar_button_hot : bar_button]
tint = highlight @0.16
edge = accent @0.9
text = text @1
[dim]
fill = black @0.55
[dialog]
fill = background *1.3 +0.015 @0.98
border = accent @0.7
border_width = 1
text = text @0.85
[dialog_button]
fill = highlight @0.07
edge = accent @0.4
edge_side = bottom
edge_width = 2
text = text @0.8
[dialog_button_hot : dialog_button]
fill = highlight @0.2
edge = accent @0.95
text = text @1
[danger_button : dialog_button]
edge = danger @0.5
[danger_button_hot : dialog_button_hot]
edge = danger @0.95
[arrow]
text = accent @0.9
[control_bar]
fill = background @0.5 translucent
text = white
[control_button]
fill = highlight @0.12
[control_button_hot]
fill = highlight @0.28
[help]
fill = background @0.9 translucent
border = white @0.12
border_width = 1
[help_header]
fill = background *1.6 +0.02 @0.8 translucent
edge = accent @0.8
edge_side = bottom
edge_width = 2
[keycap]
fill = accent *0.18 +0.06 @0.95
shadow = accent *0.3 @0.9
shadow_y = 2
edge = accent @0.35
edge_side = top
edge_width = 1
text = accent *0.35 +0.65
[notice]
fill = background @0.95
border = accent @0.7
border_width = 1
)";

// Classic 95-style: grey 3D bevels (light top-left, dark bottom-right), a navy selection, pale yellow tooltips,
// square corners, no transparency and no text shadows.
const char *kClassic = R"(
[theme]
name = Classic 95-style
font = Tahoma, "DejaVu Sans"
accent = #000080
text = #000000
background = #c0c0c0
highlight = #000080
text_shadow = 0
tab_size = 0.85

[backdrop]
fill = black @0.25
[panel]
fill = background
border = none
bevel = raised 2
bevel_light = white
bevel_dark = black
[header]
fill = none
[tab]
fill = background
bevel = raised 2
grow_x = 9
grow_y = 3
text = text
edge = none
[tab_active : tab]
fill = background *1.06
grow_y = 6
[section_title]
fill = background *0.67
text = accent
[row_selected]
fill = highlight
text = white
edge = none
[slot]
fill = highlight
text = white
[slot_capture]
fill = accent *1.4
text = white
[button]
fill = background
bevel = raised 2
text = text
[button_hot : button]
border = black
border_width = 1
[field]
fill = white
bevel = sunken 2
edge = none
text = black
[scroll_track]
fill = background *1.16
[scroll_track_hot : scroll_track]
[scroll_thumb]
fill = background
bevel = raised 1
[scroll_thumb_hot : scroll_thumb]
[scroll_thumb_drag : scroll_thumb]
fill = background *0.92
[hint]
text = text @0.7
[tooltip]
fill = #ffffe1
border = black
border_width = 1
text = black
[bar_button : button]
tint = none
edge = none
[bar_button_hot : button_hot]
tint = none
edge = none
[dim]
fill = black @0.2
[dialog]
fill = background
border = none
bevel = raised 2
text = text
[dialog_button : button]
edge = none
[dialog_button_hot : button_hot]
edge = none
[danger_button : dialog_button]
text = #800000
[danger_button_hot : dialog_button_hot]
text = #800000
[arrow]
text = accent
[control_bar]
fill = background
bevel = raised 2
text = text
[control_button : button]
[control_button_hot : button_hot]
[track_info]
text = white
[help]
fill = background
border = none
bevel = raised 2
text = text
[help_header]
fill = accent
edge = none
text = white
[keycap]
fill = background
shadow = none
edge = none
bevel = raised 2
text = black
[notice]
fill = #ffffe1
border = black
text = black
)";

// Metro-style: flat, opaque, no borders or shadows; solid accent tiles for the selection and buttons, a white
// text field, light tooltips.
const char *kMetro = R"(
[theme]
name = Metro-style
font = "Segoe UI", Selawik, "Open Sans"
accent = #1ba1e2
text = #ffffff
background = #1d1d1d
highlight = #1ba1e2
text_shadow = 0
tab_size = 1.08

[backdrop]
fill = black @0.55
[panel]
fill = background
border = none
[header]
fill = none
[tab]
text = text @0.4
[tab_active]
text = text @1
edge = none
[section_title]
fill = none
text = accent
[row_selected]
fill = highlight
text = white
edge = none
[slot]
fill = white @0.18
[slot_capture]
fill = white @0.32
[button]
fill = white @0.14
text = text
[button_hot]
fill = accent
text = white
[field]
fill = white
edge = accent
edge_side = bottom
edge_width = 2
text = black
[scroll_track]
fill = white @0.04
[scroll_track_hot]
fill = white @0.08
[scroll_thumb]
fill = white @0.32
[scroll_thumb_hot]
fill = white @0.5
[scroll_thumb_drag]
fill = accent
[hint]
text = text @0.5
[tooltip]
fill = #f2f2f2
border = #808080
border_width = 1
text = #1d1d1d
[bar_button]
fill = background
tint = none
edge = none
text = text
[bar_button_hot : bar_button]
fill = accent
text = white
[dim]
fill = black @0.5
[dialog]
fill = background *1.5 +0.02
border = accent
border_width = 2
text = text
[dialog_button]
fill = white @0.14
edge = none
text = text
[dialog_button_hot : dialog_button]
fill = accent
text = white
[danger_button : dialog_button]
[danger_button_hot : dialog_button]
fill = #c42b1c
text = white
[arrow]
text = accent
[control_bar]
fill = background @0.92
text = text
[control_button]
fill = white @0.06
[control_button_hot]
fill = accent
[help]
fill = background
border = none
[help_header]
fill = none
edge = accent
edge_side = bottom
edge_width = 2
[keycap]
fill = white @0.14
shadow = none
edge = none
text = text
[notice]
fill = background *1.5 +0.02
border = accent
border_width = 2
[track_info]
text = white
)";

// Glossy Aero-style: light frosted glass (translucent white-blue gradients), rounded corners, soft shadows, glossy
// buttons and a glossy accent selection; dark text.
const char *kAero = R"(
[theme]
name = Glossy Aero-style
font = "Segoe UI", Selawik, "Open Sans"
accent = #1f8ee8
text = #10263c
background = #e2f1fd
highlight = #1f8ee8
text_shadow = 0

[backdrop]
fill = #0a2a4a @0.22
[panel]
fill = background @0.86 translucent
fill2 = background *0.86 @0.94 translucent
radius = 10
border = white @0.85
border_width = 1
shadow = #002040 @0.45
shadow_blur = 26
shadow_y = 6
[header]
fill = white @0.5
fill2 = white @0
radius = 10
[tab]
text = text @0.55
[tab_active]
fill = white @0.85
fill2 = accent *0.4 +0.6
radius = 7
border = accent @0.55
border_width = 1
gloss = 0.5
grow_x = 9
grow_y = 2
edge = none
text = text
[section_title]
fill = accent @0.3
text = accent *0.75
[row_selected]
fill = accent *0.6 +0.4
fill2 = accent *0.85
radius = 6
border = accent *0.6
border_width = 1
gloss = 0.55
edge = none
text = white
[slot]
fill = white @0.7
radius = 4
border = accent @0.45
border_width = 1
text = text
[slot_capture : slot]
fill = #fff4b8
[button]
fill = white @0.95
fill2 = #c9e2f8
radius = 6
border = #7fa6cc
border_width = 1
gloss = 0.5
text = text
[button_hot : button]
fill2 = #9fd2ff
border = accent
[field]
fill = white
radius = 4
border = accent
border_width = 1
edge = none
text = black
[scroll_track]
fill = #0a2a4a @0.07
radius = 3
[scroll_track_hot : scroll_track]
fill = #0a2a4a @0.12
[scroll_thumb]
fill = accent *0.6 +0.4
fill2 = accent *0.85
radius = 3
gloss = 0.4
[scroll_thumb_hot : scroll_thumb]
fill = accent *0.8 +0.2
[scroll_thumb_drag : scroll_thumb]
fill = accent
fill2 = accent *0.7
[hint]
text = text @0.6
[tooltip]
fill = white @0.98
fill2 = #e2f0fc
radius = 5
border = #7fa6cc
border_width = 1
shadow = #002040 @0.3
shadow_blur = 10
shadow_y = 3
text = text
[bar_button : button]
tint = none
edge = none
[bar_button_hot : button_hot]
tint = none
edge = none
[dim]
fill = #0a2a4a @0.3
[dialog : panel]
fill = background @0.97
fill2 = background *0.9
gloss = 0.3
text = text
[dialog_button : button]
edge = none
[dialog_button_hot : button_hot]
edge = none
[danger_button : button]
edge = none
text = #b01818
[danger_button_hot : button_hot]
edge = none
fill2 = #ffc2c2
border = #c03030
text = #b01818
[arrow]
text = accent *0.75
[control_bar]
fill = background @0.8 translucent
fill2 = background *0.85 @0.9 translucent
radius = 8
border = white @0.8
border_width = 1
gloss = 0.4
text = text
[control_button]
fill = white @0.35
radius = 5
[control_button_hot]
fill = accent *0.6 +0.4
fill2 = accent *0.85
radius = 5
gloss = 0.5
[track_info]
text = white
[help : panel]
fill = background @0.97
fill2 = background *0.88 @0.98
gloss = 0.2
text = text
[help_header]
fill = white @0.55
fill2 = white @0
radius = 10
edge = accent @0.5
edge_side = bottom
edge_width = 1
edge_inset = 12
text = text
[keycap]
fill = white
fill2 = #d4e7f8
radius = 4
border = #8aaccd
border_width = 1
shadow = #4a6c8c @0.55
shadow_blur = 0
shadow_y = 1.5
edge = none
gloss = 0.4
text = text
[notice]
fill = white @0.97
fill2 = #e2f0fc
radius = 8
border = accent @0.6
border_width = 1
shadow = #002040 @0.3
shadow_blur = 12
shadow_y = 3
text = text
)";

// Material-style (dark): tonal surfaces, elevation shadows instead of borders, pill-shaped selection and buttons,
// text buttons in dialogs, a light "snackbar" for notices.
const char *kMaterial = R"(
[theme]
name = Material-style
font = Roboto, "Noto Sans"
accent = #d0bcff
text = #e6e1e5
background = #1c1b1f
highlight = #4a4458
text_shadow = 0
tab_size = 0.95

[backdrop]
fill = black @0.5
[panel]
fill = background *1.15 +0.02
radius = 16
border = none
shadow = black @0.5
shadow_blur = 24
shadow_y = 8
[header]
fill = none
[tab]
text = text @0.6
[tab_active]
text = accent
edge = accent
edge_side = bottom
edge_width = 3
edge_inset = 6
[section_title]
fill = text @0.12
text = accent
[row_selected]
fill = highlight
radius = 14
edge = none
text = #e8def8
[slot]
fill = white @0.08
radius = 8
[slot_capture]
fill = accent @0.3
radius = 8
[button]
fill = highlight
radius = 18
text = #e8def8
[button_hot : button]
fill = highlight *1.3
[field]
fill = white @0.06
radius = 4
edge = accent
edge_side = bottom
edge_width = 2
text = text
[scroll_track]
fill = white @0.04
radius = 4
[scroll_track_hot : scroll_track]
fill = white @0.08
[scroll_thumb]
fill = white @0.3
radius = 4
[scroll_thumb_hot : scroll_thumb]
fill = white @0.45
[scroll_thumb_drag : scroll_thumb]
fill = accent
[hint]
text = text @0.6
[tooltip]
fill = #e6e1e5
radius = 4
border = none
shadow = black @0.4
shadow_blur = 8
shadow_y = 2
text = #313033
[bar_button]
fill = background *1.15 +0.02
radius = 18
tint = none
edge = none
shadow = black @0.4
shadow_blur = 10
shadow_y = 3
text = accent
[bar_button_hot : bar_button]
fill = background *1.5 +0.05
[dim]
fill = black @0.5
[dialog]
fill = background *1.35 +0.03
radius = 24
border = none
shadow = black @0.5
shadow_blur = 24
shadow_y = 8
text = text
[dialog_button]
fill = none
radius = 18
edge = none
text = accent
[dialog_button_hot : dialog_button]
fill = accent @0.12
[danger_button : dialog_button]
text = #f2b8b5
[danger_button_hot : danger_button]
fill = #f2b8b5 @0.12
[arrow]
text = accent
[control_bar]
fill = background *1.15 +0.02 @0.92
radius = 16
shadow = black @0.4
shadow_blur = 12
shadow_y = 4
text = text
[control_button]
fill = white @0.06
radius = 12
[control_button_hot]
fill = highlight
radius = 12
[track_info]
text = white
[help : panel]
text = text
[help_header]
fill = none
edge = none
text = text
[keycap]
fill = white @0.08
radius = 6
border = text @0.3
border_width = 1
shadow = none
edge = none
text = text
[notice]
fill = #e6e1e5
radius = 4
border = none
shadow = black @0.4
shadow_blur = 10
shadow_y = 3
text = #313033
)";

// Brushed Metal-style (skeuomorphic): grey metal gradients, a glossy blue gel selection, glossy pill buttons, inset
// white fields, raised keycaps; dark text.
const char *kMetal = R"(
[theme]
name = Brushed Metal-style
font = "Lucida Grande", "DejaVu Sans"
accent = #2f7fe0
text = #1b1b1d
background = #c9c9cc
highlight = #2f7fe0
text_shadow = 0

[backdrop]
fill = black @0.3
[panel]
fill = background *1.1
fill2 = background *0.82
radius = 7
border = #555559
border_width = 1
shadow = black @0.55
shadow_blur = 20
shadow_y = 6
gloss = 0.15
[header]
fill = background *0.95
fill2 = background *0.78
radius = 7
[tab]
text = text @0.6
[tab_active]
fill = white @0.9
fill2 = background *1.05
radius = 9
border = #6a6a70
border_width = 1
gloss = 0.4
grow_x = 9
grow_y = 2
edge = none
text = text
[section_title]
fill = #6a6a70 @0.45
text = #45454a
[row_selected]
fill = accent *0.55 +0.45
fill2 = accent *0.8
radius = 4
border = accent *0.55
border_width = 1
gloss = 0.7
edge = none
text = white
[slot]
fill = white
radius = 3
border = #7a7a80
border_width = 1
text = text
[slot_capture : slot]
fill = #fff2a8
[button]
fill = white
fill2 = background *0.9
radius = 11
border = #6a6a70
border_width = 1
gloss = 0.55
text = text
[button_hot : button]
fill = accent *0.4 +0.6
fill2 = accent *0.75
border = accent *0.5
text = white
[field]
fill = white
bevel = sunken 1
bevel_light = #f4f4f4
bevel_dark = #6a6a70
edge = none
text = black
[scroll_track]
fill = black @0.1
fill2 = black @0.04
radius = 4
[scroll_track_hot : scroll_track]
[scroll_thumb]
fill = white @0.95
fill2 = background *0.8
radius = 4
border = #7a7a80
border_width = 1
gloss = 0.4
[scroll_thumb_hot : scroll_thumb]
fill = white
[scroll_thumb_drag : scroll_thumb]
fill = accent *0.55 +0.45
fill2 = accent *0.8
border = accent *0.55
[hint]
text = text @0.65
[tooltip]
fill = #fffbe0
fill2 = #f3ecc4
radius = 4
border = #8c8466
border_width = 1
shadow = black @0.3
shadow_blur = 8
shadow_y = 3
text = black
[bar_button : button]
tint = none
edge = none
[bar_button_hot : button_hot]
tint = none
edge = none
[dim]
fill = black @0.35
[dialog : panel]
text = text
[dialog_button : button]
edge = none
[dialog_button_hot : button_hot]
edge = none
[danger_button : button]
edge = none
text = #9a1010
[danger_button_hot : button_hot]
edge = none
fill = #ff9a9a
fill2 = #c82828
border = #801010
text = white
[arrow]
text = accent *0.7
[control_bar]
fill = background *1.05 @0.95
fill2 = background *0.78 @0.95
radius = 8
border = #555559
border_width = 1
gloss = 0.3
text = text
[control_button]
fill = white @0.5
fill2 = white @0.1
radius = 6
border = #7a7a80 @0.6
border_width = 1
[control_button_hot]
fill = accent *0.4 +0.6
fill2 = accent *0.75
radius = 6
border = accent *0.5
border_width = 1
gloss = 0.5
[track_info]
text = white
[help : panel]
text = text
[help_header]
fill = background *0.95
fill2 = background *0.78
radius = 7
edge = #555559
edge_side = bottom
edge_width = 1
text = text
[keycap]
fill = white
fill2 = #d8d8dc
radius = 4
border = #7a7a80
border_width = 1
shadow = #4a4a50 @0.7
shadow_blur = 0
shadow_y = 2
edge = none
gloss = 0.3
text = text
[notice]
fill = #fffbe0
fill2 = #f3ecc4
radius = 6
border = #8c8466
border_width = 1
shadow = black @0.35
shadow_blur = 12
shadow_y = 4
text = black
)";

// Neumorphism (soft UI): monochrome light surfaces, no borders, every raised element lifted off the background by
// a soft blurred shadow instead; sunken fields use a soft bevel in the same two tones.
const char *kNeumo = R"(
[theme]
name = Neumorphism
font = "Inter", Selawik
accent = #6e7bf2
text = #44476a
background = #e3e8f0
highlight = #6e7bf2
text_shadow = 0

[backdrop]
fill = black @0.15
[panel]
fill = background
radius = 22
border = none
shadow = black @0.18
shadow_blur = 30
shadow_y = 12
[header]
fill = none
[tab]
text = text @0.5
[tab_active]
fill = background
radius = 14
shadow = black @0.14
shadow_blur = 10
shadow_y = 4
edge = none
grow_x = 6
grow_y = 3
text = accent
[section_title]
fill = none
text = accent @0.85
[row_selected]
fill = background
radius = 14
shadow = black @0.14
shadow_blur = 10
shadow_y = 3
edge = none
text = accent
[slot]
fill = background
radius = 10
bevel = sunken 2
bevel_light = white @0.6
bevel_dark = black @0.12
text = text
[slot_capture : slot]
fill = accent @0.15
[button]
fill = background
radius = 14
shadow = black @0.15
shadow_blur = 8
shadow_y = 3
text = text
[button_hot : button]
shadow = accent @0.3
text = accent
[field]
fill = background
radius = 10
bevel = sunken 2
bevel_light = white @0.6
bevel_dark = black @0.15
edge = none
text = text
[scroll_track]
fill = background *0.95
radius = 6
bevel = sunken 1
bevel_light = white @0.4
bevel_dark = black @0.1
[scroll_track_hot : scroll_track]
[scroll_thumb]
fill = background
radius = 6
shadow = black @0.12
shadow_blur = 4
shadow_y = 2
[scroll_thumb_hot : scroll_thumb]
shadow = accent @0.25
[scroll_thumb_drag : scroll_thumb]
fill = accent @0.3
[hint]
text = text @0.55
[tooltip]
fill = background
radius = 10
border = none
shadow = black @0.2
shadow_blur = 12
shadow_y = 4
text = text
[bar_button]
fill = background
radius = 16
tint = none
edge = none
shadow = black @0.15
shadow_blur = 8
shadow_y = 3
text = text @0.85
[bar_button_hot : bar_button]
shadow = accent @0.3
text = accent
[dim]
fill = black @0.3
[dialog]
fill = background
radius = 20
border = none
shadow = black @0.25
shadow_blur = 24
shadow_y = 10
text = text
[dialog_button]
fill = background
radius = 14
edge = none
shadow = black @0.14
shadow_blur = 6
shadow_y = 2
text = text
[dialog_button_hot : dialog_button]
shadow = accent @0.3
text = accent
[danger_button : dialog_button]
text = danger
[danger_button_hot : dialog_button_hot]
shadow = danger @0.3
text = danger
[arrow]
text = accent
[control_bar]
fill = background @0.85 translucent
radius = 16
shadow = black @0.18
shadow_blur = 14
shadow_y = 5
text = text
[control_button]
fill = background
radius = 12
shadow = black @0.12
shadow_blur = 5
shadow_y = 2
[control_button_hot]
fill = background
radius = 12
shadow = accent @0.3
shadow_blur = 6
shadow_y = 2
[track_info]
text = white
[help]
fill = background
radius = 18
border = none
shadow = black @0.2
shadow_blur = 20
shadow_y = 8
text = text
[help_header]
fill = none
edge = accent @0.3
edge_side = bottom
edge_width = 2
text = text
[keycap]
fill = background
radius = 8
shadow = black @0.18
shadow_blur = 0
shadow_y = 2
edge = none
text = text
[notice]
fill = background
radius = 12
border = none
shadow = black @0.22
shadow_blur = 14
shadow_y = 5
text = text
)";

// Flat/Minimal (iOS 7-style): no bevels, shadows or gloss anywhere; thin accent lines and translucent tints stand
// in for borders; plenty of flat white space.
const char *kFlat = R"(
[theme]
name = Flat-style
font = "SF Pro Text", "Helvetica Neue", Roboto
accent = #007aff
text = #1c1c1e
background = #ffffff
highlight = #007aff
text_shadow = 0

[backdrop]
fill = black @0.08
[panel]
fill = background
radius = 0
border = text @0.08
border_width = 1
[header]
fill = none
edge = text @0.08
edge_side = bottom
edge_width = 1
[tab]
text = text @0.45
[tab_active]
fill = none
edge = accent
edge_side = bottom
edge_width = 2
text = accent
[section_title]
fill = none
text = text @0.45
[row_selected]
fill = accent @0.08
edge = accent
edge_side = left
edge_width = 3
text = accent
[slot]
fill = text @0.05
radius = 4
text = text
[slot_capture : slot]
fill = accent @0.15
[button]
fill = none
border = text @0.15
border_width = 1
radius = 6
text = accent
[button_hot : button]
fill = accent @0.08
border = accent
[field]
fill = none
edge = text @0.2
edge_side = bottom
edge_width = 1
text = text
[scroll_track]
fill = text @0.04
[scroll_track_hot : scroll_track]
fill = text @0.07
[scroll_thumb]
fill = text @0.25
[scroll_thumb_hot : scroll_thumb]
fill = text @0.4
[scroll_thumb_drag : scroll_thumb]
fill = accent
[hint]
text = text @0.4
[tooltip]
fill = #1c1c1e
radius = 6
border = none
text = white
[bar_button]
fill = background @0.9 translucent
border = text @0.1
border_width = 1
radius = 6
tint = none
edge = none
text = text @0.85
[bar_button_hot : bar_button]
fill = accent @0.12
border = accent
text = accent
[dim]
fill = black @0.3
[dialog]
fill = background
radius = 10
border = none
text = text
[dialog_button]
fill = none
edge = none
text = accent
[dialog_button_hot : dialog_button]
fill = accent @0.08
[danger_button : dialog_button]
text = danger
[danger_button_hot : dialog_button_hot]
fill = danger @0.08
text = danger
[arrow]
text = accent
[control_bar]
fill = background @0.9 translucent
border = text @0.08
border_width = 1
text = text
[control_button]
fill = background @0.75 translucent
border = text @0.12
border_width = 1
radius = 20
[control_button_hot]
fill = accent @0.25
border = accent
radius = 20
[track_info]
text = white
[help]
fill = background
border = none
text = text
[help_header]
fill = none
edge = text @0.1
edge_side = bottom
edge_width = 1
text = text
[keycap]
fill = text @0.05
radius = 4
shadow = none
edge = none
text = text
[notice]
fill = #1c1c1e
radius = 8
border = none
text = white
)";

// Synthwave/Retrowave: deep purple-black with hot magenta and cyan neon, glowing borders (a same-colour shadow with
// no offset stands in for a neon halo), mostly square corners.
const char *kSynth = R"(
[theme]
name = Synthwave
font = Orbitron, "DejaVu Sans Mono"
accent = #ff2bd6
text = #f8f1ff
background = #170825
highlight = #00e5ff

[backdrop]
fill = black @0.5
[panel]
fill = background @0.92
border = accent
border_width = 2
radius = 4
shadow = accent @0.45
shadow_blur = 24
shadow_y = 0
[header]
fill = highlight @0.08
edge = accent @0.6
edge_side = bottom
edge_width = 2
[tab]
text = text @0.5
[tab_active]
fill = none
edge = highlight
edge_side = bottom
edge_width = 3
shadow = highlight @0.55
shadow_blur = 10
shadow_y = 0
text = highlight
[section_title]
fill = accent @0.18
text = accent
[row_selected]
fill = highlight @0.18
border = highlight
border_width = 1
radius = 4
shadow = highlight @0.45
shadow_blur = 14
shadow_y = 0
edge = none
text = highlight
[slot]
fill = background *1.4 +0.05
border = accent @0.4
border_width = 1
radius = 3
text = text
[slot_capture : slot]
fill = highlight @0.25
border = highlight
[button]
fill = background *1.3 +0.04
border = accent @0.7
border_width = 1
radius = 3
text = accent
[button_hot : button]
fill = accent @0.18
shadow = accent @0.55
shadow_blur = 12
shadow_y = 0
text = white
[field]
fill = black @0.4
border = none
edge = highlight
edge_side = bottom
edge_width = 2
text = text
[scroll_track]
fill = white @0.04
[scroll_track_hot : scroll_track]
fill = white @0.08
[scroll_thumb]
fill = accent @0.5
[scroll_thumb_hot : scroll_thumb]
fill = accent @0.75
[scroll_thumb_drag : scroll_thumb]
fill = highlight
[hint]
text = text @0.5
[tooltip]
fill = background *1.3 +0.04 @0.95
border = highlight @0.7
border_width = 1
radius = 3
text = highlight
[bar_button]
fill = background @0.85 translucent
tint = none
border = accent @0.5
border_width = 1
radius = 3
text = text @0.85
[bar_button_hot : bar_button]
border = accent
shadow = accent @0.5
shadow_blur = 10
shadow_y = 0
text = white
[dim]
fill = black @0.6
[dialog]
fill = background *1.2 +0.03
border = accent
border_width = 2
radius = 4
shadow = accent @0.4
shadow_blur = 20
shadow_y = 0
text = text
[dialog_button]
fill = none
border = accent @0.6
border_width = 1
radius = 3
text = accent
[dialog_button_hot : dialog_button]
fill = accent @0.15
border = accent
text = white
[danger_button : dialog_button]
border = danger @0.7
text = danger
[danger_button_hot : dialog_button_hot]
border = danger
fill = danger @0.18
text = white
[arrow]
text = highlight
[control_bar]
fill = background @0.6 translucent
border = accent @0.4
border_width = 1
radius = 4
text = text
[control_button]
fill = background *1.3 +0.04
border = accent @0.4
border_width = 1
radius = 3
[control_button_hot]
fill = accent @0.2
border = accent
shadow = accent @0.5
shadow_blur = 10
shadow_y = 0
[track_info]
text = highlight
[help]
fill = background @0.985
border = accent @0.5
border_width = 1
text = text
[help_header]
fill = highlight @0.08
edge = accent
edge_side = bottom
edge_width = 2
text = highlight
[keycap]
fill = background *1.3 +0.04
border = accent @0.7
border_width = 1
radius = 3
shadow = accent @0.5
shadow_blur = 0
shadow_y = 2
edge = none
text = accent
[notice]
fill = background *1.2 +0.03
border = highlight
border_width = 1
radius = 4
shadow = highlight @0.4
shadow_blur = 14
shadow_y = 0
text = highlight
)";

// Brutalist: raw black and white, thick square borders everywhere, no radius, shadow or gloss; one jarring red
// accent used sparingly.
const char *kBrutal = R"(
[theme]
name = Brutalist
font = Helvetica, Arial, "DejaVu Sans"
accent = #ff0000
text = #000000
background = #ffffff
highlight = #000000
text_shadow = 0

[backdrop]
fill = black @0.3
[panel]
fill = background
border = black
border_width = 3
radius = 0
[header]
fill = none
edge = black
edge_side = bottom
edge_width = 3
[tab]
text = text @0.6
[tab_active]
fill = black
edge = none
grow_y = 2
text = white
[section_title]
fill = accent
text = accent
[row_selected]
fill = black
edge = none
text = white
[slot]
fill = background
border = black
border_width = 2
text = text
[slot_capture : slot]
fill = accent
border = black
text = white
[button]
fill = background
border = black
border_width = 2
text = text
[button_hot : button]
fill = black
text = white
[field]
fill = background
border = black
border_width = 2
edge = none
text = black
[scroll_track]
fill = background
border = black
border_width = 1
[scroll_track_hot : scroll_track]
[scroll_thumb]
fill = black
[scroll_thumb_hot : scroll_thumb]
fill = accent
[scroll_thumb_drag : scroll_thumb]
fill = accent
[hint]
text = text @0.7
[tooltip]
fill = black
border = none
text = white
[bar_button]
fill = background
border = black
border_width = 2
tint = none
edge = none
text = text
[bar_button_hot : bar_button]
fill = black
text = white
[dim]
fill = black @0.4
[dialog]
fill = background
border = black
border_width = 4
radius = 0
text = text
[dialog_button]
fill = background
border = black
border_width = 2
edge = none
text = text
[dialog_button_hot : dialog_button]
fill = black
text = white
[danger_button : dialog_button]
border = accent
text = accent
[danger_button_hot : dialog_button_hot]
fill = accent
border = black
text = white
[arrow]
text = accent
[control_bar]
fill = background
border = black
border_width = 2
text = text
[control_button]
fill = background
border = black
border_width = 1
[control_button_hot]
fill = black
[track_info]
text = white
[help]
fill = background
border = black
border_width = 3
text = text
[help_header]
fill = black
edge = none
text = white
[keycap]
fill = background
border = black
border_width = 2
shadow = none
edge = none
text = black
[notice]
fill = black
border = accent
border_width = 2
text = white
)";

// Terminal/CRT: monochrome phosphor green on near-black, square corners, thin green outlines; a hard (unblurred)
// shadow on keycaps stands in for a CRT bezel edge.
const char *kTerminal = R"(
[theme]
name = Terminal-style
font = "Cascadia Mono", Consolas, "DejaVu Sans Mono"
accent = #33ff66
text = #33ff66
background = #020502
highlight = #33ff66
text_shadow = 0

[backdrop]
fill = black @0.6
[panel]
fill = background @0.95
border = accent @0.6
border_width = 1
radius = 0
[header]
fill = none
edge = accent @0.5
edge_side = bottom
edge_width = 1
[tab]
text = text @0.45
[tab_active]
fill = accent @0.12
edge = accent
edge_side = bottom
edge_width = 2
text = accent
[section_title]
fill = accent @0.1
text = accent
[row_selected]
fill = accent @0.18
edge = accent
edge_side = left
edge_width = 2
edge_inset = 2
text = white
[slot]
fill = black @0.5
border = accent @0.4
border_width = 1
text = text
[slot_capture : slot]
fill = accent @0.3
border = accent
[button]
fill = black @0.4
border = accent @0.5
border_width = 1
text = accent
[button_hot : button]
fill = accent @0.2
border = accent
text = white
[field]
fill = black @0.6
border = none
edge = accent
edge_side = bottom
edge_width = 1
text = accent
[scroll_track]
fill = accent @0.06
[scroll_track_hot : scroll_track]
fill = accent @0.1
[scroll_thumb]
fill = accent @0.4
[scroll_thumb_hot : scroll_thumb]
fill = accent @0.65
[scroll_thumb_drag : scroll_thumb]
fill = accent
[hint]
text = text @0.5
[tooltip]
fill = black @0.95
border = accent
border_width = 1
text = accent
[bar_button]
fill = background @0.9 translucent
tint = none
border = accent @0.4
border_width = 1
edge = none
text = text @0.85
[bar_button_hot : bar_button]
border = accent
fill = accent @0.15
text = white
[dim]
fill = black @0.7
[dialog]
fill = background @0.97
border = accent
border_width = 1
text = accent
[dialog_button]
fill = black @0.4
border = accent @0.5
border_width = 1
edge = none
text = accent
[dialog_button_hot : dialog_button]
fill = accent @0.2
border = accent
text = white
[danger_button : dialog_button]
border = danger @0.6
text = danger
[danger_button_hot : dialog_button_hot]
fill = danger @0.2
border = danger
text = white
[arrow]
text = accent
[control_bar]
fill = background @0.7 translucent
border = accent @0.4
border_width = 1
text = text
[control_button]
fill = black @0.3
border = accent @0.3
border_width = 1
[control_button_hot]
fill = accent @0.2
border = accent
[track_info]
text = accent
[help]
fill = background @0.99
border = accent
border_width = 1
text = accent
[help_header]
fill = accent @0.1
edge = accent
edge_side = bottom
edge_width = 1
text = accent
[keycap]
fill = black @0.5
border = accent @0.6
border_width = 1
shadow = accent @0.5
shadow_blur = 0
shadow_y = 1
edge = none
text = accent
[notice]
fill = black @0.95
border = accent
border_width = 1
text = accent
)";

// Adwaita-style (GNOME): light, restrained flat surfaces with a little rounding and very soft shadows, a muted
// blue accent, thin hairline borders instead of heavier outlines.
const char *kAdwaita = R"(
[theme]
name = Adwaita-style
font = Cantarell, "Noto Sans"
accent = #3584e4
text = #2e3436
background = #fafafa
highlight = #3584e4
text_shadow = 0

[backdrop]
fill = black @0.25
[panel]
fill = #f6f5f4
radius = 12
border = black @0.08
border_width = 1
shadow = black @0.15
shadow_blur = 16
shadow_y = 4
[header]
fill = white @0.6
radius = 12
[tab]
text = text @0.55
[tab_active]
fill = white
radius = 8
border = black @0.08
border_width = 1
grow_y = 2
edge = none
text = accent
[section_title]
fill = none
text = text @0.5
[row_selected]
fill = accent @0.12
radius = 8
edge = none
text = accent
[slot]
fill = black @0.05
radius = 6
text = text
[slot_capture : slot]
fill = accent @0.18
[button]
fill = white
radius = 8
border = black @0.1
border_width = 1
text = text
[button_hot : button]
fill = black @0.04
border = black @0.15
[field]
fill = white
radius = 6
border = black @0.12
border_width = 1
edge = none
text = black
[scroll_track]
fill = black @0.04
radius = 4
[scroll_track_hot : scroll_track]
fill = black @0.07
[scroll_thumb]
fill = black @0.25
radius = 4
[scroll_thumb_hot : scroll_thumb]
fill = black @0.4
[scroll_thumb_drag : scroll_thumb]
fill = accent
[hint]
text = text @0.55
[tooltip]
fill = #2e3436
radius = 6
border = none
text = white
[bar_button]
fill = #f6f5f4
radius = 10
tint = none
border = black @0.08
border_width = 1
edge = none
text = text @0.85
[bar_button_hot : bar_button]
fill = black @0.04
[dim]
fill = black @0.35
[dialog]
fill = #f6f5f4
radius = 14
border = black @0.08
border_width = 1
shadow = black @0.2
shadow_blur = 20
shadow_y = 6
text = text
[dialog_button]
fill = white
radius = 8
border = black @0.1
border_width = 1
edge = none
text = accent
[dialog_button_hot : dialog_button]
fill = black @0.04
[danger_button : dialog_button]
text = danger
[danger_button_hot : dialog_button_hot]
fill = danger @0.08
text = danger
[arrow]
text = accent
[control_bar]
fill = #f6f5f4 @0.9 translucent
radius = 12
border = black @0.08
border_width = 1
text = text
[control_button]
fill = white @0.6
radius = 10
[control_button_hot]
fill = accent @0.18
radius = 10
[track_info]
text = white
[help]
fill = #f6f5f4
radius = 12
border = black @0.08
border_width = 1
text = text
[help_header]
fill = white @0.6
radius = 12
edge = black @0.08
edge_side = bottom
edge_width = 1
text = text
[keycap]
fill = white
radius = 5
border = black @0.12
border_width = 1
shadow = black @0.15
shadow_blur = 0
shadow_y = 1
edge = none
text = text
[notice]
fill = #2e3436
radius = 8
border = none
text = white
)";

const char *kPartNames[] = {"backdrop", "panel", "header", "tab", "tab_active", "section_title", "row", "row_selected",
                            "slot", "slot_capture", "button", "button_hot", "field", "scroll_track", "scroll_track_hot",
                            "scroll_thumb", "scroll_thumb_hot", "scroll_thumb_drag", "hint", "tooltip", "bar_button",
                            "bar_button_hot", "dim", "dialog", "dialog_button", "dialog_button_hot", "danger_button",
                            "danger_button_hot", "arrow", "control_bar", "control_button", "control_button_hot",
                            "track_info", "help", "help_header", "keycap", "notice"};
static_assert(sizeof kPartNames / sizeof *kPartNames == size_t(Part::Count));

// One-line reminders for styleText()'s generated comments; kept in sync with docs/THEMES.md's Elements table.
const char *kPartHints[] = {
    "the shade over the visuals behind the menu", "the menu panel", "the band behind the tabs", "a menu tab",
    "the selected menu tab", "a group title inside a tab (fill = the rule line next to it)", "a setting row",
    "the selected setting row", "a key binding cell", "a key binding cell while waiting for a key",
    "a button in the menu", "a button under the mouse", "the text box for renaming a preset",
    "a scroll bar's track", "a scroll bar's track under the mouse", "a scroll bar's thumb",
    "a scroll bar's thumb under the mouse", "a scroll bar's thumb while dragged", "the hint line at the bottom",
    "a tooltip", "the Reset button under the menu", "the Quit button under the menu", "the dim behind a dialog",
    "a confirmation dialog", "a dialog button", "a dialog button under the mouse", "a dangerous dialog button",
    "a dangerous dialog button under the mouse", "the < > arrows on the selected row",
    "the on-screen playback bar", "an on-screen playback/preset button", "that button under the mouse",
    "text over the visuals: track title, time (only text is used)", "the shortcuts screen (H)",
    "the shortcuts screen's title band", "a key on the shortcuts screen", "a message box at the top of the window"};
static_assert(sizeof kPartHints / sizeof *kPartHints == size_t(Part::Count));

int partIndex(const std::string &name)
{
    for (size_t i = 0; i < size_t(Part::Count); i++)
        if (name == kPartNames[i])
            return int(i);
    return -1;
}

std::string trim(const std::string &s)
{
    const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool parseNumber(const std::string &s, float &out)
{
    char *end = nullptr;
    const float f = std::strtof(s.c_str(), &end);
    if (end == s.c_str() || *end || !std::isfinite(f))
        return false;
    out = f;
    return true;
}

// "accent", "#c0c0c0", "0.5 0.5 0.5", "none", then *scale +add @alpha translucent.
bool parseColour(const std::string &text, Colour &c, std::string &why)
{
    std::istringstream in(text);
    std::vector<std::string> tok;
    for (std::string t; in >> t;)
        tok.push_back(t);
    if (tok.empty()) {
        why = "empty colour";
        return false;
    }
    Colour out;
    size_t i = 1;
    static const std::map<std::string, Role> roles = {
        {"none", Role::None},           {"accent", Role::Accent}, {"text", Role::Text},   {"background", Role::Background},
        {"highlight", Role::Highlight}, {"white", Role::White},   {"black", Role::Black}, {"danger", Role::Danger}};
    if (auto it = roles.find(tok[0]); it != roles.end()) {
        out.role = it->second;
    } else if (tok[0].size() == 7 && tok[0][0] == '#') {
        const long v = std::strtol(tok[0].c_str() + 1, nullptr, 16);
        out.role = Role::Fixed;
        for (int k = 0; k < 3; k++)
            out.rgb[k] = float((v >> (16 - 8 * k)) & 0xff) / 255.0f;
    } else {
        float rgb[3];
        if (tok.size() < 3 || !parseNumber(tok[0], rgb[0]) || !parseNumber(tok[1], rgb[1]) || !parseNumber(tok[2], rgb[2])) {
            why = "unknown colour \"" + tok[0] + "\"";
            return false;
        }
        out.role = Role::Fixed;
        std::copy(rgb, rgb + 3, out.rgb);
        i = 3;
    }
    for (; i < tok.size(); i++) {
        const std::string &t = tok[i];
        float v = 0;
        if (t == "translucent")
            out.translucent = true;
        else if (t.size() > 1 && (t[0] == '*' || t[0] == '+' || t[0] == '@') && parseNumber(t.substr(1), v))
            (t[0] == '*' ? out.scale : t[0] == '+' ? out.add : out.alpha) = v;
        else if (t.size() > 1 && t[0] == '-' && parseNumber(t, v))
            out.add = v;
        else {
            why = "unknown colour modifier \"" + t + "\"";
            return false;
        }
    }
    c = out;
    return true;
}

bool parseRgb(const std::string &text, float rgb[3])
{
    Colour c;
    std::string why;
    if (!parseColour(text, c, why) || c.role != Role::Fixed)
        return false;
    std::copy(c.rgb, c.rgb + 3, rgb);
    return true;
}

// The element keys (in the order a written theme lists them). (Functions, not globals: themes can be parsed during
// static initialisation, before this file's globals exist.)
const std::vector<std::pair<std::string, Colour Paint::*>> &colourKeys()
{
    static const std::vector<std::pair<std::string, Colour Paint::*>> keys = {
        {"fill", &Paint::fill},     {"fill2", &Paint::fill2},           {"tint", &Paint::tint},
        {"border", &Paint::border}, {"bevel_light", &Paint::bevelLight}, {"bevel_dark", &Paint::bevelDark},
        {"shadow", &Paint::shadow}, {"edge", &Paint::edge},             {"text", &Paint::text}};
    return keys;
}
const std::vector<std::pair<std::string, float Paint::*>> &numberKeys()
{
    static const std::vector<std::pair<std::string, float Paint::*>> keys = {
        {"radius", &Paint::radius},         {"border_width", &Paint::borderWidth}, {"shadow_blur", &Paint::shadowBlur},
        {"shadow_y", &Paint::shadowY},      {"gloss", &Paint::gloss},              {"edge_width", &Paint::edgeWidth},
        {"edge_inset", &Paint::edgeInset}, {"grow_x", &Paint::growX},            {"grow_y", &Paint::growY}};
    return keys;
}

// A number as a theme file writes it (shortest exact-enough form).
std::string numberText(float f)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", double(f));
    return buf;
}

std::string colourText(const Colour &c)
{
    static const char *roleNames[] = {"none", "accent", "text", "background", "highlight", "white", "black", "danger"};
    std::string out;
    if (c.role == Role::Fixed) {
        bool bytes = true; // #rrggbb when that's exact
        for (float v : c.rgb)
            bytes = bytes && std::abs(v * 255.0f - std::round(v * 255.0f)) < 1e-3f;
        if (bytes) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "#%02x%02x%02x", int(std::lround(c.rgb[0] * 255)), int(std::lround(c.rgb[1] * 255)),
                          int(std::lround(c.rgb[2] * 255)));
            out = buf;
        } else {
            out = numberText(c.rgb[0]) + " " + numberText(c.rgb[1]) + " " + numberText(c.rgb[2]);
        }
    } else {
        out = roleNames[int(c.role)];
        if (c.role == Role::None)
            return out;
    }
    if (c.scale != 1)
        out += " *" + numberText(c.scale);
    if (c.add != 0)
        out += (c.add > 0 ? " +" : " ") + numberText(c.add);
    if (c.alpha != 1)
        out += " @" + numberText(c.alpha);
    if (c.translucent)
        out += " translucent";
    return out;
}

bool sameColour(const Colour &a, const Colour &b)
{
    return colourText(a) == colourText(b);
}

std::string rgbText(const float rgb[3]) // (#rrggbb: rounded to whole steps, easier to edit)
{
    Colour c;
    c.role = Role::Fixed;
    for (int k = 0; k < 3; k++)
        c.rgb[k] = std::round(std::clamp(rgb[k], 0.0f, 1.0f) * 255.0f) / 255.0f;
    return colourText(c);
}

bool setPaintKey(Paint &p, const std::string &k, const std::string &v, std::string &why)
{
    static const std::map<std::string, Colour Paint::*> colours(colourKeys().begin(), colourKeys().end());
    static const std::map<std::string, float Paint::*> numbers(numberKeys().begin(), numberKeys().end());
    if (auto it = colours.find(k); it != colours.end())
        return parseColour(v, p.*(it->second), why);
    if (auto it = numbers.find(k); it != numbers.end()) {
        if (!parseNumber(v, p.*(it->second))) {
            why = "\"" + v + "\" is not a number";
            return false;
        }
        return true;
    }
    if (k == "edge_side") {
        static const std::map<std::string, Side> sides = {
            {"none", Side::None}, {"bottom", Side::Bottom}, {"left", Side::Left}, {"top", Side::Top}};
        auto it = sides.find(v);
        if (it == sides.end()) {
            why = "edge_side is none, bottom, left or top";
            return false;
        }
        p.edgeSide = it->second;
        return true;
    }
    if (k == "bevel") { // raised / sunken / none, optionally a width in pixels: "raised 2"
        std::istringstream in(v);
        std::string kind;
        int width = 1;
        in >> kind >> width;
        if (kind != "raised" && kind != "sunken" && kind != "none") {
            why = "bevel is raised, sunken or none";
            return false;
        }
        p.bevel = kind == "none" ? 0 : (kind == "raised" ? 1 : -1) * std::clamp(width, 1, 4);
        return true;
    }
    why = "unknown key \"" + k + "\"";
    return false;
}

} // namespace

Style parseStyle(const std::string &text, const Style &base, std::string *error)
{
    Style s = base;
    std::istringstream in(text);
    int section = -2; // -2: none yet, -1: [theme]
    int lineNo = 0;
    auto fail = [&](const std::string &why) {
        if (error && error->empty())
            *error = "line " + std::to_string(lineNo) + ": " + why;
    };
    for (std::string line; std::getline(in, line);) {
        lineNo++;
        if (const size_t hash = line.find(';'); hash != std::string::npos) // comments: ; ...
            line.resize(hash);
        line = trim(line);
        if (line.empty())
            continue;
        if (line.front() == '[') { // [part] or [part : base part]
            const size_t close = line.find(']');
            std::string name = trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)), from;
            if (const size_t colon = name.find(':'); colon != std::string::npos) {
                from = trim(name.substr(colon + 1));
                name = trim(name.substr(0, colon));
            }
            section = name == "theme" ? -1 : partIndex(name);
            if (section == -2 || (section < 0 && name != "theme")) {
                fail("unknown section [" + name + "]");
                section = -3;
                continue;
            }
            if (!from.empty() && section >= 0) {
                const int f = partIndex(from);
                if (f < 0)
                    fail("unknown section [" + from + "]");
                else
                    s.paints[size_t(section)] = s.paints[size_t(f)];
            }
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            fail("expected key = value");
            continue;
        }
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        std::string why;
        if (section == -1) {
            static const char *roleKeys[] = {"accent", "text", "background", "highlight"};
            bool done = false;
            for (int r = 0; r < 4; r++)
                if (k == roleKeys[r]) {
                    if (!parseRgb(v, s.defaults[r]))
                        fail("\"" + v + "\" is not a colour (r g b or #rrggbb)");
                    done = true;
                }
            if (done)
                continue;
            if (k == "name")
                s.name = v;
            else if (k == "base")
                s.base = v;
            else if (k == "font")
                s.font = v;
            else if (k == "font_scale") {
                if (!parseNumber(v, s.fontScale))
                    fail("\"" + v + "\" is not a number");
            }
            else if (k == "text_shadow")
                s.textShadow = v == "1" || v == "on" || v == "yes";
            else if (k == "tab_size") {
                if (!parseNumber(v, s.tabSize))
                    fail("\"" + v + "\" is not a number");
            } else
                fail("unknown key \"" + k + "\"");
        } else if (section >= 0) {
            if (!setPaintKey(s.paints[size_t(section)], k, v, why))
                fail(why);
        } else if (section == -2) {
            fail("a key before any [section]");
        }
    }
    return s;
}

const std::vector<Style> &builtinStyles()
{
    static const std::vector<Style> styles = [] {
        std::vector<Style> v;
        std::string error;
        v.push_back(parseStyle(kPs3, Style{}, &error));
        for (const char *text : {kClassic, kMetro, kAero, kMaterial, kMetal, kNeumo, kFlat, kSynth, kBrutal, kTerminal,
                                  kAdwaita}) // (each starts from the PS3 look)
            v.push_back(parseStyle(text, v.front(), &error));
        if (!error.empty()) { // (a broken built-in theme: a programming error)
            std::fprintf(stderr, "built-in UI theme: %s\n", error.c_str());
            std::abort();
        }
        return v;
    }();
    return styles;
}

namespace {
std::vector<Style> &allStyles()
{
    static std::vector<Style> all = builtinStyles();
    return all;
}
} // namespace

const std::vector<Style> &styles() { return allStyles(); }

const Style &style(int index)
{
    const auto &all = styles();
    return all[size_t(std::clamp(index, 0, int(all.size()) - 1))];
}

int styleIndex(const std::string &name)
{
    const auto &all = styles();
    for (size_t i = 0; i < all.size(); i++)
        if (all[i].name == name)
            return int(i);
    return -1;
}

std::string styleText(const Style &s, const std::string &name, const float colours[4][3], const Style *base)
{
    // The built-in theme it starts from: the theme itself when it is one, else its base (default PS3).
    const auto &builtin = builtinStyles();
    if (!base) {
        base = &builtin.front();
        for (const Style &b : builtin)
            if (b.name == s.name || b.name == s.base)
                base = &b;
    }
    std::ostringstream out;
    out << "; " << name << ": a PS3 Visualizer UI theme (Esc > Misc > Theme). It lists what differs from its base\n"
        << "; theme; add any element and key from docs/THEMES.md. The files in this folder are re-read whenever the\n"
        << "; menu opens.\n"
        << ";\n; Element keys, used below (full list in docs/THEMES.md):\n"
        << ";  fill, fill2    background colour; both together = top-to-bottom gradient\n"
        << ";  tint           a colour laid over the fill\n"
        << ";  radius         corner radius\n"
        << ";  border, border_width       an outline\n"
        << ";  bevel, bevel_light/dark    \"raised N\" / \"sunken N\" / \"none\": 3D edges N px wide (1-4)\n"
        << ";  shadow, shadow_blur, shadow_y   a drop shadow (shadow_blur 0 = hard copy moved down by shadow_y)\n"
        << ";  gloss          a white sheen over the top half (0-1)\n"
        << ";  edge, edge_side, edge_width, edge_inset   a coloured line on one side (top/bottom/left/none)\n"
        << ";  grow_x, grow_y draw the element this much bigger than its slot\n"
        << ";  text           the colour of text on the element\n"
        << "; Colours: a role (accent/text/background/highlight/white/black/danger/none), #rrggbb, or \"r g b\" (0-1),\n"
        << "; then optional *k (multiply), +k/-k (add/subtract), @a (opacity), translucent (scale by Background opacity).\n"
        << ";\n; Elements below are only listed if they differ from the base theme " << base->name << ".\n";
    out << "\n[theme]\nname = " << name << "\nbase = " << base->name << "\n";
    static const char *roleKeys[] = {"accent", "text", "background", "highlight"};
    for (int r = 0; r < 4; r++)
        out << roleKeys[r] << " = " << rgbText(colours[r]) << "\n";
    if (s.textShadow != base->textShadow)
        out << "text_shadow = " << (s.textShadow ? 1 : 0) << "\n";
    if (s.tabSize != base->tabSize)
        out << "tab_size = " << numberText(s.tabSize) << "\n";
    if (s.font != base->font)
        out << "font = " << s.font << "\n";
    if (s.fontScale != base->fontScale)
        out << "font_scale = " << numberText(s.fontScale) << "\n";
    for (size_t i = 0; i < size_t(Part::Count); i++) {
        const Paint &p = s.paints[i], &b = base->paints[i];
        std::string keys;
        for (const auto &[k, member] : colourKeys())
            if (!sameColour(p.*member, b.*member))
                keys += k + " = " + colourText(p.*member) + "\n";
        for (const auto &[k, member] : numberKeys())
            if (p.*member != b.*member)
                keys += k + " = " + numberText(p.*member) + "\n";
        if (p.edgeSide != b.edgeSide) {
            static const char *sides[] = {"none", "bottom", "left", "top"};
            keys += std::string("edge_side = ") + sides[int(p.edgeSide)] + "\n";
        }
        if (p.bevel != b.bevel)
            keys += "bevel = " + std::string(p.bevel == 0 ? "none" : p.bevel > 0 ? "raised " : "sunken ") +
                    (p.bevel ? std::to_string(std::abs(p.bevel)) : "") + "\n";
        if (!keys.empty())
            out << "\n; " << kPartHints[i] << "\n[" << kPartNames[i] << "]\n" << keys;
    }
    return out.str();
}

Style *userStyle(int index)
{
    auto &all = allStyles();
    if (index < int(builtinStyles().size()) || index >= int(all.size()))
        return nullptr;
    return &all[size_t(index)];
}

bool saveUserStyle(int index, std::string *error)
{
    const Style *s = userStyle(index);
    if (!s || s->file.empty())
        return false;
    std::ofstream out(s->file, std::ios::binary);
    out << styleText(*s, s->name, s->defaults);
    out.close();
    if (!out && error)
        *error = "Couldn't write " + utf8(s->file);
    return bool(out);
}

const char *partName(Part p) { return kPartNames[std::clamp(int(p), 0, int(Part::Count) - 1)]; }

namespace {
std::vector<std::filesystem::path> &bundledFontDirs()
{
    static std::vector<std::filesystem::path> dirs;
    return dirs;
}
} // namespace

void setBundledFontDirs(const std::vector<std::filesystem::path> &dirs) { bundledFontDirs() = dirs; }

std::vector<std::string> fontNames(const std::string &list)
{
    std::vector<std::string> out;
    std::string name;
    char quote = 0;
    auto done = [&] {
        name = trim(name);
        if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'') && name.back() == name.front())
            name = name.substr(1, name.size() - 2);
        if (!name.empty())
            out.push_back(name);
        name.clear();
    };
    for (char ch : list) {
        if (quote) {
            if (ch == quote)
                quote = 0;
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
        } else if (ch == ',') {
            done();
            continue;
        }
        name += ch;
    }
    done();
    return out;
}

namespace {

std::string lowerCase(std::string t)
{
    for (char &ch : t)
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return t;
}

// A font file's names (from its OpenType name table; the first font of a collection).
struct FaceNames {
    std::string family, sub, full; // e.g. "Segoe UI", "Regular", "Segoe UI" (lower-case)
};

bool readFaceNames(const std::filesystem::path &file, FaceNames &out)
{
    std::ifstream in(file, std::ios::binary);
    auto u16 = [&](uint32_t at) {
        unsigned char b[2] = {};
        in.seekg(at);
        in.read(reinterpret_cast<char *>(b), 2);
        return uint32_t(b[0] << 8 | b[1]);
    };
    auto u32 = [&](uint32_t at) { return u16(at) << 16 | u16(at + 2); };
    if (!in)
        return false;
    uint32_t font = 0;
    if (u32(0) == 0x74746366) // 'ttcf': a collection
        font = u32(12);
    const uint32_t tables = u16(font + 4);
    if (!in || tables == 0 || tables > 200)
        return false;
    uint32_t name = 0, nameLen = 0;
    for (uint32_t t = 0; t < tables; t++)
        if (u32(font + 12 + 16 * t) == 0x6e616d65) { // 'name'
            name = u32(font + 12 + 16 * t + 8);
            nameLen = u32(font + 12 + 16 * t + 12);
        }
    if (!in || !name || nameLen < 6 || nameLen > (1u << 20))
        return false;
    std::string table(nameLen, '\0');
    in.seekg(name);
    in.read(table.data(), std::streamsize(nameLen));
    if (!in)
        return false;
    auto at16 = [&](size_t i) { return i + 2 <= table.size() ? uint32_t(uint8_t(table[i]) << 8 | uint8_t(table[i + 1])) : 0u; };
    const uint32_t count = at16(2), strings = at16(4);
    // Best record per name id: Windows Unicode US English, then any Windows Unicode, then Mac Roman.
    std::string best[18];
    int rank[18] = {};
    for (uint32_t r = 0; r < count; r++) {
        const size_t rec = 6 + 12 * size_t(r);
        const uint32_t platform = at16(rec), encoding = at16(rec + 2), language = at16(rec + 4), id = at16(rec + 6);
        const uint32_t len = at16(rec + 8), off = strings + at16(rec + 10);
        if (id >= 18 || off + len > table.size())
            continue;
        const bool win = platform == 3 && (encoding == 1 || encoding == 10);
        const int score = win && language == 0x409 ? 3 : win ? 2 : platform == 1 && encoding == 0 ? 1 : 0;
        if (score <= rank[id])
            continue;
        std::string text;
        if (win) { // UTF-16BE -> UTF-8
            for (uint32_t i = 0; i + 1 < len; i += 2) {
                uint32_t cp = uint32_t(uint8_t(table[off + i]) << 8 | uint8_t(table[off + i + 1]));
                if (cp >= 0xd800 && cp < 0xdc00 && i + 3 < len) {
                    const uint32_t lo = uint32_t(uint8_t(table[off + i + 2]) << 8 | uint8_t(table[off + i + 3]));
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    i += 2;
                }
                if (cp < 0x80) text += char(cp);
                else if (cp < 0x800) text += char(0xc0 | cp >> 6), text += char(0x80 | (cp & 0x3f));
                else if (cp < 0x10000) text += char(0xe0 | cp >> 12), text += char(0x80 | (cp >> 6 & 0x3f)), text += char(0x80 | (cp & 0x3f));
                else text += char(0xf0 | cp >> 18), text += char(0x80 | (cp >> 12 & 0x3f)), text += char(0x80 | (cp >> 6 & 0x3f)), text += char(0x80 | (cp & 0x3f));
            }
        } else if (score) {
            text = table.substr(off, len);
        }
        best[id] = text;
        rank[id] = score;
    }
    // Typographic family/subfamily (16/17) first: "Segoe UI" + "Light" rather than "Segoe UI Light" + "Regular".
    out.family = lowerCase(trim(best[16].empty() ? best[1] : best[16]));
    out.sub = lowerCase(trim(best[17].empty() ? best[2] : best[17]));
    out.full = lowerCase(trim(best[4]));
    return !out.family.empty();
}

struct FontFile {
    std::filesystem::path path;
    FaceNames names;
    bool named = false; // names read
};

// Fonts in these folders (searched below them), in order.
std::vector<FontFile> fontFilesIn(const std::vector<std::filesystem::path> &dirs)
{
    namespace fs = std::filesystem;
    std::vector<FontFile> out;
    std::error_code ec;
    for (const fs::path &d : dirs)
        for (fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec)) {
                const std::string ext = lowerCase(utf8(it->path().extension()));
                if (ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc")
                    out.push_back({it->path(), {}, false});
            }
    return out;
}

std::vector<std::filesystem::path> systemFontDirs()
{
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs;
    auto env = [](const char *k) { const char *v = std::getenv(k); return std::string(v ? v : ""); };
#ifdef _WIN32
    dirs.push_back(pathFromUtf8(env("WINDIR").empty() ? "C:\\Windows" : env("WINDIR")) / "Fonts");
    if (!env("LOCALAPPDATA").empty())
        dirs.push_back(pathFromUtf8(env("LOCALAPPDATA")) / "Microsoft" / "Windows" / "Fonts");
#elif defined(__APPLE__)
    dirs = {"/System/Library/Fonts", "/Library/Fonts"};
    if (!env("HOME").empty())
        dirs.push_back(pathFromUtf8(env("HOME")) / "Library" / "Fonts");
#else
    const std::string home = env("HOME");
    dirs.push_back(env("XDG_DATA_HOME").empty() ? pathFromUtf8(home) / ".local/share/fonts"
                                                : pathFromUtf8(env("XDG_DATA_HOME")) / "fonts");
    dirs.push_back(pathFromUtf8(home) / ".fonts");
    std::stringstream dataDirs(env("XDG_DATA_DIRS").empty() ? "/usr/local/share:/usr/share" : env("XDG_DATA_DIRS"));
    for (std::string d; std::getline(dataDirs, d, ':');)
        if (!d.empty())
            dirs.push_back(pathFromUtf8(d) / "fonts");
    for (const char *d : {"/usr/share/fonts", "/run/host/fonts", "/run/host/user-fonts"}) // (+ a Flatpak's host)
        dirs.push_back(d);
#endif
    return dirs;
}

} // namespace

std::vector<std::filesystem::path> findFonts(const std::string &list, const std::filesystem::path &themesDir)
{
    namespace fs = std::filesystem;
    // The bundled and system fonts are listed once (their names read on the first family-name lookup); the themes
    // folder every time (fonts can be dropped in while the player runs).
    static std::vector<FontFile> installed;
    static bool scanned = false;
    if (!scanned) {
        scanned = true;
        installed = fontFilesIn(systemFontDirs()); // (the system's first: the bundled ones are stand-ins)
        for (FontFile &f : fontFilesIn(bundledFontDirs()))
            installed.push_back(std::move(f));
    }
    std::vector<FontFile> fonts = themesDir.empty() ? std::vector<FontFile>() : fontFilesIn({themesDir});
    const size_t local = fonts.size();
    std::vector<fs::path> found;
    auto add = [&](const fs::path &f) {
        if (std::find(found.begin(), found.end(), f) == found.end())
            found.push_back(f);
    };
    for (const std::string &name : fontNames(list)) {
        const fs::path p = pathFromUtf8(name);
        const std::string ext = lowerCase(utf8(p.extension()));
        std::error_code ec;
        if (ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc" || p.is_absolute()) {
            // A file: a full path, or a file name in the themes folder, the system's fonts or the bundled ones.
            if (p.is_absolute()) {
                if (fs::is_regular_file(p, ec))
                    add(p);
                continue;
            }
            if (!themesDir.empty() && fs::is_regular_file(themesDir / p, ec)) {
                add(themesDir / p);
                continue;
            }
            const std::string file = lowerCase(utf8(p.filename()));
            for (const FontFile &f : installed)
                if (lowerCase(utf8(f.path.filename())) == file) {
                    add(f.path);
                    break;
                }
            continue;
        }
        // A family ("Segoe UI") or full face name ("Segoe UI Semibold"): the family's regular face, else any.
        const std::string want = lowerCase(name);
        const FontFile *best = nullptr;
        int bestScore = 1 << 30;
        for (size_t i = 0; i < local + installed.size(); i++) {
            FontFile &f = i < local ? fonts[i] : installed[i - local];
            if (!f.named) {
                f.named = true;
                if (!readFaceNames(f.path, f.names))
                    f.names = {};
            }
            int score;
            if (f.names.family == want) {
                const std::string &sub = f.names.sub;
                score = sub.empty() || sub == "regular" || sub == "book" || sub == "normal" || sub == "roman" ? 0
                        : sub == "medium"                                                                ? 1
                        : sub.find("italic") == std::string::npos && sub.find("oblique") == std::string::npos ? 2
                                                                                                         : 3;
            } else if (f.names.full == want) {
                score = 4;
            } else {
                continue;
            }
            if (score < bestScore) { // (ties: the earlier folder wins)
                best = &f;
                bestScore = score;
            }
        }
        if (best)
            add(best->path);
    }
    return found;
}

std::vector<std::string> loadUserStyles(const std::filesystem::path &dir)
{
    namespace fs = std::filesystem;
    const auto &builtin = builtinStyles();
    std::vector<Style> all = builtin;
    std::vector<std::string> errors;
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".theme" && it->is_regular_file(ec))
            files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const fs::path &file : files) {
        const std::string fileName = utf8(file.filename());
        std::ifstream in(file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        if (!in) {
            errors.push_back(fileName + ": can't be read");
            continue;
        }
        std::string error;
        Style s = parseStyle(text.str(), builtin.front(), &error);
        s.file = file;
        std::string baseName = builtin.front().name;
        if (!s.base.empty()) { // start again from the named built-in theme
            const auto it = std::find_if(builtin.begin(), builtin.end(), [&](const Style &b) { return b.name == s.base; });
            if (it == builtin.end()) {
                if (error.empty())
                    error = "base: there's no built-in theme \"" + s.base + "\"";
            } else {
                std::string again;
                s = parseStyle(text.str(), *it, &again);
                s.file = file;
                baseName = it->name;
            }
        }
        if (!error.empty())
            errors.push_back(fileName + ": " + error);
        // A missing or taken name: the file's name tells it apart.
        const std::string stem = utf8(file.stem());
        if (s.name.empty() || s.name == baseName)
            s.name = stem;
        for (const Style &other : all)
            if (other.name == s.name) {
                s.name += " (" + stem + ")";
                break;
            }
        all.push_back(std::move(s));
    }
    allStyles() = std::move(all);
    return errors;
}

Rgba resolve(const Colour &c, const UiTheme &pal, float fade)
{
    const float *src = nullptr;
    static const float white[3] = {1, 1, 1}, black[3] = {0, 0, 0}, danger[3] = {1.0f, 0.4f, 0.4f};
    switch (c.role) {
    case Role::None: return {0, 0, 0, 0};
    case Role::Accent: src = pal.accent; break;
    case Role::Text: src = pal.text; break;
    case Role::Background: src = pal.bg; break;
    case Role::Highlight: src = pal.highlight; break;
    case Role::White: src = white; break;
    case Role::Black: src = black; break;
    case Role::Danger: src = danger; break;
    case Role::Fixed: src = c.rgb; break;
    }
    float a = c.alpha * fade;
    if (c.translucent)
        a = std::min(1.0f, a * pal.opacity);
    return {src[0] * c.scale + c.add, src[1] * c.scale + c.add, src[2] * c.scale + c.add, a};
}

namespace {
// What an element sits on in the UI's layout (its text is read against both); Count = straight on the visuals.
Part under(Part p)
{
    switch (p) {
    case Part::Panel: return Part::Backdrop;
    case Part::Header: case Part::SectionTitle: case Part::Row: case Part::RowSelected: case Part::Button:
    case Part::ButtonHot: case Part::Field: case Part::Hint: case Part::Tooltip: case Part::Arrow: case Part::Dim:
    case Part::ScrollTrack: case Part::ScrollTrackHot:
        return Part::Panel;
    case Part::Tab: case Part::TabActive: return Part::Header;
    case Part::Slot: case Part::SlotCapture: return Part::RowSelected;
    case Part::Dialog: return Part::Dim;
    case Part::DialogButton: case Part::DialogButtonHot: case Part::DangerButton: case Part::DangerButtonHot:
        return Part::Dialog;
    case Part::ControlButton: case Part::ControlButtonHot: return Part::ControlBar;
    case Part::HelpHeader: case Part::Keycap: return Part::Help;
    default: return Part::Count; // backdrop, help, notice, control bars, track info
    }
}

// The opaque colour behind an element's text, at height f (0 top .. 1 bottom): its fill (gradient), tint and
// gloss over whatever it sits on, down to the visuals (taken as near-black: they mostly are, and text over them
// has its shadow).
void surface(const Style &s, Part part, const UiTheme &pal, float f, float out[3])
{
    if (part == Part::Count) {
        out[0] = out[1] = out[2] = 0.04f;
        return;
    }
    surface(s, under(part), pal, f, out);
    const Paint &p = s[part];
    auto over = [&](const Rgba &c) {
        const float a = std::clamp(c.a, 0.0f, 1.0f), rgb[3] = {c.r, c.g, c.b};
        for (int k = 0; k < 3; k++)
            out[k] += (std::clamp(rgb[k], 0.0f, 1.0f) - out[k]) * a;
    };
    if (p.fill.set()) {
        const Rgba top = resolve(p.fill, pal), bottom = p.fill2.set() ? resolve(p.fill2, pal) : top;
        over({top.r + (bottom.r - top.r) * f, top.g + (bottom.g - top.g) * f, top.b + (bottom.b - top.b) * f,
              top.a + (bottom.a - top.a) * f});
    }
    if (p.tint.set())
        over(resolve(p.tint, pal));
    if (p.gloss > 0 && f < 0.5f)
        over({1, 1, 1, (0.55f + (0.08f - 0.55f) * f * 2) * p.gloss});
}

float luminance(const float rgb[3]) // WCAG relative luminance
{
    float l[3];
    for (int k = 0; k < 3; k++) {
        const float c = std::clamp(rgb[k], 0.0f, 1.0f);
        l[k] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    return 0.2126f * l[0] + 0.7152f * l[1] + 0.0722f * l[2];
}
float contrast(float la, float lb) { return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f); }
} // namespace

float textContrast(const Style &s, Part part, const UiTheme &pal, const Rgba &ink)
{
    float worst = 21;
    for (const float f : {0.35f, 0.65f}) { // (the band the text is in, on a gradient)
        float bg[3];
        surface(s, part, pal, f, bg);
        const float a = std::clamp(ink.a, 0.0f, 1.0f), seen[3] = {bg[0] + (ink.r - bg[0]) * a,
                                                                   bg[1] + (ink.g - bg[1]) * a, bg[2] + (ink.b - bg[2]) * a};
        worst = std::min(worst, contrast(luminance(seen), luminance(bg)));
    }
    return worst;
}

Rgba readableOn(const Style &s, Part part, const UiTheme &pal, Rgba ink, float want, float need)
{
    need = std::min(need, want);
    if (textContrast(s, part, pal, ink) >= want)
        return ink;
    auto towards = [&](float to, float t) {
        return Rgba{ink.r + (to - ink.r) * t, ink.g + (to - ink.g) * t, ink.b + (to - ink.b) * t, ink.a};
    };
    // The least t (0..1, towards `to`, the text's own side so contrast only grows) that reaches `goal`, or -1.
    auto reach = [&](float to, float goal) {
        if (textContrast(s, part, pal, towards(to, 1)) < goal)
            return -1.0f;
        float lo = 0, hi = 1;
        for (int k = 0; k < 12; k++) {
            const float mid = (lo + hi) * 0.5f;
            (textContrast(s, part, pal, towards(to, mid)) >= goal ? hi : lo) = mid;
        }
        return hi;
    };
    // Lighter text gets lighter, darker text darker (keeping its hue); it only flips to the other side if that alone
    // can't get to `need`.
    float bg[3];
    surface(s, part, pal, 0.5f, bg);
    const float ownRgb[3] = {ink.r, ink.g, ink.b};
    const float same = luminance(ownRgb) >= luminance(bg) ? 1.0f : 0.0f, other = 1 - same;
    if (textContrast(s, part, pal, ink) >= 1.5f) { // (else it's about as bright as the background: no side of its own)
        if (const float t = reach(same, want); t >= 0)
            return towards(same, t);
        if (textContrast(s, part, pal, towards(same, 1)) >= need)
            return towards(same, 1);
    }
    // A flip loses the colour anyway: plain black or white, whichever reads better (a mid grey looks washed out).
    if (textContrast(s, part, pal, towards(other, 1)) > textContrast(s, part, pal, towards(same, 1)))
        return towards(other, 1);
    return towards(same, 1);
}

Rgba textOn(const Style &s, Part part, const UiTheme &pal, float alpha)
{
    const Paint &p = s[part];
    return colourOn(s, part, pal, p.text.set() ? p.text : Colour{Role::Text}, alpha);
}

Rgba colourOn(const Style &s, Part part, const UiTheme &pal, const Colour &colour, float alpha)
{
    Rgba c = resolve(colour, pal);
    if (pal.readable) { // (checked at the colour's own alpha; `alpha` then dims it: disabled rows, fades)
        // Aim for what the theme has with its own colours (a dimmed tab label stays dimmed), at most 4.5:1.
        UiTheme own = pal;
        float *to[4] = {own.accent, own.text, own.bg, own.highlight};
        for (int n = 0; n < 4; n++)
            std::copy(s.defaults[n], s.defaults[n] + 3, to[n]);
        const Rgba ref = resolve(colour, own);
        const float need = std::min(4.5f, textContrast(s, part, own, ref));
        // Other colours than the theme's own (custom, from the visuals): aim for 4.5:1, accepting the theme's own
        // level (white on a saturated blue may be lower, and reads fine) only if it takes no flip from light to dark.
        bool ownColours = true;
        const float *mine[4] = {pal.accent, pal.text, pal.bg, pal.highlight};
        for (int n = 0; n < 4; n++)
            for (int k = 0; k < 3; k++)
                ownColours = ownColours && std::fabs(mine[n][k] - to[n][k]) < 1e-3f;
        c = readableOn(s, part, pal, c, ownColours ? need : 4.5f, need);
    }
    c.a *= alpha;
    return c;
}

void paint(Overlay &o, const Style &s, Part part, float x, float y, float w, float h, float u, const UiTheme &pal,
           float fade, int layers)
{
    const Paint &p = s[part];
    x -= p.growX * u, y -= p.growY * u, w += 2 * p.growX * u, h += 2 * p.growY * u;
    const float radius = p.radius * u;
    if (layers & Body) {
        if (p.shadow.set() && p.shadowBlur > 0) {
            const Rgba c = resolve(p.shadow, pal, fade);
            const float blur = p.shadowBlur * u;
            o.box(x, y + p.shadowY * u, w, h, c, c, radius + blur * 0.5f, 0, blur);
        } else if (p.shadow.set() && p.shadowY != 0) { // hard shadow (a keycap's side)
            const Rgba c = resolve(p.shadow, pal, fade);
            if (radius <= 0)
                o.rect(x, y + p.shadowY * u, w, h, c.r, c.g, c.b, c.a);
            else
                o.box(x, y + p.shadowY * u, w, h, c, c, radius);
        }
        if (p.fill.set()) {
            const Rgba top = resolve(p.fill, pal, fade), bottom = p.fill2.set() ? resolve(p.fill2, pal, fade) : top;
            if (radius <= 0 && !p.fill2.set())
                o.rect(x, y, w, h, top.r, top.g, top.b, top.a); // (plain rectangles stay pixel-snapped)
            else
                o.box(x, y, w, h, top, bottom, radius);
        }
        if (p.tint.set()) {
            const Rgba c = resolve(p.tint, pal, fade);
            if (radius <= 0)
                o.rect(x, y, w, h, c.r, c.g, c.b, c.a);
            else
                o.box(x, y, w, h, c, c, radius);
        }
        if (p.gloss > 0) { // a sheen fading out over the top half
            o.box(x, y, w, h * 0.5f, Rgba{1, 1, 1, 0.55f * p.gloss * fade}, Rgba{1, 1, 1, 0.08f * p.gloss * fade}, radius);
        }
    }
    if (!(layers & Frame))
        return;
    if (p.bevel != 0) { // 3D edges in whole pixels: outer line in the full colour, inner lines towards the fill
        const int n = std::abs(p.bevel);
        const float px = std::max(1.0f, std::floor(u + 0.25f));
        Rgba light = resolve(p.bevelLight.set() ? p.bevelLight : Colour{Role::White}, pal, fade);
        Rgba dark = resolve(p.bevelDark.set() ? p.bevelDark : Colour{Role::Black}, pal, fade);
        if (p.bevel < 0)
            std::swap(light, dark);
        const Rgba face = p.fill.set() ? resolve(p.fill, pal, fade) : Rgba{0.5f, 0.5f, 0.5f, 1};
        for (int k = 0; k < n; k++) {
            const float t = n > 1 ? float(k) / float(n) : 0.0f, o0 = float(k) * px;
            auto mix = [&](const Rgba &a) {
                return Rgba{a.r + (face.r - a.r) * t, a.g + (face.g - a.g) * t, a.b + (face.b - a.b) * t, a.a};
            };
            const Rgba l = mix(light), d = mix(dark);
            o.rect(x + o0, y + o0, w - 2 * o0, px, l.r, l.g, l.b, l.a);              // top
            o.rect(x + o0, y + o0, px, h - 2 * o0, l.r, l.g, l.b, l.a);              // left
            o.rect(x + o0, y + h - o0 - px, w - 2 * o0, px, d.r, d.g, d.b, d.a);     // bottom
            o.rect(x + w - o0 - px, y + o0, px, h - 2 * o0, d.r, d.g, d.b, d.a);     // right
        }
    }
    if (p.border.set() && p.borderWidth > 0) {
        const Rgba c = resolve(p.border, pal, fade);
        const float bw = p.borderWidth * u;
        if (radius <= 0) { // pixel-snapped lines, like the PS3 look's hairline borders
            o.rect(x, y, w, bw, c.r, c.g, c.b, c.a);
            o.rect(x, y + h - bw, w, bw, c.r, c.g, c.b, c.a);
            o.rect(x, y + bw, bw, h - 2 * bw, c.r, c.g, c.b, c.a);
            o.rect(x + w - bw, y + bw, bw, h - 2 * bw, c.r, c.g, c.b, c.a);
        } else {
            o.box(x, y, w, h, c, c, radius, bw);
        }
    }
    if (p.edge.set() && p.edgeSide != Side::None && p.edgeWidth > 0) {
        const Rgba c = resolve(p.edge, pal, fade);
        const float ew = p.edgeWidth * u, in = p.edgeInset * u;
        switch (p.edgeSide) {
        case Side::Bottom: o.rect(x + in, y + h - ew, w - 2 * in, ew, c.r, c.g, c.b, c.a); break;
        case Side::Top: o.rect(x + in, y, w - 2 * in, ew, c.r, c.g, c.b, c.a); break;
        case Side::Left: o.rect(x, y + in, ew, h - 2 * in, c.r, c.g, c.b, c.a); break;
        case Side::None: break;
        }
    }
}

} // namespace ui
