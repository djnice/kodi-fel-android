#!/usr/bin/env python3
"""Module version (CRC) helper for building dvfel against the Dune 5.4 GKI kernel.

The Dune kernel source is not available; dvfel is built against a reconstructed
header set whose layouts were verified against the Dune binaries, but genksyms
CRCs still differ (nested types we do not use). This tool:

  db   <out.json> <ko...>   collect symbol -> CRC from the __versions sections
                            and exported __crc_* symbols of reference modules
  symvers <db.json> <out> <ko...>
                            write a Module.symvers (KBUILD_EXTRA_SYMBOLS) with the
                            Dune CRCs; <ko...> name the modules owning exports
  show <ko>                 list the __versions entries of a module
  fix  <db.json> <ko>       rewrite the __versions CRCs of <ko> (our own module)
                            to the Dune values; fails on unknown symbols
"""
import json
import struct
import subprocess
import sys

ENTRY = 64  # struct modversion_info on 64-bit: unsigned long crc; char name[56]


def sections(path):
    out = subprocess.check_output(["readelf", "-S", "-W", path]).decode()
    res = {}
    for line in out.splitlines():
        parts = line.replace("[ ", "[").split()
        if len(parts) > 6 and parts[0].startswith("[") and parts[0] != "[Nr]":
            res[parts[1]] = (int(parts[4], 16), int(parts[5], 16))
    return res


def versions(path):
    sec = sections(path).get("__versions")
    if not sec:
        return []
    off, size = sec
    data = open(path, "rb").read()[off:off + size]
    res = []
    for i in range(0, len(data), ENTRY):
        crc = struct.unpack_from("<Q", data, i)[0]
        name = data[i + 8:i + ENTRY].split(b"\0")[0].decode()
        res.append((i + off, name, crc))
    return res


def exported(path):
    out = subprocess.check_output(["nm", path]).decode()
    res = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] == "A" and p[2].startswith("__crc_"):
            res[p[2][6:]] = int(p[0], 16) & 0xffffffff
    return res


def main():
    cmd = sys.argv[1]
    if cmd == "db":
        db, conflicts = {}, 0
        for ko in sys.argv[3:]:
            items = [(n, c) for _, n, c in versions(ko)] + list(exported(ko).items())
            for name, crc in items:
                if name in db and db[name] != crc:
                    conflicts += 1
                    print(f"conflict {name}: {db[name]:#x} vs {crc:#x} ({ko})", file=sys.stderr)
                db.setdefault(name, crc)
        json.dump(db, open(sys.argv[2], "w"), indent=0, sort_keys=True)
        print(f"{len(db)} symbols, {conflicts} conflicts")
    elif cmd == "symvers":
        # symvers <db.json> <out Module.symvers> <exporting ko...>: a Module.symvers for
        # KBUILD_EXTRA_SYMBOLS so modpost writes the Dune CRCs into __versions directly
        db = json.load(open(sys.argv[2]))
        owner = {}
        for ko in sys.argv[4:]:
            mod = ko.rsplit("/", 1)[-1][:-3].replace("-", "_")
            for name in exported(ko):
                owner.setdefault(name, mod)
        with open(sys.argv[3], "w") as f:
            for name in sorted(db):
                f.write(f"0x{db[name]:08x}\t{name}\t{owner.get(name, 'vmlinux')}\tEXPORT_SYMBOL\t\n")
        print(f"{len(db)} symbols, {len(owner)} module exports")
    elif cmd == "show":
        for off, name, crc in versions(sys.argv[2]):
            print(f"{crc:#010x} {name}")
    elif cmd == "fix":
        db = json.load(open(sys.argv[2]))
        ko = sys.argv[3]
        data = bytearray(open(ko, "rb").read())
        missing, changed = [], 0
        for off, name, crc in versions(ko):
            if name not in db:
                missing.append(name)
                continue
            if db[name] != crc:
                struct.pack_into("<Q", data, off, db[name])
                changed += 1
                print(f"{name}: {crc:#010x} -> {db[name]:#010x}")
        if missing:
            print("unknown symbols (not imported by any Dune module):", " ".join(missing))
            sys.exit(1)
        open(ko, "wb").write(data)
        print(f"{changed} CRCs rewritten")


if __name__ == "__main__":
    main()
