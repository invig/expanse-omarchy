#!/usr/bin/python3 -I
"""Goes through Expanse's features in a Try Omarchy VM and checks each one.

    scripts/sweep.py

Real keyboard and pointer input through QEMU's QMP socket (scripts/qmp-input.py),
everything else through `hyprctl` over SSH (VM_PORT, VM_KEY as scripts/vm.sh).
It opens its own windows on an empty workspace and closes them at the end.
Each check prints PASS or FAIL; the exit status is the number that failed.
"""

import glob
import json
import os
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
WORKSPACE = "7"
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


W, H = layout()


def point(x, y):
    # The pointer, moved the last few pixels as a separate step so Hyprland sees motion there.
    qmp("move", int(x) - 6, int(y) - 6, W, H, "sleep", 80, "move", int(x), int(y), W, H, "sleep", 250)


def wait(seconds=1.6):
    time.sleep(seconds)


def settle(timeout=8):
    # Until the canvas is back at 100% (frames crawl while the VM window is hidden).
    end = time.time() + timeout
    while time.time() < end:
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


def check(name, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}  {name}{'  (' + detail + ')' if detail and not ok else ''}")
    if not ok:
        failed.append(name)


def mine(state=None):
    state = state or ctl()
    return {w["class"]: w for w in state["windows"] if w["class"] in APPS}


def box(window, key="canvas"):
    return window[key]


def overlap(a, b):
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


def any_overlap(boxes):
    return any(overlap(a, b) for i, a in enumerate(boxes) for b in boxes[i + 1:])


def centre(b):
    return b[0] + b[2] / 2, b[1] + b[3] / 2


def gap(a, b):
    # The space between two boxes side by side, or one above the other.
    horizontal = max(b[0] - (a[0] + a[2]), a[0] - (b[0] + b[2]))
    vertical = max(b[1] - (a[1] + a[3]), a[1] - (b[1] + b[3]))
    return max(horizontal, vertical)


def real():
    return {c["class"]: c["at"] + c["size"] for c in json.loads(vm("hyprctl clients -j")) if c["class"] in APPS}


def screen_middle():
    usable = json.loads(vm("hyprctl monitors -j"))[0]
    left, top, right, bottom = usable["reserved"]
    w, h = usable["width"] / usable["scale"], usable["height"] / usable["scale"]
    return usable["x"] + left + (w - left - right) / 2, usable["y"] + top + (h - top - bottom) / 2


# --- Setup: three windows of apps with no saved spots, on an empty workspace.
vm(f'hyprctl dispatch "hl.dsp.focus({{ workspace = {WORKSPACE} }})"')
wait(1)
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
check("zoomed out, every window is on screen", all(0 <= w["screen"][0] and w["screen"][0] + w["screen"][2] <= W for w in mine(s).values()))
qmp("key", "alt+grave_accent")
check("Alt + ` again goes back to 100%", settle())
check("going back without picking leaves every window where it was", real() == before, f"{before} -> {real()}")

# --- Hot corner (bottom corners by default).
point(W / 2, H / 2)
qmp("move", 1, H - 1, W, H, "sleep", 80, "move", 0, H, W, H, "sleep", 900)
check("resting in a bottom corner zooms out", ctl()["state"] == "overview")
point(W / 2, H / 2)
qmp("move", 1, H - 1, W, H, "sleep", 80, "move", 0, H, W, H, "sleep", 900)
check("the corner again zooms back in", settle())
point(W / 2, H / 2)

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
check("0 fits everything", all(0 <= w["screen"][0] and w["screen"][0] + w["screen"][2] <= W for w in mine(fit).values()))

# --- Super + scroll zooms around the pointer; scroll pans.
point(W * 0.3, H * 0.6)
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
point(W - 16 - 208 + 30, H - 16 - 136 + 30)
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
for x in range(100, W - 300, 150):
    for y in range(150, H - 300, 150):
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

# --- Follows you between screens, and back exactly.
before = real()
ctl("screen 1440 874")
wait(2.5)
small = real()
fits = all(b[2] <= 1440 and b[3] <= 874 for b in small.values())
check("on a smaller screen every window fits", fits, f"{small}")
ctl("screen")
wait(2.5)
check("back on the big screen everything returns exactly", real() == before, f"{before} -> {real()}")

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
usable_ok = all(0 <= b[0] and b[0] + b[2] <= W and 0 <= b[1] and b[1] + b[3] <= H for b in real().values())
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

# --- Clean up.
for c in json.loads(vm("hyprctl clients -j")):
    if c["class"] in APPS:
        vm(f"kill {c['pid']}")
wait(1)
vm('hyprctl dispatch "hl.dsp.focus({ workspace = 1 })"')
print(f"\n{len(failed)} failed" + (": " + "; ".join(failed) if failed else ""))
sys.exit(len(failed))
