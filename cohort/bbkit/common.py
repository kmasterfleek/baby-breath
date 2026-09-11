"""Shared helpers: paths, checkpoints, HTTP, network info, prompts."""
from __future__ import annotations

import json
import os
import platform
import re
import socket
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

COHORT_DIR = Path(__file__).resolve().parent.parent
REPO_ROOT = COHORT_DIR.parent
RELEASE_BINS = REPO_ROOT / "firmware" / "esp32-csi-node" / "release_bins"
PROVISION_PY = REPO_ROOT / "firmware" / "esp32-csi-node" / "provision.py"
APP_DIR = REPO_ROOT / "babybreath-app"
UI_DIR = APP_DIR / "ui"
SERVER_BIN = APP_DIR / "bin" / ("sensing-server.exe" if os.name == "nt" else "sensing-server")

HTTP_PORT = 8080
WS_PORT = 8765
UDP_PORT = 5005
HTTP_BASE = f"http://localhost:{HTTP_PORT}"
WS_URL = f"ws://localhost:{WS_PORT}/ws/sensing"
UI_URL = f"{HTTP_BASE}/ui/baby.html"

PIP_DEPS = ["esptool", "esp-idf-nvs-partition-gen", "pyserial", "websockets"]

IS_MAC = platform.system() == "Darwin"
IS_WIN = os.name == "nt"


class StepFailed(Exception):
    """Raised by a subcommand to end with a FAIL checkpoint."""


def say(msg: str = "") -> None:
    print(msg, flush=True)


def checkpoint(step: str, ok: bool, detail: str) -> int:
    """Print the final PASS/FAIL line for a step and return the exit code."""
    verdict = "PASS" if ok else "FAIL"
    say("")
    say(f"CHECKPOINT {step}: {verdict} - {detail}")
    return 0 if ok else 1


def confirm(question: str, assume_yes: bool = False) -> bool:
    if assume_yes:
        say(f"{question} [auto-yes]")
        return True
    try:
        answer = input(f"{question} [y/N] ").strip().lower()
    except EOFError:
        return False
    return answer in ("y", "yes")


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    """Run a command, echoing it first."""
    say("$ " + " ".join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd], **kw)


def capture(cmd: list[str], timeout: float = 60) -> tuple[int, str]:
    """Run a command quietly; return (returncode, combined output)."""
    try:
        p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout)
    except FileNotFoundError as e:
        return 127, str(e)
    except subprocess.TimeoutExpired:
        return 124, "timed out"
    return p.returncode, (p.stdout or "") + (p.stderr or "")


# ---------------------------------------------------------------- HTTP ----
def http_json(path: str, method: str = "GET", body: dict | None = None, timeout: float = 5):
    """GET/POST a JSON endpoint on the local server. Returns parsed JSON or None."""
    data = None
    headers = {}
    if method == "POST":
        data = json.dumps(body if body is not None else {}).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(HTTP_BASE + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8", "replace")
    except (urllib.error.URLError, OSError, ConnectionError):
        return None
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return {"raw": raw}


def server_up() -> bool:
    return http_json("/health") is not None


# ------------------------------------------------------------- network ----
STATE_FILE = COHORT_DIR / ".last_provision.json"
VPN_IFACE_PREFIXES = ("utun", "tun", "tap", "ppp", "wg", "tailscale", "ipsec", "gpd")


def is_private(ip: str) -> bool:
    """RFC1918 home-LAN ranges only."""
    a, b = (int(x) for x in ip.split(".")[:2])
    return a == 10 or a == 192 and b == 168 or a == 172 and 16 <= b <= 31


def is_vpn_or_special(ip: str) -> bool:
    a, b = (int(x) for x in ip.split(".")[:2])
    return a == 127 or a == 169 and b == 254 or a == 100 and 64 <= b <= 127


def _mask_from_hex(h: str) -> str:
    v = int(h, 16)
    return ".".join(str((v >> s) & 255) for s in (24, 16, 8, 0))


def list_ipv4_ifaces() -> list[dict]:
    """[{iface, ip, mask}] for every usable LAN address: no loopback, link-local, VPN/CGNAT, tun."""
    out: list[dict] = []
    if IS_WIN:
        rc, txt = capture(["ipconfig"])
        if rc != 0:
            return out
        for block in re.split(r"\r?\n(?=\S)", txt):
            head = block.splitlines()[0].strip().rstrip(":") if block.strip() else ""
            ip = re.search(r"IPv4 Address[ .]*:\s*(\d+\.\d+\.\d+\.\d+)", block)
            mask = re.search(r"Subnet Mask[ .]*:\s*(\d+\.\d+\.\d+\.\d+)", block)
            if ip:
                out.append({"iface": head, "ip": ip.group(1), "mask": mask.group(1) if mask else "255.255.255.0"})
    else:
        rc, txt = capture(["ifconfig"])
        if rc != 0:
            return out
        iface = ""
        for ln in txt.splitlines():
            m = re.match(r"^([a-zA-Z0-9.\-]+):", ln)
            if m:
                iface = m.group(1)
                continue
            m = re.search(r"inet (\d+\.\d+\.\d+\.\d+).*?netmask (0x[0-9a-fA-F]+|\d+\.\d+\.\d+\.\d+)", ln)
            if m:
                mask = m.group(2)
                out.append({"iface": iface, "ip": m.group(1),
                            "mask": _mask_from_hex(mask) if mask.startswith("0x") else mask})
    keep = []
    for e in out:
        low = e["iface"].lower()
        if low.startswith(VPN_IFACE_PREFIXES) or "vpn" in low or is_vpn_or_special(e["ip"]):
            continue
        keep.append(e)
    return keep


def all_ipv4() -> list[str]:
    return [e["ip"] for e in list_ipv4_ifaces()]


def same_network(a: str, b: str, mask: str = "255.255.255.0") -> bool:
    ia = [int(x) for x in a.split(".")]
    ib = [int(x) for x in b.split(".")]
    im = [int(x) for x in mask.split(".")]
    return all((x & m) == (y & m) for x, y, m in zip(ia, ib, im))


def same_subnet(a: str, b: str) -> bool:
    return same_network(a, b)


def wifi_iface() -> str | None:
    """Name of the Wi-Fi interface (macOS only; None elsewhere)."""
    if not IS_MAC:
        return None
    rc, out = capture(["networksetup", "-listallhardwareports"])
    m = re.search(r"Hardware Port: Wi-Fi\s*\nDevice: (\w+)", out) if rc == 0 else None
    return m.group(1) if m else None


def current_wifi_ssid() -> str | None:
    """SSID the Wi-Fi interface is joined to, if the OS lets us read it (macOS often redacts it)."""
    iface = wifi_iface()
    if not iface:
        return None
    rc, out = capture(["networksetup", "-getairportnetwork", iface])
    m = re.search(r"Current Wi-Fi Network:\s*(.+)", out) if rc == 0 else None
    if m and "<redacted>" not in m.group(1):
        return m.group(1).strip()
    rc, out = capture(["ipconfig", "getsummary", iface])
    m = re.search(r"^\s*SSID\s*:\s*(.+)$", out, flags=re.M) if rc == 0 else None
    if m and "<redacted>" not in m.group(1):
        return m.group(1).strip()
    return None


def save_state(**fields) -> None:
    try:
        STATE_FILE.write_text(json.dumps(fields, indent=2))
    except OSError:
        pass


def load_state() -> dict:
    try:
        return json.loads(STATE_FILE.read_text())
    except (OSError, json.JSONDecodeError):
        return {}


def saved_wifi_networks() -> list[str]:
    """Names of WiFi networks this computer has saved (macOS / Windows). Empty if unknown."""
    names: list[str] = []
    if IS_MAC:
        for iface in ("en0", "en1", "en2"):
            rc, out = capture(["networksetup", "-listpreferredwirelessnetworks", iface])
            if rc == 0 and "Preferred networks" in out:
                names += [ln.strip() for ln in out.splitlines()[1:] if ln.strip()]
    elif IS_WIN:
        rc, out = capture(["netsh", "wlan", "show", "profiles"])
        if rc == 0:
            names += re.findall(r"Profile\s*:\s*(.+?)\s*$", out, flags=re.M)
    seen: list[str] = []
    for n in names:
        if n not in seen:
            seen.append(n)
    return seen


def python_exe() -> str:
    return sys.executable
