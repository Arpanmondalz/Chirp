#!/usr/bin/env python3
"""Normalise a music folder for the Chirp player.

For every audio file found, this:
  - re-encodes to 192 kbps stereo MP3 (44.1 kHz)
  - strips album art and every tag except title and artist
  - rewrites title/artist and the filename to characters the OLED can render
  - writes the result to <folder>_cleaned next to the source folder

The Chirp firmware shows the ID3 title/artist and falls back to the filename,
so both are sanitised to the u8g2 "_tf" glyph range (U+0020..U+00FF).

Set MUSIC_FOLDER below, then run: python clean_music.py
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import unicodedata
from pathlib import Path

# ---------------------------------------------------------------
MUSIC_FOLDER = r"D:\Music"
# ---------------------------------------------------------------

BITRATE = "192k"

try:
    from tqdm import tqdm
except ImportError:
    sys.exit("Missing dependency. Run: pip install -r requirements.txt")

AUDIO_EXTS = {
    ".mp3", ".m4a", ".m4b", ".aac", ".flac", ".wav", ".wave", ".ogg",
    ".oga", ".opus", ".wma", ".aif", ".aiff", ".alac", ".ape", ".mp2",
}

# The OLED fonts cover U+0020..U+00FF, so anything outside that is folded to
# an ASCII lookalike here or dropped below.
TRANSLITERATE = {
    "\u2018": "'", "\u2019": "'", "\u201a": "'", "\u201b": "'",
    "\u201c": '"', "\u201d": '"', "\u201e": '"', "\u2032": "'", "\u2033": '"',
    "\u2010": "-", "\u2011": "-", "\u2012": "-", "\u2013": "-",
    "\u2014": "-", "\u2015": "-", "\u2212": "-",
    "\u2026": "...", "\u2022": "-", "\u00b7": "-", "\u2044": "/",
    "\u00a0": " ", "\u2009": " ", "\u200a": " ", "\u202f": " ",
    "\u200b": "", "\u200c": "", "\u200d": "", "\ufeff": "",
    "\u2122": "TM", "\u00c6": "AE", "\u00e6": "ae", "\u0152": "OE",
    "\u0153": "oe", "\u00df": "ss", "\u0141": "L", "\u0142": "l",
    "\u00d8": "O", "\u00f8": "o", "\u0110": "D", "\u0111": "d",
}

# Path separators read best as a dash; the rest are just noise on an OLED.
ILLEGAL_TO_DASH = '/\\:'
ILLEGAL_TO_DROP = '<>"|?*'
RESERVED_WIN = {
    "CON", "PRN", "AUX", "NUL",
    *(f"COM{i}" for i in range(1, 10)),
    *(f"LPT{i}" for i in range(1, 10)),
}
MAX_NAME_LEN = 64


def find_ffmpeg() -> str:
    exe = shutil.which("ffmpeg")
    if exe:
        return exe
    try:
        import imageio_ffmpeg

        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        sys.exit("ffmpeg not found. Install it and put it on PATH, or "
                 "run: pip install -r requirements.txt")


def fold_char(ch: str) -> str:
    if ch in TRANSLITERATE:
        return TRANSLITERATE[ch]
    code = ord(ch)
    if code < 32 or code == 127:
        return " "
    if code <= 255:
        return ch
    # Last resort: decompose and keep whatever ASCII base survives.
    decomposed = unicodedata.normalize("NFKD", ch)
    return "".join(
        c for c in decomposed if ord(c) <= 255 and not unicodedata.combining(c)
    )


def clean_text(value: str) -> str:
    if not value:
        return ""
    folded = "".join(fold_char(c) for c in unicodedata.normalize("NFC", value))
    folded = re.sub(r"\s+", " ", folded).strip()
    return folded[:MAX_NAME_LEN].strip()


def clean_filename(stem: str) -> str:
    name = clean_text(stem)
    name = "".join(
        "-" if c in ILLEGAL_TO_DASH else "" if c in ILLEGAL_TO_DROP else c
        for c in name
    )
    name = re.sub(r"-{2,}", "-", name)
    name = re.sub(r"\s+", " ", name).strip(" .-")
    if name.upper() in RESERVED_WIN:
        name = f"_{name}"
    return name or "track"


def unique_path(folder: Path, stem: str, taken: set[str]) -> Path:
    candidate, n = stem, 2
    while candidate.lower() in taken:
        candidate = f"{stem}_{n}"
        n += 1
    taken.add(candidate.lower())
    return folder / f"{candidate}.mp3"


def read_tags(ffmpeg: str, src: Path) -> dict[str, str]:
    """Read tags via ffmpeg's ffmetadata muxer so ffprobe isn't required."""
    proc = subprocess.run(
        [ffmpeg, "-hide_banner", "-v", "quiet", "-i", str(src), "-f", "ffmetadata", "-"],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    tags: dict[str, str] = {}
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line or line[0] in ";#" or "=" not in line:
            continue
        key, raw = line.split("=", 1)
        tags[key.strip().lower()] = re.sub(r"\\(.)", r"\1", raw)
    return tags


def convert(ffmpeg: str, src: Path, dst: Path) -> tuple[bool, str]:
    tags = read_tags(ffmpeg, src)
    title = clean_text(tags.get("title", "")) or dst.stem
    artist = clean_text(tags.get("artist", ""))

    cmd = [
        ffmpeg, "-hide_banner", "-v", "error", "-y",
        "-i", str(src),
        "-map", "0:a:0",        # first audio stream only, so cover art is excluded
        "-map_metadata", "-1",  # drop every inherited tag
        "-vn",
        "-c:a", "libmp3lame", "-b:a", BITRATE,
        "-ac", "2", "-ar", "44100",
        "-id3v2_version", "3", "-write_id3v1", "0",
        "-metadata", f"title={title}",
    ]
    if artist:
        cmd += ["-metadata", f"artist={artist}"]
    cmd.append(str(dst))

    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        lines = (proc.stderr or "").strip().splitlines()
        return False, lines[-1] if lines else "ffmpeg failed"
    return True, ""


def collect(folder: Path) -> list[Path]:
    return sorted(
        p for p in folder.rglob("*")
        if p.is_file() and p.suffix.lower() in AUDIO_EXTS
    )


def main() -> int:
    src_root = Path(MUSIC_FOLDER).expanduser().resolve()
    if not src_root.is_dir():
        sys.exit(f"Not a folder: {src_root}\nEdit MUSIC_FOLDER at the top of this script.")

    out_root = src_root.parent / f"{src_root.name}_cleaned"
    ffmpeg = find_ffmpeg()

    files = collect(src_root)
    if not files:
        print(f"No audio files found in {src_root}")
        return 0

    print(f"{len(files)} file(s) -> {out_root}")
    taken: dict[Path, set[str]] = {}
    failures: list[tuple[Path, str]] = []
    unnameable: list[Path] = []
    skipped = 0

    for src in tqdm(files, unit="file"):
        rel_dir = src.parent.relative_to(src_root)
        dst_dir = out_root.joinpath(*(clean_filename(p) for p in rel_dir.parts))
        names = taken.setdefault(dst_dir, set())
        if not clean_text(src.stem):
            unnameable.append(src)
        dst = unique_path(dst_dir, clean_filename(src.stem), names)

        if dst.exists():
            skipped += 1
            continue

        dst_dir.mkdir(parents=True, exist_ok=True)
        ok, err = convert(ffmpeg, src, dst)
        if not ok:
            failures.append((src, err))
            dst.unlink(missing_ok=True)

    done = len(files) - len(failures) - skipped
    print(f"\nConverted {done}, skipped {skipped}, failed {len(failures)}")
    if unnameable:
        print(f"{len(unnameable)} file(s) had no displayable characters in the "
              f"name and became track/track_2/...:")
        for src in unnameable[:10]:
            print(f"  {src.name}")
    for src, err in failures:
        print(f"  FAILED {src.name}: {err}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
