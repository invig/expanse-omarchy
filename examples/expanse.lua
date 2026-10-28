-- Expanse for Hyprland 0.56+ (Lua config, as on Omarchy).
--
-- Load the plugin first, from hyprland.lua:
--   hl.plugin.load("/path/to/expanse.so")
-- then load this file after it:
--   dofile("/path/to/expanse-omarchy/examples/expanse.lua")

-- Hyprland reads the config again once the plugin is in; until then
-- hl.plugin.expanse doesn't exist.
if not (hl.plugin.expanse and hl.plugin.expanse.overview) then
  return
end

-- Omarchy's o.bind also lists bindings in its key guide; elsewhere, plain binds.
local bind = (o and o.bind) or function(keys, _description, action)
  hl.bind(keys, action)
end

-- Expanse is a layout, "expanse": a workspace with that layout is a canvas,
-- and every other one keeps its own. On Omarchy, SUPER + L cycles a workspace
-- through tiling, scrolling, floating and Expanse (below), and Expanse's setup
-- can make it the default. Elsewhere, for every workspace:
-- hl.config({ general = { layout = "expanse" } })
-- or for some:
-- hl.workspace_rule({ workspace = "3", layout = "expanse" })

-- Resting the pointer in a corner zooms out too: "bottom" (the default; the
-- bar has Omarchy's menu top left), "all", "top_left", "top_right",
-- "bottom_left", "bottom_right" or "off".
-- hl.config({ plugin = { expanse = { hot_corner = "bottom" } } })

-- The canvas on a plain background instead of the dimmed desktop picture.
-- hl.config({ plugin = { expanse = { wallpaper = false } } })
-- App names above the windows on the canvas (off, as on the Mac):
-- hl.config({ plugin = { expanse = { window_names = true } } })

-- ALT + ` zooms out to every window and back, as on the Mac.
bind("ALT + grave", "Zoom out to every window", hl.plugin.expanse.overview())

-- SUPER + arrows hop to the nearest window that way, as ⌃ + arrows on the Mac.
-- They replace Omarchy's focus keys (movefocus), which do the same job on a
-- canvas less well, and do what those did on a workspace with another
-- layout. Hyprland fires every bind on a key, so those go first. CTRL +
-- arrows stay with apps (word-jump).
for _, direction in ipairs({ "left", "right", "up", "down" }) do
  hl.unbind("SUPER + " .. direction)
  bind("SUPER + " .. direction, "Hop to the window " .. (direction == "up" and "above" or direction == "down" and "below" or "on the " .. direction),
    hl.plugin.expanse.hop(direction))
end

-- CTRL + ALT + C puts the window you're in in the middle of the screen.
bind("CTRL + ALT + C", "Centre the window you're in", hl.plugin.expanse.centre())

-- ALT + SHIFT + ` turns auto-centre on or off: off, clicking a window that's
-- partly on screen leaves the canvas where it is, for side-by-side work.
bind("ALT + SHIFT + grave", "Auto-centre on or off", hl.plugin.expanse.auto_centre())

-- On Omarchy, SUPER + L (and SUPER + ALT + L, back) cycles the workspace
-- through Omarchy's layouts with Expanse added after floating, as
-- omarchy/expanse in this checkout does it.
local here = debug.getinfo(1, "S").source:match("^@(.*)/examples/[^/]+$")
local cycle = here and (here .. "/omarchy/expanse")
local found = cycle and io.open(cycle, "r")
if o and found then
  found:close()
  hl.unbind("SUPER + L")
  hl.unbind("SUPER + ALT + L")
  bind("SUPER + L", "Cycle workspace layout", cycle .. " layout")
  bind("SUPER + ALT + L", "Cycle workspace layout back", cycle .. " layout back")
end
