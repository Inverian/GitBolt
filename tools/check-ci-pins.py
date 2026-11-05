#!/usr/bin/env python3
"""Report (or apply) newer releases of what GitBolt's builds pin, and
security advisories published since each was last reviewed. What it
follows, the comments it reads and how to act on a report are in
tools/README.md.

Usage:
  tools/check-ci-pins.py            report; exits 1 if anything is
                                    behind or has an advisory, 2 if
                                    anything couldn't be checked
  tools/check-ci-pins.py --write    also rewrite the fetch and uses: pins
  tools/check-ci-pins.py --offline  only list the pins found (no network)

Set GITHUB_TOKEN to lift the GitHub API's rate limit.
"""

from __future__ import annotations

import datetime
import glob
import hashlib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
LIBGIT2 = "cmake/Libgit2.cmake"
GIT_VERSION = "src/git/GitVersion.cpp"
# The advisories that must have a reviewed date, and where it's kept: the
# git GitBolt runs comes from git, Git for Windows and Git LFS.
MUST_REVIEW = {"libgit2/libgit2": LIBGIT2, "git/git": GIT_VERSION,
               "git-for-windows/git": GIT_VERSION, "git-lfs/git-lfs": GIT_VERSION}
# A release on GitHub counts once it has been out this long, time for a
# bad upload to be noticed and pulled (the wait .github/qt-requirements.in
# sets for the lock); an advisory counts at once. Qt's and NSIS's count at
# once: they have no advisory feed here, so a release is how a fix shows.
COOLDOWN = datetime.timedelta(days=14)

QT_RELEASES = "https://download.qt.io/official_releases/qt/"
QT_SUPPORT = "https://endoflife.date/api/v1/products/qt/"
NSIS_FEED = "https://sourceforge.net/projects/nsis/rss?path=/NSIS%20{major}&limit=100"
USER_AGENT = "GitBolt-check-ci-pins (+https://github.com/Inverian/GitBolt)"

# fetch https://github.com/<owner>/<repo>/releases/download/<tag>/<asset> \
#       <sha256>
FETCH = re.compile(
    r"fetch (https://github\.com/([^/\s]+)/([^/\s]+)/releases/download/"
    r"([^/\s]+)/([^/\s]+))\s*\\\s*\n\s*([0-9a-f]{64})")
# uses: <owner>/<repo>[/<path>]@v<major>
# (actions/cache/restore is a path in the actions/cache repository)
USES = re.compile(r"uses: ([^/\s]+)/([^/@\s]+)(/[^@\s]*)?@(v\d+)(?![\w.])")
LIBGIT2_VERSION = re.compile(r"set\(GITBOLT_LIBGIT2_VERSION (\d+\.\d+\.\d+)\)")
QT_VERSION = re.compile(r"\bCI_QT_VERSION: '?(\d+\.\d+\.\d+)\b")
NSIS = re.compile(
    r"downloads\.sourceforge\.net/project/nsis/NSIS%20(\d+)/(\d+(?:\.\d+)+)/nsis-\2\.zip")
# name==version, or name @ URL, as pip's requirement files have them
REQUIREMENT = re.compile(
    r"([A-Za-z0-9][A-Za-z0-9._-]*)(?:\[[^\]]*\])?\s*(?:==\s*([^\s\\;]+)|@\s*([^\s\\;]+))")
GITHUB_COMMIT = re.compile(
    r"github\.com/([^/\s]+)/([^/\s@]+?)(?:\.git)?(?:/archive/|@)([0-9a-f]{40})\b")
REVIEWED = re.compile(r"check-ci-pins: (\S+/\S+) advisories reviewed (\d{4}-\d{2}-\d{2})\b")
ACCEPT = re.compile(r"check-ci-pins: accept (\S+) until (\d{4}-\d{2}-\d{2})\b")

# Statuses that fail the run, with their GitHub annotation level.
FAILING = {"behind": "warning", "eol": "error", "advisory": "error"}
NOTICE = {"error": "error", "newer": "notice", "accepted": "notice"}


def http(url: str, headers: dict | None = None, timeout: int = 60) -> bytes:
    """GET, tried three times: one hiccup shouldn't fail the weekly run,
    and a source that stays down raises, so it's reported as not
    checked instead of reading as all clear."""
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT,
                                                   **(headers or {})})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.read()
        except urllib.error.HTTPError as e:
            e.close()
            if attempt == 2 or (e.code < 500 and e.code != 429):
                raise
        except OSError:   # URLError, timeouts, resets
            if attempt == 2:
                raise
        time.sleep(10 * (attempt + 1))
    raise AssertionError("unreachable")


def api(path: str):
    """GitHub's REST API. GITHUB_TOKEN only lifts the rate limit: every
    call reads public data. Actions' token can be refused an endpoint
    its permissions don't name even on a public repository (403
    'Resource not accessible by integration'), so a 401 or 403 is tried
    again without it."""
    url = "https://api.github.com/" + path
    headers = {"Accept": "application/vnd.github+json",
               "X-GitHub-Api-Version": "2022-11-28"}
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        try:
            return json.loads(http(url, {**headers, "Authorization": "Bearer " + token}))
        except urllib.error.HTTPError as e:
            if e.code not in (401, 403):
                raise
    return json.loads(http(url, headers))


def version_key(tag: str) -> tuple:
    """The numbers in a tag, compared in turn: 'v1.9.7' (1, 9, 7),
    '1-alpha-20251107-1' (1, 20251107, 1). Release order is no guide: a
    project that still fixes an older major releases those too
    (actions/cache published v5.1.0 after v6.1.0, libgit2 1.8.x and
    1.9.x together)."""
    return tuple(int(n) for n in re.findall(r"\d+", tag))


def releases(owner: str, repo: str, cutoff: str) -> list:
    """(version_key, tag) of every release published by `cutoff` that is
    neither a draft, a pre-release nor a 'continuous' build, newest
    version first."""
    answer = api(f"repos/{owner}/{repo}/releases?per_page=100")
    if not isinstance(answer, list):
        raise ValueError(f"{owner}/{repo}: releases answered {type(answer).__name__}")
    found = sorted(((version_key(r["tag_name"]), r["tag_name"]) for r in answer
                    if not (r["draft"] or r["prerelease"] or r["tag_name"] == "continuous")
                    and (r.get("published_at") or "")[:10] <= cutoff),
                   reverse=True)
    if not found:
        raise ValueError(f"{owner}/{repo}: no tagged release published by {cutoff}")
    return found


def latest_tag(owner: str, repo: str, cutoff: str) -> str:
    return releases(owner, repo, cutoff)[0][1]


def sha256_of(url: str) -> str:
    return hashlib.sha256(http(url, timeout=300)).hexdigest()


class Report:
    """Prints one line per finding (and GitHub annotations under
    Actions) and keeps the exit status."""

    def __init__(self, accepted: dict, today: str, annotate: bool):
        self.accepted, self.today, self.annotate = accepted, today, annotate
        # The last day a release may have been published to count
        self.cutoff = (datetime.date.fromisoformat(today) - COOLDOWN).isoformat()
        self.used = set()
        self.failing = self.unchecked = 0

    def add(self, status: str, text: str, key: str | None = None) -> str:
        head, *rest = text.split("\n")
        if status in FAILING and key in self.accepted:
            self.used.add(key)
            until = self.accepted[key]
            if until >= self.today:
                status, head = "accepted", f"{head} (until {until})"
            else:
                rest.append(f"(accepted until {until}, which has passed)")
        print(f"{status:<9}{head}" + (f" [{key}]" if key else ""))
        for line in rest:
            print(" " * 9 + line)
        level = FAILING.get(status) or NOTICE.get(status)
        if level and self.annotate:
            message = f"{head} [{key}]" if key else head
            message = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
            print(f"::{level} title=check-ci-pins {status}::{message}")
        self.failing += status in FAILING
        self.unchecked += status == "error"
        return status

    def guard(self, what: str, check, *args) -> None:
        """Runs one check; whatever stops it (a source down, an answer
        of another shape) is reported, and the other checks still run."""
        try:
            check(self, *args)
        except Exception as e:   # a monitor reports; it doesn't crash
            self.add("error", f"{what}: not checked ({type(e).__name__}: {e})")

    def status(self) -> int:
        return 2 if self.unchecked else 1 if self.failing else 0


class Pins:
    """Every pin and marker under a source tree, and what's missing."""

    def __init__(self, root: str):
        self.root = root
        self.texts = {}
        self.missing = []
        self.workflows = self.find(".github/workflows/*.yml")
        # A hash-locked requirements file is one with --hash= options,
        # and the file it was compiled from names what isn't in it.
        self.locks = [rel for rel in self.find(".github/*.txt")
                      if "--hash=" in self.texts[rel]]
        self.sources = self.find(".github/*.in")
        libgit2 = self.read(LIBGIT2)
        self.read(GIT_VERSION)

        self.fetch = [(rel, m) for rel in self.workflows for m in self.matches(FETCH, rel)]
        # A line a workflow repeats is checked once
        self.uses = list({(rel, m.group(0)): (rel, m) for rel in self.workflows
                          for m in self.matches(USES, rel)}.values())
        # An action pinned any other way (a full tag, a commit) isn't followed
        for rel in self.workflows:
            for ref in re.findall(r"^[\s-]*uses: (\S+)", self.texts[rel], re.M):
                if not ref.startswith("./") and not USES.search(f"uses: {ref}"):
                    self.missing.append(f"{rel}: uses: {ref} isn't on a major tag (@vN), "
                                        "so it isn't followed")

        m = LIBGIT2_VERSION.search(libgit2)
        self.libgit2 = m and m.group(1)
        if not self.libgit2:
            self.missing.append(f"{LIBGIT2}: no set(GITBOLT_LIBGIT2_VERSION x.y.z)")

        qt = {m.group(1): rel for rel in self.workflows for m in self.matches(QT_VERSION, rel)}
        self.qt = next(iter(qt.items())) if len(qt) == 1 else None
        if not self.qt:
            self.missing.append(f"Qt version: found {', '.join(sorted(qt)) or 'none'} as "
                                "CI_QT_VERSION in .github/workflows, expected one")

        nsis = {m.groups(): rel for rel in self.workflows for m in self.matches(NSIS, rel)}
        self.nsis = next(iter(nsis.items())) if len(nsis) == 1 else None
        if not self.nsis:
            self.missing.append("NSIS: expected one downloads.sourceforge.net/project/nsis "
                                f"zip in .github/workflows, found {len(nsis)}")

        self.python = self.python_pins()
        if not self.python:
            self.missing.append("Python packages: no hash-locked requirements file under "
                                ".github/ (qt-requirements.txt)")

        self.reviewed = {}
        for rel, text in self.texts.items():
            for m in REVIEWED.finditer(text):
                # Two dates for one repository: the older, to miss nothing.
                repo, date = m.groups()
                if repo not in self.reviewed or date < self.reviewed[repo][0]:
                    self.reviewed[repo] = (date, rel)
        for repo, rel in MUST_REVIEW.items():
            if repo not in self.reviewed:
                self.missing.append(f"{rel}: no 'check-ci-pins: {repo} advisories "
                                    "reviewed <date>' comment")

        self.accepted = {}
        for text in self.texts.values():
            for key, until in ACCEPT.findall(text):
                self.accepted[key] = until

    def matches(self, pattern, rel: str):
        return pattern.finditer(self.texts[rel])

    def find(self, pattern: str) -> list:
        """The files a glob matches, read, as '/'-separated relative paths."""
        found = sorted(os.path.relpath(p, self.root).replace(os.sep, "/") for p in
                       glob.glob(os.path.join(glob.escape(self.root), *pattern.split("/"))))
        for rel in found:
            self.read(rel)
        return found

    def read(self, rel: str) -> str:
        if rel not in self.texts:
            try:
                with open(os.path.join(self.root, rel), encoding="utf-8") as fh:
                    self.texts[rel] = fh.read()
            except OSError as e:
                self.missing.append(f"{rel}: {e.strerror}")
                self.texts[rel] = ""
        return self.texts[rel]

    def python_pins(self) -> list:
        """(name, version or None, GitHub commit URL or None, where) per
        package: every name==version in a lock, less what only PyPy
        installs (CI runs CPython), and every name @ URL in the file it
        was compiled from (aqtinstall at a commit: the lock leaves it
        out)."""
        pins = []
        for rel in self.locks:
            for line in self.texts[rel].splitlines():
                m = REQUIREMENT.match(line)
                if m and m.group(2) and "platform_python_implementation == 'PyPy'" not in line:
                    pins.append((re.sub(r"[-_.]+", "-", m.group(1)).lower(), m.group(2),
                                 None, rel))
        for rel in self.sources:
            for line in self.texts[rel].splitlines():
                m = REQUIREMENT.match(line)
                if not (m and m.group(3)):
                    continue
                if GITHUB_COMMIT.search(m.group(3)):
                    pins.append((m.group(1).lower(), None, m.group(3), rel))
                else:
                    self.missing.append(f"{rel}: {m.group(1)} @ {m.group(3)}: only versions "
                                        "and GitHub commits are followed")
        return pins

    def describe(self) -> list:
        lines = [f"{owner}/{repo} {tag} ({rel})"
                 for rel, m in self.fetch for _, owner, repo, tag, _, _ in [m.groups()]]
        lines += [f"{m.group(1)}/{m.group(2)}{m.group(3) or ''} {m.group(4)} ({rel})"
                  for rel, m in self.uses]
        if self.libgit2:
            lines.append(f"libgit2 {self.libgit2} ({LIBGIT2})")
        if self.qt:
            lines.append(f"Qt {self.qt[0]} ({self.qt[1]})")
        if self.nsis:
            lines.append(f"NSIS {self.nsis[0][1]} ({self.nsis[1]})")
        lines += [f"{name} {version or url} ({rel})" for name, version, url, rel in self.python]
        lines += [f"{repo} advisories reviewed {date} ({where})"
                  for repo, (date, where) in sorted(self.reviewed.items())]
        lines += [f"accept {key} until {until}" for key, until in sorted(self.accepted.items())]
        return lines


# ---- checks: each takes the Report first ----------------------------------

def check_fetch(out: Report, updated: dict, rel: str, m) -> None:
    url, owner, repo, tag, asset, sha = m.groups()
    newest = latest_tag(owner, repo, out.cutoff)
    if version_key(newest) <= version_key(tag):
        out.add("ok", f"{owner}/{repo} {tag}")
        return
    new_url = url.replace(f"/download/{tag}/", f"/download/{newest}/")
    new_sha = sha256_of(new_url)
    if out.add("behind", f"{owner}/{repo} {tag} -> {newest}\n{new_url}\n{new_sha}",
               f"{repo}-{newest}") == "behind":
        updated[rel] = updated[rel].replace(
            m.group(0), m.group(0).replace(url, new_url).replace(sha, new_sha))


def check_uses(out: Report, updated: dict, rel: str, m) -> None:
    """A major tag takes its major's releases as they come; behind once
    a newer major is out."""
    owner, repo, path, tag = m.groups()
    action = f"{owner}/{repo}{path or ''}"
    newest = latest_tag(owner, repo, out.cutoff)
    major = f"v{version_key(newest)[0]}"
    if version_key(major) <= version_key(tag):
        out.add("ok", f"{action} {tag} ({newest})")
        return
    if out.add("behind", f"{action} {tag} -> {major} ({newest})",
               f"{repo}-{major}") == "behind":
        updated[rel] = updated[rel].replace(m.group(0), f"uses: {action}@{major}")


def check_libgit2(out: Report, version: str) -> None:
    """Behind when the pinned series has a newer release (libgit2's
    security fixes come as those); a newer series is news, not a
    failure: the advisories say when the pinned one is affected."""
    found = releases("libgit2", "libgit2", out.cutoff)
    pin = version_key(version)
    series = [tag for key, tag in found if key[:2] == pin[:2] and key > pin]
    if series:
        new = series[0].lstrip("v")
        url = f"https://github.com/libgit2/libgit2/archive/refs/tags/{series[0]}.tar.gz"
        out.add("behind", f"libgit2 {version} -> {new}: bump the version and SHA256 in "
                f"{LIBGIT2}; rebase cmake/PatchLibgit2.cmake if it no longer applies\n"
                f"{url}\n{sha256_of(url)}", f"libgit2-{new}")
    elif found[0][0] > pin:
        out.add("newer", f"libgit2 {version}, the newest {pin[0]}.{pin[1]}; "
                f"{found[0][1].lstrip('v')} is out")
    else:
        out.add("ok", f"libgit2 {version}")


def repository_advisories(repo: str) -> list:
    answer = api(f"repos/{repo}/security-advisories"
                 "?state=published&sort=published&direction=desc&per_page=100")
    if not isinstance(answer, list):
        raise ValueError(f"{repo}: advisories answered {type(answer).__name__}")
    return answer


def check_advisories(out: Report, repo: str, reviewed: str, where: str) -> None:
    """Every advisory of `repo` published on or after `reviewed` (one
    from later that day wasn't read)."""
    new = 0
    for a in repository_advisories(repo):
        published = (a.get("published_at") or "")[:10]
        # Newest first, but a page in another order mustn't stop it early.
        if published < reviewed or a.get("withdrawn_at"):
            continue
        new += 1
        about = ", ".join(x for x in (a.get("cve_id"), a.get("severity"), published) if x)
        out.add("advisory", f"{repo} {a['ghsa_id']} ({about}): {a['summary'].strip()}\n"
                f"{a['html_url']}\n"
                f"Read it against {where}, act on it, then move its date ({reviewed})",
                a["ghsa_id"])
    if not new:
        out.add("ok", f"{repo} advisories: none since {reviewed}")


def check_qt(out: Report, version: str, where: str) -> None:
    """Behind when download.qt.io lists a newer patch release of the
    pinned series; past its end when the series' open-source support
    has ended. Qt says that only in prose ('the final release of the
    6.x series'); endoflife.date's 'eolFrom', which it labels 'OSS
    support', is the machine-readable copy. A series that has left
    official_releases is past it whatever that says."""
    pin = version_key(version)
    series = f"{pin[0]}.{pin[1]}"
    offered = re.findall(r'href="(\d+\.\d+)/"', http(QT_RELEASES).decode("utf-8", "replace"))
    if not offered:
        raise ValueError(f"{QT_RELEASES} lists no Qt series")
    newest_series = max(offered, key=version_key)
    if series not in offered:
        out.add("eol", f"Qt {version}: {series} has left {QT_RELEASES} (archived), so it "
                f"gets no more open-source releases; newest series {newest_series}",
                f"qt-eol-{series}")
        return
    listing = http(f"{QT_RELEASES}{series}/").decode("utf-8", "replace")
    newest = max(re.findall(rf'href="({re.escape(series)}\.\d+)/"', listing),
                 key=version_key, default=version)
    failed = False
    if version_key(newest) > pin:
        failed = out.add("behind", f"Qt {version} -> {newest} ({where})",
                         f"qt-{newest}") != "accepted"

    data = json.loads(http(QT_SUPPORT))["result"]["releases"]
    if not data:
        raise ValueError(f"{QT_SUPPORT} lists no Qt release")
    entry = next((r for r in data if r.get("name") == series), None)
    end = entry and entry.get("eolFrom")
    if entry is None:
        out.add("check", f"Qt {series}: endoflife.date has no entry for it yet")
    elif end and end <= out.today:
        failed |= out.add("eol", f"Qt {series}'s open-source support ended {end} "
                          f"(endoflife.date); newest series {newest_series}",
                          f"qt-eol-{series}") != "accepted"
    if failed:
        return
    if version_key(newest_series) > pin[:2]:
        until = f"; {series} is supported until {end}" if end and end > out.today else ""
        out.add("newer", f"Qt {version}, the newest {series}; {newest_series} is out{until}")
    elif version_key(newest) <= pin:
        out.add("ok", f"Qt {version}")


def check_nsis(out: Report, major: str, version: str) -> None:
    feed = ET.fromstring(http(NSIS_FEED.format(major=major)))
    found = set()
    for title in feed.iter("title"):
        m = re.fullmatch(rf"/NSIS {major}/(\d+(?:\.\d+)+)/nsis-\1\.zip",
                         (title.text or "").strip())
        if m:
            found.add(m.group(1))
    if not found:
        raise ValueError("SourceForge's NSIS feed lists no release zip")
    newest = max(found, key=version_key)
    if version_key(newest) > version_key(version):
        url = (f"https://downloads.sourceforge.net/project/nsis/NSIS%20{major}/{newest}/"
               f"nsis-{newest}.zip")
        out.add("behind", f"NSIS {version} -> {newest}: in ci.yml bump the URL, the folder "
                f"and cache key (nsis-{newest}) and NSIS_SHA256 by hand\n{url}",
                f"nsis-{newest}")
    else:
        out.add("ok", f"NSIS {version}")


def check_python(out: Report, name: str, version: str | None, url: str | None,
                 where: str) -> None:
    if url:
        # A commit, for a fix not released yet: behind once a release
        # has it, so the pin can go back to a version.
        owner, repo, commit = GITHUB_COMMIT.search(url).groups()
        tag = latest_tag(owner, repo, out.cutoff)
        status = api(f"repos/{owner}/{repo}/compare/{commit}...{tag}")["status"]
        if status in ("ahead", "identical"):
            out.add("behind", f"{name} @ {commit[:12]}: {owner}/{repo} {tag} has it; pin "
                    f"{name}=={tag.lstrip('v')} instead ({where})",
                    f"{name}-{tag.lstrip('v')}")
        else:
            out.add("ok", f"{name} @ {commit[:12]} (not in {tag}, its newest release)")
        return
    # GitHub's advisory database, which had brotli's and certifi's
    # October advisories while OSV, and so PyPI, had neither.
    query = urllib.parse.urlencode({"ecosystem": "pip", "affects": f"{name}@{version}",
                                    "per_page": 100})
    answer = api(f"advisories?{query}")
    if not isinstance(answer, list):
        raise ValueError(f"{name}: advisories answered {type(answer).__name__}")
    findings = 0
    for a in answer:
        if a.get("withdrawn_at"):
            continue
        findings += 1
        fixed = ", ".join(v["first_patched_version"] for v in a.get("vulnerabilities", [])
                          if v.get("first_patched_version")) or "nothing yet"
        about = ", ".join(x for x in (a.get("cve_id"), a.get("severity")) if x)
        out.add("advisory", f"{name} {version}: {a['ghsa_id']} ({about}), fixed in {fixed} "
                f"({where})\n{a.get('html_url', '')}", a["ghsa_id"])
    if not findings:
        out.add("ok", f"{name} {version}")


def main(argv: list | None = None, root: str = ROOT, today: str | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    unknown = [a for a in args if a not in ("--write", "--offline")]
    if unknown:
        print(__doc__)
        return 2
    write, offline = "--write" in args, "--offline" in args
    today = today or datetime.datetime.now(datetime.timezone.utc).date().isoformat()

    pins = Pins(root)
    out = Report(pins.accepted, today, os.environ.get("GITHUB_ACTIONS") == "true")
    for problem in pins.missing:
        out.add("error", problem)
    if offline:
        for line in pins.describe():
            out.add("pin", line)
        return out.status()

    updated = {rel: pins.texts[rel] for rel in pins.workflows}
    for rel, m in pins.fetch:
        out.guard(f"{m.group(2)}/{m.group(3)}", check_fetch, updated, rel, m)
    for rel, m in pins.uses:
        out.guard(f"{m.group(1)}/{m.group(2)}", check_uses, updated, rel, m)
    if pins.libgit2:
        out.guard("libgit2", check_libgit2, pins.libgit2)
    if pins.qt:
        out.guard("Qt", check_qt, *pins.qt)
    if pins.nsis:
        out.guard("NSIS", check_nsis, *pins.nsis[0])
    for pin in pins.python:
        out.guard(pin[0], check_python, *pin)
    for repo, (date, where) in sorted(pins.reviewed.items()):
        out.guard(f"{repo} advisories", check_advisories, repo, date, where)

    for key, until in sorted(pins.accepted.items()):
        if key not in out.used and not out.unchecked:
            out.add("check", f"accept {key} until {until} matches nothing now: remove it")
    out.add("check", "the ubuntu and debian image digests in ci.yml by hand")

    if write:
        for rel, text in updated.items():
            if text != pins.texts[rel]:
                with open(os.path.join(root, rel), "w", encoding="utf-8") as fh:
                    fh.write(text)
                print(f"wrote    {rel}")
    return out.status()


if __name__ == "__main__":
    sys.exit(main())
