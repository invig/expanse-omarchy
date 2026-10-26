# Expanse on Omarchy/Hyprland: feasibility read (2026-10-27)

## Verdict
Feasible, and easier than on macOS: a Hyprland plugin runs inside the compositor, so it can draw live scaled windows and own input while zoomed out. Milestone 1 (zoom out and pick) works on Omarchy aarch64 in Try Omarchy.

## VM on this Mac
- try-omarchy is a notarized Mac app: QEMU + Apple Hypervisor framework + VirGL GPU, running stock aarch64 Omarchy (edge channel), plus one guest package for Mac integration.
- Needs Apple Silicon, macOS 15+, ~15 GB. This Mac: Apple M5, 32 GB, macOS 27.0.1. Supported.
- Route: v1.2.0 `TryOmarchy.dmg` (14 MB) from GitHub releases; first launch downloads the ~3 GB image. First boot runs Omarchy's setup (the account). Launcher "Port Forwarding → Add SSH" forwards Mac 127.0.0.1:2222 to guest 22.
- Guest: Omarchy 4.0.0, kernel 7.2.8, Hyprland 0.56.2 with headers, GCC 16, Lua 5.5. No `hyprpm` by default (it's in the omarchy repo).
- Real keyboard and pointer input reach the guest through QEMU's QMP socket (`send-key`, `input-send-event`). A virtual keyboard (`wtype`) can't trigger Hyprland keybinds.
- When the VM window is hidden (another Space), the guest renders only a few frames a second, so animations stall there.

## Hyprland plugin API (0.56.2, Lua config)
- C++ shared object. Load with `hl.plugin.load(path)` in `hyprland.lua` or `hyprctl plugin load`. Lua functions via `HyprlandAPI::addLuaFunction` appear as `hl.plugin.<name>.<fn>`; to work in `hl.bind` and `hyprctl dispatch` they return the action, as `hl.dsp.*` do.
- Build against the Lua Hyprland links (5.5), not 5.4.
- **Function hooks are x86_64-only.** `CFunctionHook::hook()` returns false on aarch64, so on ARM Omarchy (every Apple Silicon VM) a plugin can't hook internal functions. Phantomat, which hooks ~13 of them, can't load there.
- Without hooks: the event bus (render stages, input, window events), render-pass elements, dispatchers, hyprctl commands, layouts and decorations. That's enough:
  - Render stages fire inside each frame, and the pass is drawn after `RENDER_LAST_MOMENT`. So the plugin hides canvas windows at `RENDER_BEGIN` and draws them itself at `RENDER_POST_WINDOWS`.
  - Render modifiers (scale + translate) can't place a window that really sits off-screen: Hyprland clips each surface to the frame damage in untransformed coordinates. Instead, each window gets its on-screen box as its current animated geometry until the frame is drawn, the way Hyprland draws a window mid-animation, then it's restored.
  - Input events are cancellable, so the zoomed-out canvas owns the pointer and keys.
- Protected members (`renderWindow`, a window's raw hidden flag, an animated variable's "being animated" flag) are reached through explicit-instantiation member pointers. `#define private public` breaks GCC 16's `<any>`.
- No stable ABI: rebuild on every Hyprland update.
- Camera vs windows: Hyprland has no global camera. At 100% the camera is implicit and panning moves every floating window's real position (Expanse's macOS approach); input, focus, popups and X11 stay native. Only while zoomed out or gliding is anything drawn through the camera.
- Workspaces: canvas = the monitor's active workspace, all windows floating. Workspace keys are left alone for now.

## Prior art
- Phantomat (github.com/kaolti/phantomat, BSD-3): an Expanse-like infinite canvas plugin made on Omarchy with Hyprland 0.56 (x86_64, NVIDIA): zoom out, type/click to land, SUPER+arrows hops, tidy-up, remembers spots, lens effects. Fork of hyprland-scroll-overview. Closest prior art; relies on function hooks, so x86_64 only.
- driftwm (github.com/malbiruk/driftwm, GPL-3): standalone smithay compositor with the same camera model (native-size windows, Mod+arrows nearest window, zoom-to-fit, snapping clusters). Learn from its UX.
- hyprexpo and hyprscrolling are no longer in hyprland-plugins (unmaintained plugins dropped May 2026). niri: scrollable tiling plus overview, 1D. Nourish, CanvasWM: early canvas compositors.

## Recommendation
Keep building our own small, hook-free plugin: it runs on ARM and x86 alike, which Phantomat doesn't, and it matches Expanse's model. Compare against Phantomat on an x86 machine before investing heavily.
