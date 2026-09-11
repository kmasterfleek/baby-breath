# cohort/ — Baby Breath class starter kit

What is in this folder:

- `babybreath.py` + `bbkit/` — the class CLI. `python3 babybreath.py --help`. Steps: setup, detect, flash, provision, bootlog, serve, check, walktest, calibrate. Each ends with a PASS/FAIL checkpoint.
- `RUNBOOK.md` — the human session plan with commands, checkpoints, failure branches and the placement diagram.
- `CLAUDE.md` — instructions for the family's Claude Code session (`cd cohort && claude`).
- `.venv/` and `.last_provision.json` — created by `setup` / `provision`; not shipped.

Not a medical device. A learning toy that shows motion in a room from WiFi reflections.

Instructor pre-class checklist:

1. Flash both boards per family: `python3 babybreath.py flash --port <port>` (needs `setup` first). Label them 1 and 2.
2. Copy the prebuilt server to `babybreath-app/bin/sensing-server` (Mac binary; `chmod +x` it) and make sure `babybreath-app/ui/cohort.html` (the class page) is present.
3. Confirm `firmware/esp32-csi-node/release_bins/` holds bootloader.bin, partition-table.bin, ota_data_initial.bin, esp32-csi-node.bin.
4. Dry-run the whole runbook on your own 2.4 GHz network: provision, bootlog, serve, check, walktest. Note your walk/still ratio so you know what "good" looks like.
5. Ask each family in advance for their WiFi name (exact spelling), whether it is mesh, and to bring two USB data cables.
6. Hand out this folder inside the repository, with Python 3.10+ and Claude Code installed on the family laptop.
