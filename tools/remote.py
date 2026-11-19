#!/usr/bin/env python3
"""Send a script to sloperinaGZ's command file (src/ootgz/remote.h) and print what it did.

    python3 tools/remote.py "fire savepos" "wait 30" "fire loadpos"
    python3 tools/remote.py < setup.txt
    python3 tools/remote.py --sd ~/.local/share/lichii/sdmc status

Needs settings > remote control on (remote_control=1 in sloperinaGZ/settings.ini). The script goes to
<SD card>/sloperinaGZ/remote/cmd.txt; the module deletes that file when it takes it and writes a line per script
line to remote/out.txt, which this prints until the script's "end". Exit status: 0 if every line was ok, 1 if one
wasn't, 2 if the script wasn't taken or didn't end in time.

The SD card is --sd, or $SLOPERINA_SD, or the first of the emulators' folders that has sloperinaGZ in it.
"""
import argparse
import os
import secrets
import sys
import time
from pathlib import Path

HOME = Path.home()
APPDATA = Path(os.environ.get("APPDATA", HOME / "AppData/Roaming"))
SD_CARDS = [base / name / "sdmc" for name in ("lichii", "fritchii", "eden")
            for base in (HOME / ".local/share", HOME / "Library/Application Support", APPDATA)]


def find_sd(given):
    if given:
        return Path(given).expanduser()
    if os.environ.get("SLOPERINA_SD"):
        return Path(os.environ["SLOPERINA_SD"]).expanduser()
    for sd in SD_CARDS:
        if (sd / "sloperinaGZ").is_dir():
            return sd
    sys.exit("no SD card folder with sloperinaGZ in it: give one with --sd")


def send(sd, lines, timeout=30.0, wait_free=10.0, show=print):
    """Run the lines; returns (results, ended): the out.txt lines of this script, and whether it reached its end."""
    folder = sd / "sloperinaGZ" / "remote"
    folder.mkdir(parents=True, exist_ok=True)
    cmd, out = folder / "cmd.txt", folder / "out.txt"
    deadline = time.monotonic() + wait_free
    while cmd.exists():  # the script before this one hasn't been taken yet
        if time.monotonic() > deadline:
            sys.stderr.write(f"{cmd} is still there: is the game running, with settings > remote control on?\n")
            return [], False
        time.sleep(0.02)
    tag = secrets.token_hex(3)
    start = out.stat().st_size if out.exists() else 0
    tmp = folder / "cmd.tmp"
    tmp.write_text(f"id {tag}\n" + "\n".join(lines) + "\n")
    tmp.replace(cmd)  # in one step: the module never reads half a script

    results, ended, pending = [], False, b""
    deadline = time.monotonic() + timeout
    while not ended and time.monotonic() < deadline:
        size = out.stat().st_size if out.exists() else 0
        if size < start:  # out.txt was cleared meanwhile
            start = 0
        if size > start:
            with open(out, "rb") as f:
                f.seek(start)
                pending += f.read(size - start)
            start = size
            *whole, pending = pending.split(b"\n")
            for raw in whole:
                frame, _, rest = raw.decode(errors="replace").partition(" ")
                ident, _, text = rest.partition(" ")
                if ident != tag:
                    continue
                if text == "end":
                    ended = True
                    break
                results.append((int(frame) if frame.isdigit() else -1, text))
                show(f"{frame} {text}")
        else:
            time.sleep(0.01)
    if not ended:
        where = "wasn't taken" if cmd.exists() else "didn't end"
        sys.stderr.write(f"the script {where} within {timeout:g} s\n")
    return results, ended


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sd", help="the SD card's folder (the one sloperinaGZ/ is in)")
    parser.add_argument("--timeout", type=float, default=30.0, help="seconds to wait for the script's end")
    parser.add_argument("lines", nargs="*", help="the script's lines (none: read from standard input)")
    args = parser.parse_args()
    lines = args.lines or [line.rstrip("\n") for line in sys.stdin]
    results, ended = send(find_sd(args.sd), lines, args.timeout)
    if not ended:
        sys.exit(2)
    # "<line number> ok|err ..."; a list's rows are "<line number> | ..."
    sys.exit(1 if any(text.split(" ", 2)[1:2] == ["err"] for _, text in results) else 0)


if __name__ == "__main__":
    main()
