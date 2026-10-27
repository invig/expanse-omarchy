#!/usr/bin/python3 -I
"""Goes through Expanse's features in a Try Omarchy VM and checks each one.

    scripts/sweep.py

Real keyboard and pointer input through QEMU's QMP socket (scripts/qmp-input.py),
everything else through `hyprctl` over SSH (VM_PORT, VM_KEY as scripts/vm.sh).
It opens its own windows on an empty workspace and closes them at the end.
Each check prints PASS or FAIL; the exit status is the number that failed.
"""

import atexit
import glob
import json
import os
import shlex
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.environ.get("VM_PORT", "2222")
KEY = os.environ.get("VM_KEY", os.path.expanduser("~/.ssh/omarchy_vm_ed25519"))
SSH = ["ssh", "-i", KEY, "-p", PORT, "-o", "BatchMode=yes", "-o", "LogLevel=ERROR", "test@127.0.0.1"]
ENV = ("export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-1; "
       "export HYPRLAND_INSTANCE_SIGNATURE=$(ls -t $XDG_RUNTIME_DIR/hypr | head -1); ")
QMP = sorted(glob.glob("/tmp/omarchy-qemu-gpu.*/qmp.sock"))[-1]
RUN = str(int(time.time()))[-5:]
APPS = [f"sweep-{name}-{RUN}" for name in ("a", "b", "c")]
# Its workspace is the one Super + 7 goes to on the test monitor, and the
# other one Super + 8's: Omarchy gives each monitor workspaces of its own
# (41-50 for this one, say), and renames others to fit.
WORKSPACE_KEY, OTHER_KEY = "7", "8"
failed = []


def vm(command):
    return subprocess.run(SSH + [ENV + command], capture_output=True, text=True).stdout


def ctl(verb=""):
    out = vm(f"hyprctl expanse {verb}")
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        return {"error": out.strip(), "state": None, "windows": [], "scale": 0, "origin": [0, 0], "paused": None, "autoCentre": None}


def qmp(*steps):
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "qmp-input.py"), QMP, *map(str, steps)], check=True)


def layout():
    monitors = json.loads(vm("hyprctl monitors -j"))
    width = max(m["x"] + m["width"] / m["scale"] for m in monitors)
    height = max(m["y"] + m["height"] / m["scale"] for m in monitors)
    return round(width), round(height)


# A run cut short leaves its windows behind: they'd keep workspace 7 on
# whichever monitor it was on.
for c in json.loads(vm("hyprctl clients -j")):
    if c["class"].startswith(("sweep-", "org.expanse.Sweep")):
        vm(f"kill {c['pid']}")

# The sweep runs on a monitor of its own. Virtual-1 follows the VM's window
# on the Mac (moving or resizing that changes the screen mid-run); a headless
# output doesn't. Both are pinned, Virtual-1 at 0, 0 and the test monitor
# below it, so the layout's extent, and the pointer's mapping onto it, stay
# put. The rules are in scripts/sweep-monitors.lua, which the VM's config runs
# (added here if it isn't yet): loading the plugin reloads the config.
MON = "SWEEP-MAIN"
RULES = "/home/test/expanse-hypr/scripts/sweep-monitors.lua"
CONFIG = "~/.config/hypr/hyprland.lua"
vm(f"grep -qF {shlex.quote(RULES)} {CONFIG} || printf %s {shlex.quote(chr(10) + '-- scripts/sweep.py: its test monitor' + chr(10) + f'dofile("{RULES}")' + chr(10))} >> {CONFIG}")
vm("hyprctl reload")
time.sleep(1.5)
vm(f"hyprctl output create headless {MON}")
time.sleep(3)
# However the run ends: the test monitor goes, and Virtual-1's own rule comes back.
atexit.register(lambda: (vm(f"hyprctl output remove {MON}"), time.sleep(1), vm("hyprctl reload")))


def test_monitor():
    return next(m for m in json.loads(vm("hyprctl monitors -j")) if m["name"] == MON)


_m = test_monitor()
OX, OY, MW, MH = _m["x"], _m["y"], _m["width"] / _m["scale"], _m["height"] / _m["scale"]
W, H = layout()


def point(x, y):
    # The pointer, moved the last few pixels as a separate step so Hyprland sees motion there.
    qmp("move", int(x) - 6, int(y) - 6, W, H, "sleep", 80, "move", int(x), int(y), W, H, "sleep", 250)


def wait(seconds=1.6):
    time.sleep(seconds)


def settle(timeout=10):
    # Until the canvas is back at 100% and stays there: a glide asked for a
    # moment ago may not have started yet (frames crawl while the VM window is
    # hidden).
    end = time.time() + timeout
    while time.time() < end:
        if ctl()["state"] == "idle":
            time.sleep(0.8)
            if ctl()["state"] == "idle":
                return True
        time.sleep(0.4)
    return False


def zoom_out(timeout=8):
    # Zoomed out, once the camera has stopped: positions read mid-animation are stale.
    vm("hyprctl expanse open")
    end = time.time() + timeout
    while time.time() < end:
        if ctl()["state"] == "overview":
            time.sleep(0.4)
            return True
        time.sleep(0.3)
    return False


SCREENS = {}


def screen_size():
    return tuple((m["name"], m["x"], m["y"], m["width"], m["height"]) for m in json.loads(vm("hyprctl monitors -j")) if m["name"] == MON)


def check(name, ok, detail=""):
    # If the test monitor changed mid-run, the canvas rightly moved
    # everything, and what follows can't be compared. Say so, and stop.
    size = screen_size()
    if SCREENS.setdefault("start", size) != size:
        print(f"STOP  the test monitor changed during the run ({SCREENS['start']} -> {size}): run again")
        sys.exit(100)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{'  (' + detail + ')' if detail and not ok else ''}")
    if not ok:
        failed.append(name)


def mine(state=None):
    # This run's windows on the canvas, by app id.
    state = state or ctl()
    return {w["class"]: w for w in state["windows"] if w["class"].startswith("sweep-") and w["class"].endswith(RUN)}


def box(window, key="canvas"):
    return window[key]


def overlap(a, b):
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


def any_overlap(boxes):
    return any(overlap(a, b) for i, a in enumerate(boxes) for b in boxes[i + 1:])


def centre(b):
    return b[0] + b[2] / 2, b[1] + b[3] / 2


def near(p, q, tolerance=2):
    return abs(p[0] - q[0]) <= tolerance and abs(p[1] - q[1]) <= tolerance


def gap(a, b):
    # The space between two boxes side by side, or one above the other.
    horizontal = max(b[0] - (a[0] + a[2]), a[0] - (b[0] + b[2]))
    vertical = max(b[1] - (a[1] + a[3]), a[1] - (b[1] + b[3]))
    return max(horizontal, vertical)


def real():
    # Every window this run opened, by app id.
    return {c["class"]: c["at"] + c["size"] for c in json.loads(vm("hyprctl clients -j")) if c["class"].startswith("sweep-") and c["class"].endswith(RUN)}


def screen_middle():
    usable = test_monitor()
    left, top, right, bottom = usable["reserved"]
    w, h = usable["width"] / usable["scale"], usable["height"] / usable["scale"]
    return usable["x"] + left + (w - left - right) / 2, usable["y"] + top + (h - top - bottom) / 2


# --- Setup: three windows of apps with no saved spots, on an empty workspace
# on the test monitor. The canvas at 100% to begin with (it may have been left
# zoomed out or paused).
ctl("resume")
if ctl()["state"] != "idle":
    vm("hyprctl expanse close")
    settle()
point(OX + MW / 2, OY + MH / 2)
vm(f'hyprctl dispatch "hl.dsp.focus({{ monitor = \\"{MON}\\" }})"')
qmp("key", f"meta_l+{WORKSPACE_KEY}")
wait(1)
WORKSPACE = test_monitor()["activeWorkspace"]["name"]
for app, size in zip(APPS, ("900 560", "800 500", "700 440")):
    vm(f'hyprctl dispatch "hl.dsp.exec_cmd(\\"foot --app-id={app} sleep infinity\\", {{ float = true, size = \\"{size}\\" }})"')
    wait(2.2)
settle()
windows = mine()
check("three windows open on the canvas", len(windows) == 3, f"{list(windows)}")
check("new windows open in free space, not on top of each other", not any_overlap([box(w) for w in windows.values()]))

# --- Zoom out and back with Alt + `.
before = real()
qmp("key", "alt+grave_accent")
wait()
s = ctl()
check("Alt + ` zooms out to every window", s["state"] == "overview" and s["scale"] < 1, f"{s['state']} {s['scale']}")
check("zoomed out, every window is on screen", all(OX <= w["screen"][0] and w["screen"][0] + w["screen"][2] <= OX + MW for w in mine(s).values()))
qmp("key", "alt+grave_accent")
check("Alt + ` again goes back to 100%", settle())
check("going back without picking leaves every window where it was", real() == before, f"{before} -> {real()}")

# --- Hot corner (bottom corners by default).
point(OX + MW / 2, OY + MH / 2)
qmp("move", OX + 1, OY + MH - 1, W, H, "sleep", 80, "move", OX, OY + MH, W, H, "sleep", 900)
check("resting in a bottom corner zooms out", ctl()["state"] == "overview")
point(OX + MW / 2, OY + MH / 2)
qmp("move", OX + 1, OY + MH - 1, W, H, "sleep", 80, "move", OX, OY + MH, W, H, "sleep", 900)
check("the corner again zooms back in", settle())
point(OX + MW / 2, OY + MH / 2)

# --- Click a window to glide to it, centred and focused.
zoom_out()
target = mine()[APPS[2]]
point(*centre(target["screen"]))
qmp("click")
check("clicking a window glides to it", settle())
active = json.loads(vm("hyprctl activewindow -j"))
middle = screen_middle()
landed = real()[APPS[2]]
check("the window clicked has focus", active.get("class") == APPS[2], active.get("class"))
check("and is centred on screen", abs(centre(landed)[0] - middle[0]) <= 2 and abs(centre(landed)[1] - middle[1]) <= 2, f"{centre(landed)} vs {middle}")

# --- Super + arrows hop to the nearest window that way.
hopped = None
for direction in ("left", "right", "up", "down"):
    qmp("key", f"meta_l+{direction}")
    settle()
    hopped = json.loads(vm("hyprctl activewindow -j")).get("class")
    if hopped != APPS[2]:
        break
check("Super + arrow hops to the nearest window that way", hopped in APPS and hopped != APPS[2], hopped)
landed = real()[hopped] if hopped in APPS else [0, 0, 0, 0]
check("and centres it", abs(centre(landed)[0] - middle[0]) <= 2 and abs(centre(landed)[1] - middle[1]) <= 2, f"{centre(landed)} vs {middle}")

# --- Keys on the canvas: 1, -, =, 0.
zoom_out()
qmp("key", "1")
wait()
check("1 zooms to 100% on the canvas", abs(ctl()["scale"] - 1) < 0.01, str(ctl()["scale"]))
qmp("key", "minus")
wait()
s1 = ctl()["scale"]
check("- zooms out", s1 < 0.9, str(s1))
qmp("key", "equal")
wait()
check("= zooms in", ctl()["scale"] > s1, str(ctl()["scale"]))
qmp("key", "0")
wait()
fit = ctl()
check("0 fits everything", all(OX <= w["screen"][0] and w["screen"][0] + w["screen"][2] <= OX + MW for w in mine(fit).values()))

# --- Super + scroll zooms around the pointer; scroll pans.
point(OX + MW * 0.3, OY + MH * 0.6)
before = ctl()
qmp("hold", "meta_l", "wheel", "up", 5, "let", "meta_l", "sleep", 500)
after = ctl()
check("Super + scroll zooms", abs(after["scale"] - before["scale"]) > 0.01, f"{before['scale']} -> {after['scale']}")
before = ctl()["origin"]
qmp("wheel", "down", 5, "sleep", 500)
check("scroll pans", ctl()["origin"] != before)

# --- Tidy, then drag one window near another: it snaps to the 20 px gap.
qmp("key", "t")
wait(2.5)
t = mine()
boxes = [box(w) for w in t.values()]
check("T tidies up: no overlaps", not any_overlap(boxes))
qmp("key", "0")
wait()
s = ctl()
a, b = mine(s)[APPS[0]], mine(s)[APPS[1]]
# Drag A to just right of B's right edge, a little short of the gap; it snaps to exactly 20.
scale = s["scale"]
target_x = b["screen"][0] + b["screen"][2] + 14 * scale
start = (a["screen"][0] + 30, a["screen"][1] + 30)
end = (target_x + 30, b["screen"][1] + 30)
point(*start)
qmp("press", "sleep", 150, "move", int((start[0] + end[0]) / 2), int((start[1] + end[1]) / 2), W, H, "sleep", 200,
    "move", int(end[0]), int(end[1]), W, H, "sleep", 400, "release", "sleep", 600)
s = ctl()
a, b = mine(s)[APPS[0]], mine(s)[APPS[1]]
check("a dragged window snaps to the 20 px gap", abs(gap(box(a), box(b)) - 20) <= 1, f"gap {gap(box(a), box(b))}")
check("and lines up with its neighbour", abs(box(a)[1] - box(b)[1]) <= 1, f"{box(a)[1]} vs {box(b)[1]}")

# --- Box-select two windows, drag the group: both move together.
s = ctl()
ws = mine(s)
xs = [w["screen"][0] for w in ws.values()] + [w["screen"][0] + w["screen"][2] for w in ws.values()]
ys = [w["screen"][1] for w in ws.values()] + [w["screen"][1] + w["screen"][3] for w in ws.values()]
corner = (max(min(xs) - 40, 5), max(min(ys) - 40, 40))
point(*corner)
qmp("press", "sleep", 150, "move", int(max(xs) + 20), int(max(ys) + 20), W, H, "sleep", 400, "release", "sleep", 500)
selected = [w["class"] for w in mine().values() if w["selected"]]
check("dragging on empty canvas selects the windows it covers", len(selected) == 3, f"{selected}")
s = ctl()
before = {k: box(v) for k, v in mine(s).items()}
grab = mine(s)[APPS[1]]["screen"]
point(grab[0] + 20, grab[1] + 20)
qmp("hold", "meta_l", "press", "sleep", 150, "move", int(grab[0] + 80), int(grab[1] + 70), W, H, "sleep", 400, "release", "let", "meta_l", "sleep", 500)
after = {k: box(v) for k, v in mine().items()}
deltas = {(round(after[k][0] - before[k][0]), round(after[k][1] - before[k][1])) for k in before}
check("dragging a selected window moves the whole group", len(deltas) == 1 and deltas != {(0, 0)}, f"{deltas}")
qmp("key", "esc")
wait(0.8)
check("Esc clears the selection first", ctl()["state"] == "overview" and not any(w["selected"] for w in mine().values()))
qmp("key", "esc")
check("then Esc goes back to 100%", settle())

# --- Minimap: a click moves the view there.
zoom_out()
before = ctl()["origin"]
point(OX + MW - 16 - 208 + 30, OY + MH - 16 - 136 + 30)
qmp("click")
wait()
check("clicking the minimap moves the view", ctl()["origin"] != before)
qmp("key", "esc")
settle()

# --- Auto-centre toggles with Alt + Shift + `.
was = ctl()["autoCentre"]
qmp("key", "alt+shift+grave_accent")
wait(0.8)
check("Alt + Shift + ` turns auto-centre off and on", ctl()["autoCentre"] != was)
qmp("key", "alt+shift+grave_accent")
wait(0.8)

# --- Super + W on the canvas closes the hovered window.
zoom_out()
qmp("key", "t")  # nothing covering it (the drag above can leave a window on top of it)
wait(2.5)
state = ctl()
victim = mine(state)[APPS[2]]["screen"]
# A spot on it that no other window covers.
spot = next(((x, y) for x in range(int(victim[0]) + 10, int(victim[0] + victim[2]) - 10, 10) for y in range(int(victim[1]) + 10, int(victim[1] + victim[3]) - 10, 10)
             if not any(w["class"] != APPS[2] and w["screen"][0] <= x <= w["screen"][0] + w["screen"][2] and w["screen"][1] <= y <= w["screen"][1] + w["screen"][3]
                        for w in state["windows"])), centre(victim))
point(*spot)
qmp("key", "meta_l+w")
wait(2)
check("Super + W closes the hovered window", APPS[2] not in real() and APPS[0] in real(), f"{list(real())}")
qmp("key", "esc")
settle()

# --- Scrolling on the empty desktop starts moving around the canvas at 100%.
state = ctl()
empty = None
for x in range(int(OX) + 100, int(OX + MW) - 300, 150):
    for y in range(int(OY) + 150, int(OY + MH) - 300, 150):
        if not any(w["screen"][0] - 20 <= x <= w["screen"][0] + w["screen"][2] + 20 and w["screen"][1] - 20 <= y <= w["screen"][1] + w["screen"][3] + 20
                   for w in state["windows"]):
            empty = (x, y)
            break
    if empty:
        break
point(*empty)
qmp("wheel", "down", 2, "sleep", 600)
s = ctl()
check("scrolling on the empty desktop opens the canvas at 100%", s["state"] == "overview" and abs(s["scale"] - 1) < 0.01, f"{s['state']} {s['scale']}")
qmp("key", "esc")
settle()

# --- Pinch (through Hyprland's touchpad input path).
point(*empty)
s = ctl("pinch 0.9 0.8 0.7")
check("pinching on the desktop zooms out with the fingers", s["state"] == "overview" and abs(s["scale"] - 0.7) < 0.01, f"{s['state']} {s['scale']}")
qmp("key", "esc")
settle()

# --- Follows you between screens: every window fits a smaller one, the
# window you were in goes in the middle of each, and the layout comes back exactly.
WIN, BESIDE = APPS[1], APPS[0]
ctl(f"focus {BESIDE}")
settle()
o = real()[BESIDE]
vm(f'hyprctl dispatch "hl.dsp.window.move({{ x = {o[0] + o[2] + 40}, y = {o[1]}, window = \\"class:{WIN}\\" }})"')
wait(1)
vm(f'hyprctl dispatch "hl.dsp.focus({{ window = \\"class:{WIN}\\" }})"')
settle()
before = real()
ctl("screen 1440 874")
wait(2.5)
small = real()
fits = all(b[2] <= 1440 and b[3] <= 874 for b in small.values())
check("on a smaller screen every window fits", fits, f"{small}")
reserved = test_monitor()["reserved"]
check("and the window you were in goes in its middle", near(centre(small[WIN]), (OX + reserved[0] + 720, OY + reserved[1] + 437)), f"{small[WIN]}")
ctl("screen")
wait(2.5)
back = real()
around = lambda boxes: {k: (b[0] - boxes[WIN][0], b[1] - boxes[WIN][1], b[2], b[3]) for k, b in boxes.items()}
check("back on the big screen it's in the middle, and the layout returns exactly", near(centre(back[WIN]), screen_middle()) and around(back) == around(before),
      f"{before} -> {back}")

# --- Pause puts windows back; resume brings the canvas back exactly.
before = real()
ctl("pause")
wait(1.5)
check("pause", ctl()["paused"])
ctl("resume")
wait(2)
check("resume brings every window back to its spot", real() == before, f"{before} -> {real()}")

# --- Reloading the plugin keeps the canvas exactly.
before = real()
vm("hyprctl plugin unload ~/expanse-hypr/expanse.so; hyprctl plugin load ~/expanse-hypr/expanse.so")
wait(2)
check("reloading the plugin keeps every window's spot", real() == before, f"{before} -> {real()}")

# --- Restore all windows into view.
zoom_out()
ctl("pan 4000 0")
qmp("key", "esc")
settle()
ctl("restore")
wait(1.5)
usable_ok = all(OX <= b[0] and b[0] + b[2] <= OX + MW and OY <= b[1] and b[1] + b[3] <= OY + MH for b in real().values())
check("restore brings every window into view", usable_ok, f"{real()}")

# --- Scripting.
s = ctl(f"focus {APPS[0]}")
check("hyprctl expanse focus <app> glides to it", settle() and json.loads(vm("hyprctl activewindow -j")).get("class") == APPS[0])
check("hyprctl expanse keys lists the canvas's keys", "Zoomed out" in vm("hyprctl expanse keys"))

# --- Full screen: off the canvas while it's up, back after.
vm(f'hyprctl dispatch "hl.dsp.focus({{ window = \\"class:{APPS[0]}\\" }})"; hyprctl dispatch "hl.dsp.window.fullscreen()"')
wait(1.5)
fs = lambda: next((c["fullscreen"] for c in json.loads(vm("hyprctl clients -j")) if c["class"] == APPS[0]), None)
zoom_out()
check("zooming out takes a full-screen window out of full screen", fs() == 0)
qmp("key", "esc")
settle()
wait(1)
check("leaving without picking another puts it back in full screen", fs() == 2)
vm('hyprctl dispatch "hl.dsp.window.fullscreen()"')
wait(1)

# Back from full screen, a window gets the view wherever it was (here it went
# full screen half off screen: with auto-centre off, focus didn't move the view).
ctl("autocentre off")
vm(f'hyprctl dispatch "hl.dsp.window.move({{ x = {int(OX + MW) - 400}, y = {int(OY) + 300}, window = \\"class:{APPS[0]}\\" }})"')
wait(1)
vm(f'hyprctl dispatch "hl.dsp.focus({{ window = \\"class:{APPS[0]}\\" }})"; hyprctl dispatch "hl.dsp.window.fullscreen()"')
wait(1.5)
vm('hyprctl dispatch "hl.dsp.window.fullscreen()"')
wait(1.2)
settle()
ctl("autocentre on")
back = real()[APPS[0]]
check("a window back from full screen gets the view, wherever it was", fs() == 0 and near(centre(back), screen_middle()), f"{back} {fs()}")

# ===== The README's other claims.
A, B = APPS[0], APPS[1]
D, E, F, G = (f"sweep-{name}-{RUN}" for name in ("d", "e", "f", "g"))
DIALOG = f"org.expanse.Sweep{RUN}"
hyprland = vm("pgrep -x Hyprland").strip()
reports = vm("ls ~/.cache/hyprland | wc -l").strip()


def clients():
    return json.loads(vm("hyprctl clients -j"))


def client(match):
    return next((c for c in clients() if match in (c["class"], c["title"])), None)


def open_app(app, size="700 440", workspace=None):
    rule = f'float = true, size = \\"{size}\\"' + (f', workspace = \\"{workspace} silent\\"' if workspace else "")
    vm(f'hyprctl dispatch "hl.dsp.exec_cmd(\\"foot --app-id={app} sleep infinity\\", {{ {rule} }})"')
    wait(2.4)


def move_by(app, dx, dy):
    vm(f'hyprctl dispatch "hl.dsp.window.move({{ x = {int(dx)}, y = {int(dy)}, relative = true, window = \\"class:{app}\\" }})"')
    wait(0.8)


def focus(app):
    vm(f'hyprctl dispatch "hl.dsp.focus({{ window = \\"class:{app}\\" }})"')


def active():
    return json.loads(vm("hyprctl activewindow -j")).get("class")


def cursor():
    x, y = vm("hyprctl cursorpos").split(",")
    return float(x), float(y)


def inside(p, b, margin=0):
    return b[0] - margin <= p[0] <= b[0] + b[2] + margin and b[1] - margin <= p[1] <= b[1] + b[3] + margin


def centred(b):
    middle = screen_middle()
    return abs(centre(b)[0] - middle[0]) <= 2 and abs(centre(b)[1] - middle[1]) <= 2


def pixel(x, y):
    return vm(f'grim -g "{int(x)},{int(y)} 1x1" -s 1 /tmp/sweep-pixel.png && magick /tmp/sweep-pixel.png -format "%[hex:p{{0,0}}]" info:').strip()


def canvas_of(app):
    return box(mine()[app]) if app in mine() else None


def empty_spot(state, avoid_corner=True):
    for x in range(int(OX) + 80, int(OX + MW) - 400, 60):
        for y in range(int(OY) + 120, int(OY + MH) - 320, 60):
            if not any(inside((x, y), w["screen"], 30) for w in state["windows"]):
                return x, y
    return None


# The saved layout keeps titles only as a hash.
saved = json.loads(vm("cat ~/.local/state/expanse/layout.json"))
check("the saved layout keeps window titles only as a hash", all(w["title"] == "" or w["title"].startswith("fnv1a:") for w in saved["windows"]))

# A window its app opens again goes back to its spot on the canvas.
spot = canvas_of(B)
vm(f"kill {client(B)['pid']}")
wait(2)
open_app(B, "800 500")
settle()
check("a window its app opens again returns to its spot", canvas_of(B) == spot, f"{spot} -> {canvas_of(B)}")

# Lua functions return actions, for binds and hyprctl dispatch alike.
vm('hyprctl dispatch "hl.plugin.expanse.overview()"')
wait()
check("hl.plugin.expanse.overview() zooms out", ctl()["state"] in ("opening", "overview"))
vm('hyprctl dispatch "hl.plugin.expanse.overview()"')
check("and back", settle())
vm('hyprctl dispatch "hl.plugin.expanse.tidy()"')
wait(2.5)
check("hl.plugin.expanse.tidy() tidies on the canvas", ctl()["state"] == "overview" and not any_overlap([box(w) for w in mine().values()]))
qmp("key", "esc")
settle()

# Ctrl + arrows stay with apps.
focus(A)
settle()
before = real()
qmp("key", "ctrl+right", "sleep", 600)
check("Ctrl + arrows are left to apps", ctl()["state"] == "idle" and real() == before and active() == A)

# A row of three: a hop lands the pointer on the window, and a hop pressed mid-glide hops on.
open_app(D, "700 440")
settle()
r = real()
row_y = r[A][1]
move_by(B, r[A][0] + r[A][2] + 120 - r[B][0], row_y - r[B][1])
r = real()
move_by(D, r[B][0] + r[B][2] + 120 - r[D][0], row_y - r[D][1])
focus(A)
settle()
qmp("key", "meta_l+right")
settle()
check("Super + arrow lands the pointer on the window it hops to", active() == B and inside(cursor(), real()[B]), f"{active()} {cursor()} {real().get(B)}")
focus(A)
settle()
qmp("key", "meta_l+right", "sleep", 60, "key", "meta_l+right")
settle()
check("a hop pressed mid-glide hops on from where the glide is heading", active() == D, active())

# Focus that follows the mouse never moves the canvas; a click centres; the pointer stays put.
r = real()
move_by(B, OX + MW - 260 - r[B][0], 0)
before = real()
peek = (OX + MW - 120, centre(before[B])[1])
point(*peek)
wait(1.5)
check("focus that follows the mouse never moves the canvas", real() == before and ctl()["state"] == "idle")
qmp("click")
settle()
check("clicking a window partly off screen centres it", centred(real()[B]), f"{real()[B]}")
check("and leaves the pointer where you clicked", abs(cursor()[0] - peek[0]) <= 2 and abs(cursor()[1] - peek[1]) <= 2, f"{cursor()} vs {peek}")

# Keyboard focus centres a window that isn't fully on screen; with auto-centre off it doesn't.
point(OX + MW / 2, OY + MH / 2)
r = real()
move_by(D, OX + MW - 200 - r[D][0], 0)
focus(A)
settle()
focus(D)
settle()
check("keyboard focus centres a window that isn't fully on screen", centred(real()[D]), f"{real()[D]}")
ctl("autocentre off")
r = real()
move_by(D, OX + MW - 200 - r[D][0], 0)
focus(A)
wait(1)
before = real()
focus(D)
wait(2)
check("with auto-centre off it leaves the canvas where it is", real() == before and active() == D)
ctl("autocentre on")

# The plain background.
zoom_out()
spot = empty_spot(ctl())
with_picture = pixel(*spot)
ctl("wallpaper off")
wait(1)
plain = pixel(*spot)
check("plugin:expanse:wallpaper off puts the canvas on a plain background", plain != with_picture and plain.upper().startswith(("212121", "202020", "222222")), f"{with_picture} -> {plain}")
ctl("wallpaper default")
qmp("key", "esc")
settle()

# The hot corner can be turned off.
ctl("corner off")
point(OX + MW / 2, OY + MH / 2)
qmp("move", OX + 1, OY + MH - 1, W, H, "sleep", 80, "move", OX, OY + MH, W, H, "sleep", 900)
check("hot corner off: resting in a corner does nothing", ctl()["state"] == "idle")
ctl("corner default")
point(OX + MW / 2, OY + MH / 2)

# Full screen: pick another window and it stays where it is.
focus(A)
settle()
vm('hyprctl dispatch "hl.dsp.window.fullscreen()"')
wait(1.5)
zoom_out()
point(*centre(mine()[B]["screen"]))
qmp("click")
settle()
wait(1)
check("picking another window leaves a full-screen one where it is on the canvas", fs() == 0 and active() == B, f"{fs()} {active()}")

# The workspaces bar, and following a workspace bind while zoomed out.
other = str(int(WORKSPACE) + int(OTHER_KEY) - int(WORKSPACE_KEY))
open_app(E, "700 440", other)
zoom_out()
tiles = {t["name"]: t["tile"] for t in ctl()["spaces"]}
tile = tiles.get(f"Workspace {OTHER_KEY}")
if tile:
    point(tile[0] + tile[2] / 2, tile[1] + tile[3] / 2)
    qmp("click")
    wait(2.5)
s = ctl()
check("clicking a workspace tile goes to its canvas, still zoomed out", bool(tile) and s["workspace"] == other and s["state"] == "overview", f"{s['workspace']} {s['state']} {list(tiles)}")
qmp("key", f"meta_l+{WORKSPACE_KEY}")
wait(2.5)
s = ctl()
check("a workspace bind while zoomed out brings the canvas along", s["workspace"] == WORKSPACE and s["state"] == "overview", f"{s['workspace']} {s['state']}")
qmp("key", "esc")
settle()

# A new window whose nearest free spot is off screen: the canvas glides there.
ctl("screen 900 600")
wait(2.5)
open_app(F, "700 440")
settle()
f = real().get(F, [0, 0, 0, 0])
top = OY + test_monitor()["reserved"][1]
check("a new window opened off screen is glided to", OX <= f[0] and f[0] + f[2] <= OX + 900 and top <= f[1] and f[1] + f[3] <= top + 600, f"{f}")
ctl("screen")
wait(2.5)

# Dialogs keep the spot their app gives them.
vm(f"setsid python3 /home/test/expanse-hypr/scripts/dialog.py {DIALOG} >/dev/null 2>&1 &")
wait(5)
dialog, parent = client("dialog-child"), client("dialog-parent")
wait(1.5)
again = client("dialog-child")
check("a dialog keeps the spot its app gives it", bool(dialog) and bool(again) and again["at"] == dialog["at"], f"{dialog and dialog['at']} {again and again['at']}")
if parent:
    vm(f"kill {parent['pid']}")
    wait(1)

# The Omarchy menu rows.
rows = json.loads("\n".join(line for line in open(os.path.join(HERE, "..", "examples", "omarchy-menu.jsonc")) if not line.strip().startswith("//")))
verbs = {"toggle", "tidy", "restore", "autocentre", "wallpaper", "pause", "corner"}
actions_ok = all(r.get("action", "hyprctl expanse tidy").split()[2] in verbs if r.get("action", "").startswith("hyprctl expanse") else r["action"].startswith("omarchy-launch-")
                 for r in rows.values() if "action" in r)
checked_ok = all(subprocess.run(SSH + [ENV + r["checked"]], capture_output=True).returncode in (0, 1) for r in rows.values() if "checked" in r)
check("the Omarchy menu rows parse, and their actions and checks run", actions_ok and checked_ok)

# ===== The README, line by line: what the checks above don't cover.
H1, H2, X1, X2, X3, T1, BIG = (f"sweep-{name}-{RUN}" for name in ("h1", "h2", "x1", "x2", "x3", "t1", "big"))
POPUP = f"org.expanse.SweepPopup{RUN}"


def view_centre(state):
    # The canvas point in the middle of the usable screen.
    middle = screen_middle()
    monitor = test_monitor()
    return (state["origin"][0] + (middle[0] - monitor["x"]) / state["scale"], state["origin"][1] + (middle[1] - monitor["y"]) / state["scale"])


def drag(start, end, modifier=None, button="left"):
    point(*start)
    steps = ["hold", modifier] if modifier else []
    steps += ["press", button, "sleep", 150, "move", int((start[0] + end[0]) / 2), int((start[1] + end[1]) / 2), W, H, "sleep", 200,
              "move", int(end[0]), int(end[1]), W, H, "sleep", 400, "release", button, "sleep", 500]
    if modifier:
        steps += ["let", modifier]
    qmp(*steps)


def greens():
    return float(vm('grim -s 0.25 /tmp/sweep-green.png && magick /tmp/sweep-green.png -channel RGB -fx "g>0.85&&r<0.2&&b<0.2" -separate -delete 1,2 -format "%[fx:mean*w*h]" info:').strip() or 0)


# Plain arrows move the highlight on the canvas, the view following; Return dives into it.
zoom_out()
before = ctl()
qmp("key", "right", "sleep", 900)
after = ctl()
check("plain arrows move the highlight on the canvas and the view follows", after["origin"] != before["origin"], f"{before['origin']} -> {after['origin']}")
qmp("key", "ret")
check("Return dives into the highlighted window", settle() and active() in real())

# Middle-drag and Space + drag pan.
zoom_out()
spot = empty_spot(ctl())
before = ctl()["origin"]
drag(spot, (spot[0] + 120, spot[1] + 80), button="middle")
check("middle-drag pans", ctl()["origin"] != before)
spot = empty_spot(ctl())
before = ctl()
drag(spot, (spot[0] + 120, spot[1] + 80), modifier="spc")
after = ctl()
check("Space + drag pans", after["origin"] != before["origin"] and {k: box(v) for k, v in mine(after).items()} == {k: box(v) for k, v in mine(before).items()})

# Snapping to the middle of the screen; Super turns snapping off. On the
# canvas at 100%, with the window in view.
qmp("key", "esc")
settle()
ctl(f"focus {A}")
settle()
point(*empty_spot(ctl()))
ctl("pinch 1.0")
wait()
s = ctl()
w = mine(s)[A]
middle = screen_middle()
offset = (centre(w["screen"])[0] - middle[0] - 7, centre(w["screen"])[1] - middle[1] - 5)  # land 7, 5 px from the middle
drag((w["screen"][0] + 40, w["screen"][1] + 40), (w["screen"][0] + 40 - offset[0], w["screen"][1] + 40 - offset[1]))
s = ctl()
c = centre(real()[A])
vc = view_centre(s)  # at 100%, real coordinates
check("a dragged window snaps to the middle of the screen", abs(c[0] - vc[0]) <= 1 and abs(c[1] - vc[1]) <= 1, f"{c} vs {vc}")
w = mine(s)[A]
drag((w["screen"][0] + 40, w["screen"][1] + 40), (w["screen"][0] + 40 + 7, w["screen"][1] + 40 + 5), modifier="meta_l")
c = centre(real()[A])
check("holding Super turns snapping off", 4 <= c[0] - vc[0] <= 9 and 3 <= c[1] - vc[1] <= 7, f"{c} vs {vc}")
qmp("key", "esc")
settle()

# Shift-click and Super + A select; the x on a selected window closes every selected one.
open_app(X1, "700 440")
open_app(X2, "700 440")
settle()
zoom_out()
qmp("hold", "shift")
point(*centre(mine()[X1]["screen"]))
qmp("click", "sleep", 300, "let", "shift", "sleep", 200)
ctl("zoom 0.6")  # the x shows on windows at least 220 px wide on screen
wait()
x2 = mine()[X2]["screen"]
middle = screen_middle()
ctl(f"pan {centre(x2)[0] - middle[0]:.0f} {centre(x2)[1] - middle[1]:.0f}")
wait(0.8)
qmp("hold", "shift")
point(*centre(mine()[X2]["screen"]))
qmp("click", "sleep", 300, "let", "shift", "sleep", 300)
picked = {k for k, v in mine().items() if v["selected"]}
check("Shift-click adds windows to the selection", picked == {X1, X2}, f"{picked}")
x2 = mine()[X2]["screen"]
point(x2[0] + x2[2] - 23, x2[1] + 23)  # its x
qmp("click", "sleep", 2000)
check("the x on a selected window closes every selected window", X1 not in real() and X2 not in real(), f"{list(real())}")
qmp("key", "meta_l+a", "sleep", 600)
check("Super + A selects every window", all(v["selected"] for v in mine().values()) and len(mine()) >= 3)
qmp("key", "esc", "sleep", 400)

# Minimap drag; right-click goes back to 100%.
m = ctl()["minimap"] or [OX + MW - 224, OY + MH - 152, 208, 136]
start = (m[0] + 40, m[1] + 40)
point(*start)
qmp("press", "sleep", 200)
first = ctl()["origin"]
qmp("move", int(start[0] + 60), int(start[1] + 30), W, H, "sleep", 500)
moved = ctl()["origin"]
qmp("release", "sleep", 300)
check("dragging on the minimap moves the view with it", moved != first, f"{first} -> {moved}")
spot = empty_spot(ctl())
point(*spot)
qmp("click", "right")
check("right-click on the canvas goes back to 100%", settle())

# Auto-centre off: clicking a window partly on screen leaves the canvas where it is.
ctl("autocentre off")
r = real()
move_by(B, OX + MW - 260 - r[B][0], 0)
before = real()
point(OX + MW - 120, centre(before[B])[1])
qmp("click", "sleep", 1500)
check("auto-centre off: clicking a window partly on screen leaves the canvas where it is", real() == before and active() == B)
# ...and keyboard focus to a window entirely off screen says so.
move_by(B, 3 * W, 0)
focus(A)
wait(1)
focus(B)
wait(1.5)
check("auto-centre off: focus on a window off screen gives a notification", ctl()["offscreenNotice"] == B, ctl()["offscreenNotice"])
ctl("autocentre on")
focus(A)
settle()

# After a restart too: the spot comes from the saved file.
spot = canvas_of(D)
vm("hyprctl plugin unload ~/expanse-hypr/expanse.so")
vm(f"kill {client(D)['pid']}")
wait(1.5)
vm("hyprctl plugin load ~/expanse-hypr/expanse.so")
wait(1.5)
open_app(D, "700 440")
settle()
check("after a restart too, a window its app opens again returns to its spot", canvas_of(D) == spot, f"{spot} -> {canvas_of(D)}")

# A tiled window floats on the canvas, and pausing puts it back into the layout.
open_app(T1, "700 440")
vm(f'hyprctl dispatch "hl.dsp.window.float({{ action = \\"unset\\", window = \\"class:{T1}\\" }})"')
wait(1.5)
tiled = lambda: next((not c["floating"] for c in clients() if c["class"] == T1), None)
was = tiled()
zoom_out()
floats = tiled() is False
qmp("key", "esc")
settle()
ctl("pause")
wait(1.5)
check("a tiled window floats on the canvas, and pausing puts it back into its layout", was is True and floats and tiled() is True, f"{was} {floats} {tiled()}")
ctl("resume")
wait(2)
vm(f"kill {client(T1)['pid']}")

# A window too big for the screen shrinks to fit as you go to it, and grows back on a bigger one.
# Sizes relative to the screen: the VM's follows its window on the Mac.
big = (int(MW * 0.6), int(MH * 0.6))
tight = (int(MW * 0.45), int(MH * 0.45))
open_app(BIG, f"{big[0]} {big[1]}")
chosen = real()[BIG][2:]
ctl(f"screen {tight[0]} {tight[1]}")
wait(2.5)
ctl(f"focus {BIG}")
settle()
small = real()[BIG]
check("a window too big for the screen shrinks to fit as you go to it", small[2] <= tight[0] and small[3] <= tight[1], f"{small} on {tight}")
ctl("screen")
wait(2.5)
check("and grows back on the big screen", real()[BIG][2:] == chosen, f"{real()[BIG]} vs {chosen}")

# Leaving a workspace from the canvas without moving around leaves it as it was.
before = real()
zoom_out()
tiles = {t["name"]: t["tile"] for t in ctl()["spaces"]}
tile = tiles[f"Workspace {OTHER_KEY}"]
point(tile[0] + tile[2] / 2, tile[1] + tile[3] / 2)
qmp("click", "sleep", 2500)
check("leaving a workspace from the canvas leaves its windows where they were", {k: v for k, v in real().items() if k in before} == before)
qmp("key", "esc")
settle()
qmp("key", f"meta_l+{WORKSPACE_KEY}", "sleep", 2000)
settle()

# Placement on a workspace that isn't showing, mid-glide, and while zoomed out.
open_app(H1, "700 440", other)
on_other = [r for k, r in real().items() if k in (E, H1)]
check("a window opened on a workspace that isn't showing gets free space there", len(on_other) == 2 and not any_overlap(on_other))
zoom_out()
open_app(H2, "700 440", other)
qmp("key", "esc")
settle()
wait(1)
on_other = [r for k, r in real().items() if k in (E, H1, H2)]
check("one opened elsewhere while zoomed out is placed once the canvas is back", len(on_other) == 3 and not any_overlap(on_other))
qmp("key", "meta_l+right")
vm(f'hyprctl dispatch "hl.dsp.exec_cmd(\\"foot --app-id={X3} sleep infinity\\", {{ float = true, size = \\"640 400\\" }})"')
settle()
wait(1)
r = real()
others = [b for k, b in r.items() if k != X3 and client(k) and client(k)["workspace"]["name"] == WORKSPACE]
check("one opened mid-glide is placed once the glide lands", X3 in r and not any(overlap(r[X3], b) for b in others), f"{r.get(X3)}")

# An open menu isn't drawn on the canvas, and is there again at 100%.
vm(f"setsid python3 /home/test/expanse-hypr/scripts/dialog.py {POPUP} popup >/dev/null 2>&1 &")
wait(5)
open_menu = greens()
zoom_out()
zoomed = greens()
qmp("key", "esc")
settle()
again = greens()
check("an open menu isn't drawn on the canvas, and is there again at 100%", open_menu > 200 and zoomed == 0 and again > 200, f"{open_menu} {zoomed} {again}")
if client(POPUP):
    vm(f"kill {client(POPUP)['pid']}")

# Maximised windows sit outside the canvas too.
focus(A)
settle()
vm('hyprctl dispatch "hl.dsp.window.fullscreen({ mode = \\"maximized\\" })"')
wait(1.5)
zoom_out()
during = fs()
qmp("key", "esc")
settle()
wait(1)
check("a maximised window leaves maximised on the canvas and gets it back", during == 0 and fs() == 1, f"{during} {fs()}")
vm('hyprctl dispatch "hl.dsp.window.fullscreen({ mode = \\"maximized\\" })"')
wait(1)

# Super + arrows: Omarchy's movefocus binds are gone; with cursor:no_warps the pointer stays put.
binds = json.loads(vm("hyprctl binds -j"))
check("Super + arrows have one bind each (Omarchy's movefocus ones unbound)",
      all(sum(1 for b in binds if b["modmask"] == 64 and b["key"].lower() == key) == 1 for key in ("left", "right", "up", "down")))
vm('hyprctl eval "hl.config({ cursor = { no_warps = true } })"')
point(OX + MW / 2, OY + MH / 2 + 200)
here = cursor()
qmp("key", "meta_l+left")
settle()
check("with cursor:no_warps a hop leaves the pointer where it is", abs(cursor()[0] - here[0]) <= 2 and abs(cursor()[1] - here[1]) <= 2, f"{here} -> {cursor()}")
vm('hyprctl eval "hl.config({ cursor = { no_warps = false } })"')

# Each monitor's canvas has its own minimap.
vm("hyprctl output create headless SWEEP-3")
wait(3)
W, H = layout()
third = next(m for m in json.loads(vm("hyprctl monitors -j")) if m["name"] == "SWEEP-3")
point(third["x"] + 300, third["y"] + 300)
zoom_out()
m = ctl()["minimap"]
check("each monitor's canvas has its own minimap", m and third["x"] <= m[0] <= third["x"] + third["width"] / third["scale"], f"{m} {third['x']}")
qmp("key", "esc")
settle()
vm("hyprctl output remove SWEEP-3")
wait(2)
W, H = layout()
point(OX + MW / 2, OY + MH / 2)

# Drawn as Expanse draws them: each window's border, and a soft shadow, where the camera puts it.
ctl("wallpaper off")
zoom_out()
s = ctl()
w = max(mine(s).values(), key=lambda v: v["screen"][2])["screen"]
below = pixel(w[0] + w[2] - 30, w[1] + w[3] + 1)  # the border, just outside the window
shade = pixel(w[0] + w[2] - 30, w[1] + w[3] + 3)  # the shadow, just past the border (the label is at the left)
far = pixel(*empty_spot(s))                       # the plain background
qmp("key", "esc")
settle()
ctl("wallpaper default")
darker = int(shade[:2], 16) < int(far[:2], 16) if len(shade) >= 6 and len(far) >= 6 else False
check("zoomed out, every window has its theme border and a soft shadow", below[:6] != far[:6] and darker, f"border {below} shadow {shade} background {far}")

for app in (X1, X3, H1, H2, BIG):
    if client(app):
        vm(f"kill {client(app)['pid']}")
APPS += [X1, X3, H1, H2, BIG]

# ===== A touchpad, through the kernel and libinput (needs root in the VM for /dev/uinput).
ASKPASS = os.environ.get("VM_ASKPASS")


def touchpad(start, end, seconds=0.6):
    sudo = f"SUDO_ASKPASS={ASKPASS} sudo -A" if ASKPASS else "sudo -n"
    return subprocess.run(SSH + [f"{sudo} python3 -I /home/test/expanse-hypr/scripts/touchpad.py pinch {start} {end} {seconds}"], capture_output=True).returncode == 0


state = ctl()
spot = empty_spot(state)
point(*spot)
if touchpad(0.5, 0.25):
    s = ctl()
    check("touchpad: pinching in on the desktop zooms out with the fingers", s["state"] == "overview" and s["scale"] < 0.75, f"{s['state']} {s['scale']}")
    before = ctl()["scale"]
    touchpad(0.25, 0.5)
    check("touchpad: spreading the fingers zooms in", ctl()["scale"] > before * 1.4, f"{before} -> {ctl()['scale']}")
    qmp("key", "esc")
    settle()
    visible = [b for b in real().values() if OX <= b[0] and b[0] + b[2] <= OX + MW and OY + 30 <= b[1] and b[1] + b[3] <= OY + MH - 60]
    point(*centre(visible[0] if visible else real()[A]))
    touchpad(0.5, 0.4, 0.4)
    check("touchpad: a pinch over a window is the app's", ctl()["state"] == "idle")
    qmp("hold", "alt", "sleep", 150)
    touchpad(0.5, 0.3)
    qmp("let", "alt", "sleep", 600)
    check("touchpad: Alt + pinching in over a window zooms out", ctl()["state"] == "overview")
    qmp("key", "esc")
    settle()
else:
    print("SKIP  touchpad (no root in the VM: set VM_ASKPASS to an askpass program there)")

# ===== Try Omarchy's pinch device, the one the Mac's trackpad drives: two
# fingers as multi-touch events through QMP, then the guest's kernel and
# libinput, as a pinch on the trackpad arrives.
import socket


def device_pinch(start, end, steps=29):
    connection = socket.socket(socket.AF_UNIX)
    connection.connect(QMP)
    stream = connection.makefile("rw")

    def call(command, **arguments):
        stream.write(json.dumps({"execute": command, **({"arguments": arguments} if arguments else {})}) + "\n")
        stream.flush()
        while True:
            reply = json.loads(stream.readline())
            if "return" in reply or "error" in reply:
                return reply

    def touch(kind, slot, tracking, axis="x", value=0):
        return {"type": "mtt", "data": {"type": kind, "slot": slot, "tracking-id": tracking, "axis": axis, "value": value}}

    # A finger lifts with tracking id -1 (QEMU passes the id through as it
    # is). Any left down by a run cut short mid-gesture lift first.
    lift = [touch("end", 0, -1), touch("end", 1, -1)]
    stream.readline()
    call("qmp_capabilities")
    call("input-send-event", events=lift)
    time.sleep(0.1)
    middle = 16384
    half = lambda spread: int(spread * 16384)
    ok = "return" in call("input-send-event", events=[
        touch("begin", 0, 1, "x", middle - half(start)), touch("data", 0, 1, "x", middle - half(start)), touch("data", 0, 1, "y", middle),
        touch("begin", 1, 2, "x", middle + half(start)), touch("data", 1, 2, "x", middle + half(start)), touch("data", 1, 2, "y", middle)])
    for i in range(1, steps + 1):
        spread = start + (end - start) * i / steps
        # A data event is for the slot last named: update names it.
        call("input-send-event", events=[touch("update", 0, 1), touch("data", 0, 1, "x", middle - half(spread)),
                                         touch("update", 1, 2), touch("data", 1, 2, "x", middle + half(spread))])
        time.sleep(0.015)
    call("input-send-event", events=lift)
    connection.close()
    time.sleep(0.8)
    return ok


point(*empty_spot(ctl()))
if "pinch-touchpad" in vm("hyprctl devices -j") and device_pinch(0.5, 0.25):
    s = ctl()
    check("Try Omarchy's pinch device: pinching in on the desktop zooms out", s["state"] == "overview" and s["scale"] < 0.8, f"{s['state']} {s['scale']}")
    before = ctl()["scale"]
    device_pinch(0.25, 0.5)
    check("Try Omarchy's pinch device: spreading zooms in", ctl()["scale"] > before * 1.4, f"{before} -> {ctl()['scale']}")
    qmp("key", "esc")
    settle()
else:
    print("SKIP  Try Omarchy's pinch device (not in this VM)")

# ===== Several monitors (a headless one beside the screen).
vm("hyprctl output create headless SWEEP-2")
wait(3)
W, H = layout()
second = next(m for m in json.loads(vm("hyprctl monitors -j")) if m["name"] == "SWEEP-2")
sx, sy = second["x"], second["y"]
sw, sh = second["width"] / second["scale"], second["height"] / second["scale"]
point(OX + MW / 4, OY + MH / 2)
refused = vm(f"hyprctl expanse space {second['activeWorkspace']['name']}")
check("hyprctl expanse space refuses another monitor's workspace", "isn't on the canvas's monitor" in refused and ctl()["state"] == "idle", refused.strip())
focus(B)
settle()
r = real()
move_by(A, sx + 200 - r[A][0], sy + 150 - r[A][1])
a = real()[A]
body = pixel(*centre(real()[B]))
there = pixel(*centre(a))
check("several monitors: a window parked over the other monitor isn't drawn there", there != body, f"{there} vs {body}")
point(*centre(a))
check("and doesn't take the pointer's focus there", active() != A, active())
point(OX + MW / 4, OY + MH / 2)
focus(A)
settle()
a = real()[A]
check("keyboard focus brings it back to the middle of its own monitor", centred(a), f"{a}")
point(sx + sw / 2, sy + sh / 2)
before = real()
zoom_out()
s = ctl()
check("the canvas opens on the monitor with the pointer", s["workspace"] == second["activeWorkspace"]["name"], f"{s['workspace']}")
qmp("key", "esc")
settle()
check("and closes without moving anything", real() == before)
point(OX + MW / 4, OY + MH / 2)
zoom_out()
point(sx + sw / 2, sy + sh / 2)
qmp("click")
check("a click on the other monitor closes the canvas", settle())
open_app(G, "600 380", second["activeWorkspace"]["name"])
K = f"sweep-k-{RUN}"
open_app(K, "400 300", second["activeWorkspace"]["name"])
g = real()[G]
point(sx + sw / 2, sy + sh / 2)
vm("hyprctl output remove SWEEP-2")
wait(3)
gone = real()[G]
landed = next(m for m in json.loads(vm("hyprctl monitors -j")) if m["id"] == client(G)["monitor"])
check("unplugging a monitor keeps its windows' places on the screen", gone[0] == g[0] - sx + landed["x"] and gone[1] == g[1] - sy + landed["y"], f"{g} -> {gone} on {landed['name']}")
# Hyprland moves the pointer about as the monitor goes (often into a
# bottom corner of another), and re-sends its position as windows close
# there. That isn't moving into the hot corner.
gone_state = ctl()["state"]
vm(f"kill {client(K)['pid']}")
wait(1.5)
check("unplugging the monitor with the pointer on it, or a window closing after, doesn't zoom out", gone_state == "idle" and ctl()["state"] == "idle", f"{gone_state} {ctl()['state']} {cursor()}")
vm("hyprctl output create headless SWEEP-2")
wait(3.5)
back = real()[G]
check("plugging it back in puts them back exactly", back == g, f"{g} -> {back}")
vm("hyprctl output remove SWEEP-2")
wait(2)
W, H = layout()
point(OX + MW / 2, OY + MH / 2)

# ===== Edge cases.
# The workspaces bar fits beside the minimap however many workspaces there are.
EXTRA = [f"name:edge{i}" for i in range(26)]
for name in EXTRA:
    vm(f'hyprctl dispatch "hl.dsp.exec_cmd(\\"foot --app-id=sweep-edge-{RUN} sleep infinity\\", {{ workspace = \\"{name} silent\\", float = true, size = \\"400 300\\" }})"')
wait(6)
zoom_out()
s = ctl()
tiles = [t["tile"] for t in s["spaces"]]
m = s["minimap"]
check("the workspaces bar fits beside the minimap with 30 workspaces", len(tiles) >= 28 and min(t[0] for t in tiles) >= 16 and max(t[0] + t[2] for t in tiles) <= m[0] - 12,
      f"{len(tiles)} tiles from {min(t[0] for t in tiles) if tiles else None} to {max(t[0] + t[2] for t in tiles) if tiles else None}, minimap at {m}")
qmp("key", "esc")
settle()
for c in clients():
    if c["class"] == f"sweep-edge-{RUN}":
        vm(f"kill {c['pid']}")
wait(1.5)

# A workspace bind right as a glide ends: you stay on the workspace you went to.
stayed = []
for i, delay in enumerate((0.1, 0.18, 0.25, 0.3, 0.35, 0.42, 0.5)):
    focus(A)
    settle()
    qmp("key", "meta_l+right" if i % 2 == 0 else "meta_l+left", "sleep", int(delay * 1000), "key", f"meta_l+{OTHER_KEY}", "sleep", 2500)
    stayed.append(json.loads(vm("hyprctl activeworkspace -j"))["name"] == other)
    qmp("key", f"meta_l+{WORKSPACE_KEY}", "sleep", 1500)
    settle()
check("a workspace bind as a glide ends never bounces back", all(stayed), f"{stayed}")

# ===== Unloading in the middle of things.
UNLOAD = "hyprctl plugin unload ~/expanse-hypr/expanse.so >/dev/null; hyprctl plugin load ~/expanse-hypr/expanse.so >/dev/null"
for _ in range(4):
    qmp("key", "meta_l+right")
    vm(UNLOAD)
    wait(0.8)
for _ in range(3):
    zoom_out()
    vm(UNLOAD)
    wait(0.8)
for i in range(3):
    vm(f'hyprctl dispatch "hl.dsp.exec_cmd(\\"foot --app-id={G}-{i} sleep infinity\\", {{ float = true }})" >/dev/null; sleep 0.15; {UNLOAD}')
    wait(1.5)
for _ in range(2):
    zoom_out()
    vm(f'hyprctl dispatch "hl.dsp.cursor.move({{ x = {OX + 600}, y = {OY + 400} }})"')
    qmp("press", "sleep", 150)
    vm(f'hyprctl dispatch "hl.dsp.cursor.move({{ x = {OX + 640}, y = {OY + 400} }})"')
    vm(UNLOAD)
    qmp("release", "sleep", 300)
    qmp("key", "esc")
    settle()
check("unloading mid-glide, zoomed out, mid-drag and as windows open never crashes Hyprland",
      vm("pgrep -x Hyprland").strip() == hyprland and vm("ls ~/.cache/hyprland | wc -l").strip() == reports)

for c in clients():
    if c["class"].startswith(f"sweep-") and c["class"].endswith(RUN) or c["class"].startswith(f"{G}-") or c["class"] == DIALOG:
        vm(f"kill {c['pid']}")
APPS += [D, E, F, G, K]

# --- Clean up.
for c in json.loads(vm("hyprctl clients -j")):
    if c["class"] in APPS:
        vm(f"kill {c['pid']}")
wait(1)
vm('hyprctl dispatch "hl.dsp.focus({ workspace = 1 })"')
print(f"\n{len(failed)} failed" + (": " + "; ".join(failed) if failed else ""))
sys.exit(len(failed))
