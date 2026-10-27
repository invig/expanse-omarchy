#!/usr/bin/python3 -I
"""A virtual touchpad, to pinch through the kernel and libinput as a real one does.

    sudo scripts/touchpad.py pinch FROM TO [SECONDS]

Creates a two-finger touchpad with uinput (no dependencies; needs root for
/dev/uinput), waits for the compositor to pick it up, then moves two fingers
from FROM to TO apart (as fractions of the pad's width; less is pinching in)
over SECONDS, and lifts them. libinput turns that into a pinch gesture.
"""

import fcntl
import os
import struct
import sys
import time

# linux/input-event-codes.h
EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
BTN_LEFT, BTN_TOOL_FINGER, BTN_TOUCH, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP = 0x110, 0x145, 0x14A, 0x14D, 0x14E
ABS_X, ABS_Y = 0x00, 0x01
ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x2F, 0x35, 0x36, 0x39
INPUT_PROP_POINTER, INPUT_PROP_BUTTONPAD = 0x00, 0x02
BUS_VIRTUAL = 0x06

# linux/uinput.h
UINPUT_IOCTL_BASE = ord("U")


def _IOW(nr, size):
    return (1 << 30) | (size << 16) | (UINPUT_IOCTL_BASE << 8) | nr


def _IO(nr):
    return (UINPUT_IOCTL_BASE << 8) | nr


UI_DEV_CREATE, UI_DEV_DESTROY = _IO(1), _IO(2)
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT, UI_SET_PROPBIT = _IOW(100, 4), _IOW(101, 4), _IOW(103, 4), _IOW(110, 4)
SETUP = struct.Struct("HHHH80sI")             # struct uinput_setup: input_id, name, ff_effects_max
ABS_SETUP = struct.Struct("H2xiiiiii")         # struct uinput_abs_setup: code, input_absinfo
UI_DEV_SETUP, UI_ABS_SETUP = _IOW(3, SETUP.size), _IOW(4, ABS_SETUP.size)
EVENT = struct.Struct("llHHi")                 # struct input_event (64-bit)

WIDTH, HEIGHT, RESOLUTION = 4000, 2500, 40     # 100 x 62.5 mm


def create(fd):
    for ev in (EV_SYN, EV_KEY, EV_ABS):
        fcntl.ioctl(fd, UI_SET_EVBIT, ev)
    for key in (BTN_LEFT, BTN_TOOL_FINGER, BTN_TOUCH, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP):
        fcntl.ioctl(fd, UI_SET_KEYBIT, key)
    for prop in (INPUT_PROP_POINTER, INPUT_PROP_BUTTONPAD):
        fcntl.ioctl(fd, UI_SET_PROPBIT, prop)
    axes = {
        ABS_X: (0, WIDTH, RESOLUTION), ABS_Y: (0, HEIGHT, RESOLUTION),
        ABS_MT_POSITION_X: (0, WIDTH, RESOLUTION), ABS_MT_POSITION_Y: (0, HEIGHT, RESOLUTION),
        ABS_MT_SLOT: (0, 4, 0), ABS_MT_TRACKING_ID: (0, 65535, 0),
    }
    for code, (low, high, resolution) in axes.items():
        fcntl.ioctl(fd, UI_SET_ABSBIT, code)
        fcntl.ioctl(fd, UI_ABS_SETUP, ABS_SETUP.pack(code, 0, low, high, 0, 0, resolution))
    fcntl.ioctl(fd, UI_DEV_SETUP, SETUP.pack(BUS_VIRTUAL, 0x1234, 0x5678, 1, b"Expanse test touchpad", 0))
    fcntl.ioctl(fd, UI_DEV_CREATE)


def emit(fd, events):
    now = time.time()
    seconds, micros = int(now), int((now % 1) * 1e6)
    os.write(fd, b"".join(EVENT.pack(seconds, micros, kind, code, value) for kind, code, value in events + [(EV_SYN, SYN_REPORT, 0)]))


def fingers(spread):
    # Two fingers either side of the middle, `spread` (a fraction of the width) apart.
    middle_x, y = WIDTH // 2, HEIGHT // 2
    half = int(spread * WIDTH / 2)
    return (middle_x - half, y), (middle_x + half, y)


def pinch(fd, start, end, seconds):
    (ax, ay), (bx, by) = fingers(start)
    emit(fd, [(EV_ABS, ABS_MT_SLOT, 0), (EV_ABS, ABS_MT_TRACKING_ID, 1), (EV_ABS, ABS_MT_POSITION_X, ax), (EV_ABS, ABS_MT_POSITION_Y, ay),
              (EV_ABS, ABS_MT_SLOT, 1), (EV_ABS, ABS_MT_TRACKING_ID, 2), (EV_ABS, ABS_MT_POSITION_X, bx), (EV_ABS, ABS_MT_POSITION_Y, by),
              (EV_KEY, BTN_TOUCH, 1), (EV_KEY, BTN_TOOL_DOUBLETAP, 1), (EV_ABS, ABS_X, ax), (EV_ABS, ABS_Y, ay)])
    steps = max(int(seconds / 0.012), 2)
    for i in range(1, steps + 1):
        time.sleep(seconds / steps)
        (ax, ay), (bx, by) = fingers(start + (end - start) * i / steps)
        emit(fd, [(EV_ABS, ABS_MT_SLOT, 0), (EV_ABS, ABS_MT_POSITION_X, ax), (EV_ABS, ABS_MT_SLOT, 1), (EV_ABS, ABS_MT_POSITION_X, bx),
                  (EV_ABS, ABS_X, ax)])
    time.sleep(0.05)
    emit(fd, [(EV_ABS, ABS_MT_SLOT, 0), (EV_ABS, ABS_MT_TRACKING_ID, -1), (EV_ABS, ABS_MT_SLOT, 1), (EV_ABS, ABS_MT_TRACKING_ID, -1),
              (EV_KEY, BTN_TOUCH, 0), (EV_KEY, BTN_TOOL_DOUBLETAP, 0)])


def main():
    if len(sys.argv) < 4 or sys.argv[1] != "pinch":
        raise SystemExit(__doc__)
    start, end = float(sys.argv[2]), float(sys.argv[3])
    seconds = float(sys.argv[4]) if len(sys.argv) > 4 else 0.6
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    try:
        create(fd)
        time.sleep(1.5)  # for udev, libinput and the compositor to add it
        pinch(fd, start, end, seconds)
        time.sleep(0.5)
    finally:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)


if __name__ == "__main__":
    main()
