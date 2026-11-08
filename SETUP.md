# PS3 Visualizer: setup guide

This gets you from the files you were sent to music playing with the PS3 visualizer.
It takes about 10 minutes, mostly downloads.

**You need:** a computer with an OpenGL 3.3 graphics card (anything from the last ~12
years), Linux (best tested), Windows or macOS, and about 1 GB of free space for the PS3
system software.

---

## 1. Get the PS3 system software (one-time)

The visualizer's graphics, presets and fonts come from the official PlayStation 3 system
software. They aren't included in what you were sent; you install them yourself, for free.

1. **Install RPCS3**, the PS3 emulator: https://rpcs3.net/download
   (on Arch/CachyOS: `sudo pacman -S rpcs3`, or use the AppImage/Flatpak).
2. **Download the PS3 system software** (`PS3UPDAT.PUP`, about 200 MB) from Sony:
   https://www.playstation.com/support/hardware/ps3/system-software/
3. Open RPCS3, choose **File → Install Firmware**, and pick `PS3UPDAT.PUP`. Wait for it to finish.

That's all. The player looks for RPCS3's copy in the usual places:
- Linux and the Steam Deck: `~/.config/rpcs3/dev_flash` (also the Flatpak RPCS3's folder). If RPCS3's
  `dev_flash` was moved elsewhere (its `vfs.yml`, e.g. by EmuDeck onto an SD card), the player follows that
- Windows: RPCS3 is a portable folder, so `dev_flash` sits next to `rpcs3.exe`. The player
  checks common spots (`C:\RPCS3`, `C:\Games\rpcs3`, `rpcs3` in your user, Desktop or
  Downloads folder, Program Files, `%APPDATA%\rpcs3`).
- macOS: `~/Library/Application Support/rpcs3/dev_flash`

If it can't find it, the player asks you to **locate your RPCS3 folder** (the one with
`rpcs3.exe` or `dev_flash` in it) and remembers your choice. You don't need to keep RPCS3
open or even installed afterwards; only the `dev_flash` folder is used.

## 2. Build the player

Unpack the folder you were sent, open a terminal inside it, and run the lines for your
system.

**Linux, the easy way:** run
```sh
sh tools/linux-build.sh
```
It checks for everything the build needs, offers to install what's missing (Arch/CachyOS/Manjaro,
Debian/Ubuntu, Fedora and openSUSE; it shows the command and asks first), builds the player into
`build/`, and tells you whether it found the PS3 system software. Add `--install` to also put it in
`~/.local/bin` with a *PS3 Visualizer* entry in your app menu, `--yes` to install packages without
asking, or `--clean` to rebuild from scratch. Or do it by hand:

**Arch / CachyOS / Manjaro**
```sh
sudo pacman -S --needed base-devel cmake ninja sdl3 ffmpeg pkgconf
cmake -S . -B build -G Ninja && cmake --build build
```

**Debian / Ubuntu** (Ubuntu 25.04+ or Debian 13+, for SDL3)
```sh
sudo apt install build-essential cmake ninja-build pkg-config libsdl3-dev \
    libavformat-dev libavcodec-dev libswresample-dev libavutil-dev
cmake -S . -B build -G Ninja && cmake --build build
```

**Fedora**
```sh
sudo dnf install gcc-c++ cmake ninja-build SDL3-devel ffmpeg-free-devel
cmake -S . -B build -G Ninja && cmake --build build
```

**Windows 10/11** (64-bit). Not tested on real Windows yet, so please report anything odd.

1. Install [Scoop](https://scoop.sh): open **PowerShell** (not as administrator) and run
   ```powershell
   Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
   Invoke-RestMethod -Uri https://get.scoop.sh | Invoke-Expression
   ```
2. Still in PowerShell, install Git (which includes **Git Bash**), the compiler and the build tools:
   ```powershell
   scoop install git mingw cmake ninja
   ```
3. Open **Git Bash** (Start menu → *Git Bash*) and go to the unpacked folder. Windows drives are
   under `/c/`, `/d/`, … For example, if you unpacked it to `C:\Users\you\Downloads\ps3vis`:
   ```sh
   cd /c/Users/you/Downloads/ps3vis
   sh tools/windows-build.sh
   ```
   The script downloads the two libraries the player needs (SDL3 from libsdl.org's GitHub and a
   prebuilt FFmpeg from BtbN's FFmpeg-Builds, about 60 MB in total, into `deps\`), builds the
   player, and collects everything into `builds\ps3vis-windows\` and `builds\ps3vis-windows.zip`.
4. Start it by double-clicking **`builds\ps3vis-windows\ps3vis.exe`**. When sharing the zip,
   the other person has to *extract* it first (right-click → *Extract All*). Double-clicking the
   exe inside the zip without extracting fails with "…dll was not found". That folder (or the zip) runs
   on any 64-bit Windows 10/11 PC without installing anything else, so it's also what you send
   to friends.

On Windows the player is a normal windowed app: double-click it, then drag music files or
folders onto the window, or add them from Esc → *Playlist*. You can also drop a folder onto
`ps3vis.exe` itself, or make a shortcut whose target ends in your music folder, for example
`"C:\…\ps3vis.exe" "D:\Music"`.

**Steam Deck / SteamOS / any locked-down Linux** (Flatpak): on systems where you can't install
compilers or libraries (SteamOS's system is read-only), build a Flatpak instead. It needs only
`flatpak`, which SteamOS and most desktops already have. On the Deck, switch to *Desktop Mode* and
open *Konsole* in the unpacked folder (or build on any Linux PC and copy the `.flatpak` file over):
```sh
sh tools/flatpak-build.sh --install
```
The first build downloads the Flatpak build tools and the Freedesktop runtime (about 1 GB, once) and
builds SDL3 and FFmpeg inside it, so it takes a while. The result is `builds/ps3vis-<date>-<n>.flatpak`;
`--install` also installs it for your user (*PS3 Visualizer* appears in the app menu). On another
machine install the file with `flatpak install --user ps3vis-<date>-<n>.flatpak`. Start it from the
app menu, or with `flatpak run io.github.ps3vis.Visualizer ~/Music`. To use it in the Deck's Game
Mode, add it to Steam (Steam → *Add a Non-Steam Game*, pick *PS3 Visualizer*); controllers work.

The Flatpak may read only what it needs: RPCS3's firmware folders (native and Flatpak RPCS3), your
*Music* folder and SD cards / USB drives (`/run/media`), all read-only. Anything elsewhere can be
opened with *Add files* / *Add folder* (the system file chooser grants access to what you pick). Its
settings live in `~/.var/app/io.github.ps3vis.Visualizer/`.

**AppImage** (build here, copy the one file to the target): an alternative to the Flatpak when you'd
rather not install anything on the target machine. On your Linux PC (needs only `flatpak` and `curl`):
```sh
sh tools/appimage-build.sh
```
The build runs inside the Freedesktop 23.08 SDK (installed for your user on first use, about 1.6 GB),
so the result runs on systems with glibc 2.38 or newer: SteamOS 3.5+, Ubuntu 24.04+, Debian 13+,
Fedora 39+ and anything newer. SDL3 and FFmpeg are bundled; besides those it needs only the C library,
libm and zlib. The result is `builds/ps3vis-<date>-<n>-x86_64.AppImage` (about 14 MB). Copy it to the
target and **make it executable before anything else** (`chmod +x <file>` in Konsole, or *Properties →
Permissions → Is executable* in the Deck's file manager). Copying usually drops that permission: Desktop
Mode offers to fix it when you open the file, but Steam's Game Mode can't, and the file then simply
doesn't start. If it won't start, look in `~/.local/share/ps3vis/PS3 Visualizer/`: `apprun.log` (written
by the AppImage's launcher, with any error from before the program runs) and `startup.log` (the program's
first steps). If neither exists, the AppImage itself never ran.

The same build also makes `builds/ps3vis-<date>-<n>-x86_64-portable.tar.gz`: the same program as a plain
folder. It doesn't mount itself like an AppImage (which needs FUSE), so it also starts where that isn't
available, for example from a Steam shortcut in Game Mode. Unpack it on the target (`tar -xzf <file>`, or
*Extract* in the file manager; this keeps it executable) and start `ps3vis/ps3vis.sh` (for Steam: *Add a
Non-Steam Game*, then point the shortcut's target at `ps3vis.sh`). It reads the firmware from RPCS3's folders like the normal build; its settings go
to `~/.local/share/ps3vis/`. For the Deck's Game Mode, add it to Steam as a non-Steam game.

**macOS** (untested), with [Homebrew](https://brew.sh):
```sh
brew install cmake ninja pkgconf sdl3 ffmpeg
cmake -S . -B build -G Ninja && cmake --build build
```
Or `sh tools/macos-build.sh` for a ready-to-share `ps3vis.app`, bundled with every library it
needs (see `--help`): it installs the same Homebrew packages (plus `dylibbundler`) and zips the
result into `builds/ps3vis-macos.zip`. It isn't notarized (that needs a paid Apple Developer ID),
so Gatekeeper calls it "unidentified developer" on first launch; right-click it and choose Open.

## 3. Play music

```sh
./build/ps3vis ~/Music                # a folder (searched recursively)
./build/ps3vis song.flac other.mp3    # or files
./build/ps3vis                        # or start empty and drag files onto the window
```
(On Windows, see the end of the Windows build steps above.)

Anything common plays: MP3, FLAC, Ogg/Opus, AAC/M4A, WAV, and more.

**The basics:**
- **Esc or right-click** opens the menu. Tabs, left to right:
  - *Playlist*: add, remove and reorder tracks
  - *Visualizer*: presets, camera, music response, look, HDR
  - *Custom*: build your own preset
  - *Presets*: save, import and export custom presets (clipboard or file); **Enter** uses one, **Del** deletes
  - *EQ*: equalizer, for the sound, the visuals, or both
  - *Misc*: playback (volume, shuffle, repeat, song fades, mouse wheel), track info & album art (with its
    own scale), on-screen layout (UI scale, positions, vertical playback buttons, *Arrange on-screen
    elements*), window
  - *Keys*: rebind any key
  - *Debug*: debug dump key (F12), debug spectrum, extensive logging
- **Arrange on-screen elements** (Misc tab): drag the track info, playback buttons and preset bar
  anywhere. They snap to the window edges, the centre and each other (lined up or side by side);
  hold **Shift** to place them freely. The **mouse wheel** over an element resizes it (0.5×–2.5×).
  **R** resets positions and sizes, **Enter/Esc** finishes.
  Picking a position in the menu again replaces a dragged one.
- **Move the mouse near the bottom edge** to fade in the on-screen controls: the preset arrows
  (bottom centre by default) and the playback buttons bottom-right (shuffle, previous, rewind, play/pause, forward, next, repeat). Shuffle and repeat
  are dim when off; repeat shows a small **1** when it repeats one song.
- **Click or drag the progress bar** under the song title to jump to any point in the song.
- **Scroll the mouse wheel** to change the volume (can be turned off in the menu).
- In the menu, **drag the scrollbar** (or click anywhere along it) to jump through long lists such
  as a big playlist; the mouse wheel scrolls too.
- **Space** pause, **←/→** seek, **↑/↓** volume, **N/P** next/previous track,
  **V / Shift+V** next/previous visual preset, **I / Shift+I** track info / album art (off → auto → always), **S** shuffle, **R** repeat (off → playlist →
  one song), **F** fullscreen, **H** shows all keys, **Q** quits.
- Esc → *Misc* → *Display* picks *Window*, *Borderless fullscreen* or *Exclusive fullscreen*
  (remembered; **F** or a double-click switches between the window and your chosen fullscreen).
- **HDR monitors:** turn HDR on for your display (Windows: Settings → Display → *Use HDR*;
  Linux: in your desktop's display settings or compositor config), then start the player. With
  *HDR: Auto* (the default) it switches to HDR output by itself; the menu row shows what it's
  doing (e.g. *Auto: HDR10*). *HDR highlight boost* sets how bright the glow may get.
- Esc → *Misc* has *UI scale* (0.5×–2×) and where things go: *Track info position* (any
  corner), *Playback buttons position* (bottom right/centre/left) and *Preset bar position*
  (top/bottom). The track info moves up automatically if it would overlap the buttons.

Settings are remembered between runs.

**Tips:**
- Start with the *canyon* presets (canyon01 is the classic orange one). Some others are
  deliberately abstract: warp tunnels, floating tiles, pale haze.
- Like on the PS3, quiet music (solo piano, for example) gives a calm landscape. For more
  movement, raise *Sensitivity* or set *Music response* to *Enhanced*. For bass-heavy music,
  *PS3, smooth bass* makes the bass swell steadily instead of flickering. If it's too spiky,
  lower it.
- Esc → *Visualizer* → *Colours from album art* tints each preset with the colours of the song's
  embedded cover; songs without cover art keep the normal preset colours.
- The *Custom* tab lets you copy any preset and tweak all 46 of its values live.

## Troubleshooting

| Problem | Fix |
| --- | --- |
| "PS3 system software files weren't found" | Install the firmware in RPCS3 (step 1), then choose *Locate RPCS3 folder…* and pick the folder containing `rpcs3.exe` / `dev_flash`. Or start it with `--firmware /path/to/dev_flash` |
| Windows: "…dll was not found" (avcodec, avformat, avutil, swresample, SDL3) | The DLLs must sit next to `ps3vis.exe`. Extract the zip first instead of running from inside it, and keep the whole `ps3vis-windows` folder together. If you built it yourself, re-run `sh tools/windows-build.sh` (it now checks the bundle and also copies the DLLs into `build-win\`) |
| Windows: the build script says something is missing | Run `scoop install git mingw cmake ninja` in PowerShell, then open a *new* Git Bash window |
| Windows: SmartScreen "Windows protected your PC" | The program isn't code-signed. Choose *More info → Run anyway* |
| Build fails: SDL3 not found | Your distribution is too old for SDL3; build SDL3 from https://github.com/libsdl-org/SDL or use a newer release |
| Window opens but stays black, or "OpenGL 3.3 is required" | Update your graphics drivers |
| No sound | Check the system volume and output device; the player uses the default output |
| Visuals seem out of sync with the music | Adjust *Visual sync* in the Esc menu (raise it for wireless headphones). *Debug spectrum* shows the green *audio* meter (what's playing now) and the bars (what the landscape is fed, a little ahead, like on the PS3) |
| Something looks broken | Turn on Esc → *Debug* → *Debug dump key* (and *Extensive logging* for a detailed trail of what happened), press **F12** when it happens, and send the newest folder from `dumps` in the settings folder: `~/.local/share/ps3vis/PS3 Visualizer/` on Linux, `%APPDATA%\ps3vis\PS3 Visualizer\` on Windows |

`./build/ps3vis --help` lists all command-line options (on Windows, run it from Git Bash to see
the text). See `README.md` for details on how
the visualizer was rebuilt from the PS3 firmware.
