#!/usr/bin/env python3
"""Serial transport for Chirp sync mode.

The device speaks one JSON object per line. A "put" is answered with a ready
line, after which the host streams exactly `size` raw bytes and waits for the
result. Uploads are verified device-side by CRC32 before being renamed into
place, so a failed transfer never leaves a partial file on the card.

Usable on its own:
    python chirp_link.py dirs
    python chirp_link.py list /Rock
    python chirp_link.py put song.mp3 /Rock/song.mp3
    python chirp_link.py del /Rock/song.mp3
"""

from __future__ import annotations

import json
import sys
import time
import zlib
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("Missing dependency. Run: pip install -r requirements.txt")

ESPRESSIF_VID = 0x303A
CHUNK = 4096
READ_TIMEOUT = 10.0
PROBE_TIMEOUT = 1.5


class ChirpError(RuntimeError):
    pass


class ChirpLink:
    def __init__(self, port: str, timeout: float = READ_TIMEOUT):
        # Arduino-ESP32's USB CDC treats some DTR/RTS patterns as a request to
        # jump to the bootloader, so both must stay low or opening the port
        # reboots the player.
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = timeout
        self.ser.write_timeout = timeout
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.dtr = False
        self.ser.rts = False
        time.sleep(0.05)
        self.ser.reset_input_buffer()

    # -- framing ---------------------------------------------------------
    def _send(self, obj: dict) -> None:
        self.ser.write((json.dumps(obj) + "\n").encode("utf-8"))
        self.ser.flush()

    def _read(self) -> dict:
        """Read lines until one parses as JSON, so stray output can't desync us."""
        deadline = time.monotonic() + self.ser.timeout
        while time.monotonic() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            try:
                obj = json.loads(raw.decode("utf-8", "replace").strip())
            except (ValueError, UnicodeDecodeError):
                continue
            if isinstance(obj, dict):
                return obj
        raise ChirpError("no response from device")

    def _cmd(self, **kw) -> dict:
        self._send(kw)
        reply = self._read()
        if not reply.get("ok"):
            raise ChirpError(reply.get("err", "unknown error"))
        return reply

    # -- commands --------------------------------------------------------
    def hello(self) -> dict:
        return self._cmd(cmd="hello")

    def dirs(self) -> list[str]:
        return self._cmd(cmd="dirs")["dirs"]

    def list(self, directory: str) -> list[dict]:
        return self._cmd(cmd="list", dir=directory)["files"]

    def delete(self, path: str) -> None:
        self._cmd(cmd="del", path=path)

    def put(self, local: Path, remote: str, progress=None) -> int:
        data = local.read_bytes()
        crc = zlib.crc32(data) & 0xFFFFFFFF

        self._send({"cmd": "put", "path": remote, "size": len(data), "crc": crc})
        reply = self._read()
        if not reply.get("ok"):
            raise ChirpError(reply.get("err", "put refused"))

        sent = 0
        for off in range(0, len(data), CHUNK):
            self.ser.write(data[off:off + CHUNK])
            sent += min(CHUNK, len(data) - off)
            if progress:
                progress(sent, len(data))
        self.ser.flush()

        result = self._read()
        if not result.get("ok"):
            raise ChirpError(result.get("err", "transfer failed"))
        return result.get("written", sent)

    def close(self) -> None:
        try:
            self._send({"cmd": "bye"})
        except Exception:
            pass
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def candidate_ports() -> list[str]:
    ports = list(list_ports.comports())
    ports.sort(key=lambda p: p.vid != ESPRESSIF_VID)
    return [p.device for p in ports]


def find_device() -> ChirpLink:
    """Probe every serial port for a Chirp in sync mode."""
    errors = []
    for port in candidate_ports():
        try:
            link = ChirpLink(port, timeout=PROBE_TIMEOUT)
        except Exception as exc:
            errors.append(f"{port}: {exc}")
            continue
        try:
            info = link.hello()
            if info.get("fw") == "chirp":
                link.ser.timeout = READ_TIMEOUT
                return link
            errors.append(f"{port}: not a Chirp")
        except Exception as exc:
            errors.append(f"{port}: {exc}")
        link.ser.close()

    detail = "; ".join(errors) if errors else "no serial ports found"
    raise ChirpError(
        "No Chirp found in sync mode. Go to Screen 3 and hold the centre "
        f"button, then retry. ({detail})"
    )


def _main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 1

    action, args = argv[0], argv[1:]
    with find_device() as link:
        if action == "hello":
            print(json.dumps(link.hello(), indent=2))
        elif action == "dirs":
            for d in link.dirs():
                print(d)
        elif action == "list":
            for f in link.list(args[0] if args else "/"):
                print(f"{f['s']:>10}  {f['n']}")
        elif action == "put":
            local, remote = Path(args[0]), args[1]
            def show(done, total):
                print(f"\r{done * 100 // total:3d}%", end="", flush=True)
            written = link.put(local, remote, show)
            print(f"\rsent {written} bytes")
        elif action == "del":
            link.delete(args[0])
            print("deleted")
        else:
            print(f"Unknown action: {action}")
            return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(_main(sys.argv[1:]))
    except ChirpError as exc:
        sys.exit(str(exc))
