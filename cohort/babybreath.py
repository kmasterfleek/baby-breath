#!/usr/bin/env python3
"""Baby Breath cohort CLI — walk two ESP32-S3 boards from the box to a moving motion meter.

Run `python3 babybreath.py --help`. Every subcommand ends with a line
`CHECKPOINT <step>: PASS|FAIL - <reason>` and exits 0 on PASS, 1 on FAIL.
Not a medical device: the numbers on screen are a curiosity, never health information.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from bbkit import common as c  # noqa: E402
from bbkit.common import say, checkpoint  # noqa: E402

NEEDS_VENV = {"detect", "flash", "provision", "bootlog", "walktest"}
VENV_DIR = HERE / ".venv"


def venv_python(explicit: str | None) -> Path | None:
    if explicit:
        return Path(explicit)
    for cand in (VENV_DIR / "bin" / "python", VENV_DIR / "Scripts" / "python.exe"):
        if cand.exists():
            return cand
    return None


def deps_present() -> bool:
    try:
        import serial  # noqa: F401
        import websockets  # noqa: F401
        import esptool  # noqa: F401
    except ImportError:
        return False
    return True


def maybe_reexec(args, argv: list[str]) -> None:
    """If this interpreter lacks the deps, re-run under the venv's python."""
    if args.cmd not in NEEDS_VENV or deps_present():
        return
    py = venv_python(args.python)
    if py is None or not py.exists():
        say("This step needs the helper tools (pyserial, esptool, websockets) and none are installed.")
        say("Run `python3 babybreath.py setup` once, then try again.")
        sys.exit(checkpoint(args.cmd, False, "no ./.venv yet; run `setup` first"))
    if Path(sys.prefix).absolute() == py.absolute().parent.parent:  # already inside that venv
        sys.exit(checkpoint(args.cmd, False, f"{py} is missing pyserial/esptool/websockets; run `setup` again"))
    os.execv(str(py), [str(py), str(HERE / "babybreath.py"), *argv]) if os.name != "nt" else \
        sys.exit(subprocess.call([str(py), str(HERE / "babybreath.py"), *argv]))


# --------------------------------------------------------------- setup ----
def cmd_setup(args) -> int:
    base = sys.executable
    if VENV_DIR.exists() and venv_python(None):
        say(f"{VENV_DIR} already exists; installing/upgrading the tools inside it.")
    else:
        say(f"Creating a private Python environment at {VENV_DIR} ...")
        p = c.run([base, "-m", "venv", str(VENV_DIR)])
        if p.returncode != 0 or not venv_python(None):
            say("Plain venv failed (this happens on exFAT drives); retrying with --copies ...")
            p = c.run([base, "-m", "venv", "--copies", str(VENV_DIR)])
            if p.returncode != 0:
                return checkpoint("setup", False, "could not create ./.venv")
    py = venv_python(None)
    p = c.run([str(py), "-m", "pip", "install", "--upgrade", "pip", *c.PIP_DEPS])
    if p.returncode != 0:
        return checkpoint("setup", False, "pip install failed (internet? proxy?)")
    say("")
    say("Done. You do not have to activate anything: babybreath.py finds ./.venv on its own.")
    say("If you want the tools in your own shell:")
    say("  macOS/Linux:  source .venv/bin/activate")
    say("  Windows:      .venv\\Scripts\\activate")
    return checkpoint("setup", True, f"tools installed in {VENV_DIR}")


# --------------------------------------------------------------- parser ----
def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="babybreath.py",
        description="Baby Breath class kit: from two boards in a box to a moving motion meter.",
        epilog="Order: setup, detect, flash (skip if pre-flashed), provision, bootlog, serve, check, walktest, calibrate.")
    ap.add_argument("--python", help="python inside a venv that has esptool/pyserial/websockets (default ./.venv)")
    ap.add_argument("-y", "--yes", action="store_true", help="answer yes to confirmations (for scripted runs)")
    sp = ap.add_subparsers(dest="cmd", required=True)

    sp.add_parser("setup", help="create ./.venv and install esptool, pyserial, websockets").set_defaults(fn=cmd_setup)
    sp.add_parser("detect", help="list boards and ask each for chip / PSRAM / flash size").set_defaults(fn="detect")

    f = sp.add_parser("flash", help="erase a board and write the prebuilt firmware (ESP32-S3 only)")
    f.add_argument("--port", required=True)
    f.add_argument("--baud", type=int, default=460800)
    f.set_defaults(fn="flash")

    pv = sp.add_parser("provision", help="write WiFi name/password, node id and this computer's IP to a board")
    pv.add_argument("--port", required=True)
    pv.add_argument("--node-id", type=int, required=True, help="1 for the first board, 2 for the second")
    pv.add_argument("--ssid", required=True, help="2.4 GHz WiFi name, spelled exactly")
    pv.add_argument("--password", required=True)
    pv.add_argument("--target-ip", help="this computer's WiFi IP (default: detected, you confirm)")
    pv.add_argument("--filter-mac", help="access point bssid from `bootlog` (mesh WiFi only)")
    pv.add_argument("--tdm-total", type=int, default=2, help="number of boards (default 2)")
    pv.set_defaults(fn="provision")

    b = sp.add_parser("bootlog", help="reset a board, read its boot log, summarize WiFi join")
    b.add_argument("--port", required=True)
    b.add_argument("--seconds", type=float, default=25)
    b.add_argument("--quiet", action="store_true", help="summary only, hide the raw log")
    b.set_defaults(fn="bootlog")

    s = sp.add_parser("serve", help="start the sensing server and wait until it is healthy")
    s.add_argument("--extra", nargs=argparse.REMAINDER, help="extra flags passed to the server")
    s.set_defaults(fn="serve")

    ck = sp.add_parser("check", help="wait until at least one board is sending frames")
    ck.add_argument("--seconds", type=int, default=60)
    ck.set_defaults(fn="check")

    w = sp.add_parser("walktest", help="measure motion still vs walking; prints the ratio")
    w.add_argument("--seconds", type=int, default=30, help="length of each phase")
    w.add_argument("--min-ratio", type=float, default=1.5, help="walk/still ratio needed to PASS")
    w.add_argument("--once", action="store_true", help="only the still phase (baseline numbers)")
    w.set_defaults(fn="walktest")

    cal = sp.add_parser("calibrate", help="empty-room calibration (~10 min, 12000 frames)")
    cal.set_defaults(fn="calibrate")
    return ap


def dispatch(args) -> int:
    if callable(args.fn):
        return args.fn(args)
    if args.cmd in ("detect", "flash", "provision", "bootlog"):
        from bbkit import boards
        return getattr(boards, "cmd_" + args.cmd)(args)
    from bbkit import server
    return getattr(server, "cmd_" + args.cmd)(args)


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    args = build_parser().parse_args(argv)
    if sys.version_info < (3, 10):
        return checkpoint(args.cmd, False, f"Python 3.10+ needed, this is {sys.version.split()[0]}")
    maybe_reexec(args, argv)
    try:
        return dispatch(args)
    except KeyboardInterrupt:
        return checkpoint(args.cmd, False, "interrupted")


if __name__ == "__main__":
    sys.exit(main())
