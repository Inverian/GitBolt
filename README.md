# GitBolt

**Fast, cross-platform Git GUI client built with Qt 6, C++, and libgit2.**

GitBolt is a high-performance Git client designed as a modern alternative to GitExtensions, bringing its complete feature set to macOS, Linux, and Windows. Built natively in C++ with Qt 6 for maximum rendering performance and minimal resource usage.

> **License:** GitBolt is licensed under the **GNU General Public License v3.0**. See [LICENSE](LICENSE) for the full text. The "GitBolt" name and logo are protected trademarks — see [TRADEMARK.md](TRADEMARK.md) for the trademark policy. Forks are welcome under the GPL but must be rebranded.

---

## Why GitBolt?

The macOS Git GUI ecosystem is dominated by Electron-based clients (GitKraken, GitHub Desktop) or expensive native apps (Tower, Fork). On Linux, options are even more limited. GitBolt fills the gap with a single codebase that delivers:

- **Native performance** — Qt 6 with libgit2, no Electron, no JavaScript runtime
- **Power user features** — Interactive rebase, blame drill-down, three-way merge, Git Flow
- **Lightweight** — Compiled binary, hardware-accelerated rendering
- **Cross-platform** — Identical experience on macOS, Linux, and Windows
- **Extensible** — Plugin system for custom workflows

---

## Features

### Core Git Operations
- Visual revision graph with lane-based topology rendering
- Side-by-side and unified diff viewer with syntax highlighting
- Chunk-level and line-level staging
- Three-way merge conflict resolution
- Interactive rebase with drag-and-drop reordering
- Cherry-pick with multi-commit selection
- Stash save/apply/pop/drop with diff preview
- Branch management (create, delete, rename, checkout, merge)
- Push, pull, fetch with credential helper integration

### Repository Inspection
- Commit log with paged loading (handles 100K+ commits smoothly)
- Blame view with drill-down to commits
- File history with rename tracking
- Reflog viewer with checkout and reset actions
- Search by message, author, date range, file content (pickaxe), or file path

### Repository Management
- Tag management (annotated and lightweight)
- Submodule operations (init, update, sync, deinit)
- Worktree management (add, remove, lock)
- Git Flow workflow integration
- Repository maintenance (gc, prune, fsck, repack) with disk stats
- Multi-repository session support

### User Experience
- Light, Dark, and System theme modes
- Customizable code font and tab size
- Configurable keyboard shortcuts
- Recent repositories with drag-and-drop
- Plugin system with built-in extensions:
  - Background fetch with new commit notifications
  - Repository statistics with charts

### Platform Integration
- macOS Finder Sync extension ("Open in GitBolt" context menu)
- Windows Explorer shell extension
- Linux Nautilus extension and `.desktop` file
- Cross-platform installers (DMG, NSIS, DEB, AppImage)

---

## Architecture

GitBolt uses a **12-module architecture** following Qt convention with strict single-responsibility:

```
src/
├── git/        Core git operations (libgit2 wrappers + CLI fallback)
├── conf/       Settings persistence and theme management
├── watcher/    File system monitoring
├── util/       Threading, caching, performance, crash handling
├── models/     Qt data models (QAbstractItemModel subclasses)
├── services/   Business logic orchestration (GitService, RepoManager)
├── editor/     Custom renderers (revision graph delegate, syntax highlighter)
├── widgets/    Reusable UI components (graph, diff, staging, blame, etc.)
├── ui/         Main windows and views (MainWindow, RepositoryView, Dashboard)
├── dialogs/    Modal dialogs (clone, settings, rebase, merge, etc.)
├── plugins/    Extension system with built-in plugins
└── app/        Application entry point and platform-specific integrations
```

### Module Dependency Graph

```
util  conf  watcher          ← Foundation (zero cross-deps)
  \    |    /
   git/                       ← Git operations
     |
   models/                    ← Qt data models
     |
   services/                  ← Business logic
   /     \
editor/  widgets/             ← Rendering & reusable components
     \   /
      ui/                     ← Main windows
      |
   dialogs/   plugins/        ← Modal UI & extensions
      \       /
       app/                   ← Entry point
```

Each module is a separate CMake `STATIC` library, enforcing clean dependency boundaries and enabling fast incremental builds.

### Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| **Qt 6 + C++20** | Maximum native performance, mature widget system, hardware-accelerated rendering |
| **libgit2 primary, git CLI fallback** | libgit2 for hot path (log, diff, blame, status); CLI for credentials, interactive rebase, git-flow |
| **Headers and sources together** | Qt convention; matches Gittyup, KeePassXC, FreeCAD |
| **PascalCase filenames** | Qt convention (`QMainWindow.h` style) |
| **Background threading via QtConcurrent** | All git ops run on worker threads with QFutureWatcher signals back to UI |
| **Paged commit log model** | `fetchMore()` with 256-row pages + sliding-window cache eviction |
| **Lane-based revision graph** | Greedy lane assignment with bezier merge curves, custom QStyledItemDelegate |

---

## Building from Source

### Prerequisites

- **CMake** 3.21 or newer
- **Qt 6.5** or newer (Widgets, Concurrent, Test modules)
- **libgit2** 1.7 or newer
- **C++20 compiler** (GCC 11+, Clang 14+, MSVC 19.30+)

### macOS

```bash
brew install cmake qt@6 libgit2
git clone https://github.com/ThePieMonster/GitBolt.git
cd GitBolt
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/src/app/GitBolt.app/Contents/MacOS/GitBolt
```

### Linux (Ubuntu/Debian)

```bash
sudo apt install cmake qt6-base-dev libgit2-dev pkg-config build-essential
git clone https://github.com/ThePieMonster/GitBolt.git
cd GitBolt
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/src/app/gitbolt
```

### Windows

```powershell
# Install Qt 6 from qt.io and libgit2 via vcpkg
vcpkg install libgit2
git clone https://github.com/ThePieMonster/GitBolt.git
cd GitBolt
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
.\build\src\app\Release\GitBolt.exe
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `GITBOLT_BUILD_TESTS` | `ON` | Build unit tests |
| `GITBOLT_BUILD_PLUGINS` | `ON` | Build plugin system and built-in plugins |
| `GITBOLT_SANITIZERS` | `OFF` | Enable AddressSanitizer + UndefinedBehaviorSanitizer |

Example:
```bash
cmake -B build -DGITBOLT_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
```

### Running Tests

```bash
cmake --build build --target gitbolt_tests
ctest --test-dir build --output-on-failure
```

### Creating Installers

```bash
cmake --build build --target package
# Outputs: GitBolt-0.1.0-Darwin-arm64.dmg (macOS)
#          GitBolt-0.1.0-Windows-AMD64.exe (NSIS installer)
#          GitBolt-0.1.0-Linux-x86_64.deb (Debian package)
```

---

## Project Status

GitBolt is currently in **early development**. The core architecture and all major modules are scaffolded:

| Phase | Status |
|-------|--------|
| Phase 0 — Project Scaffolding | Complete |
| Phase 1 — Core Git Layer (libgit2 wrappers) | Complete |
| Phase 2 — Commit History + Revision Graph | Complete |
| Phase 3 — Staging, Diffing, Committing | Complete |
| Phase 4 — Branch Management, Merge, Push/Pull | Complete |
| Phase 5 — Blame, File History, Search | Complete |
| Phase 6 — Interactive Rebase, Cherry-Pick, Stash | Complete |
| Phase 7 — Tags, Submodules, Worktrees, Reflog | Complete |
| Phase 8 — Settings, Themes, Dashboard | Complete |
| Phase 9 — Plugin System | Complete |
| Phase 10 — Shell Integration & Packaging | Complete |
| Phase 11 — Git Flow & Maintenance | Complete |
| Phase 12 — Performance & Hardening | Complete |

**~16,500 lines of C++ across 157 source files.**

---

## License

GitBolt is licensed under the **GNU General Public License v3.0** (GPL-3.0).

This means you are free to:

- **Use** GitBolt for any purpose, including commercial use
- **Study** the source code and learn from it
- **Modify** the source code to suit your needs
- **Distribute** your modifications, provided they are also licensed under the GPL-3.0
- **Contribute back** to the official GitBolt project

In exchange, the GPL requires that:

- Modified versions you distribute must remain under the GPL-3.0
- You must make the source code available with any binary distribution
- You must preserve copyright notices and license information
- You cannot impose additional restrictions on the recipients of your distribution

See [LICENSE](LICENSE) for the full GPL-3.0 text.

### Trademark

The name **"GitBolt"** and the GitBolt logo are **trademarks** of the GitBolt project, protected separately from the source code license. The GPL grants you rights to the source code, but not to the name or logo.

If you fork GitBolt under the GPL, you are required to **rebrand** your fork — choose a different name and use a different logo. This is the same approach used by Firefox, Visual Studio Code, Chromium, and Krita, and protects users from confusion about what is the official GitBolt project.

See [TRADEMARK.md](TRADEMARK.md) for the full trademark policy.

### Contributing

Contributions are welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines on reporting bugs, suggesting features, and submitting pull requests.
