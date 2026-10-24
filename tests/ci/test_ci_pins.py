#!/usr/bin/env python3
"""What CI installs Qt with stays pinned, and tools/check-ci-pins.py can
follow the pins.

.github/qt-requirements.txt must hold every package with its sha256,
ci.yml must install nothing from an index without --require-hashes,
and the actions that install Qt must be pinned to commits. And
check-ci-pins.py, which the weekly dependency check runs, must find
every pin in the tree, read a pin on a path inside an action's
repository (actions/cache/restore), never offer an older major's
backport release as the newest (actions/cache published v5.1.0 after
v6.1.0), report the advisories it hasn't seen, and never take a source
that failed for all clear.

Offline and stdlib only. Usage: test_ci_pins.py [<source dir>]
"""

import importlib.util
import os
import re
import sys
import tempfile
import unittest
from unittest import mock

# Loading tools/check-ci-pins.py must not leave a __pycache__ in the
# source tree.
sys.dont_write_bytecode = True

ROOT = os.path.abspath(sys.argv.pop(1) if len(sys.argv) > 1 else
                       os.path.join(os.path.dirname(__file__), "..", ".."))
CI = os.path.join(ROOT, ".github", "workflows", "ci.yml")
LOCK = os.path.join(ROOT, ".github", "qt-requirements.txt")
LOCK_IN = os.path.join(ROOT, ".github", "qt-requirements.in")


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def load_pins():
    spec = importlib.util.spec_from_file_location(
        "check_ci_pins", os.path.join(ROOT, "tools", "check-ci-pins.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def logical_lines(text):
    """Lines with backslash continuations joined, comments dropped."""
    joined = re.sub(r"\\\n", " ", text)
    return [line.strip() for line in joined.splitlines()
            if line.strip() and not line.strip().startswith("#")]


def release(tag, draft=False, prerelease=False):
    return {"tag_name": tag, "draft": draft, "prerelease": prerelease}


def setUpModule():
    # A source tree without .github (a tarball made with export-ignore)
    # has nothing to check.
    if not os.path.exists(CI):
        raise unittest.SkipTest("no .github/workflows/ci.yml")


class QtRequirements(unittest.TestCase):
    def test_every_package_is_pinned_with_hashes(self):
        entries = logical_lines(read(LOCK))
        self.assertTrue(entries)
        names = set()
        for entry in entries:
            m = re.fullmatch(
                r"([A-Za-z0-9._-]+)==([^\s;]+)(?: ; [^-]+?)?"
                r"((?:\s+--hash=sha256:[0-9a-f]{64})+)", entry)
            self.assertIsNotNone(m, f"not name==version with hashes: {entry}")
            names.add(m.group(1).lower())
        # Built from its commit, never from an index
        self.assertNotIn("aqtinstall", names)
        # aqtinstall's build requirements: CI builds it with no index
        self.assertLessEqual({"setuptools", "setuptools-scm"}, names)

    def test_security_floors(self):
        # Each a fix newer than the lock's cutoff (qt-requirements.in): a
        # later recompile must not roll one back.
        floors = {"py7zr": (1, 1, 4), "brotli": (1, 2, 1),
                  "certifi": (2026, 10, 19), "setuptools": (84, 1, 0)}
        lock = read(LOCK)
        for name, floor in floors.items():
            m = re.search(rf"^{name}==(\d+)\.(\d+)\.(\d+)[ ;]", lock, re.M)
            self.assertIsNotNone(m, f"{name} not in the lock")
            self.assertGreaterEqual(tuple(map(int, m.groups())), floor, name)

    def test_aqtinstall_is_one_commit(self):
        commits = re.findall(
            r"^aqtinstall @ git\+https://github\.com/miurahr/aqtinstall@([0-9a-f]{40})$",
            read(LOCK_IN), re.M)
        self.assertEqual(len(commits), 1)


class Workflow(unittest.TestCase):
    def setUp(self):
        self.text = read(CI)

    def test_no_unhashed_pip_install(self):
        installs = [line for line in logical_lines(self.text)
                    if re.search(r"\b(?:pip[\d.]*|pipx|uv)\b.*\binstall\b", line)]
        self.assertTrue(installs)
        for line in installs:
            hashed = "--require-hashes" in line
            offline = "--no-index" in line and "--no-deps" in line
            self.assertTrue(hashed or offline, f"unhashed install: {line}")

    def test_qt_actions_are_pinned_to_commits(self):
        uses = load_pins().USES
        for action in ("actions/setup-python", "actions/cache/restore",
                       "actions/cache/save"):
            lines = [line for line in self.text.splitlines()
                     if f"uses: {action}@" in line]
            self.assertTrue(lines, f"{action} not used")
            for line in lines:
                self.assertIsNotNone(uses.search(line), f"not pinned: {line}")

    def test_no_install_qt_action(self):
        self.assertNotRegex(self.text, r"uses:\s*jurplel/install-qt-action")

    def test_exact_qt_and_python_versions(self):
        self.assertRegex(self.text, r"\n      CI_QT_VERSION: '\d+\.\d+\.\d+'\n")
        self.assertRegex(self.text, r"\n          python-version: '\d+\.\d+\.\d+'\n")


class CheckCiPins(unittest.TestCase):
    def setUp(self):
        self.pins = load_pins()
        # Nothing here may reach the network: a call that isn't mocked
        # fails at once instead of flaking.
        patcher = mock.patch("urllib.request.urlopen",
                             side_effect=AssertionError("network in a test"))
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_uses_reads_a_path_inside_the_repository(self):
        commit = "a" * 40
        m = self.pins.USES.search(f"uses: actions/cache/restore@{commit} # v6.1.0")
        self.assertEqual(m.groups(),
                         ("actions", "cache", "/restore", commit, "v6.1.0"))
        m = self.pins.USES.search(f"uses: actions/setup-python@{commit} # v7.1.0")
        self.assertEqual(m.groups(),
                         ("actions", "setup-python", None, commit, "v7.1.0"))

    def test_latest_tag_is_the_highest_version(self):
        releases = [release("v5.1.0"), release("v6.1.0"), release("v6.0.0"),
                    release("v7.0.0", prerelease=True), release("v9.0.0", draft=True)]
        with mock.patch.object(self.pins, "api", return_value=releases):
            self.assertEqual(self.pins.latest_tag("actions", "cache"), "v6.1.0")

    def test_latest_tag_of_dated_tags_is_the_latest_published(self):
        releases = [release("continuous"), release("1-alpha-20251107-1"),
                    release("1-alpha-20250729-1")]
        with mock.patch.object(self.pins, "api", return_value=releases):
            self.assertEqual(self.pins.latest_tag("linuxdeploy", "linuxdeploy"),
                             "1-alpha-20251107-1")

    def test_write_keeps_the_path(self):
        old, new = "1" * 40, "2" * 40
        line = f"uses: actions/cache/restore@{old} # v6.1.0"

        def api(path):
            if path.endswith("/releases?per_page=100"):
                self.assertEqual(path, "repos/actions/cache/releases?per_page=100")
                return [release("v5.2.0"), release("v6.2.0"), release("v6.1.0")]
            self.assertEqual(path, "repos/actions/cache/commits/v6.2.0")
            return {"sha": new}

        updated = {"ci.yml": f"      - {line}\n"}
        out = self.report()
        with mock.patch.object(self.pins, "api", side_effect=api), mock.patch("sys.stdout"):
            self.pins.check_uses(out, updated, "ci.yml", self.pins.USES.search(line))
        self.assertEqual(out.status(), 1)
        self.assertEqual(updated["ci.yml"],
                         f"      - uses: actions/cache/restore@{new} # v6.2.0\n")

    def test_finds_every_pin_in_the_tree(self):
        # What the weekly run checks is all there: the Qt version CI
        # installs, NSIS, libgit2 and its review date, the locked Python
        # packages and aqtinstall's commit, and the git advisory table.
        pins = self.pins.Pins(ROOT)
        self.assertEqual(pins.missing, [])
        self.assertIsNotNone(pins.qt)
        self.assertIsNotNone(pins.nsis)
        self.assertTrue(pins.libgit2)
        self.assertIn("libgit2/libgit2", pins.reviewed)
        names = {name for name, *_ in pins.python}
        self.assertLessEqual({"aqtinstall", "py7zr", "certifi"}, names)
        # CI runs CPython: what only PyPy installs isn't checked.
        self.assertNotIn("brotlicffi", names)
        self.assertIsNotNone(pins.git)
        self.assertIn("GHSA-vwqx-4fm8-6qc9", pins.git[1])
        self.assertEqual(len(pins.python_runtime), 1)
        self.assertLessEqual({"miurahr/py7zr", "miurahr/aqtinstall"}, set(pins.reviewed))

    def test_git_advisories_since_the_table_was_checked(self):
        def advisory(ghsa, published):
            return {"ghsa_id": ghsa, "published_at": published + "T00:00:00Z",
                    "summary": "x", "html_url": "https://example.invalid/" + ghsa,
                    "severity": "high", "cve_id": None, "withdrawn_at": None}

        answer = [advisory("GHSA-new", "2026-10-30"), advisory("GHSA-known", "2026-10-30"),
                  advisory("GHSA-sameday", "2026-10-24"), advisory("GHSA-old", "2026-10-01")]
        out = self.report()
        with mock.patch.object(self.pins, "api", return_value=answer), \
             mock.patch("sys.stdout") as stdout:
            self.pins.check_advisories(out, "git/git", "2026-10-24", "table",
                                       frozenset({"GHSA-known"}))
        printed = "".join(c.args[0] for c in stdout.write.call_args_list)
        self.assertEqual(out.status(), 1)
        self.assertIn("GHSA-new", printed)
        # Published later on the day the table was checked: not read.
        self.assertIn("GHSA-sameday", printed)
        self.assertNotIn("GHSA-known", printed)
        self.assertNotIn("GHSA-old", printed)

    def test_python_package_advisory(self):
        def api(path):
            self.assertTrue(path.startswith("advisories?"), path)
            self.assertIn("affects=brotli%401.2.0", path)
            return [{"ghsa_id": "GHSA-9v8h-qqgm-4m35", "severity": "critical",
                     "cve_id": None, "html_url": "https://example.invalid",
                     "vulnerabilities": [{"first_patched_version": "1.2.1"}]}]

        out = self.report()
        with mock.patch.object(self.pins, "api", side_effect=api), mock.patch("sys.stdout"):
            self.pins.check_python(out, "brotli", "1.2.0", None, "lock")
        self.assertEqual(out.status(), 1)

    def test_a_source_that_fails_is_not_all_clear(self):
        def down(out):
            raise OSError("no route to host")

        out = self.report()
        with mock.patch("sys.stdout"):
            out.guard("somewhere", down)
        self.assertEqual(out.status(), 2)

    def test_an_accepted_finding_passes_until_its_date(self):
        out = self.pins.Report({"qt-eol-6.11": "2026-12-15"}, "2026-10-24", False)
        with mock.patch("sys.stdout"):
            out.add("eol", "Qt 6.11 has ended", "qt-eol-6.11")
        self.assertEqual(out.status(), 0)
        out = self.pins.Report({"qt-eol-6.11": "2026-12-15"}, "2026-12-16", False)
        with mock.patch("sys.stdout"):
            out.add("eol", "Qt 6.11 has ended", "qt-eol-6.11")
        self.assertEqual(out.status(), 1)

    def report(self):
        return self.pins.Report({}, "2026-10-24", False)

if __name__ == "__main__":
    unittest.main()
