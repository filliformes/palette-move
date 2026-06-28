# PALETTE — User Manual

**A 4-slot serial multi-effect for Ableton Move (Schwung), with a shared palette of 24 effects.**
Inspired by the Hologram Electronics [*Chroma Console*](https://www.hologramelectronics.com/products/chroma-console?variant=50128739533104) —
reorderable-module soul + DRIFT instability philosophy — but with more live flexibility, a
genuinely stereo signal path, and four new originals the Chroma doesn't have.

> **Status: v0.1.0, in testing.** Shared for feedback before public release. The **GLOBAL**
> page (feedback + tempo sync) is the newest addition and still being validated on hardware.
> Tell me what to add or change — that's exactly what this build is for.

---

## 1. The idea

The Chroma Console locks each of its four modules to one effect category, so you can't run a
delay *and* a reverb at once, can't stack two drives, etc. **PALETTE removes that constraint:**
four generic serial slots, each loading **any** of 24 effects (or Off). Want delay → reverb?
Put a delay in one slot and a reverb in another. "Reorder" isn't a special mode — it's just
which effect sits in which slot, switchable live.

```
IN → [Input Vol] → SLOT a → SLOT b → SLOT c → SLOT d → [Mix] → OUT   (stereo, in FX-Reorder order)
                     ↑___________ Global Feedback send ___________|
```

Each effect occupies **at most one slot** — selecting an effect another slot already holds
*skips* past it. Every effect has three knobs: **Amount · Macro · Drift**.

- **Amount** — how much. At **0% the effect is essentially bypassed** (loaded but inaudible),
  so you can keep four effects on standby and dial them in live.
- **Macro** — the secondary control; its meaning changes per effect (tone, rate, size, pitch…),
  exactly like the Chroma's Tilt/Rate/Time.
- **Drift** — a per-effect instability macro. Tasteful at every position; never unusable.

---

## 2. Pages

PALETTE has five menu pages. The landing screen mirrors the **PALETTE** console knobs.

| Page | Knobs |
|------|-------|
| **PALETTE** | FX1–4 **Amount/Macro** pairs — the Chroma-style 8-knob console |
| **PRESETS&RND** | Current Preset · Rnd Patch/Effect/Amount/Macro/Drift · Input Vol · Mix · *(menu)* FX Reorder |
| **FX 1&2** | FX1 Select/Amount/Macro/Drift · FX2 Select/Amount/Macro/Drift |
| **FX 3&4** | same for slots 3 & 4 |
| **GLOBAL** | Feedback · Tempo Src · Tempo · Time Division |

Amount and Macro are the **same parameters** shown on the PALETTE console and the FX pages.
All knob moves and preset loads are **20 ms smoothed** (no zipper), and switching an effect
crossfades over 25 ms (click-free).

---

## 3. The 24 effects

Grouped only for organisation; any effect fits any slot. Each row lists **Amount · Macro · Drift**.

### Character
| Effect | Amount · Macro · Drift | Notes |
|--------|------------------------|-------|
| **Drive** | drive · tone tilt · bias wander | tube-ish overdrive (super-boom voicing) |
| **Sweeten** | comp+sat · tone (dark↔air) · level drift | console preamp (Airwindows Density + Air tilt) |
| **Fuzz** | intensity · bias/tone · bias instability | vintage fuzz (super-boom) |
| **Howl** | drive+resonance · resonant freq · freq wander | resonant filter-fuzz / synth stab (stable TPT SVF) |
| **Fold** ★ | fold depth · offset/symmetry · fold wobble | West-Coast wavefolder (authentic Warps fold curve) |
| **Swell** | swell time · sensitivity · threshold jitter | auto volume-swell (Airwindows Swell) |

### Movement
| Effect | Amount · Macro · Drift | Notes |
|--------|------------------------|-------|
| **Doubler** | mix · time · momentary detune | ADT / slapback double-track |
| **Vibrato** | depth · rate · waveshape + FM | fully-stereo modulated delay |
| **Phaser** | stages/intensity · rate · sweep randomness | frequency-accurate allpass phaser |
| **Tremolo** | depth · rate · **AutoPan** | Airwindows skew/density; Drift pans across the field |
| **Pitch** | mix · pitch (±1 oct) · resolution drop | dual-tap pitch shifter |
| **Shift** ★ | mix · shift Hz (±) · shift wander | Bode single-sideband frequency shifter (Warps Hilbert) |

### Diffusion
| Effect | Amount · Macro · Drift | Notes |
|--------|------------------------|-------|
| **Cascade** | feedback · time · degrade | BBD bucket-brigade delay — bright, fast clock warble |
| **Reels** | feedback (self-osc) · time · tape degrade | worn tape echo (RE-201) — warm, wow+flutter+hiss |
| **Collage** | feedback · loop time · glitch grains | granular looping delay |
| **Reverse** | mix · segment time · pitch mod | reverse delay |
| **Space** | wet · size · tone wander | lush reverb (Mutable Clouds) |
| **Bloom** ★ | wet · size · shimmer/tone | granular shimmer reverb (Clouds + octave-up regen) |

### Texture
| Effect | Amount · Macro · Drift | Notes |
|--------|------------------------|-------|
| **Filter** | **HP↔LP sweep** (centre = open) · **resonance** · cutoff wander | DJ-style filter (stable TPT SVF) |
| **Squash** | compression · mu character · threshold jitter | vari-mu compressor + overdrive (Airwindows Pressure4) |
| **Cassette** | degrade · tone · warble depth | wow + flutter + tape compression + saturation |
| **Broken** | breakdown · rate · dropout randomness | motor-failure pitch drops + AM/FM |
| **Interference** | crush · carrier/tone · static | telecom/radio bit-crush (Airwindows DeRez2) |
| **Halo** ★ | resonance/sustain · root + brightness · voice detune | ethereal harmonic resonator pad — Karplus chord bank (Walrus Qi / OBNE Dark-Star vibe) |

★ = the four originals not in the Chroma: **Fold, Shift, Bloom, Halo**.

---

## 4. GLOBAL page — performance layer

The newest additions, for live use and feedback experimentation.

- **Feedback** — a global send: the chain's wet output is fed back into its input, low-pass
  damped and soft-limited so it can drone, build infinite reverbs, and self-oscillate without
  squealing. 0% = none. A little goes a long way; it interacts with whatever's in the slots.
- **Tempo Src** — **Move** (lock to the Move's clock) or **Int** (use the Tempo knob).
- **Tempo** — 10–500 BPM (when Src = Int, or as a fallback).
- **Time Division** — **Free** (continuous, per-effect Macro time) or a musical division
  (1/1 … 1/32, with triplet `T` and dotted `D`). When set, the time-based effects snap to the
  grid: **delays** (Cascade, Reels, Doubler, Reverse, Collage) and **LFOs** (Vibrato, Tremolo,
  Phaser).

> Tempo Src = **Move** depends on the Move forwarding MIDI clock to audio-FX; if it doesn't,
> PALETTE falls back to the Int tempo automatically. **Int** sync always works.

---

## 5. Presets & randomizers (PRESETS&RND page)

**50 factory presets**, organised utilitarian → character → weird → experimental, each a chain
the Chroma can't do. Every preset loads **four distinct effects** — the ones it features are
turned up, the rest sit at 0% (loaded, silent, ready to bring in live). Tempo and Feedback are
**global**, so changing presets keeps your performance settings.

The **PALETTE / Init** preset is the classic starting chain — **Drive → Doubler → Cascade →
Filter**, all transparent by default (Filter centred/open), ready to dial in.

**Six randomizers** (momentary — one tap fires once), all across the four slots at once:

- **Rnd Patch** — completely new patch (4 distinct effects + all params)
- **Rnd Effect** — new effect types, keep current params
- **Rnd Amount / Macro / Drift** — randomise just that knob across all slots

**FX Reorder** (menu-only) chooses any of the 24 chain permutations in real time.

---

## 6. Live-use tips

- Keep four effects loaded at 0% and ride the **Amount** knobs on the PALETTE console — that's
  the Chroma performance feel.
- **Tremolo Drift = AutoPan**, **Filter** is a DJ HP/LP sweep, **Halo** rings out a tuned
  harmonic chord pad — all great for hands-on moves.
- A touch of **GLOBAL → Feedback** turns any delay/reverb into a self-building texture; back it
  off before it runs away.
- Use **Rnd Effect** to discover combinations, then tweak.

---

## 7. Install / build

```bash
./scripts/build.sh     # Docker ARM64 cross-compile (Docker Desktop must be running)
./scripts/install.sh   # SCP to ableton@move.local, then power-cycle the Move
```

Or grab the release tarball, unpack into
`/data/UserData/schwung/modules/audio_fx/palette/`, and power-cycle.

---

## 8. What I'm looking for feedback on

This is shared for the Schwung community's input. Especially curious about:

- **Which effects need re-voicing?** Levels, character, the four originals (Fold/Shift/Bloom/Halo).
- **Performance layer** — is a global Feedback + tempo sync the right direction, or would you
  rather have pad control (momentary freeze/capture, scene launch), an A/B morph, CV-style
  expression, etc.?
- **Missing effects?** The palette is full at 24, but if something obvious is absent, say so.
- **Workflow** — does the 5-page layout feel right on the Move? Anything awkward?

Open an issue, or ping me on Discord.

---

## Credits & license

**MIT** © Filliformes. Built on MIT-licensed DSP from **Mutable Instruments** (Clouds, Warps —
Émilie Gillet), **Airwindows** (Chris Johnson), **Signalsmith Audio** (Geraint Luff), and the
Filliformes Move-plugin family (super-boom, mello, krautdrums). Inspired by the Hologram
Electronics Chroma Console. See `vendor/SOURCES.md` for the per-effect sourcing record.
