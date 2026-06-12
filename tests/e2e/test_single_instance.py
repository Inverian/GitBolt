#!/usr/bin/env python3
"""E2E: a second launch forwards its repo path and exits 0.

This is how "Open in GitBolt" from a file manager reaches an already-
open window. The second process must exit cleanly (not show a modal
warning) and the first instance must switch to the forwarded repo.
"""

from __future__ import annotations

import os
import subprocess

from e2elib import App, Failure, make_repo, run


def test(binary: str, scratch: str, apps: list) -> None:
    repo_a = make_repo(os.path.join(scratch, "repo-a"))
    repo_b = make_repo(os.path.join(scratch, "repo-b"))

    first = App(binary, repo_a, label="first instance")
    apps.append(first)
    first.wait_bridge()
    first.wait_until(
        lambda s: s.get("repoPath", "").rstrip("/").endswith("repo-a"),
        "repo A open")

    # Same instance namespace => the guard collides and the new
    # process forwards instead of starting a window.
    env = first.env.copy()
    env["GITBOLT_TEST_BRIDGE"] = "gb-e2e-unused-second"
    second = subprocess.run(
        [binary, repo_b], env=env, capture_output=True, timeout=30)
    if second.returncode != 0:
        raise Failure("second instance should forward and exit 0, got "
                      f"rc={second.returncode}: {second.stdout[-400:]}")

    first.wait_until(
        lambda s: s.get("repoPath", "").rstrip("/").endswith("repo-b"),
        "first instance to switch to forwarded repo B")

    rc = first.quit()
    if rc != 0:
        raise Failure(f"app exited rc={rc} after quit{first.tail()}")


if __name__ == "__main__":
    run(test)
