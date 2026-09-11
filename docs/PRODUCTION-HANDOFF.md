# Baby Breath / "My Baby" — Production & Beta Handoff

> **Purpose:** hand this project to a fresh context (new Claude session or a collaborator) with everything
> needed to go from *working prototype* → *beta with friends* → *crowdfunding/pre-sale*. Written 2026-07-05.
> Read this first, then `babybreath-app/README.md`.

---

## 0. TL;DR — where we are

- **It works.** Two ESP32-S3 boards on a bed sense breathing + heart rate + motion contactlessly over WiFi.
  First real overnight-style capture (a 6-year-old, 2h20m) clearly showed **sleep onset** — breathing slowed,
  motion collapsed, signal quality jumped. Data is real and legible.
- **It is a prototype, not a product.** It runs in **dev mode on the developer's Mac**. There is no installer,
  no packaged app, no appliance, and it depends on a computer staying awake on the same WiFi all night.
- **It is explicitly NOT a medical device** and never will be — positioning is *parent curiosity / sleep insight*.
  This framing is legally load-bearing (see §7).

The gap from here to "give it to 5 friends" is **productization + reliability**, not core signal work.

---

## 1. What the product is

**"My Baby"** (`babybreath-app/`, package name `my-baby`) is an **Electron desktop app** that:
1. Spawns a prebuilt **Rust `sensing-server`** (Axum) as a child process.
2. The server ingests **UDP CSI/vitals packets** from ESP32-S3 sensor nodes over the local WiFi.
3. Runs the vitals + fusion pipeline and streams results over **WebSocket** to the UI (`ui/baby.html`).
4. A first-run **setup wizard** (`ui/setup.html`) provisions boards with WiFi creds over USB.

It sits on top of a large fork of **RuView / wifi-densepose** (the "engine"). The four core techs
(DensePose = vitals/pose, RuVector = signal cleanup, MinCut = person separation, RuView = multi-node fusion)
all live in the engine and are already wired up. See `babybreath-app/README.md` for the file-level map.

**Ports:** HTTP/UI `8080`, WebSocket `8765`, UDP ingest `5005`.
**Repos (private, GitHub `kmasterfleek`):** `baby-breath` (full monorepo — canonical) and `babybreath-app`
(older app-only, now redundant).

---

## 2. Hardware (validated)

| Device | Chip | Flash/PSRAM | Role | ~Cost |
|--------|------|-------------|------|-------|
| **ESP32-S3-DevKitC-1-N16R8** ×2 | Xtensa dual-core | 16 MB / 8 MB octal | CSI sensing nodes (validated) | ~$9 |
| ESP32-S3 SuperMini | Xtensa dual-core | 4 MB | compact node (firmware NOT yet fixed — see §5) | ~$6 |

- **Two nodes**, one on each side of the bed (body *between* them). 6-year-old on a normal bed worked well.
- Nodes need only **5 V USB power** (wall charger — *not* a power bank; low draw makes many banks auto-shut-off).
- **2.4 GHz WiFi only** (ESP32-S3 has no 5 GHz radio).
- Boards enumerate on macOS as `/dev/cu.usbmodem*` (native USB-Serial-JTAG); either USB-C port powers them.
- Data flows over **WiFi**, not USB — USB is only for power + flashing/provisioning.

---

## 3. Firmware — current state & the one bug you must know

- Source: `firmware/esp32-csi-node/`. Prebuilt binaries: `release_bins/`. **Current version: 0.6.1.**
- **Fixed this cycle (v0.6.1): the `node_id` NVS bug.** The old prebuilt binary *ignored* the provisioned
  `node_id`, so **every board booted as node 1** and collided into a single node on the server. Rebuilt from
  source (which reads `node_id` in `main/nvs_config.c`) — verified two boards now report as node 1 and node 2.
- **Build (macOS):** `source ~/esp/esp-idf/export.sh && cd firmware/esp32-csi-node && idf.py build`.
  ⚠️ An incremental build does **not** re-read `version.txt` — run `idf.py reconfigure` after a version bump.
- **Flash (preserves NVS/provisioning):** write `0x0 / 0x8000 / 0xf000 / 0x20000` with `--flash_size keep`;
  do **not** write `0x9000` (that's the NVS config partition). Full command in `babybreath-app/README.md` §5.
- **⚠️ The 4 MB SuperMini variant still carries the old bug** — not rebuilt; the current ~1.3 MB app may not fit
  the 4 MB app partition without config tuning, and it has no 4 MB hardware to validate. Do NOT ship 4 MB until fixed.

---

## 4. Provisioning — how a board gets set up (and where it bites)

`firmware/esp32-csi-node/provision.py` writes an NVS partition over USB with: WiFi SSID/password, **target IP**
(the host running the server), target port (5005), `node_id`, `tdm_slot`, `tdm_total`, and `edge_tier`.

Two ways:
- **App setup wizard** (`ui/setup.html`): enter WiFi → **use the "USB Cable" tab** → Set Up Sensors.
  ⚠️ The **"Wireless (Bluetooth)" tab does NOT work** with this firmware — it looks for a `MYBABY_` BLE device
  that doesn't exist. Non-technical users will get stuck here; this must be fixed or the BLE tab removed before beta.
- **CLI:** `provision.py --port <P> --ssid ... --password ... --target-ip <HOST_IP> --target-port 5005
  --node-id 1 --tdm-slot 0 --tdm-total 2` (node 2: `--node-id 2 --tdm-slot 1`).

**Gotchas we hit live (all real, all will hit beta testers):**
- **Wrong WiFi baked in** → boards silently retry forever (`errno 118` on send). Fix = re-provision to the
  right SSID. The app must surface "sensor can't reach WiFi" instead of failing silently.
- **DHCP / multi-interface IP:** the target IP is baked into NVS. If the host's IP changes (DHCP lease, VPN,
  Ethernet+WiFi both up), packets go nowhere. The app's IP auto-detect can pick the wrong interface. Needs
  a reserved/static host IP or mDNS/`.local` discovery instead of a hardcoded IP.

---

## 5. Data & modes (edge tiers)

- Packets are `sensing_update` JSON, one per line, to `data/recordings/*.jsonl` when recording.
- `provision.py --edge-tier {0,1,2}`: **0 = raw CSI**, **1 = stats**, **2 = vitals** (default).
- **We ran tier 2** (compact, on-board DSP → vitals). Reliable and streams well.
- **Calibration ("teach the room") needs raw CSI (tier 0).** At tier 2 the field-model calibration accumulated
  **0 / 12000 frames** — it never gets the raw frames it needs. To unlock room-baseline calibration AND richer
  **sleep-position/pose** data, re-provision with `--edge-tier 0`. Cost: much larger files, less overnight-tested.
- **Data volume is large regardless:** the recorded `sensing_update` stream ran **~1.3 MB/s (~35 fps, 2 nodes)**
  → ~**4–5 GB/hour**, ~40–55 GB per night. Storage + privacy is a real product concern (§7).

---

## 6. Reliability lessons from the first live night (must-fix for beta)

- **The server died mid-recording.** It was running as a child of the Electron app under a terminal session;
  when that was killed, the server + recording stopped (~2h in). **Fix:** run the `sensing-server` binary
  **headless + detached** (`nohup … & disown`), independent of any GUI/terminal. We recovered this way; the
  recording resumed into a second file. **A beta build must run the server as a resilient background service,
  not tied to the app window.**
- **The Mac must stay awake** all night (`caffeinate -dimsu`) or it sleeps and the capture stops.
- **Host-computer dependency is the #1 adoption barrier:** today every user needs a computer running 24/7 on
  their WiFi. See §8 for the appliance path.

---

## 7. Product / legal / safety realities (read before crowdfunding)

- **NOT a medical device. Never imply monitoring, alarms, apnea/SIDS detection, or safety reliance.** For a
  baby-adjacent product this is the single biggest liability. Market it as a **curiosity / sleep-insight
  instrument**. Add a visible disclaimer **in the app UI** (currently there is none) and on all campaign material.
- **Accuracy is unvalidated.** Numbers are directional (trust trends, not absolutes; HR is noisiest). Do not
  publish accuracy claims without validation against a reference.
- **Selling RF hardware:** use pre-certified ESP32-S3 modules; understand FCC (US) / CE (EU) obligations for a
  *sold* product vs. a hobby board. Get advice before taking money for hardware.
- **Privacy:** CSI recordings are sensitive home data (tens of GB/night). Decide storage/retention/consent up
  front — especially for beta testers' children. Keep captures local by default; never auto-upload.
- **Per-room calibration:** the signal is room-specific. A baseline captured in one room does not transfer.

---

## 8. The path — phased plan

### Phase A — Source ESP32 chips
- Standardize on **ESP32-S3 (8 MB+ flash)**. DevKitC-1-N16R8 is validated; a smaller pre-certified module +
  simple carrier is the productization target. Buy **2 boards per tester + spares** (we saw ~1 in 3 boards
  need a re-flash/re-provision dance). Add **wall chargers + short USB-C cables** to each kit.
- Decide enclosure + mounting (bed-rail clip) and cable-safety story (cords away from the child).

### Phase B — Provisioning + packaging for the Electron app  ← **biggest eng lift**
1. **Package the app.** `package.json` expects to bundle `bin/sensing-server`, but **`bin/` doesn't exist** —
   the app only runs in dev mode. Build the server, copy it into `babybreath-app/bin/`, wire `electron-builder`,
   and produce a **signed/notarized installer** (macOS notarization; Windows code-signing).
2. **Fix provisioning UX** (§4): remove/replace the broken BLE tab, surface WiFi-join failures, and replace the
   hardcoded target IP with **mDNS/`.local` discovery** (or a reserved-IP setup step) so it survives DHCP.
3. **Run the server as a resilient background service** (§6), not tied to the app window; auto-restart; keep host awake.
4. Bake the **v0.6.1 node_id fix** into whatever testers flash; ship a dead-simple flash/provision flow.

### Phase C — Beta with a few friends
- Give each: 2 provisioned boards + chargers + the packaged app + a **one-page setup card** + the **safety
  disclaimer**. Decide the **host**: their own always-on computer, or a small **appliance** (the repo already
  has a Raspberry Pi "Cognitum Seed" edge-appliance concept — evaluate turning that into the shippable host so
  testers don't tie up a laptop).
- Collect: setup friction, overnight reliability, and (with consent) sample recordings. Ship the sleep-timeline
  visual (the artifact) back to them — it's a strong retention/wow moment.

### Phase D — Crowdfunding / pre-sale
- Assets you already have: a **working demo**, real data, and a polished **sleep-onset visual** (great campaign
  hero). Needs: honest positioning (§7), a BOM + margin model, a realistic fulfillment/delivery timeline, and a
  clear "requires a 2.4 GHz WiFi + an always-on host (or our appliance)" disclosure so backers aren't surprised.
- Pick platform (Kickstarter/Indiegogo pre-sale vs. simple pre-order page). Keep claims curiosity-framed.

### Phase E — Real launch
- Contingent on: validated setup UX, appliance or rock-solid host story, manufacturing/enclosure, support plan,
  and regulatory sign-off for selling RF hardware.

---

## 9. Prioritized engineering backlog (to reach Phase C)

1. ~~**Package the app** (bin/ + electron-builder + signing)~~ — **DONE 2026-07-05** (unsigned DMG builds + verified; signing/notarization still needs an Apple Developer ID). See `babybreath-app/README.md` §"Build a distributable".
2. ~~**Resilient headless server service** + keep-awake~~ — **DONE 2026-07-05** (auto-restart with backoff, powerSaveBlocker, window-close no longer stops monitoring on macOS). Also fixed: server startup read whole recordings into RAM to count frames — now estimated, startup is seconds not minutes.
3. **Provisioning UX** — **PARTLY DONE 2026-07-05**: broken BLE tab removed, post-provision online-check with clear failure messages added. Remaining: mDNS/`.local` discovery instead of hardcoded IP (firmware change, needs hardware validation).
4. ~~**Safety disclaimer in the UI** (setup + monitor)~~ — **DONE 2026-07-05.**
5. **Appliance evaluation** (Pi "Cognitum Seed") so testers don't need a laptop running all night.
6. **Tier-0 raw-CSI path** for calibration + sleep-position data (optional for first beta, needed for the "study poses" pitch).
7. **Rebuild + validate the 4 MB firmware variant** (only if SuperMini is in the kit).

---

## 10. Key files, commands, pointers

- Product app: `babybreath-app/` (`main.js`, `ui/baby.html`, `ui/setup.html`, `README.md`).
- Server crate: `rust-port/wifi-densepose-rs/crates/wifi-densepose-sensing-server/` (`vital_signs.rs`,
  `pose.rs`, `rvf_pipeline.rs`, `field_bridge.rs` = calibration).
- Firmware: `firmware/esp32-csi-node/` (`provision.py`, `main/nvs_config.c`, `release_bins/`, `version.txt`).
- Run headless server (overnight-safe):
  `nohup rust-port/wifi-densepose-rs/target/release/sensing-server --bind-addr 0.0.0.0 --source esp32
  --ui-path babybreath-app/ui --http-port 8080 --ws-port 8765 --udp-port 5005 & disown`
- Key API: `GET /api/v1/nodes`, `/vital-signs`, `/sensing/latest`; `POST /api/v1/recording/{start,stop}`,
  `/calibration/{start,stop}`.
- First-night finding: 6-year-old, 2h20m, sleep onset ≈9:17 PM; median breathing 19.8/min, HR 84 bpm;
  recordings `rec_1783220579.jsonl` (8.9 GB) + `rec_1783225298.jsonl` (7.2 GB). Analysis scripts were ad-hoc
  streaming Python (files are 16 GB — always stream, never load).
- Simulation (no hardware): run the server with `--source simulate`, open `http://localhost:8080/ui/baby.html`.
