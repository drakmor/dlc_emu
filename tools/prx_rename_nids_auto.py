#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Rename SCE PRX exported NID strings and rebuild the ELF .hash table automatically.

PRX dynsym names are stored in .dynstr as:
    <11-char-NID>#<library-id-char>#<module-id-char>

For exported PRX symbols, prospero-lld / the runtime hash lookup uses the
canonical string:
    <11-char-NID>#<real-export-library-name>#<real-module-name>

This script parses the real export library names and module name from .dynamic,
patches old NIDs to new NIDs in .dynstr, and moves the changed dynsym entries
to their correct SysV .hash buckets.

Examples:
    # List export groups and current PRX-style symbols
    python3 prx_rename_nids_auto.py libSceAppContent.prx --list

    # Dry-run replacement, auto-detecting export library/module names
    python3 prx_rename_nids_auto.py in.prx out.prx OLDNID12345=NEWNID12345 --dry-run

    # Patch several replacements from command line
    python3 prx_rename_nids_auto.py in.prx out.prx \
        ts6GlZOKRrE=AAAAAAAAAAA Uco1I0dlDi8=BBBBBBBBBBB

    # Patch from a text map file, one mapping per line: OLD=NEW or OLD NEW
    python3 prx_rename_nids_auto.py in.prx out.prx --map rename.txt

    # Only patch one export group, for safety
    python3 prx_rename_nids_auto.py in.prx out.prx OLD=NEW --only-suffix '#E#A'
    python3 prx_rename_nids_auto.py in.prx out.prx OLD=NEW --only-library libSceAppContent

Map file syntax:
    # comments and empty lines are ignored
    ts6GlZOKRrE=AAAAAAAAAAA
    Uco1I0dlDi8 BBBBBBBBBBB

JSON map syntax is also supported:
    {"oldNIDxxxxx": "newNIDyyyyy"}
    [{"old": "oldNIDxxxxx", "new": "newNIDyyyyy"}]
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

SCE_NID_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"
NID_RE = re.compile(r"^[A-Za-z0-9+\-]{11}$")
PRX_SYM_RE = re.compile(r"^([A-Za-z0-9+\-]{11})#([^#])#([^#])$")

# Standard ELF dynamic tags used here.
DT_NULL = 0
DT_STRTAB = 5

# SCE PRX dynamic tags observed in prospero-lld PRX output.
DT_SCE_MODULE_INFO = 0x61000043
DT_SCE_EXPORT_LIB = 0x61000047


class PrxError(ValueError):
    pass


def id_char(n: int) -> str:
    if 0 <= n < len(SCE_NID_ALPHABET):
        return SCE_NID_ALPHABET[n]
    return f"?{n}?"


def id_num(ch: str) -> int:
    try:
        return SCE_NID_ALPHABET.index(ch)
    except ValueError:
        raise PrxError(f"bad PRX short id char {ch!r}; alphabet is {SCE_NID_ALPHABET!r}") from None


def elf_hash(name: str | bytes) -> int:
    """SysV ELF hash used for .hash buckets."""
    data = name.encode("utf-8") if isinstance(name, str) else name
    h = 0
    for c in data:
        h = ((h << 4) + c) & 0xFFFFFFFF
        h ^= (h >> 24) & 0xF0
    return h & 0x0FFFFFFF


@dataclass
class Section:
    index: int
    name: str
    name_off: int
    type: int
    flags: int
    addr: int
    offset: int
    size: int
    link: int
    info: int
    addralign: int
    entsize: int


@dataclass
class DynSym:
    index: int
    name: str
    st_name: int
    st_info: int
    st_other: int
    st_shndx: int
    st_value: int
    st_size: int
    name_file_off: int | None

    @property
    def nid_parts(self) -> tuple[str, str, str] | None:
        m = PRX_SYM_RE.match(self.name)
        if not m:
            return None
        return m.group(1), m.group(2), m.group(3)


@dataclass
class ExportGroup:
    lib_id: int
    mod_id: int
    lib_char: str
    mod_char: str
    library: str
    module: str

    @property
    def suffix(self) -> str:
        return f"#{self.lib_char}#{self.mod_char}"

    def canonical(self, nid: str) -> str:
        return f"{nid}#{self.library}#{self.module}"


class Elf64LE:
    def __init__(self, data: bytes):
        self.data = data
        if len(data) < 0x40:
            raise PrxError("file too small for ELF64 header")
        if data[:4] != b"\x7fELF":
            raise PrxError("not an ELF file")
        if data[4] != 2:
            raise PrxError("expected ELF64")
        if data[5] != 1:
            raise PrxError("expected little-endian ELF")

        self.e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
        self.e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
        self.e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
        self.e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]
        if self.e_shentsize < 64:
            raise PrxError("unexpected ELF64 section header size")
        if self.e_shoff + self.e_shnum * self.e_shentsize > len(data):
            raise PrxError("section header table is outside file")

        raw_sections = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            sh = struct.unpack_from("<IIQQQQIIQQ", data, off)
            raw_sections.append(sh)

        if not (0 <= self.e_shstrndx < len(raw_sections)):
            raise PrxError("bad e_shstrndx")
        shstr_raw = raw_sections[self.e_shstrndx]
        shstr_off, shstr_size = shstr_raw[4], shstr_raw[5]
        if shstr_off + shstr_size > len(data):
            raise PrxError("section string table is outside file")
        shstr = data[shstr_off:shstr_off + shstr_size]

        self.sections: list[Section] = []
        for i, sh in enumerate(raw_sections):
            name_off = sh[0]
            end = shstr.find(b"\0", name_off) if name_off < len(shstr) else -1
            name = shstr[name_off:end].decode("utf-8", "replace") if end >= 0 else ""
            self.sections.append(Section(i, name, *sh))

    def section(self, name: str) -> Section:
        for sh in self.sections:
            if sh.name == name:
                return sh
        raise PrxError(f"section not found: {name}")

    def str_at(self, strtab: Section, off: int) -> str:
        start = strtab.offset + off
        if not (strtab.offset <= start < strtab.offset + strtab.size):
            raise PrxError(f"string offset 0x{off:x} outside {strtab.name}")
        end = self.data.find(b"\0", start, strtab.offset + strtab.size)
        if end < 0:
            raise PrxError(f"unterminated string at {strtab.name}+0x{off:x}")
        return self.data[start:end].decode("utf-8", "replace")

    def dynsyms(self) -> list[DynSym]:
        dynsym = self.section(".dynsym")
        dynstr = self.section(".dynstr")
        if dynsym.entsize == 0:
            raise PrxError(".dynsym entsize is zero")
        if dynsym.entsize < 24:
            raise PrxError(f"unexpected .dynsym entsize {dynsym.entsize}")
        count = dynsym.size // dynsym.entsize
        syms: list[DynSym] = []
        for i in range(count):
            off = dynsym.offset + i * dynsym.entsize
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", self.data, off)
            name = ""
            name_file_off = None
            if st_name:
                name_file_off = dynstr.offset + st_name
                if not (dynstr.offset <= name_file_off < dynstr.offset + dynstr.size):
                    raise PrxError(f"symbol {i} st_name points outside .dynstr")
                end = self.data.find(b"\0", name_file_off, dynstr.offset + dynstr.size)
                if end < 0:
                    raise PrxError(f"symbol {i} name is not NUL-terminated inside .dynstr")
                name = self.data[name_file_off:end].decode("ascii", "replace")
            syms.append(DynSym(i, name, st_name, st_info, st_other, st_shndx, st_value, st_size, name_file_off))
        return syms

    def hash_table(self) -> tuple[Section, int, int, list[int], list[int]]:
        sh = self.section(".hash")
        if sh.size < 8 or sh.size % 4 != 0:
            raise PrxError("bad .hash size")
        vals = list(struct.unpack_from(f"<{sh.size // 4}I", self.data, sh.offset))
        nbucket, nchain = vals[0], vals[1]
        expected_words = 2 + nbucket + nchain
        if expected_words > len(vals):
            raise PrxError(".hash is truncated")
        buckets = vals[2:2 + nbucket]
        chains = vals[2 + nbucket:2 + nbucket + nchain]
        return sh, nbucket, nchain, buckets, chains

    def dynamic_entries(self) -> list[tuple[int, int]]:
        dyn = self.section(".dynamic")
        if dyn.entsize == 0:
            raise PrxError(".dynamic entsize is zero")
        if dyn.entsize < 16:
            raise PrxError(f"unexpected .dynamic entsize {dyn.entsize}")
        count = dyn.size // dyn.entsize
        out: list[tuple[int, int]] = []
        for i in range(count):
            tag, val = struct.unpack_from("<QQ", self.data, dyn.offset + i * dyn.entsize)
            out.append((tag, val))
            if tag == DT_NULL:
                break
        return out

    def export_id_maps(self) -> tuple[dict[int, str], dict[int, str]]:
        """
        Return (library_by_id, module_by_id).

        In observed prospero-lld PRX files, DT_SCE_MODULE_INFO gives the module
        name and module id 0, encoded as 'A'. DT_SCE_EXPORT_LIB entries carry
        the export library id in the high 16 bits of d_val and the name offset
        in the low 32 bits.
        """
        dynstr = self.section(".dynstr")
        module_by_id: dict[int, str] = {}
        library_by_id: dict[int, str] = {}
        for tag, val in self.dynamic_entries():
            name_off = val & 0xFFFFFFFF
            if tag == DT_SCE_MODULE_INFO:
                module_by_id[0] = self.str_at(dynstr, name_off)
            elif tag == DT_SCE_EXPORT_LIB:
                lib_id = val >> 48
                library_by_id[lib_id] = self.str_at(dynstr, name_off)
        return library_by_id, module_by_id

    def export_groups(self) -> dict[tuple[int, int], ExportGroup]:
        lib_by_id, mod_by_id = self.export_id_maps()
        groups: dict[tuple[int, int], ExportGroup] = {}
        for lib_id, library in lib_by_id.items():
            for mod_id, module in mod_by_id.items():
                groups[(lib_id, mod_id)] = ExportGroup(
                    lib_id=lib_id,
                    mod_id=mod_id,
                    lib_char=id_char(lib_id),
                    mod_char=id_char(mod_id),
                    library=library,
                    module=module,
                )
        return groups


def buckets_to_symbol_map(buckets: list[int], chains: list[int]) -> dict[int, int]:
    """Return dynsym_index -> bucket_index from an existing SysV .hash table."""
    out: dict[int, int] = {}
    for bucket_index, head in enumerate(buckets):
        i = head
        seen: set[int] = set()
        while i:
            if i in seen:
                raise PrxError(f"cycle in .hash bucket {bucket_index} at symbol {i}")
            if i >= len(chains):
                raise PrxError(f".hash bucket {bucket_index} points outside chain table: {i}")
            seen.add(i)
            out[i] = bucket_index
            i = chains[i]
    return out


def rebuild_hash(nbucket: int, nchain: int, sym_to_bucket: dict[int, int]) -> tuple[list[int], list[int]]:
    """
    Rebuild bucket/chain arrays while preserving existing bucket assignments for
    unchanged symbols. Changed symbols must already have their new bucket in
    sym_to_bucket.
    """
    buckets = [0] * nbucket
    chains = [0] * nchain
    for sym_index in range(1, nchain):
        if sym_index not in sym_to_bucket:
            # Leave unusual unreachable symbols unreachable instead of guessing.
            continue
        bucket_index = sym_to_bucket[sym_index]
        if not (0 <= bucket_index < nbucket):
            raise PrxError(f"bad bucket {bucket_index} for symbol {sym_index}")
        chains[sym_index] = buckets[bucket_index]
        buckets[bucket_index] = sym_index
    return buckets, chains


def validate_nid(nid: str, label: str) -> None:
    if not NID_RE.match(nid):
        raise PrxError(f"{label} must be exactly 11 chars from alphabet {SCE_NID_ALPHABET!r}; got {nid!r}")


def parse_mapping_item(item: str) -> tuple[str, str]:
    if "=" in item:
        old, new = item.split("=", 1)
    else:
        parts = item.split()
        if len(parts) != 2:
            raise PrxError(f"bad mapping {item!r}; expected OLD=NEW or 'OLD NEW'")
        old, new = parts
    old = old.strip()
    new = new.strip()
    validate_nid(old, "old NID")
    validate_nid(new, "new NID")
    return old, new


def load_map_file(path: Path) -> list[tuple[str, str]]:
    text = path.read_text(encoding="utf-8")
    if path.suffix.lower() == ".json":
        obj = json.loads(text)
        if isinstance(obj, dict):
            items = list(obj.items())
        elif isinstance(obj, list):
            items = []
            for entry in obj:
                if isinstance(entry, dict):
                    old = entry.get("old") or entry.get("from") or entry.get("old_nid")
                    new = entry.get("new") or entry.get("to") or entry.get("new_nid")
                    if not old or not new:
                        raise PrxError(f"bad JSON mapping entry: {entry!r}")
                    items.append((old, new))
                elif isinstance(entry, (list, tuple)) and len(entry) == 2:
                    items.append((entry[0], entry[1]))
                else:
                    raise PrxError(f"bad JSON mapping entry: {entry!r}")
        else:
            raise PrxError("JSON mapping must be an object or a list")
        result = []
        for old, new in items:
            old, new = str(old), str(new)
            validate_nid(old, "old NID")
            validate_nid(new, "new NID")
            result.append((old, new))
        return result

    result: list[tuple[str, str]] = []
    for lineno, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        # '#' is a comment only at line start. PRX symbol suffixes contain '#'.
        if not line or line.startswith("#") or line.startswith(";"):
            continue
        try:
            result.append(parse_mapping_item(line))
        except Exception as e:
            raise PrxError(f"{path}:{lineno}: {e}") from e
    return result


def dedupe_and_validate_mappings(mappings: Iterable[tuple[str, str]]) -> dict[str, str]:
    out: dict[str, str] = {}
    for old, new in mappings:
        validate_nid(old, "old NID")
        validate_nid(new, "new NID")
        if old in out and out[old] != new:
            raise PrxError(f"old NID {old!r} has two replacements: {out[old]!r} and {new!r}")
        out[old] = new
    if not out:
        raise PrxError("no NID replacements were supplied")
    return out


def normalize_suffix(s: str) -> tuple[str, str]:
    s = s.strip()
    if s.count("#") == 2 and s.startswith("#"):
        _, lib_ch, mod_ch = s.split("#")
    elif len(s) == 2:
        lib_ch, mod_ch = s[0], s[1]
    else:
        raise PrxError("suffix must look like '#E#A' or 'EA'")
    if len(lib_ch) != 1 or len(mod_ch) != 1:
        raise PrxError("suffix chars must be one char each")
    id_num(lib_ch)
    id_num(mod_ch)
    return lib_ch, mod_ch


def group_for_symbol(groups: dict[tuple[int, int], ExportGroup], lib_ch: str, mod_ch: str) -> ExportGroup | None:
    return groups.get((id_num(lib_ch), id_num(mod_ch)))


def symbol_is_selected(
    group: ExportGroup,
    *,
    only_suffixes: set[tuple[str, str]],
    only_libraries: set[str],
    only_modules: set[str],
) -> bool:
    if only_suffixes and (group.lib_char, group.mod_char) not in only_suffixes:
        return False
    if only_libraries and group.library not in only_libraries:
        return False
    if only_modules and group.module not in only_modules:
        return False
    return True


def print_groups(elf: Elf64LE) -> None:
    lib_by_id, mod_by_id = elf.export_id_maps()
    print("export library ids:")
    if lib_by_id:
        for i in sorted(lib_by_id):
            print(f"  {id_char(i)} ({i}) -> {lib_by_id[i]}")
    else:
        print("  <none>")
    print("module ids:")
    if mod_by_id:
        for i in sorted(mod_by_id):
            print(f"  {id_char(i)} ({i}) -> {mod_by_id[i]}")
    else:
        print("  <none>")


def print_list(data: bytes, *, show_all_prx_names: bool = False) -> None:
    elf = Elf64LE(data)
    _, nbucket, nchain, buckets, chains = elf.hash_table()
    sym_bucket = buckets_to_symbol_map(buckets, chains)
    groups = elf.export_groups()

    print(f".hash: nbucket={nbucket}, nchain={nchain}")
    print_groups(elf)
    print("symbols:")
    print("index  nid          suffix  actual-bucket  canonical-bucket  group / canonical-name")
    for sym in elf.dynsyms()[1:]:
        parts = sym.nid_parts
        if not parts:
            continue
        nid, lib_ch, mod_ch = parts
        group = group_for_symbol(groups, lib_ch, mod_ch)
        actual = sym_bucket.get(sym.index)
        if not group:
            if show_all_prx_names:
                print(f"{sym.index:5d}  {nid:11s}  #{lib_ch}#{mod_ch:<2s}  {str(actual):>13s}  {'?':>16s}  <unknown group>")
            continue
        canonical = group.canonical(nid)
        expected = elf_hash(canonical) % nbucket
        mark = "" if actual == expected else " !"
        print(
            f"{sym.index:5d}  {nid:11s}  {group.suffix:<6s}  {str(actual):>13s}  "
            f"{expected:16d}{mark}  {group.library} / {group.module} / {canonical}"
        )


def apply_renames(
    data: bytes,
    replacements: dict[str, str],
    *,
    only_suffixes: set[tuple[str, str]] | None = None,
    only_libraries: set[str] | None = None,
    only_modules: set[str] | None = None,
    first_only: bool = False,
    require_all: bool = True,
    require_known_export_group: bool = True,
) -> tuple[bytes, list[str]]:
    only_suffixes = only_suffixes or set()
    only_libraries = only_libraries or set()
    only_modules = only_modules or set()

    work = bytearray(data)
    elf = Elf64LE(bytes(work))
    hash_sh, nbucket, nchain, buckets, chains = elf.hash_table()
    syms = elf.dynsyms()
    groups = elf.export_groups()
    sym_to_bucket = buckets_to_symbol_map(buckets, chains)

    if not groups:
        raise PrxError("no SCE export groups found in .dynamic; cannot compute canonical export hashes")

    found_count = {old: 0 for old in replacements}
    changed: dict[int, str] = {}
    reports: list[str] = []

    for sym in syms[1:]:
        parts = sym.nid_parts
        if not parts:
            continue
        old_nid, lib_ch, mod_ch = parts
        if old_nid not in replacements:
            continue
        if first_only and found_count[old_nid] > 0:
            continue

        group = group_for_symbol(groups, lib_ch, mod_ch)
        if group is None:
            msg = f"symbol {sym.index} {sym.name}: no export group for suffix #{lib_ch}#{mod_ch}"
            if require_known_export_group:
                raise PrxError(msg)
            reports.append("skip: " + msg)
            continue

        if not symbol_is_selected(
            group,
            only_suffixes=only_suffixes,
            only_libraries=only_libraries,
            only_modules=only_modules,
        ):
            continue

        new_nid = replacements[old_nid]
        found_count[old_nid] += 1
        if sym.name_file_off is None:
            raise PrxError(f"symbol {sym.index} has no file-backed name")
        if sym.index >= nchain:
            raise PrxError(f"symbol {sym.index} is outside .hash nchain={nchain}")

        old_full = sym.name
        new_full = f"{new_nid}#{lib_ch}#{mod_ch}"
        if len(old_full) != len(new_full):
            raise PrxError(f"length changed unexpectedly for symbol {sym.index}: {old_full!r} -> {new_full!r}")

        # Patch exactly the 11-char NID and keep #libId#modId unchanged.
        work[sym.name_file_off:sym.name_file_off + len(new_full)] = new_full.encode("ascii")

        canonical = group.canonical(new_nid)
        old_bucket = sym_to_bucket.get(sym.index)
        new_bucket = elf_hash(canonical) % nbucket
        sym_to_bucket[sym.index] = new_bucket
        changed[sym.index] = canonical
        reports.append(
            f"sym {sym.index}: {old_full} -> {new_full}; "
            f"{group.library} / {group.module}; bucket {old_bucket} -> {new_bucket}; canonical={canonical}"
        )

    missing = [old for old, count in found_count.items() if count == 0]
    if missing and require_all:
        raise PrxError("old NID(s) not found in selected export groups: " + ", ".join(missing))
    if not changed:
        raise PrxError("no matching NIDs were renamed")

    new_buckets, new_chains = rebuild_hash(nbucket, nchain, sym_to_bucket)
    vals = [nbucket, nchain] + new_buckets + new_chains
    struct.pack_into(f"<{len(vals)}I", work, hash_sh.offset, *vals)

    # Sanity-check exactly the changed dynsym indexes.
    patched = Elf64LE(bytes(work))
    _, nb2, nchain2, b2, c2 = patched.hash_table()
    patched_buckets = buckets_to_symbol_map(b2, c2)
    for sym_index, canonical in changed.items():
        if sym_index >= nchain2:
            raise PrxError(f"sanity check failed: changed symbol {sym_index} outside nchain={nchain2}")
        expected = elf_hash(canonical) % nb2
        actual = patched_buckets.get(sym_index)
        if actual != expected:
            raise PrxError(
                f"sanity check failed for symbol {sym_index}: actual bucket {actual}, expected {expected}, canonical={canonical}"
            )

    if missing:
        reports.append("warning: old NID(s) not found in selected export groups: " + ", ".join(missing))
    reports.append(f"rebuilt .hash at file offset 0x{hash_sh.offset:x}; nbucket={nbucket}, nchain={nchain}")
    return bytes(work), reports


def collect_mappings(args) -> dict[str, str]:
    raw_mappings: list[tuple[str, str]] = []
    for item in args.pairs:
        raw_mappings.append(parse_mapping_item(item))
    for mf in args.map_file or []:
        raw_mappings.extend(load_map_file(Path(mf)))
    return dedupe_and_validate_mappings(raw_mappings)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Rename PRX exported NIDs in .dynstr and automatically rebuild correct SysV .hash buckets."
    )
    ap.add_argument("in_prx", help="input PRX/ELF")
    ap.add_argument("out_prx", nargs="?", help="output PRX/ELF; omitted when --list is used")
    ap.add_argument("pairs", nargs="*", help="replacement pairs: OLD=NEW or 'OLD NEW' quoted as one argument")
    ap.add_argument("--map", dest="map_file", action="append", help="mapping file: txt OLD=NEW / OLD NEW, or JSON object/list")
    ap.add_argument("--dry-run", action="store_true", help="show what would be changed but do not write output")
    ap.add_argument("--list", action="store_true", help="list auto-detected export groups and PRX-style dynsym NIDs")
    ap.add_argument("--show-all-prx-names", action="store_true", help="with --list, also show PRX-style names whose suffix is not an export group")
    ap.add_argument("--first-only", action="store_true", help="rename only first occurrence of each old NID")
    ap.add_argument("--allow-missing", action="store_true", help="do not fail if a mapping old NID is absent in the selected export groups")
    ap.add_argument("--allow-unknown-groups", action="store_true", help="skip matching symbols whose #lib#module id is not in .dynamic instead of failing")
    ap.add_argument("--only-suffix", action="append", default=[], help="restrict to one suffix, e.g. '#E#A' or 'EA'; can be repeated")
    ap.add_argument("--only-library", action="append", default=[], help="restrict to real export library name; can be repeated")
    ap.add_argument("--only-module", action="append", default=[], help="restrict to real module name; can be repeated")
    args = ap.parse_args(argv)

    in_path = Path(args.in_prx)
    data = in_path.read_bytes()

    if args.list:
        print_list(data, show_all_prx_names=args.show_all_prx_names)
        return 0

    if not args.out_prx:
        ap.error("out_prx is required unless --list is used")

    replacements = collect_mappings(args)
    only_suffixes = {normalize_suffix(s) for s in args.only_suffix}
    only_libraries = set(args.only_library)
    only_modules = set(args.only_module)

    patched, reports = apply_renames(
        data,
        replacements,
        only_suffixes=only_suffixes,
        only_libraries=only_libraries,
        only_modules=only_modules,
        first_only=args.first_only,
        require_all=not args.allow_missing,
        require_known_export_group=not args.allow_unknown_groups,
    )

    for line in reports:
        print(line)

    if args.dry_run:
        print("dry-run: output file was not written")
        return 0

    out_path = Path(args.out_prx)
    out_path.write_bytes(patched)
    print(f"wrote {out_path} ({len(patched)} bytes)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        raise SystemExit(1)
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        raise SystemExit(1)
