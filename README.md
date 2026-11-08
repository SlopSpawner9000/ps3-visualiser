# PS3 Visualizer

> ## Human message:
> Couldn't find a PS3-like visualiser anywhere so i just had the slop generator make me one. It piggybanks off of RPCS3's data directory as i want to avoid any and all license conflicts :)
> ## AI slop readme follows:

A desktop music player that plays your music with the PlayStation 3's music
visualizer: the flowing "canyon" landscapes from the XMB, all 57 presets of them.

The visualizer is not an imitation. Its shaders, presets, normal map and system
fonts are loaded from **your own PS3 firmware files** at runtime:

- The RSX shader microcode is translated to GLSL on the fly.
- The CPU side (terrain physics, camera, render passes) was rewritten from the
  firmware's `qgl_canyon_app` module.

No Sony data is included in this repository.

Platforms: Linux (primary), Windows and macOS. Requires OpenGL 3.3.

## What you need

1. **PS3 firmware, installed.** The app reads `dev_flash` (the extracted system
   files), not the `.pup` file itself. The easiest way to get it is
   [RPCS3](https://rpcs3.net): *File → Install Firmware → PS3UPDAT.PUP*. The app
   then finds it automatically. It searches, in order:
   - `--firmware PATH` (`dev_flash` itself, or any folder containing it)
   - `$PS3VIS_FIRMWARE`
   - `./dev_flash`, `./PS3-HDD-DATA/dev_flash`, next to the executable and one level up
   - RPCS3's config dir: `~/.config/rpcs3` (and its Flatpak dir) on Linux,
     common portable-RPCS3 folders on Windows (`C:\RPCS3`, your user folder, …), `~/Library/Application Support/rpcs3` on macOS
2. **Build dependencies:** a C++20 compiler, CMake ≥ 3.20, SDL3, FFmpeg
   (libavformat, libavcodec, libswresample, libavutil) and pkg-config (not needed on Windows,
   see below). zlib is not needed: QRC containers are inflated with stb_image's decoder.

## Building

**Arch / CachyOS**
```sh
sudo pacman -S --needed cmake ninja sdl3 ffmpeg pkgconf
cmake -S . -B build -G Ninja && cmake --build build
```

**Debian / Ubuntu** (SDL3 is packaged from Ubuntu 25.04 / Debian 13)
```sh
sudo apt install cmake ninja-build pkg-config libsdl3-dev \
    libavformat-dev libavcodec-dev libswresample-dev libavutil-dev
cmake -S . -B build -G Ninja && cmake --build build
```

**Fedora**
```sh
sudo dnf install cmake ninja-build SDL3-devel ffmpeg-free-devel
cmake -S . -B build -G Ninja && cmake --build build
```

**Windows** (Git Bash, tools from [Scoop](https://scoop.sh): `scoop install git mingw cmake ninja`)
```sh
sh tools/windows-build.sh
```
It downloads the SDL3 MinGW development package and a prebuilt FFmpeg (BtbN, LGPL shared) into
`deps/`, builds with `-DFFMPEG_DIR=deps/ffmpeg` (no pkg-config needed), and bundles the exe and
its DLLs into `builds/ps3vis-windows/` + `.zip`. Step-by-step instructions are in SETUP.md. If
nothing is found at start-up, the player asks you to locate your RPCS3 folder and remembers it.

**macOS** (Homebrew)
```sh
brew install cmake ninja pkgconf sdl3 ffmpeg
cmake -S . -B build -G Ninja && cmake --build build
```

Steam Deck / SteamOS and other locked-down systems: build a Flatpak with `sh tools/flatpak-build.sh`, or
an AppImage to copy over with `sh tools/appimage-build.sh` (see SETUP.md).

## Using it

```sh
./build/ps3vis ~/Music                      # a folder (recursively), files, or both
./build/ps3vis --shuffle --fullscreen ~/Music
./build/ps3vis --preset Volcano01 song.flac  # start with another preset
./build/ps3vis --random-presets ~/Music      # a random preset per track
./build/ps3vis --list-presets
```

You can also start it empty and drag files or folders onto the window. It starts on
Blue02 and, like the PS3, slowly flows through its family (Blue02 → 03 → 04 → 01…); press V
to browse the other presets. Presets whose `TIME AHEAD` is below
the terrain's far edge (all Bir and Volvic, Voyage01–04/09, canyon11–14/19/20) shatter
the far terrain into tiles that drop into place. That effect is the firmware's own, but
by default (*Falling tiles effect: Auto*) it's only kept for the presets built from it:
the canyon12/13 and Voyage01/11 warps, the Volvic tile fields and Bir_B01's swarm (`TIME AHEAD` < 100).
Set it to On or Off in the menu. Anything FFmpeg
can decode plays (MP3, FLAC, Ogg/Opus, AAC/M4A, WAV, ATRAC, ...).

Default keys (all rebindable in the menu's **Keys** tab):

| Key | Action |
| --- | --- |
| Esc, M, right-click | menu: visual settings, playlist and keys |
| Space, K | play / pause |
| ← / → (J / L) | seek 10 s; with Shift, 60 s |
| ↑ / ↓ | volume |
| N / P (PageUp / PageDown) | next / previous track (P restarts the track if more than 3 s in) |
| V / Shift+V (] / [) | next / previous visual preset |
| F, F11, double-click | fullscreen |
| I | keep the track info on screen |
| S | shuffle on/off |
| H, / | help (lists your current bindings) |
| Q | quit |
| F12 | debug dump (off by default; see below) |

**Game controllers** (Xbox, PlayStation, Steam Deck/Controller and most others; Misc → *Controller
support*, on by default): Cross/A play/pause, D-pad seek (←/→) and volume (↑/↓), L1/R1 (LB/RB)
previous/next track, L2/R2 (LT/RT) previous/next preset, Triangle/Y track info, Square/X shuffle, R3
repeat, L3 fullscreen, Create/View help, Options/Menu the menu. The left stick works like the D-pad.
In the menu: D-pad to move and change, Cross/A to select, Circle/B back, L1/R1 switch tabs,
Triangle/Y deletes or clears. Every button is rebindable in the Keys tab. *Arrange on-screen elements*
needs a mouse, so it's greyed out while a controller is in use (Circle/B or Start leaves it if it's open). While a controller is the
input in use, the help screen, the Keys tab and the menu's hint line show its button glyphs (Xbox,
PlayStation or Steam style: *Button glyphs* Auto picks them from the controller). Keyboards that
pose as a controller (analog keyboards, "gamepad mode") are skipped: only devices SDL knows as
gamepads are used, known keyboard makers are ignored, and a device whose presses keep arriving
together with key presses is dropped for the session. The *Controller support* row shows which
controller is in use, or why none is.

When you move the mouse, on-screen controls fade in for a few seconds:
- a **‹ preset name ›** bar at the top, for cycling presets
- a transport bar at the bottom right: previous track, rewind 10 s, play/pause,
  forward 10 s, next track

Turn off *On-screen controls* in the menu for a clean picture.

### The menu

**Visualizer** tab: the first row picks the visualizer, *Waves (canyon)* or *Earth* (the PS3's
second one: the planet seen from orbit). The settings below it change with the choice.

*Earth* (first version: our own renderer on the firmware's textures, camera flights and sun/exposure
values; the atmosphere is an approximation, a faithful port is planned): *View* (the PS3's 30 camera
flights, also on V / the preset bar), *Next view after each flight*, *Flight speed*, *Clouds*, *Cloud
drift*, *Stars*, *Sun glare*, *Exposure*, and *React to music* (the PS3's Earth doesn't react; Gentle /
Strong pulse the light and glare with the bass). HDR works with both.

With *Waves* selected, ←/→ change values:
- preset, and whether to pick a new one for every track
- preset flow (off / within the preset's family / all presets), its interval and transition time
- camera motion on/off, drift distance and speed, and camera smoothing (how quickly the
  camera eases to where it should be; the original eases 10% per frame, ≈ 0.16 s)
- sensitivity, wave height, line width, glow, depth of field, field of view, speed
- volume, shuffle, repeat (off / whole playlist / one song), and when to show track info
- click or drag the progress bar to seek (the track info also shows while the mouse moves)
- *Song fades* (Misc tab, off by default) with *Fade duration* 10 ms – 10 s: songs crossfade. That
  long before a song ends, the next one (following shuffle/repeat) starts and fades in while the
  current one fades out (equal-power curves, two decoders mixed). The end is measured from the
  decoded audio, so there's no silent gap. Songs started by hand fade in. The track info and album art crossfade along
  with the music (*Crossfade track info*, on by default; off = the new info replaces the old at once). Next/previous also crossfade; pressing them again mid-fade cuts straight to the
  track, and for 3 s after that skips stay instant (for scrolling through songs).
- *Album art* (Misc tab, off by default; Shift+I cycles it): the song's embedded cover next to the track
  info, which moves right to make room. *Auto* shows and hides the cover together with the track info;
  *Always* keeps it on screen even when the track info hides itself
- *Colours* (Misc tab): accent (selection, section titles, keycaps, progress bar), text, background
  (menu, help and control bars) and highlight (selected row, buttons) colours, each a named choice or
  *Custom*, plus background opacity (30–130%). *Custom* adds hue, saturation and brightness strips:
  click or drag along them, or nudge with ←/→. *UI colours from visuals* themes all of it from the
  album art's colours (tracks with a cover) or the current preset's fog/line colours, fading along
  with preset changes. *Keep text readable* (on by default) lightens or darkens text and icons where a
  colour choice or the album art would make them hard to read (with all colours on *Theme* and colours from visuals off, the theme is
  used exactly as designed, no corrections)
- *When paused* (Misc tab): *Keep moving* (default; the flight carries on over the last sound), *Glide to
  a stop* (slows to a halt with friction and picks up again on play) or *Stop at once* (the picture holds)
- Volume: hold Shift for 1% steps (menu, arrow keys, mouse wheel)
- *Shuffle repeat gap* (Misc tab, default 33%): a track that played can't come back until that share
  of the playlist has played since (0% only prevents the same track twice in a row)
- *Menu size* (Misc tab): *Tall* uses the screen height minus 15% at the top and bottom (useful with a
  UI scale below 1)
- *High-quality blur* (Visualizer tab, off): the depth-of-field blur at full resolution with as many
  passes as needed for the same blur size as *Blur resolution*: smoother, much heavier on the GPU
- HDR: if HDR output fails at start-up (no Vulkan, memory sharing fails, ...), HDR is switched Off
  and a message says why; *On* with a display that offers no HDR output says so too. On Windows,
  *Auto* asks Vulkan whether the display offers HDR, since SDL rarely reports it there
- `settings.ini` carries `config_version`; a file written by a newer version is read but never
  overwritten. Version 2 moved Next/Previous track to PageUp/PageDown (older files are migrated)
- *Volume curve* (Misc tab): *Linear* (default) or *Logarithmic*, where 0–100% spans −50 to 0 dB so
  every step sounds equally big (the lowest 2% fade to silence; 100–150% still boosts up to 1.5×)
- Arrange mode: elements snap flush against each other; Shift+wheel resizes in fine (0.01) steps
- Shuffle plays every track once per round in random order (Previous/Next walk back and forth through
  what played), and a new round keeps the most recently heard half of the playlist for later
- H shows the keys grouped by Playback / Visuals & window / App, with each key drawn as a keycap
- Hovering a menu row for a second shows a tooltip explaining it (with a controller: resting the selection
  on a row for a second)
- The mouse pointer hides after 5 s without movement (not while the menu is open or while arranging)
- *Colours from album art* (off by default): shifts the preset's colours to the hues of the track's
  embedded cover (main hue → landscape and fog, a second hue → the line), keeping each preset's
  brightness; grey covers or tracks without embedded art keep the preset's own colours
- display: window, borderless fullscreen or exclusive fullscreen (remembered; F toggles)
- HDR: *Auto* (default) / *On* / *Off*, and *HDR highlight boost*. OpenGL can't output HDR on
  most drivers, so in HDR mode the frames are handed to Vulkan (shared GPU memory, no copies
  through the CPU), which shows them through an HDR10 or scRGB swapchain. *Auto* uses this path
  when a display has HDR switched on at start-up; it follows HDR being toggled or the window
  moving between displays. In HDR the glow can go brighter than SDR white (up to the boost);
  text and menus stay at SDR white. Switching between the OpenGL and Vulkan paths needs a
  restart. Vulkan is loaded at runtime; the Khronos headers are in `third_party/vulkan`.
- UI scale and the positions of the track info, playback buttons and preset bar; mouse wheel
  volume on/off

Switching presets quickly (clicking the arrows repeatedly or holding V) only shows the names
as you go. The visualizer switches once you've paused for 80 ms, so it doesn't restart for
every preset you skip past.

**Custom** tab: your own preset, listed as *Custom* after the firmware presets.
- *Use custom preset now* switches to it. While it's active, every change applies live.
- *Include in preset rotation* (off by default) lets V, the on-screen arrows, preset
  flow and random picks land on it. It's always reachable from this tab.
- *Copy values from …* takes the values of the last firmware preset you used.
- All 46 firmware parameters follow, grouped like the firmware's menus (Camera, Terrain,
  Line, Colour, Fog, Blur), with the video feedback in its own *Feedback (video trails)* group:
  FEEDBACK (how much of the previous frame is mixed in, 0–1), FEEDBACK X/Y (stretch per frame)
  and FEEDBACK ROT (turn per frame, in radians; ±3.14 gives a mirrored copy). ←/→ adjusts a value; hold Shift for 10× steps. It starts out
  as a copy of canyon01 and is saved with your settings.

**Presets** tab: share custom presets. *Export* puts the current custom preset on the clipboard or
in a `.ps3vis` file, as one line `PS3VIS-PRESET1:<base64>`; the base64 holds `name=…` and one
`PARAMETER=value` line per firmware parameter, so it survives chats and e-mail. *Import* (clipboard
or files) adds presets to the list kept in `<settings folder>/presets/`; Enter on one replaces the
custom preset with it and switches to it. Each saved preset has **Rename** (or F2: type, Enter keeps,
Esc cancels) and **Delete** (or Del; asks first) buttons. The list shows every valid preset file in that
folder, including ones you copy in yourself (`.ps3vis`, `.txt` or no extension, also in subfolders up
to two levels deep). Files are checked first: at most 64 KB, a `PS3VIS-PRESET1:` preset with at least
one known parameter; numbers that aren't finite are dropped, the rest clamped, and names cleaned of
control characters and cut at 60 characters. Anything else is ignored.

**EQ** tab: a basic 10-band equalizer (31 Hz–16 kHz, ±12 dB in 1 dB steps) with a preamp.
- *Equalizer* turns it on or off (off by default); *Reset to flat* zeroes every band.
- *Applies to*: *Audio only* (default) changes what you hear but leaves the visualizer
  reacting to the original music, as the PS3 would; *Audio and visualizer* also feeds the
  equalized sound to the landscape (for example, cut the bass to see more treble movement);
  *Visualizer only* leaves the sound untouched and only reshapes what the landscape reacts to.
- Boosts can clip loud music; lower the *Preamp* if it distorts.
- With *Debug spectrum* on, red squares show the EQ's boost/cut at each landscape column (centre
  line = 0 dB, edges = ±12 dB; dimmed when it only affects the sound), with the mode and the
  changed bands listed underneath.

Under the menu panel are **Quit** and **Reset to defaults** (mouse, or ↓ past the last row then
←/→ and Enter). Reset asks first (*Cancel* is pre-selected) and puts every setting back to its
default: visuals, layout, colours, keys, equalizer, debug options. It keeps the playlist, the Custom
preset, the saved preset library and the volume.

**Keys** tab: two bindings per action (with Shift/Ctrl/Alt). Use ←/→ to pick a slot,
Enter or click to rebind, Del to clear, and *Reset keys to defaults*. A key combination
belongs to one action only, so assigning it elsewhere removes it here. Esc always opens
the menu, whatever the bindings.

**Playlist** tab:
- add files or folders: the system's file dialogs, or a built-in browser inside the menu (Misc →
  *File browser*: System dialogs / Built-in / Auto, the default, which uses the built-in one while a
  controller is in use). The built-in browser lists folders first, then music files (hidden files are
  skipped); Enter / Cross/A opens a folder or adds a track, *Add everything in this folder* adds it with
  its subfolders, *Places…* jumps to home, Music, Desktop, Downloads and mounted drives / SD cards (drive
  letters on Windows), Backspace / ← / D-pad ← goes up, Esc / Circle/B returns to the playlist. It
  remembers the last folder
- Enter or double-click to play, Del to remove, Shift+↑/↓ to reorder, clear the playlist

Settings are saved to `settings.ini` in your user data folder
(`~/.local/share/ps3vis/PS3 Visualizer/` on Linux) and restored on the next start.
Command-line options override them.

`--gain X` sets the sensitivity (default 3).

### Offline rendering

`--screenshot out.png` renders without opening a visible window or playing audio.
It simulates `--frames N` frames at 60 fps from `--start S` seconds of the first file,
at `--size WxH`. `--overlay` adds the track info, and `--every K` with a `%d` in the
file name saves a frame sequence.

```sh
./build/ps3vis --preset canyon01 --overlay --start 30 --screenshot shot.png song.mp3
```

## How accurate is it?

The terrain simulation, parameter handling, camera and render passes follow the
original code. The shaders are the original shaders. Known differences:

- **Spectrum input.** By default the app reproduces the PS3's own spectrum feed, decoded
  from `soundvisualizer_plugin`. It takes the last 512 samples at 48 kHz, converted from other
  rates by the PS3's crude sample-and-hold. It applies a very short Gaussian window, an FFT and
  smoothing across frequency, maps the result onto a 4-octave exponential scale, and
  log-compresses it. The visualizer then boosts the middle columns per preset and divides by
  the peak only when it exceeds 1, as the original does. Quiet music therefore gives calm
  terrain, as on the PS3. The terrain centre shows the very highest frequencies, which MP3s
  mostly lack. The sample-and-hold conversion still puts some energy there, as on the console.
  Esc → *Music response* → *PS3, smooth bass* keeps the PS3 feed but steadies deep bass. The PS3's
  ~1 ms analysis window catches a different point of each bass wave every frame, so bass columns
  jitter. This mode averages the bass columns and the row's scaling over 50–100 ms.
  *Enhanced* switches to this app's earlier feed. That feed uses log
  bands from 40 Hz to 5 kHz, normalised to the song's loudness, so every preset moves a lot
  even with quiet music. *Sensitivity* scales the input level (3 = as on the PS3).
  Like the PS3, it reads the audio a little ahead of playback, by a per-preset amount taken from
  the PS3's own table (270 ms for Blue01, up to ~1 s), so a sound's ripple reaches the camera's focus
  when you hear it. *Visual sync* in the menu adjusts this (for example for Bluetooth headphones).
  Compared with recordings of the real PS3 (Blue01–04, three songs), the PS3 feed matches
  clearly better. Blue04's light rays and the bands near the camera in Blue02 are still weaker
  here; they are probably rendering differences (line effect or glow).
- **Strip stitching.** On the PS3 each terrain strip is flat along its depth, which
  leaves hairline cracks wherever the next strip is higher. Here each strip's far edge
  is joined to the next strip, so the surface is continuous.
- **Glow.** The PS3 adds a soft bloom through a pyramid of the firmware's `ConeFilter`
  shader. The code that chains and blends it lives in the shared `qglbase` engine and
  wasn't decoded. This app builds the pyramid from the same shader, but the threshold,
  level weights and blend are tuned by eye. Adjust it under *Glow* in the menu.
- **Blur strength and framing.** The size of the PS3's offscreen blur buffers isn't known;
  this app blurs at a quarter of the window resolution by default. *Blur resolution* in the
  menu picks full, 1/2, 1/4 or 1/8 (lower = softer and cheaper), and *Depth of field*
  sets how strong the blur is.
  `ZOOM` is the vertical field of view of a 16:9 frame; wider windows keep the 16:9
  framing by cropping top and bottom. *Field of view* in the menu scales it.
- **Preset flow and camera motion.** The firmware ships its presets in numbered families
  (Volcano01–04, Blue01–04, …) whose members mostly differ in camera position. The code
  that steps through them wasn't found: the name table in `qgl_gaia_app` is never
  referenced, and the firmware's script interpreter ships no scripts. A recording of the
  real PS3 visualizer (in RPCS3) settled the behaviour instead:
  - it starts on Blue02
  - it steps Blue02 → Blue03 → Blue04 → Blue01 → … every 17–28 s, blending over ~8 s

  *Preset flow* (on by default) reproduces that: 22 s on average, randomised ±25 %,
  with 8 s transitions, all adjustable.

  *Camera drift* (my addition) slides the camera slowly left and right without turning it.
  The recording shows no large sideways movement within a preset, so set it to 0 if you
  want the stricter look.
- **Keep camera above terrain** (on by default; my addition) raises the camera (up to 30
  units; the target stays put, so it tilts down) when hills would reach it. Low-flying presets such as Voyage03 otherwise end up inside the terrain.
- The visualizer runs at a fixed 60 Hz, whatever your display's refresh rate, because
  its feedback effects are tuned per frame.
- The Earth visualizer (`qgl_gaia_app`, `earth.qrc`) isn't a full port: it is our own renderer on the
  firmware's textures, camera flights and sun/exposure values (see *Earth* under the Visualizer tab above).

`docs/NOTES.md` describes the file formats and what was recovered from the firmware.
`tools/re/` holds the Python tools used for that.

`docs/THEMES.md` explains the UI themes, their fonts and how to write your own (Esc > Misc > Theme). The
`fonts/` folder holds the free fonts the themes fall back on, with their licences.

## Debugging

Every start writes `startup.log` next to `settings.ini` (overwritten each time): the session (desktop,
Wayland/X11, gamescope / Steam Game Mode, AppImage), where the firmware was looked for, the HDR and
graphics decisions and any error. Useful when the player fails to start somewhere it can't show a message,
such as the Steam Deck's Game Mode: read it afterwards in Desktop Mode.

**F12** (rebindable; switch it on with *Debug dump key* in the menu first) writes a snapshot to
`~/.local/share/ps3vis/PS3 Visualizer/dumps/dump-<date>-<time>/`:
- `state.txt`: app, audio, playlist and visualizer state, live preset values, spectrum columns; its
  `build =` line says which build script made the program (linux-build, windows-build, flatpak,
  appimage, or manual for a plain CMake build), the build type and time, and whether it's running as a
  Flatpak, an AppImage or a plain executable
- `history.csv`: the last ~15 s, frame by frame
- `frame.png`, `log.txt`, and the settings (in memory and on disk)

Press it right when something looks wrong and attach the folder to the bug report.

Debug tab options:
- *Hardware info panel*: CPU, RAM (and free), GPU, output path (OpenGL / Vulkan HDR), display, audio
  output, window system and controller in use, top right with the other panels
- *Show current values* / *Show valid values* (with the settings keys): each key line also gets its type
  and value and what it accepts, e.g. `[settings.ini] -> repeat  <int> 1; 0-2 (off, playlist, one song)`
- *Show settings keys in tooltips* (off by default): every tooltip also names the option's key in
  `settings.ini` under its description, e.g. `[settings.ini] -> song_fades` (Keys tab: `key.<action>`, or
  `pad.<action>` while a controller is in use; Custom tab: `custom.<PARAMETER>`)
- *Dump: hide personal info* (on by default) removes your user and computer names, IP/MAC and e-mail
  addresses from the dump, and paths in your home folder become `~`.
- *Dump: hardware details* / *Dump: software details* add `[hardware]` (CPU, GPU, RAM, displays,
  audio devices) and `[software]` (OS, desktop session, SDL/OpenGL/FFmpeg versions, compiler)
  sections to `state.txt`.
- *Stress tests* (never saved; they reset on restart, and a red banner lists the active ones; the frame rate limit
  alone gets no banner, but is listed when other tests run): frame
  rate limit (VSync / Unlocked / caps at 30–240 fps; unlocked uses an immediate present mode with HDR
  too), visualizer update rate (60 Hz normal / every frame / 120 / 240 Hz; faster changes the trails),
  render passes per update (1–8), render scale (25–300%), preset storm, seek storm, random frame
  stalls, and background CPU load. *Stop all stress tests* turns them all off. Debug dumps list them.
- *Frame rate display* and *Resource usage display* (Off / Numbers / Numbers + graph) show the
  refresh rate, fps and frame times, and the player's CPU, RAM, I/O and thread count, top right.
  The frame rate panel also shows the 1% low (average fps of the slowest 1% of frames), minimum and
  maximum fps over the last *Frame stats window* (1 / 5 / 10 / 30 / 60 s, default 10 s).

- `PS3VIS_DUMP_SHADERS=1` prints the GLSL generated from each RSX program.
- `PS3VIS_DEBUG=color` shows the raw terrain pass; `PS3VIS_DEBUG=amount` shows the blur-amount pass.
- `PS3VIS_DEBUG_SPECTRUM=1` logs the spectrum levels reaching the terrain.

## Legal

This is an independent, non-commercial project, not affiliated with or endorsed by
Sony Interactive Entertainment. PlayStation is a trademark of Sony Interactive
Entertainment. You must supply firmware files you obtained yourself; none are
distributed here.

ps3vis's own code is MIT-licensed, see [`LICENSE`](LICENSE). That file also
lists the separately-licensed third-party components it builds against or
bundles (SDL3, FFmpeg, Vulkan headers, stb libraries, and the fallback UI
fonts in [`fonts/`](fonts/README.md)) — none of which are GPL/copyleft.
