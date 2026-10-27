-- For scripts/sweep.py: the headless monitor it tests on, below Virtual-1
-- (pinned at 0, 0), so moving or resizing the VM's window on the Mac doesn't
-- move it. In the config rather than `hyprctl eval`: loading the plugin
-- reloads the config. SWEEP-MAIN exists only during a run.
hl.monitor({ output = "Virtual-1", mode = "preferred", position = "0x0", scale = "auto" })
hl.monitor({ output = "SWEEP-MAIN", mode = "6016x3384@60", position = "0x2000", scale = 2 })
