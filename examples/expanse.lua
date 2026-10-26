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
