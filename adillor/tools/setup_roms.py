#!/usr/bin/env python3
"""Put the Armadillo Racing ROM files where the game looks for them (extracted/), and
build adillor_main.bin, the 4 MB 68EC020 program (MAME ROM_LOAD32_BYTE:
am2vera.rom4 -> byte 0, .rom3 -> 1, .rom2 -> 2, .rom1 -> 3 of each long).

    python3 tools/setup_roms.py /path/to/adillor.zip
    python3 tools/setup_roms.py /path/to/folder-with-the-rom-files

Accepts MAME's set adillor.zip (World, AM2 Ver.A) or a folder holding the same chips or the zip.
Every chip is found by its CRC32 (the name is only a fallback for the file written), because MAME
sets have shipped the same data under older names (am1cg3.14d holds am1cg4.14d's data in some zips).
Exits 0 when extracted/ is complete.
"""
import os
import sys
import zipfile
import zlib

# canonical name -> (size, crc32), from MAME's ROM_START( adillor ). The duplicate board copies
# (am1cg*.1xb, am1ccr*.7b/5b, am1scg*.1xl) carry the same data and are not needed.
REQUIRED = {
    "am2vera.rom1": (0x100000, 0xb974e931),
    "am2vera.rom2": (0x100000, 0xc7cd78f1),
    "am2vera.rom3": (0x100000, 0x80e9435a),
    "am2vera.rom4": (0x100000, 0x1e3a6032),
    "am1data.8k":   (0x80000,  0x3c176589),
    "am1scg0.12f":  (0x200000, 0xe576f993),
    "am1scg1.10f":  (0x200000, 0xd6d26cc1),
    "am1cg0.8d":    (0x200000, 0xb91e4259),
    "am1cg1.10d":   (0x200000, 0xc1b82a26),
    "am1cg2.12d":   (0x200000, 0x36a7d3fd),
    "am1cg3.13d":   (0x200000, 0x8b0857ef),
    "am1cg4.14d":   (0x200000, 0xebc516fe),
    "am1cg5.16d":   (0x200000, 0x30fad30d),
    "am1cg6.18d":   (0x200000, 0x62eea883),
    "am1cg7.19d":   (0x200000, 0x2d257f47),
    "am1ccrl.3d":   (0x200000, 0x76c6d5f3),
    "am1ccrh.1d":   (0x80000,  0x180c4a33),
    "am1ptrl0.18k": (0x80000,  0x6fdb34f6),
    "am1ptrl1.16k": (0x80000,  0x32e40601),
    "am1ptrl2.15k": (0x80000,  0xe27e29cc),
    "am1ptrl3.14k": (0x80000,  0x0c638e3e),
    "am1ptrm0.18j": (0x80000,  0xe676cd98),
    "am1ptrm1.16j": (0x80000,  0xeb115d9d),
    "am1ptrm2.15j": (0x80000,  0x86b4ed0c),
    "am1ptrm3.14j": (0x80000,  0x97c1c6b0),
    "am1ptru0.18f": (0x80000,  0x6a3fd855),
    "am1ptru1.16f": (0x80000,  0x551bf714),
    "am1ptru2.15f": (0x80000,  0xce790635),
    "am1ptru3.14f": (0x80000,  0x5e4e6333),
    "am1wavea.2l":  (0x400000, 0x48f8c20c),
    "am1waveb.1l":  (0x400000, 0xfd8e7384),
    "c71.bin":      (0x2000,   0x47c623ab),
}
BY_CRC = {crc: name for name, (size, crc) in REQUIRED.items()}

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
DEST = os.path.join(TOP, "extracted")


def have_complete(folder):
    for name, (size, _) in REQUIRED.items():
        p = os.path.join(folder, name)
        if not os.path.isfile(p) or os.path.getsize(p) != size:
            return False
    return True


def take(found, data):
    name = BY_CRC.get(zlib.crc32(data) & 0xffffffff)
    if name and len(data) == REQUIRED[name][0]:
        found[name] = data


def from_zip(path):
    """the zip's top level only: MAME's adillor.zip also carries the Japanese program under adillorj/"""
    found = {}
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            if info.is_dir() or "/" in info.filename.strip("/"):
                continue
            if info.CRC in BY_CRC:
                take(found, z.read(info))
    return found


def from_folder(path):
    found = {}
    for entry in sorted(os.listdir(path)):
        p = os.path.join(path, entry)
        if os.path.isfile(p) and os.path.getsize(p) in {s for s, _ in REQUIRED.values()}:
            with open(p, "rb") as f:
                take(found, f.read())
    if not all(n in found for n in REQUIRED):
        for entry in sorted(os.listdir(path)):
            if entry.lower() in ("adillor.zip",):
                for k, v in from_zip(os.path.join(path, entry)).items():
                    found.setdefault(k, v)
    return found


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__.strip())
        return 2
    if args[0] == "--check":
        return 0 if have_complete(DEST) else 1
    found = {}
    for a in args:
        src = os.path.expanduser(a)
        if not os.path.exists(src):
            print(f"Can't find '{src}'. Check the path.")
            return 1
        try:
            found.update(from_folder(src) if os.path.isdir(src) else from_zip(src))
        except zipfile.BadZipFile:
            print(f"'{src}' is not a zip file (or it is damaged).")
            return 1
    missing = [n for n in REQUIRED if n not in found]
    if missing:
        print("This ROM set can't be used:")
        for n in missing:
            print(f"  missing (or a bad dump): {n}  ({REQUIRED[n][0]} bytes, CRC {REQUIRED[n][1]:08x})")
        print("You need MAME's Armadillo Racing (World, AM2 Ver.A) set 'adillor'.")
        return 1
    os.makedirs(DEST, exist_ok=True)
    for name, data in found.items():
        with open(os.path.join(DEST, name), "wb") as f:
            f.write(data)
    lanes = [found["am2vera.rom%d" % k] for k in (4, 3, 2, 1)]
    prog = bytearray(4 * len(lanes[0]))
    for l in range(4):
        prog[l::4] = lanes[l]
    with open(os.path.join(TOP, "adillor_main.bin"), "wb") as f:
        f.write(prog)
    print(f"ROMs OK: {len(found)} files copied into extracted/, adillor_main.bin built")
    return 0


if __name__ == "__main__":
    sys.exit(main())
