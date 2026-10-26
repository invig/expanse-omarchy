#!/usr/bin/python3 -I
"""Real keyboard and tablet input for the Try Omarchy VM, through QEMU's QMP.

    qmp-input.py SOCKET STEP...
      key COMBO            qcodes joined by +, e.g. alt+grave_accent
      move X Y W H         pointer to X,Y on a WxH (logical) screen
      click [left|right]   press and release a button
      press [button] / release [button]
      wheel up|down|left|right [N]
      hold KEY / let KEY    a qcode down, or up again (for chords with the pointer)
      sleep MS
"""

import json
import socket
import sys
import time


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    connection = socket.socket(socket.AF_UNIX)
    connection.settimeout(5)
    connection.connect(sys.argv[1])
    stream = connection.makefile("rw")

    def call(command, **arguments):
        stream.write(json.dumps({"execute": command, **({"arguments": arguments} if arguments else {})}) + "\n")
        stream.flush()
        while True:
            reply = json.loads(stream.readline())
            if "return" in reply:
                return reply["return"]
            if "error" in reply:
                raise SystemExit(f"qmp-input: {reply['error'].get('desc', reply['error'])}")

    json.loads(stream.readline())
    call("qmp_capabilities")

    args = sys.argv[2:]
    i = 0
    while i < len(args):
        step = args[i]
        if step == "key":
            call("send-key", keys=[{"type": "qcode", "data": k} for k in args[i + 1].split("+")], **{"hold-time": 60})
            time.sleep(0.15)
            i += 2
        elif step == "move":
            x, y, w, h = (float(v) for v in args[i + 1:i + 5])
            call("input-send-event", events=[
                {"type": "abs", "data": {"axis": "x", "value": int(x / w * 32767)}},
                {"type": "abs", "data": {"axis": "y", "value": int(y / h * 32767)}},
            ])
            time.sleep(0.05)
            i += 5
        elif step == "click":
            button = args[i + 1] if i + 1 < len(args) and args[i + 1] in ("left", "right", "middle") else "left"
            call("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": button}}])
            time.sleep(0.06)
            call("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": button}}])
            time.sleep(0.05)
            i += 2 if button == (args[i + 1] if i + 1 < len(args) else None) else 1
        elif step in ("press", "release"):
            button = args[i + 1] if i + 1 < len(args) and args[i + 1] in ("left", "right", "middle") else "left"
            call("input-send-event", events=[{"type": "btn", "data": {"down": step == "press", "button": button}}])
            time.sleep(0.05)
            i += 2 if button == (args[i + 1] if i + 1 < len(args) else None) else 1
        elif step == "wheel":
            way = args[i + 1]
            count = int(args[i + 2]) if i + 2 < len(args) and args[i + 2].isdigit() else 1
            for _ in range(count):
                for down in (True, False):
                    call("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "wheel-" + way}}])
                time.sleep(0.03)
            i += 3 if i + 2 < len(args) and args[i + 2].isdigit() else 2
        elif step in ("hold", "let"):
            call("input-send-event", events=[{"type": "key", "data": {"down": step == "hold", "key": {"type": "qcode", "data": args[i + 1]}}}])
            time.sleep(0.05)
            i += 2
        elif step == "sleep":
            time.sleep(float(args[i + 1]) / 1000)
            i += 2
        else:
            raise SystemExit(f"qmp-input: unknown step {step!r}")


if __name__ == "__main__":
    main()
