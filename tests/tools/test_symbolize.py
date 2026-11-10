#!/usr/bin/env python3
"""tools/symbolize.py reads GitBolt's crash reports and symbol files.

Offline: the crash.log fixtures (which frame is a fault's crash point,
and which are return addresses, looked up one byte back), a GlitchTip
event as sent and as GlitchTip's API gives it, the command that fetches
a release's symbols, and the build ID, base and entry point read from
synthetic Mach-O, ELF and PE headers (so the Windows path is tested off
Windows too).

With the crash helper (POSIX): its real crash.log's Build ID, which the
handler reads from memory, is the one the tool reads from the file;
symbolizing names faultNow at the crash point; and --check passes for
the helper's own symbols and fails for another binary's. Parts skip
where atos, llvm-symbolizer, addr2line, dsymutil or objcopy is missing,
or where the build has no line tables.

Stdlib only, and no __pycache__ in the source tree.
Usage: test_symbolize.py [<source dir> [<crash helper>]]
"""

import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True

# The source tree and the crash helper: the arguments ctest passes.
# Only a directory, then a file, count, so unittest's own arguments
# don't stand in for them.
ROOT = os.path.abspath(sys.argv.pop(1) if __name__ == "__main__" and len(sys.argv) > 1
                       and os.path.isdir(sys.argv[1]) else
                       os.path.join(os.path.dirname(__file__), "..", ".."))
HELPER = (os.path.abspath(sys.argv.pop(1)) if __name__ == "__main__" and len(sys.argv) > 1
          and os.path.isfile(sys.argv[1]) else "")
SCRIPT = os.path.join(ROOT, "tools", "symbolize.py")
FIXTURES = os.path.join(ROOT, "tests", "fixtures", "crash")


def load_tool():
    spec = importlib.util.spec_from_file_location("symbolize", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules["symbolize"] = module   # dataclasses look their module up
    spec.loader.exec_module(module)
    return module


sym = load_tool()


def fixture(name):
    return os.path.join(FIXTURES, name)


def cli(*args):
    return subprocess.run([sys.executable, "-B", SCRIPT, *args], capture_output=True,
                          text=True, check=False)


# ---- synthetic binaries ------------------------------------------------------

def macho(uuid, entryoff, line_tables=True):
    segment = "<2I16s4Q4I"
    text = struct.pack(segment, 0x19, 72, b"__TEXT", 0x100000000, 0x4000, 0, 0x4000, 5, 5, 0, 0)
    section = struct.pack("<16s16s2Q8I", b"__debug_line" if line_tables else b"__debug_info",
                          b"__DWARF", 0x100004000, 0x100, 0x1000, 0, 0, 0, 0, 0, 0, 0)
    dwarf = struct.pack(segment, 0x19, 72 + 80, b"__DWARF", 0x100004000, 0x1000, 0x1000,
                        0x1000, 7, 3, 1, 0) + section
    commands = (text + dwarf + struct.pack("<2I", 0x1B, 24) + uuid
                + struct.pack("<2I2Q", 0x80000028, 24, entryoff, 0))
    return struct.pack("<8I", 0xFEEDFACF, 0x0100000C, 0, 2, 4, len(commands), 0, 0) + commands


def elf(build_id, e_type=3, load_vaddr=0, entry=0x1040, debug=True):
    note = struct.pack("<3I", 4, len(build_id), 3) + b"GNU\0" + build_id
    names = b"\0.note.gnu.build-id\0.debug_line\0.shstrtab\0"
    phoff, note_at = 64, 64 + 2 * 56
    debug_at = note_at + len(note)
    names_at = debug_at + 16
    shoff = (names_at + len(names) + 7) // 8 * 8
    shdr = "<2I4Q2I2Q"
    sections = [struct.pack(shdr, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
                struct.pack(shdr, 1, 7, 2, note_at, note_at, len(note), 0, 0, 4, 0)]
    if debug:
        sections.append(struct.pack(shdr, 20, 1, 0, 0, debug_at, 16, 0, 0, 1, 0))
    sections.append(struct.pack(shdr, 32, 3, 0, 0, names_at, len(names), 0, 0, 1, 0))
    header = (b"\x7fELF\x02\x01\x01" + bytes(9)
              + struct.pack("<2HI3QI6H", e_type, 62, 1, entry, phoff, shoff, 0, 64, 56, 2, 64,
                            len(sections), len(sections) - 1))
    phdrs = (struct.pack("<2I6Q", 1, 5, 0, load_vaddr, load_vaddr, 0x2000, 0x2000, 0x1000)
             + struct.pack("<2I6Q", 4, 4, note_at, load_vaddr + note_at, 0, len(note),
                           len(note), 4))
    body = header + phdrs + note + bytes(16) + names
    return body + bytes(shoff - len(body)) + b"".join(sections)


GUID = (struct.pack("<I2H", 0x3F2A9C1D, 0x5E6B, 0x47A8) + bytes.fromhex("b9c0d1e2f3a4b5c6"))


def pe(entry_rva=0x1234, age=1, path=b"D:\\a\\GitBolt\\build\\src\\app\\GitBolt.pdb"):
    lfanew = 0x80
    dos = b"MZ" + bytes(0x3A) + struct.pack("<I", lfanew) + bytes(lfanew - 0x40)
    optional = bytearray(240)
    struct.pack_into("<H", optional, 0, 0x20B)
    struct.pack_into("<I", optional, 16, entry_rva)
    struct.pack_into("<Q", optional, 24, 0x140000000)
    struct.pack_into("<I", optional, 108, 16)
    struct.pack_into("<2I", optional, 112 + 6 * 8, 0x2000, 28)
    headers = (dos + b"PE\0\0" + struct.pack("<2H3I2H", 0x8664, 1, 0, 0, 0, 240, 0x22)
               + bytes(optional)
               + struct.pack("<8s6I2HI", b".rdata", 0x1000, 0x2000, 0x200, 0x400, 0, 0, 0, 0,
                             0x40000040))
    record = b"RSDS" + GUID + struct.pack("<I", age) + path + b"\0"
    debug = struct.pack("<2I2H4I", 0, 0, 0, 0, 2, len(record), 0x2040, 0x440)
    return headers + bytes(0x400 - len(headers)) + debug + bytes(0x40 - 28) + record


class Reports(unittest.TestCase):
    def crash_point(self, report):
        return [(f.index, f.module, f.offset) for f in report.frames if f.exact]

    def test_macos_report(self):
        report = sym.load_report(fixture("macos.log"))
        self.assertEqual((report.version, report.os, report.arch, report.signal, report.build_id),
                         ("0.1.5", "macOS", "arm64", "SIGSEGV", ""))
        # #2 is the interrupted pc; the others are return addresses.
        self.assertEqual(self.crash_point(report), [(2, "GitBolt", 0x5C3F0)])
        binary = sym.Binary(path="GitBolt", kind="macho", base=0x100000000)
        ours = {f.index: sym.address_of(f, binary) for f in report.frames if sym.is_gitbolt(f)}
        self.assertEqual(ours, {0: 0x10001A2B3, 2: 0x10005C3F0, 3: 0x10005B11F, 14: 0x100003E0F})

    def test_linux_report(self):
        report = sym.load_report(fixture("linux.log"))
        self.assertEqual((report.os, report.signal), ("Linux", "SIGABRT"))
        self.assertEqual(self.crash_point(report), [(2, "libc.so.6", 0x9EB2C)])
        # A frame written with its path is GitBolt's all the same.
        self.assertEqual([f.index for f in report.frames if sym.is_gitbolt(f)], [0, 5, 8])

    def test_windows_reports(self):
        # 0.9.x: the fault's frame comes after KiUserExceptionDispatcher.
        old = sym.load_report(fixture("windows.log"))
        self.assertEqual(self.crash_point(old), [(6, "GitBolt.exe", 0x51A40)])
        # An uncaught C++ exception: every frame is a return address.
        self.assertEqual(self.crash_point(sym.load_report(fixture("windows-exception.log"))), [])
        # From the context: #0 is the faulting instruction, and the
        # module list after the frames isn't read as frames.
        new = sym.load_report(fixture("windows-context.log"))
        self.assertEqual((new.os, new.signal, new.build_id),
                         ("Windows", "SIGSEGV", "3f2a9c1d5e6b47a8b9c0d1e2f3a4b5c61"))
        self.assertEqual(len(new.frames), 8)
        self.assertEqual(self.crash_point(new), [(0, "GitBolt.exe", 0x51A40)])
        binary = sym.Binary(path="GitBolt.exe", kind="pe")
        self.assertEqual([sym.address_of(f, binary) for f in new.frames if sym.is_gitbolt(f)],
                         [0x51A40, 0x52B0F, 0x3E0F])

    def test_windows_context_cxx_exception_has_no_exact_frame(self):
        with open(fixture("windows-context.log"), encoding="utf-8") as fh:
            text = fh.read().replace("Signal: SIGSEGV (11)", "Signal: SIGABRT (22)")
        self.assertEqual(self.crash_point(sym.parse_crash_log(text)), [])

    def test_pasted_report(self):
        # As pasted into an issue: indented, CRLF.
        with open(fixture("macos.log"), encoding="utf-8") as fh:
            text = "Here it is:\r\n" + "".join("    " + line + "\r\n" for line in fh.read().splitlines())
        self.assertEqual(self.crash_point(sym.parse_crash_log(text)), [(2, "GitBolt", 0x5C3F0)])
        with self.assertRaises(sym.Error):
            sym.parse_crash_log("hello")

    def test_glitchtip_event_both_shapes(self):
        with open(fixture("glitchtip-event.json"), encoding="utf-8") as fh:
            event = json.load(fh)
        exception = event["exception"]["values"][0]
        api = {
            "release": {"version": event["release"]},
            "contexts": event["contexts"],
            "tags": [{"key": k, "value": v} for k, v in event["tags"].items()],
            "entries": [
                {"type": "message", "data": {}},
                {"type": "exception", "data": {"values": [{
                    "type": exception["type"], "value": exception["value"],
                    "stacktrace": {"frames": [
                        {k: v for k, v in (("package", f.get("package")),
                                           ("function", f.get("function")),
                                           ("instructionAddr", f.get("instruction_addr")),
                                           ("imageAddr", f.get("image_addr")),
                                           ("inApp", f.get("in_app"))) if v is not None}
                        for f in exception["stacktrace"]["frames"]]}}]}},
            ],
        }
        for shape in (event, api):
            report = sym.parse_event(shape)
            self.assertEqual((report.version, report.os, report.arch, report.signal,
                              report.build_id),
                             ("0.1.5", "macOS", "arm64", "SIGSEGV",
                              "ec89b33079773318a1942396eef8118b"))
            # Newest first; the newest is the crash point.
            self.assertEqual(self.crash_point(report), [(0, "GitBolt", 0x5C3F0)])
            self.assertEqual([(f.module, f.offset, f.name) for f in report.frames][:4],
                             [("GitBolt", 0x5C3F0, ""), ("GitBolt", 0x5B120, ""),
                              ("QtCore", 0x2D1A40, "QMetaObject::activate"), ("", None, "")])
            self.assertEqual(sum(sym.is_gitbolt(f) for f in report.frames), 3)

    def test_glitchtip_windows_abort_has_no_exact_frame(self):
        report = sym.parse_event({
            "release": "gitbolt@0.1.5", "contexts": {"os": {"name": "Windows"}},
            "exception": {"values": [{"type": "SIGABRT", "stacktrace": {"frames": [
                {"package": "GitBolt.exe", "instruction_addr": "0x400"}]}}]}})
        self.assertFalse(report.frames[0].exact)

    def test_fetch_command(self):
        for name, platform in (("macos.log", "Darwin-arm64"), ("linux.log", "Linux-x86_64"),
                               ("windows.log", "Windows-AMD64")):
            report = sym.load_report(fixture(name))
            self.assertEqual(sym.fetch_command(report),
                             "gh release download v0.1.5 --repo Inverian/GitBolt -p "
                             f"'GitBolt-0.1.5-{platform}-symbols.zip'")
        out = cli(fixture("windows-context.log"))
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("GitBolt-0.1.5-Windows-AMD64-symbols.zip", out.stdout)


class Binaries(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name, data):
        path = os.path.join(self.tmp.name, name)
        with open(path, "wb") as fh:
            fh.write(data)
        return path

    def test_macho(self):
        uuid = bytes.fromhex("ec89b33079773318a1942396eef8118b")
        binary = sym.read_binary(self.write("GitBolt", macho(uuid, 0x3E10)))
        self.assertEqual((binary.kind, binary.build_id, binary.base, binary.entry, binary.arch),
                         ("macho", uuid.hex(), 0x100000000, 0x100003E10, "arm64"))
        self.assertTrue(binary.line_tables and binary.debug_sections)
        self.assertFalse(sym.read_binary(self.write("other", macho(uuid, 0, False))).line_tables)
        out = cli("--id", os.path.join(self.tmp.name, "GitBolt"))
        self.assertEqual(out.stdout.strip(), uuid.hex())

    def test_dsym_folder(self):
        uuid = bytes(range(16))
        dwarf = os.path.join(self.tmp.name, "GitBolt.dSYM", "Contents", "Resources", "DWARF")
        os.makedirs(dwarf)
        with open(os.path.join(dwarf, "GitBolt"), "wb") as fh:
            fh.write(macho(uuid, 0x10))
        self.assertEqual(sym.read_binary(os.path.join(self.tmp.name, "GitBolt.dSYM")).build_id,
                         uuid.hex())
        with tempfile.TemporaryDirectory() as scratch:
            self.assertEqual(sym.find_symbols(self.tmp.name, scratch).build_id, uuid.hex())

    def test_elf(self):
        build_id = bytes(range(20))
        pie = sym.read_binary(self.write("gitbolt.debug", elf(build_id)))
        self.assertEqual((pie.kind, pie.build_id, pie.base, pie.entry, pie.arch),
                         ("elf", build_id.hex(), 0, 0x1040, "x86_64"))
        self.assertTrue(pie.line_tables and pie.debug_sections)
        fixed = sym.read_binary(self.write("gitbolt", elf(build_id, e_type=2, load_vaddr=0x400000,
                                                          entry=0x401040, debug=False)))
        self.assertEqual((fixed.base, fixed.entry), (0x400000, 0x401040))
        self.assertFalse(fixed.line_tables or fixed.debug_sections)

    def test_pe(self):
        path = self.write("GitBolt.exe", pe())
        binary = sym.read_binary(path)
        # The symbol server's key, lower-case: GUID, then the age.
        self.assertEqual((binary.kind, binary.build_id, binary.base, binary.entry, binary.arch),
                         ("pe", "3f2a9c1d5e6b47a8b9c0d1e2f3a4b5c61", 0, 0x1234, "x86_64"))
        self.assertFalse(binary.line_tables)   # no PDB beside it
        self.write("GitBolt.pdb", b"Microsoft C/C++ MSF 7.00\r\n")
        binary = sym.read_binary(path)
        self.assertEqual(os.path.basename(binary.pdb), "GitBolt.pdb")
        self.assertTrue(binary.line_tables)
        self.assertEqual(sym.read_binary(self.write("aged.exe", pe(age=0x2A))).build_id[-2:],
                         "2a")
        with tempfile.TemporaryDirectory() as scratch:
            self.assertEqual(sym.find_symbols(self.tmp.name, scratch).path, path)

    def test_not_a_binary(self):
        with self.assertRaises(sym.Error):
            sym.read_binary(self.write("notes.txt", b"hello there"))


@unittest.skipIf(not HELPER or os.name == "nt", "needs the crash helper (POSIX)")
class RoundTrip(unittest.TestCase):
    """The crash helper's real crash.log, read back with its own symbols."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        home = os.path.join(cls.tmp.name, "home")
        os.makedirs(home)
        env = dict(os.environ, HOME=home)
        env.pop("GITBOLT_CRASH_HELPER_DIR", None)
        subprocess.run([HELPER, "fault"], env=env, capture_output=True, timeout=60, check=False)
        cls.log = os.path.join(home, ".gitbolt", "crash.log")
        cls.report = sym.load_report(cls.log)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_build_id_is_the_files(self):
        self.assertTrue(self.report.build_id)
        self.assertEqual(sym.read_binary(HELPER).build_id, self.report.build_id)
        self.assertEqual(cli("--id", HELPER).stdout.strip(), self.report.build_id)

    def test_names_the_crash_point(self):
        try:
            sym.pick_tool("auto", sym.read_binary(HELPER))
        except sym.Error as error:
            self.skipTest(str(error))
        out = cli(self.log, HELPER)
        self.assertEqual(out.returncode, 0, out.stderr)
        crash = [line for line in out.stdout.splitlines() if line.endswith("<- crash point")]
        self.assertEqual(len(crash), 1, out.stdout)
        self.assertIn("faultNow", crash[0], out.stdout)

    def test_another_binarys_symbols_are_refused(self):
        other = os.path.join(self.tmp.name, "other")
        with open(other, "wb") as fh:
            fh.write(macho(bytes(16), 0x10) if sys.platform == "darwin" else elf(bytes(20)))
        out = cli(self.log, other)
        self.assertEqual(out.returncode, 1)
        self.assertIn("build ID mismatch", out.stderr)

    def test_check(self):
        symbols = os.path.join(self.tmp.name, "symbols")
        os.makedirs(symbols, exist_ok=True)
        shipped = HELPER
        if sys.platform == "darwin":
            if not shutil.which("dsymutil"):
                self.skipTest("no dsymutil")
            dsym = os.path.join(symbols, "gitbolt.dSYM")
            subprocess.run(["dsymutil", HELPER, "-o", dsym], capture_output=True, check=True)
            symbol_file = dsym
        else:
            if not shutil.which("objcopy"):
                self.skipTest("no objcopy")
            symbol_file = os.path.join(symbols, "gitbolt.debug")
            shipped = os.path.join(self.tmp.name, "gitbolt")
            subprocess.run(["objcopy", "--only-keep-debug", HELPER, symbol_file], check=True)
            subprocess.run(["objcopy", "--strip-debug", HELPER, shipped], check=True)
        if not sym.read_binary(symbol_file).line_tables:
            self.skipTest("a build without line tables")
        try:
            sym.pick_tool("auto", sym.read_binary(symbol_file))
        except sym.Error as error:
            self.skipTest(str(error))
        out = cli("--check", symbol_file, shipped)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("symbols match", out.stdout)

        other = os.path.join(self.tmp.name, "other-shipped")
        with open(other, "wb") as fh:
            fh.write(macho(bytes(16), 0x10) if sys.platform == "darwin" else elf(bytes(20)))
        out = cli("--check", symbol_file, other)
        self.assertEqual(out.returncode, 1)
        self.assertIn("build IDs differ", out.stderr)


if __name__ == "__main__":
    unittest.main()
