"""Offline harness for src/runtime_check.cpp: runs the same start-up checks on a readable AE executable.

Usage: python -I tools/runtime_check_harness.py <versionlib.bin> <SkyrimSE.exe>
  e.g. the Steam 1.7.104 install: Data/SKSE/Plugins/versionlib-1-7-104-0.bin and SkyrimSE.exe

Reads the id tables out of src/runtime_check.cpp (the ids and sizes are shared; the rules themselves, the member
displacements and the hook offsets are copied here and must be kept in step by hand), checks each address the way the
plugin does (AE rules), and then runs each check against a wrong neighbour function, which must FAIL. Exit code 0 = every expected
PASS passed and every expected FAIL failed. Read-only. SE cannot be run here (no 1.5.97 executable): AE ids only.
The on-disk Steam executables of 1.6.x are SteamStub-encrypted; 1.7.104 is not.
"""
from __future__ import annotations

import argparse
import bisect
import logging
import re
import struct
import sys
from pathlib import Path

import pefile

log = logging.getLogger("runtime_check_harness")
ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "runtime_check.cpp"
BASE_DEFAULT = 0x140000000


def load_ids(path: Path) -> dict[int, int]:
    """AE address library, format 2 (packed, CommonLib IDDB.cpp unpack_file) or format 5 (dense uint32 per id)."""
    raw = path.read_bytes()
    fmt = struct.unpack_from("<i", raw, 0)[0]
    if fmt == 5:
        count = struct.unpack_from("<i", raw, 92)[0]
        return {i: o for i, o in enumerate(struct.unpack_from(f"<{count}I", raw, 96)) if o}
    if fmt != 2:
        raise ValueError(f"{path}: address library format {fmt} not handled (2 or 5 expected)")
    pos = 4 + 16
    name_len = struct.unpack_from("<i", raw, pos)[0]
    pos += 4 + name_len
    ptr_size, count = struct.unpack_from("<ii", raw, pos)
    pos += 8
    out: dict[int, int] = {}
    pid = poff = 0

    def take(f: str) -> int:
        nonlocal pos
        (v,) = struct.unpack_from(f, raw, pos)
        pos += struct.calcsize(f)
        return v

    for _ in range(count):
        t = take("<B")
        lo, hi = t & 0xF, t >> 4
        i = {0: lambda: take("<Q"), 1: lambda: pid + 1, 2: lambda: pid + take("<B"), 3: lambda: pid - take("<B"), 4: lambda: pid + take("<H"),
             5: lambda: pid - take("<H"), 6: lambda: take("<H"), 7: lambda: take("<I")}[lo]()
        tmp = poff // ptr_size if hi & 8 else poff
        h = hi & 7
        o = {0: lambda: take("<Q"), 1: lambda: tmp + 1, 2: lambda: tmp + take("<B"), 3: lambda: tmp - take("<B"), 4: lambda: tmp + take("<H"),
             5: lambda: tmp - take("<H"), 6: lambda: take("<H"), 7: lambda: take("<I")}[h]()
        if hi & 8:
            o *= ptr_size
        out[i] = o
        pid, poff = i, o
    return out


class Tables:
    """The AE half of every table in runtime_check.cpp."""

    def __init__(self, text: str) -> None:
        def function(name: str) -> tuple[int, int]:
            m = re.search(rf"constexpr Function {name}\{{ \"\w+\", \{{ (\d+), (\d+) \}}, (0x[0-9A-F]+), (0x[0-9A-F]+) \}}", text)
            if not m:
                raise ValueError(f"{name} not found in {SOURCE}")
            return int(m.group(2)), int(m.group(4), 16)

        def pairs(name: str) -> list[int]:
            m = re.search(rf"std::array<Ids, (\d+)>\s+{name}\{{ \{{ (.*?) \}} \}};", text)
            if not m:
                raise ValueError(f"{name} not found in {SOURCE}")
            found = [int(ae) for _, ae in re.findall(r"\{ (\d+), (\d+) \}", m.group(2))]
            if len(found) != int(m.group(1)):
                raise ValueError(f"{name}: read {len(found)} pairs, the array holds {m.group(1)}; the table layout changed")
            return found

        def pair(name: str) -> int:
            m = re.search(rf"constexpr Ids\s+{name}\{{ (\d+), (\d+) \}};", text)
            if not m:
                raise ValueError(f"{name} not found in {SOURCE}")
            return int(m.group(2))

        def number(name: str) -> int:
            m = re.search(rf"constexpr std::size_t\s+{name} = (\d+);", text)
            if not m:
                raise ValueError(f"{name} not found in {SOURCE}")
            return int(m.group(1))

        self.preload = function("kPreload")
        self.release = function("kRelease")
        self.set_art = function("kSetArt")
        self.detach = function("kDetach")
        self.charge = function("kChargeCaller")
        self.preload_callees = pairs("kPreloadCallees")
        self.preload_needed = number("kPreloadNeeded")
        self.release_callee = pair("kReleaseCallee")
        self.set_art_tail = pair("kSetArtTail")
        self.detach_callees = pairs("kDetachCallees")
        self.detach_needed = number("kDetachNeeded")
        self.charge_callees = pairs("kChargeCallees")
        self.charge_needed = number("kChargeNeeded")
        self.main_update = pair("kMainUpdate")
        self.frame_targets = pairs("kFrameTargets")
        self.menu_target = pair("kMenuEquipTarget")
        m = re.search(r"std::array<VSlot, (\d+)> kVSlots\{ \{(.*?)\} \};", text, re.S)
        if not m:
            raise ValueError(f"kVSlots not found in {SOURCE}")
        self.vslots = [(tbl, int(slot, 16), int(ae)) for tbl, slot, _, ae in re.findall(r"\{ Table::(\w+), (0x[0-9A-F]+), \{ (\d+), (\d+) \} \}", m.group(2))]
        if len(self.vslots) != int(m.group(1)):
            raise ValueError("kVSlots: the table layout changed")


class Game:
    def __init__(self, lib: Path, exe: Path) -> None:
        self.ids = load_ids(lib)
        self.offsets = sorted(set(self.ids.values()))
        self.pe = pefile.PE(str(exe), fast_load=True)
        self.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
        self.base = self.pe.OPTIONAL_HEADER.ImageBase or BASE_DEFAULT
        self.pdata = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in getattr(self.pe, "DIRECTORY_ENTRY_EXCEPTION", []))
        self.pdata_begins = [b for b, _ in self.pdata]

    def addr(self, ae_id: int) -> int:
        return self.base + self.ids[ae_id]

    def span(self, ae_id: int) -> int:
        """Distance to the next id: the size runtime_check.cpp's tables hold."""
        off = self.ids[ae_id]
        return self.offsets[bisect.bisect_right(self.offsets, off)] - off

    def start(self, address: int) -> str:
        """Port of FunctionStart (RtlLookupFunctionEntry on the image's .pdata)."""
        rva = address - self.base
        i = bisect.bisect_right(self.pdata_begins, rva) - 1
        if i < 0 or not (self.pdata[i][0] <= rva < self.pdata[i][1]):
            return "noentry"
        return "yes" if self.pdata[i][0] == rva else "mid"

    def code(self, address: int, size: int) -> bytes:
        return self.pe.get_data(address - self.base, size)


def branches_to(code: bytes, at: int, targets: set[int], jumps_too: bool) -> list[tuple[int, int, int]]:
    """Port of core::FindBranchesTo."""
    hits = []
    for i in range(len(code) - 4):
        op = code[i]
        if op != 0xE8 and not (jumps_too and op == 0xE9):
            continue
        target = at + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
        if target in targets:
            hits.append((i, target, op))
    return hits


def has_disp32(code: bytes, disp: int) -> bool:
    return struct.pack("<I", disp) in code


def called(g: Game, code: bytes, at: int, ids: list[int]) -> int:
    return len({t for _, t, _ in branches_to(code, at, {g.addr(c) for c in ids}, False)})


def check_callees(g: Game, fid: int, size: int, callees: list[int], needed: int) -> tuple[bool, str]:
    at = g.addr(fid)
    start = g.start(at)
    found = called(g, g.code(at, size), at, callees)
    return start != "mid" and found >= needed, f"id {fid} ({start}): calls {found} of {len(callees)} (needs {needed})"


def check_release(g: Game, t: Tables, fid: int, size: int) -> tuple[bool, str]:
    at = g.addr(fid)
    start = g.start(at)
    code = g.code(at, size)
    calls = called(g, code, at, [t.release_callee]) == 1
    members = has_disp32(code, 0x80) and has_disp32(code, 0x88)
    xadd = re.search(rb"\xF0[\x40-\x4F]?\x0F\xC1", code) is not None  # REX prefix allowed, as HasLockXadd
    preload_calls = called(g, code, at, t.preload_callees)
    ok = start != "mid" and members and calls and preload_calls < t.preload_needed  # AE rule: the helper call is required
    return ok, f"id {fid} ({start}): +0x80/+0x88 {members}, helper {calls}, lock xadd {xadd}, preload callees {preload_calls}"


def check_set_art(g: Game, fid: int, size: int, tail: int) -> tuple[bool, str]:
    at = g.addr(fid)
    start = g.start(at)
    code = g.code(at, size)
    jumps = bool(branches_to(code, at, {g.addr(tail)}, True))
    members = has_disp32(code, 0xE0) and has_disp32(code, 0xF8)
    return start != "mid" and members and jumps, f"id {fid} ({start}): +0xE0/+0xF8 {members}, jumps on {jumps} (AE rule: both)"


def check_charge(g: Game, t: Tables, fid: int, size: int, lookup_id: int) -> tuple[bool, str]:
    at = g.addr(fid)
    start = g.start(at)
    code = g.code(at, size)
    hits = branches_to(code, at, {g.addr(lookup_id)}, False)
    own = called(g, code, at, t.charge_callees)
    ok = start != "mid" and len(hits) == 1 and own >= t.charge_needed
    return ok, f"id {fid} ({start}): {len(hits)} call(s) to {lookup_id}, {own} own callees" + (f", return +0x{hits[0][0] + 5:X}" if ok else "")


# CommonLib Offsets_VTABLE.h, AE ids of the vtables runtime_check.cpp VtableSlotsOk reads.
VTABLES_AE = {"kEnchantmentItem": 186389, "kActorMagicCaster": 205828, "kCharacterMagicTarget": 207894, "kPlayerMagicTarget": 208048}


def check_vslot(g: Game, table: str, slot: int, expected: int) -> tuple[bool, str]:
    value = struct.unpack("<Q", g.code(g.addr(VTABLES_AE[table]) + slot * 8, 8))[0]
    ok = value == g.addr(expected)
    return ok, f"{table} slot 0x{slot:X} holds {'the expected ' + str(expected) if ok else hex(value - g.base)}"


def check_frame(g: Game, t: Tables, candidates: list[int]) -> tuple[bool, str]:
    base = g.addr(t.main_update)
    targets = {g.addr(x) for x in t.frame_targets}
    for off in candidates:
        code = g.code(base + off, 5)
        if code[0] == 0xE8 and base + off + 5 + struct.unpack_from("<i", code, 1)[0] in targets:
            return True, f"Main::Update +0x{off:X} calls a known per-frame function"
    return False, f"no candidate of {[hex(c) for c in candidates]} calls a known per-frame function"


def check_menu(g: Game, t: Tables, parent: int, offset: int) -> tuple[bool, str]:
    site = g.addr(parent) + offset
    code = g.code(site, 5)
    if code[0] != 0xE8:
        return False, f"{parent}+0x{offset:X} not a CALL"
    target = site + 5 + struct.unpack_from("<i", code, 1)[0]
    ok = target == g.addr(t.menu_target)
    name = "38928" if ok else hex(target - g.base)
    return ok, f"{parent}+0x{offset:X} calls {name} ({g.start(target)}; the SE rule needs a function start)"


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    ap = argparse.ArgumentParser()
    ap.add_argument("addrlib", type=Path)
    ap.add_argument("exe", type=Path)
    ap.add_argument("--frame-candidates", default="0xC38,0xC3D", help="Main::Update offsets for this build (runtime_check.cpp FrameCallSite)")
    a = ap.parse_args()
    t = Tables(SOURCE.read_text(encoding="utf-8"))
    g = Game(a.addrlib, a.exe)
    frame = [int(x, 16) for x in a.frame_candidates.split(",")]

    sizes = {"PRELOAD": t.preload, "RELEASE": t.release, "SETART": t.set_art, "DETACH": t.detach, "CHARGE": t.charge}
    size_bad = [(n, hex(sz), hex(g.span(fid))) for n, (fid, sz) in sizes.items() if g.span(fid) != sz]
    log.info("table sizes vs this library: %s", "all equal" if not size_bad else f"DIFFER {size_bad}")
    mid = g.start(g.addr(11303) + 0x10)
    # (name, expected pass, result). A wrong function is scanned over its own span, as the tables would hold for it.
    cases = [
        ("PRELOAD", True, check_callees(g, t.preload[0], t.preload[1], t.preload_callees, t.preload_needed)),
        ("RELEASE", True, check_release(g, t, t.release[0], t.release[1])),
        ("SETART", True, check_set_art(g, t.set_art[0], t.set_art[1], t.set_art_tail)),
        ("DETACH", True, check_callees(g, t.detach[0], t.detach[1], t.detach_callees, t.detach_needed)),
        ("CHARGE", True, check_charge(g, t, t.charge[0], t.charge[1], 34609)),
        ("FRAME", True, check_frame(g, t, frame)),
        ("MENU", True, check_menu(g, t, 38908, 0xD7)),
        ("DETOUR 34609", True, (g.start(g.addr(34609)) == "yes", f"start check reads {g.start(g.addr(34609))}")),
        ("DETOUR 11321", True, (g.start(g.addr(11321)) == "yes", f"start check reads {g.start(g.addr(11321))}")),
        *[(f"VTABLE {tbl} 0x{slot:X}", True, check_vslot(g, tbl, slot, ae)) for tbl, slot, ae in t.vslots],
        ("VTABLE one slot off", False, check_vslot(g, "kEnchantmentItem", 0x5D, 11294)),
        # wrong neighbours: each must fail
        ("PRELOAD on 11302", False, check_callees(g, 11302, g.span(11302), t.preload_callees, t.preload_needed)),
        ("PRELOAD on 11307", False, check_callees(g, 11307, g.span(11307), t.preload_callees, t.preload_needed)),
        ("PRELOAD on 11306", False, check_callees(g, 11306, g.span(11306), t.preload_callees, t.preload_needed)),
        ("RELEASE on 11303", False, check_release(g, t, 11303, g.span(11303))),
        ("RELEASE on 11305", False, check_release(g, t, 11305, g.span(11305))),
        ("RELEASE on 11307", False, check_release(g, t, 11307, g.span(11307))),
        ("SETART on 34149", False, check_set_art(g, 34149, g.span(34149), t.set_art_tail)),
        ("SETART on 34151", False, check_set_art(g, 34151, g.span(34151), t.set_art_tail)),
        ("DETACH on 34151", False, check_callees(g, 34151, g.span(34151), t.detach_callees, t.detach_needed)),
        ("DETACH on 34153", False, check_callees(g, 34153, g.span(34153), t.detach_callees, t.detach_needed)),
        ("CHARGE on 38822", False, check_charge(g, t, 38822, g.span(38822), 34609)),
        ("CHARGE on 38824", False, check_charge(g, t, 38824, g.span(38824), 34609)),
        ("11303+0x10 is not a start", False, (mid != "mid", f"start check reads {mid} (the plugin rejects only mid)")),
        ("FRAME one byte off", False, check_frame(g, t, [c + 1 for c in frame])),
        ("MENU at +0xB7", False, check_menu(g, t, 38908, 0xB7)),
    ]
    bad = 0
    for name, expect, (ok, detail) in cases:
        right = ok == expect
        bad += not right
        log.info("%s %-22s %s (%s)", "OK  " if right else "BAD ", name, "PASS" if ok else "FAIL", detail)
    log.info("harness: %d cases, %d not as expected", len(cases), bad)
    return 0 if bad == 0 and not size_bad else 1


if __name__ == "__main__":
    sys.exit(main())
