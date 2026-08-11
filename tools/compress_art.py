#!/usr/bin/env python3
"""
Shrink embedded album art in MP3/FLAC files, in place.

The now-playing screen's art panel is small (104x104px), but embedded cover
art is usually sized for a phone or a desktop player -- 500 to 1500px on a
side, hundreds of KB to a few MB per track. This resizes it down to fit the
panel (with some headroom) and re-encodes it as JPEG, which typically cuts
each file's art down to a handful of KB.

If a file has more than one embedded picture (front cover, back cover, liner
notes, ...), only the largest is kept -- the player only ever shows one.

This OVERWRITES the files it touches. If you want to keep full-resolution art
anywhere, run this against a copy of your library, not the original, or use
--dry-run first to see what it would do.

Setup:
    pip install mutagen pillow

Usage:
    python compress_art.py D:\\Music\\ToCopy
    python compress_art.py D:\\Music\\ToCopy --dry-run
    python compress_art.py D:\\Music\\ToCopy --max-size 200 --quality 80

    # Automatic: compress whatever's already there, then keep running and
    # catch new files as you copy them in. Ctrl+C to stop.
    python compress_art.py D:\\Music\\ToCopy --watch
"""
import argparse
import io
import sys
import time
from pathlib import Path

from PIL import Image
from mutagen.flac import FLAC, Picture
from mutagen.id3 import ID3, APIC, ID3NoHeaderError

EXTS = {".mp3", ".flac"}


def compress_image(data, max_size, quality):
    """Resize+recompress image bytes as JPEG. Returns None if the result
    wouldn't actually be smaller (e.g. already a small icon)."""
    try:
        img = Image.open(io.BytesIO(data))
        img.load()
    except Exception:
        return None

    if img.mode != "RGB":
        img = img.convert("RGB")

    img.thumbnail((max_size, max_size), Image.LANCZOS)

    out = io.BytesIO()
    img.save(out, format="JPEG", quality=quality, optimize=True)
    compressed = out.getvalue()

    if len(compressed) >= len(data):
        return None
    return compressed


def process_mp3(path, max_size, quality, dry_run):
    try:
        tags = ID3(path)
    except ID3NoHeaderError:
        return None

    apics = tags.getall("APIC")
    if not apics:
        return None

    best = max(apics, key=lambda f: len(f.data))
    original_total = sum(len(f.data) for f in apics)

    compressed = compress_image(best.data, max_size, quality)
    if compressed is None:
        return (original_total, original_total)

    tags.delall("APIC")
    tags.add(APIC(
        encoding=3,
        mime="image/jpeg",
        type=3,  # front cover
        desc="Cover",
        data=compressed,
    ))

    if not dry_run:
        tags.save(path)

    return (original_total, len(compressed))


def process_flac(path, max_size, quality, dry_run):
    audio = FLAC(path)
    if not audio.pictures:
        return None

    original_total = sum(len(p.data) for p in audio.pictures)
    best = max(audio.pictures, key=lambda p: len(p.data))

    compressed = compress_image(best.data, max_size, quality)
    if compressed is None:
        return (original_total, original_total)

    pic = Picture()
    pic.type = 3
    pic.mime = "image/jpeg"
    pic.desc = "Cover"
    pic.data = compressed

    audio.clear_pictures()
    audio.add_picture(pic)

    if not dry_run:
        audio.save()

    return (original_total, len(compressed))


def process_one(path, max_size, quality, dry_run):
    handler = process_mp3 if path.suffix.lower() == ".mp3" else process_flac
    try:
        return handler(path, max_size, quality, dry_run)
    except Exception as e:
        print(f"  ! {path.name}: {e}")
        return None


def human(n):
    n = float(n)
    for unit in ("B", "KB", "MB"):
        if n < 1024:
            return f"{n:.0f}{unit}"
        n /= 1024
    return f"{n:.1f}GB"


def scan_once(folder, max_size, quality, dry_run):
    """One pass over every .mp3/.flac currently under folder. Returns the
    number of files whose art was actually shrunk."""
    files = sorted(p for p in folder.rglob("*") if p.suffix.lower() in EXTS)
    if not files:
        print("No .mp3/.flac files found.")
        return 0

    total_before = 0
    total_after = 0
    touched = 0

    for path in files:
        result = process_one(path, max_size, quality, dry_run)
        if result is None:
            continue

        before, after = result
        total_before += before
        total_after += after
        if after < before:
            touched += 1
            print(f"  {path.name}: {human(before)} -> {human(after)}")

    saved = total_before - total_after
    verb = "Would save" if dry_run else "Saved"
    print(f"\n{touched} file(s) with art compressed. {verb} {human(saved)} "
          f"({human(total_before)} -> {human(total_after)}).")
    if dry_run:
        print("Dry run -- nothing was written.")
    return touched


def _is_stable(path, st, settle=0.3):
    """A file mid-copy keeps growing; wait a moment and confirm size/mtime
    didn't move before touching it, so a big transfer isn't read half-done."""
    time.sleep(settle)
    try:
        st2 = path.stat()
    except OSError:
        return False
    return st2.st_size == st.st_size and st2.st_mtime == st.st_mtime and st.st_size > 0


def watch(folder, max_size, quality, dry_run, interval=2.0):
    """Poll folder for new or changed .mp3/.flac files and compress each one
    as it settles. Simple polling rather than a filesystem-events library
    (no extra dependency, and this is a personal music folder, not something
    that needs sub-second reaction time)."""
    print(f"Watching {folder} for new/changed .mp3/.flac files "
          f"(checking every {interval:.0f}s). Ctrl+C to stop.\n")

    last_seen = {}   # path -> (mtime, size) as of the last time we handled it
    try:
        while True:
            for path in sorted(p for p in folder.rglob("*") if p.suffix.lower() in EXTS):
                try:
                    st = path.stat()
                except OSError:
                    continue

                if last_seen.get(path) == (st.st_mtime, st.st_size):
                    continue   # unchanged since we last handled it

                if not _is_stable(path, st):
                    continue   # still being written; catch it next pass

                result = process_one(path, max_size, quality, dry_run)

                try:
                    st = path.stat()   # re-stat: our own edit changed mtime/size
                except OSError:
                    pass
                last_seen[path] = (st.st_mtime, st.st_size)

                if result:
                    before, after = result
                    if after < before:
                        print(f"  {path.name}: {human(before)} -> {human(after)}")

            time.sleep(interval)
    except KeyboardInterrupt:
        print("\nStopped.")


def main():
    # Line-buffer stdout: without this, output sits in a buffer and doesn't
    # appear until it fills up or the process exits -- fine for the one-shot
    # scan, but useless for --watch, where the whole point is seeing activity
    # as it happens.
    sys.stdout.reconfigure(line_buffering=True)

    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", type=Path,
                    help="Folder to scan recursively for .mp3/.flac files")
    ap.add_argument("--max-size", type=int, default=160,
                    help="Max width/height in px (default: 160, matches the player's art panel)")
    ap.add_argument("--quality", type=int, default=85,
                    help="JPEG quality 1-95 (default: 85)")
    ap.add_argument("--dry-run", action="store_true",
                    help="Report what would change without writing anything")
    ap.add_argument("--watch", action="store_true",
                    help="After the initial pass, keep running and automatically "
                         "compress art in new/changed files as they show up "
                         "under folder (e.g. while you copy an album in). "
                         "Stop with Ctrl+C.")
    args = ap.parse_args()

    if not args.folder.is_dir():
        print(f"Not a folder: {args.folder}", file=sys.stderr)
        sys.exit(1)

    scan_once(args.folder, args.max_size, args.quality, args.dry_run)

    if args.watch:
        watch(args.folder, args.max_size, args.quality, args.dry_run)


if __name__ == "__main__":
    main()
