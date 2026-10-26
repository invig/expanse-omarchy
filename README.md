# Expanse for Hyprland

[Expanse](https://github.com/invig/expanse)'s infinite canvas as a Hyprland plugin: your windows live at fixed spots on one endless canvas, you zoom out to see all of them, and click one to glide to it at 100%.

Early: zoom out and pick, and hopping between windows. Built and tested on Omarchy (Hyprland 0.56.2, aarch64) in [Try Omarchy](https://github.com/omacom/try-omarchy).

![Zoomed out: every window on the canvas, the one under the pointer ringed](docs/overview.jpg)

## Keys

| Keys | Action |
| --- | --- |
| <kbd>Alt</kbd> + <kbd>`</kbd> | Zoom out to every window, and back |
| <kbd>Ctrl</kbd> + arrows | Glide to the nearest window that way, centred, and focus it. Zoomed out, the highlight moves and the view follows |
| Click a window that isn't fully on screen | Glide to centre it |
| Click a window (zoomed out) | Glide to it at 100% and focus it |
| <kbd>Return</kbd> (zoomed out) | Glide to the highlighted window |
| <kbd>Esc</kbd>, right-click, or click empty canvas | Glide back to where you were |

The nearest window that way is the one Expanse on the Mac picks: a window sharing the row (or column) wins, nearest first; failing that, the closest within ~35° of straight that way. A hop pressed mid-glide hops on from the window the glide is heading to.

Focus moved from the keyboard (Omarchy's <kbd>Super</kbd> + arrows, Alt-Tab) also centres a window that isn't fully on screen. Focus that follows the mouse never moves the canvas.

<kbd>Ctrl</kbd> + <kbd>←</kbd> <kbd>→</kbd> is word-jump in terminals, browsers and editors on Linux, and the bind takes it from them. To hop with <kbd>Super</kbd> + arrows instead (replacing Omarchy's focus keys), change `hop_prefix` in [examples/expanse.lua](examples/expanse.lua).

## How it works

- **Camera.** A camera (origin + scale) maps the canvas to the monitor. Canvas coordinates are the windows' real positions, so at 100% the camera changes nothing and Hyprland draws everything itself.
- **At 100%, windows are real.** Every window on the canvas workspace floats where the canvas puts it, off-screen included. Zooming out floats tiled windows where they are. Input, focus, popups and X11 apps all stay native.
- **Zoomed out, windows are drawn through the camera.** No function hooks: Hyprland's only work on x86_64. At the start of each frame the plugin hides the canvas windows from Hyprland's own pass. After Hyprland's windows, it draws each one where the camera puts it, by giving it that box as its current (animated) geometry until the frame is drawn, the way Hyprland draws a window mid-animation, then restores it. The desktop is dimmed underneath; the bar stays on top. Pointer and keys go to the canvas, not to apps.
- **Gliding** animates the camera (geometric zoom, straight-line pan of the view's centre), then moves every real window once so the camera is back at 100%, and focuses the picked window. A hop at 100% is the same glide without the zoom: for 0.3 s every window is drawn through the camera.

## Build and load

You need Hyprland's headers (`hyprland` on Arch and Omarchy ships them), `base-devel` and `pkg-config`.

```sh
make
hyprctl plugin load "$PWD/expanse.so"
```

To load it at login, add to `~/.config/hypr/hyprland.lua`:

```lua
hl.plugin.load("/path/to/expanse-hypr/expanse.so")
dofile("/path/to/expanse-hypr/examples/expanse.lua")
```

[examples/expanse.lua](examples/expanse.lua) binds <kbd>Alt</kbd> + <kbd>`</kbd> and <kbd>Ctrl</kbd> + arrows. `hl.plugin.expanse.overview()` and `hl.plugin.expanse.hop("left")` return their actions, like `hl.dsp.*`, so they work in `hl.bind` and `hyprctl dispatch` alike.

`hyprpm` isn't installed on Omarchy; [hyprpm.toml](hyprpm.toml) is ready for when it is (`hyprpm add <this repo>`, `hyprpm enable expanse`).

Rebuild after every Hyprland update: plugins are tied to the exact Hyprland build.

## Scripting and tests

`hyprctl expanse` prints the canvas as JSON. `hyprctl expanse toggle|open|close` drives it, `hyprctl expanse hop <left|right|up|down>` hops, and `hyprctl expanse pick <x> <y>` clicks the zoomed-out canvas at a global logical point.

`make test` checks the nearest-window rule case by case. It was also checked against Expanse's own Swift on 5,000 random layouts, with no differences.

[scripts/vm.sh](scripts/vm.sh) syncs, builds and reloads the plugin in a Try Omarchy VM over SSH. [scripts/qmp-input.py](scripts/qmp-input.py) presses keys and moves and clicks the VM's real pointer through QEMU's QMP socket; a virtual keyboard (`wtype`) can't trigger Hyprland keybinds.

## Credits

Techniques learned from [Phantomat](https://github.com/kaolti/phantomat) by kaolti (BSD-3-Clause), itself built on [hyprland-scroll-overview](https://github.com/yayuuu/hyprland-scroll-overview): drawing a window at a box of the plugin's choosing through its animated geometry.

## License

MIT
