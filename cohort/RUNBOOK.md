# Baby Breath class runbook

Goal for one class session: two ESP32-S3 boards go from the box to a motion meter
in your browser that moves when you walk between them. Claude Code drives each
step, runs the checkpoint, and you read the PASS/FAIL line together before moving on.

**This is not a medical device.** It is a curiosity and a learning tool. Any
breathing or heart number it shows is a signal-processing guess about radio
waves in a room, never information about anyone's health. Do not use it to
monitor a baby, a patient, or anyone else, and never act on its numbers.

## What you need

- A Mac (Windows and Linux work but got less testing), Python 3.10 or newer
- Two ESP32-S3 boards and two USB **data** cables (charge-only cables look identical and do nothing)
- A 2.4 GHz WiFi network whose name and password you know
- This folder (`cohort/`) inside the Baby Breath repository, with the prebuilt
  server at `babybreath-app/bin/sensing-server` (the instructor supplies it)

Every command below is run from this folder:

```
cd cohort
python3 babybreath.py <step> ...
```

Every step ends with `CHECKPOINT <step>: PASS - ...` or `FAIL - ...`. Do not
go to the next step on a FAIL; follow the "If it fails" branch instead.

## Session plan

| # | Step | Command | Checkpoint |
|---|------|---------|------------|
| 0 | setup | `python3 babybreath.py setup` | tools installed in `./.venv` |
| 1 | detect | `python3 babybreath.py detect` | at least one ESP32-S3 answered |
| 2 | flash | `python3 babybreath.py flash --port <port>` | 4 images written (skip if pre-flashed) |
| 3 | provision | `python3 babybreath.py provision --port <port> --node-id 1 --ssid "<name>" --password "<pw>"` | node written, target IP shown |
| 4 | bootlog | `python3 babybreath.py bootlog --port <port>` | board joined WiFi, "Got IP" |
| 5 | serve | `python3 babybreath.py serve` | server healthy, UI URL printed |
| 6 | check | `python3 babybreath.py check` | >= 1 node active |
| 7 | placement | (no command; see diagram) | boards placed, fan off |
| 8 | walktest | `python3 babybreath.py walktest` | walk/still ratio >= 1.5x |
| 9 | calibrate | `python3 babybreath.py calibrate` | 12000 empty-room frames |
| 10 | breathing (optional) | open the UI, sit still | a number appears; it is a curiosity only |

Steps 2, 3 and 4 are done **once per board** (node-id 1, then node-id 2).

### 0. setup

`python3 babybreath.py setup` creates a private Python environment in
`cohort/.venv` and installs `esptool`, `pyserial`, `websockets` and the NVS
tool. Nothing else on the computer is touched. Later steps find `.venv` on
their own; you never need to "activate" it.

If it fails: no internet or a proxy. Fix the connection and re-run.

### 1. detect

Plug in both boards. `detect` lists serial ports that look like a board and
asks each one for its chip, PSRAM and flash size.

- macOS: boards appear as `/dev/cu.usbmodem*` (genuine Espressif USB) or
  `/dev/cu.wchusbserial*` / `/dev/cu.usbserial*` (CH340 clones)
- Windows: `COM3`, `COM7`, ...
- Linux: `/dev/ttyACM*` or `/dev/ttyUSB*`

Write down which port is which board. Unplug one to tell them apart.

If it fails: swap the cable for a known data cable, try another USB port, and
make sure nothing else (a serial monitor) has the port open.

### 2. flash (skip if the instructor pre-flashed the boards)

`flash --port <port>` erases the board and writes the four firmware images from
`firmware/esp32-csi-node/release_bins`. It refuses any chip that is not an
ESP32-S3. It asks before erasing.

If it fails: run `detect` again; if the board answers there, retry `flash`.
If `write-flash` stalls, hold the BOOT button while plugging the board in.

### 3. provision

```
python3 babybreath.py provision --port <port> --node-id 1 --ssid "<wifi name>" --password "<wifi password>"
python3 babybreath.py provision --port <other port> --node-id 2 --ssid "<wifi name>" --password "<wifi password>"
```

This writes the WiFi name, password, node number and this computer's address
into the board, then resets it. It wipes the old settings each time, so pass
every flag every time.

Two things the tool checks, and you must confirm out loud:

- **SSID read-back.** On macOS the tool reads the computer's saved WiFi list and
  warns if your spelling is not in it, suggesting the closest saved name. A
  wrong SSID is silent: the board retries ten times and gives up with no
  explanation. Compare letter by letter. The network must be **2.4 GHz**.
- **Target IP.** The tool lists this computer's LAN addresses with their
  interface names (VPN and link-local addresses hidden). The boards send their
  data to the one you choose, so it must be this computer's address **on the
  same network as the WiFi router the boards join**. If the Mac's Wi-Fi is
  joined to that very network, the tool proposes it; otherwise it asks you to
  pick, defaulting to the first home-LAN address. A Mac plugged into the router
  by Ethernet may be on that network through Ethernet while its own Wi-Fi is on
  a different network; the Ethernet address is then the right one. The choice
  is saved in `cohort/.last_provision.json` so `bootlog` can verify it.

If it fails: re-run with the exact SSID from the saved list, or with
`--target-ip <ip>` for the right address.

### 4. bootlog

`bootlog --port <port>` resets the board, reads its startup messages for 25 s,
and summarizes: firmware version, SSID it will use, target computer, whether it
connected, the access point it joined (bssid), the IP it got, MAC filter, and
how many CSI frames it has seen.

PASS means a line `Got IP:` appeared and that IP is on the same subnet as the
target computer.

If it fails:

- No `Got IP` at all: SSID typo (most common), wrong password, or 5 GHz-only
  network. Re-run `provision` with every flag, then `bootlog` again.
- `Got IP` but FAIL "board is on 192.168.4.x but the server IP is on
  192.168.10.x": the board joined a different network than the address you
  chose. The tool names this computer's address on the board's network if it
  has one; re-run `provision` with every flag plus `--target-ip <that address>`.
- **Mesh WiFi** (several access points, e.g. eero, Orbi, Deco) makes the signal
  about four times noisier because the board may hop between points. After the
  first successful join, copy the `bssid = xx:xx:xx:xx:xx:xx` value from the
  summary and re-run `provision` with every flag plus `--filter-mac <that bssid>`.
  Then `bootlog` again and confirm `MAC filter active`.

### 5. serve

`serve` starts the sensing server from the repository root and waits until it
answers. It prints the UI address: `http://localhost:8080/ui/baby.html`. Leave
that terminal window open; open a second window for the next steps.

If it fails: "exited early" usually means port 8080, 8765 or 5005 is already in
use (an old server still running; quit it) or the binary is not executable
(`chmod +x babybreath-app/bin/sensing-server` on macOS/Linux).

### 6. check

`check` asks the server every few seconds, for up to 60 s, which boards have
sent frames, and prints a table: node, status (`active` or `stale`), last
seen, signal strength, motion level. PASS when at least one node is active.

If it fails:

1. Both nodes missing: the target IP is wrong (step 3) or the computer's
   firewall blocks incoming UDP on port 5005. On macOS allow `sensing-server`
   when prompted.
2. One node `stale` with zero frames on mesh WiFi: it roamed to another access
   point. Re-run `bootlog`, read the new bssid, re-provision with `--filter-mac`.
3. The boards are too close to the router (see placement).

### 7. placement

```
        ROUTER (access point)
           |
           |   >= 8 ft
           |
   [BOARD 1]        (you)        [BOARD 2]
       ^          2-3 ft            ^
       |<-- body between router and board -->|
                 chest height
              fan OFF, door closed
```

- Boards at least 8 ft from the router. A board 1 ft from the router is blind.
- Your body between the router and a board, 2 to 3 ft from the board, board at
  chest height (a shelf or a tape strip on the wall).
- Fan, air conditioner and moving curtains off. Boards must stay on WiFi and
  powered (a phone charger is fine).

### 8. walktest

`walktest` reads the live stream for 30 s while everyone stays still, prints
the motion value once a second with its median and 90th percentile, then asks
you to walk slowly between the boards for another 30 s and prints the ratio.
PASS when walking is at least 1.5 times the still median. Use `--seconds 20`
for a shorter test, `--once` for only the still baseline.

If it fails: the meter did not move. Re-check placement (step 7), especially
distance from the router and the fan, then repeat.

### 9. calibrate

`calibrate` starts the empty-room calibration and reports progress every 10 s.
It needs 12000 frames with **nobody in the room**, about 10 minutes at 20 Hz,
and gives up after 15 minutes. Start it, leave, come back.

If it fails: the frame count stopped growing. Run `check`; a stale node means
it dropped off WiFi (mesh roaming again, or power).

### 10. breathing (optional)

Open the UI, sit still between the boards, and watch. A breathing or heart
number may appear after a minute. It is a demonstration of what radio
reflections can carry, and nothing more. It is not measuring anyone's health.
