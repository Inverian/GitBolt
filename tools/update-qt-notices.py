#!/usr/bin/env python3
"""Write the notices of the code of others that is compiled into the Qt
libraries and plugins GitBolt ships, for one Qt version, from Qt's own
metadata: the qt_attribution.json files of qt/qtbase and qt/qtsvg at the
version's tag.

Usage:
  tools/update-qt-notices.py <qt-version> [--out PATH] [--check]

  <qt-version>  the Qt GitBolt ships, e.g. 6.11.3 (tag v6.11.3 is read)
  --out PATH    where to write; default
                resources/licenses/qt-<qt-version>-third-party.txt
  --check       write nothing; exit 1 if PATH isn't what would be written

Exit status: 0 written (or, with --check, up to date); 1 Qt's metadata
needs a review here (see "When it fails" below) or --check found a
difference; 2 something couldn't be fetched, or the arguments are wrong. Standard library only. Set
GITHUB_TOKEN to lift the GitHub API's rate limit (a run makes two API
calls; the raw files it fetches don't count against it).

What it writes, one file for every platform (over-inclusion is fine, an
omission is not): a header, then per component, in the order of SHIPPED
below: Name, Version, Used in, Copyright (verbatim from the attribution, or
its CopyrightFile), Licence (the SPDX expression), the upstream URLs, where
the code is in Qt's source, and each licence text verbatim from the
attribution's LicenseFile(s) at the tag. An attribution without a
LicenseFile gets qtbase's LICENSES/<id>.txt, the REUSE copy of each licence
its expression names. A licence that needs no text (CC0-1.0, public
domain: NO_TEXT) gets none. A text that recurs is printed once and referred
to after. tika-mimetypes also gets the attribution lines of Apache Tika's
NOTICE, at the commit Qt's attribution names (Apache-2.0 section 4(d)), and
the script adds Qt's EntryPoint, which windeployqt doesn't ship but CMake
links statically into GitBolt.exe, from its source file's header, and the
code Qt annotates only in a REUSE.toml (REUSE_SHIPPED).

The output depends only on this script and on files at a tag or commit, so
two runs give the same bytes. Line endings are LF; texts lose CRs, a BOM,
trailing blanks and leading and trailing blank lines, nothing else.

When it fails (exit 1), so a Qt bump forces a review here:
  - an Id in SHIPPED is no longer in any attribution file at the tag (it
    was renamed, moved to another repo or removed): find what replaced it;
  - an attribution Id in a directory that can go into a shipped binary
    (the rule is at NEVER_BUILT) is in neither SHIPPED nor NOT_SHIPPED:
    decide whether a library or plugin GitBolt ships compiles it, and add
    it to one of the two, with what it is used in or why it isn't shipped;
  - a licence file, the Tika NOTICE lines or the EntryPoint header is not
    where it was.
"""

from __future__ import annotations

import argparse
import json
import os
import posixpath
import re
import sys
import textwrap
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
USER_AGENT = "GitBolt-update-qt-notices (+https://github.com/Inverian/GitBolt)"
TREE_URL = "https://api.github.com/repos/qt/{repo}/git/trees/{tag}?recursive=1"
RAW_URL = "https://raw.githubusercontent.com/qt/{repo}/{tag}/{path}"
SOURCE_URL = "https://github.com/qt/{repo}/tree/{tag}/{path}"
TIKA_NOTICE_URL = "https://raw.githubusercontent.com/apache/tika/{commit}/NOTICE.txt"
REPOS = ("qtbase", "qtsvg")
ATTRIBUTION = "qt_attribution.json"
RULE = "=" * 80
SUBRULE = "-" * 80

# The directory rule. Every qt_attribution.json in qtbase and qtsvg is read
# and each of its Ids must be in SHIPPED or NOT_SHIPPED, unless the file is
# under one of these top-level directories: they build only Qt's tests and
# examples, never a library or plugin. Nothing narrower is assumed, not even
# "src/ only": util/ holds generators whose output Qt compiles in (the
# WebGradients from util/gradientgen are QGradient's presets), cmake/ and
# src/tools/ are cheap to classify, and a rule that let a new directory
# under src/3rdparty pass unseen would defeat the check. A module GitBolt
# doesn't ship (Wayland, Qt Test, Qt SQL) is classified Id by Id in
# NOT_SHIPPED rather than by directory, so each new Id there is looked at.
NEVER_BUILT = ("tests/", "examples/")

# Licences that ask for no text to go with binaries. An expression needs a
# text unless one of its OR alternatives consists of these only.
NO_TEXT = {"CC0-1.0", "LicenseRef-SHA1-Public-Domain"}


def _c(id_: str, used_in: str, repo: str = "qtbase", note: str | None = None) -> dict:
    return {"id": id_, "used_in": used_in, "repo": repo, "note": note}


# What GitBolt's packages ship of Qt 6.11.3 (aqtinstall's official
# binaries): QtCore, QtGui, QtWidgets, QtNetwork and QtSvg everywhere,
# QtConcurrent on macOS, QtDBus on macOS and Linux, QtXcbQpa on Linux; the
# cocoa, windows and xcb platform plugins; the gif, ico, jpeg and svg image
# formats, the svg icon engine, the TLS backends, networkinformation and
# the styles. These are the attribution Ids compiled into one of them on
# at least one platform, each with what it is used in, and the platforms
# when not all of them have it (from the attribution's QtUsage and the
# audit of the 0.9.9 packages: strings and symbols in each binary). The
# order is the output's.
_FTL = ("This software is based in part on the work of the FreeType Team "
        "(https://www.freetype.org), as the FTL asks. GitBolt takes FreeType's "
        "code under the FTL: FTL.TXT, in FreeType's licence text.")
SHIPPED = [
    # Qt Core
    _c("pcre2", "Qt Core (QRegularExpression)"),
    _c("pcre2-sljit", "Qt Core (QRegularExpression's JIT compiler); Windows, Linux, and "
       "the x86_64 slice of the macOS libraries"),
    _c("doubleconversion", "Qt Core (conversions between numbers and text)"),
    _c("tinycbor", "Qt Core (QCborStreamReader, QCborStreamWriter)"),
    _c("easing", "Qt Core (QEasingCurve)"),
    _c("rfc6234", "Qt Core (QCryptographicHash, QMessageAuthenticationCode)"),
    _c("sha3_endian", "Qt Core (QCryptographicHash)"),
    _c("qeventdispatcher_cf", "Qt Core (the Core Foundation event dispatcher); macOS"),
    _c("zlib", "Qt Core; Windows (on macOS and Linux Qt uses the system's zlib)"),
    _c("unicode-character-database", "Qt Core (character properties, QString, "
       "QTextBoundaryFinder)"),
    _c("unicode-cldr", "Qt Core (QLocale, QTimeZone, QCalendar)"),
    _c("tika-mimetypes", "Qt Core (QMimeDatabase's built-in MIME database)"),
    _c("forkfd", "Qt Core (QProcess); macOS, Linux"),
    _c("blake2", "Qt Core (QCryptographicHash)"),
    _c("md4", "Qt Core (QCryptographicHash)"),
    _c("md5", "Qt Core (QCryptographicHash)"),
    _c("sha1", "Qt Core (QCryptographicHash)"),
    _c("sha3_keccak", "Qt Core (QCryptographicHash)"),
    _c("siphash", "Qt Core (QHash)"),
    _c("tlexpected", "Qt Core and the other Qt libraries (a private header)"),
    # Qt GUI
    _c("harfbuzz-ng", "Qt GUI (text shaping)"),
    # FreeType before grayraster, which has the same licence file, so the
    # text is printed under FreeType.
    _c("freetype","Qt GUI (font rendering); macOS, Windows (on Linux Qt uses the "
       "system's FreeType)", note=_FTL),
    _c("freetype-bdf", "Qt GUI (FreeType's BDF driver); macOS, Windows"),
    _c("freetype-pcf", "Qt GUI (FreeType's PCF driver); macOS, Windows"),
    _c("freetype-zlib", "Qt GUI (FreeType's gzip module); macOS, Windows"),
    _c("grayraster", "Qt GUI (the anti-aliasing rasterizer, qgrayraster.c)", note=_FTL),
    _c("libpng", "Qt GUI (PNG images)"),
    _c("md4c", "Qt GUI (QTextDocument's Markdown reader)"),
    _c("emoji-segmenter", "Qt GUI (emoji sequences in text)"),
    _c("smooth-scaling-algorithm", "Qt GUI (smooth image scaling, qimagescale.cpp)"),
    _c("xserverhelper", "Qt GUI (QRegion, qregion.cpp)"),
    _c("aglfn", "Qt GUI (glyph names in the PDF writer)"),
    _c("webgradients", "Qt GUI (QGradient's presets)"),
    _c("icc-srgb-color-profile", "Qt GUI (the sRGB profile in PDF/A files)"),
    _c("opengl-headers", "Qt GUI (its OpenGL declarations); Windows, Linux"),
    _c("opengl-es2-headers", "Qt GUI (its OpenGL ES declarations); Windows, Linux"),
    _c("vulkan-xml-spec", "Qt GUI (QVulkanFunctions, generated from it); Windows, Linux"),
    _c("vulkanmemoryallocator", "Qt GUI (QRhi's Vulkan backend); Windows, Linux"),
    _c("d3d12memoryallocator", "Qt GUI (QRhi's Direct3D 12 backend); Windows"),
    _c("rhi-miniengine-d3d12-mipmap", "Qt GUI (QRhi's Direct3D 12 mipmap shader); Windows"),
    # Qt Network
    _c("psl-data", "Qt Network (the public suffix check of QNetworkCookieJar)"),
    _c("libpsl", "Qt Network (the lookup in the public suffix list)"),
    # Qt D-Bus
    _c("libdbus-1-headers", "Qt D-Bus; macOS, Linux"),
    # Qt SVG
    _c("xsvg", "Qt SVG (the SVG parser, qsvghandler.cpp)", repo="qtsvg"),
    # The platform plugins and the jpeg image-format plugin
    _c("xcb-xinput", "Qt XCB QPA (libQt6XcbQpa, for the xcb platform plugin); Linux"),
    _c("wintab", "the Windows platform plugin (qwindows.dll, tablet input); Windows"),
    _c("cocoa-platform-plugin", "the macOS platform plugin (libqcocoa.dylib); macOS"),
    _c("libjpeg", "the JPEG image-format plugin (qjpeg)",
       note="IJG licence condition (2): this software is based in part on the "
       "work of the Independent JPEG Group."),
]

_WAYLAND = ("Qt's Wayland client library, platform plugin or compositor: "
            "GitBolt ships the xcb platform plugin only")
_BUILD = "CMake code used only while Qt is built"
_TEST = "Qt Test, which GitBolt doesn't ship"

# The attribution Ids at the tag that no binary GitBolt ships compiles in,
# and why.
NOT_SHIPPED = {
    "extra-cmake-modules": _BUILD,
    "kwin": _BUILD,
    "cmake-runcmake-test-modules": _BUILD,
    "android-native-style": "the Android platform plugin",
    "android-gradle-wrapper": "Android packaging",
    "dejayvu": "the WebAssembly platform plugin's font",
    "pixman": "Qt GUI on 32-bit ARM with NEON only; no GitBolt binary has it",
    "sqlite": "Qt SQL's SQLite plugin, which GitBolt doesn't ship",
    "catch2": _TEST,
    "cycle": _TEST,
    "linuxperf": _TEST,
    "valgrind": _TEST,
    "appmenu": _WAYLAND,
    "wayland-color-management-protocol": _WAYLAND,
    "fractional-scale-v1": _WAYLAND,
    "wayland-fullscreen-protocol": _WAYLAND,
    "wayland-linux-dmabuf-unstable-v1": _WAYLAND,
    "wayland-pointer-gestures-protocol": _WAYLAND,
    "wayland-pointer-warp-protocol": _WAYLAND,
    "presentation-time.xml": _WAYLAND,
    "wayland-scaler-protocol": _WAYLAND,
    "wayland-session-management-protocol": _WAYLAND,
    "wayland-tablet-protocol": _WAYLAND,
    "wayland-text-input-unstable-v1": _WAYLAND,
    "wayland-text-input-unstable-v2": _WAYLAND,
    "wayland-text-input-unstable-v3": _WAYLAND,
    "wayland-viewporter-protocol": _WAYLAND,
    "wayland-protocol": _WAYLAND,
    "wayland-eglstream-controller": _WAYLAND,
    "wlr-data-control-unstable-v1-protocol": _WAYLAND,
    "wayland-primary-selection-protocol": _WAYLAND,
    "wayland-xdg-activation": _WAYLAND,
    "wayland-xdg-decoration-protocol": _WAYLAND,
    "xdg-dialog-v1": _WAYLAND,
    "xdg-foreign-unstable-v2": _WAYLAND,
    "wayland-xdg-output-protocol": _WAYLAND,
    "wayland-xdg-shell-protocol": _WAYLAND,
    "wayland-xdg-system-bell-protocol": _WAYLAND,
    "wayland-xdg-toplevel-icon-protocol": _WAYLAND,
}

# The component this script writes itself: windeployqt ships no
# Qt6EntryPoint, but Qt's CMake links its static library into every Windows
# GUI executable, so its code is in GitBolt.exe. Its licence is read from
# the source file's SPDX header, and must offer BSD-3-Clause.
ENTRYPOINT_SOURCE = "src/entrypoint/qtentrypoint_win.cpp"
ENTRYPOINT_LICENCE = "BSD-3-Clause"

# Code of others that Qt annotates only in a REUSE.toml, with no
# qt_attribution.json, but whose text is compiled in: the D-Bus interface
# descriptions (KDE's) that Linux's QtGui embeds as introspection XML, for
# the tray icon (StatusNotifierItem), the global menu (AppMenu) and
# notifications. Their copyright and licence are read from the folder's
# REUSE.toml, which must still say them.
REUSE_SHIPPED = [
    {"repo": "qtbase", "dir": "src/3rdparty/dbus-ifaces",
     "name": "D-Bus interface descriptions (StatusNotifierItem, AppMenu, Notifications)",
     "used_in": "QtGui, compiled in as D-Bus introspection XML; Linux"},
]


class Review(Exception):
    """Qt's metadata isn't what this script expects: exit 1."""


class FetchError(Exception):
    """A download failed: exit 2."""


def fetch(url: str) -> bytes:
    headers = {"User-Agent": USER_AGENT}
    if url.startswith("https://api.github.com/"):
        headers["Accept"] = "application/vnd.github+json"
        token = os.environ.get("GITHUB_TOKEN")
        if token:
            headers["Authorization"] = "Bearer " + token
    last = None
    for attempt in range(4):
        if attempt:
            time.sleep(2 ** attempt)
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=headers),
                                        timeout=60) as reply:
                return reply.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                raise FetchError(f"{url}: not found (404)") from None
            if e.code == 403 and e.headers.get("X-RateLimit-Remaining") == "0":
                raise FetchError(f"{url}: GitHub API rate limit reached; "
                                 "set GITHUB_TOKEN") from None
            last = f"HTTP {e.code}"
        except (urllib.error.URLError, OSError) as e:
            last = str(getattr(e, "reason", e))
    raise FetchError(f"{url}: {last}")


def as_text(data: bytes, where: str) -> str:
    """A fetched text, decoded, with LF line endings, no trailing blanks on
    a line and exactly one newline at the end."""
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError as e:
        raise Review(f"{where} is not UTF-8 ({e})") from None
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    lines = [line.rstrip() for line in text.split("\n")]
    while lines and not lines[-1]:
        lines.pop()
    while lines and not lines[0]:
        lines.pop(0)
    if not lines:
        raise Review(f"{where} is empty")
    return "\n".join(lines) + "\n"


def spdx_ids(expression: str) -> list[str]:
    ids = []
    for token in re.findall(r"[A-Za-z0-9.+:-]+", expression):
        if token not in ("AND", "OR", "WITH") and token not in ids:
            ids.append(token)
    return ids


def needs_text(expression: str) -> bool:
    if "(" in expression or ")" in expression:
        return True  # not parsed: err on the side of a text
    return not any(all(i in NO_TEXT for i in spdx_ids(alternative))
                   for alternative in re.split(r"\s+OR\s+", expression.strip()))


def lines_of(value, where: str) -> list[str]:
    """A Copyright value (a string, maybe with newlines, or a list of them)
    as lines."""
    if isinstance(value, str):
        value = [value]
    if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
        raise Review(f"{where}: Copyright is neither a string nor a list of strings")
    out = []
    for v in value:
        out.extend(line.rstrip() for line in v.strip().split("\n"))
    return out


def as_list(value, where: str, key: str) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    if isinstance(value, list) and all(isinstance(v, str) for v in value):
        return value
    raise Review(f"{where}: {key} is neither a string nor a list of strings")


def one_line(value, where: str, key: str) -> str:
    if value is None:
        return ""
    if not isinstance(value, str):
        raise Review(f"{where}: {key} is not a string")
    return " ".join(value.split())


class Qt:
    """The two repositories at one tag."""

    def __init__(self, version: str):
        self.version = version
        self.tag = "v" + version
        self.trees: dict[str, dict[str, str]] = {}
        self.tree_shas: dict[str, str] = {}
        self._raw: dict[tuple[str, str], str] = {}
        for repo in REPOS:
            data = json.loads(fetch(TREE_URL.format(repo=repo, tag=self.tag)))
            if data.get("truncated"):
                raise Review(f"GitHub truncated qt/{repo}'s tree at {self.tag}; "
                             "this script needs the whole tree")
            self.trees[repo] = {e["path"]: e["type"] for e in data["tree"]}
            self.tree_shas[repo] = data["sha"]

    def has(self, repo: str, path: str) -> bool:
        return self.trees[repo].get(path) == "blob"

    def text(self, repo: str, path: str) -> str:
        if not self.has(repo, path):
            raise Review(f"qt/{repo} has no file {path} at {self.tag}")
        key = (repo, path)
        if key not in self._raw:
            data = fetch(RAW_URL.format(repo=repo, tag=self.tag,
                                        path=urllib.parse.quote(path)))
            self._raw[key] = as_text(data, f"qt/{repo} {self.tag}:{path}")
        return self._raw[key]

    def attributions(self):
        """(repo, path of the json, entry) for every attribution outside
        NEVER_BUILT."""
        for repo in REPOS:
            for path in sorted(self.trees[repo]):
                if posixpath.basename(path) != ATTRIBUTION or not self.has(repo, path):
                    continue
                if path.startswith(NEVER_BUILT):
                    continue
                where = f"qt/{repo} {self.tag}:{path}"
                try:
                    data = json.loads(self.text(repo, path), strict=False)
                except ValueError as e:
                    raise Review(f"{where} doesn't parse as JSON: {e}") from None
                for entry in data if isinstance(data, list) else [data]:
                    if not isinstance(entry, dict) or not isinstance(entry.get("Id"), str):
                        raise Review(f"{where} has an entry without an Id")
                    yield repo, path, entry


def classify(qt: Qt) -> dict[str, tuple[str, str, dict]]:
    found: dict[str, tuple[str, str, dict]] = {}
    problems = []
    for repo, path, entry in qt.attributions():
        id_ = entry["Id"]
        if id_ in found:
            problems.append(f"Id {id_} is in both qt/{found[id_][0]} {found[id_][1]} "
                            f"and qt/{repo} {path}")
        found[id_] = (repo, path, entry)
    shipped = {c["id"]: c for c in SHIPPED}
    if len(shipped) != len(SHIPPED):
        problems.append("SHIPPED lists an Id twice")
    for id_ in sorted(set(shipped) & set(NOT_SHIPPED)):
        problems.append(f"{id_} is in both SHIPPED and NOT_SHIPPED")
    for c in SHIPPED:
        if c["id"] not in found:
            problems.append(f"{c['id']} (SHIPPED) is in no {ATTRIBUTION} of "
                            f"qt/{c['repo']} at {qt.tag}: renamed, moved or removed?")
        elif found[c["id"]][0] != c["repo"]:
            problems.append(f"{c['id']} (SHIPPED, qt/{c['repo']}) is in "
                            f"qt/{found[c['id']][0]} {found[c['id']][1]}")
    for id_, (repo, path, entry) in sorted(found.items()):
        if id_ not in shipped and id_ not in NOT_SHIPPED:
            usage = one_line(entry.get("QtUsage"), path, "QtUsage")
            problems.append(f"{id_} (qt/{repo} {path}) is in neither SHIPPED nor "
                            f"NOT_SHIPPED; Qt says: {usage or 'nothing'}")
    for id_ in sorted(set(NOT_SHIPPED) - set(found)):
        print(f"note: {id_} (NOT_SHIPPED) is not in qt/qtbase or qt/qtsvg at "
              f"{qt.tag}; drop it once that is for good", file=sys.stderr)
    if problems:
        raise Review("Qt's attributions need a review in tools/update-qt-notices.py:\n"
                     + "\n".join("  - " + p for p in problems))
    return found


def field(label: str, value: str) -> list[str]:
    """The label, padded to 11 columns, and the value, wrapped at 80 columns
    with its lines under its first (a URL or other long word stays whole)."""
    return textwrap.wrap(value, width=80, initial_indent=f"{label + ':':<11}",
                         subsequent_indent=" " * 11, break_long_words=False,
                         break_on_hyphens=False)


def paragraph(text: str) -> list[str]:
    return textwrap.wrap(text, width=80, break_long_words=False, break_on_hyphens=False)


def upstream_lines(entry: dict, version: str, where: str) -> list[str]:
    out = []
    homepage = one_line(entry.get("Homepage"), where, "Homepage")
    download = one_line(entry.get("DownloadLocation"), where, "DownloadLocation")
    if homepage:
        out += field("Homepage", homepage)
    if download:
        out += field("Download", download.replace("$<VERSION>", version))
    if not out:
        # Nothing but a package URL: name what it points at.
        for purl in as_list(entry.get("PURL"), where, "PURL"):
            m = re.match(r"pkg:github/([^/@?#]+)/([^/@?#]+)", purl)
            g = re.search(r"[?&]download_url=([^&#]+)", purl)
            if m:
                out += field("Upstream", f"https://github.com/{m.group(1)}/{m.group(2)}")
            elif g:
                out += field("Upstream", g.group(1).replace("$<VERSION>", version))
            if out:
                break
    return out


def tika_notice(entry: dict, where: str) -> tuple[str, list[str]]:
    """The attribution lines of Apache Tika's NOTICE that concern its MIME
    types: the ASF's own and the PRONOM credit."""
    commit = one_line(entry.get("Version"), where, "Version")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise Review(f"{where}: tika-mimetypes' Version {commit!r} is not a commit; "
                     "find the Tika NOTICE.txt of the version Qt took")
    url = TIKA_NOTICE_URL.format(commit=commit)
    text = as_text(fetch(url), url)
    # The attribution notices come first, before a line of dashes and the
    # licences of Tika's other dependencies.
    head = re.split(r"\n-{3,}\n", text, maxsplit=1)[0]
    paragraphs = [p.strip("\n") for p in re.split(r"\n\s*\n", head) if p.strip()]
    wanted = [
        ("its first lines", lambda p: p.startswith("Apache Tika\n")),
        ("the ASF credit", lambda p: "This product includes software developed at" in p
         and "The Apache Software Foundation" in p),
        ("the PRONOM credit", lambda p: "PRONOM" in p and "Open Government Licen" in p),
    ]
    out = []
    for what, test in wanted:
        hits = [p for p in paragraphs if test(p)]
        if len(hits) != 1:
            raise Review(f"{url}: can't find {what} (found {len(hits)} paragraphs)")
        if hits[0] not in out:
            out.append(hits[0])
    return url, out


def section(title: str) -> list[str]:
    return [RULE, title, RULE, ""]


def subsection(title: str) -> list[str]:
    return [SUBRULE, title, SUBRULE, ""]


class Writer:
    def __init__(self, qt: Qt):
        self.qt = qt
        self.lines: list[str] = []
        self.printed: dict[str, str] = {}  # licence text -> where it first was
        self.names: list[str] = []

    def licence_text(self, repo: str, path: str, name: str):
        text = self.qt.text(repo, path)
        label = f"{repo}/{path}"
        if text in self.printed:
            self.lines += subsection(f"Licence text: {label}")
            self.lines += paragraph(f"The same text as {self.printed[text]}, above.") + [""]
            return
        self.printed[text] = f"{label} under {name}"
        self.lines += subsection(f"Licence text: {label}")
        self.lines += text.rstrip("\n").split("\n") + [""]

    def component(self, c: dict, repo: str, path: str, entry: dict):
        where = f"qt/{repo} {path} ({c['id']})"
        name = one_line(entry.get("Name"), where, "Name")
        version = one_line(entry.get("Version"), where, "Version")
        licence = one_line(entry.get("LicenseId"), where, "LicenseId")
        if not name or not licence:
            raise Review(f"{where} has no Name or no LicenseId")
        directory = posixpath.dirname(path)
        if entry.get("Copyright") is not None:
            copyright_ = lines_of(entry["Copyright"], where)
        elif entry.get("CopyrightFile"):
            copyright_file = posixpath.normpath(posixpath.join(
                directory, one_line(entry["CopyrightFile"], where, "CopyrightFile")))
            copyright_ = self.qt.text(repo, copyright_file).rstrip("\n").split("\n")
        else:
            raise Review(f"{where} has neither Copyright nor CopyrightFile")

        source = directory
        if isinstance(entry.get("Path"), str):
            source = posixpath.normpath(posixpath.join(directory, entry["Path"]))
        self.names.append(name)
        self.lines += section(name)
        self.lines += field("Name", name)
        self.lines += field("Version", version or "none given by Qt")
        self.lines += field("Used in", c["used_in"])
        self.lines += ["Copyright:"] + ["  " + line if line else "" for line in copyright_]
        self.lines += field("Licence", licence)
        self.lines += upstream_lines(entry, version, where)
        self.lines += field("Qt source", SOURCE_URL.format(repo=repo, tag=self.qt.tag,
                                                           path=source))
        if c["note"]:
            self.lines += field("Note", c["note"])
        self.lines += [""]

        if c["id"] == "tika-mimetypes":
            url, paragraphs = tika_notice(entry, where)
            self.lines += subsection("Apache Tika's NOTICE: its attribution notices for "
                                     "tika-mimetypes.xml")
            self.lines += field("From", url) + [""]
            for p in paragraphs:
                self.lines += p.split("\n") + [""]

        if not needs_text(licence):
            self.lines += paragraph(f"{licence} asks for no licence text.") + [""]
            return
        files = as_list(entry.get("LicenseFiles"), where, "LicenseFiles") \
            + as_list(entry.get("LicenseFile"), where, "LicenseFile")
        if files:
            paths = [posixpath.normpath(posixpath.join(directory, f)) for f in files]
        else:
            # No file of its own: qtbase's REUSE copy of each licence named.
            paths = [f"LICENSES/{i}.txt" for i in spdx_ids(licence) if i not in NO_TEXT]
            repo = "qtbase"
        for p in paths:
            self.licence_text(repo, p, name)

    def entrypoint(self):
        source = self.qt.text("qtbase", ENTRYPOINT_SOURCE)
        header = []
        for line in source.split("\n"):
            if not line.startswith("//"):
                break
            header.append(line[2:].strip())
        copyright_ = [h for h in header if re.match(r"Copyright \(C\) ", h, re.I)]
        spdx = [h.split(":", 1)[1].strip() for h in header
                if h.startswith("SPDX-License-Identifier:")]
        where = f"qt/qtbase {self.qt.tag}:{ENTRYPOINT_SOURCE}"
        if not copyright_ or len(spdx) != 1:
            raise Review(f"{where}: no Copyright or SPDX-License-Identifier line in "
                         "its leading // comment")
        if ENTRYPOINT_LICENCE not in re.split(r"\s+OR\s+", spdx[0]):
            raise Review(f"{where}: its licence is now {spdx[0]!r}, which doesn't offer "
                         f"{ENTRYPOINT_LICENCE}")
        name = "Qt EntryPoint (Windows), statically linked into GitBolt.exe"
        self.names.append(name)
        self.lines += section(name)
        self.lines += field("Name", name)
        self.lines += field("Version", self.qt.version)
        self.lines += field("Used in", "GitBolt.exe (Qt6EntryPoint, its WinMain); Windows")
        self.lines += ["Copyright:"] + ["  " + line for line in copyright_]
        self.lines += field("Licence", ENTRYPOINT_LICENCE)
        self.lines += field("Homepage", "https://www.qt.io")
        self.lines += field("Qt source", SOURCE_URL.format(
            repo="qtbase", tag=self.qt.tag, path=posixpath.dirname(ENTRYPOINT_SOURCE)))
        self.lines += field("Note", f"{ENTRYPOINT_SOURCE} is under {spdx[0]}; GitBolt "
                            f"takes it under {ENTRYPOINT_LICENCE}.")
        self.lines += [""]
        self.licence_text("qtbase", f"LICENSES/{ENTRYPOINT_LICENCE}.txt", name)

    def reuse_component(self, c: dict):
        path = posixpath.join(c["dir"], "REUSE.toml")
        where = f"qt/{c['repo']} {self.qt.tag}:{path}"
        toml = self.qt.text(c["repo"], path)
        copyright_ = re.findall(r'^SPDX-FileCopyrightText\s*=\s*"([^"]+)"', toml, re.M)
        spdx = re.findall(r'^SPDX-License-Identifier\s*=\s*"([^"]+)"', toml, re.M)
        if len(copyright_) != 1 or len(spdx) != 1:
            raise Review(f"{where}: not one SPDX-FileCopyrightText and one "
                         "SPDX-License-Identifier; read it again")
        name = c["name"]
        self.names.append(name)
        self.lines += section(name)
        self.lines += field("Name", name)
        self.lines += field("Used in", c["used_in"])
        self.lines += ["Copyright:", "  " + copyright_[0]]
        self.lines += field("Licence", spdx[0])
        self.lines += field("Qt source", SOURCE_URL.format(
            repo=c["repo"], tag=self.qt.tag, path=c["dir"]))
        self.lines += field("Note", f"Qt has no qt_attribution.json for it; this is "
                            f"from {path}.")
        self.lines += [""]
        for i in spdx_ids(spdx[0]):
            if i not in NO_TEXT:
                self.licence_text("qtbase", f"LICENSES/{i}.txt", name)

    def document(self) -> str:
        v, tag = self.qt.version, self.qt.tag
        head = section(f"Qt {v}: the code of others in the Qt libraries and plugins "
                       "GitBolt ships")
        head += [
            f"Generated by tools/update-qt-notices.py from Qt {v}'s {ATTRIBUTION}",
            f"files, at tag {tag} of:",
        ]
        head += [f"  {'qt/' + r + ':':<11}tree {self.qt.tree_shas[r]}" for r in REPOS]
        head += [
            "Do not edit it by hand; regenerate it with",
            f"  python3 tools/update-qt-notices.py {v}",
            "",
            "Qt is under the GNU Lesser General Public License v3.0. Its libraries and",
            "plugins hold the code of others below, each under its own licence, in one",
            "list for all of GitBolt's packages. \"Used in\" names the Qt library or",
            "plugin the code is in, and the platforms when not all of them have it.",
            "Licence texts are verbatim from Qt's source at the tag (line endings made",
            "LF; trailing blanks and leading and trailing blank lines dropped); a text",
            "that recurs is printed once.",
            "",
            "Components:",
        ]
        head += ["  " + n for n in self.names] + [""]
        return "\n".join(head + self.lines).rstrip("\n") + "\n"


def generate(version: str) -> str:
    qt = Qt(version)
    found = classify(qt)
    writer = Writer(qt)
    for c in SHIPPED:
        repo, path, entry = found[c["id"]]
        writer.component(c, repo, path, entry)
    for c in REUSE_SHIPPED:
        writer.reuse_component(c)
    writer.entrypoint()
    return writer.document()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Write the notices of the code of others in the Qt GitBolt ships.")
    parser.add_argument("version", help="Qt version, e.g. 6.11.3")
    parser.add_argument("--out", help="output file (default "
                        "resources/licenses/qt-<version>-third-party.txt)")
    parser.add_argument("--check", action="store_true",
                        help="write nothing; exit 1 if the output file is out of date")
    args = parser.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        parser.error(f"{args.version!r} is not a Qt version like 6.11.3")
    out = args.out or os.path.join(ROOT, "resources", "licenses",
                                   f"qt-{args.version}-third-party.txt")
    try:
        text = generate(args.version)
    except Review as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except FetchError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    data = text.encode("utf-8")
    if args.check:
        try:
            with open(out, "rb") as f:
                current = f.read()
        except FileNotFoundError:
            current = None
        if current != data:
            print(f"{out} is out of date; run tools/update-qt-notices.py {args.version}",
                  file=sys.stderr)
            return 1
        print(f"{out} is up to date")
        return 0
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    temp = out + ".tmp"
    with open(temp, "wb") as f:
        f.write(data)
    os.replace(temp, out)
    print(f"wrote {out}: {len(data)} bytes, {len(SHIPPED) + len(REUSE_SHIPPED) + 1} components")
    return 0


if __name__ == "__main__":
    sys.exit(main())
