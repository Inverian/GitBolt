# Privacy and usage statistics

GitBolt sends a small amount of usage statistics, and summaries of
crashes and errors, to the GitBolt maintainers. This page says exactly
what is sent, where it goes, how long it is kept, and how to turn it
off. It is on by default, and you can turn it off at any time.
Official builds also ask GitHub whether a newer GitBolt is out: see
[Update checks](#update-checks).

Nothing that identifies your repositories or their contents is sent:
no repository names or paths, no branch, tag or remote names, no remote
URLs, no commit messages or hashes, no file names or contents, no email
addresses or user names.

## What is sent

### Usage statistics

Each event carries:

- **the GitBolt version**, the **operating system** and its version
  (for example "macOS 15.6" or "ubuntu 24.04"), and the kind of device,
  always "desktop";
- **your install ID** and a **device ID** (see [below](#install-id-and-device-id));
- for an event that waited more than a minute before it could be sent,
  the time it happened.

The events are:

| Event | When | Also contains |
|---|---|---|
| `app_launched` | Each time GitBolt starts | CPU architecture (for example "arm64"), Qt version, and the language of your system (for example "en", without the region) |
| `command_used` | Every 10 minutes, and when GitBolt quits | The internal name of a menu or toolbar command you used (for example `act.commit` or `file.recent`), and how many times you used it |
| `repo_opened` | Each time a repository finishes opening | How many milliseconds it took to open and to show the first page of history, and a rough size of that page: "0", "1-99", "100-255" or "256+" commits |

### Error reports

When a Git operation, a commit, or opening a repository fails, GitBolt
sends the kind of error only:

- for a failed operation, its name, such as "fetch", "push" or
  "stage_hunk" ("other" for anything else);
- for a failed commit, nothing more;
- for a repository that would not open, the error code, such as
  "not_found" or "locked".

Each error report also carries the GitBolt version, the time, the
operating system and its version, the CPU architecture, the Qt version
and a random report ID. The error message itself, which can contain
paths and remote URLs, is never sent. The same error is sent at most
once per session, and at most 20 error reports per session.

### Crash reports

If GitBolt crashes, it saves a crash report on your computer. The next
time it starts, it tells you so, and when a summary of the report is
sent, it says that too. The summary goes out right away, with usage
statistics on; turning them off deletes whatever hasn't been sent yet,
the summary too if it hasn't gone out (while offline, say). The
summary contains:

- the GitBolt version that crashed, when it crashed, and a random
  report ID;
- the kind of crash (for example "SIGSEGV");
- the operating system and its version, the CPU architecture and the
  Qt version;
- the call stack, reduced to file names and positions within those
  files: GitBolt's own, with no function names, and those of the
  libraries below, with their function names:
  - Qt, and the Qt plugins that draw GitBolt's windows;
  - libgit2;
  - on macOS: the system's C and C++ libraries (`libsystem_*`,
    libc++), dyld, the Objective-C runtime, CoreFoundation,
    Foundation, AppKit and HIToolbox;
  - on Linux: libc, libpthread, libstdc++, libgcc_s and the dynamic
    loader;
  - on Windows: ntdll, kernel32, KERNELBASE, the C runtime (ucrtbase)
    and the Visual C++ runtime.

  Any other library shows as `<other>`, with no name, position or
  function, so the summary does not reveal what else is installed.

The full crash report never leaves your computer. Crash and error
reports do not carry your install ID or device ID, so they are not
linked to you or to your usage statistics.

Every request also says which version of GitBolt sent it, on which
operating system and, on Windows and Linux, for which CPU architecture,
in its `User-Agent` header (for example
`GitBolt/0.9.5 (Macintosh; Mac OS X 15_6)` or
`GitBolt/0.9.5 (X11; Linux x86_64)`).

## Install ID and device ID

The **install ID** is a random identifier (a UUID) that GitBolt creates
the first time it records usage statistics, and stores in its settings
file. It lets the maintainers tell how many installations use a
feature, without knowing who you are. You can see and copy it in
**Tools › Settings › Usage Statistics**.

The **device ID** is derived from the install ID with a SHA-256 hash,
so it identifies nothing the install ID doesn't. The analytics software
uses it to group events into sessions.

Both are sent with usage statistics only, never with crash or error
reports. Turning usage statistics off keeps the install ID on your
computer, so you can still quote it to have your data deleted.

## When it is sent

GitBolt sends nothing until its window is open. Events are sent about
10 seconds after they happen, one request at a time. A crash summary
is sent as soon as the window is open, before other events. If the
server can't be reached, they wait on your computer and GitBolt tries
again every 15 minutes; events still waiting when GitBolt quits are
sent the next time it starts. They are kept in a file next to
GitBolt's settings file: at most 500 events, for at most 14 days, and
an event that fails to send 5 times is dropped. A try that fails
before any of it reaches the server doesn't count: the server's name
can't be looked up, the connection is refused, or there is no route to
it, as while your computer is offline. A try that times out, or fails
in any other way, does.

## Where it goes

The data goes to the GitBolt maintainers' own self-hosted servers:
[OpenPanel](https://openpanel.dev) for usage statistics, and
[GlitchTip](https://glitchtip.com) for crash and error reports.

The servers are reached through Cloudflare, which processes connection
data such as your IP address at its network edge to deliver the
requests. GitBolt never puts your IP address in the data it sends, and
the servers do not store IP addresses.

## How long it is kept

- Usage statistics: 13 months.
- Crash and error reports: 90 days.

## How to turn it off

Either of these turns usage statistics, error reports and crash
reports off:

- In GitBolt, open **Tools › Settings › Usage Statistics** and clear **Send
  usage statistics and crash reports**. Sending stops at once, and any
  events still waiting on your computer are deleted.
- Set the environment variable `GITBOLT_TELEMETRY=0` (or `false`,
  `off`, `no`).

GitBolt also sends nothing when it is run by automated tests or in CI.

## Which builds send

Only official builds send anything: GitBolt's releases, and the builds
GitBolt's CI makes of its `main` branch. A build that can send shows a
**Usage Statistics** page in **Tools › Settings**; a build without that
page sends nothing. A build from source, a fork, or a
build of a pull request contains no server addresses, so it cannot
send, and a crash report it saves is never sent by an official build
either.

## Update checks

Official builds also ask whether a newer GitBolt has been released.
Unlike usage statistics, these requests go to GitHub, where GitBolt's
releases are published, not to the GitBolt maintainers.

### What is sent

A check is one HTTPS request to `api.github.com` for GitBolt's latest
release (`https://api.github.com/repos/Inverian/GitBolt/releases/latest`).
It carries the `User-Agent` header described above (the GitBolt version
and the operating system) and nothing else about you: no install ID or
device ID, no cookies, no GitHub account or token, and not your
system's language. GitHub sees your IP address, as it does for any
visit to github.com, and
[GitHub's privacy statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement)
covers what it does with it. GitBolt shows the release notes it gets
back as plain text, so nothing they link to is loaded.

Nothing is downloaded or installed until you choose **Install and
Restart**. Then GitBolt downloads the release's `SHA256SUMS.txt` and
the installer for your system from `github.com`, which hands the
download on to GitHub's file servers (`objects.githubusercontent.com`
or `release-assets.githubusercontent.com`). These requests carry the
same `User-Agent`. GitHub counts the download, as it counts one from
the release page, and shows that count publicly.

### When

- **Automatically**, once a day: 15 seconds or so after the window
  opens, and then every hour while GitBolt is open, it asks GitHub
  again if GitHub last answered more than a day ago. A check that
  fails, while you are offline for example, is tried again an hour
  later.
- **When you ask**: **Help › Check for updates** (on macOS also
  **GitBolt › Check for Updates…**) asks GitHub every time, even with
  automatic checks turned off.

GitBolt checks nothing when it is run by automated tests or in CI.

### What is checked before installing

- Only HTTPS, with your system's certificate checks, and only to the
  GitHub hosts named above.
- The release must be a published release of
  `github.com/Inverian/GitBolt`, not a draft or a pre-release, with a
  `v<version>` tag.
- The download must have the size GitHub lists for it, and the
  SHA-256 checksum the release's `SHA256SUMS.txt` lists for it (and,
  when GitHub lists one, the checksum GitHub gives).
- On macOS, the app in the disk image must pass
  `codesign --verify --deep --strict` and be GitBolt
  (`com.gitbolt.app`) at the release's version. On Windows and Linux,
  the file must have exactly the release's name for your kind of
  install.

These checks show that the file is the one on GitBolt's GitHub release
page and that it arrived intact: the same trust as downloading it from
that page yourself. They can't show who published it: GitBolt's
installers have no Apple Developer ID or Windows Authenticode
signature, and `SHA256SUMS.txt` comes from the same release. GitBolt
doesn't check the release's build attestations either (that needs a
Sigstore verifier); you can check them yourself with
`gh attestation verify <file> --repo Inverian/GitBolt`.

### Which builds check

Only official builds: GitBolt's releases, and the builds GitBolt's CI
makes of its `main` branch. A build from source, a fork, a pull
request or a Linux distribution's package doesn't check: there,
**Help › Check for updates** is greyed out (**Help › About** links to
GitBolt's releases page), and **Tools › Settings** has no **Updates**
page.

### How to turn it off

Either of these stops the automatic checks:

- In GitBolt, open **Tools › Settings › Updates** and clear **Check for
  updates automatically**.
- Set the environment variable `GITBOLT_UPDATE_CHECK=0` (or `false`,
  `off`, `no`).

**Help › Check for updates** still asks GitHub when you choose it.

## Deleting your data

To have your usage statistics deleted, email
[support@gitbolt.com](mailto:support@gitbolt.com) and quote your
install ID from **Tools › Settings › Usage Statistics**. Crash and error
reports carry no install ID; they are deleted after 90 days.

## Changes to this policy

This page describes the current GitBolt release. When a release
changes what is collected, this page changes with it, in the same
release; what earlier releases collected is in this file's history in
the GitBolt repository.
