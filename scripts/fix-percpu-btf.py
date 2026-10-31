#!/usr/bin/env python3
"""Correct pahole's per-CPU DATASEC entries using the linked ELF symbols.

Some pahole/LLVM combinations emit duplicate or stale per-CPU variable
offsets.  Preserve every BTF type and variable, correcting the DATASEC
offsets and ordering its entries by their linked addresses.
"""

import collections
import re
import struct
import subprocess
import sys


SECTION = re.compile(
    r"^\s*\[\s*(\d+)\]\s+(\S+)\s+\S+\s+"
    r"([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)"
)
SYMBOL = re.compile(
    r"^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+OBJECT\s+"
    r"\S+\s+\S+\s+(\d+)\s+(\S+)"
)
EXTRA_SIZE = {
    1: lambda n: 4,       # INT
    2: lambda n: 0,       # PTR
    3: lambda n: 12,      # ARRAY
    4: lambda n: 12 * n,  # STRUCT
    5: lambda n: 12 * n,  # UNION
    6: lambda n: 8 * n,   # ENUM
    7: lambda n: 0,       # FWD
    8: lambda n: 0,       # TYPEDEF
    9: lambda n: 0,       # VOLATILE
    10: lambda n: 0,      # CONST
    11: lambda n: 0,      # RESTRICT
    12: lambda n: 0,      # FUNC
    13: lambda n: 8 * n,  # FUNC_PROTO
    14: lambda n: 4,      # VAR
    15: lambda n: 12 * n, # DATASEC
    16: lambda n: 0,      # FLOAT
}


def readelf(readelf_cmd, option, path):
    return subprocess.check_output([readelf_cmd, "-W", option, path], text=True)


def cstring(blob, offset):
    if offset >= len(blob):
        raise ValueError("BTF string offset outside string section")
    end = blob.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated BTF string")
    return blob[offset:end].decode("utf-8")


def main(path, readelf_cmd):
    sections = {}
    for line in readelf(readelf_cmd, "-S", path).splitlines():
        match = SECTION.match(line)
        if match:
            index, name, address, offset, size = match.groups()
            sections[name] = tuple(int(x, 16) for x in (address, offset, size)) + (int(index),)
    if ".BTF" not in sections or ".data..percpu" not in sections:
        raise ValueError("missing .BTF or .data..percpu ELF section")
    percpu_addr, _, percpu_size, percpu_index = sections[".data..percpu"]
    btf_addr, btf_offset, btf_size, _ = sections[".BTF"]
    del btf_addr

    symbols = collections.defaultdict(list)
    for line in readelf(readelf_cmd, "-s", path).splitlines():
        match = SYMBOL.match(line)
        if not match:
            continue
        address, size, index, name = match.groups()
        if int(index) == percpu_index:
            offset = int(address, 16) - percpu_addr
            size = int(size)
            if offset < 0 or offset + size > percpu_size:
                raise ValueError("per-CPU ELF symbol outside section: " + name)
            symbols[name].append((offset, size))

    with open(path, "rb") as elf:
        elf.seek(btf_offset)
        btf = bytearray(elf.read(btf_size))
    if len(btf) != btf_size:
        raise ValueError("truncated BTF section")
    magic, version, flags, header_len, type_off, type_len, str_off, str_len = \
        struct.unpack_from("<HBBIIIII", btf)
    if magic != 0xeb9f or version != 1 or flags:
        raise ValueError("unsupported BTF header")
    type_start = header_len + type_off
    type_end = type_start + type_len
    str_start = header_len + str_off
    str_end = str_start + str_len
    if type_end > len(btf) or str_end > len(btf):
        raise ValueError("BTF type or string section exceeds ELF section")
    strings = btf[str_start:str_end]

    var_names = {}
    entries = []
    datasec_payload = None
    pos = type_start
    type_id = 1
    while pos < type_end:
        if pos + 12 > type_end:
            raise ValueError("truncated BTF type")
        name_off, info, size = struct.unpack_from("<III", btf, pos)
        kind = (info >> 24) & 0x1f
        vlen = info & 0xffff
        if kind not in EXTRA_SIZE:
            raise ValueError("unsupported BTF kind %d" % kind)
        payload = pos + 12
        next_pos = payload + EXTRA_SIZE[kind](vlen)
        if next_pos > type_end:
            raise ValueError("truncated BTF type payload")
        if kind == 14:
            var_names[type_id] = cstring(strings, name_off)
        elif kind == 15 and cstring(strings, name_off) == ".data..percpu":
            if datasec_payload is not None:
                raise ValueError("multiple per-CPU DATASEC types")
            datasec_payload = payload
            if size != percpu_size:
                raise ValueError("per-CPU BTF and ELF section sizes differ")
            for member in range(vlen):
                entry_pos = payload + 12 * member
                var_id, offset, var_size = struct.unpack_from("<III", btf, entry_pos)
                if var_id not in var_names:
                    raise ValueError("per-CPU DATASEC entry has no VAR type")
                entries.append((entry_pos + 4, var_names[var_id], offset, var_size))
        pos = next_pos
        type_id += 1
    if pos != type_end or not entries:
        raise ValueError("invalid or missing per-CPU DATASEC entries")

    # Exact matches reserve their symbol first.  For repeated static names,
    # pair the remaining entries and ELF symbols in address order.
    assigned = {}
    by_name = collections.defaultdict(list)
    for entry in entries:
        by_name[entry[1]].append(entry)
    for name, group in by_name.items():
        candidates = list(symbols.get(name, ()))
        if len(candidates) != len(group):
            raise ValueError("per-CPU symbol count differs for " + name)
        pending = []
        for entry in group:
            exact = (entry[2], entry[3])
            if exact in candidates:
                assigned[entry[0]] = exact[0]
                candidates.remove(exact)
            else:
                pending.append(entry)
        for entry, candidate in zip(pending, sorted(candidates)):
            if entry[3] != candidate[1]:
                raise ValueError("per-CPU variable size differs for " + name)
            assigned[entry[0]] = candidate[0]

    ordered = sorted((assigned[pos], size, name) for pos, name, _, size in entries)
    end = 0
    for offset, size, name in ordered:
        if offset < end or offset + size > percpu_size:
            raise ValueError("overlapping corrected per-CPU variable: " + name)
        end = offset + size

    fixes = sum(assigned[pos] != old_offset
                for pos, _, old_offset, _ in entries)
    corrected = []
    for pos, _, _, size in entries:
        var_id = struct.unpack_from("<I", btf, pos - 4)[0]
        corrected.append((var_id, assigned[pos], size))
    corrected.sort(key=lambda entry: entry[1])
    reordered = 0
    for index, item in enumerate(corrected):
        entry_pos = datasec_payload + 12 * index
        if struct.unpack_from("<III", btf, entry_pos) != item:
            reordered += 1
            struct.pack_into("<III", btf, entry_pos, *item)
    if reordered:
        with open(path, "r+b") as elf:
            elf.seek(btf_offset)
            elf.write(btf)
    print("BTF: checked %d per-CPU variables, corrected %d offsets and "
          "reordered %d entries" % (len(entries), fixes, reordered))


if __name__ == "__main__":
    try:
        main(sys.argv[1], sys.argv[2])
    except (IndexError, ValueError, subprocess.CalledProcessError) as error:
        print("BTF per-CPU offset fix failed: %s" % error, file=sys.stderr)
        sys.exit(1)
