"""Build SCSSoulCharge.esp (ESL-flagged) from scratch.

Contents:
  - POOL_SIZE placeholder SPEL records (SCSSC_Pool0000..). The DLL fills effects,
    name, casting type and charge time at runtime. Manual cost (NoAutoCalc), cost 0.
  - One self-test WEAP: a copy of vanilla StaffFireball (Skyrim.esm 029B82) without
    SCS keywords, used by self-tests T-C4/T-C6 to measure vanilla staff behaviour.

Usage:
  python -I tools/build_esp.py --skyrim-esm "<path>/Skyrim.esm" --out build/SCSSoulCharge.esp
"""
from __future__ import annotations

import argparse
import logging
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

log = logging.getLogger("build_esp")

PLUGIN_NAME = "SCSSoulCharge.esp"
MASTERS = ("Skyrim.esm",)
POOL_SIZE = 1024
FIRST_OBJECT_ID = 0x800          # lowest ESL object ID valid on SE 1.5.97 (HEDR 1.70)
LAST_ESL_OBJECT_ID = 0xFFF
HEDR_VERSION = 1.70
FORM_VERSION = 44
RECORD_FLAG_ESL = 0x200
RECORD_FLAG_COMPRESSED = 0x40000

EITHER_HAND_SLOT = 0x00013F44    # Skyrim.esm EitherHand equip slot
SPIT_FLAG_MANUAL_COST = 0x1
SPELL_TYPE_SPELL = 0
CAST_FIRE_AND_FORGET = 1
DELIVERY_SELF = 0

TEST_STAFF_SOURCE_ID = 0x00029B82  # Skyrim.esm StaffFireball (master index 0)
TEST_STAFF_EDID = "SCSSC_TestStaffFireball"
TEST_STAFF_NAME = "SCSSC Test Staff (Fireball)"
# Subrecords that are localized string IDs in Skyrim.esm; replaced or dropped.
LOCALIZED_WEAP_SUBRECORDS = {"FULL", "DESC"}
# SCS keywords must never be on the test staff (it measures vanilla behaviour).

POOL_EDID_FORMAT = "SCSSC_Pool{:04d}"
POOL_PLACEHOLDER_NAME = "SCSSC Empty Charge Spell"


class EspError(RuntimeError):
    pass


def subrecord(sig: str, payload: bytes) -> bytes:
    if len(sig) != 4:
        raise EspError(f"bad subrecord signature {sig!r}")
    if len(payload) > 0xFFFF:
        raise EspError(f"subrecord {sig} too large ({len(payload)} bytes); XXXX not supported")
    return sig.encode("ascii") + struct.pack("<H", len(payload)) + payload


def zstring(text: str) -> bytes:
    data = text.encode("ascii")  # non-localized plugin: keep names ASCII
    return data + b"\0"


def record(sig: str, form_id: int, flags: int, body: bytes) -> bytes:
    header = sig.encode("ascii") + struct.pack("<IIIIHH", len(body), flags, form_id, 0, FORM_VERSION, 0)
    return header + body


def group(label: str, records: list[bytes]) -> bytes:
    content = b"".join(records)
    header = b"GRUP" + struct.pack("<I", 24 + len(content)) + label.encode("ascii") + struct.pack("<IIHH", 0, 0, 0, 0)
    return header + content


def local_form_id(object_id: int) -> int:
    """FormID inside this plugin: high byte = number of masters."""
    if not FIRST_OBJECT_ID <= object_id <= LAST_ESL_OBJECT_ID:
        raise EspError(f"object id {object_id:#x} outside the ESL range")
    return (len(MASTERS) << 24) | object_id


def pool_spell(index: int, object_id: int) -> bytes:
    spit = struct.pack(
        "<IIIfIIffI",
        0,                       # base cost
        SPIT_FLAG_MANUAL_COST,   # no auto-calc: SCS must charge 0 Magicka
        SPELL_TYPE_SPELL,
        0.0,                     # charge time (set at runtime from the ENCH)
        CAST_FIRE_AND_FORGET,    # casting type (set at runtime)
        DELIVERY_SELF,           # delivery (set at runtime)
        0.0,                     # cast duration
        0.0,                     # range
        0,                       # half-cost perk
    )
    body = (
        subrecord("EDID", zstring(POOL_EDID_FORMAT.format(index)))
        + subrecord("OBND", b"\0" * 12)
        + subrecord("FULL", zstring(POOL_PLACEHOLDER_NAME))
        + subrecord("ETYP", struct.pack("<I", EITHER_HAND_SLOT))
        + subrecord("DESC", zstring(""))
        + subrecord("SPIT", spit)
    )
    return record("SPEL", local_form_id(object_id), 0, body)


@dataclass(frozen=True)
class RawRecord:
    sig: str
    form_id: int
    flags: int
    body: bytes  # decompressed


def iter_subrecords(data: bytes):
    offset = 0
    while offset < len(data):
        if offset + 6 > len(data):
            raise EspError("truncated subrecord header")
        sig = data[offset:offset + 4].decode("ascii")
        size = struct.unpack_from("<H", data, offset + 4)[0]
        if sig == "XXXX":
            raise EspError("XXXX subrecords are not supported in copied records")
        yield sig, data[offset + 6:offset + 6 + size]
        offset += 6 + size


def find_record(plugin: Path, sig: str, form_id: int) -> RawRecord:
    buf = plugin.read_bytes()
    if buf[:4] != b"TES4":
        raise EspError(f"{plugin} is not a TES4 plugin")
    offset = 24 + struct.unpack_from("<I", buf, 4)[0]
    stack = [len(buf)]
    while offset < len(buf):
        while offset >= stack[-1]:
            stack.pop()
        typ = buf[offset:offset + 4]
        size = struct.unpack_from("<I", buf, offset + 4)[0]
        if typ == b"GRUP":
            label = buf[offset + 8:offset + 12]
            group_type = struct.unpack_from("<I", buf, offset + 12)[0]
            if group_type == 0 and label != sig.encode("ascii"):
                offset += size  # skip unrelated top groups quickly
                continue
            stack.append(offset + size)
            offset += 24
            continue
        flags, fid = struct.unpack_from("<II", buf, offset + 8)
        if typ.decode("ascii") == sig and fid == form_id:
            body = buf[offset + 24:offset + 24 + size]
            if flags & RECORD_FLAG_COMPRESSED:
                body = zlib.decompress(body[4:])
            return RawRecord(sig, fid, flags & ~RECORD_FLAG_COMPRESSED, body)
        offset += 24 + size
    raise EspError(f"{sig} {form_id:08X} not found in {plugin}")


def test_staff(skyrim_esm: Path, object_id: int) -> bytes:
    source = find_record(skyrim_esm, "WEAP", TEST_STAFF_SOURCE_ID)
    parts: list[bytes] = []
    for sig, payload in iter_subrecords(source.body):
        if sig == "EDID":
            parts.append(subrecord("EDID", zstring(TEST_STAFF_EDID)))
        elif sig == "FULL":
            parts.append(subrecord("FULL", zstring(TEST_STAFF_NAME)))
        elif sig in LOCALIZED_WEAP_SUBRECORDS:
            continue
        else:
            parts.append(subrecord(sig, payload))
    if not any(p.startswith(b"FULL") for p in parts):
        raise EspError("source staff has no FULL; layout unexpected")
    return record("WEAP", local_form_id(object_id), source.flags, b"".join(parts))


def header(num_records: int, next_object_id: int) -> bytes:
    body = subrecord("HEDR", struct.pack("<fII", HEDR_VERSION, num_records, next_object_id))
    body += subrecord("CNAM", zstring("qtuna"))
    body += subrecord("SNAM", zstring("SCSSoulCharge: charge-spell pool for Spellcasting Staff NG add-on"))
    for master in MASTERS:
        body += subrecord("MAST", zstring(master)) + subrecord("DATA", struct.pack("<Q", 0))
    return record("TES4", 0, RECORD_FLAG_ESL, body)


def build(skyrim_esm: Path) -> bytes:
    next_id = FIRST_OBJECT_ID
    spells = []
    for index in range(POOL_SIZE):
        spells.append(pool_spell(index, next_id))
        next_id += 1
    staff = test_staff(skyrim_esm, next_id)
    next_id += 1
    if next_id - 1 > LAST_ESL_OBJECT_ID:
        raise EspError("too many records for an ESL")
    groups = [group("SPEL", spells), group("WEAP", [staff])]
    num_records = len(spells) + 1 + len(groups)  # records + groups, TES4 excluded
    return header(num_records, next_id) + b"".join(groups)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--skyrim-esm", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    if not args.skyrim_esm.is_file():
        raise SystemExit(f"Skyrim.esm not found: {args.skyrim_esm}")
    if args.out.name != PLUGIN_NAME:
        raise SystemExit(f"output file must be named {PLUGIN_NAME}, got {args.out.name}")
    data = build(args.skyrim_esm)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(data)
    log.info("wrote %s (%d bytes, %d pool spells, test staff %08X)", args.out, len(data), POOL_SIZE,
             local_form_id(FIRST_OBJECT_ID + POOL_SIZE))


if __name__ == "__main__":
    main()
