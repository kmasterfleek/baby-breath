"""Board-side steps: detect, flash, provision, bootlog. Needs pyserial + esptool in the venv."""
from __future__ import annotations

import difflib
import re
import sys
import time

from . import common as c
from .common import say, checkpoint, StepFailed

FLASH_LAYOUT = [
    ("0x0", "bootloader.bin"),
    ("0x8000", "partition-table.bin"),
    ("0xf000", "ota_data_initial.bin"),
    ("0x20000", "esp32-csi-node.bin"),
]
FLASH_FLAGS = ["--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "8MB"]
PORT_HINTS = ("usbmodem", "usbserial", "wchusbserial", "ttyUSB", "ttyACM", "COM")
KNOWN_VIDS = {0x303A: "Espressif USB", 0x1A86: "CH340 clone", 0x10C4: "CP210x"}


# -------------------------------------------------------------- detect ----
def candidate_ports() -> list:
    from serial.tools import list_ports
    out = []
    for p in list_ports.comports():
        name = p.device
        if p.vid in KNOWN_VIDS or any(h in name for h in PORT_HINTS):
            out.append(p)
    return out


def esptool_cmd(*args: str) -> list[str]:
    return [c.python_exe(), "-m", "esptool", *args]


def flash_id(port: str) -> dict:
    """Run `esptool flash-id` and pull out chip / PSRAM / flash size."""
    rc, out = c.capture(esptool_cmd("--port", port, "flash-id"), timeout=40)
    info = {"ok": rc == 0, "chip": "?", "psram": "?", "flash": "?", "raw": out}
    m = re.search(r"Chip type:\s*(.+)", out)
    if m:
        info["chip"] = m.group(1).strip()
    m = re.search(r"PSRAM\s*(\d+\s*MB)", out, flags=re.I)
    if m:
        info["psram"] = m.group(1).replace(" ", "")
    m = re.search(r"Detected flash size:\s*(\S+)", out)
    if m:
        info["flash"] = m.group(1)
    return info


def is_s3(info: dict) -> bool:
    return "ESP32-S3" in info["chip"].upper().replace(" ", "")


def cmd_detect(args) -> int:
    ports = candidate_ports()
    if not ports:
        say("No serial ports that look like an ESP32 were found.")
        say("Plug each board into the computer with a DATA USB cable (not a charge-only one).")
        say("macOS boards show up as /dev/cu.usbmodem*, clones as /dev/cu.wchusbserial*; Windows: COMx.")
        return checkpoint("detect", False, "0 boards found")
    say(f"{len(ports)} candidate port(s):")
    rows = []
    for p in ports:
        vid = KNOWN_VIDS.get(p.vid, f"vid={p.vid:#06x}" if p.vid else "unknown vid")
        say(f"  probing {p.device} ({vid}) ...")
        info = flash_id(p.device)
        rows.append((p.device, info))
    say("")
    say(f"{'PORT':<34} {'CHIP':<30} {'PSRAM':<7} {'FLASH':<6}")
    good = 0
    for dev, info in rows:
        chip = info["chip"] if info["ok"] else "no response (busy or not an ESP32?)"
        say(f"{dev:<34} {chip[:30]:<30} {info['psram']:<7} {info['flash']:<6}")
        if info["ok"] and is_s3(info):
            good += 1
    if good == 0:
        for dev, info in rows:
            if not info["ok"]:
                say(f"--- esptool output for {dev} ---")
                say(info["raw"].strip()[-800:])
    return checkpoint("detect", good >= 1, f"{good} ESP32-S3 board(s) answered on {len(ports)} port(s)")


# --------------------------------------------------------------- flash ----
def cmd_flash(args) -> int:
    missing = [n for _, n in FLASH_LAYOUT if not (c.RELEASE_BINS / n).exists()]
    if missing:
        return checkpoint("flash", False, f"missing firmware files in {c.RELEASE_BINS}: {missing}")
    say(f"Checking the chip on {args.port} ...")
    info = flash_id(args.port)
    if not info["ok"]:
        say(info["raw"].strip()[-800:])
        return checkpoint("flash", False, "board did not answer esptool; unplug/replug and run `detect`")
    say(f"chip={info['chip']} psram={info['psram']} flash={info['flash']}")
    if not is_s3(info):
        return checkpoint("flash", False, f"refusing: chip is {info['chip']}, this firmware is for ESP32-S3 only")
    if not c.confirm(f"Erase and flash {args.port} now? This replaces whatever is on the board.", args.yes):
        return checkpoint("flash", False, "cancelled by user")
    p = c.run(esptool_cmd("--chip", "esp32s3", "--port", args.port, "erase-flash"))
    if p.returncode != 0:
        return checkpoint("flash", False, "erase-flash failed (see output above)")
    write = ["--chip", "esp32s3", "--port", args.port, "--baud", str(args.baud), "write-flash", *FLASH_FLAGS]
    for off, name in FLASH_LAYOUT:
        write += [off, str(c.RELEASE_BINS / name)]
    p = c.run(esptool_cmd(*write))
    if p.returncode != 0:
        return checkpoint("flash", False, "write-flash failed (see output above)")
    say("Next: `provision --port ... --node-id ...`, then `bootlog`.")
    return checkpoint("flash", True, f"4 images written to {args.port}")


# ----------------------------------------------------------- provision ----
def _pick_ssid(ssid: str, assume_yes: bool) -> str:
    saved = c.saved_wifi_networks()
    if not saved:
        say("Could not read this computer's saved WiFi list; double-check the SSID spelling yourself.")
        return ssid
    if ssid in saved:
        say(f"SSID '{ssid}' matches a network saved on this computer. Good.")
        return ssid
    close = difflib.get_close_matches(ssid, saved, n=3, cutoff=0.5)
    say(f"WARNING: '{ssid}' is NOT in this computer's saved WiFi networks.")
    say("The board never reports a wrong SSID; it just retries and goes quiet.")
    if close:
        say("Closest saved names: " + ", ".join(f"'{n}'" for n in close))
    if not c.confirm(f"Use '{ssid}' exactly as typed anyway?", assume_yes):
        raise StepFailed("SSID not confirmed; re-run with the exact network name (copy it from the saved list)")
    return ssid


def _pick_target_ip(given: str | None, ssid: str, assume_yes: bool) -> str:
    cands = c.list_ipv4_ifaces()
    if given:
        say(f"Boards will send data to: {given} (from --target-ip)")
        return given
    if not cands:
        raise StepFailed("no LAN address found on this computer; join the WiFi and re-run, or pass --target-ip")
    say("This computer's network addresses (VPN and link-local hidden):")
    for i, e in enumerate(cands, 1):
        say(f"  {i}. {e['ip']:<15} on {e['iface']}  (mask {e['mask']})")
    wifi = c.wifi_iface()
    joined = c.current_wifi_ssid()
    pick = None
    if wifi and joined and joined == ssid:
        for e in cands:
            if e["iface"] == wifi:
                pick = e
                say(f"Your Wi-Fi ({wifi}) is joined to '{ssid}' right now, so its address is the one to use.")
    if pick is None:
        pick = next((e for e in cands if c.is_private(e["ip"])), cands[0])
        say(f"Could not tell which address is on '{ssid}'. Pick the one on the same network as your")
        say(f"Wi-Fi router. If this computer is on Ethernet to that same router, that is fine too.")
    if assume_yes:
        say(f"Using {pick['ip']} [auto-yes]")
        return pick["ip"]
    try:
        ans = input(f"Send board data to which address? [1-{len(cands)}, Enter = {pick['ip']}] ").strip()
    except EOFError:
        ans = ""
    if ans.isdigit() and 1 <= int(ans) <= len(cands):
        pick = cands[int(ans) - 1]
    elif ans and not ans.isdigit():
        raise StepFailed("target IP not chosen; re-run and pick a number, or pass --target-ip")
    say(f"Boards will send data to: {pick['ip']} ({pick['iface']})")
    return pick["ip"]


def cmd_provision(args) -> int:
    if not c.PROVISION_PY.exists():
        return checkpoint("provision", False, f"missing {c.PROVISION_PY}")
    try:
        ssid = _pick_ssid(args.ssid, args.yes)
        target = _pick_target_ip(args.target_ip, ssid, args.yes)
    except StepFailed as e:
        return checkpoint("provision", False, str(e))
    slot = args.node_id - 1
    cmd = [c.python_exe(), str(c.PROVISION_PY), "--port", args.port, "--ssid", ssid,
           "--password", args.password, "--target-ip", target, "--node-id", str(args.node_id),
           "--tdm-slot", str(slot), "--tdm-total", str(args.tdm_total)]
    if args.filter_mac:
        cmd += ["--filter-mac", args.filter_mac]
    say(f"Writing node {args.node_id} (slot {slot} of {args.tdm_total})" +
        (f" locked to access point {args.filter_mac}" if args.filter_mac else "") + " ...")
    shown = [("***" if i > 0 and cmd[i - 1] == "--password" else x) for i, x in enumerate(cmd)]
    say("$ " + " ".join(shown))
    import subprocess
    p = subprocess.run(cmd)
    if p.returncode != 0:
        return checkpoint("provision", False, "provision.py failed (see output above)")
    c.save_state(port=args.port, node_id=args.node_id, ssid=ssid, target_ip=target,
                 filter_mac=args.filter_mac, time=time.strftime("%Y-%m-%d %H:%M:%S"))
    say(f"Next: `bootlog --port {args.port}` and look for 'Got IP:'.")
    return checkpoint("provision", True, f"node {args.node_id} written on {args.port}, target {target}")


# ------------------------------------------------------------- bootlog ----
def capture_bootlog(port: str, seconds: float) -> str:
    import serial
    s = serial.Serial(port, 115200, timeout=0.2)
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    time.sleep(0.05)
    s.reset_input_buffer()
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        buf += s.read(4096)
    s.close()
    return buf.decode("utf-8", "replace")


def summarize_bootlog(log: str) -> dict:
    d = {}
    for key, pat in [
        ("version", r"App version:\s*(\S+)"),
        ("ssid", r"NVS override: ssid=(.*)"),
        ("target_ip", r"NVS override: target_ip=(\S+)"),
        ("node_id", r"NVS override: node_id=(\d+)"),
        ("connected", r"connected with (.*)"),
        ("bssid", r"bssid\s*=\s*([0-9a-fA-F:]{17})"),
        ("got_ip", r"Got IP:\s*(\d+\.\d+\.\d+\.\d+)"),
        ("filter", r"MAC filter active:\s*([0-9a-fA-F:]{17})"),
    ]:
        m = re.search(pat, log)
        d[key] = m.group(1).strip() if m else None
    retries = re.findall(r"Retrying WiFi connection \((\d+)/(\d+)\)", log)
    d["retries"] = retries[-1] if retries else None
    csi = re.findall(r"CSI cb #(\d+)", log)
    d["csi_count"] = int(csi[-1]) if csi else 0
    return d


def cmd_bootlog(args) -> int:
    say(f"Resetting {args.port} and reading for {args.seconds:.0f} s ...")
    try:
        log = capture_bootlog(args.port, args.seconds)
    except Exception as e:  # serial errors
        return checkpoint("bootlog", False, f"could not open {args.port}: {e}")
    if not args.quiet:
        say("---- boot log ----")
        say(log.rstrip())
        say("---- end ----")
    d = summarize_bootlog(log)
    say("")
    say(f"firmware version : {d['version'] or '?'}")
    say(f"node id          : {d['node_id'] or '?'}")
    say(f"wifi name (SSID) : {d['ssid'] or '? (no NVS override -> board not provisioned?)'}")
    say(f"target computer  : {d['target_ip'] or '?'}")
    say(f"connected        : {d['connected'] or 'NO'}")
    say(f"access point     : {d['bssid'] or '?'}   (use with --filter-mac on mesh WiFi)")
    say(f"board IP         : {d['got_ip'] or 'NONE'}")
    say(f"MAC filter       : {d['filter'] or 'off'}")
    say(f"CSI frames seen  : {d['csi_count']}")
    if not d["got_ip"]:
        say("")
        say("The board never got an IP. Most likely causes, in order:")
        say("  1. SSID typo (compare letter by letter with the saved-networks list)")
        say("  2. wrong password")
        say("  3. the network is 5 GHz only; the board needs 2.4 GHz")
        if d["retries"]:
            say(f"  (firmware retried {d['retries'][0]}/{d['retries'][1]} times)")
        say("Fix: re-run `provision` with every flag, then `bootlog` again.")
        return checkpoint("bootlog", False, "no 'Got IP' within the capture window")
    target = c.load_state().get("target_ip") or d["target_ip"]
    got = d["got_ip"]
    if target:
        cands = c.list_ipv4_ifaces()
        mine = next((e for e in cands if e["ip"] == target), None)
        mask = mine["mask"] if mine else "255.255.255.0"
        if not c.same_network(got, target, mask):
            match = next((e for e in cands if c.same_network(got, e["ip"], e["mask"])), None)
            fix = f"--target-ip {match['ip']}" if match else "--target-ip <this computer's address on that network>"
            say("")
            say(f"The board is on {got.rsplit('.', 1)[0]}.x but the server IP {target} is on "
                f"{target.rsplit('.', 1)[0]}.x. They cannot talk.")
            if match:
                say(f"This computer has {match['ip']} on {match['iface']}, which IS on the board's network.")
            say(f"Fix: re-run `provision` with every flag plus {fix}")
            return checkpoint("bootlog", False,
                              f"board is on {got.rsplit('.', 1)[0]}.x but the server IP is on "
                              f"{target.rsplit('.', 1)[0]}.x - re-run provision with {fix}")
    else:
        say("Note: no record of the provisioned target IP; could not check the subnet.")
    if d["csi_count"] == 0:
        say("Note: no CSI frames yet in this window; that is normal right after boot. `check` will confirm.")
    return checkpoint("bootlog", True, f"board joined WiFi as {got}" + (f", same network as server {target}" if target else ""))
