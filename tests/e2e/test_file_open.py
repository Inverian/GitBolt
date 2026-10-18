#!/usr/bin/env python3
"""Folders macOS asks a running GitBolt to open: a Dock drop, `open -a
GitBolt <folder>` and Finder's "Open in GitBolt" all end in a
QFileOpenEvent to the application object (src/app/OpenRequests.h),
which the bridge's file-open sends the same way.

- A folder inside a repository opens that repository, not the folder.
- A folder in no repository says so, and the open repository stays.
- A dotfiles repository at $HOME never claims a folder below it, while
  $HOME itself still opens.
- A file opens the repository it is in.
- Two requests back to back end in the second.
- Off the cocoa platform there is no service provider to send to.
"""
from __future__ import annotations

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2elib import App, Failure, git, make_repo, on_screen, run  # noqa: E402

OPEN_FAILED = "dlg.openFailed"


def same_dir(a: str, b: str) -> bool:
    return os.path.realpath(a.rstrip("/")) == os.path.realpath(b.rstrip("/"))


def opened(app: App, repo: str, what: str) -> dict:
    name = os.path.basename(repo)
    state = app.wait_until(on_screen(name), what)
    if not same_dir(state["repoPath"], repo):
        raise Failure(f"{what}: {state['repoPath']} is open, expected {repo}")
    return state


def open_failed_box(app: App) -> dict | None:
    for w in app.cmd("list-widgets")["windows"]:
        if w.get("objectName") == OPEN_FAILED:
            return w
    return None


def refused(app: App, folder: str, repo_open: str, what: str) -> None:
    """The box says `folder` is in no repository; dismiss it, and the
    repository that was open still is."""
    deadline = time.monotonic() + 20
    box = None
    while box is None and time.monotonic() < deadline:
        box = open_failed_box(app)
        time.sleep(0.2)
    if box is None:
        raise Failure(f"{what}: no {OPEN_FAILED} box")
    expected = f"{folder} is not in a Git repository."
    if box.get("text") != expected:
        raise Failure(f"{what}: the box says {box.get('text')!r}, expected {expected!r}")
    app.ok("click OK")
    deadline = time.monotonic() + 20
    while open_failed_box(app) is not None:
        if time.monotonic() > deadline:
            raise Failure(f"{what}: the box never closed")
        time.sleep(0.2)
    opened(app, repo_open, f"{what}: still open")


def test(binary: str, scratch: str, apps: list) -> None:
    repo_a = make_repo(os.path.join(scratch, "repo-a"))
    repo_b = make_repo(os.path.join(scratch, "repo-b"))
    repo_c = make_repo(os.path.join(scratch, "repo-c"))
    repo_d = make_repo(os.path.join(scratch, "repo-d"))
    deep = os.path.join(repo_b, "src", "app")
    os.makedirs(deep)
    plain = os.path.join(scratch, "plain")
    os.makedirs(plain)

    app = App(binary, repo=repo_a)
    apps.append(app)
    app.wait_bridge()
    opened(app, repo_a, "repo-a at launch")

    # 1) A subfolder: its repository.
    app.ok(f"file-open {deep}")
    opened(app, repo_b, "repo-b from src/app")

    # 2) A folder in no repository: said so, repo-b stays.
    app.ok(f"file-open {plain}")
    refused(app, plain, repo_b, "a plain folder")

    # 3) $HOME as a dotfiles repository: not the repository of a folder
    #    below it, but itself when asked for.
    home = app.env["HOME"]
    git("init", "-q", "-b", "main", cwd=home)
    below = os.path.join(home, "projects", "plain")
    os.makedirs(below)
    app.ok(f"file-open {below}")
    refused(app, below, repo_b, "a folder below a repository at $HOME")
    app.ok(f"file-open {home}/")
    state = app.wait_until(lambda s: same_dir(s.get("repoPath", ""), home),
                           "$HOME itself")
    if not any(t.startswith(os.path.basename(home)) for t in state["windows"]):
        raise Failure(f"$HOME open, but the title isn't its: {state['windows']}")

    # 4) A file: the repository it is in.
    app.ok(f"file-open {os.path.join(repo_b, 'a.txt')}")
    opened(app, repo_b, "repo-b from a.txt")

    # 4b) A minimized window comes back for it: the repository must
    #     not open unseen in the Dock.
    #     (Not repo-a: the launch folder asked for again within 2 s of
    #     launch is taken for AppKit sending it back, and dropped.)
    app.ok("minimize")
    app.wait_until(lambda s: s.get("minimized"), "the window minimized")
    app.ok(f"file-open {repo_c}")
    opened(app, repo_c, "repo-c into a minimized window")
    app.wait_until(lambda s: not s.get("minimized"), "the window restored")

    # 5) Two back to back, before either is opened: the second.
    app.batch(f"file-open {repo_c}", f"file-open {repo_d}")
    opened(app, repo_d, "repo-d after repo-c")
    time.sleep(1)
    opened(app, repo_d, "repo-d, still")

    # 6) Finder's service: on cocoa (a developer watching the run), the
    #    provider opens what it is sent; offscreen, and off macOS, there
    #    is none, and service-open says so.
    reply = app.cmd(f"service-open {repo_a}")
    if app.env.get("QT_QPA_PLATFORM") == "cocoa":
        if not reply.get("ok"):
            raise Failure(f"service-open on cocoa: {reply}")
        opened(app, repo_a, "repo-a from the service")
    elif reply.get("ok"):
        raise Failure(f"service-open worked off cocoa: {reply}")


run(test)
