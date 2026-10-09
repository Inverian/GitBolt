#!/usr/bin/env python3
"""Report (or apply) newer releases of the tools CI pins by hash.

.github/workflows/ci.yml downloads its packaging tools from tagged
GitHub releases, each checked against the sha256 it had when it was
pinned, and pins jurplel/install-qt-action to a commit. Nothing bumps
those for us, so this does the legwork: for each pin it finds the
newest tagged release (never a 'continuous' build, which is rebuilt in
place), and for a newer one downloads the asset and prints the new
tag and sha256.

Usage:
  tools/check-ci-pins.py           report only; exits 1 if any pin is behind
  tools/check-ci-pins.py --write   also rewrite ci.yml with the new pins

Review a --write like any other change: a new linuxdeploy can change
the AppImage, so run the workflow on the branch (workflow_dispatch
builds the installers) before merging.

Pins it doesn't follow, printed as reminders: the NSIS zip from
SourceForge and the ubuntu image digest the DEB is tested in.

Uses the GitHub API without a token (60 requests an hour is plenty);
set GITHUB_TOKEN to lift that limit.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import sys
import urllib.request

CI = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                  ".github", "workflows", "ci.yml")

# fetch https://github.com/<owner>/<repo>/releases/download/<tag>/<asset> \
#       <sha256>
FETCH = re.compile(
    r"fetch (https://github\.com/([^/\s]+)/([^/\s]+)/releases/download/"
    r"([^/\s]+)/([^/\s]+))\s*\\\s*\n\s*([0-9a-f]{64})")
# uses: <owner>/<repo>@<40-hex commit> # <tag>
USES = re.compile(r"uses: ([^/\s]+)/([^@\s]+)@([0-9a-f]{40}) # (\S+)")


def api(path: str):
    request = urllib.request.Request("https://api.github.com/" + path)
    request.add_header("Accept", "application/vnd.github+json")
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        request.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def latest_tag(owner: str, repo: str) -> str:
    """The newest release that is neither a draft, a pre-release nor a
    'continuous' build."""
    for release in api(f"repos/{owner}/{repo}/releases?per_page=30"):
        tag = release["tag_name"]
        if not (release["draft"] or release["prerelease"]
                or tag == "continuous"):
            return tag
    raise SystemExit(f"{owner}/{repo}: no tagged release")


def sha256_of(url: str) -> str:
    digest = hashlib.sha256()
    with urllib.request.urlopen(url, timeout=300) as response:
        for chunk in iter(lambda: response.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    write = "--write" in sys.argv[1:]
    with open(CI) as fh:
        text = fh.read()
    updated = text
    behind = 0

    for m in FETCH.finditer(text):
        url, owner, repo, tag, asset, sha = m.groups()
        newest = latest_tag(owner, repo)
        if newest == tag:
            print(f"ok      {owner}/{repo} {tag}")
            continue
        behind += 1
        new_url = url.replace(f"/download/{tag}/", f"/download/{newest}/")
        new_sha = sha256_of(new_url)
        print(f"behind  {owner}/{repo} {tag} -> {newest}\n"
              f"        {new_url}\n        {new_sha}")
        updated = updated.replace(m.group(0),
                                  m.group(0).replace(url, new_url)
                                            .replace(sha, new_sha))

    for m in USES.finditer(text):
        owner, repo, commit, tag = m.groups()
        newest = latest_tag(owner, repo)
        if newest == tag:
            print(f"ok      {owner}/{repo} {tag}")
            continue
        behind += 1
        new_commit = api(f"repos/{owner}/{repo}/commits/{newest}")["sha"]
        print(f"behind  {owner}/{repo} {tag} -> {newest}\n"
              f"        {new_commit}")
        updated = updated.replace(m.group(0),
                                  f"uses: {owner}/{repo}@{new_commit} # {newest}")

    print("check   NSIS (https://nsis.sourceforge.io/Download) and the "
          "ubuntu image digest by hand")

    if write and updated != text:
        with open(CI, "w") as fh:
            fh.write(updated)
        print(f"wrote   {os.path.relpath(CI)}")
    return 1 if behind else 0


if __name__ == "__main__":
    sys.exit(main())
