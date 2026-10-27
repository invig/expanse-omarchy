#!/usr/bin/python3 -I
"""Times the canvas's frames in a Try Omarchy VM, to find jank.

    scripts/perf.py              on Virtual-1, with the windows open there
    scripts/perf.py headless [N] on a headless monitor of the same size, with N windows of its own
    add --gpu                    to run everything again, timing each frame on the GPU (which slows them down)
    add --wait S                 to start after S seconds (time to bring the VM's window on screen)

Virtual-1 is only drawn as fast as the VM's window on the Mac is: while that
window isn't on screen (another Space), its frames crawl. A headless monitor
draws on a steady timer, so it measures the compositor's side alone.

Drives each motion with `hyprctl` over SSH (VM_PORT, VM_KEY as scripts/vm.sh)
while the plugin records every frame (`hyprctl expanse perf`), then prints,
per motion: frames drawn, how often a frame missed the screen's refresh, and
how long frames took to draw, all of it and the plugin's own share. A
workspace switch, Hyprland's own animation, is the baseline for the VM.
"""

import atexit
import json
import os
import shlex
import statistics
import subprocess
import sys
import time

PORT = os.environ.get("VM_PORT", "2222")
KEY = os.environ.get("VM_KEY", os.path.expanduser("~/.ssh/omarchy_vm_ed25519"))
SSH = ["ssh", "-i", KEY, "-p", PORT, "-o", "BatchMode=yes", "-o", "LogLevel=ERROR", "test@127.0.0.1"]
ENV = ("export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-1; "
       "export HYPRLAND_INSTANCE_SIGNATURE=$(ls -t $XDG_RUNTIME_DIR/hypr | head -1); ")
GPU_PASS = "--gpu" in sys.argv
WAIT = float(sys.argv[sys.argv.index("--wait") + 1]) if "--wait" in sys.argv else 0
ARGS = [a for i, a in enumerate(sys.argv[1:], 1) if a != "--gpu" and a != "--wait" and sys.argv[i - 1] != "--wait"]
GPU = False
HEADLESS = len(ARGS) > 0 and ARGS[0] == "headless"
COUNT = int(ARGS[1]) if HEADLESS and len(ARGS) > 1 else 12
# The sweep's test monitor and its rule (scripts/sweep-monitors.lua, which the VM's config runs).
MONITOR = "SWEEP-MAIN" if HEADLESS else "Virtual-1"
RULES = "/home/test/expanse-hypr/scripts/sweep-monitors.lua"
CONFIG = "~/.config/hypr/hyprland.lua"
RUN = str(int(time.time()))[-5:]


def vm(command):
    return subprocess.run(SSH + [ENV + command], capture_output=True, text=True).stdout


def percentile(values, p):
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(round(p / 100 * (len(ordered) - 1))))]


def report(name, data):
    mon = data.get(MONITOR)
    if not mon:
        print(f"{name:34} no frames on {MONITOR}")
        return
    period = 1000 / mon["refresh"]
    shown = mon["presented"]
    # Gaps longer than a quarter second are the screen at rest, not jank.
    gaps = [b - a for a, b in zip(shown, shown[1:]) if b - a < 250]
    missed = sum(max(0, round(g / period) - 1) for g in gaps)
    cpu, plugin, gpu, texts = mon["cpu"], mon["plugin"], mon["gpu"], mon["texts"]
    print(f"{name:34} {len(cpu):4} frames  {len(shown):4} shown  missed {missed:3} refreshes  "
          f"gap p50 {percentile(gaps, 50):5.1f} p95 {percentile(gaps, 95):5.1f} max {max(gaps, default=0):6.1f} ms  "
          f"draw p50 {percentile(cpu, 50):5.1f} p95 {percentile(cpu, 95):5.1f} max {max(cpu, default=0):6.1f}  "
          f"plugin p50 {percentile(plugin, 50):4.1f} max {max(plugin, default=0):5.1f}  "
          + (f"gpu p50 {percentile(gpu, 50):5.1f} p95 {percentile(gpu, 95):5.1f} max {max(gpu, default=0):6.1f}  " if GPU else "")
          + f"ms  texts {int(sum(texts))}")


def measure(name, commands, settle=1.0):
    vm(f"hyprctl expanse perf start{' gpu' if GPU else ''}")
    for command, wait in commands:
        vm(command)
        time.sleep(wait)
    time.sleep(settle)
    out = vm("hyprctl expanse perf stop")
    try:
        data = json.loads(out)
    except json.JSONDecodeError:
        print(f"{name}: {out.strip()}")
        return
    report(name, data)
    last = json.loads(vm("hyprctl expanse")).get("lastAnimation", "-")
    print(f"{'':34} last animation: {last}")


def headless():
    vm(f"grep -qF {shlex.quote(RULES)} {CONFIG} || printf %s {shlex.quote(chr(10) + '-- scripts/sweep.py: its test monitor' + chr(10) + f'dofile(\"{RULES}\")' + chr(10))} >> {CONFIG}")
    vm("hyprctl reload")
    time.sleep(1.5)
    vm(f"hyprctl output create headless {MONITOR}")
    time.sleep(3)
    atexit.register(lambda: (vm(f"pkill -f 'foot --app-id=perf-{RUN}'"), vm(f"hyprctl output remove {MONITOR}"), time.sleep(1), vm("hyprctl reload")))
    vm(f'hyprctl dispatch "hl.dsp.focus({{ monitor = \\"{MONITOR}\\" }})"')
    time.sleep(0.5)
    # Windows of a few sizes, as a desktop has, each placed in free space as it opens.
    sizes = ["1400 900", "900 700", "1200 800", "700 500"]
    for i in range(COUNT):
        vm(f'hyprctl dispatch "hl.dsp.exec_cmd(\\"foot --app-id=perf-{RUN}-{i} btop\\", {{ float = true, size = \\"{sizes[i % len(sizes)]}\\" }})"')
        time.sleep(0.6)
    time.sleep(1.5)


def main():
    global GPU
    if HEADLESS:
        headless()
    time.sleep(WAIT)
    run()
    if GPU_PASS:
        GPU = True
        print("\nAgain, waiting for the GPU after each frame:")
        run()


def run():
    state = json.loads(vm("hyprctl expanse"))
    if state["state"] != "idle":
        vm("hyprctl expanse close")
        time.sleep(1)
    workspace = json.loads(vm("hyprctl activeworkspace -j"))["id"]
    print(f"{MONITOR}: {len(state['windows'])} windows on the canvas")

    go = lambda ws: f"hyprctl dispatch 'hl.dsp.focus({{ workspace = \"{ws}\" }})'"
    measure("workspace switch (Hyprland's own)", [(go(workspace + 1), 0.8), (go(workspace), 0.8)])
    measure("zoom out", [("hyprctl expanse open", 0.6)])
    measure("zoomed out, at rest", [("true", 1.0)], settle=0)
    measure("zoom key (-, then =)", [("hyprctl expanse zoom 0.3", 0.5), ("hyprctl expanse zoom 0.6", 0.5)])
    measure("pan 60 steps", [(" ; ".join(["hyprctl expanse pan 12 0 >/dev/null; sleep 0.016"] * 60), 0.2)])
    measure("back to 100%", [("hyprctl expanse close", 0.8)])
    measure("hops at 100% (right, left)", [("hyprctl expanse hop right", 0.7), ("hyprctl expanse hop left", 0.7)])
    measure("chained hops (3 quick)", [("hyprctl expanse hop right; sleep 0.1; hyprctl expanse hop right; sleep 0.1; hyprctl expanse hop left", 1.0)])
    measure("at rest, 100%", [("true", 1.0)], settle=0)


if __name__ == "__main__":
    main()
