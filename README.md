# Expanse for Hyprland

[Expanse](https://github.com/invig/expanse)'s infinite canvas as a Hyprland plugin: your windows live at fixed spots on one endless canvas, you zoom out to see all of them, and click one to glide to it at 100%.

Built and tested on Omarchy (Hyprland 0.56.2, aarch64) in [Try Omarchy](https://github.com/omacom/try-omarchy).

![Zoomed out: every window on the canvas, the one under the pointer ringed; the workspaces bar and minimap bottom right](docs/overview.jpg)

## Using it

| Input | What it does |
| --- | --- |
| <kbd>Alt</kbd> + <kbd>`</kbd>, <kbd>Alt</kbd> + pinch in, or rest the pointer in a bottom corner | Zoom out to every window, and back to where you were |
| Click a window | Glide to it, centred at 100%, and use it |
| <kbd>Super</kbd> + arrows | Glide to the nearest window in that direction; on the canvas, plain arrows |
| Pinch, or <kbd>Super</kbd> + scroll | Zoom the canvas around the pointer |
| Scroll, middle-drag, or <kbd>Space</kbd> + drag | Pan |
| Scroll or pinch on the empty desktop | Start moving around the canvas from 100% |
| Drag a window | Move it; snaps to edges, a 20 px gap and the middle of the screen (hold <kbd>Super</kbd> to turn snapping off) |
| Drag on empty canvas, <kbd>Shift</kbd>-click, <kbd>Super</kbd> + <kbd>A</kbd> | Select windows; drag any selected one to move the group |
| <kbd>T</kbd> | Tidy up |
| <kbd>0</kbd> · <kbd>1</kbd> · <kbd>-</kbd> · <kbd>=</kbd> | Fit everything · 100% · zoom out · zoom in |
| <kbd>Return</kbd> · <kbd>Esc</kbd> | Dive into the highlighted window · clear the selection, then back to 100% |
| Hover <kbd>×</kbd>, or <kbd>Super</kbd> + <kbd>W</kbd> | Close the hovered window, or every selected window at once |
| <kbd>Alt</kbd> + <kbd>Shift</kbd> + <kbd>`</kbd> | Auto-centre on or off. Off: clicking a window that's partly on screen leaves the canvas where it is, for side-by-side work |
| Click or drag on the minimap | Move the view there |
| Click a tile on the workspaces bar | Go to that workspace's canvas, still zoomed out |
| Right-click on the canvas | Back to 100% |

The canvas is remembered in `~/.local/state/expanse/layout.json` (window titles only as a hash): reloading the plugin brings it back exactly, and a window an app opens again, after a restart too, returns to its spot. Unloading the plugin or pausing it puts every window back where it was before the canvas, tiled ones back into their layout; resuming brings the canvas back.

The canvas follows you between screens: it keeps a home layout at the sizes you chose. On a smaller screen windows shrink to fit and the layout tightens, each window keeping its gap to its neighbours; back on the big screen everything returns exactly. Whatever was in the middle of the old screen stays in the middle of the new one, and a window too big for the screen shrinks to fit as you go to it.

Every workspace has a canvas of its own. Zoomed out, the workspaces bar beside the minimap (Expanse's Spaces bar) shows each workspace on the monitor as a small map of its windows, the one you're on outlined; it's hidden while there's only one. Going to another workspace with the canvas up, from the bar or a workspace bind like <kbd>Super</kbd> + <kbd>2</kbd>, brings the canvas along, zoomed out, so you can pick a window there; the one you left settles where leaving it would have taken you.

New windows open in free space, next to what you were working on, instead of on top of it; if the nearest free spot is off screen, the canvas glides there. Dialogs keep the spot their app gives them.

The keys after the first three work on the canvas (zoomed out, or after scrolling on the desktop). <kbd>Super</kbd> stands in for the Mac's <kbd>⌘</kbd> and <kbd>Ctrl</kbd>, so <kbd>Super</kbd> + <kbd>W</kbd> on the canvas closes the hovered window instead of the focused one.

The nearest window that way is the one Expanse on the Mac picks: a window sharing the row (or column) wins, nearest first; failing that, the closest within ~35° of straight that way. A hop pressed mid-glide hops on from the window the glide is heading to.

<kbd>Super</kbd> + arrows take over from Omarchy's focus keys (Hyprland's `movefocus`), which do the same job on a canvas less well: they pick by angle between centres, never centre the window, and can't chain. Hyprland fires every bind on a key, so the example config unbinds those four first. Like `movefocus`, a hop lands with the pointer on the window (unless `cursor:no_warps`), so focus that follows the mouse stays put; a click-centre leaves the pointer where you clicked. <kbd>Ctrl</kbd> + arrows stay with apps for word-jump.

Focus moved from the keyboard (Alt-Tab, other binds) also centres a window that isn't fully on screen; with auto-centre off the canvas stays put and a notification says the window is off-screen. Focus that follows the mouse never moves the canvas.

A full-screen (or maximised) window sits outside the canvas, as a full-screen app does on the Mac: zooming out takes it out of full screen, back to its spot among the others. Leave without picking a window, or pick it, and it goes back to full screen; pick another and it stays where it is on the canvas.

## How it works

- **Camera.** A camera (origin + scale) maps the canvas to the monitor. Canvas coordinates are the windows' real positions, so at 100% the camera changes nothing and Hyprland draws everything itself.
- **At 100%, windows are real.** Every window on the canvas workspace floats where the canvas puts it, off-screen included. Zooming out floats tiled windows where they are. Input, focus, popups and X11 apps all stay native.
- **Zoomed out, windows are drawn through the camera.** No function hooks: Hyprland's only work on x86_64. At the start of each frame the plugin hides the canvas windows from Hyprland's own pass. After Hyprland's windows, it draws each one where the camera puts it, by giving it that box as its current (animated) geometry until the frame is drawn, the way Hyprland draws a window mid-animation, then restores it. The desktop is dimmed underneath; the bar stays on top. Pointer and keys go to the canvas, not to apps.
- **On the canvas, windows are arranged for real.** Dragging, snapping and tidying move the real windows (still drawn through the camera), so there's nothing to reconcile when you leave. The snapping, tidy-up and placement rules are Expanse's, ported line for line.
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

[examples/expanse.lua](examples/expanse.lua) floats every window from its first frame (a window rule; without it, a tiled window floats where it is when the canvas first touches it) and binds <kbd>Alt</kbd> + <kbd>`</kbd>, <kbd>Alt</kbd> + <kbd>Shift</kbd> + <kbd>`</kbd> and <kbd>Super</kbd> + arrows, with descriptions for Omarchy's key guide. `hl.plugin.expanse.overview()`, `hop("left")`, `tidy()` and `auto_centre()` return their actions, like `hl.dsp.*`, so they work in `hl.bind` and `hyprctl dispatch` alike.

The hot corner is `plugin:expanse:hot_corner`: `bottom` (both bottom corners, the default, since Omarchy's bar has its menu top left), `all`, `top_left`, `top_right`, `bottom_left`, `bottom_right` or `off`. `plugin:expanse:wallpaper = false` puts the canvas on a plain background instead of the dimmed desktop picture:

```lua
hl.config({ plugin = { expanse = { hot_corner = "all", wallpaper = false } } })
```

`hyprpm` isn't installed on Omarchy; [hyprpm.toml](hyprpm.toml) is ready for when it is (`hyprpm add <this repo>`, `hyprpm enable expanse`).

Rebuild after every Hyprland update: plugins are tied to the exact Hyprland build.

## Scripting and tests

`hyprctl expanse` prints the canvas as JSON. `hyprctl expanse` with `toggle`, `open`, `close`, `tidy`, `fit`, `zoom <scale>`, `pan <dx> <dy>`, `hop <left|right|up|down>`, `focus <app>` (or `focus address:0x…`, from `hyprctl clients`), `space <name>` (that workspace's canvas, zoomed out), `select all`, `restore` (every window back into view), `pause`, `resume`, `autocentre <on|off>` or, for debugging, `screen <w> <h>` (pretend the screen is that size; no size for the real one) drives it, and `pick <x> <y>` clicks the canvas at a global logical point. In Lua, `hl.plugin.expanse.pause()` toggles pausing and `restore()` brings windows back into view.

`make test` checks the nearest-window rule and the layout file case by case. The nearest-window rule, snapping, tidy-up, free-space placement and the layout for a smaller screen were also checked against Expanse's own Swift on thousands of random layouts, with no differences.

[scripts/vm.sh](scripts/vm.sh) syncs, builds and reloads the plugin in a Try Omarchy VM over SSH. [scripts/qmp-input.py](scripts/qmp-input.py) presses keys and moves and clicks the VM's real pointer through QEMU's QMP socket; a virtual keyboard (`wtype`) can't trigger Hyprland keybinds.

## Credits

Techniques learned from [Phantomat](https://github.com/kaolti/phantomat) by kaolti (BSD-3-Clause), itself built on [hyprland-scroll-overview](https://github.com/yayuuu/hyprland-scroll-overview): drawing a window at a box of the plugin's choosing through its animated geometry.

## License

MIT
