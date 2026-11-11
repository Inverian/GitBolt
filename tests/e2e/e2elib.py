"""Shared harness for GitBolt's end-to-end tests.

Each test launches the real app binary with the test bridge enabled
(docs/AGENT_TESTING.md) and asserts repository outcomes on disk or
through `dump-state` — never by scraping widgets. Isolation, so a
test run can coexist with a real GitBolt session and with other tests:

- GITBOLT_TEST_BRIDGE: unique socket name per App instance (on
  Windows the named pipe \\\\.\\pipe\\<name>).
- GITBOLT_INSTANCE_NAME: unique namespace for the single-instance
  lock + forwarding socket, so a test launch can never forward its
  repo into a session the user has open.
- TMPDIR + XDG_RUNTIME_DIR (+ TMP and TEMP on Windows): a private
  runtime dir per App. Everything the app creates at runtime lands in
  it — the instance lock and socket (QDir::tempPath() on macOS and
  Windows, $XDG_RUNTIME_DIR on Linux), the bridge socket, the update
  folder, the log — and kill() removes it, so even a SIGKILLed app
  leaves nothing behind.
- XDG_CONFIG_HOME: a private dir unless the test passes config_home,
  so GitBolt.ini reads/writes never touch the user's real settings —
  one per App even where Apps share a runtime dir (see App). On
  Windows main() honours it for instance-named sessions.
- HOME: a private dir too, the same way. ~/.gitbolt (the crash log,
  which a crashing test would otherwise truncate and the user's next
  launch would report as theirs), ~/.gitconfig (read by the app's git
  and libgit2, written by Settings' Git Config page) and
  QDir::homePath() all hang off it. The harness's own git() keeps the
  test's HOME: make_repo sets the identity the app's commits need.
  On Windows HOME moves git's config only: QDir::homePath() is the
  profile folder whatever HOME says, so the tests that need ~/.gitbolt
  or $HOME moved skip there.
- XDG_STATE_HOME: a private dir, for the app's log on Linux (macOS
  keeps it under HOME, Windows in the real %LOCALAPPDATA%): an
  exported one would reach the developer's.
- QT_QPA_PLATFORM=offscreen: no focus stealing, same behavior on
  headless CI. An exported QT_QPA_PLATFORM wins for the apps a test
  drives, so a developer can watch them; never for a second launch,
  which may end in a modal nobody would dismiss.
- QT_FORCE_STDERR_LOGGING=1 on Windows: GitBolt is a GUI-subsystem app,
  whose messages would otherwise go to OutputDebugString, not the log.
- GITBOLT_TELEMETRY=0: usage statistics off, on top of
  the app's own rule that a bridge or instance-named session never
  sends (src/telemetry/TelemetryGate.h; test_telemetry_inert checks
  it). CI tests the very binary it ships, endpoints compiled in.
"""

from __future__ import annotations

import http.server
import json
import os
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import uuid

WINDOWS = os.name == "nt"

# A hook's own pid, as sh writes it: MSYS sh's $$ is no Windows pid.
SH_PID = "$(cat /proc/$$/winpid)" if WINDOWS else "$$"


def scratch_root() -> str | None:
    """Where make_runtime_dir() and run() make their dirs: the default,
    but on Windows its long form (the runner's %TEMP% is 8.3, Qt reports
    long paths, and a '~' must never reach sh)."""
    return os.path.realpath(tempfile.gettempdir()) if WINDOWS else None


def make_runtime_dir() -> str:
    """A private (0700) dir to serve as an app's TMPDIR and
    XDG_RUNTIME_DIR. Short prefix: the unix sockets inside it must fit
    sun_path (104 bytes on macOS)."""
    return tempfile.mkdtemp(prefix="gb-", dir=scratch_root())


def remove_tree(path: str) -> None:
    """shutil.rmtree, errors ignored. git's object files are read-only,
    which Windows won't delete: those are made writable first."""
    def writable_again(func, failed: str, _info) -> None:
        try:
            os.chmod(failed, stat.S_IWRITE)
            func(failed)
        except OSError:
            pass
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=writable_again)
    else:
        shutil.rmtree(path, onerror=writable_again)


def pid_alive(pid: int) -> bool:
    """Whether process `pid` still runs. Never os.kill(pid, 0) on
    Windows: signal 0 is CTRL_C_EVENT there."""
    if WINDOWS:
        import _winapi
        try:
            handle = _winapi.OpenProcess(_winapi.SYNCHRONIZE, False, pid)
        except PermissionError:
            return True
        except OSError:
            return False
        try:
            return _winapi.WaitForSingleObject(handle, 0) == _winapi.WAIT_TIMEOUT
        finally:
            _winapi.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def kill_pid(pid: int) -> None:
    """SIGKILL, or TerminateProcess on Windows; a process already gone
    is fine."""
    try:
        os.kill(pid, signal.SIGTERM if WINDOWS else signal.SIGKILL)
    except OSError:
        pass


def _nt_process(pid: int, call: str) -> None:
    """ntdll's NtSuspendProcess or NtResumeProcess on `pid`."""
    import ctypes
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.OpenProcess.argtypes = (ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong)
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
    nt_call = getattr(ctypes.WinDLL("ntdll"), call)
    nt_call.argtypes = (ctypes.c_void_p,)
    handle = kernel32.OpenProcess(0x0800, False, pid)   # PROCESS_SUSPEND_RESUME
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        status = nt_call(handle)
        if status:
            raise OSError(f"{call}({pid}): NTSTATUS {status & 0xFFFFFFFF:#x}")
    finally:
        kernel32.CloseHandle(handle)


def suspend(pid: int) -> None:
    """Stop every thread of `pid` (SIGSTOP), as a busy GUI thread would
    be: alive, but answering nothing."""
    if WINDOWS:
        _nt_process(pid, "NtSuspendProcess")
    else:
        os.kill(pid, signal.SIGSTOP)


def resume(pid: int) -> None:
    """Undo suspend() (SIGCONT)."""
    if WINDOWS:
        _nt_process(pid, "NtResumeProcess")
    else:
        os.kill(pid, signal.SIGCONT)


def write_script(path: str, text: str) -> None:
    """An executable sh script (a hook, a stub) with \\n line ends: Git
    for Windows runs hooks through its sh, which CRLF breaks."""
    with open(path, "w", newline="\n") as fh:
        fh.write(text)
    os.chmod(path, 0o755)


def sh_quote(path: str) -> str:
    """`path` quoted for sh. Forward slashes on Windows, which MSYS sh
    takes in a C:/... path as it does on the command line."""
    return shlex.quote(path.replace("\\", "/") if WINDOWS else path)


def bridge_text(text: str) -> str:
    """`text` for the bridge's `type`, which decodes \\n, \\t and \\\\: a
    Windows path's backslashes go doubled."""
    return text.replace("\\", "\\\\")


def app_env(runtime_dir: str, instance_name: str,
            bridge_name: str | None = None,
            config_home: str | None = None,
            home: str | None = None) -> dict:
    """Environment for a GitBolt process isolated into `runtime_dir`."""
    env = os.environ.copy()
    env.pop("GITBOLT_TEST_BRIDGE", None)
    if bridge_name:
        env["GITBOLT_TEST_BRIDGE"] = bridge_name
    env["GITBOLT_INSTANCE_NAME"] = instance_name
    env["TMPDIR"] = runtime_dir
    env["XDG_RUNTIME_DIR"] = runtime_dir
    env["XDG_CONFIG_HOME"] = (config_home
                              or os.path.join(runtime_dir, "config"))
    env["HOME"] = home or os.path.join(runtime_dir, "home")
    os.makedirs(env["HOME"], exist_ok=True)
    env["XDG_STATE_HOME"] = os.path.join(runtime_dir, "state")
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    env["GITBOLT_TELEMETRY"] = "0"
    if WINDOWS:
        # QDir::tempPath() reads TMP, then TEMP.
        env["TMP"] = env["TEMP"] = runtime_dir
        env["QT_FORCE_STDERR_LOGGING"] = "1"
    return env


def git(*args: str, cwd: str) -> str:
    """Run git, returning stripped stdout; raises on failure."""
    out = subprocess.run(
        ["git", *args], cwd=cwd, capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError(
            f"git {' '.join(args)} failed in {cwd}:\n{out.stderr}")
    return out.stdout.strip()


def make_repo(path: str, files=("a.txt",), msg: str = "base") -> str:
    """Init a repo at `path` with one commit containing `files`."""
    os.makedirs(path, exist_ok=True)
    git("init", "-q", "-b", "main", cwd=path)
    git("config", "user.email", "e2e@gitbolt.test", cwd=path)
    git("config", "user.name", "GitBolt E2E", cwd=path)
    for f in files:
        with open(os.path.join(path, f), "w") as fh:
            fh.write(f"{f}\n")
    git("add", "-A", cwd=path)
    git("commit", "-q", "-m", msg, cwd=path)
    return path


class Failure(Exception):
    """Raised by assertions; main() turns it into a FAIL + exit 1."""


class SilentServer:
    """A git:// "server" on 127.0.0.1 that accepts one connection and
    never answers, so a fetch from it is stuck in git's first read for
    as long as the test likes. `closed` is set once the connection has
    ended: every process holding it is gone (or hang_up() was called).
    """

    def __init__(self) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.url = f"git://127.0.0.1:{self.port}/repo.git"
        self.conn: socket.socket | None = None
        self.connected = threading.Event()
        self.closed = threading.Event()
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self) -> None:
        conn, _ = self.sock.accept()
        self.conn = conn
        self.connected.set()
        # A git killed on Windows resets the connection, not closes it.
        try:
            with conn:
                while conn.recv(4096):      # git's request, never answered
                    pass
        except OSError:
            pass
        finally:
            self.closed.set()

    def hang_up(self) -> None:
        """Drop the connection unanswered: git's fetch then fails."""
        if self.conn is not None:
            try:
                self.conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass    # git has closed it already


class OpenGate:
    """Holds every open of repository `repo` until release(), for a test
    that needs something to happen while GitBolt opens it: the repo's
    config includes a FIFO, and reading that config (libgit2's open,
    and any git command run in the repo) blocks on the FIFO until
    something writes to it. Released, a thread keeps opening the FIFO
    for writing, so every read of it gets its end at once from then on.
    """

    def __init__(self, repo: str, scratch: str) -> None:
        if WINDOWS:
            raise Failure("OpenGate needs a FIFO, which Windows has not")
        self.fifo = os.path.join(scratch, f"gate-{uuid.uuid4().hex[:8]}.fifo")
        os.mkfifo(self.fifo)
        git("config", "include.path", self.fifo, cwd=repo)
        self._released = threading.Event()
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self) -> None:
        self._released.wait()
        while True:
            try:
                with open(self.fifo, "w"):
                    pass
            except OSError:
                return      # the scratch dir is gone
            time.sleep(0.01)

    def release(self) -> None:
        self._released.set()


class AuthServer:
    """An http:// git "server" on 127.0.0.1 that wants a user name and
    password: every request without credentials is answered 401 with a
    Basic challenge, so git asks for them. One with credentials gets a
    404, which ends the fetch."""

    def __init__(self) -> None:
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                if self.headers.get("Authorization"):
                    self.send_response(404)
                else:
                    self.send_response(401)
                    self.send_header("WWW-Authenticate", 'Basic realm="e2e"')
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *args) -> None:
                pass    # keep the test's output to its verdict

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.origin = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        self.url = f"{self.origin}/repo.git"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def askpass_children(self) -> list[str]:
        """The askpass processes asking for a user name for this server:
        git passes its question as the argument, and it names the server
        ("Username for 'http://127.0.0.1:<port>': ")."""
        if WINDOWS:
            listing = ["powershell", "-NoProfile", "-NonInteractive", "-Command",
                       "Get-CimInstance Win32_Process | ForEach-Object "
                       "{ '{0} {1}' -f $_.ProcessId, $_.CommandLine }"]
        else:
            listing = ["ps", "-A", "-o", "pid=,args="]
        out = subprocess.run(listing, capture_output=True, text=True,
                             errors="replace").stdout
        question = f"Username for '{self.origin}'"
        return [line.strip() for line in out.splitlines()
                if question in line]

    def close(self) -> None:
        """Stop serving, and end any askpass still asking (a failed test
        must not leave one behind)."""
        self.httpd.shutdown()
        self.httpd.server_close()
        for line in self.askpass_children():
            try:
                kill_pid(int(line.split()[0]))
            except ValueError:
                pass


def _pipe_exchange(name: str, payload: bytes, replies: int,
                   timeout: float) -> bytes:
    """App._exchange over the Windows named pipe `name`, overlapped so
    that the write has a deadline too: Qt's pipes have no buffer, so a
    write waits until the app's event loop takes the connection."""
    import _winapi
    deadline = time.monotonic() + timeout

    def ms_left() -> int:
        left = int((deadline - time.monotonic()) * 1000)
        if left <= 0:
            raise TimeoutError(f"the bridge {name} didn't answer within {timeout} s")
        return left

    while True:
        try:
            handle = _winapi.CreateFile(
                name, _winapi.GENERIC_READ | _winapi.GENERIC_WRITE, 0,
                _winapi.NULL, _winapi.OPEN_EXISTING,
                _winapi.FILE_FLAG_OVERLAPPED, _winapi.NULL)
            break
        except OSError as error:    # no pipe yet: FileNotFoundError
            if error.winerror != _winapi.ERROR_PIPE_BUSY:
                raise
        try:
            _winapi.WaitNamedPipe(name, ms_left())
        except OSError as error:
            if error.winerror != _winapi.ERROR_SEM_TIMEOUT:
                raise

    def finish(ov, err: int) -> None:
        if err == _winapi.ERROR_IO_PENDING:
            try:
                waited = _winapi.WaitForSingleObject(ov.event, ms_left())
            except TimeoutError:
                waited = _winapi.WAIT_TIMEOUT
            if waited != _winapi.WAIT_OBJECT_0:
                ov.cancel()
                try:
                    ov.GetOverlappedResult(True)
                except OSError:
                    pass
                raise TimeoutError(f"the bridge {name} didn't answer within {timeout} s")
        ov.GetOverlappedResult(True)

    buf = b""
    try:
        finish(*_winapi.WriteFile(handle, payload, overlapped=True))
        while buf.count(b"\n") < replies:
            try:
                ov, err = _winapi.ReadFile(handle, 65536, overlapped=True)
                finish(ov, err)
            except BrokenPipeError:     # the bridge hung up
                break
            chunk = ov.getbuffer()
            if not chunk:
                break
            buf += chunk
    finally:
        _winapi.CloseHandle(handle)
    return buf


class App:
    """One GitBolt process plus its private bridge connection."""

    def __init__(self, binary: str, repo: str | None = None,
                 config_home: str | None = None,
                 instance_name: str | None = None,
                 runtime_dir: str | None = None,
                 label: str = "app",
                 home: str | None = None,
                 cwd: str | None = None,
                 extra_env: dict | None = None):
        tag = uuid.uuid4().hex[:8]
        self.binary = binary
        self.label = label
        self.bridge_name = f"gb-e2e-{tag}"
        self.instance_name = instance_name or f"gb-e2e-inst-{tag}"
        # Pass another App's runtime_dir (with its instance_name) to
        # launch into its single-instance namespace; only the App that
        # created the dir removes it.
        self._owns_runtime_dir = runtime_dir is None
        self.runtime_dir = runtime_dir or make_runtime_dir()
        # Settings of its own all the same. A SIGKILL can land inside
        # a settings write and leave QSettings' GitBolt.ini.lock empty:
        # no PID for QLockFile to prove it stale, so the next app to
        # save a setting would sit on its GUI thread (bridge silent)
        # until the lock is 30 s old.
        if config_home is None and not self._owns_runtime_dir:
            config_home = os.path.join(self.runtime_dir, f"config-{tag}")
        # And a home: two apps writing one ~/.gitbolt/crash.log would
        # clobber each other's.
        if home is None and not self._owns_runtime_dir:
            home = os.path.join(self.runtime_dir, f"home-{tag}")
        self.log_path = os.path.join(self.runtime_dir,
                                     f"{self.bridge_name}.log")
        self.env = app_env(self.runtime_dir, self.instance_name,
                           bridge_name=self.bridge_name,
                           config_home=config_home, home=home)
        # extra_env: on top, as a shell that exports them would have it.
        self.env.update(extra_env or {})

        args = [binary] + ([repo] if repo else [])
        self._log = open(self.log_path, "w")
        # stdin from nowhere, as a desktop launch has it: no terminal.
        # cwd: where the app starts, as from a terminal in a folder.
        self.proc = subprocess.Popen(
            args, env=self.env, stdin=subprocess.DEVNULL, stdout=self._log,
            stderr=subprocess.STDOUT, cwd=cwd)

    @property
    def lock_path(self) -> str:
        return os.path.join(self.runtime_dir, self.instance_name + ".lock")

    def instance_files(self) -> list[str]:
        """This app's lock and sockets still on disk (names only)."""
        names = (self.instance_name, self.instance_name + ".lock",
                 self.instance_name + ".lock.rmlock", self.bridge_name)
        return [n for n in names
                if os.path.exists(os.path.join(self.runtime_dir, n))]

    def start_second(self, *args: str) -> subprocess.Popen:
        """Start another GitBolt in this app's single-instance
        namespace — it should hand `args` over to this app. The caller
        reaps it (communicate) or kills it."""
        env = dict(self.env)
        # Its own bridge name: a window it wrongly opened must not take
        # over this app's bridge socket.
        env["GITBOLT_TEST_BRIDGE"] = f"{self.bridge_name}-second"
        # Offscreen whatever the developer exported: a launch that gives
        # up shows its "not responding" modal on any real platform
        # (wayland, xcb, cocoa), and nobody would dismiss it — the run
        # would hang to its timeout instead of seeing exit 1.
        env["QT_QPA_PLATFORM"] = "offscreen"
        return subprocess.Popen([self.binary, *args], env=env,
                                stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)

    def launch_second(self, *args: str,
                      timeout: float = 30) -> subprocess.CompletedProcess:
        """Run start_second(*args) to completion."""
        proc = self.start_second(*args)
        try:
            out, err = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            raise Failure(f"second launch {list(args)} never exited — "
                          "it opened its own window or hung on a "
                          "dialog") from None
        return subprocess.CompletedProcess(proc.args, proc.returncode,
                                           out, err)

    # ---- bridge protocol -------------------------------------------------

    def _exchange(self, payload: bytes, replies: int, timeout: float) -> bytes:
        """Send `payload` to the bridge, and read until `replies` lines
        are in or it hangs up. A timeout raises TimeoutError, an OSError."""
        if WINDOWS:
            return _pipe_exchange("\\\\.\\pipe\\" + self.bridge_name, payload,
                                  replies, timeout)
        path = os.path.join(self.runtime_dir, self.bridge_name)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(timeout)
            sock.connect(path)
            sock.sendall(payload)
            buf = b""
            while buf.count(b"\n") < replies:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                buf += chunk
        return buf

    def cmd(self, line: str, timeout: float = 10.0) -> dict:
        buf = self._exchange(line.encode() + b"\n", 1, timeout)
        return json.loads(buf.decode())

    def wait_bridge(self, timeout: float = 20.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise Failure(
                    f"{self.label} exited early "
                    f"(rc={self.proc.returncode}) — log: {self.tail()}")
            try:
                if self.cmd("dump-state").get("ok"):
                    return
            except (OSError, json.JSONDecodeError):
                time.sleep(0.25)
        raise Failure(f"{self.label}: bridge never came up "
                      f"— log: {self.tail()}")

    def state(self) -> dict:
        return self.cmd("dump-state")

    def wait_until(self, pred, what: str, timeout: float = 20.0) -> dict:
        """Poll dump-state until pred(state) is truthy."""
        deadline = time.monotonic() + timeout
        last = {}
        while time.monotonic() < deadline:
            last = self.state()
            if pred(last):
                return last
            time.sleep(0.3)
        raise Failure(f"{self.label}: timed out waiting for {what}; "
                      f"last state: {json.dumps(last)}")

    # ---- widget helpers ----------------------------------------------------

    def find_action_slug(self, prefix: str) -> str:
        """First enabled action whose slug is `prefix` or `prefix-<n>`
        (live-count suffixes: 'commands/commit' matches
        'commands/commit-4')."""
        actions = self.cmd("list-actions")["actions"]
        for a in actions:
            slug = a["slug"]
            if slug == prefix or (
                    slug.startswith(prefix + "-")
                    and slug[len(prefix) + 1:].isdigit()):
                if a["enabled"]:
                    return slug
        raise Failure(f"no enabled action matching {prefix}; have: "
                      + ", ".join(a["slug"] for a in actions[:40]))

    def action_enabled(self, object_name: str) -> bool:
        for a in self.cmd("list-actions")["actions"]:
            if a.get("objectName") == object_name:
                return a["enabled"]
        raise Failure(f"no action named {object_name}")

    def view_rows(self, object_name: str) -> int:
        for v in self.cmd("list-widgets")["views"]:
            if v.get("objectName") == object_name:
                return v["rows"]
        raise Failure(f"view not visible: {object_name}")

    def wait_view_rows(self, object_name: str, expected: int,
                       timeout: float = 15.0) -> None:
        """Poll until a view holds exactly `expected` rows.

        click/trigger are queued (the bridge replies before the action
        runs), and views populate from async refreshes — any read-after-
        write of a row count must poll or it races the event loop.
        """
        deadline = time.monotonic() + timeout
        last: int | None = None
        while time.monotonic() < deadline:
            try:
                last = self.view_rows(object_name)
            except Failure:
                last = None    # view not created/visible yet
            if last == expected:
                return
            time.sleep(0.2)
        raise Failure(f"{self.label}: {object_name} expected {expected} "
                      f"rows, last saw {last}")

    def ok(self, line: str) -> dict:
        """Send a command and fail the test on {"ok": false}."""
        resp = self.cmd(line)
        if not resp.get("ok"):
            raise Failure(f"`{line}` -> {json.dumps(resp)}")
        return resp

    def batch(self, *lines: str, timeout: float = 10.0) -> list[dict]:
        """ok() for several commands sent in one write. The bridge
        handles every line it has before going back to the event loop,
        so the actions they trigger are queued back to back: all of
        them run before anything the first one sets off on a worker
        (that worker's finished signal, say) can get in between."""
        buf = self._exchange("".join(line + "\n" for line in lines).encode(),
                             len(lines), timeout)
        replies = [json.loads(r) for r in buf.decode().splitlines()]
        if len(replies) != len(lines):
            raise Failure(f"{lines} -> {len(replies)} replies: {replies}")
        for line, resp in zip(lines, replies):
            if not resp.get("ok"):
                raise Failure(f"`{line}` -> {json.dumps(resp)}")
        return replies

    # ---- teardown ----------------------------------------------------------

    def quit(self, timeout: float = 10.0) -> int:
        """Graceful close; returns exit code."""
        try:
            self.cmd("quit")
        except OSError:
            pass
        try:
            return self.proc.wait(timeout)
        finally:
            self._log.close()

    def crash(self) -> None:
        """SIGKILL, leaving the lock and sockets behind as a crash or a
        force-quit does."""
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(5)

    def kill(self) -> None:
        """Teardown: SIGKILL if still running, then remove the runtime
        dir with whatever the app left in it."""
        self.crash()
        if not self._log.closed:
            self._log.close()
        if self._owns_runtime_dir:
            remove_tree(self.runtime_dir)

    def log_text(self) -> str:
        """Everything the app printed so far (its stdout and stderr)."""
        if not self._log.closed:
            self._log.flush()
        with open(self.log_path, encoding="utf-8", errors="replace") as fh:
            return fh.read()

    def tail(self, lines: int = 15) -> str:
        try:
            return "\n" + "".join(self.log_text().splitlines(True)[-lines:])
        except OSError:
            return "(no log)"


# ---- what the window shows -------------------------------------------------

def on_screen(name: str):
    """Predicate on dump-state: repository `name` is open and its title
    is up. After Close the service still has it open; the title tells
    them apart."""
    def pred(s: dict) -> bool:
        return (any(t.startswith(name) for t in s["windows"])
                and s.get("repoPath", "").rstrip("/").endswith(name))
    return pred


def home_screen(s: dict) -> bool:
    """Predicate on dump-state: the home screen Close leaves (File →
    Home keeps the repository's title)."""
    return "GitBolt" in s["windows"]


def check_for(seconds: float, what: str, bad) -> None:
    """Fail if bad() names a problem at any point in the next `seconds`:
    for what must not happen once queued work has run (an op's failure
    and its finished handler land a moment after its git exits)."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        problem = bad()
        if problem:
            raise Failure(f"{what}: {problem}")
        time.sleep(0.2)


# ---- periodic background fetch ---------------------------------------------

AUTO_FETCH_TIMER = "periodicFetchTimer"


def auto_fetch_config(scratch: str) -> str:
    """A config_home with Plugins → Periodic background fetch on, as the
    menu saves it. The hour-long interval keeps the real timer out of
    the way: tests fire it through the bridge (stall_auto_fetch)."""
    config_home = os.path.join(scratch, "config")
    ini_dir = os.path.join(config_home, "GitBolt")
    os.makedirs(ini_dir)
    with open(os.path.join(ini_dir, "GitBolt.ini"), "w") as fh:
        fh.write("[plugins]\n"
                 "periodicFetch\\enabled=true\n"
                 "periodicFetch\\intervalMinutes=60\n")
    return config_home


def stall_auto_fetch(app: App, work: str) -> SilentServer:
    """Point origin at a fresh silent server and fire an auto-fetch at
    it; returns once git is connected and waiting. Fires again while
    ticks are skipped: the last remote op's finished handler may still
    be queued behind whatever the test waited for."""
    server = SilentServer()
    git("remote", "set-url", "origin", server.url, cwd=work)
    deadline = time.monotonic() + 20
    while not server.connected.is_set():
        if time.monotonic() > deadline:
            raise Failure("the auto-fetch never reached the server")
        app.ok(f"fire-timer {AUTO_FETCH_TIMER}")
        server.connected.wait(1)
    return server


def run(test_fn) -> None:
    """Entry point: scratch dir + app binary from argv, PASS/FAIL exit."""
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <path-to-gitbolt-binary>")
        sys.exit(2)
    # Absolute: a test may start the app in a folder of its own (cwd).
    binary = os.path.abspath(sys.argv[1])
    if not os.access(binary, os.X_OK):
        print(f"FAIL: not executable: {binary}")
        sys.exit(1)
    scratch = tempfile.mkdtemp(prefix="gb-e2e-", dir=scratch_root())
    apps: list[App] = []
    passed = False
    try:
        test_fn(binary, scratch, apps)
        passed = True
    except Failure as e:
        print(f"FAIL: {e}")
    finally:
        if not passed:
            # The logs go with each app's runtime dir; keep their ends.
            for app in apps:
                print(f"--- {app.label} log:{app.tail()}")
        for app in apps:
            app.kill()
        remove_tree(scratch)
    if passed:
        print("PASS")
    sys.exit(0 if passed else 1)
