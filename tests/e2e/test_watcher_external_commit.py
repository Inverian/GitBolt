#!/usr/bin/env python3
"""E2E: an external `git commit` refreshes the open log automatically.

The file watcher watches .git internals (HEAD, refs) precisely so
that commits made outside GitBolt — terminal, scripts, another tool —
show up without a manual refresh. logRows moving 1 -> 2 is the
observable outcome.
"""

from __future__ import annotations

import os

from e2elib import App, Failure, git, make_repo, run


def test(binary: str, scratch: str, apps: list) -> None:
    repo = make_repo(os.path.join(scratch, "repo"))

    app = App(binary, repo)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("logRows") == 1, "initial log loaded")
    head_before = app.state()["headOid"]

    with open(os.path.join(repo, "external.txt"), "w") as fh:
        fh.write("written behind the app's back\n")
    git("add", "-A", cwd=repo)
    git("commit", "-q", "-m", "external commit", cwd=repo)

    state = app.wait_until(
        lambda s: s.get("logRows") == 2 and s.get("headOid") != head_before,
        "watcher-driven log refresh after external commit", timeout=30)
    if state.get("branch") != "main":
        raise Failure(f"branch drifted: {state.get('branch')!r}")

    rc = app.quit()
    if rc != 0:
        raise Failure(f"app exited rc={rc} after quit{app.tail()}")


if __name__ == "__main__":
    run(test)
