#!/usr/bin/env python3
"""E2E: a persisted Dark theme launches dark — palette, not just INI.

Regression lock for the startup-ordering bug where the color-scheme
reset ran after the restored theme was applied: the INI said Dark,
dump-state's stored preference said Dark, but the window rendered
light. Only the *effective* palette distinguishes that state, so the
assertion here is on paletteWindow's luminance.
"""

from __future__ import annotations

import os

from e2elib import App, Failure, make_repo, run


def _luminance(hex_color: str) -> float:
    r = int(hex_color[1:3], 16)
    g = int(hex_color[3:5], 16)
    b = int(hex_color[5:7], 16)
    return 0.299 * r + 0.587 * g + 0.114 * b


def _assert_dark(state: dict, which: str) -> None:
    if state.get("theme") != "Dark":
        raise Failure(f"{which}: stored theme is {state.get('theme')!r}, "
                      "expected 'Dark'")
    palette = state.get("paletteWindow", "")
    if not palette.startswith("#") or len(palette) != 7:
        raise Failure(f"{which}: no paletteWindow in dump-state: {palette!r}")
    lum = _luminance(palette)
    if lum > 90:
        raise Failure(f"{which}: INI says Dark but the effective palette "
                      f"is light ({palette}, luminance {lum:.0f}) — "
                      "startup ordering regression")


def test(binary: str, scratch: str, apps: list) -> None:
    repo = make_repo(os.path.join(scratch, "repo"))
    config_home = os.path.join(scratch, "config")
    ini_dir = os.path.join(config_home, "GitBolt")
    os.makedirs(ini_dir)
    with open(os.path.join(ini_dir, "GitBolt.ini"), "w") as fh:
        fh.write("[appearance]\ntheme=Dark\n")

    first = App(binary, repo, config_home=config_home, label="first run")
    apps.append(first)
    first.wait_bridge()
    _assert_dark(first.state(), "first run")
    rc = first.quit()
    if rc != 0:
        raise Failure(f"first run exited rc={rc}{first.tail()}")

    # Relaunch against the same config: the preference written by the
    # previous session must survive a full restart.
    second = App(binary, repo, config_home=config_home, label="relaunch")
    apps.append(second)
    second.wait_bridge()
    _assert_dark(second.state(), "relaunch")
    rc = second.quit()
    if rc != 0:
        raise Failure(f"relaunch exited rc={rc}{second.tail()}")


if __name__ == "__main__":
    run(test)
