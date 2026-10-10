#!/usr/bin/env python3
"""List function starts in NFSC.exe that the translation does not carry.

Ghidra misses functions that are only reached through pointers built at
runtime (vtables filled in by constructors, callback tables), and the
translator only adds an address on its own when it is 16-byte aligned or
something in the image points at it. Every "call to unknown target" so far
has been one of these: code that starts right after the previous function's
RET / RET n / INT3 padding, with an ordinary first instruction.

This scans the code section for exactly that shape and prints the addresses
the build does not already have, ready for game.toml's entry_points.

    python tools/find_missed_functions.py            # summary + list
    python tools/find_missed_functions.py --toml     # just the toml list
"""
import argparse, re, struct, sys
from pathlib import Path

import capstone

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "original" / "NFSC.exe"
LISTINGS = ROOT / "analysis" / "decompiled" / "NFSC.exe" / "functions"
TABLE = ROOT / "build" / "recomp" / "gen" / "table.c"

# Addresses already found by hand from run logs; the scan must report all of them.
KNOWN_MISSES = [0x0089f604, 0x0089f07c, 0x0089f494, 0x008999ee, 0x0054bac0,
                0x0088510e, 0x00884a1d, 0x0059d6d0, 0x0089777e]

# First instructions MSVC 7.1 functions (and the hand-written MMX code) start with.
PROLOGUE_MNEMONICS = {"push", "mov", "sub", "xor", "lea", "fld", "fldz", "movq", "pxor", "cmp", "test"}


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    n = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    out = []
    for i in range(n):
        name, vsize, va, rsize, raw, _, _, _, _, chars = struct.unpack_from(
            "<8sIIIIIIHHI", data, pe + 24 + opt + 40 * i)
        out.append((name.rstrip(b"\0").decode(), base + va, vsize, raw, rsize, chars))
    return base, out


def load_listings():
    """Ghidra's instructions: start address -> next start in the same function."""
    starts, spans, branch_targets = set(), [], set()
    tgt = re.compile(r"\b(J[A-Z]+|CALL|LOOP[A-Z]*)\s+0x([0-9a-f]+)\b")
    for f in LISTINGS.glob("*.asm"):
        prev = None
        for line in f.read_text(errors="replace").splitlines():
            parts = line.split(None, 1)
            if not parts or len(parts[0]) != 8:
                continue
            try:
                a = int(parts[0], 16)
            except ValueError:
                continue
            starts.add(a)
            if prev is not None and a > prev:
                spans.append((prev, a))
            prev = a
            m = tgt.search(line)
            if m:
                branch_targets.add(int(m.group(2), 16))
    return starts, spans, branch_targets


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--toml", action="store_true", help="print only the entry_points list")
    args = ap.parse_args()

    data = EXE.read_bytes()
    base, secs = sections(data)
    text = [s for s in secs if s[5] & 0x20000000]  # IMAGE_SCN_MEM_EXECUTE
    have = {int(h, 16) for h in re.findall(r"fn_([0-9a-f]{8})", TABLE.read_text())}
    starts, spans, branch_targets = load_listings()

    # Interior of a Ghidra instruction (not its first byte) is never a start.
    spans.sort()
    import bisect
    span_lo = [s[0] for s in spans]

    def interior(a):
        i = bisect.bisect_right(span_lo, a) - 1
        if i < 0:
            return False
        lo, hi = spans[i]
        # spans are start..next-start; instruction length is at most 15
        return lo < a < hi and a - lo < 16 and a not in starts

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def decodes_cleanly(a, off_of):
        """Linear decode until RET/JMP; reject int3/bad/privileged noise."""
        o = off_of(a)
        n = 0
        for ins in md.disasm(data[o:o + 4096], a):
            n += 1
            if ins.mnemonic in ("int3", "hlt", "in", "out", "insb", "insd", "outsb", "outsd",
                                "iretd", "retf", "arpl", "into", "bound", "les", "lds"):
                return False
            if ins.mnemonic in ("ret", "jmp"):
                return n >= 1
            if n > 2000:
                return True
        return False  # ran into undecodable bytes

    found = []
    for name, va, vsize, raw, rsize, _ in text:
        size = min(vsize, rsize)
        off_of = lambda a, va=va, raw=raw: a - va + raw
        for a in range(va + 1, va + size - 1):
            if a in have and a not in KNOWN_MISSES:
                continue
            o = off_of(a)
            b1 = data[o - 1]
            # RET, INT3, RET n, or a JMP (E9 rel32, EB rel8, FF /4 indirect)
            after_end = (b1 in (0xC3, 0xCC) or data[o - 3] in (0xC2, 0xFF)
                         or data[o - 5] == 0xE9 or data[o - 2] in (0xEB, 0xFF)
                         or data[o - 6] == 0xFF)
            if not after_end:
                continue
            if data[o] == 0xCC or data[o] == 0x00:
                continue
            if interior(a) or a in branch_targets:
                continue
            ins = next(md.disasm(data[o:o + 16], a, count=1), None)
            if ins is None or ins.mnemonic not in PROLOGUE_MNEMONICS:
                continue
            # A C3/CC/E9 byte that is really the tail of an earlier instruction:
            # make sure the bytes before decode to a RET, INT3 or JMP ending at a.
            ok = False
            for back in (1, 2, 3, 5, 6, 7):
                p = list(md.disasm(data[o - back:o], a - back, count=1))
                if p and p[0].size == back and p[0].mnemonic in ("ret", "int3", "jmp"):
                    ok = True
            if not ok or not decodes_cleanly(a, off_of):
                continue
            found.append(a)

    missing_known = [k for k in KNOWN_MISSES if k not in found]
    found = [a for a in found if a not in have]
    if args.toml:
        print("entry_points_scan = [" + ", ".join(f"0x{a:08x}" for a in found) + "]")
        return
    print(f"{len(found)} candidate function starts not in the build")
    print("hand-found misses also found by the scan:",
          len(KNOWN_MISSES) - len(missing_known), "/", len(KNOWN_MISSES),
          "" if not missing_known else f"MISSED: {[hex(k) for k in missing_known]}")
    for a in found:
        o = a - next(s[1] for s in text if s[1] <= a < s[1] + s[2]) + next(s[3] for s in text if s[1] <= a < s[1] + s[2])
        print(f"0x{a:08x}  {data[o:o + 8].hex()}")


if __name__ == "__main__":
    main()
