# PALETTE

**A 4-slot serial multi-effect for Ableton Move (Schwung) — a shared palette of 24 effects, reorderable, with per-effect instability, live randomization, feedback, and tempo sync.**

PALETTE is a reinterpretation of the [Hologram Electronics **Chroma Console**](https://www.hologramelectronics.com/products/chroma-console?variant=50128739533104).
It keeps the Chroma's reorderable-module soul and its DRIFT instability philosophy, but drops
the rigid one-effect-per-category rule: **four fully generic serial slots, each loading any of
24 effects (or Off).** The Chroma's most-cited limitation — no delay *and* reverb at once —
disappears by construction. Every effect has a breakup/instability sweet-spot near its maximum,
in the Chroma spirit of *"exploit the limitations."*

**Author:** Filliformes · **License:** MIT · **API:** `audio_fx_api_v2` · 44100 Hz, stereo.

> 🌐 Web manual: [filliformes.github.io/palette-move](https://filliformes.github.io/palette-move/) · 📦 [Releases](https://github.com/filliformes/palette-move/releases) · 🔧 [INSTALL.md](INSTALL.md)

---

## Table of contents
1. [Concept & signal flow](#1-concept--signal-flow)
2. [The three knobs: Amount · Macro · Drift](#2-the-three-knobs-amount--macro--drift)
3. [Effect uniqueness (the "skip" rule)](#3-effect-uniqueness-the-skip-rule) — **read this first**
4. [The five pages](#4-the-five-pages)
5. [The 24 effects](#5-the-24-effects)
6. [The GLOBAL page: feedback + tempo sync](#6-the-global-page-feedback--tempo-sync)
7. [Presets & randomizers](#7-presets--randomizers)
8. [Live-use tips](#8-live-use-tips)
9. [Install](#9-install)
10. [Build from source](#10-build-from-source)
11. [Credits & license](#11-credits--license)

---

## 1. Concept & signal flow

```
IN → [Input Vol] → SLOT a → SLOT b → SLOT c → SLOT d → [Mix] → OUT     (stereo)
                     ↑______________ Global Feedback send ______________|
                     (a,b,c,d = the four slots in FX-Reorder order, switchable live)
```

- **4 generic slots.** Each loads any of 24 effects, or Off. The **FX Reorder** menu picks any
  of the 24 chain permutations in real time, so you can move a reverb before a fuzz, run a
  reverse delay into a pitch shifter, etc.
- **Input Vol** (global, pre-slots) sets the drive/breakup point going in (0–2×, unity = 1).
- **Mix** (global, equal-power dry/wet) blends the processed chain against the dry input.
- **Global Feedback** (GLOBAL page) sends the chain's output back to its input for drones,
  infinite reverbs and controlled self-oscillation.
- **Click-free everywhere:** 15 ms analog-style smoothing on every knob and preset load; a 25 ms
  crossfade when an effect is switched; and a fade-out→apply→fade-in *duck* when you randomize
  or change presets.

---

## 2. The three knobs: Amount · Macro · Drift

Every effect exposes exactly three controls, Chroma-style:

- **Amount** — how much. **At 0% the effect is bypassed** (loaded but silent), so you can keep
  four effects on standby and ride them in live. This is the core performance gesture.
- **Macro** — the secondary control; its meaning changes per effect (tone, rate, size, pitch,
  resonance…), like the Chroma's Tilt/Rate/Time knob.
- **Drift** — a per-effect instability macro: bounded random pitch wander + progressive
  degradation, voiced to stay musical at every position, never unusable.

Knob response is **accelerated**: fine (~1%) when turned slowly for precision, up to ~7% on a
fast spin, so a full 0→100 % sweep is one brisk turn.

---

## 3. Effect uniqueness (the "skip" rule)

**Each of the 24 effects can occupy at most one slot at a time.** (Off is exempt — any number of
slots can be Off.) When you scroll a slot's Select onto an effect another slot already holds, the
encoder **skips past it** to the next free effect, in the direction you're scrolling.

> ⚠️ **This trips up new users.** If turning a slot's Amount/Macro seems to "do nothing," check
> that the effect you *think* is there didn't get skipped — the same effect may be sitting on a
> different slot. Every patch is always four *distinct* effects.

---

## 4. The five pages

The module opens on the **PALETTE** console. Pages:

| Page | Knobs |
|------|-------|
| **PALETTE** | the Chroma-style 8-knob console — FX1–4 **Amount/Macro** pairs |
| **PRESETS&RND** | Current Preset · Rnd Patch · Rnd Effect · Rnd Amount · Rnd Macro · **Rnd Values** · Input Vol · Mix · *(menu)* FX Reorder |
| **FX 1&2** | FX1 Select/Amount/Macro/Drift · FX2 Select/Amount/Macro/Drift |
| **FX 3&4** | same for slots 3 & 4 |
| **GLOBAL** | Feedback · Tempo Src · Tempo · Time Division |

Amount and Macro are the **same parameters** on the PALETTE console and the FX pages.

---

## 5. The 24 effects

Grouped only for organisation — any effect fits any slot. Columns are **Amount · Macro · Drift**.
**★ = the four originals** not found on the Chroma. All real DSP — see
[vendor/SOURCES.md](vendor/SOURCES.md) for the per-effect sourcing record.

### Character (saturation / dynamics)
| Effect | Amount | Macro | Drift | Engine |
|---|---|---|---|---|
| **Drive** | drive (keeps low end) | tone tilt | bias wander | split-band + asymmetric anti-aliased tanh |
| **Sweeten** | comp + saturation | tone (dark↔air) | level drift | Airwindows Density/Mojo density fold |
| **Fuzz** | sustain / gain | tone / bias | bias instability | two-stage Big-Muff cascade |
| **Howl** | drive + resonance | resonant freq | freq wander | fuzz → near-self-oscillating SVF (sings) |
| **Fold** ★ | fold depth | offset / symmetry | fold wobble | authentic Warps wavefolder LUT |
| **Swell** | swell time | sensitivity | threshold jitter | Airwindows Swell auto-volume |

### Movement (modulation)
| Effect | Amount | Macro | Drift | Engine |
|---|---|---|---|---|
| **Doubler** | mix | time | momentary detune | ADT / slapback double-track |
| **Vibrato** | depth | rate | waveshape + FM | fully-stereo modulated delay |
| **Phaser** | stages / intensity | rate | sweep randomness | stereo (quadrature) allpass phaser |
| **Tremolo** | depth | rate | **AutoPan** | Airwindows skew/density; Drift pans the field |
| **Pitch** | mix | pitch (±1 oct) | resolution drop | dual-tap pitch shifter |
| **Shift** ★ | mix | shift Hz (±) | shift wander | Bode single-sideband (Warps Hilbert) |

### Diffusion (time-based)
| Effect | Amount | Macro | Drift | Engine |
|---|---|---|---|---|
| **Cascade** | feedback | time | degrade | BBD bucket-brigade — dark, fast clock warble |
| **Reels** | feedback (self-osc) | time | tape degrade | RE-201 tape echo — bright, wow + flutter + hiss |
| **Collage** | feedback | loop time | glitch grains | granular looping delay |
| **Reverse** | mix | segment time | pitch mod | reverse delay |
| **Space** | wet | size | tone wander | Mutable Clouds reverb |
| **Bloom** ★ | wet | size | shimmer / tone | Clouds reverb + octave-up shimmer |

### Texture (destruction)
| Effect | Amount | Macro | Drift | Engine |
|---|---|---|---|---|
| **Filter** | **HP↔LP sweep** (centre = open) | **resonance** | cutoff wander | DJ-style TPT SVF |
| **Squash** | compression | mu character | threshold jitter | Airwindows Pressure4 vari-mu |
| **Cassette** | degrade | tone | warble depth | wow + flutter + tape comp + saturation |
| **Broken** | breakdown | rate | dropout randomness | periodic motor-stall pitch drops + AM/FM |
| **Interference** | crush | carrier / tone | static | Airwindows DeRez2 telecom crush |
| **Halo** ★ | resonance / sustain | root + brightness | voice detune | Karplus harmonic resonator pad (Qi / Dark-Star vibe) |

Delay- and LFO-time effects (Cascade, Reels, Doubler, Reverse, Collage; Vibrato, Tremolo, Phaser)
glide smoothly when you turn their time/rate Macro — no zipper or pitch-click.

---

## 6. The GLOBAL page: feedback + tempo sync

The performance layer.

- **Feedback** — a global send: the chain's wet output is fed back into its input, DC/sub
  high-passed and soft-limited. The whole 0–100 % range is usable: low = subtle repeats, high =
  lush infinite wash, and the **top ~20 % self-oscillates into a soft-limited drone/howl** (the
  Chroma "pleasingly explode" behaviour). It's cleared automatically on any patch/preset change so
  a hot loop never carries over.
- **Tempo Src** — **Move** (lock to the Move's MIDI clock) or **Int** (use the Tempo knob).
- **Tempo** — 10–500 BPM (used when Src = Int, or as a fallback).
- **Time Division** — **Free** (continuous, per-effect Macro time) or a musical division
  (1/1 … 1/32, with triplet `T` and dotted `D`). When set, the delays and LFOs snap to the grid.

> Tempo Src = **Move** depends on the Move forwarding MIDI clock to audio-FX; if it doesn't,
> PALETTE falls back to the Int tempo automatically. **Int** always works.

---

## 7. Presets & randomizers

**50 factory presets**, grouped utilitarian → character → weird → experimental, each a chain the
Chroma can't do. Every preset loads **four distinct effects** — the featured ones up, the rest at
0 % (loaded, silent, ready to bring in). Tempo and Feedback are global, so changing presets keeps
your performance settings. **Init** is the classic chain **Drive → Doubler → Cascade → Filter**,
transparent by default.

Six **momentary** randomizers (one tap = one fire), all across the four slots at once:

- **Rnd Patch** — a whole new patch: four effects + all params.
- **Rnd Effect** — new effect types, keep the current params.
- **Rnd Amount / Macro** — randomise just that knob across all slots.
- **Rnd Values** — Amount + Macro + Drift together, **keeping the effects** (the "tweak the
  random chain" move).

The randomizer is voiced for musicality: ~70 % of Rnd Patch/Effect rolls are **balanced** (one
effect per category, in random slot order — the Chroma feel), ~30 % are free stacks — and a hard
rule caps it at **no more than two drive/distortion effects** at once. It's seeded from OS entropy
(random from the first tap) and ducks click-free when it fires.

---

## 8. Live-use tips

- Keep four effects loaded at 0 % and ride the **Amount** knobs on the PALETTE console — that's
  the Chroma performance feel.
- **Filter** is a DJ HP/LP sweep (centre = open), **Tremolo Drift = AutoPan**, **Halo** rings out
  a tuned harmonic chord, **Howl** near max sings — all great for hands-on moves.
- A touch of **GLOBAL → Feedback** turns any delay/reverb into a self-building texture; the top of
  the knob self-oscillates (soft-limited, won't damage), so back it off to tame it.
- Use **Rnd Effect** to discover combinations, then **Rnd Values** to explore variations of a
  chain you like.
- Remember the [skip rule](#3-effect-uniqueness-the-skip-rule): every patch is four *distinct*
  effects.

---

## 9. Install

**No build required.** Grab `palette-module.tar.gz` from the
[Releases](https://github.com/filliformes/palette-move/releases) page and side-load over SSH:

```bash
scp palette-module.tar.gz ableton@move.local:/tmp/
ssh ableton@move.local 'mkdir -p /data/UserData/schwung/modules/audio_fx && \
  tar -xzf /tmp/palette-module.tar.gz -C /data/UserData/schwung/modules/audio_fx/ && \
  rm /tmp/palette-module.tar.gz'
```

Then **power-cycle the Move** and add PALETTE as an audio FX. Full steps + troubleshooting →
**[INSTALL.md](INSTALL.md)**. (Once PALETTE is in the Schwung Module Store, you can also install it
from the Manager at `http://move.local:7700`.)

---

## 10. Build from source

```bash
./scripts/build.sh          # Docker ARM64 cross-compile (Docker Desktop must be running)
./scripts/install.sh        # SCP to ableton@move.local, then power-cycle the Move
```

**Architecture:** host + 21 pure-C effects in `src/dsp/palette.c`; the two Clouds reverbs
(SPACE/BLOOM) in `src/dsp/fx_clouds.cc`; authentic Warps lookup tables in `src/dsp/warps_data.c`.
Three translation units, cross-compiled for aarch64 and compiled `-Wall -Wextra` clean. See
[CLAUDE.md](CLAUDE.md) for the full build notes.

---

## 11. Credits & license

**MIT** © Filliformes. Built on MIT-licensed DSP from **Mutable Instruments** (Clouds, Warps —
Émilie Gillet), **Airwindows** (Chris Johnson), **Signalsmith Audio** (Geraint Luff), and the
Filliformes Move-plugin family (super-boom, mello, krautdrums). Inspired by the Hologram
Electronics [Chroma Console](https://www.hologramelectronics.com/products/chroma-console?variant=50128739533104).
Per-effect sourcing is documented in [vendor/SOURCES.md](vendor/SOURCES.md).
