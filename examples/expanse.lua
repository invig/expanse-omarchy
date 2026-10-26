-- Expanse for Hyprland 0.56+ (Lua config, as on Omarchy).
--
-- Load the plugin first, from hyprland.lua:
--   hl.plugin.load("/path/to/expanse.so")
-- then load this file after it:
--   dofile("/path/to/expanse-hypr/examples/expanse.lua")

-- Hyprland reads the config again once the plugin is in; until then
-- hl.plugin.expanse doesn't exist.
if not (hl.plugin.expanse and hl.plugin.expanse.overview) then
  return
end

-- ALT + ` zooms out to every window and back, as on the Mac.
hl.bind("ALT + grave", hl.plugin.expanse.overview())

-- CTRL + arrows hop to the nearest window that way, as ⌃ + arrows on the Mac.
-- On Linux, CTRL + left/right is also word-jump in terminals, browsers and
-- editors; for SUPER + arrows (Omarchy's focus keys) instead, change the prefix.
local hop_prefix = "CTRL + "
for _, direction in ipairs({ "left", "right", "up", "down" }) do
  hl.bind(hop_prefix .. direction, hl.plugin.expanse.hop(direction))
end
