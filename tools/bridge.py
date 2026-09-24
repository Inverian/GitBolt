#!/usr/bin/env python3
"""Client for GitBolt's env-gated test bridge (docs/AGENT_TESTING.md).

Launch the app with the bridge enabled, then send commands:

    GITBOLT_TEST_BRIDGE=1 ./build/src/app/GitBolt.app/Contents/MacOS/GitBolt &
    tools/bridge.py dump-state
    tools/bridge.py list-actions | python3 -m json.tool
    tools/bridge.py trigger commands/resolve-conflicts
    tools/bridge.py select-row QTableView:0 0
    tools/bridge.py select-row commit.unstagedList 0,2,5
    tools/bridge.py click "Lock/Unlock"
    tools/bridge.py select-item toolbar.branchCombo feature/login
    tools/bridge.py select-item QComboBox:0 '#1'
    tools/bridge.py type CommitMessageEdit:0 'subject\\n\\nbody'
    tools/bridge.py quit

The socket lives at $TMPDIR/<name> (QLocalServer's default placement
on macOS); <name> defaults to "gitbolt-test-bridge" or the value of
GITBOLT_TEST_BRIDGE when it isn't "1". One JSON object per response
line is printed to stdout; exits non-zero when the bridge reports
{"ok": false}.
"""

import json
import os
import socket
import sys
import tempfile


def socket_path() -> str:
    name = os.environ.get("GITBOLT_TEST_BRIDGE", "")
    if not name or name == "1":
        name = "gitbolt-test-bridge"
    if name.startswith("/"):
        return name
    # QLocalServer puts non-path names in QDir::tempPath(), which on
    # macOS is $TMPDIR (per-user, e.g. /var/folders/.../T/).
    return os.path.join(
        os.environ.get("TMPDIR", tempfile.gettempdir()), name)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__.strip())
        return 2

    command = " ".join(sys.argv[1:])
    path = socket_path()

    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(10)
            sock.connect(path)
            sock.sendall(command.encode() + b"\n")
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = sock.recv(65536)
                if not chunk:
                    break
                buf += chunk
    except FileNotFoundError:
        print(f"bridge socket not found at {path} — is GitBolt "
              "running with GITBOLT_TEST_BRIDGE=1?", file=sys.stderr)
        return 1
    except (ConnectionRefusedError, TimeoutError) as e:
        print(f"bridge connection failed: {e}", file=sys.stderr)
        return 1

    text = buf.decode().strip()
    print(text)
    try:
        return 0 if json.loads(text).get("ok") else 1
    except json.JSONDecodeError:
        return 1


if __name__ == "__main__":
    sys.exit(main())
