#!/usr/bin/env python3
"""E2E: the periodic background fetch never asks for credentials.

Every git GitBolt starts gets GitBolt itself as its askpass, so a
question git or ssh has for the user becomes a dialog. The auto-fetch
had it too: against a server that wants a password, each tick popped
an always-on-top "Username for …" dialog nobody asked for, and the
fetch hung on it until it was answered or git's 2-minute timeout. An
auto-fetch now fails at once instead, with git's own reason in its
note; the toolbar Fetch, which the user did ask for, still asks (and a
credential helper keeps what is typed there for the ticks after).

The server is AuthServer: every request without credentials gets a 401.
"""

from __future__ import annotations

import os
import subprocess
import time

from e2elib import (AUTO_FETCH_TIMER, App, AuthServer, Failure,
                    auto_fetch_config, git, make_repo, run)


def status(state: dict) -> str:
    return state.get("statusMessage", "")


def test(binary: str, scratch: str, apps: list) -> None:
    # git's own config only: a credential helper from the system's or
    # the user's (osxkeychain, Git Credential Manager) would answer, or
    # ask in a window of its own, in place of GitBolt's askpass.
    os.environ["GIT_CONFIG_NOSYSTEM"] = "1"
    os.environ["GIT_CONFIG_GLOBAL"] = os.devnull
    work = make_repo(os.path.join(scratch, "work"))
    server = AuthServer()
    try:
        git("remote", "add", "origin", server.url, cwd=work)
        app = App(binary, work, config_home=auto_fetch_config(scratch))
        apps.append(app)
        app.wait_bridge()
        app.wait_until(lambda s: s.get("repoOpen"), "repo open")
        _check(app, server)
    finally:
        server.close()


def _check(app: App, server: AuthServer) -> None:
    # 1. The auto-fetch fails at once, with git's reason, asking nothing.
    app.ok(f"fire-timer {AUTO_FETCH_TIMER}")
    try:
        state = app.wait_until(
            lambda s: status(s).startswith("Auto-fetch failed:"),
            "the auto-fetch failure note", timeout=15)
    except Failure:
        raise Failure("the auto-fetch did not fail at once; askpass "
                      f"running: {server.askpass_children()}") from None
    if "terminal prompts disabled" not in status(state):
        raise Failure(f"not git's reason: {status(state)!r}")
    if server.askpass_children():
        raise Failure("the auto-fetch asked for credentials: "
                      f"{server.askpass_children()}")

    # 2. A Fetch the user clicks still asks.
    app.ok("trigger act.fetch")
    deadline = time.monotonic() + 15
    while not server.askpass_children():
        if time.monotonic() > deadline:
            raise Failure("the toolbar Fetch did not ask for credentials; "
                          f"status: {status(app.state())!r}")
        if status(app.state()) == "Another remote operation is still running…":
            # The auto-fetch's finished handler was still queued.
            app.ok("trigger act.fetch")
        time.sleep(0.3)

    # Quitting stops git, and the question with it.
    try:
        rc = app.quit(timeout=15)
    except subprocess.TimeoutExpired:
        raise Failure("GitBolt was still running 15 s after quit") from None
    if rc != 0:
        raise Failure(f"GitBolt exited with {rc} after quit")
    deadline = time.monotonic() + 5
    while server.askpass_children():
        if time.monotonic() > deadline:
            raise Failure("the askpass outlived GitBolt: "
                          f"{server.askpass_children()}")
        time.sleep(0.2)


if __name__ == "__main__":
    run(test)
