#!/usr/bin/env python3
"""E2E: the app's desktop file name names the desktop entry it ships.

On Wayland the window's app_id is QGuiApplication::desktopFileName(),
and GNOME Shell and KWin find the window's icon and launcher by
"<app_id>.desktop", then by StartupWMClass. GitBolt never set it, so
Qt made up "com.gitbolt.gitbolt", which no installed file matches: a
generic icon, and a window apart from its launcher and favorite.
dump-state reports the name the app runs with; the entry it names
must exist and claim that class too (a renamed copy, as AppImage
integrators make, then still matches).
"""

from __future__ import annotations

import os

from e2elib import App, Failure, make_repo, run

LINUX_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         os.pardir, os.pardir, "src", "app", "platform",
                         "linux")


def test(binary: str, scratch: str, apps: list) -> None:
    repo = make_repo(os.path.join(scratch, "repo"))
    app = App(binary, repo)
    apps.append(app)
    app.wait_bridge()
    state = app.wait_until(lambda s: s.get("repoOpen"), "repo open")

    name = state.get("desktopFileName")
    if name != "gitbolt":
        raise Failure(f"desktopFileName is {name!r}, expected 'gitbolt' "
                      "(the shipped gitbolt.desktop)")
    entry = os.path.join(LINUX_DIR, f"{name}.desktop")
    if not os.path.isfile(entry):
        raise Failure(f"no desktop entry {entry} for desktopFileName {name!r}")
    with open(entry) as fh:
        lines = fh.read().splitlines()
    if f"StartupWMClass={name}" not in lines:
        raise Failure(f"{entry} lacks StartupWMClass={name}")

    rc = app.quit()
    if rc != 0:
        raise Failure(f"quit rc={rc}{app.tail()}")


if __name__ == "__main__":
    run(test)
