# PALETTE

**A 4-slot serial multi-effect for Ableton Move (Schwung), with a shared palette of 24 effects.**

Inspired by the [Hologram Electronics Chroma Console](https://www.hologram.fyi/) — its
reorderable-module soul and DRIFT instability philosophy — but with more live flexibility, a
genuinely stereo path, and four new originals the Chroma doesn't have. Four generic serial slots,
each loading **any** of 24 effects (or Off), so the Chroma's biggest limitation (no delay + reverb
at once) disappears: just put a delay in one slot and a reverb in another.

> **📖 Full manual: [MANUAL.md](MANUAL.md)**
>
> **Status: v0.1.0 — in testing, not yet released.** Shared with the Schwung community for
> feedback on what to add/improve. The GLOBAL page (feedback + tempo sync) is the newest piece
> and still being validated on hardware.

## The 24 effects

| Group | Effects |
|-------|---------|
| **Character** | Drive · Sweeten · Fuzz · Howl · **Fold** · Swell |
| **Movement** | Doubler · Vibrato · Phaser · Tremolo · Pitch · **Shift** |
| **Diffusion** | Cascade · Reels · Collage · Reverse · Space · **Bloom** |
| **Texture** | Filter · Squash · Cassette · Broken · Interference · **Freeze** |

Each effect gets **Amount · Macro · Drift**; at Amount 0% it's bypassed (loaded, silent, ready).
Real DSP throughout — Mutable Clouds/Warps, Airwindows, Signalsmith FFT, and re-voiced kernels
from the Filliformes Move family. See [vendor/SOURCES.md](vendor/SOURCES.md).

## Pages

**PALETTE** (8-knob console) · **PRESETS&RND** (50 presets + 6 randomizers + FX Reorder) ·
**FX 1&2** / **FX 3&4** (per-slot Select/Amount/Macro/Drift) · **GLOBAL** (Feedback + tempo sync).

## Build & install

```bash
./scripts/build.sh          # Docker ARM64 cross-compile (Docker Desktop must be running)
./scripts/install.sh        # SCP to ableton@move.local, then power-cycle the Move
```

## License

MIT © Filliformes. Built on MIT DSP from Mutable Instruments, Airwindows, Signalsmith Audio, and
the Filliformes Move-plugin family. Inspired by the Hologram Electronics Chroma Console.
