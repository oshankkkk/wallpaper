# Slimesim wallpaper

Physarum polycephalum simulation running as a live Wayland wallpaper. Thousands of agents create and follow glowing trails, forming evolving networks across the desktop. Built in a single C file without a game library, rendering directly through Wayland at native resolution.

## Requirements

A Wayland compositor that supports the `wlr-layer-shell` and `wp_viewporter`
protocols. That includes Hyprland, Sway, river, niri, labwc, KDE Plasma and COSMIC.
**GNOME does not support layer-shell, so it will not work there.**

Build dependencies on Arch Linux:

```
sudo pacman -S base-devel cmake wayland wayland-protocols wlr-protocols
```

## Build

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The binary `build/slime-wallpaper`.

If CMake can't find `wlr-layer-shell-unstable-v1.xml`, set `WLR_PROTOCOLS` in
`CMakeLists.txt` to the directory that contains `unstable/wlr-layer-shell-unstable-v1.xml`
(on Arch that is `/usr/share/wlr-protocols`).

## Run

```
./build/slime-wallpaper                      # looks for slimewallpaper.conf (see below)
./build/slime-wallpaper /path/to/my.conf     # use a specific config file
```

Stop it with Ctrl+C, or `pkill slime-wallpaper` if it runs in the background.

### Autostart on Hyprland

Add this to `~/.config/hypr/hyprland.lua`:

```lua
hl.on("hyprland.start", function()
    hl.exec_cmd("/home/you/slime-wallpaper/build/slime-wallpaper")
end)
```

If you start Hyprland through uwsm:

```lua
hl.on("hyprland.start", function()
    hl.exec_cmd("uwsm app -- /home/you/slime-wallpaper/build/slime-wallpaper")
end)
```

The `hyprland.start` event only runs at startup, so after you rebuild or edit the config, restart it by hand:

```
pkill slime-wallpaper; /home/you/slime-wallpaper/build/slime-wallpaper &
```

On other compositors, add the same command to that compositor's autostart
(for example `exec /path/to/slime-wallpaper` in Sway).

## Configuration

Settings live in `slimewallpaper.conf` and are read once at startup. **Restart the
program to apply changes.**

Example (the Rose Pine palette):

```
background   (25, 23, 36)
agentcolor1  (235, 111, 146)
agentcolor2  (38, 35, 58)
speed        1
agentcount  2000
```


| Key          | Default          | What it does |
|--------------|------------------|--------------|
| `background` | `(0, 0, 0)`      | Color of the empty space |
| `agentcolor1`| `(30, 150, 255)` | Color of the first slime species |
| `agentcolor2`| `(0, 255, 255)`  | Color of the second slime species |
| `speed`      | `1`              | Speed multiplier: `1` = normal, `0.5` = half speed, `2` = double |
| `agentcount` | `2000`           | Increase or decrease the number of slime agents |


**Where it looks for the file** (the first one found is used):
1. The path passed as the first argument
2. `./slimewallpaper.conf` in the current directory
3. `~/.config/slimewallpaper/slimewallpaper.conf` (or `$XDG_CONFIG_HOME/slimewallpaper/slimewallpaper.conf`)

If no file is found, the built-in defaults are used. The program prints which file it
loaded when it starts.

**File format:**

- One setting per line, in the form `name value`.
- Names are not case-sensitive.
- Brackets, commas and `=` are optional, so `(1, 2, 3)`, `1 2 3` and `= (1,2,3)` all work.
- Everything after `#` or `//` is a comment.
- Colors above 255 are clamped. `speed` is clamped to between 0.1 and 20.
- A malformed line or unknown name prints a warning with the line number and is skipped.

## Tuning in the source

These are `#define`s at the top of `slime_wallpaper.c`. Change them and rebuild.

| Setting          | Default | Effect                                                                        |
|------------------|---------|-------------------------------------------------------------------------------|
| `FPS_LIMIT`      | `60`    | Frame rate cap. `30` roughly halves CPU use.                                  |
| `GRIDSIZE`       | `1`     | Screen pixels per simulation cell. `1` is crisp at native resolution; `2` is lighter but chunkier. |
| `AGENTS_REF`     | `2000`  | Agents on a 1200x800 screen. The count scales with screen area, so a 1920x1080 screen gets about 4,300. |
| `SENSEDISTANCE`  | `16`    | How far ahead agents look for trails, in pixels                               |
| `DIFFUSE_WEIGHT` | `0.33`  | How much trails spread out. Higher gives thicker, softer trails.              |
| `DECAY_RATE`     | `0.11`  | How fast trails fade. Lower gives longer-lasting trails.                      |
| `TURNSPEED`      | `0.2`   | Angle of the left and right sensors, in radians                               |

At 1920x1080 one simulation step took about 6 ms on a single core of a 2.8 GHz server CPU,
well inside the 16.7 ms budget for 60 FPS.

## How it works

- The simulation runs on the CPU and writes its pixels into a shared-memory buffer
  (`wl_shm`), double-buffered.
- A `wlr-layer-shell` surface on the background layer shows that buffer behind every
  window, covering the whole output.
- `wp_viewporter` tells the compositor the surface size. With `GRIDSIZE 1` the buffer is
  the same size as the screen, so it is shown 1:1 with no scaling blur.
- Frames are paced by Wayland frame callbacks and capped at `FPS_LIMIT`.
- The blur is a 3x3 box blur done as two 1D passes on byte arrays, and each pixel color
  comes from a precomputed lookup table.

## Limitations

- **One monitor.** The program creates a single surface and the compositor chooses which
  output gets it.
- **Fractional scaling.** The buffer matches the logical screen size. With scaling such
  as 1.25x on Hyprland, the compositor scales it and it will not be pixel-perfect.
- **No input.** Clicks and keys do not reach the simulation, as with any wallpaper.
- **Idle behavior.** The program relies on frame callbacks, so it should stop working
  when the wallpaper is hidden, for example under a fullscreen window. Exactly when
  callbacks stop depends on the compositor.

## Troubleshooting

| Problem                                                      | What to try                                                                 |
|--------------------------------------------------------------|-----------------------------------------------------------------------------|
| `Compositor doesn't support wlr-layer-shell`                 | You are probably on GNOME. This needs a layer-shell compositor.             |
| Black screen or two wallpapers fighting                      | Stop other wallpaper daemons (hyprpaper, swww, swaybg).                     |
| Build errors about undeclared `zwlr_...` functions           | The generated protocol headers are stale. Delete the `build` directory and rebuild. |
| Colors or speed don't change                                 | Restart the program. Check the terminal output to see which file it loaded and for warnings. |
| Want to confirm it is running on Hyprland                    | `hyprctl layers` should list a `slime-wallpaper` surface on the background layer. |
| High CPU use                                                 | Set `FPS_LIMIT` to `30`, or `GRIDSIZE` to `2`, and rebuild.                 |


> See https://github.com/oshankkkk/slimesim for the simulation details


