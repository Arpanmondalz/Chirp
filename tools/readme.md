# Chirp tools

Host-side tools for the Chirp player. Two jobs: preparing music so the device
can play and display it, and getting it onto the SD card without pulling the
card out.

```powershell
pip install -r requirements.txt
```

`ffmpeg` is used for all audio conversion. If it isn't on your PATH, the
`imageio-ffmpeg` package in the requirements ships a binary that gets picked up
automatically.

---

## 1. Bulk preparation — `clean_music.py`

Use this for the initial fill of a library, or any time you want to convert a
whole folder tree at once. It never touches the device.

Edit the two constants at the top of the file:

```python
MUSIC_FOLDER = r"D:\Music"
BITRATE = "192k"
```

Then run it:

```powershell
python clean_music.py
```

It walks `MUSIC_FOLDER` recursively and writes a mirror of the tree to
`D:\Music_cleaned`. Subfolders are preserved, because the player treats each
folder as a playlist.

For every file it:

- re-encodes to 192 kbps stereo MP3 at 44.1 kHz
- takes only the first audio stream, so embedded cover art is excluded outright
- drops every inherited tag, then writes back **only** title and artist
- writes ID3v2.3, which is what the firmware's tag parser handles most reliably
- rewrites the filename and both tags to characters the OLED can actually draw

Files already present in the output are skipped, so re-running it after adding
new music only converts what's new.

### Name cleaning

The player's fonts cover U+0020–U+00FF. Anything outside that range is folded to
an ASCII lookalike, or dropped if there's no sensible equivalent.

| Input | Becomes |
| --- | --- |
| `Beyoncé – Halo (Live)` | `Beyoncé - Halo (Live)` |
| `'Smart' "Quotes"…` | `'Smart' Quotes` |
| `AC/DC: Back in Black?` | `AC-DC- Back in Black` |
| `Łukasz Kaźmierczak` | `Lukasz Kazmierczak` |
| `中文歌曲` | `track` |

Accented Latin-1 survives because the fonts contain those glyphs. CJK has no
ASCII fallback, so those files become `track`, `track_2`, … and the script
prints a list of them at the end so you can rename them yourself.

Also handled: FAT-illegal characters, Windows reserved names (`CON` → `_CON`),
trailing dots and spaces, a 64-character cap, and per-folder collision suffixes.

---

## 2. Adding music without removing the card — sync mode

Day-to-day additions go over the USB cable. The firmware owns the filesystem the
whole time and writes through the normal file API — there is no raw sector
access, so a failed transfer cannot corrupt the card.

### Putting the device in sync mode

1. Long-press **left** or **right** until you reach Screen 3 (the sync icon).
2. **Hold the centre button.** Playback stops and the screen shows
   `Waiting for PC`.

While armed, screen navigation and playback controls are locked. The display
still sleeps after 8 seconds of button inactivity to avoid burn-in, but unlike
normal operation **any** button press wakes it, and that press is swallowed by
the wake. Transfers continue with the screen off — watch progress in the browser
instead.

To leave: wake the screen if it's off, then hold the centre button again. Any
transfer in flight is cancelled cleanly, the card is re-indexed with a progress
bar, and the player returns to Now Playing.

### The file manager — `chirp_web.py`

```powershell
python chirp_web.py
```

Then open <http://127.0.0.1:5000>. It binds to localhost only, deliberately —
it exposes unauthenticated upload and delete endpoints and has no business
being reachable from your network.

**Left pane — staging.** Drop files of any format. Each one is converted in the
background using exactly the same rules as `clean_music.py`, so what you see
listed is the final filename as it will appear on the card. Status goes
`queued → converting → ready`.

**Right pane — the card.** Browse any folder on the device and delete files from
it.

When everything reads `ready`, choose a destination folder and click **Sync**.

- Folders come from the device; you can't type a path and you can't create new
  folders. Make folders by putting the card in a reader.
- A file whose name already exists in the destination is skipped. The check runs
  against the *cleaned* name, and the firmware enforces it again on its side.
- Each upload goes to a temp file, is verified by CRC32 and length, and is only
  then renamed into place.

Transfers run around 300–600 KB/s — the ESP32-S3's USB is full-speed only, so
budget 10–20 seconds per track. Fine for topping up; use `clean_music.py` and a
card reader for the first bulk load.

### Command line — `chirp_link.py`

The transport is usable directly, which is handy for debugging:

```powershell
python chirp_link.py hello
python chirp_link.py dirs
python chirp_link.py list /Rock
python chirp_link.py put "song.mp3" /Rock/song.mp3
python chirp_link.py del /Rock/song.mp3
```

It finds the device by probing serial ports, preferring Espressif's USB vendor
ID, and confirming with a handshake.

---

## Troubleshooting

**"No Chirp found in sync mode"** — the device isn't armed. Screen 3, hold
centre. Also close the Arduino Serial Monitor: only one program can hold the
port.

**The player reboots when the tool connects** — shouldn't happen, since the
client explicitly clears DTR and RTS on open. Arduino-ESP32's USB CDC reads
certain DTR/RTS patterns as a request to enter the bootloader. If you write your
own client, clear both.

**Garbled responses** — something is printing to `Serial` while sync mode is
running. The data channel and the debug console are the same port, so debug
output has to stay silent while armed.

**A file uploaded but won't play** — check it converted rather than being copied
raw. Only `ready` items are sent, and everything sent has been through ffmpeg.

## Files

| File | Purpose |
| --- | --- |
| `clean_music.py` | Bulk folder conversion and name cleaning |
| `chirp_link.py` | Serial protocol client, also a CLI |
| `chirp_web.py` | Flask file manager |
| `templates/index.html` | The file manager page |
| `_staging/` | Scratch space for converted uploads; safe to delete |
