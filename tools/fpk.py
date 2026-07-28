#!/usr/bin/env python3
"""Extract and rebuild Sid Meier's Pirates .FPK archives.

The format is the Firaxis one also used by Civ4:

    u32 version (always 2)
    u32 asset_count
    asset_count x {
        u32   filename_length
        char  filename[round_up(filename_length, 4)]   each byte +1; see below
        u32   checksum
        u32   tag
        u32   length
        u32   offset
    }
    ...file bodies, in offset order, with the gaps preserved verbatim...

Two details that are easy to get wrong:

  * **Filenames are obfuscated**, every byte incremented by one, and padded up to
    a 4-byte boundary. The padding is not zeroes: it is `bytes([n]) + b"\\0" * (n-1)`
    where n is the number of padding bytes, which is why PADDINGS is indexed by
    *padding length* rather than by `len(name) % 4`.
  * **The namespace is flat.** `check_path_safe` rejects any separator, so an
    archive cannot carry a directory and an entry cannot escape the output folder.

Round-tripping is byte-exact: everything not described by the index -- alignment
gaps, trailing bytes -- is captured in the `_db.json` as `padding` parts and
written back unchanged. Verified on lang0.FPK: extract then assemble reproduces
the original file byte for byte.

    fpk.py d <dir> <file.FPK> [...]     extract, writing <dir>_db.json
    fpk.py a <dir> <file.FPK> [...]     rebuild from <dir> and <dir>_db.json

⚠️ `checksum` and `tag` are carried through from the index, NOT recomputed. A file
whose contents you changed goes back in with its original checksum. Whether the
engine validates it is an open question -- see docs/ASSETS.md.
"""

from __future__ import annotations

import binascii
import json
import os
import sys
from os import fstat, makedirs, mkdir, path
from struct import pack, unpack

PADDINGS = [b"", b"\x01", b"\x02\0", b"\x03\0\0"]
encodebin = lambda s: binascii.b2a_hex(s).decode("ascii")
decodebin = lambda s: binascii.a2b_hex(s)


def check_path_safe(p: str) -> bool:
    p = path.normpath(p)
    return p == path.normpath(p) and ("/" not in p) and ("\\" not in p)


def read_asset(fpk):
    filename_length = unpack("<I", fpk.read(4))[0]
    filename_blocks = (filename_length + 3) // 4
    filename = fpk.read(filename_blocks * 4)
    assert len(filename) == filename_blocks * 4
    padding = filename[filename_length:]
    assert padding == PADDINGS[len(padding)]
    filename = bytes((x - 1) % 256 for x in filename[:filename_length]).decode("utf-8")
    assert check_path_safe(filename)
    checksum, tag, length, offset = unpack("<IIII", fpk.read(16))
    return {
        "filename": filename,
        "checksum": checksum,
        "tag": tag,
        "length": length,
        "offset": offset,
    }


def extract(fpk, folder, db):
    fpk_size = fstat(fpk.fileno()).st_size
    version, asset_count = unpack("<II", fpk.read(8))
    assert version == 2
    assets = [read_asset(fpk) for _ in range(asset_count)]
    position = fpk.tell()
    parts = []
    for asset in assets:
        assert asset["offset"] >= position and asset["offset"] + asset["length"] <= fpk_size
        if asset["offset"] > position:
            length = asset["offset"] - position
            contents = fpk.read(length)
            assert len(contents) == length
            parts.append({"padding": True, "contents": encodebin(contents)})
        for fpk_name, fpk_db in db["fpks"].items():
            filenames = [part["filename"] for part in fpk_db["parts"] if "filename" in part]
            if asset["filename"] in filenames:
                raise Exception(f"Filename {asset['filename']} already belongs to FPK '{fpk_name}'")
        contents = fpk.read(asset["length"])
        assert len(contents) == asset["length"]
        position = asset["offset"] + asset["length"]
        print(f"Extracting file: {asset['filename']}")
        file_to_write = path.join(folder, asset["filename"])
        makedirs(path.dirname(file_to_write), exist_ok=True)
        with open(file_to_write, "wb") as f:
            f.write(contents)
        entry = dict(asset)
        del entry["length"]
        del entry["offset"]
        parts.append(entry)
    assert fpk_size >= position
    if fpk_size > position:
        contents = fpk.read(fpk_size - position)
        assert len(contents) == (fpk_size - position)
        parts.append({"padding": True, "contents": encodebin(contents)})
    return {"version": version, "parts": parts}


def assemble(fpk, folder, fpk_db):
    # The filename field is a whole number of 4-byte BLOCKS, but it is measured
    # here in BYTES -- so the block count is multiplied back up. Counting blocks
    # as bytes under-sizes the header by three quarters of every filename, and
    # the mismatch only surfaces at the assert below, after the archive has
    # already been written.
    get_part_length = lambda part: 4 + ((len(part["filename"]) + 3) // 4) * 4 + 16
    header_size = 8 + sum(get_part_length(part) for part in fpk_db["parts"] if "padding" not in part)
    fpk.seek(header_size)
    header = pack(
        "<II",
        fpk_db["version"],
        sum(int("padding" not in part) for part in fpk_db["parts"]),
    )
    position = header_size
    for part in fpk_db["parts"]:
        if part.get("padding"):
            fpk.write(decodebin(part["contents"]))
            position = fpk.tell()
            continue
        print(f"Assembling file: {part['filename']}")
        file_to_read = path.join(folder, part["filename"])
        with open(file_to_read, "rb") as f:
            fpk.write(f.read())
        filename = bytes((x + 1) % 256 for x in part["filename"].encode("utf-8"))
        # PADDINGS is indexed by PADDING LENGTH -- that is how read_asset checks
        # it -- not by `len(filename) % 4`. The two agree only by accident at
        # length 2, so a name of any other length round-trips to different bytes.
        filename += PADDINGS[(4 - len(filename) % 4) % 4]
        offset, position = position, fpk.tell()
        header += pack("<I", len(part["filename"])) + filename + pack(
            "<IIII", part["checksum"], part["tag"], position - offset, offset
        )
    fpk.seek(0)
    assert len(header) == header_size
    fpk.write(header)
    print("Header written.")


DB_VERSION = 1


def main(argv: list[str]) -> int:
    if len(argv) < 3 or argv[1] not in ("d", "a"):
        print(
            "Usage:\n  fpkextract.py d <extract_dir> <file.FPK> [...]\n  fpkextract.py a <extract_dir> <file.FPK> [...]",
            file=sys.stderr,
        )
        return 1
    folder, fpks = argv[2], argv[3:]
    db_file = path.normpath(folder) + "_db.json"
    if argv[1] == "d":
        if path.exists(db_file):
            with open(db_file, "r", encoding="utf-8") as f:
                db = json.loads(f.read())
            assert db["version"] == DB_VERSION
        else:
            db = {"version": DB_VERSION, "fpks": {}}
        if not path.exists(folder):
            mkdir(folder)
        for fpk_file in fpks:
            fpk_name = path.normpath(path.relpath(fpk_file, start=path.dirname(folder)))
            if fpk_name in db["fpks"]:
                del db["fpks"][fpk_name]
            with open(fpk_file, "rb") as fpk:
                print(f"Processing FPK: {fpk_name}")
                db["fpks"][fpk_name] = extract(fpk, folder, db)
        with open(db_file, "w", encoding="utf-8") as f:
            f.write(json.dumps(db, indent=4, sort_keys=True, ensure_ascii=False) + "\n")
    else:
        with open(db_file, "r", encoding="utf-8") as f:
            db = json.loads(f.read())
        assert db["version"] == DB_VERSION
        for fpk_file in fpks:
            fpk_name = path.normpath(path.relpath(fpk_file, start=path.dirname(folder)))
            if fpk_name not in db["fpks"]:
                raise Exception(f"FPK not found in DB: {fpk_name}")
            # Build beside the target and move into place only on success. The
            # obvious `open(fpk_file, "wb")` destroys the archive the moment it
            # opens, so any fault in assemble leaves a truncated file where a
            # 100 MB game asset used to be -- and these archives ship with the
            # game, so there is nothing to restore from.
            tmp = fpk_file + ".partial"
            try:
                with open(tmp, "wb") as fpk:
                    print(f"Processing FPK: {fpk_name}")
                    assemble(fpk, folder, db["fpks"][fpk_name])
                os.replace(tmp, fpk_file)
            except BaseException:
                if path.exists(tmp):
                    os.remove(tmp)
                raise
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
