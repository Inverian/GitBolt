#!/usr/bin/env python3
"""Read a GitBolt crash report's own frames with a release's symbol files.

Usage:
  symbolize.py REPORT [SYMBOLS]
      REPORT: a crash.log (as the Crash Detected box or an issue shows
      it) or a GlitchTip event's JSON (as sent, or from GlitchTip's
      API). SYMBOLS: the release's GitBolt-<version>-<platform>-
      symbols.zip, a folder with its files, or a binary. Without
      SYMBOLS, prints the command that fetches the zip.
  symbolize.py --id FILE
      FILE's build ID, as a report's "Build ID:" line writes it.
  symbolize.py --check SYMBOLS SHIPPED
      For CI: SYMBOLS belong to SHIPPED (same build ID), have line
      tables, and resolve SHIPPED's entry point; a shipped ELF has no
      .debug_* sections. Exits 1, saying why, otherwise.

Options:
  --tool auto|llvm-symbolizer|atos|addr2line   (default auto: llvm-
      symbolizer if found, else atos for Mach-O, addr2line for ELF;
      Windows reports need llvm-symbolizer)
  --force   symbolize even when the build IDs differ

Only GitBolt's own frames are looked up: the report's offset plus the
image's base, less 1 for return addresses (every frame but the
crash point of a fault). Other modules' frames keep the report's
names. Stdlib only; Python 3.9 or newer.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import dataclass, field

REPO = "Inverian/GitBolt"
GITBOLT = re.compile(r"^(gitbolt|gitbolt\.exe)$", re.IGNORECASE)
DISPATCH = re.compile(
    r"^(ntdll|kernelbase|kernel32|ucrtbase|vcruntime140[_a-z0-9]*)\.dll$", re.IGNORECASE)
FAULTS = ("SIGSEGV", "SIGFPE", "SIGBUS")


class Error(Exception):
    """A problem to report on stderr, exit status 1."""


# ---- reports ----------------------------------------------------------------

@dataclass
class Frame:
    index: int          # newest first
    module: str         # file name; "" when the report has none
    offset: int | None  # from the module's base
    name: str           # the report's function name, or ""
    exact: bool = False  # the crash point of a fault: no -1


@dataclass
class Report:
    version: str = ""
    os: str = ""        # macOS, Windows or Linux
    arch: str = ""
    build_id: str = ""
    signal: str = ""
    frames: list = field(default_factory=list)


def module_name(module: str) -> str:
    return re.split(r"[/\\]", module)[-1]


def is_gitbolt(frame: Frame) -> bool:
    return bool(GITBOLT.match(module_name(frame.module))) and frame.offset is not None


def os_family(name: str) -> str:
    name = name.lower()
    if name in ("macos", "osx"):
        return "macOS"
    if name == "windows":
        return "Windows"
    return "Linux"


FRAME = re.compile(r"^#(\d+) (\S+) \+0x([0-9a-fA-F]+) (.*)$")
NO_MODULE = re.compile(r"^#(\d+) \? 0x([0-9a-fA-F]+)$")
SYMBOL = re.compile(r"^(.+)\+0x[0-9a-fA-F]+$")


def parse_crash_log(text: str) -> Report:
    lines = [line.strip() for line in text.replace("\r\n", "\n").split("\n")]
    try:
        i = lines.index("=== GitBolt Crash Report ===")
    except ValueError:
        raise Error("not a GitBolt crash report") from None
    header = {}
    for i in range(i + 1, len(lines)):
        if lines[i] == "--- Stack Trace ---":
            break
        key, sep, value = lines[i].partition(": ")
        if sep:
            header.setdefault(key, value)
    else:
        raise Error("the report has no stack trace")
    report = Report(version=header.get("App version", ""),
                    os=os_family(header.get("OS", "").split(" ")[0]),
                    arch=header.get("CPU Arch", ""),
                    build_id=header.get("Build ID", "").lower(),
                    signal=header.get("Signal", "").split(" ")[0])
    for line in lines[i + 1:]:
        if line in ("=== End of Report ===", "--- Modules ---"):
            break
        m = FRAME.match(line)
        if m:
            symbol = SYMBOL.match(m.group(4))
            report.frames.append(Frame(int(m.group(1)), m.group(2), int(m.group(3), 16),
                                       symbol.group(1) if symbol else ""))
        elif NO_MODULE.match(line):
            report.frames.append(Frame(int(NO_MODULE.match(line).group(1)), "", None, ""))
    mark_crash_point(report, header.get("Stack") == "exception")
    return report


def mark_crash_point(report: Report, from_context: bool) -> None:
    """Which frame is the exact pc rather than a return address:
    POSIX #2 (the interrupted pc); Windows from the context, #0 of a
    fault; older Windows reports, the frame after the dispatch."""
    frames = report.frames
    fault = report.signal in FAULTS
    if report.os != "Windows":
        for frame in frames:
            frame.exact = frame.index == 2
        return
    if not fault or not frames:
        return
    if from_context:
        frames[0].exact = frames[0].index == 0
        return
    dispatcher = [n for n, f in enumerate(frames) if f.name == "KiUserExceptionDispatcher"]
    if dispatcher:
        at = dispatcher[-1] + 1
    else:
        at = 1
        while at < len(frames) and DISPATCH.match(module_name(frames[at].module)):
            at += 1
    if at < len(frames):
        frames[at].exact = True


def parse_event(event: dict) -> Report:
    """A GlitchTip event: as sent (exception.values, tags a dict) or
    from the API (entries[type=exception], tags a list, camelCase)."""
    values = (event.get("exception") or {}).get("values")
    for entry in event.get("entries") or []:
        if values is None and entry.get("type") == "exception":
            values = (entry.get("data") or {}).get("values")
    if not values:
        raise Error("no exception in the event")
    exception = values[-1]
    tags = event.get("tags") or {}
    if isinstance(tags, list):
        tags = dict((t.get("key"), t.get("value")) if isinstance(t, dict) else tuple(t)
                    for t in tags)
    release = event.get("release") or ""
    if isinstance(release, dict):
        release = release.get("version") or release.get("shortVersion") or ""
    contexts = event.get("contexts") or {}
    report = Report(version=release.split("@", 1)[-1],
                    os=os_family((contexts.get("os") or {}).get("name", "")),
                    arch=(contexts.get("device") or {}).get("arch", ""),
                    build_id=str(tags.get("build_id") or "").lower(),
                    signal=exception.get("type", ""))
    raw = ((exception.get("stacktrace") or {}).get("frames")) or []
    for n, frame in enumerate(reversed(raw)):   # newest first, as crash.log
        address = frame.get("instruction_addr", frame.get("instructionAddr"))
        name = frame.get("function") or ""
        module = frame.get("package") or ""
        report.frames.append(Frame(n, "" if module == "<other>" else module,
                                   int(address, 16) if address else None,
                                   "" if name == "<unknown>" else name))
    if report.frames:
        report.frames[0].exact = report.os != "Windows" or report.signal in FAULTS
    return report


def load_report(path: str) -> Report:
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    if text.lstrip().startswith("{"):
        return parse_event(json.loads(text))
    return parse_crash_log(text)


def symbols_zip_name(report: Report) -> str:
    arch = report.arch or "x86_64"
    platform = {"macOS": f"Darwin-{arch}",
                "Windows": "Windows-" + ("ARM64" if arch == "arm64" else "AMD64"),
                }.get(report.os, f"Linux-{arch}")
    return f"GitBolt-{report.version}-{platform}-symbols.zip"


def fetch_command(report: Report) -> str:
    return (f"gh release download v{report.version} --repo {REPO} "
            f"-p '{symbols_zip_name(report)}'")


# ---- binaries ---------------------------------------------------------------

@dataclass
class Binary:
    path: str
    kind: str               # macho, elf or pe
    build_id: str = ""
    base: int = 0           # what the report's offsets are added to
    entry: int | None = None
    arch: str = ""
    line_tables: bool = False
    debug_sections: bool = False
    pdb: str = ""           # PE: the PDB beside it, if any


def read_at(fh, offset: int, size: int) -> bytes:
    fh.seek(offset)
    data = fh.read(size)
    if len(data) != size:
        raise Error(f"{fh.name}: truncated")
    return data


def cstr(data: bytes) -> str:
    return data.split(b"\0", 1)[0].decode("latin-1")


def read_macho(fh, binary: Binary) -> None:
    _, cputype, _, _, ncmds, sizeofcmds = struct.unpack_from("<6I", read_at(fh, 0, 32))
    binary.arch = {0x0100000C: "arm64", 0x01000007: "x86_64"}.get(cputype, hex(cputype))
    commands = read_at(fh, 32, sizeofcmds)
    at = 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<2I", commands, at)
        if cmd == 0x1B:                                  # LC_UUID
            binary.build_id = commands[at + 8:at + 24].hex()
        elif cmd == 0x19:                                # LC_SEGMENT_64
            segment = cstr(commands[at + 8:at + 24])
            vmaddr = struct.unpack_from("<Q", commands, at + 24)[0]
            nsects = struct.unpack_from("<I", commands, at + 64)[0]
            if segment == "__TEXT":
                binary.base = vmaddr
            for s in range(nsects):
                name = cstr(commands[at + 72 + 80 * s:at + 88 + 80 * s])
                if segment == "__DWARF":
                    binary.debug_sections = True
                    binary.line_tables |= name == "__debug_line"
        elif cmd == 0x80000028:                          # LC_MAIN
            binary.entry = struct.unpack_from("<Q", commands, at + 8)[0]
        at += size
    if binary.entry is not None:
        binary.entry += binary.base


def elf_notes(data: bytes, align: int):
    at = 0
    while at + 12 <= len(data):
        namesz, descsz, kind = struct.unpack_from("<3I", data, at)
        name = data[at + 12:at + 12 + namesz]
        desc_at = at + 12 + (namesz + align - 1) // align * align
        yield name, kind, data[desc_at:desc_at + descsz]
        at = desc_at + (descsz + align - 1) // align * align


def read_elf(fh, binary: Binary) -> None:
    header = read_at(fh, 0, 64)
    if header[4] != 2 or header[5] != 1:
        raise Error(f"{binary.path}: only little-endian ELF64 is read")
    e_type, machine = struct.unpack_from("<2H", header, 16)
    binary.entry, phoff, shoff = struct.unpack_from("<3Q", header, 24)
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<5H", header, 54)
    binary.arch = {62: "x86_64", 183: "aarch64"}.get(machine, str(machine))
    loads = []
    notes = []   # sections first: a .debug file's segments may be stale
    sections = [struct.unpack_from("<2I4Q2I2Q", read_at(fh, shoff + n * shentsize, 64))
                for n in range(shnum)]
    if sections and shstrndx < len(sections):
        strtab = read_at(fh, sections[shstrndx][4], sections[shstrndx][5])
        for name_at, sh_type, _, _, sh_offset, sh_size, _, _, sh_align, _ in sections:
            name = cstr(strtab[name_at:])
            if name.startswith(".debug_"):
                binary.debug_sections = True
                binary.line_tables |= name == ".debug_line" and sh_type != 8 and sh_size > 0
            if sh_type == 7 and sh_size:                 # SHT_NOTE
                notes.append((sh_offset, sh_size, sh_align))
    for n in range(phnum):
        p_type, _, p_offset, p_vaddr, _, p_filesz, _, p_align = struct.unpack_from(
            "<2I6Q", read_at(fh, phoff + n * phentsize, 56))
        if p_type == 1:                                  # PT_LOAD
            loads.append(p_vaddr)
        elif p_type == 4 and p_filesz:                   # PT_NOTE
            notes.append((p_offset, p_filesz, p_align))
    binary.base = 0 if e_type == 3 or not loads else min(loads)   # ET_DYN: PIE
    for offset, size, align in notes:
        try:
            data = read_at(fh, offset, size)
        except Error:
            continue
        for name, kind, desc in elf_notes(data, 8 if align == 8 else 4):
            if kind == 3 and name == b"GNU\0":           # NT_GNU_BUILD_ID
                binary.build_id = desc.hex()
                return


def read_pe(fh, binary: Binary) -> None:
    pe = struct.unpack_from("<I", read_at(fh, 0x3C, 4))[0]
    if read_at(fh, pe, 4) != b"PE\0\0":
        raise Error(f"{binary.path}: not a PE image")
    machine, nsections, _, _, _, optsize, _ = struct.unpack_from("<2H3I2H", read_at(fh, pe + 4, 20))
    optional = read_at(fh, pe + 24, optsize)
    if struct.unpack_from("<H", optional, 0)[0] != 0x20B:
        raise Error(f"{binary.path}: only PE32+ is read")
    binary.arch = {0x8664: "x86_64", 0xAA64: "arm64"}.get(machine, hex(machine))
    binary.entry = struct.unpack_from("<I", optional, 16)[0]   # an RVA, as the offsets
    count = struct.unpack_from("<I", optional, 108)[0]
    if count <= 6:
        return
    debug_rva, debug_size = struct.unpack_from("<2I", optional, 112 + 6 * 8)
    table = read_at(fh, pe + 24 + optsize, 40 * nsections)
    file_offset = None
    for s in range(nsections):
        vsize, va, rawsize, rawptr = struct.unpack_from("<4I", table, 40 * s + 8)
        if va <= debug_rva < va + max(vsize, rawsize):
            file_offset = rawptr + debug_rva - va
    if file_offset is None:
        return
    for n in range(debug_size // 28):
        _, _, _, _, kind, size, _, rawptr = struct.unpack_from(
            "<2I2H4I", read_at(fh, file_offset + 28 * n, 28))
        if kind != 2 or size < 24:                       # IMAGE_DEBUG_TYPE_CODEVIEW
            continue
        record = read_at(fh, rawptr, size)
        if record[:4] != b"RSDS":
            continue
        data1, data2, data3 = struct.unpack_from("<I2H", record, 4)
        age = struct.unpack_from("<I", record, 20)[0]
        binary.build_id = f"{data1:08x}{data2:04x}{data3:04x}{record[12:20].hex()}{age:x}"
        pdb = os.path.join(os.path.dirname(binary.path),
                           re.split(r"[/\\]", cstr(record[24:]))[-1])
        binary.pdb = pdb if os.path.isfile(pdb) else ""
        binary.line_tables = bool(binary.pdb) and os.path.getsize(binary.pdb) > 0
        return


def read_binary(path: str) -> Binary:
    if os.path.isdir(path):
        path = dsym_file(path) or path
    binary = Binary(path=path, kind="")
    with open(path, "rb") as fh:
        magic = fh.read(4)
        if magic == b"\xcf\xfa\xed\xfe":
            binary.kind = "macho"
            read_macho(fh, binary)
        elif magic == b"\x7fELF":
            binary.kind = "elf"
            read_elf(fh, binary)
        elif magic[:2] == b"MZ":
            binary.kind = "pe"
            read_pe(fh, binary)
        elif magic in (b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"):
            raise Error(f"{path}: a universal binary; pass one slice (lipo -thin)")
        else:
            raise Error(f"{path}: not a Mach-O, ELF or PE file")
    return binary


def dsym_file(path: str) -> str:
    found = sorted(glob.glob(os.path.join(glob.escape(path), "Contents", "Resources",
                                          "DWARF", "*")))
    return found[0] if found else ""


def find_symbols(path: str, scratch: str) -> Binary:
    """The symbol file in a release's symbols zip, a folder of its
    files, a .dSYM, or a binary given as it is."""
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            archive.extractall(scratch)
        path = scratch
    if os.path.isfile(path):
        return read_binary(path)
    if path.rstrip("/\\").endswith(".dSYM"):
        return read_binary(path)
    candidates = (glob.glob(os.path.join(glob.escape(path), "*.dSYM"))
                  + glob.glob(os.path.join(glob.escape(path), "*.debug"))
                  + [p for p in glob.glob(os.path.join(glob.escape(path), "*"))
                     if GITBOLT.match(os.path.basename(p))])
    for candidate in candidates:
        try:
            return read_binary(candidate)
        except (Error, OSError):
            continue
    raise Error(f"{path}: no symbol file (GitBolt.dSYM, gitbolt.debug or GitBolt.exe) in it")


# ---- symbolizers --------------------------------------------------------------

def find_llvm_symbolizer() -> str:
    found = shutil.which("llvm-symbolizer")
    if found:
        return found
    if shutil.which("xcrun"):
        try:
            out = subprocess.run(["xcrun", "--find", "llvm-symbolizer"], capture_output=True,
                                 text=True, check=False).stdout.strip()
        except OSError:
            out = ""
        if out and os.path.isfile(out):
            return out
    candidates = ["/opt/homebrew/opt/llvm/bin/llvm-symbolizer",
                  "/usr/local/opt/llvm/bin/llvm-symbolizer",
                  r"C:\Program Files\LLVM\bin\llvm-symbolizer.exe"]
    candidates += sorted(glob.glob("/usr/lib/llvm-*/bin/llvm-symbolizer"),
                         key=lambda p: [int(n) for n in re.findall(r"\d+", p)], reverse=True)
    return next((c for c in candidates if os.path.isfile(c)), "")


def pick_tool(choice: str, binary: Binary) -> tuple:
    """(tool name, its path), or Error naming what is missing."""
    if choice in ("auto", "llvm-symbolizer"):
        path = find_llvm_symbolizer()
        if path:
            return "llvm-symbolizer", path
        if choice != "auto":
            raise Error("llvm-symbolizer not found")
    if binary.kind == "pe":
        raise Error("Windows reports need llvm-symbolizer "
                    "(brew install llvm, or the LLVM installer)")
    if choice in ("auto", "atos") and binary.kind == "macho" and shutil.which("atos"):
        return "atos", shutil.which("atos")
    if choice in ("auto", "addr2line") and binary.kind == "elf" and shutil.which("addr2line"):
        return "addr2line", shutil.which("addr2line")
    raise Error(f"no symbolizer for {binary.kind} files here "
                "(llvm-symbolizer, atos for Mach-O, addr2line for ELF)")


def run(args: list) -> str:
    out = subprocess.run(args, capture_output=True, text=True, check=False)
    if out.returncode != 0:
        raise Error(f"{os.path.basename(args[0])} failed: {out.stderr.strip()}")
    return out.stdout


def lookup(tool: tuple, binary: Binary, addresses: list) -> list:
    """For each address, its frames, innermost first: (function, place)."""
    if not addresses:
        return []
    name, path = tool
    hexes = [hex(a) for a in addresses]
    if name == "llvm-symbolizer":
        args = [path, f"--obj={binary.path}", "--output-style=JSON", "--inlining", "--demangle"]
        if binary.kind == "pe":
            args.append("--relative-address")
        text = run(args + hexes).strip()
        try:
            parsed = json.loads(text)
            objects = parsed if isinstance(parsed, list) else [parsed]
        except ValueError:
            objects = []
            for line in text.splitlines():
                item = json.loads(line)
                objects += item if isinstance(item, list) else [item]
        result = []
        for obj in objects:
            frames = []
            for symbol in obj.get("Symbol") or []:
                function = symbol.get("FunctionName") or ""
                place = (f"{symbol.get('FileName')}:{symbol.get('Line')}"
                         if symbol.get("FileName") and symbol.get("Line") else "")
                if function and function != "??":
                    frames.append((function, place))
            result.append(frames)
        return result
    if name == "atos":
        out = run([path, "-o", binary.path, "-arch", binary.arch, "-l", hex(binary.base), "-i"]
                  + hexes)
        groups = [g for g in re.split(r"\n\s*\n", out.strip("\n"))]
        if len(groups) != len(addresses):
            groups = [run([path, "-o", binary.path, "-arch", binary.arch, "-l",
                           hex(binary.base), "-i", h]).strip() for h in hexes]
        result = []
        for group in groups:
            frames = []
            for line in group.splitlines():
                m = re.match(r"^(.*?) \(in [^)]*\)(?: \((.*:\d+)\))?", line.strip())
                if m:
                    frames.append((m.group(1), m.group(2) or ""))
            result.append(frames)
        return result
    # addr2line: "-a" starts each address's group with the address.
    out = run([path, "-a", "-f", "-C", "-i", "-e", binary.path] + hexes)
    result = []
    lines = out.splitlines()
    i = 0
    while i < len(lines):
        if lines[i].startswith("0x"):
            result.append([])
            i += 1
            continue
        function = lines[i].strip()
        place = lines[i + 1].strip() if i + 1 < len(lines) else ""
        if result and function != "??":
            result[-1].append((function, "" if place.startswith("??") else place))
        i += 2
    return result


def address_of(frame: Frame, binary: Binary) -> int:
    return binary.base + frame.offset - (0 if frame.exact else 1)


# ---- commands ---------------------------------------------------------------

def symbolize(report_path: str, symbols: str | None, tool_choice: str, force: bool) -> int:
    report = load_report(report_path)
    print(f"GitBolt {report.version or '?'}, {report.os} {report.arch}, {report.signal or '?'}"
          + (f", build {report.build_id}" if report.build_id else ""))
    if not symbols:
        print("The symbol files for it:")
        print(f"  {fetch_command(report)}")
        return 0
    with tempfile.TemporaryDirectory() as scratch:
        binary = find_symbols(symbols, scratch)
        if not report.build_id:
            print("warning: the report has no Build ID (0.9.x); trusting version, OS and "
                  "architecture", file=sys.stderr)
        elif report.build_id != binary.build_id:
            message = (f"build ID mismatch: the report's is {report.build_id}, "
                       f"{os.path.basename(binary.path)}'s {binary.build_id or 'none'}")
            if not force:
                raise Error(message + " (--force to symbolize anyway)")
            print(f"warning: {message}", file=sys.stderr)
        tool = pick_tool(tool_choice, binary)
        ours = [f for f in report.frames if is_gitbolt(f)]
        found = lookup(tool, binary, [address_of(f, binary) for f in ours])
        names = dict(zip((f.index for f in ours), found))
        for frame in report.frames:
            where = (f"{module_name(frame.module)} +0x{frame.offset:x}" if frame.module
                     and frame.offset is not None else "?")
            if frame.index in names:
                inlined = names[frame.index] or [("??", "")]
                text = "  ".join(p for p in inlined[0] if p)
                print(f"#{frame.index:<3} {where:<28} {text}"
                      + ("   <- crash point" if frame.exact else ""))
                for function, place in inlined[1:]:
                    print(f"{'':33} inlined into {function}  {place}".rstrip())
            else:
                print(f"#{frame.index:<3} {where:<28} {frame.name}".rstrip()
                      + ("   <- crash point" if frame.exact else ""))
    return 0


def check(symbols: str, shipped_path: str, tool_choice: str) -> int:
    problems = []
    with tempfile.TemporaryDirectory() as scratch:
        binary = find_symbols(symbols, scratch)
        shipped = read_binary(shipped_path)
        if not binary.build_id or binary.build_id != shipped.build_id:
            problems.append(f"build IDs differ: {os.path.basename(binary.path)} "
                            f"{binary.build_id or 'none'}, shipped {shipped.build_id or 'none'}")
        if not binary.line_tables:
            problems.append(f"{os.path.basename(binary.path)} has no line tables"
                            + (" (no PDB beside it)" if binary.kind == "pe" else ""))
        if shipped.kind == "elf" and shipped.debug_sections:
            problems.append("the shipped binary still has .debug_* sections")
        expected = {"macho": "main", "elf": "_start", "pe": "CRTStartup"}[shipped.kind]
        if shipped.entry is None:
            problems.append("the shipped binary has no entry point")
        else:
            try:
                found = lookup(pick_tool(tool_choice, binary), binary, [shipped.entry])
                name = found[0][-1][0] if found and found[0] else ""
                if not (name == expected or (expected == "CRTStartup" and name.endswith(expected))):
                    problems.append(f"the entry point resolves to {name or 'nothing'}, "
                                    f"not {expected}")
            except Error as error:
                problems.append(str(error))
    for problem in problems:
        print(f"symbolize --check: {problem}", file=sys.stderr)
    if not problems:
        print(f"symbols match {os.path.basename(shipped_path)} ({shipped.build_id})")
    return 1 if problems else 0


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(
        description="Read a GitBolt crash report's frames with a release's symbol files.")
    parser.add_argument("report", nargs="?", help="crash.log or GlitchTip event JSON")
    parser.add_argument("symbols", nargs="?", help="the release's -symbols.zip, a folder, "
                                                   "or a binary")
    parser.add_argument("--id", metavar="FILE", help="print FILE's build ID")
    parser.add_argument("--check", nargs=2, metavar=("SYMBOLS", "SHIPPED"))
    parser.add_argument("--tool", default="auto",
                        choices=["auto", "llvm-symbolizer", "atos", "addr2line"])
    parser.add_argument("--force", action="store_true",
                        help="symbolize even when the build IDs differ")
    args = parser.parse_args(argv)
    try:
        if args.id:
            print(read_binary(args.id).build_id or "(none)")
            return 0
        if args.check:
            return check(args.check[0], args.check[1], args.tool)
        if not args.report:
            parser.error("a report, --id or --check")
        return symbolize(args.report, args.symbols, args.tool, args.force)
    except (Error, OSError, ValueError, struct.error) as error:
        print(f"symbolize: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
