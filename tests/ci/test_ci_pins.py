#!/usr/bin/env python3
"""What CI installs Qt with stays pinned, and tools/check-ci-pins.py can
follow the pins.

.github/qt-requirements.txt must hold every package with its sha256,
and ci.yml must install nothing from an index without --require-hashes.
And check-ci-pins.py, which the weekly dependency check runs, must find
every pin in the tree, read a pin on a path inside an action's
repository (actions/cache/restore), never offer an older major's
backport release as the newest (actions/cache published v5.1.0 after
v6.1.0) nor one on GitHub out less than two weeks, report the
advisories it hasn't seen, and never take a source that failed for all
clear.

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

# The source tree: the argument ctest passes, else this file's. Only a
# directory counts, so unittest's and pytest's arguments (-v, this file)
# don't stand in for it and skip every test.
ROOT = os.path.abspath(sys.argv.pop(1) if __name__ == "__main__" and len(sys.argv) > 1
                       and os.path.isdir(sys.argv[1]) else
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


def release(tag, draft=False, prerelease=False, published="2026-01-01"):
    return {"tag_name": tag, "draft": draft, "prerelease": prerelease,
            "published_at": published + "T12:00:00Z"}


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

    def test_no_install_qt_action(self):
        self.assertNotRegex(self.text, r"uses:\s*jurplel/install-qt-action")

    def test_exact_qt_version(self):
        self.assertRegex(self.text, r"\n      CI_QT_VERSION: '\d+\.\d+\.\d+'\n")


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
        m = self.pins.USES.search("uses: actions/cache/restore@v6\n")
        self.assertEqual(m.groups(), ("actions", "cache", "/restore", "v6"))
        m = self.pins.USES.search("uses: actions/setup-python@v7\n")
        self.assertEqual(m.groups(), ("actions", "setup-python", None, "v7"))

    def test_latest_tag_is_the_highest_version(self):
        releases = [release("v5.1.0"), release("v6.1.0"), release("v6.0.0"),
                    release("v7.0.0", prerelease=True), release("v9.0.0", draft=True)]
        with mock.patch.object(self.pins, "api", return_value=releases):
            self.assertEqual(self.pins.latest_tag("actions", "cache", "2026-10-10"), "v6.1.0")

    def test_latest_tag_of_dated_tags_is_the_latest_published(self):
        releases = [release("continuous"), release("1-alpha-20251107-1"),
                    release("1-alpha-20250729-1")]
        with mock.patch.object(self.pins, "api", return_value=releases):
            self.assertEqual(self.pins.latest_tag("linuxdeploy", "linuxdeploy", "2026-10-10"),
                             "1-alpha-20251107-1")

    def test_write_keeps_the_path(self):
        line = "uses: actions/cache/restore@v6"

        def api(path):
            self.assertEqual(path, "repos/actions/cache/releases?per_page=100")
            return [release("v5.2.0"), release("v7.0.0"), release("v6.2.0")]

        updated = {"ci.yml": f"      - {line}\n"}
        out = self.report()
        with mock.patch.object(self.pins, "api", side_effect=api), mock.patch("sys.stdout"):
            self.pins.check_uses(out, updated, "ci.yml", self.pins.USES.search(line))
        self.assertEqual(out.status(), 1)
        self.assertEqual(updated["ci.yml"], "      - uses: actions/cache/restore@v7\n")

    def test_a_release_counts_two_weeks_after_it_is_out(self):
        # The report's today is 2026-10-24: out on 2026-10-10 counts, on
        # 2026-10-11 not yet. Qt's and NSIS's count at once (no advisory
        # feed for either).
        out = self.report()
        line = "uses: actions/cache@v6"
        answer = [release("v7.0.0", published="2026-10-11"),
                  release("v6.2.0", published="2026-10-10")]
        pages = {
            self.pins.QT_RELEASES: b'<a href="6.11/">6.11/</a>',
            self.pins.QT_RELEASES + "6.11/":
                b'<td><a href="6.11.4/">6.11.4/</a></td><td>23-Oct-2026 07:24</td>\n'
                b'<td><a href="6.11.3/">6.11.3/</a></td><td>10-Oct-2026 07:24</td>\n',
            self.pins.QT_SUPPORT:
                b'{"result": {"releases": [{"name": "6.11", "eolFrom": "2027-03-31"}]}}',
            self.pins.NSIS_FEED.format(major="3"):
                b"<rss><channel>"
                b"<item><title>/NSIS 3/3.13/nsis-3.13.zip</title>"
                b"<pubDate>Fri, 23 Oct 2026 15:34:58 UT</pubDate></item>"
                b"</channel></rss>"}
        with mock.patch.object(self.pins, "api", return_value=answer), \
             mock.patch.object(self.pins, "http", side_effect=lambda url, *_: pages[url]), \
             mock.patch("sys.stdout") as stdout:
            self.pins.check_uses(out, {"ci.yml": line}, "ci.yml", self.pins.USES.search(line))
            self.pins.check_qt(out, "6.11.3", "ci.yml")
            self.pins.check_nsis(out, "3", "3.12")
        printed = "".join(c.args[0] for c in stdout.write.call_args_list)
        self.assertIn("ok       actions/cache v6 (v6.2.0)", printed)
        self.assertIn("behind   Qt 6.11.3 -> 6.11.4 ", printed)
        self.assertIn("behind   NSIS 3.12 -> 3.13:", printed)

    def test_finds_every_pin_in_the_tree(self):
        # What the weekly run checks is all there: the actions, the Qt
        # version CI installs, NSIS, libgit2, the locked Python packages
        # and aqtinstall's commit, and the review dates of the advisories.
        pins = self.pins.Pins(ROOT)
        self.assertEqual(pins.missing, [])
        self.assertIn(("actions", "cache", "/restore"),
                      {m.group(1, 2, 3) for _, m in pins.uses})
        self.assertIsNotNone(pins.qt)
        self.assertIsNotNone(pins.nsis)
        self.assertTrue(pins.libgit2)
        names = {name for name, *_ in pins.python}
        self.assertLessEqual({"aqtinstall", "py7zr", "certifi"}, names)
        # CI runs CPython: what only PyPy installs isn't checked.
        self.assertNotIn("brotlicffi", names)
        self.assertLessEqual({*self.pins.MUST_REVIEW, "miurahr/py7zr", "miurahr/aqtinstall"},
                             set(pins.reviewed))

    def test_every_git_advisory_review_date_is_required(self):
        # The git GitBolt runs is git's, Git for Windows' or Git LFS's:
        # each one's date must be there, as libgit2's must.
        files = {"cmake/Libgit2.cmake":
                     "# check-ci-pins: libgit2/libgit2 advisories reviewed 2026-10-24\n",
                 "src/git/GitVersion.cpp":
                     "// check-ci-pins: git/git advisories reviewed 2026-10-24\n"
                     "// check-ci-pins: git-for-windows/git advisories reviewed 2026-10-24\n"}
        with tempfile.TemporaryDirectory() as root:
            for rel, text in files.items():
                os.makedirs(os.path.join(root, os.path.dirname(rel)))
                with open(os.path.join(root, rel), "w", encoding="utf-8") as fh:
                    fh.write(text)
            missing = [m for m in self.pins.Pins(root).missing if "reviewed" in m]
        self.assertEqual(missing, ["src/git/GitVersion.cpp: no 'check-ci-pins: git-lfs/git-lfs "
                                   "advisories reviewed <date>' comment"])

    def test_an_action_off_a_major_tag_is_reported(self):
        # Only @vN is followed: a commit or a full tag would be skipped.
        workflow = ("      - uses: actions/checkout@v7\n"
                    "      - uses: ./.github/actions/local\n"
                    "        uses: actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97\n")
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, ".github", "workflows"))
            with open(os.path.join(root, ".github", "workflows", "ci.yml"), "w",
                      encoding="utf-8") as fh:
                fh.write(workflow)
            missing = [m for m in self.pins.Pins(root).missing if "uses:" in m]
        self.assertEqual(missing, [".github/workflows/ci.yml: uses: actions/setup-python@"
                                   "5fda3b95a4ea91299a34e894583c3862153e4b97 isn't on a "
                                   "major tag (@vN), so it isn't followed"])

    def test_advisories_since_the_review_date(self):
        def advisory(ghsa, published):
            return {"ghsa_id": ghsa, "published_at": published + "T00:00:00Z",
                    "summary": "x", "html_url": "https://example.invalid/" + ghsa,
                    "severity": "high", "cve_id": None, "withdrawn_at": None}

        answer = [advisory("GHSA-new", "2026-10-23"), advisory("GHSA-sameday", "2026-10-20"),
                  advisory("GHSA-old", "2026-10-01")]
        out = self.report()
        with mock.patch.object(self.pins, "api", return_value=answer), \
             mock.patch("sys.stdout") as stdout:
            self.pins.check_advisories(out, "git/git", "2026-10-20", "src/git/GitVersion.cpp")
        printed = "".join(c.args[0] for c in stdout.write.call_args_list)
        self.assertEqual(out.status(), 1)
        # A day old: an advisory doesn't wait as a release does.
        self.assertIn("GHSA-new", printed)
        # Published later on the day of the review: not read.
        self.assertIn("GHSA-sameday", printed)
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
