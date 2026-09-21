#!/usr/bin/env python3
"""Manage the firmware version_id for PY5OL OTA builds.

Two subcommands, both driven by `make ota`:

  bump   Rewrite the `version_id` line in RX_FSK/version.h to "py5ol<timestamp>"
         (local time, YYYYMMDDHHMMSS) so every build gets a unique id. Run BEFORE
         the compile so the new id is baked into the binary.

  info   Read version.h and print "<version_id>-<Letter><Number>" (Letter = FS_MAJOR
         as A=1..., Number = FS_MINOR) — the string served as update-info.html and
         used by upd.html's compatibility gating.

The version_id prefix is intentionally fixed to "py5ol" to identify this fork/server.
"""
import argparse
import hashlib
import re
import sys
from datetime import datetime
from pathlib import Path

VERSION_H = Path(__file__).resolve().parent.parent / "RX_FSK" / "version.h"
PREFIX = "py5ol-"

VERSION_ID_RE = re.compile(r'(const\s+char\s*\*\s*version_id\s*=\s*")([^"]*)(";)')
FS_MAJOR_RE = re.compile(r'FS_MAJOR\s*=\s*(\d+)')
FS_MINOR_RE = re.compile(r'FS_MINOR\s*=\s*(\d+)')
# Baseline hash of the LittleFS source, stored as a comment in version.h so it stays
# next to (and in sync with) FS_MINOR. Written/updated by the 'fsbump' subcommand.
FSHASH_RE = re.compile(r'//\s*ota_fsdata_sha256:\s*([0-9a-fA-F]+)')


def read_version_h():
    try:
        return VERSION_H.read_text()
    except OSError as e:
        sys.exit(f"ota_version: cannot read {VERSION_H}: {e}")


def find(regex, text, what):
    m = regex.search(text)
    if not m:
        sys.exit(f"ota_version: could not find {what} in {VERSION_H}")
    return m


def cmd_bump(_args):
    text = read_version_h()
    find(VERSION_ID_RE, text, "version_id")  # validate presence before writing
    new_id = PREFIX + datetime.now().strftime("%Y%m%d%H%M%S")
    new_text = VERSION_ID_RE.sub(lambda m: m.group(1) + new_id + m.group(3), text, count=1)
    VERSION_H.write_text(new_text)
    print(new_id)


def cmd_info(_args):
    text = read_version_h()
    version_id = find(VERSION_ID_RE, text, "version_id").group(2)
    fs_major = int(find(FS_MAJOR_RE, text, "FS_MAJOR").group(1))
    fs_minor = int(find(FS_MINOR_RE, text, "FS_MINOR").group(1))
    letter = chr(ord("A") + fs_major - 1)
    print(f"{version_id}-{letter}{fs_minor}")


def hash_data_dir(datadir):
    """Deterministic hash of exactly the files that go into update.fs.bin.

    Mirrors makefsupdate.py: the OTA archive contains only the TOP-LEVEL .js/.html/.css
    files of RX_FSK/data (listdir is non-recursive and filtered by extension) -- not
    subdirectories, fonts, screens, .txt or images. Those extras live in the full
    LittleFS partition but are not OTA-deliverable, so a change to them is a manual
    FS_MAJOR / re-flash decision, not an automatic FS_MINOR bump. Hash the same set,
    sorted, so FS_MINOR bumps exactly when the online-updatable content changes.
    Keep this selection in sync with scripts/makefsupdate.py."""
    root = Path(datadir)
    if not root.is_dir():
        sys.exit(f"ota_version: data dir not found: {root}")
    exts = (".js", ".html", ".css")
    h = hashlib.sha256()
    for f in sorted(p for p in root.iterdir() if p.is_file() and p.suffix in exts):
        h.update(f.name.encode())
        h.update(b"\0")
        h.update(f.read_bytes())
        h.update(b"\0")
    return h.hexdigest()


def cmd_fsbump(args):
    """Increment FS_MINOR when the filesystem source changed since the recorded hash.

    FS_MAJOR (filesystem-layout compatibility -> forces a USB re-flash) stays manual:
    an incompatible layout change is a semantic decision a content diff can't infer.
    The first run just records the baseline hash and does not bump.
    """
    text = read_version_h()
    current = hash_data_dir(args.datadir)
    m = FSHASH_RE.search(text)
    if not m:
        # First run: establish the baseline, do not bump.
        if not text.endswith("\n"):
            text += "\n"
        VERSION_H.write_text(text + f"// ota_fsdata_sha256: {current}\n")
        print(f"fs baseline recorded (no bump): {current[:12]}")
        return
    if m.group(1).lower() == current.lower():
        print("fs unchanged (FS_MINOR kept)")
        return
    old_minor = int(find(FS_MINOR_RE, text, "FS_MINOR").group(1))
    new_minor = old_minor + 1
    text = re.sub(r'(FS_MINOR\s*=\s*)\d+', lambda mm: mm.group(1) + str(new_minor), text, count=1)
    text = FSHASH_RE.sub(f"// ota_fsdata_sha256: {current}", text, count=1)
    VERSION_H.write_text(text)
    print(f"fs changed -> FS_MINOR {old_minor} -> {new_minor}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("bump", help="rewrite version_id to py5ol-<timestamp>").set_defaults(func=cmd_bump)
    sub.add_parser("info", help="print <version_id>-<Letter><Number>").set_defaults(func=cmd_info)
    fsb = sub.add_parser("fsbump", help="bump FS_MINOR if the RX_FSK/data tree changed")
    fsb.add_argument("datadir", help="LittleFS source dir (e.g. RX_FSK/data)")
    fsb.set_defaults(func=cmd_fsbump)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
