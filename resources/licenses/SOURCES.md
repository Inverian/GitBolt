# Where the licence texts come from

Every licence text here was fetched from its upstream at a pinned version and
is kept verbatim. The exceptions are `NSIS-3.13.txt`, whose line endings
were made LF, and the texts in `qt-<version>-third-party.txt`, which
`tools/update-qt-notices.py` writes from Qt's source and normalises as its
header says. `cmake/Notices.cmake` puts these texts in the packages' notices.

Files GitBolt composed or wrote say so below, with how they were put
together. SHA-256 values are of the files as fetched. Everything was fetched
and checked on 2026-11-07.

When a component is updated, fetch its texts again at the new version, check
them the same way, and update this file.

## Copied whole

| File | Upstream | Version | SHA-256 of the fetched file |
|---|---|---|---|
| `../../LICENSE` (GitBolt's own) | https://www.gnu.org/licenses/gpl-3.0.txt | GPL v3, 29 June 2007 | `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986` |
| `LGPL-3.0.txt` | https://www.gnu.org/licenses/lgpl-3.0.txt | LGPL v3, 29 June 2007 | `e3a994d82e644b03a792a930f574002658412f62407f5fee083f2555c5f23118` |
| `LGPL-2.1.txt` | https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt | LGPL v2.1, February 1999 | `20e50fe7aae3e56378ebf0417d9de904f55a0e61e4df315333e632a4d3555d95` |
| `Apache-2.0.txt` | https://www.apache.org/licenses/LICENSE-2.0.txt | Apache License 2.0, January 2004 | `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30` |
| `ICU-73.2.txt` | https://raw.githubusercontent.com/unicode-org/icu/release-73-2/icu4c/LICENSE | ICU 73.2, tag `release-73-2` (commit `680f521746a3bd6a86f25f25ee50a62d88b489cf`) | `f3005e195ff74d8812cc1f182a1c446fab678d70a10e3dada497585befee5416` |

Byte for byte, each file here is the fetched file.

- `LICENSE` and `LGPL-3.0.txt`: compared with gnu.org's texts as fetched.
- `LGPL-2.1.txt`: this is gnu.org's current text. libgit2's `COPYING` and
  libfuse's `LGPL2.txt` hold older copies of LGPL 2.1. Those differ only in
  the FSF's postal address and the example name in the appendix, so
  `appimage-runtime-20251108.txt` carries libfuse's own copy.
- `Apache-2.0.txt`: google/material-design-icons' `LICENSE` (see
  `material-symbols.txt`) is the same text, without the last newline.
- `ICU-73.2.txt`: this licence covers the ICU4C libraries (libicuuc,
  libicui18n, libicudata). It is the ICU licence itself plus the licences of
  the third-party data and code in ICU.
  - The tag has no top-level `LICENSE`; the URL answers 404.
  - The file is identical to `icu/LICENSE` in the release's own source
    archive, https://github.com/unicode-org/icu/releases/download/release-73-2/icu4c-73_2-src.tgz.
    That archive's SHA-512, `76dd782d…74027ce62`, matches the release's
    `SHASUM512.txt`.

## Composed by GitBolt

### `libgit2-1.9.7-extra.txt`

The notices of code in libgit2 1.9.7 that libgit2's `COPYING` doesn't carry,
under a short introduction and section headings GitBolt wrote. The excerpts
are verbatim, comment markers included. Each comes from
`https://raw.githubusercontent.com/libgit2/libgit2/v1.9.7/<path>` (lightweight
tag, commit `49e408b3208bc3093757a1c2db938d3590f3f412`).

| Path | Lines taken | SHA-256 of the fetched file |
|---|---|---|
| `deps/xdiff/xdiff.h` | 1-21 | `6459a65d5e0f910f6bdcba30cd5332a58fbb75118acb5757f5596bd22c00a6cb` |
| `deps/xdiff/xmerge.c` | 3 | `05e2e14cb5b268b4f92aede76d1c3cd9b7966d6ec9268bc7687aac0e76377376` |
| `deps/xdiff/xpatience.c` | 3 | `ff098106ced16724ffe55d4a371af314fedf356576d5ae2c3ed4e617938152ea` |
| `deps/xdiff/xhistogram.c` | 1-42 | `74bc7e7b8926418d20d2772fe8aa5c3f4c525530305c09cfe81712c80ee92890` |
| `src/util/hashmap.h` | 10-36 | `1c7d053bb193e1413ae50eb3038127e9027707509e77d6ff82cc29d0a80f3719` |
| `src/util/util.c` | 540-569 (`git__bsearch`), 479-485 (MurmurHash3) | `9864552954bb737677930b5c2fa5bd2b459c5fa90277ceecb7c8287b3e4218ab` |
| `src/util/utf8.c` | 12-35 | `6cbf514d854524883a85a781e5e2a9003d71793fb2cd527795d26c972831a8ef` |
| `src/util/tsort.c` | 10-16 | `d1dee30457c185e86bc47efecc5417459aefeabe7cfaf8aebe5eb2239296ee20` |

**tsort's licence text.** `tsort.c` names the MIT licence but doesn't hold its
text. The text is lines 5-21 of
https://raw.githubusercontent.com/swenson/sort/c5253b5979347eea6d1665b6b544b06c78affa4d/LICENSE.md
(SHA-256 `c5a07616b686ec1f61b21ff84cd4f3b2796ad4abfbdc7e0e1ee96cb1a7424455`).
That is the first licence file of the project the code comes from.

**LibXDiff's licence text.** LibXDiff is under LGPL 2.1 or later, and the
file refers to `LGPL-2.1.txt` for that text.

**What was scanned.** Every `.c` and `.h` file under `src/` (except `src/cli`,
which isn't built), `deps/` and `include/` in GitHub's v1.9.7 tarball was
checked for copyright, licence and public-domain lines that aren't "the
libgit2 contributors". The tarball's SHA-256,
`1a4fbe7589e814777ae76b64734ad80f4ecad22cd33a22682a2aaea4ae5375e7`, is the
one `cmake/Libgit2.cmake` pins. The build options come from
`cmake/Libgit2.cmake`:
- No SSH or GSSAPI.
- HTTPS: SecureTransport (macOS), WinHTTP (Windows), off (Linux).
- Builtin llhttp and PCRE2.
- Bundled zlib on Windows only.
- ntlmclient on macOS only, since it needs an HTTPS backend on Unix.

Everything the scan found that isn't listed above falls into one of two
groups.

Already in `COPYING`:
- PCRE2, llhttp, zlib, sha1dc and ntlmclient, including its utf8.h, LLVM,
  Unicode, Team Explorer Everywhere and MD4 parts
- RFC 6234 SHA-256, wildmatch, Android `basename_r`, xoroshiro256**, and the
  OpenSSL headers

Not built by GitBolt's configuration:
- `deps/winhttp` (MinGW only)
- `deps/chromium-zlib`
- `include/git2/stdint.h` (no header includes it)
- `src/libgit2/streams/openssl_dynamic.h`

`src/libgit2/delta.c` also says its delta application was "heavily cribbed
from BinaryDelta.java in JGit", but it carries no copyright or licence
statement of JGit's, so there is no notice to copy.

### `material-symbols.txt`

GitBolt's attribution for its menu icons, `resources/icons/menu/*.svg` (see
`README.md` there). It is GitBolt's own wording.

**Licence.**
- https://raw.githubusercontent.com/google/material-design-icons/10ecc14790612ca8fd164e3f4ffe9c16fc50d5b7/LICENSE
  (SHA-256 `58d1e17ffe5109a7ae296caafcadfdbe6a7d176f0bc4ab01e12a689b0499d8bd`)
  is the Apache License 2.0, the same text as `Apache-2.0.txt`.
- The README at the same commit
  (SHA-256 `e1ab9d093fa612bd83a1043717ac150d636354c444792893dba2119f22a212c3`)
  gives that licence.
- The repository has no `NOTICE` file.

**Modification.** For each of the 69 files, GitBolt's `fill` attribute was
taken off the root `<svg>`. The result was compared with two upstream copies:
- `https://fonts.gstatic.com/s/i/short-term/release/materialsymbolsoutlined/<icon>/default/24px.svg`,
  for all 4,157 icons in the repository's `symbols/web` at that commit:
  68 files are identical to it.
- `https://raw.githubusercontent.com/google/material-design-icons/10ecc14790612ca8fd164e3f4ffe9c16fc50d5b7/symbols/web/<icon>/materialsymbolsoutlined/<icon>_24px.svg`:
  56 files are identical to it, `extension.svg` among them.

So the `fill` attribute is GitBolt's only change.
- `extension.svg` is identical to the repository's copy;
  fonts.gstatic.com serves a redrawn `extension` now.
- The 13 files that differ from the repository's copies all match
  fonts.gstatic.com. Twelve differ from the repository only in their path
  data. For `insights.svg`, the repository still holds the older drawing,
  which has a 24-unit canvas and no `viewBox`.

### `appimage-runtime-20251108.txt`

The notices for the AppImage runtime CI pins: type2-runtime release
`20251108`, `runtime-x86_64`.
- Download URL:
  https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
- SHA-256 `2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d`,
  which matches both CI's pin and the release's asset digest.

The file has two parts:
- a header GitBolt wrote, naming each component, its version, licence and
  source;
- the licence texts verbatim, each under a heading GitBolt wrote.

| Text | Upstream | SHA-256 |
|---|---|---|
| type2-runtime `LICENSE` | https://raw.githubusercontent.com/AppImage/type2-runtime/dd6cebedcbddde9c82f89b011e8e1d40b6e43868/LICENSE | `aa154fc9070614bbe7921f89db11efd1dba7a1f3a41685958110e2230f9c0ca1` |
| type2-runtime `src/runtime/runtime.c`, lines 1-33 | same commit, `src/runtime/runtime.c` | `581c98a4bf7ac6289338da36122ccb91a46941c1148760eb22e3002c085180b7` |
| musl `COPYRIGHT` | https://git.musl-libc.org/cgit/musl/plain/COPYRIGHT?h=v1.2.5 | `f9bc4423732350eb0b3f7ed7e91d530298476f8fec0c6c427a1c04ade22655af` |
| libfuse `LICENSE` | https://raw.githubusercontent.com/libfuse/libfuse/fuse-3.15.0/LICENSE | `b8832d9caaa075bbbd2aef24efa09f8b7ab66a832812d88c602da0c7b4397fad` |
| libfuse `LGPL2.txt` | https://raw.githubusercontent.com/libfuse/libfuse/fuse-3.15.0/LGPL2.txt | `dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551` |
| squashfuse `LICENSE` | https://raw.githubusercontent.com/vasi/squashfuse/0.5.2/LICENSE | `9e909cc8a8ba27b1a649c964cf9eda37de911f3138c2b64e6d2f01703904ac13` |
| zstd `LICENSE` | https://raw.githubusercontent.com/facebook/zstd/v1.5.6/LICENSE | `7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8` |
| zlib `LICENSE` | https://raw.githubusercontent.com/madler/zlib/v1.3.1/LICENSE | `845efc77857d485d91fb3e0b884aaa929368c717ae8186b66fe1ed2495753243` |
| mimalloc `LICENSE` | https://raw.githubusercontent.com/microsoft/mimalloc/v2.1.7/LICENSE | `19c99805e7a44a34b297a75d1edea9985e300066dfc024d5c99d4236d4573b5d` |
| libfuse patch (named, not copied) | https://raw.githubusercontent.com/AppImage/type2-runtime/dd6cebedcbddde9c82f89b011e8e1d40b6e43868/patches/libfuse/mount.c.diff | `1c7fd9e26717545a476b226b083a9f9d05676c180edbd71a04bbd8a73599dc44` |

Three texts were also checked against the source archives the builds use:
- musl's `COPYRIGHT` is identical in `musl-1.2.5.tar.gz`
  (`a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`).
- libfuse's `LGPL2.txt` and `LICENSE` are identical in `fuse-3.15.0.tar.xz`
  (`70589cfd5e1cff7ccd6ac91c86c01be340b227285c5e200baa284e401eea2ca0`, the hash
  type2-runtime's script checks).
- squashfuse's `LICENSE` is identical in `0.5.2.tar.gz`
  (`db0238c5981dabbd80ee09ae15387f390091668ca060a7bc38047912491443d3`).

libfuse's `LGPL2.txt` is not byte-identical to `LGPL-2.1.txt` (see above), so
it is copied rather than referred to.

**How each version was confirmed.** The runtime binary was inspected with
`strings` only. Its release's debug file, `runtime-x86_64.debug`
(SHA-256 `1f3c4a8439e344d74b61bfd0610b89e8499453791bbc3c5b4ae38c181fe54942`),
was inspected the same way.

| Component | Version | Evidence |
|---|---|---|
| type2-runtime | dd6cebe | The binary contains `https://github.com/AppImage/type2-runtime/commit/dd6cebe`. Tag `20251108` is commit `dd6cebedcbddde9c82f89b011e8e1d40b6e43868`. |
| squashfuse | 0.5.2 | The binary contains `squashfuse 0.5.2`. `scripts/common/install-dependencies.sh` fetches 0.5.2. |
| libfuse | 3.15.0, patched | The binary contains `3.15.0` beside `FUSE library version: %s`. The debug file's build directory is `fuse-3.15.0`. The patch's `FUSERMOUNT_PROG` lookup is present. |
| zstd | 1.5.6 | The binary contains `1.5.6` ahead of zstd's error strings. |
| zlib | 1.3.1 | The binary contains ` inflate 1.3.1 Copyright 1995-2024 Mark Adler `. |
| musl | 1.2.5 | The debug file's path is `/home/buildozer/aports/main/musl/src/musl-1.2.5`. |
| mimalloc | 2.1.7 | See below. |

The commit's `Dockerfile` (`alpine:3.21`) installs zstd, zlib, musl and
mimalloc as Alpine packages.

**mimalloc.** The runtime's `Makefile` links `-lmimalloc`, from Alpine's
`mimalloc-dev`. In Alpine 3.21 that name is provided only by
`mimalloc2-dev 2.1.7-r0`, built 2024-07-23, which is still the branch's
current build. The binary agrees:
- it has mimalloc's `retry_on_oom` option, which first appears in v2.1.7;
- it lacks the `visit_abandoned` and `guarded_*` options that v2.1.8 adds.

The release's build log (Actions run 20037536435) has expired, so it could
not be read.

### `windows-runtime.txt`

GitBolt's statement for the Microsoft Visual C++ runtime DLLs the installer
ships, as the Distributable Code terms require. No text is copied.

**Which Visual Studio.** CI's Windows image is `windows-2025-vs2026`
(20261103.298). Its `VCToolsRedistDir` is
`C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Redist\MSVC\14.51.36231\`,
so the DLLs are Visual Studio Enterprise 2026's.

**The terms.** They are at
https://visualstudio.microsoft.com/license-terms/vs2026-ga-pro-enterprise/.
The page embeds
https://visualstudio.microsoft.com/wp-content/uploads/2025/10/Visual_Studio_2026-License-Enterprise_Professional_ENU.docx
(SHA-256 `dd2ab92a7c2bb90dd509e16572707a34b13bc0bc09b9199d115f25594ac39053`;
"Last Updated: October 1, 2025", EULAID `VS2026_EntPro_ENU.1033`).
- Its Distributable Code section requires distributors to "require
  distributors and external end users to agree to terms that protect the
  Distributable Code at least as much as these license terms".
- The protections the statement passes on are taken from the same terms:
  the Scope of License restrictions, and the Limited Warranty section's
  exclusion of other warranties and of damages. The statement gives end users
  no warranty from Microsoft at all, which protects the files at least as
  much as those terms do.
- The Distributable List, https://aka.ms/vs/18/redistribution, lists
  `[VisualStudioFolder]\VC\redist` under "Visual C++ Runtime Files". The
  list resolves to
  https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution,
  dated 2025-11-11.

**Visual Studio 2022.** Its terms were compared:
https://visualstudio.microsoft.com/license-terms/vs2022-ga-proenterprise/,
whose embedded document has SHA-256
`9c0cd52b20db9d081854c75bd1b50c75514b8f8cb09c8cad15e89d90b97b5bf3` and
EULAID `VS_EntPro_2022_ENU.1033`. Its Distributable Code section asks the
same of distributors. Only its wording and its list's URL differ:
https://aka.ms/vs/17/redistribution.

### `NSIS-3.13.txt`

NSIS's `COPYING`, as shipped in `nsis-3.13.zip`, with one line GitBolt wrote
before it.
- The archive is
  https://downloads.sourceforge.net/project/nsis/NSIS%203/3.13/nsis-3.13.zip,
  with SHA-256 `ba63dffc4410ee89193e1cb5a41989991bd77c61068da17e3156d136b7b0b3d8`,
  the hash `.github/workflows/ci.yml` pins.
- In the archive, `nsis-3.13/COPYING` has SHA-256
  `388357c1215ff403c5ebde3a5ecd273e68f8b79a579996775245d1ee65442aba` and CRLF
  line endings.
- The copy here has LF line endings (SHA-256 of the converted text
  `6cc665347be5c84b76bf0fd6f85fbb0c33261e89b3408aa3e3e55dbe95e12d89`); nothing
  else is changed.
- The added line names NSIS's source archive,
  https://downloads.sourceforge.net/project/nsis/NSIS%203/3.13/nsis-3.13-src.tar.bz2,
  as CPL-1.0 section 3 asks for the LZMA module.

## Written by a tool

`qt-<version>-third-party.txt` is written by `tools/update-qt-notices.py`. Its
header records the Qt tag and trees it was generated from.
