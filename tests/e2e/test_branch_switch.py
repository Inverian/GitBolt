#!/usr/bin/env python3
"""E2E: switching branches through both combo-box paths.

Drives the bridge's `select-item` against the two places a user picks
a branch from a list: the Commands > Checkout branch dialog (a
QInputDialog combo read on OK) and the toolbar branch switcher, which
checks out only on `activated` (a real user pick), never on a
programmatic setCurrentIndex. Also pins the error contract: an unknown
item fails loudly and lists what the combo actually holds.
"""

from __future__ import annotations

import os

from e2elib import App, Failure, git, make_repo, run


def test(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "work"))
    # Two extra branches so the checkout dialog's default (index 0,
    # alphabetical) is NOT the one we want: a bare OK would pick
    # "alpha", so landing on "feature" proves the selection took.
    git("branch", "alpha", cwd=work)
    git("checkout", "-q", "-b", "feature", cwd=work)
    with open(os.path.join(work, "f.txt"), "w") as fh:
        fh.write("feature\n")
    git("add", "f.txt", cwd=work)
    git("commit", "-q", "-m", "feature work", cwd=work)
    git("checkout", "-q", "main", cwd=work)

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("branch") == "main",
                   "repo open on main")

    # 1. Checkout dialog: pick "feature" from the dialog's combo.
    app.ok("trigger commands.checkout-branch")
    app.wait_until(lambda s: "Checkout Branch" in s.get("windows", []),
                   "checkout dialog visible")
    combos = app.cmd("list-widgets")["combos"]
    dialog_combo = next((c for c in combos if "feature" in c["items"]
                         and c.get("objectName") != "toolbar.branchCombo"),
                        None)
    if dialog_combo is None:
        raise Failure(f"checkout dialog combo not reported: {combos}")
    if dialog_combo["current"] == "feature":
        raise Failure("dialog preselected 'feature'; test can't prove "
                      "the selection took")
    sel = app.ok(f"select-item QComboBox:{dialog_combo['index']} feature")
    if sel["text"] != "feature":
        raise Failure(f"select-item picked {sel['text']!r}")
    app.ok("click OK")
    app.wait_until(lambda s: s.get("branch") == "feature",
                   "checkout of 'feature' via the dialog")
    if git("rev-parse", "--abbrev-ref", "HEAD", cwd=work) != "feature":
        raise Failure("dialog checkout didn't reach the repository")

    # 2. Toolbar switcher: only `activated` checks out, so this proves
    #    select-item emits it.
    app.wait_until(
        lambda s: any(c.get("objectName") == "toolbar.branchCombo"
                      and c["current"] == "feature"
                      for c in app.cmd("list-widgets")["combos"]),
        "toolbar switcher to show 'feature'")
    app.ok("select-item toolbar.branchCombo main")
    app.wait_until(lambda s: s.get("branch") == "main",
                   "checkout of 'main' via the toolbar switcher")
    if git("rev-parse", "--abbrev-ref", "HEAD", cwd=work) != "main":
        raise Failure("toolbar checkout didn't reach the repository")

    # 3. Unknown item: loud failure that names the real entries.
    bad = app.cmd("select-item toolbar.branchCombo no-such-branch")
    if bad.get("ok") or "have:" not in bad.get("error", "") \
            or "feature" not in bad["error"]:
        raise Failure(f"unknown item should fail listing entries: {bad}")
    if app.state().get("branch") != "main":
        raise Failure("a failed select-item must not switch branches")

    rc = app.quit()
    if rc != 0:
        raise Failure(f"app exited rc={rc} after quit{app.tail()}")


if __name__ == "__main__":
    run(test)
