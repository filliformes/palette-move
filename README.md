# PALETTE

**A 4-slot serial multi-effect for Ableton Move (Schwung), with a shared palette of 24 effects.**

PALETTE is a reinterpretation of the [Hologram Electronics **Chroma Console**](https://www.hologramelectronics.com/products/chroma-console?variant=50128739533104) —
it keeps the Chroma's reorderable-module soul and its DRIFT instability philosophy, but trades
the rigid one-effect-per-category rule for **four fully generic serial slots**, adds a genuinely
stereo signal path, a live performance layer, and four new original effects the Chroma doesn't
have. The Chroma's most-cited limitation — no delay *and* reverb at once — disappears by
construction: put a delay in one slot and a reverb in another.

> **📖 Full manual → [MANUAL.md](MANUAL.md)** &nbsp;·&nbsp; 🌐 [filliformes.github.io/palette-move](https://filliformes.github.io/palette-move/)
>
> **Status: v0.1.0 — in testing, not yet released.** Shared with the Schwung community for
> feedback on what to add / improve before a public release. The **GLOBAL** page (feedback +
> tempo sync) is the newest addition and is still being validated on hardware.

---

## Concept

```
IN → [Input Vol] → SLOT a → SLOT b → SLOT c → SLOT d → [Mix] → OUT     (stereo)
                     ↑______________ Global Feedback send ______________|
                     (a,b,c,d = the four slots in FX-Reorder order, switchable live)
```

- **4 generic slots**, each loads **any** of 24 effects (or Off). Reorder the chain in real time.
- **Effect uniqueness:** each effect occupies at most one slot — selecting an effect another slot
  holds *skips* past it. Every patch is four distinct effects.
- **Three knobs per effect — Amount · Macro · Drift:**
  - **Amount** = how much. **At 0% the effect is bypassed** (loaded but silent), so you can keep
    four effects on standby and ride them in live — the Chroma performance feel.
  - **Macro** = the secondary control; meaning changes per effect (tone / rate / size / pitch…),
    like the Chroma's Tilt/Rate/Time.
  - **Drift** = a per-effect instability macro — musical at every position, never unusable.
- **Click-free everywhere:** 20 ms analog-style smoothing on every knob and preset load; 25 ms
  crossfade when switching an effect.

---

## The 24 effects

Grouped for organisation only — any effect fits any slot. **★ = the four originals** not in the
Chroma. Columns are **Amount · Macro · Drift**.

### Character
| Effect | Amount | Macro | Drift | Engine |
|---|---|---|---|---|
| **Drive** | drive | tone tilt | bias wander | super-boom Tube |
| **Sweeten** | comp + sat | tone (dark↔air) | level drift | Airwindows Density + Air |
| **Fuzz** | intensity | bias / tone | bias instability | super-boom Fuzz |
| **Howl** | drive + resonance | resonant freq | freq wander | resonant TPT SVF |
| **Fold** ★ | fold depth | offset / symmetry | fold wobble | authentic Warps fold LUT |
| **Swell** | swell time | sensitivity | threshold jitter | Airwindows Swell |

### Movement
| Effect | Amount | Macro | Drift | Engine |
|---|---|---|---|---|
| **Doubler** | mix | time | momentary detune | ADT / slapback |
| **Vibrato** | depth | rate | waveshape + FM | fully-stereo mod delay |
| **Phaser** | stages / intensity | rate | sweep randomness | frequency-accurate allpass |
| **Tremolo** | depth | rate | **AutoPan** | Airwindows skew/density |
| **Pitch** | mix | pitch (±1 oct) | resolution drop | dual-tap shifter |
| **Shift** ★ | mix | shift Hz (±) | shift wander | Warps Hilbert SSB |

### Diffusion
| Effect | Amount | Macro | Drift | Engine |
|---|---|---|---|---|
| **Cascade** | feedback | time | degrade | BBD — bright, fast clock warble |
| **Reels** | feedback (self-osc) | time | tape degrade | RE-201 tape — wow + flutter + hiss |
| **Collage** | feedback | loop time | glitch grains | granular looping delay |
| **Reverse** | mix | segment time | pitch mod | reverse delay |
| **Space** | wet | size | tone wander | Mutable Clouds reverb |
| **Bloom** ★ | wet | size | shimmer / tone | Clouds + octave-up shimmer |

### Texture
| Effect | Amount | Macro | Drift | Engine |
|---|---|---|---|---|
| **Filter** | **HP↔LP sweep** (centre = open) | **resonance** | cutoff wander | DJ-style TPT SVF |
| **Squash** | compression | mu character | threshold jitter | Airwindows Pressure4 |
| **Cassette** | degrade | tone | warble depth | wow + flutter + tape comp + sat |
| **Broken** | breakdown | rate | dropout randomness | motor-failure + AM/FM |
| **Interference** | crush | carrier / tone | static | Airwindows DeRez2 |
| **Halo** ★ | resonance / sustain | root + brightness | voice detune | Karplus harmonic resonator pad (Qi / Dark-Star vibe) |

All real DSP — see [vendor/SOURCES.md](vendor/SOURCES.md) for the per-effect sourcing record
(Mutable Clouds/Warps, Airwindows, Signalsmith, and re-voiced kernels from the Filliformes Move
family).

---

## Pages

| Page | Contents |
|------|----------|
| **PALETTE** | the Chroma-style 8-knob console — FX1–4 Amount/Macro pairs |
| **PRESETS&RND** | Current Preset · 6 randomizers · Input Vol · Mix · *(menu)* FX Reorder |
| **FX 1&2** / **FX 3&4** | per-slot Select / Amount / Macro / Drift |
| **GLOBAL** | **Feedback** · **Tempo Src** (Move/Int) · **Tempo** (10–500 BPM) · **Time Division** |

**GLOBAL** is the performance layer: a global **feedback** send (drones, infinite reverbs,
self-oscillation — LP-damped and soft-limited so it stays musical), and **tempo sync** — set a
Time Division and the delays (Cascade, Reels, Doubler, Reverse, Collage) and LFOs (Vibrato,
Tremolo, Phaser) lock to the grid, from the Move's clock or an internal BPM.

---

## Presets & randomizers

**50 factory presets**, grouped utilitarian → character → weird → experimental. Every preset
loads **four distinct effects** (the featured ones up, the rest at 0% = loaded and ready). Tempo
and Feedback are global, so changing presets keeps your performance settings. **Init** is the
classic chain **Drive → Doubler → Cascade → Filter**, transparent by default.

Six **momentary** randomizers (one tap = one fire): **Rnd Patch / Effect / Amount / Macro /
Drift**, all across the four slots.

---

## Build & install

```bash
./scripts/build.sh          # Docker ARM64 cross-compile (Docker Desktop must be running)
./scripts/install.sh        # SCP to ableton@move.local, then power-cycle the Move
```

**Just want to test it on your Move (no build)?** Grab `palette-module.tar.gz` from the
[Releases](https://github.com/filliformes/palette-move/releases) page and follow
**[INSTALL.md](INSTALL.md)** — a 30-second SSH side-load, no Docker or module store needed.

**Architecture:** host + 21 pure-C effects in `src/dsp/palette.c`; the three Clouds effects in
`src/dsp/fx_clouds.cc`; authentic Warps lookup tables in `src/dsp/warps_data.c`. See
[CLAUDE.md](CLAUDE.md) for the full build notes.

---

## Feedback wanted

This build is shared for the Schwung community's input — see the
[manual's feedback section](MANUAL.md#8-what-im-looking-for-feedback-on). Especially: effect
voicing, whether the GLOBAL performance layer is the right direction (vs. pad control / A/B
morph / CV-style expression), any obviously-missing effect, and how the 5-page layout feels on
the Move. Open an issue or ping me on Discord.

---

## License

**MIT** © Filliformes. Built on MIT-licensed DSP from **Mutable Instruments** (Clouds, Warps —
Émilie Gillet), **Airwindows** (Chris Johnson), **Signalsmith Audio** (Geraint Luff), and the
Filliformes Move-plugin family. Inspired by the Hologram Electronics
[Chroma Console](https://www.hologramelectronics.com/products/chroma-console?variant=50128739533104).
