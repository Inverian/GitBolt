#!/usr/bin/env python3
"""Nautilus extension for GitBolt - adds 'Open in GitBolt' context menu item."""
import os
import subprocess
from gi.repository import Nautilus, GObject


def in_repository(path):
    """Whether `path` is a repository's folder or inside one, found the
    way GitBolt finds it (git::Repository::openContaining): up the
    parent folders, never across a filesystem boundary, and never into
    $HOME or above it from a folder below it, where a dotfiles
    repository would claim every folder. $HOME, or a folder above it,
    counts only if it is a repository itself."""
    home = os.path.realpath(os.path.expanduser('~'))
    here = os.path.realpath(path)
    if os.path.exists(os.path.join(here, '.git')):
        return True
    if here == home or home.startswith(here.rstrip('/') + '/'):
        return False
    while not os.path.ismount(here):
        parent = os.path.dirname(here)
        if parent in (here, home):
            return False
        here = parent
        if os.path.exists(os.path.join(here, '.git')):
            return True
    return False


class GitBoltExtension(GObject.GObject, Nautilus.MenuProvider):
    def get_file_items(self, *args):
        files = args[-1]  # Handle both Nautilus 3.x and 4.x API
        if len(files) != 1:
            return []

        file_info = files[0]
        if not file_info.is_directory():
            return []

        # None for what is no local folder (a network location, Trash).
        path = file_info.get_location().get_path()
        if not path or not in_repository(path):
            return []

        item = Nautilus.MenuItem(
            name='GitBolt::open',
            label='Open in GitBolt',
            tip="Open this folder's repository in GitBolt"
        )
        item.connect('activate', self._open_gitbolt, path)
        return [item]

    def _open_gitbolt(self, menu, path):
        subprocess.Popen(['gitbolt', path])
