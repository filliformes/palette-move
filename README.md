# PALETTE

**A 4-slot serial multi-effect for Ableton Move (Schwung), with a shared palette of 24 effects.**

Inspired by the [Hologram Electronics Chroma Console](https://www.hologram.fyi/), PALETTE keeps
the reorderable-module soul and the DRIFT instability philosophy, but drops the rigid
one-category-per-module rule. There are **4 generic serial slots**, each loading **any** of 24
effects (or Off) — so the Chroma's biggest limitation (no delay + reverb at once) disappears by
construction: just put a delay in one slot and a reverb in another.

## The palette — 24 effects

| Group | Effects |
|-------|---------|
| **Character** | Drive · Sweeten · Fuzz · Howl · Swell · **Fold** |
| **Movement** | Doubler · Vibrato · Phaser · Tremolo · Pitch · **Shift** |
| **Diffusion** | Cascade · Reels · Space · Collage · Reverse · **Bloom** |
| **Texture** | Filter · Squash · Cassette · Broken · Interference · **Freeze** |

The 20 Chroma-inspired effects are re-voiced as original DSP (not clones), plus 4 new originals:
**Fold** (West-Coast wavefolder), **Shift** (Bode linear frequency shifter), **Bloom** (granular
shimmer reverb), **Freeze** (spectral magnitude freeze).

## Layout

- **Presets** (landing page) — 25 factory presets, plus 6 randomizers (Patch / Effect / Amount /
  Macro / Drift), Input Volume, Global Mix, and a menu-only **FX Reorder** (all 24 chain orders).
- **PALETTE** — the Chroma-style 8-knob console: the four slots' Amount/Macro pairs.
- **FX 1&2** / **FX 3&4** — per-slot Select / Amount / Macro / Drift.

Each effect once only: selecting an effect another slot already holds skips to the next free one.
Every slot has its own **Drift** — a per-effect instability macro.

## Build & install

```bash
./scripts/build.sh          # Docker ARM64 cross-compile (Docker Desktop must be running)
./scripts/install.sh        # SCP to ableton@move.local, then power-cycle the Move
```

## License

MIT © Filliformes. Built on MIT-licensed DSP from Mutable Instruments, Airwindows, and the
Filliformes Move-plugin family.
