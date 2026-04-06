#!/usr/bin/env python3
"""
Synthetic mouse-click helper using Quartz CGEvents.

Bypasses any application-level click interception (overlay apps like
BetterDisplay, accessibility shims, etc.) by posting events directly to
the HID event tap — the same path real hardware uses.

Usage:
    click.py x y                  # left click at (x, y)
    click.py x y double           # double click
    click.py x y triple           # triple click
    click.py x y right            # right click
    click.py x y move             # just move the cursor, no click
    click.py x y drag x2 y2       # press, move to x2,y2, release

Coordinates are in macOS screen points (top-left origin).

IMPORTANT: screenshots captured by MCP/Claude Code clients are typically
1456 pixels wide, while macOS Sequoia on a 2560-point-wide display
reports coordinates in points. The conversion factor is ~1.758x.
See tools/README.md for the calibration procedure.

Why this tool exists
--------------------
During end-to-end testing of GitBolt on macOS we discovered that
BetterDisplay (a popular menu-bar display manager) installs a
fullscreen invisible overlay that intercepts clicks. The computer-use
MCP's click collision detection refuses to click "through" it, which
means automated testing of GitBolt's UI was completely blocked.

CGEventPost with kCGHIDEventTap bypasses the overlay entirely because
it injects the event at the lowest layer — before any application-
level event filtering can see it. This is the same mechanism cliclick
and other Quartz-based automation tools use.

Requirements: pyobjc-framework-Quartz (`pip3 install --user pyobjc-framework-Quartz`)
"""
import sys
import time

try:
    from Quartz import (
        CGEventCreateMouseEvent,
        CGEventPost,
        CGEventSetIntegerValueField,
        kCGEventLeftMouseDown,
        kCGEventLeftMouseDragged,
        kCGEventLeftMouseUp,
        kCGEventMouseMoved,
        kCGEventRightMouseDown,
        kCGEventRightMouseUp,
        kCGHIDEventTap,
        kCGMouseButtonLeft,
        kCGMouseButtonRight,
        kCGMouseEventClickState,
    )
except ImportError as e:
    sys.stderr.write(
        "error: PyObjC Quartz bindings not installed.\n"
        "    pip3 install --user pyobjc-framework-Quartz\n"
    )
    sys.exit(1)


def post_click_pair(kind_down, kind_up, button, x, y, click_count):
    """Post a press+release pair with a consistent click count field.

    Setting kCGMouseEventClickState on BOTH the down and up events is
    what AppKit and Qt use to distinguish single vs double vs triple
    clicks — the click count must be the same on the pair.
    """
    down = CGEventCreateMouseEvent(None, kind_down, (x, y), button)
    CGEventSetIntegerValueField(down, kCGMouseEventClickState, click_count)
    CGEventPost(kCGHIDEventTap, down)

    up = CGEventCreateMouseEvent(None, kind_up, (x, y), button)
    CGEventSetIntegerValueField(up, kCGMouseEventClickState, click_count)
    CGEventPost(kCGHIDEventTap, up)


def move_cursor(x, y):
    """Post a cursor-move event so the mouse visibly tracks to (x, y)."""
    ev = CGEventCreateMouseEvent(None, kCGEventMouseMoved, (x, y), kCGMouseButtonLeft)
    CGEventPost(kCGHIDEventTap, ev)


def left_click(x, y, count=1):
    """Single/double/triple left click at (x, y)."""
    move_cursor(x, y)
    for n in range(1, count + 1):
        post_click_pair(
            kCGEventLeftMouseDown,
            kCGEventLeftMouseUp,
            kCGMouseButtonLeft,
            x, y,
            n,
        )


def right_click(x, y):
    """Right click at (x, y)."""
    move_cursor(x, y)
    post_click_pair(
        kCGEventRightMouseDown,
        kCGEventRightMouseUp,
        kCGMouseButtonRight,
        x, y,
        1,
    )


def drag(x1, y1, x2, y2, steps=20):
    """Press at (x1,y1), drag to (x2,y2), release."""
    move_cursor(x1, y1)
    down = CGEventCreateMouseEvent(None, kCGEventLeftMouseDown, (x1, y1), kCGMouseButtonLeft)
    CGEventSetIntegerValueField(down, kCGMouseEventClickState, 1)
    CGEventPost(kCGHIDEventTap, down)

    # Interpolated drag events so the target sees a realistic motion path
    for i in range(1, steps + 1):
        t = i / steps
        x = round(x1 + (x2 - x1) * t)
        y = round(y1 + (y2 - y1) * t)
        ev = CGEventCreateMouseEvent(None, kCGEventLeftMouseDragged, (x, y), kCGMouseButtonLeft)
        CGEventSetIntegerValueField(ev, kCGMouseEventClickState, 1)
        CGEventPost(kCGHIDEventTap, ev)
        time.sleep(0.01)  # ~16 ms/frame

    up = CGEventCreateMouseEvent(None, kCGEventLeftMouseUp, (x2, y2), kCGMouseButtonLeft)
    CGEventSetIntegerValueField(up, kCGMouseEventClickState, 1)
    CGEventPost(kCGHIDEventTap, up)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)

    x = int(sys.argv[1])
    y = int(sys.argv[2])
    mode = sys.argv[3] if len(sys.argv) > 3 else "click"

    if mode == "move":
        move_cursor(x, y)
    elif mode == "click":
        left_click(x, y, 1)
    elif mode == "double":
        left_click(x, y, 2)
    elif mode == "triple":
        left_click(x, y, 3)
    elif mode == "right":
        right_click(x, y)
    elif mode == "drag":
        if len(sys.argv) < 6:
            sys.stderr.write("error: drag mode requires x y drag x2 y2\n")
            sys.exit(2)
        x2 = int(sys.argv[4])
        y2 = int(sys.argv[5])
        drag(x, y, x2, y2)
    else:
        sys.stderr.write(f"error: unknown mode '{mode}'\n")
        sys.exit(2)

    print(f"{mode} at ({x}, {y})")


if __name__ == "__main__":
    main()
