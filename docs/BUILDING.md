# Building GitBolt

This guide walks you through setting up a development environment and building GitBolt from source on **macOS** and **Windows**. Linux instructions are similar to macOS — see the [Linux notes](#linux-notes) section.

The Mac and Windows instructions are intentionally kept as parallel as possible. The same dependencies are installed in the same order, just through each platform's preferred package manager.

> If you only want to **use** GitBolt and not develop it, download a pre-built installer from the [Releases page](https://github.com/ThePieMonster/GitBolt/releases) instead.

---

## Table of Contents

- [Prerequisites at a glance](#prerequisites-at-a-glance)
- [macOS — Step by step](#macos--step-by-step)
- [Windows — Step by step](#windows--step-by-step)
- [Building](#building)
- [Running](#running)
- [Tests](#tests)
- [Build options](#build-options)
- [Common issues](#common-issues)
- [Linux notes](#linux-notes)

---

## Prerequisites at a glance

| Dependency | Minimum version | macOS source | Windows source |
|---|---|---|---|
| C++ compiler | C++20 (Apple Clang 14+ / MSVC 19.30+) | Xcode Command Line Tools | Visual Studio 2022 |
| CMake | 3.21 | Homebrew | Visual Studio installer or [cmake.org](https://cmake.org/download/) |
| Qt | 6.5 | Homebrew (`qt`) | [Qt Online Installer](https://www.qt.io/download-qt-installer) |
| libgit2 | 1.7 | Homebrew | vcpkg |
| pkg-config / pkgconf | any recent | Homebrew | vcpkg |
| Ninja | any recent | Homebrew | Qt installer or [ninja-build.org](https://ninja-build.org/) |
| Git | 2.30+ | Apple CLT (already installed) | [git-scm.com](https://git-scm.com/download/win) |

A working install on either platform takes **10–20 minutes** including downloads. Most of that is Qt itself (~1.5 GB).

> **Tested configuration as of April 2026:** macOS Sequoia 15.6 with Apple Clang 17, Homebrew 5.1.4, CMake 4.3.1, Qt 6.11.0, libgit2 1.9.2, Ninja 1.13.2.

---

## macOS — Step by step

These instructions are tested on **macOS 14 (Sonoma) and macOS 15 (Sequoia)** on both Apple Silicon and Intel.

### 1. Install Xcode Command Line Tools

The compiler and Git come with the Xcode Command Line Tools. Open **Terminal** and run:

```bash
xcode-select --install
```

If a dialog appears, click **Install** and wait for it to finish (~5 minutes). If you already have it, you'll see a message saying so — that's fine.

Verify:

```bash
clang --version    # Should print Apple clang 14.0 or newer
git --version      # Should print git version 2.30 or newer
```

### 2. Install Homebrew

[Homebrew](https://brew.sh) is the macOS package manager we use to install everything else. If you already have it, skip this step.

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

The installer will ask for your **administrator password** once and then run for several minutes. When it finishes, follow its on-screen instructions to add `brew` to your `PATH` (the installer prints two `eval` commands you should run).

Verify:

```bash
brew --version     # Should print Homebrew 4.x or newer
```

### 3. Install GitBolt's build dependencies

Install everything in a single command:

```bash
brew install cmake qt libgit2 pkg-config ninja
```

| Package | Why we need it |
|---|---|
| `cmake` | The build system GitBolt uses |
| `qt` | The Qt 6 framework — provides the GUI toolkit |
| `libgit2` | The C library that GitBolt uses to talk to Git |
| `pkg-config` | Helps CMake find libgit2's compile flags |
| `ninja` | A fast build tool that CMake will use under the hood (optional but recommended) |

Qt is the largest download (~1.5 GB). Expect this command to take 5–10 minutes the first time.

> **Note:** Homebrew's mainline Qt formula is `qt`, not `qt@6` — it currently installs Qt 6.11. If a future Qt 7 ships, the formula may split into `qt@6` and `qt@7`. Run `brew info qt` to check the current version.

### 4. Configure your shell so CMake can find Qt 6

Homebrew installs Qt as a "keg-only" formula, which means it isn't automatically added to your `PATH`. We need to point CMake at it.

Pick **one** of these approaches:

**Option A — Permanent (recommended):** Add Qt to your shell profile so all future Terminal sessions can find it. On modern macOS the default shell is zsh, so we use `~/.zshrc`:

```bash
echo 'export PATH="/opt/homebrew/opt/qt/bin:$PATH"' >> ~/.zshrc
echo 'export CMAKE_PREFIX_PATH="/opt/homebrew/opt/qt:$CMAKE_PREFIX_PATH"' >> ~/.zshrc
source ~/.zshrc
```

> **Intel Macs:** Replace `/opt/homebrew` with `/usr/local` everywhere above.
> **bash users:** Use `~/.bash_profile` instead of `~/.zshrc`.

**Option B — Per-build:** Pass the path on the command line every time you configure:

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt -G Ninja
```

Verify Qt is discoverable. Note that the executable is `qmake` (not `qmake6`) when installed via Homebrew's `qt` formula:

```bash
/opt/homebrew/opt/qt/bin/qmake -v   # Should print "Using Qt version 6.11.0" or similar
```

You can also verify libgit2 was found by pkg-config:

```bash
pkg-config --modversion libgit2     # Should print 1.9.x or similar
```

### 5. Clone the GitBolt repository

```bash
cd ~/Developer    # or wherever you keep code
git clone https://github.com/ThePieMonster/GitBolt.git
cd GitBolt
```

You're now ready to [build](#building).

---

## Windows — Step by step

These instructions are tested on **Windows 10 21H2** and **Windows 11**.

### 1. Install Visual Studio 2022

Download the **free Community edition** from [visualstudio.microsoft.com](https://visualstudio.microsoft.com/downloads/).

When the installer asks which workloads to install, check:

- **Desktop development with C++**

Within that workload, make sure these individual components are selected (they usually are by default):

- MSVC v143 — VS 2022 C++ x64/x86 build tools
- Windows 11 SDK (or Windows 10 SDK)
- C++ CMake tools for Windows
- Git for Windows

Click **Install** and wait. This is a large download (~5 GB).

After installation, open **x64 Native Tools Command Prompt for VS 2022** from the Start menu — this is the shell you'll use for all subsequent commands. It has the compiler and CMake on its `PATH` automatically.

Verify:

```cmd
cl              REM Should print Microsoft (R) C/C++ Optimizing Compiler
cmake --version REM Should print cmake version 3.28 or similar
git --version   REM Should print git version 2.x
```

### 2. Install Qt 6

Qt does not have an official Windows package manager equivalent to Homebrew. Use the official installer:

1. Go to [qt.io/download-qt-installer](https://www.qt.io/download-qt-installer) and download the **Qt Online Installer for Windows**.
2. Run the installer. You'll need to create a free Qt account (or use an existing one) — Qt requires this for the open-source download.
3. Choose **Custom installation**.
4. Select these components:
   - **Qt → Qt 6.7.x → MSVC 2019 64-bit** (or newer MSVC variant matching your VS install)
   - **Qt → Qt 6.7.x → Sources** (optional, but useful for debugging)
   - **Developer and Designer Tools → CMake** (skip if you already installed CMake via VS)
   - **Developer and Designer Tools → Ninja**
5. Accept the open-source license and click **Install**. Expect ~1.5 GB download.

By default Qt installs to `C:\Qt`. Remember this path — you'll need it for the next step.

### 3. Install vcpkg and libgit2

[vcpkg](https://vcpkg.io) is Microsoft's C++ package manager. We use it to install libgit2 because there is no official Windows installer for libgit2.

In the **x64 Native Tools Command Prompt**:

```cmd
cd C:\
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg integrate install
.\vcpkg install libgit2:x64-windows pkgconf:x64-windows
```

The last command takes a few minutes — it builds libgit2 from source.

Verify:

```cmd
.\vcpkg list libgit2  REM Should show libgit2:x64-windows installed
```

### 4. Configure environment variables

So CMake can find both Qt and vcpkg, set these environment variables. Open **Settings → System → About → Advanced system settings → Environment Variables**, and add user variables:

| Variable | Value |
|---|---|
| `CMAKE_PREFIX_PATH` | `C:\Qt\6.7.2\msvc2019_64` (adjust version) |
| `VCPKG_ROOT` | `C:\vcpkg` |

(Alternatively, pass `-DCMAKE_PREFIX_PATH` and `-DCMAKE_TOOLCHAIN_FILE` on the command line at configure time — see [Building](#building).)

After changing environment variables, **close and reopen** the x64 Native Tools Command Prompt so the new values take effect.

Verify:

```cmd
echo %CMAKE_PREFIX_PATH%
echo %VCPKG_ROOT%
```

### 5. Clone the GitBolt repository

```cmd
cd %USERPROFILE%
mkdir Developer
cd Developer
git clone https://github.com/ThePieMonster/GitBolt.git
cd GitBolt
```

You're now ready to [build](#building).

---

## Building

The same commands work on both macOS and Windows once your environment is set up. Run them from the GitBolt source directory.

> **macOS users:** if you just installed Homebrew in this Terminal session and `cmake: command not found`, your PATH isn't set up yet. Either open a fresh Terminal window, or run `eval "$(/opt/homebrew/bin/brew shellenv)"` in the current one.

### Configure

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -G Ninja
```

> On Windows, if you didn't set `CMAKE_PREFIX_PATH` and `VCPKG_ROOT` as environment variables, pass them explicitly:
> ```cmd
> cmake -B build -DCMAKE_BUILD_TYPE=Debug -G Ninja ^
>   -DCMAKE_PREFIX_PATH=C:\Qt\6.7.2\msvc2019_64 ^
>   -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
> ```

CMake will print a summary of what it found. Look for:

- `Checking for module 'libgit2'` followed by `Found libgit2, version 1.x.x` — libgit2 was discovered via pkg-config
- `Configuring done`
- `Generating done`
- `Build files have been written to: <path>/build`

You may see `Could NOT find WrapVulkanHeaders` — this is harmless. GitBolt does not use Vulkan.

If you see "Could NOT find Qt6" or other errors, see [Common issues](#common-issues).

### Compile

```bash
cmake --build build --parallel
```

This compiles all 12 modules of GitBolt. On a modern laptop the first build takes **2–5 minutes**. Subsequent incremental builds are typically under 30 seconds.

Use `--parallel <N>` to control the number of jobs (defaults to all cores).

### Build a release

For a production build, replace `Debug` with `Release`:

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build-release --parallel
```

Release builds run substantially faster than debug builds and are what you should use when measuring performance.

---

## Running

After a successful build:

**macOS:**

```bash
open build/src/app/GitBolt.app
```

Or to see stdout/stderr in your terminal:

```bash
./build/src/app/GitBolt.app/Contents/MacOS/GitBolt
```

**Windows:**

```cmd
build\src\app\Debug\GitBolt.exe
```

To open a specific repository on launch, pass it as an argument:

```bash
./build/src/app/GitBolt.app/Contents/MacOS/GitBolt ~/Developer/GitBolt
```

---

## Tests

GitBolt uses **QTest** integrated with **CTest**.

```bash
cmake --build build --target gitbolt_tests
ctest --test-dir build --output-on-failure
```

To run a single test executable directly with verbose output:

```bash
./build/tests/gitbolt_tests
```

---

## Build options

These CMake options can be passed at configure time with `-D<NAME>=<VALUE>`:

| Option | Default | Description |
|---|---|---|
| `GITBOLT_BUILD_TESTS` | `ON` | Build the unit test executables |
| `GITBOLT_BUILD_PLUGINS` | `ON` | Build the plugin system and built-in plugins |
| `GITBOLT_SANITIZERS` | `OFF` | Enable AddressSanitizer + UndefinedBehaviorSanitizer (Debug only) |
| `CMAKE_BUILD_TYPE` | (none) | `Debug`, `Release`, `RelWithDebInfo`, or `MinSizeRel` |

Example — debug build with sanitizers:

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DGITBOLT_SANITIZERS=ON -G Ninja
cmake --build build-asan --parallel
```

---

## Common issues

### "Could NOT find Qt6"

CMake can't locate the Qt installation.

- **macOS:** Make sure `CMAKE_PREFIX_PATH=/opt/homebrew/opt/qt` is set (or `/usr/local/opt/qt` on Intel). Run `brew --prefix qt` to find the exact path on your system.
- **Windows:** Make sure `CMAKE_PREFIX_PATH` points to the directory containing `Qt6Config.cmake`, typically `C:\Qt\<version>\msvc2019_64`.

### "Could NOT find PkgConfig" or "libgit2 not found"

CMake can't locate libgit2.

- **macOS:** Confirm `brew list libgit2` lists the package. Reinstall with `brew reinstall libgit2 pkg-config` if needed. Note that on recent Homebrew versions the package is `pkgconf` (a drop-in replacement for `pkg-config`); both work.
- **Windows:** Confirm `vcpkg list libgit2` shows it installed. Make sure `-DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake` was passed at configure time, or that `VCPKG_ROOT` is set as an environment variable.

### "MOC found unused class"

This usually means you have a `Q_OBJECT` macro in a class that wasn't picked up by `AUTOMOC`. Check that the file is listed in the relevant `CMakeLists.txt` and that the class declaration is in a `.h` file (Qt's `AUTOMOC` only scans header files by default).

### `ld: library 'git2' not found` on macOS

The libgit2 library wasn't linked. Verify pkg-config knows about it:

```bash
pkg-config --libs libgit2
```

If that command fails, reinstall libgit2:

```bash
brew reinstall libgit2 pkg-config
```

Then **delete the build directory** and re-run cmake from scratch:

```bash
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build build --parallel
```

### "C++20 features not available" or compiler version errors

Your compiler is too old.

- **macOS:** Make sure Xcode Command Line Tools is up to date. Run `xcode-select --install` again, or `softwareupdate --install --all` to update everything.
- **Windows:** Make sure you have Visual Studio 2022 (not 2019). Older versions don't fully support C++20.

### Build is very slow

By default `cmake --build` uses all CPU cores. If your system is becoming unresponsive, limit jobs:

```bash
cmake --build build --parallel 4
```

The largest module to compile is `git/` (42 files). If incremental builds are still slow, double-check that you're using **Ninja** (`-G Ninja` at configure time) instead of the default Make generator.

### "App is damaged and can't be opened" on macOS

This appears when running a development build that hasn't been code-signed. To allow it:

```bash
xattr -cr build/src/app/GitBolt.app
```

For production releases, the binary is code-signed by the GitBolt project.

---

## Linux notes

GitBolt builds and runs on Linux too. The instructions are very similar to macOS — install dependencies through your distro's package manager, then follow the same [Building](#building) steps.

**Ubuntu / Debian:**

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build pkg-config \
                 qt6-base-dev qt6-base-dev-tools \
                 libgit2-dev git
```

**Fedora:**

```bash
sudo dnf install cmake ninja-build pkgconf-pkg-config gcc-c++ \
                 qt6-qtbase-devel libgit2-devel git
```

**Arch:**

```bash
sudo pacman -S cmake ninja pkgconf gcc qt6-base libgit2 git
```

After installing dependencies, the configure and build commands are identical to macOS.

---

## Need help?

If you hit a build issue not covered above, please:

1. Search [existing issues](https://github.com/ThePieMonster/GitBolt/issues) on GitHub.
2. Open a new issue including:
   - Your OS and version
   - The exact CMake/build command that failed
   - The full error message
   - Output of `cmake --version`, `qmake6 -v` (or `qmake -v`), and the compiler version
