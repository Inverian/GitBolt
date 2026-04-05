#!/usr/bin/env python3
"""Nautilus extension for GitBolt - adds 'Open in GitBolt' context menu item."""
import os
import subprocess
from gi.repository import Nautilus, GObject

class GitBoltExtension(GObject.GObject, Nautilus.MenuProvider):
    def get_file_items(self, *args):
        files = args[-1]  # Handle both Nautilus 3.x and 4.x API
        if len(files) != 1:
            return []

        file_info = files[0]
        if not file_info.is_directory():
            return []

        path = file_info.get_location().get_path()
        git_dir = os.path.join(path, '.git')
        if not os.path.exists(git_dir):
            return []

        item = Nautilus.MenuItem(
            name='GitBolt::open',
            label='Open in GitBolt',
            tip='Open this repository in GitBolt'
        )
        item.connect('activate', self._open_gitbolt, path)
        return [item]

    def _open_gitbolt(self, menu, path):
        subprocess.Popen(['gitbolt', path])
