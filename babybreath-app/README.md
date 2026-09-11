# Baby Breath — "My Baby" WiFi Vital-Signs Monitor

Contactless baby monitoring from WiFi radio signals. No camera, no wearable, no contact —
two cheap ESP32 boards on the crib rails sense the baby's **breathing rate** and **heart rate**
by measuring tiny disturbances in the WiFi radio field (Channel State Information / CSI), and a
desktop app shows live vitals with apnea alerting.

> **Status:** Working prototype. Validated end-to-end on real hardware (2-node ESP32-S3 mesh
> streaming live breathing + heart rate). Not a certified medical device — research/hobby use only.

---

## 1. What this is

`babybreath-app/` is an **Electron desktop app** ("My Baby") that is the consumer-facing product.
It is a thin shell around a much larger WiFi-sensing platform (`wifi-densepose`) that lives in the
parent repository. The app:

1. Spawns a prebuilt **Rust sensing server** (`sensing-server`) as a child process.
2. The server listens for **UDP CSI/vitals packets** from ESP32 sensor nodes on the local WiFi.
3. It runs the signal-processing + vitals pipeline and streams results over **WebSocket** to the UI.
4. The UI (`ui/baby.html`) renders breathing/heart-rate cards, waveforms, a sleep timer, an alert
   log, and a full-screen **apnea banner**.

A first-run **setup wizard** (`ui/setup.html`) provisions new ESP32 boards with your WiFi
credentials over a USB cable.

```
┌────────────┐   WiFi CSI    ┌──────────────────┐   UDP :5005   ┌─────────────────────┐   WS :8765   ┌──────────────┐
│ ESP32-S3   │──────────────▶│  (radio field    │──────────────▶│  sensing-server     │─────────────▶│  My Baby UI  │
│ node 1     │  perturbation │   between nodes) │   vitals/CSI  │  (Rust + Axum)      │   sensing_   │  baby.html   │
│ ESP32-S3   │──────────────▶│                  │   packets     │  DSP + fusion       │   update     │  (Electron)  │
│ node 2     │               └──────────────────┘               └─────────────────────┘              └──────────────┘
└────────────┘                                                     HTTP REST API :8080
```

---

## 2. The four core technologies

The vitals come out of the parent `wifi-densepose` platform. Four subsystems matter most:

| Tech | Where it lives | Role in the baby monitor |
|------|----------------|--------------------------|
| **DensePose** | `sensing-server/src/vital_signs.rs`, `pose.rs` | Extracts vitals from CSI: breathing via **Goertzel** in the 0.1–0.5 Hz band, heart rate, 17-keypoint skeleton, presence/motion classification. Produces the breaths/min + BPM numbers. |
| **RuVector** | vendored `vendor/ruvector` (v2.1.0-40), `rvf_pipeline.rs` | Signal cleanup: subcarrier importance weighting, temporal keypoint smoothing (EMA), coherence gating, compressed history. Small specialized models (RVF container + LoRA/SONA profiles) rather than one giant model. |
| **MinCut** | `ruvector-mincut::DynamicMinCut` (direct dep of the server) | Person separation — runs min-cut on the subcarrier temporal-correlation graph to detect independent motion clusters. Keeps the monitor locked on the *baby* vs. miscounting people. |
| **RuView** | `wifi-densepose-ruvector/src/viewpoint/`, `field_model.rs` | Cross-viewpoint / multistatic fusion — fuses multiple ESP32 nodes (RSSI-weighted) into one estimate, and the SVD "field model" that separates *environmental drift* from *body perturbation* (the "teach the room" baseline). |

**RuView calibration philosophy** (from the RuView author): *teach the room before you teach the
model.* Capture a quiet-room baseline (environmental fingerprint) → capture guided anchors
(lying still, slow/normal breathing, small movement) → compress into small specialized models.
The backend supports this via `/api/v1/calibration/*` and `/api/v1/recording/*`; a guided
calibration UI in the consumer app is **not yet built** (see Roadmap).

---

## 3. Hardware

| Device | Chip | Role | Notes |
|--------|------|------|-------|
| **ESP32-S3-DevKitC-1-N16R8** ×2 | Xtensa dual-core, 16MB flash / 8MB PSRAM | WiFi CSI sensing nodes | Validated. Place on opposite crib rails. |
| ESP32-S3 SuperMini (4MB) | Xtensa dual-core | Compact CSI node | 4MB firmware variant is **not yet rebuilt with the node_id fix** — see Known Issues. |

- Nodes are powered by USB; **data flows over WiFi (UDP), not USB.** USB is only power + flashing/console.
- Requires a **2.4 GHz** SSID — the ESP32-S3 has no 5 GHz radio.
- Each DevKitC has two USB-C ports (**UART** and **USB**); both enumerate on macOS as `/dev/cu.usbmodem*`
  (native USB-Serial-JTAG) and both carry the console.

---

## 4. Quick start

### Prerequisites
- **Node.js** (for Electron) and **npm**
- The prebuilt Rust **`sensing-server`** binary at
  `../rust-port/wifi-densepose-rs/target/release/sensing-server`
  (build it with: `cd ../rust-port/wifi-densepose-rs && cargo build --release -p wifi-densepose-sensing-server`)
- For flashing/provisioning boards: **ESP-IDF v5.4** + esptool (Python). On the reference Mac these
  live at `~/esp/esp-idf` and `~/.espressif/python_env/idf5.4_py3.14_env/`.

### Run the app
```bash
cd babybreath-app
npm install
npm start          # launches Electron; auto-spawns the sensing-server
```

- **No boards yet, no data?** The app opens the **setup wizard**: enter your 2.4 GHz WiFi, plug both
  boards in via USB, and it provisions them as node 1 / node 2, then waits until they actually come
  online over WiFi before declaring success (surfacing wrong-password / 5 GHz-only failures).
- **Boards already provisioned?** They reconnect automatically and the monitor loads.

### Build a distributable (macOS, unsigned)
```bash
cd babybreath-app
mkdir -p bin && cp ../rust-port/wifi-densepose-rs/target/release/sensing-server bin/
npx electron-builder -c.directories.output="$HOME/babybreath-dist"   # DMG + .app
```
- The server binary, `ui/`, and `provision.py` are bundled as `extraResources` (real files on
  disk — the Rust server child process can't read inside `app.asar`).
- **Output must go to a local APFS disk.** Building into `dist/` on an exFAT volume (like the
  LaCie drive this repo lives on) corrupts the asar during integrity checks — hence the
  `-c.directories.output` override.
- The build is **unsigned** (`mac.identity: null`): recipients must right-click → Open the first
  time. Signing/notarization needs an Apple Developer ID (pre-beta TODO).

### Run with no hardware (simulation)
The server supports a synthetic source. Run it standalone:
```bash
../rust-port/wifi-densepose-rs/target/release/sensing-server \
  --bind-addr 127.0.0.1 --source simulate \
  --ui-path ./ui --http-port 8080 --ws-port 8765 --udp-port 5005
# then open http://localhost:8080/ui/baby.html
```

---

## 5. Flashing & provisioning ESP32 nodes

Firmware source: `../firmware/esp32-csi-node/`. Prebuilt binaries: `.../release_bins/` (currently **v0.6.1**).

**Flash an 8MB board** (preserves the NVS config partition at `0x9000`, so re-flashing does NOT wipe provisioning):
```bash
ESP=~/.espressif/python_env/idf5.4_py3.14_env/bin
B=../firmware/esp32-csi-node/release_bins
"$ESP/python3" "$ESP/esptool.py" --chip esp32s3 -p <PORT> -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 80m --flash_size keep \
  0x0 $B/bootloader.bin 0x8000 $B/partition-table.bin \
  0xf000 $B/ota_data_initial.bin 0x20000 $B/esp32-csi-node.bin
```

**Provision** (write WiFi creds + node identity to NVS). Easiest via the app's USB setup wizard; or CLI:
```bash
"$ESP/python3" ../firmware/esp32-csi-node/provision.py --port <PORT> \
  --ssid "YourWiFi" --password "secret" \
  --target-ip <THIS_MACHINE_LAN_IP> --target-port 5005 \
  --node-id 1 --tdm-slot 0 --tdm-total 2      # node 2: --node-id 2 --tdm-slot 1
```

> **No ESP-IDF needed to flash or provision.** A plain venv with `pip install esptool esp-idf-nvs-partition-gen pyserial` does both
> (esptool 5.x commands are `erase-flash` / `write-flash` / `flash-id`). The `../cohort/` starter kit wraps every step with
> PASS/FAIL checkpoints: `python3 ../cohort/babybreath.py detect | flash | provision | bootlog | serve | check | walktest | calibrate`.
>
> **Mesh WiFi (several access points): add `--filter-mac <bssid>`.** Without it the node captures frames from every transmitter
> and the signal is ~4× noisier. Read the BSSID from the boot log line `connected with <ssid> ... bssid = xx:xx:xx:xx:xx:xx`
> after the first join, then re-provision with that value. A node that later shows `stale` with zero frames has probably roamed
> to another access point — re-read the BSSID and re-provision.
>
> **Placement matters more than anything:** boards ≥ 8 ft from the router, the person's body between the router and a board,
> board 2–3 ft from the chest, fan off. A board 1 ft from the router is effectively blind.
>
> The server keeps nodes streaming by itself (`--node-poke-hz`, default 20): ESP32 CSI only exists when the access point sends
> the node a frame, so the server sends each active node a 1-byte UDP datagram 20× per second.
>
> **Rebuild firmware** (macOS): `source ~/esp/esp-idf/export.sh && cd ../firmware/esp32-csi-node && idf.py build`.
> Note: an incremental build does **not** re-read `version.txt` — run `idf.py reconfigure` first if you bump the version.

---

## 6. Server API (for integrators)

HTTP REST on `:8080`, WebSocket stream on `ws://localhost:8765/ws/sensing`. Key endpoints:

| Endpoint | Purpose |
|----------|---------|
| `GET /health` | Liveness (`{"status":"ok",...}`) |
| `GET /api/v1/nodes` | Connected sensor nodes (id, status, rssi, person_count, motion) |
| `GET /api/v1/vital-signs` | Current fused breathing/heart rate + confidences + signal quality |
| `GET /api/v1/sensing/latest` | Latest classification + feature vector + per-node amplitudes |
| `POST /api/v1/calibration/start` · `/stop` · `GET /status` | Room-baseline (field-model) calibration |
| `POST /api/v1/recording/start` · `/stop` · `GET /list` | Record labeled CSI clips (training anchors) |
| `GET /api/v1/pose/current` | Current 17-keypoint skeleton |

**Wire protocol** (ESP32 → server, UDP, little-endian, magic-prefixed):

| Magic | Packet |
|-------|--------|
| `0xC5110001` | Raw CSI frame |
| `0xC5110002` | Vitals (breathing/heart rate) |
| `0xC5110003` | Feature vector (8 normalized dims @ 1 Hz) |
| `0xC5110004` | WASM output event |
| `0xC5110005` | Compressed frame |
| `0xC5110006` | Fused vitals |

---

## 7. Repository layout (relevant parts)

```
babybreath-app/           ← THIS app (Electron "My Baby")
  main.js                 ← spawns sensing-server, window + IPC, USB provisioning
  preload.js              ← contextBridge API (window.babybreath.*)
  ui/baby.html            ← live monitor (vitals, waveforms, apnea banner, "nerd mode")
  ui/setup.html           ← first-run provisioning wizard (use the USB tab)
../rust-port/wifi-densepose-rs/crates/
  wifi-densepose-sensing-server/   ← the Rust server (vital_signs.rs, pose.rs, rvf_pipeline.rs …)
  wifi-densepose-ruvector/         ← RuView cross-viewpoint fusion (viewpoint/)
  wifi-densepose-signal/           ← RuvSense modules incl. field_model (room baseline)
../firmware/esp32-csi-node/        ← ESP32-S3 C firmware (CSI capture, NVS config, provision.py, release_bins/)
../vendor/ruvector/                ← vendored RuVector v2.1.0-40
```

---

## 8. Known issues & roadmap

**Known issues**
- **4MB SuperMini firmware** (`esp32-csi-node-4mb.bin`) still carries an older `node_id` bug and has
  not been rebuilt/validated; the current ~1.3 MB app may not fit its smaller partition without tuning.
- **Unsigned build:** the packaged app is not code-signed or notarized (no Apple Developer ID yet).
  Gatekeeper will warn on first launch — right-click → Open. Must be fixed before public beta.
- **The packaged app's wizard still prefers the ESP-IDF Python env** for `provision.py`; the `../cohort/` kit is the
  ESP-IDF-free path (pip `esptool` + `esp-idf-nvs-partition-gen`) and is what the cohort class uses.
- **DHCP caveat:** nodes have the server's IP baked into NVS. If the host machine's LAN IP changes,
  UDP won't arrive ("no data") — re-run the setup wizard (auto-detects current IP) or reserve a static IP.
  The host may also have several LAN IPs (Ethernet vs Wi-Fi on different subnets): the target must be on the
  network the boards join; `cohort/babybreath.py bootlog` checks the board's "Got IP" against the provisioned target.
- **Presence of a still person is weak; motion is strong.** The server's `classification.presence` and person count
  are not shown in the UI any more (they read "present, ~22 persons" in an empty room). The UI's motion meter
  compares against a per-room empty baseline ("Teach the room"); thresholds were tuned on walking adults.
- **Breathing on these boards is not yet validated** with a paced test; the raw signal is there once placement is right.
- Confidence/signal-quality start low until a **room baseline** is captured and buffers fill (~30–60 s).

**Roadmap**
- Paced-breathing validation (empty 60 s → seated, one breath per 10 s × 90 s) and a breathing estimator that
  actually tracks the rhythm visible in the raw subcarriers.
- Hosyond 2.8" ILI9341 on-device display: retarget `display_hal.c` (currently Waveshare ST7789V2/CST816) to the
  ILI9341 pinout, identify the touch IC, and show the presence grid / motion meter on the node itself.
- Firmware auto-`filter_mac` on the joined BSSID (upstream RuView declined single-transmitter filtering), and
  `SO_BROADCAST` on the UDP sender so a broadcast target can replace the baked IP.
- Guided **calibration wizard** in the consumer app (quiet baseline → guided breathing anchors),
  wiring the existing `/calibration` + `/recording` endpoints into a parent-friendly "Set up this room" flow.
- Train small specialized RuVector models per vital (breathing / heartbeat / restlessness / posture).
- Rebuild + validate the 4MB firmware variant.
- **mDNS/`.local` host discovery** in firmware to replace the NVS-baked target IP (kills the DHCP caveat).
- Sign + notarize the installer (needs an Apple Developer ID).

---

## 9. Safety

This is an experimental research prototype, **not a certified medical device**. Do not rely on it as
the sole means of monitoring an infant. WiFi-CSI vital-sign estimation is sensitive to placement,
motion, and RF environment.
