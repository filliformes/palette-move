# Installing PALETTE on your Move (no module store)

PALETTE is a Schwung **audio FX** module. This is a side-load — you copy two files onto your
Move over SSH. No Docker, no building, no module store. Takes about 30 seconds.

## What you need

- An Ableton **Move running Schwung**, reachable over the network (`move.local`) or USB.
- **SSH access** to it (the standard Schwung setup: user `ableton`).
- The module tarball **`palette-module.tar.gz`** (from this repo's
  [Releases](https://github.com/filliformes/palette-move/releases) page, or wherever it was shared).

## Install

From a terminal on your computer, in the folder where you downloaded the tarball:

```bash
scp palette-module.tar.gz ableton@move.local:/tmp/
ssh ableton@move.local 'mkdir -p /data/UserData/schwung/modules/audio_fx && \
  tar -xzf /tmp/palette-module.tar.gz -C /data/UserData/schwung/modules/audio_fx/ && \
  rm /tmp/palette-module.tar.gz'
```

This lands the module at `/data/UserData/schwung/modules/audio_fx/palette/`.

## ⚡ Power-cycle the Move

**Hold the power button → wait for full shutdown → turn it back on.** This is required — the
host caches `module.json` at startup, so the new pages/params won't show until a power cycle.
(Removing/re-adding the FX is *not* enough on first install.)

Then add an **audio FX** to a track and pick **PALETTE**. It opens to the PALETTE console; the
menu has PALETTE · PRESETS&RND · FX 1&2 · FX 3&4 · GLOBAL.

## Uninstall

```bash
ssh ableton@move.local 'rm -rf /data/UserData/schwung/modules/audio_fx/palette'
```
…then power-cycle again.

## Troubleshooting

- **`move.local` won't resolve** → use the Move's IP instead (Settings → Wi-Fi shows it), e.g.
  `scp palette-module.tar.gz ableton@192.168.x.x:/tmp/`. Over USB, `move.local` usually works
  once the USB-network adapter comes up.
- **PALETTE doesn't appear / pages look wrong** → you skipped the power cycle, or it didn't fully
  shut down. Power-cycle again.
- **No SSH access?** You need Schwung's SSH enabled (default on the hacked firmware). If `ssh
  ableton@move.local` asks for a password you don't have, check your Schwung setup.

---

This is a **v0.1.0 test build** shared for feedback — see [MANUAL.md](MANUAL.md) for the full
guide and the "what I'm looking for feedback on" section. Bugs/ideas → open an issue or ping on
Discord.
