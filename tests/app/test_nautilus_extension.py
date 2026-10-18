#!/usr/bin/env python3
"""The Nautilus extension (src/app/platform/linux/nautilus-gitbolt.py)
offers Open in GitBolt where GitBolt finds a repository: on a
repository's folder and any folder inside one, never on a folder that
only a dotfiles repository at $HOME is around. Nautilus itself is not
needed: gi.repository is stubbed for the import.
"""
import importlib.util
import os
import subprocess
import sys
import tempfile
import types

source = sys.argv[1]

gi = types.ModuleType("gi")
repository = types.ModuleType("gi.repository")
repository.Nautilus = types.SimpleNamespace(MenuProvider=type("MenuProvider", (), {}),
                                            MenuItem=None)
repository.GObject = types.SimpleNamespace(GObject=type("GObject", (), {}))
gi.repository = repository
sys.modules["gi"] = gi
sys.modules["gi.repository"] = repository

spec = importlib.util.spec_from_file_location("nautilus_gitbolt", source)
ext = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ext)

failures = []


def expect(path, want, what):
    got = ext.in_repository(path)
    if got != want:
        failures.append(f"{what}: in_repository({path}) is {got}, expected {want}")


def init(path):
    os.makedirs(path, exist_ok=True)
    subprocess.run(["git", "init", "-q", path], check=True)


with tempfile.TemporaryDirectory() as tmp:
    home = os.path.join(tmp, "home")
    repo = os.path.join(home, "code", "proj")
    init(repo)
    os.makedirs(os.path.join(repo, "src", "app"))
    plain = os.path.join(home, "Documents", "notes")
    os.makedirs(plain)
    os.environ["HOME"] = home

    expect(repo, True, "a repository's folder")
    expect(os.path.join(repo, "src", "app"), True, "a folder inside one")
    expect(plain, False, "a folder in none")

    init(home)   # dotfiles
    expect(plain, False, "a folder below a repository at $HOME")
    expect(home, True, "$HOME, a repository itself")
    expect(os.path.join(repo, "src"), True, "a folder inside a repository below $HOME")

    above = os.path.dirname(home)
    expect(above, False, "a folder above $HOME")

for line in failures:
    print("FAIL:", line)
print("PASS" if not failures else f"{len(failures)} failed")
sys.exit(1 if failures else 0)
