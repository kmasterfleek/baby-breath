"""Server-side steps: serve, check, walktest, calibrate. Talk to the local sensing-server."""
from __future__ import annotations

import json
import statistics
import subprocess
import sys
import time

from . import common as c
from .common import say, checkpoint

CALIBRATION_FRAMES = 12000
CALIBRATION_MAX_S = 15 * 60


# --------------------------------------------------------------- serve ----
def cmd_serve(args) -> int:
    if c.server_up():
        say(f"A server is already answering on {c.HTTP_BASE}. Using it.")
        say(f"Open the UI: {c.UI_URL}")
        return checkpoint("serve", True, "server already running")
    if not c.SERVER_BIN.exists():
        return checkpoint("serve", False, f"server binary not found at {c.SERVER_BIN} (ask the instructor for it)")
    cmd = [str(c.SERVER_BIN), "--bind-addr", "0.0.0.0", "--source", "esp32",
           "--ui-path", str(c.UI_DIR), "--http-port", str(c.HTTP_PORT),
           "--ws-port", str(c.WS_PORT), "--udp-port", str(c.UDP_PORT)]
    if args.extra:
        cmd += args.extra
    say("$ " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=str(c.REPO_ROOT))
    deadline = time.time() + 30
    while time.time() < deadline:
        if proc.poll() is not None:
            return checkpoint("serve", False, f"server exited early with code {proc.returncode} (port in use? binary not executable?)")
        if c.server_up():
            break
        time.sleep(0.5)
    else:
        proc.terminate()
        return checkpoint("serve", False, "server did not answer /health within 30 s")
    say("")
    say(f"Server is up. Open this in your browser:  {c.UI_URL}")
    say(f"Boards send to UDP port {c.UDP_PORT} on this computer.")
    checkpoint("serve", True, "server healthy; leave this window open (Ctrl-C stops it)")
    try:
        proc.wait()
    except KeyboardInterrupt:
        say("\nStopping server ...")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    return 0


# --------------------------------------------------------------- check ----
def print_nodes(nodes: list) -> None:
    say(f"{'NODE':<5} {'STATUS':<7} {'LAST SEEN':<10} {'RSSI dBm':<9} {'MOTION'}")
    for n in nodes:
        say(f"{str(n.get('node_id')):<5} {str(n.get('status')):<7} "
            f"{str(n.get('last_seen_ms')) + ' ms':<10} {str(n.get('rssi_dbm')):<9} {n.get('motion_level')}")


def cmd_check(args) -> int:
    if not c.server_up():
        return checkpoint("check", False, "server not reachable; run `serve` in another window first")
    deadline = time.time() + args.seconds
    last = []
    while time.time() < deadline:
        data = c.http_json("/api/v1/nodes") or {}
        last = data.get("nodes", [])
        active = [n for n in last if n.get("status") == "active"]
        if active:
            print_nodes(last)
            say("Next: place the boards (see RUNBOOK placement), then `walktest`.")
            return checkpoint("check", True, f"{len(active)} of {len(last)} node(s) active")
        remaining = int(deadline - time.time())
        say(f"  waiting for boards ... {len(last)} known, 0 active ({remaining}s left)")
        time.sleep(3)
    if last:
        print_nodes(last)
    say("No node is sending frames. Check, in order:")
    say("  1. `bootlog` shows 'Got IP:' on the same subnet as this computer")
    say("  2. the target IP written by `provision` is THIS computer's WiFi address")
    say("  3. stale node with 0 frames on mesh WiFi -> it roamed; re-read bssid, re-provision with --filter-mac")
    return checkpoint("check", False, f"0 active nodes after {args.seconds} s")


# ------------------------------------------------------------ walktest ----
def sample_motion(seconds: float) -> list[float]:
    """Read motion_band_power off the WebSocket for `seconds`; print one value per second."""
    import asyncio
    import websockets

    async def go() -> list[float]:
        vals: list[float] = []
        end = time.time() + seconds
        next_print = time.time() + 1
        latest = None
        async with websockets.connect(c.WS_URL, max_size=None) as ws:
            while time.time() < end:
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=1)
                except asyncio.TimeoutError:
                    msg = None
                if msg:
                    m = json.loads(msg)
                    if m.get("type") == "sensing_update":
                        latest = (m.get("features") or {}).get("motion_band_power")
                if time.time() >= next_print:
                    next_print += 1
                    if latest is None:
                        say(f"  t={int(seconds - (end - time.time())):>3}s  motion=--- (no data yet)")
                    else:
                        vals.append(float(latest))
                        say(f"  t={int(seconds - (end - time.time())):>3}s  motion={latest:.4f}")
        return vals
    return asyncio.run(go())


def stats(vals: list[float]) -> tuple[float, float]:
    if not vals:
        return 0.0, 0.0
    s = sorted(vals)
    p90 = s[min(len(s) - 1, int(round(0.9 * (len(s) - 1))))]
    return statistics.median(s), p90


def cmd_walktest(args) -> int:
    if not c.server_up():
        return checkpoint("walktest", False, "server not reachable; run `serve` first")
    say(f"Phase 1 of 2: everyone STILL and out of the space between the boards, fan OFF. {args.seconds} s.")
    try:
        still = sample_motion(args.seconds)
    except Exception as e:
        return checkpoint("walktest", False, f"could not read the stream at {c.WS_URL}: {e}")
    if not still:
        return checkpoint("walktest", False, "no sensing updates arrived; run `check` first")
    med_s, p90_s = stats(still)
    say(f"still: median={med_s:.4f} p90={p90_s:.4f} ({len(still)} samples)")
    if args.once:
        return checkpoint("walktest", True, f"baseline median {med_s:.4f}")
    say("")
    say("Phase 2 of 2: now WALK slowly back and forth between the two boards for the whole time.")
    if not args.yes:
        try:
            input("Press Enter when you are ready to start walking ... ")
        except EOFError:
            pass
    walk = sample_motion(args.seconds)
    med_w, p90_w = stats(walk)
    ratio = (med_w / med_s) if med_s > 0 else float("inf") if med_w > 0 else 0.0
    say("")
    say(f"still : median={med_s:.4f}  p90={p90_s:.4f}")
    say(f"walk  : median={med_w:.4f}  p90={p90_w:.4f}")
    say(f"ratio (walk / still median) = {ratio:.2f}x")
    ok = ratio >= args.min_ratio
    if not ok:
        say("The meter barely moved. Fix placement: boards >= 8 ft from the router, your body between")
        say("the router and a board, board 2-3 ft from you at chest height, fan off. Then re-run.")
    return checkpoint("walktest", ok, f"walk/still ratio {ratio:.2f}x (need >= {args.min_ratio}x)")


# ----------------------------------------------------------- calibrate ----
def cmd_calibrate(args) -> int:
    if not c.server_up():
        return checkpoint("calibrate", False, "server not reachable; run `serve` first")
    say("Calibration teaches the server what the EMPTY room looks like. Nobody (and no pets) may be")
    say(f"between the boards for ~10 minutes ({CALIBRATION_FRAMES} frames).")
    if not c.confirm("Is the room empty and will it stay empty?", args.yes):
        return checkpoint("calibrate", False, "not started; run again when the room is empty")
    res = c.http_json("/api/v1/calibration/start", method="POST", body={}) or {}
    if res.get("success") is False:
        say(f"Server said: {res.get('error')}")
        if "already in progress" not in str(res.get("error")):
            return checkpoint("calibrate", False, "server refused to start calibration")
    start = time.time()
    last = 0
    while time.time() - start < CALIBRATION_MAX_S:
        st = c.http_json("/api/v1/calibration/status") or {}
        frames = int(st.get("frame_count") or 0)
        status = st.get("status")
        pct = min(100, 100 * frames // CALIBRATION_FRAMES)
        say(f"  {int(time.time() - start):>4}s  frames={frames:>6}/{CALIBRATION_FRAMES}  {pct:>3}%  status={status}")
        if frames >= CALIBRATION_FRAMES or (status and status != "Collecting" and frames > 0):
            return checkpoint("calibrate", True, f"{frames} frames collected, status {status}")
        if frames == last and time.time() - start > 60:
            say("  (frame count is not increasing; are the boards still active? see `check`)")
        last = frames
        time.sleep(10)
    return checkpoint("calibrate", False, f"only {last} frames after 15 min; boards may be stale (`check`)")
