# GitBolt

**Fast, cross-platform Git GUI client built with Qt 6, C++, and libgit2.**

GitBolt is a high-performance, feature-complete Git client for macOS, Linux, and Windows. Built natively in C++ with Qt 6 for maximum rendering performance and minimal resource usage.

> **License:** GitBolt is licensed under the **GNU General Public License v3.0**. See [LICENSE](LICENSE) for the full text. The "GitBolt" name and logo are protected trademarks — see [TRADEMARK.md](TRADEMARK.md) for the trademark policy. Forks are welcome under the GPL but must be rebranded.

---

## Why GitBolt?

Most desktop Git GUIs today are either Electron-based (dragging a full JavaScript runtime along for the ride) or expensive commercial native apps — and Linux options are especially thin. GitBolt fills the gap with a single codebase that delivers:

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
- Recent repositories with drag-and-drop, max-count + alphabetical-sort
  + path-shortening preferences
- Configurable default window size (with min-size enforcement) and a
  single global default size for every popup dialog; both with optional
  "restore previous" persistence per launch
- Per-dialog geometry persistence so each popup remembers its last
  drag-resized shape independently
- "Contained in branches" lookup on every commit detail (resolves via
  `git branch --all --contains` so it covers both local and remote
  tracking branches)
- File-status legend tooltip in the Commit dialog headers (hover the
  ⓘ icon to see what M / A / D / R / U / ? mean)
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

For full step-by-step instructions on installing dependencies and building GitBolt on **macOS**, **Windows**, and **Linux**, see **[docs/BUILDING.md](docs/BUILDING.md)**.

The short version, once your environment is set up:

```bash
git clone https://github.com/ThePieMonster/GitBolt.git
cd GitBolt
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build --parallel
```

Required tools at a glance:

| Dependency | Minimum version |
|---|---|
| C++ compiler | C++20 (Apple Clang 14+, GCC 11+, MSVC 19.30+) |
| CMake | 3.21 |
| Qt | 6.5 |
| libgit2 | 1.7 |

See [docs/BUILDING.md](docs/BUILDING.md) for how to install each of these on your platform, configure CMake, run the tests, build installers, and troubleshoot common issues.

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

**~28,900 lines of C++ across 185 source files.**

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
