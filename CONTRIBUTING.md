# Contributing to GitBolt

Thank you for your interest in contributing to GitBolt! We welcome contributions from the community and appreciate your help in making GitBolt better.

This document explains how to contribute code, report bugs, suggest features, and participate in the project.

---

## Code of Conduct

GitBolt is committed to providing a welcoming and inclusive environment for everyone. We expect all contributors to:

- Be respectful and considerate in all interactions
- Welcome newcomers and help them get started
- Focus on what is best for the project and community
- Show empathy toward other community members
- Accept constructive criticism gracefully

Harassment, discrimination, personal attacks, and other unprofessional behavior will not be tolerated.

---

## Ways to Contribute

There are many ways to contribute to GitBolt:

- **Report bugs** — Open an issue describing what went wrong
- **Suggest features** — Share your ideas for improvements
- **Improve documentation** — Fix typos, clarify confusing sections, add examples
- **Write code** — Fix bugs or implement new features
- **Review pull requests** — Help us review and test contributions from others
- **Test pre-releases** — Try beta builds and report issues
- **Create plugins** — Extend GitBolt's functionality via the plugin system
- **Translate** — Help localize GitBolt into your language (when translation infrastructure is in place)

---

## Reporting Bugs

Before opening a new issue, please:

1. **Search existing issues** to see if the bug has already been reported
2. **Update to the latest version** to confirm the bug still exists
3. **Try to reproduce** the bug with minimal steps

When reporting a bug, include:

- **GitBolt version** (Help → About)
- **Operating system and version** (e.g., macOS 14.5, Ubuntu 24.04, Windows 11)
- **Qt version** used to build (if you compiled from source)
- **Steps to reproduce** the bug
- **Expected behavior** vs **actual behavior**
- **Screenshots or screen recordings** if relevant
- **Stack trace or crash log** from `~/.gitbolt/crash.log` if the app crashed
- **Repository details** that may be relevant (size, structure, special configurations)

---

## Suggesting Features

Feature suggestions are welcome! Before opening a feature request, please:

1. **Search existing issues** to see if the feature has been suggested before
2. **Check the project roadmap** to see if it's already planned
3. **Consider whether the feature fits the project's scope** — GitBolt aims to be a power-user Git GUI, comparable to GitExtensions

When suggesting a feature, include:

- **Clear description** of the feature
- **Use case** — why is this feature needed? what problem does it solve?
- **Proposed implementation** if you have ideas (optional)
- **Alternatives considered** — have you considered other approaches?
- **Examples from other tools** — does any other Git client implement this well?

---

## Submitting Pull Requests

We welcome pull requests! To make the review process smooth:

### Before You Start

1. **Open an issue first** for any non-trivial change. This lets us discuss the approach before you invest significant time in implementation.
2. **Check the project structure** in the [README](README.md) to understand the 12-module architecture.
3. **Read the code style guidelines** below.

### Development Workflow

1. **Fork the repository** on GitHub
2. **Clone your fork** locally
3. **Create a new branch** from `main` with a descriptive name:
   ```bash
   git checkout -b fix-revision-graph-rendering
   git checkout -b feature-stash-search
   ```
4. **Make your changes** following the code style guidelines
5. **Build and test** your changes locally
6. **Commit** with clear, descriptive commit messages
7. **Push** to your fork
8. **Open a pull request** against the `main` branch of the upstream repository

### Pull Request Guidelines

- **Keep PRs focused** — One feature or fix per PR. Multiple unrelated changes should be split into separate PRs.
- **Write clear commit messages** explaining *why*, not just *what*
- **Update documentation** if your change affects user-facing behavior or APIs
- **Add or update tests** when fixing bugs or adding features
- **Ensure CI passes** — your PR will be tested on macOS, Linux, and Windows
- **Be responsive to review feedback** — we may ask for changes before merging

### What Happens After You Submit

1. A maintainer will review your PR, usually within a few days
2. We may request changes, ask questions, or suggest improvements
3. Once approved, a maintainer will merge your PR
4. Your contribution will be credited in the commit history and release notes

---

## Code Style

GitBolt follows consistent C++20 style across the codebase. Please match the existing style.

### General Rules

- **C++20** standard, no compiler extensions
- **PascalCase** for class names and filenames (`MainWindow.h`, `RevisionGraphDelegate.cpp`)
- **camelCase** for methods, variables, and member fields
- **Trailing underscore** for private member fields (`commits_`, `model_`)
- **UPPER_CASE** for constants and macros
- **Namespaces** match directory names (`gitbolt::git`, `gitbolt::ui`, `gitbolt::services`, etc.)

### Formatting

- We use **clang-format** with the configuration in [`.clang-format`](.clang-format)
- 4-space indentation, no tabs
- 120-character line limit
- Braces on the same line as function declarations and control flow

Run `clang-format -i path/to/file.cpp` to format a file.

### Headers

- Use `#pragma once` instead of include guards
- Include order: own header first, then project headers, then Qt, then standard library
- Forward-declare in headers when possible to reduce compilation time
- Keep headers self-contained — anyone should be able to include them without including other headers first

Example:

```cpp
#pragma once

#include "git/Repository.h"
#include "models/CommitLogModel.h"

#include <QObject>

#include <memory>
#include <vector>

namespace gitbolt::services {

class GitService : public QObject {
    Q_OBJECT
public:
    explicit GitService(QObject* parent = nullptr);
    // ...
private:
    std::unique_ptr<git::Repository> repo_;
};

} // namespace gitbolt::services
```

### Memory and Resource Management

- **Use RAII** for all resources, including libgit2 objects (wrap in `std::unique_ptr` with custom deleter)
- **Prefer smart pointers** over raw `new`/`delete`
- **Use `Result<T>`** for fallible operations (defined in `git/Error.h`)
- **No exceptions** in the core library — use `Result<T>` instead

### Threading

- **Never call libgit2 from the UI thread** — all git operations run on background threads via `AsyncRunner`
- **Never share libgit2 objects across threads** — each thread opens its own `Repository` handle
- **Use Qt signals** for cross-thread communication (queued connections by default)
- **Workers don't drive QObjects** — a worker emits signals or posts to the object's thread (`QMetaObject::invokeMethod(obj, fn, Qt::QueuedConnection)`); it never calls into an object's state or creates children for it. `GitService::refresh*()` make that hop themselves, and `AsyncRunner` re-posts (with a warning) a submission from another thread — see the threading rule at the top of `services/GitService.h`

### Testability — name your actions and key widgets

GitBolt ships an env-gated **test bridge** (`GITBOLT_TEST_BRIDGE=1`)
that lets automated tests and AI agents drive the app by triggering
actions and inspecting state, instead of clicking pixels. See
[docs/AGENT_TESTING.md](docs/AGENT_TESTING.md). For it to reach a new
feature:

- **Every `QAction` must be addressable.** Call `setObjectName()` on
  any new menu / toolbar / context-menu action, using the dotted
  `area.action` convention (`"commands.resolveConflicts"`,
  `"worktree.lockUnlock"`). Explicit names are required because they
  stay stable when the display text changes. `MainWindow` runs an
  `assignActionObjectNames()` safety net that auto-derives a name from
  the menu path for anything left unnamed — **treat that as a backstop,
  not a substitute**; a derived name breaks the moment someone reworks
  the menu wording.
- **Give the dialog / view / editor a name too** when a feature adds
  one the bridge needs to drive (`select-row` a table, `type` into an
  editor). `setObjectName()` on the widget; otherwise the bridge can
  only address it by `ClassName[:index]`, which is positional and
  fragile.
- **Verify it:** run the app with `GITBOLT_TEST_BRIDGE=1`, then
  `tools/bridge.py list-actions` — your new action must appear with an
  `objectName`, and `tools/bridge.py trigger <name>` must fire it.

---

## Building from Source

See the [Building from Source](README.md#building-from-source) section in the README for full instructions.

Quick reference:

**macOS:**
```bash
brew install cmake qt@6 libgit2
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

---

## License of Contributions

GitBolt is licensed under the **GNU General Public License v3.0** (see [LICENSE](LICENSE)).

By submitting a contribution to GitBolt, you agree that your contribution will be licensed under the GPL-3.0. You retain copyright of your contribution, but grant the GitBolt project and its users the rights provided by the GPL-3.0 license.

Please make sure that:

- Your contribution is your own original work, or you have the legal right to submit it
- You have the right to license your contribution under the GPL-3.0
- You understand that contributions to GitBolt become part of a GPL-licensed work
- You agree to follow the project's [trademark policy](TRADEMARK.md)

You do **not** need to sign a Contributor License Agreement (CLA). Submitting a pull request is sufficient agreement to the above terms.

---

## Trademark Policy

The name "GitBolt" and the GitBolt logo are trademarks of the project, separately from the source code license. Please review the [trademark policy](TRADEMARK.md) before using the name or logo in any context, particularly if you create a fork or derivative work.

In short: the GPL allows you to fork the code, but you must rebrand any fork you distribute. This protects users from confusion about what is the official GitBolt project.

---

## Getting Help

If you need help contributing:

- **Open a discussion** on GitHub for general questions
- **Comment on an issue** if you'd like to work on it but need guidance
- **Ask in your pull request** if you're unsure about an implementation detail

We're happy to help newcomers get started!

---

## Recognition

All contributors are recognized in the project's commit history and release notes. Thank you for helping make GitBolt better!
