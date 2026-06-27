# PALETTE — Design Spec

**A 4-slot serial multi-effect for Ableton Move (Schwung), with a shared palette of 24 effects.**
Reinterpretation of the Hologram Chroma Console: keep the reorderable-module soul and the DRIFT
instability philosophy, drop the rigid one-category-per-module constraint, populate with original DSP.

- **Author:** Filliformes
- **Module id:** `palette`
- **Repo:** `filliformes/palette-move`
- **Component type:** `audio_fx` (api_version 2)
- **Status:** Design locked — ready for Stage 2 (DSP fetch) → Stage 3 (scaffold)

---

## 1. Core idea

The Chroma Console locks each of its 4 modules to one effect category (Character / Movement /
Diffusion / Texture). Its single biggest documented limitation is the resulting "one effect per
module" rule — you can't run a delay and a reverb at once, can't stack two drives, etc.

**PALETTE removes that constraint.** There are **4 generic serial slots**. Each slot can load **any**
of **24 palette effects** (or be Off). The signal runs:

```
IN → [Input Volume] → SLOT a → SLOT b → SLOT c → SLOT d → [Mix] → OUT   (stereo)
# a,b,c,d = the 4 slots in FX Reorder order (default 1,2,3,4 — see §4)
```

Because slots are freely assignable, the famous "delay + reverb" problem dissolves by construction:
put a delay in slot 2 and a reverb in slot 3. "Reordering" is no longer a special mode — it's just
which effect you assign to which slot.

---

## 2. The palette — 24 effects

The roster is the Chroma Console's 20 (re-voiced as original DSP, not clones) **+ 4 new originals**
(BLOOM, FOLD, SHIFT, FREEZE). Slots are generic; the grouping below is only for organization and
LED-colour theming.

### Character (saturation / dynamics)
| # | Effect | Character | Amount | Macro | DRIFT behaviour |
|---|--------|-----------|--------|---------|-----------------|
| 1 | DRIVE | tube-ish overdrive, warm→full | intensity | Tone (tilt) | adds bias wander |
| 2 | SWEETEN | preamp: EQ + gentle comp + sat | comp/sat amount | Tone | subtle level drift |
| 3 | FUZZ | vintage fuzz, bias/transient shift across Tone | intensity | Tone (bias) | bias instability |
| 4 | HOWL | resonant filter-fuzz, sustaining/synth-stab | intensity | Tone (res freq) | res-freq wander |
| 5 | SWELL | envelope-triggered volume swell | attack/decay time | sensitivity | trigger threshold jitter |
| 24 | **FOLD** *(new)* | West-Coast wavefolder (Buchla/Serge) | fold depth | symmetry/offset | fold-point wobble |

### Movement (modulation)
| # | Effect | Character | Amount | Macro | DRIFT behaviour |
|---|--------|-----------|--------|---------|-----------------|
| 6 | DOUBLER | stereo double-track → slapback | mix | Rate (time) | random momentary pitch shifts |
| 7 | VIBRATO | pitch mod sine→random warble | depth | Rate | sine→random waveshape + width |
| 8 | PHASER | 2–12 stage allpass phaser | intensity/stages | Rate | waveform randomness |
| 9 | TREMOLO | amp mod sine→square chop | depth | Rate | amp/freq variation |
| 10 | PITCH | ±1 octave + lo-fi grit | wet mix | Rate (pitch) | resolution drop + instability |
| 11 | **SHIFT** *(new)* | Bode linear frequency shifter (inharmonic) | wet mix | Rate (shift Hz, ±) | shift-amount wander |

### Diffusion (time-based)
| # | Effect | Character | Amount | Macro | DRIFT behaviour |
|---|--------|-----------|--------|---------|-----------------|
| 12 | CASCADE | BBD analog delay, dark, self-osc | feedback | Time | pitch mod + degradation |
| 13 | REELS | worn tape echo, gritty | feedback | Time | tape degradation |
| 14 | SPACE | multi-blend reverb (lush, not utilitarian) | wet mix | Time (size) | pitch mod in tail |
| 15 | COLLAGE | glitch/granular looping delay | feedback | Time (subdiv) | random double-speed loops |
| 16 | REVERSE | reverse delay ±1 oct | wet mix | Time (speed/pitch) | pitch mod on repeats |
| 17 | **BLOOM** *(new)* | spectral/granular cloud shimmer reverb | wet mix | Time (size/shimmer) | grain scatter + pitch smear |

### Texture (lo-fi / destruction)
| # | Effect | Character | Amount | Macro | DRIFT behaviour |
|---|--------|-----------|--------|---------|-----------------|
| 18 | FILTER | multimode Tilt / LP / HP | cutoff | mode + resonance | cutoff wander |
| 19 | SQUASH | heavy comp + overdrive | comp/drive amount | Tone | threshold jitter |
| 20 | CASSETTE | wow / flutter / tape degrade | intensity | Tone | warble depth |
| 21 | BROKEN | motor-failure pitch drops + AM/FM | breakdown amount | Rate | dropout randomness |
| 22 | INTERFERENCE | telecom/radio static, bitcrush glitch | intensity | Tone | static randomness |
| 23 | **FREEZE** *(new)* | spectral magnitude freeze / smear | freeze/smear amount | Tone (spectral blur) | spectral drift |

> **Macro** is the slot's secondary control; its meaning changes with the loaded effect, exactly
> like the Chroma's Tilt/Rate/Time. **Every effect gets a DRIFT meaning** (the original only had DRIFT
> on Movement + Diffusion) — that's the per-slot DRIFT decision.

---

## 3. DSP sourcing map (Stage 2 plan)

**Sibling-first** (the simplexity rule): Vincent's own Move plugins are pure C, `audio_fx`-compatible,
already gain-staged for the Docker ARM64 toolchain. Reach for these before external sources.

| Effect | Primary source | Notes / license |
|--------|----------------|-----------------|
| DRIVE | `super-boom-move` `apply_dist` Tube + `sb_tanh` | sibling, MIT |
| SWEETEN | `super-boom-move` `tape_sat` + `mello-move` `biquad_set_peaking` head-bump | sibling |
| FUZZ | `super-boom-move` Fuzz mode + asym shaper; Airwindows `Fuzz`/`Edge` for voicing | sibling + Airwindows (MIT) |
| HOWL | `mello-move`/`verglas-move` SVF + resonant self-osc + drive | sibling |
| SWELL | from scratch: envelope follower → VCA (ref AudioKit env follower) | trivial |
| FOLD ★ | `pichenettes/eurorack` Warps `modulator.{cc,h}` → `ALGORITHM_FOLD` (verified) | Mutable MIT |
| DOUBLER | `krautdrums-move` `delay_tap_read` + wow/flutter LFOs (short times) | sibling |
| VIBRATO | `krautdrums-move` fractional delay + LFO | sibling |
| PHASER | N-stage allpass cascade (from scratch); Faust `pf.phaser2_stereo` as ref | Faust GPL/MIT |
| TREMOLO | from scratch: LFO × gain, sine→square morph | trivial |
| PITCH | `mello-move` Hermite cubic interp + granular pitch; Soundpipe `pshift` | sibling + AudioKit |
| SHIFT ★ | `pichenettes/eurorack` Warps `quadrature_transform.h` + `quadrature_oscillator.h` (single-sideband freq shift, verified) | Mutable MIT |
| CASCADE | `krautdrums-move` delay chain + companding + dark LPF | sibling |
| REELS | `krautdrums-move` RE-201 voicing (canonical kernel) | sibling |
| SPACE | Airwindows `Galactic`/`Verbity` (lush — answers "utilitarian reverb"); or Mutable Clouds reverb | Airwindows MIT / Mutable MIT |
| COLLAGE | `verglas-move` grain cloud + destructive delay edits | sibling |
| REVERSE | `krautdrums-move` delay buffer + reverse read + pitch | sibling-derived |
| BLOOM ★ | `verglas-move` grain cloud → reverb + octave-up shimmer feedback; Mutable Clouds reverb | sibling + Mutable |
| FILTER | `mello-move`/`verglas-move` SVF + tilt EQ (Airwindows `Tilt`/`Capacitor`) | sibling + Airwindows |
| SQUASH | `super-boom-move` drive + compressor (Airwindows `Pressure4`) | sibling + Airwindows |
| CASSETTE | `mello-move` `apply_tape_stage` per-style sat + `krautdrums-move` wow/flutter | sibling |
| BROKEN | from scratch: periodic pitch-drop LFO + dropout gate + AM/FM (ref Airwindows `Desk`) | original |
| INTERFERENCE | bitcrush + noise + ring-mod (Airwindows `DeRez`/`Crunch`; ELSE `crackle~`) | Airwindows + original |
| FREEZE ★ | `verglas-move` spectral freeze (Clouds-port); SoundHack / CDP8 spectral as ref | sibling |

★ = the 4 new originals. Half the palette comes straight from Vincent's sibling plugins, which keeps
the build tractable and Move-native.

---

## 4. UI / knob mapping (Move, api_v2, multi-page `ui_hierarchy`)

Each slot exposes exactly four params: **Select, Amount, Macro, Drift**. "Macro" is the slot's
secondary control — its meaning changes with the loaded effect (the Chroma's Tilt/Rate/Time role).
No per-slot Effect Vol in v1 (Amount + global Mix cover balance). Page 1 surfaces the Amount/Macro
pairs for performance; pages 2–3 expose the full four-param set per slot. Amount and Macro are the
**same parameters** mirrored on page 1 and the FX pages.

**On open → PRESETS** (Ambiotica-style landing) — the module opens focused on Page 4 (Presets); navigate
out into the pages below. 25 presets (see §5).

**Page 1 — PALETTE** (the Chroma-style 8-knob console):

| Knob | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|------|---|---|---|---|---|---|---|---|
| Param | FX1 Amount | FX1 Macro | FX2 Amount | FX2 Macro | FX3 Amount | FX3 Macro | FX4 Amount | FX4 Macro |
| Default | 0% | 0% | 0% | 0% | 0% | 0% | 0% | 0% |

Menu-only on Page 1: **Mix** (mirrored readout, default 100%). Input Volume and Mix get dedicated
knobs on Page 4; **Output is dropped** for v1 (Mix + Input Volume cover gain staging).

**Page 2 — FX 1&2:**

| Knob | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|------|---|---|---|---|---|---|---|---|
| Param | FX1 Select | FX1 Amount | FX1 Macro | FX1 Drift | FX2 Select | FX2 Amount | FX2 Macro | FX2 Drift |
| Default | — | — | — | 0% | — | 0% | — | 0% |

**Page 3 — FX 3&4:**

| Knob | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|------|---|---|---|---|---|---|---|---|
| Param | FX3 Select | FX3 Amount | FX3 Macro | FX3 Drift | FX4 Select | FX4 Amount | FX4 Macro | FX4 Drift |
| Default | — | — | — | 0% | — | 0% | — | 0% |

A fresh patch = **all slots Off**, all Amounts at 0 (clean passthrough), Mix 100%, Input Volume unity.
**FX Select** is a 25-way enum (24 effects + **Off**, default Off).

**Page 4 — PRESETS** (the landing page on open):

| Knob | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|------|---|---|---|---|---|---|---|---|
| Param | Current Preset | Rnd Patch | Rnd Effect | Rnd Amount | Rnd Macro | Rnd Drift | Input Volume | Global Mix |
| Type | enum 1–25 | trigger | trigger | trigger | trigger | trigger | 0–unity–boost | 0–100% |

Menu-only on Page 4: **FX Reorder** — enum of all **24 chain permutations** of the 4 slots
(`1-2-3-4` … `4-3-2-1`), default `1-2-3-4`, switchable in real time. Reorders only the processing
path; each slot keeps its effect, params and buffers (so a reverb tail stays with its slot — only the
connection order changes). Implementation: hold `uint8_t order[4]` decoded from the enum; the render
loop iterates slots in `order[]` instead of 0…3. Optional 2–5 ms output crossfade on change to
smooth the path-switch transient. Saved per preset; orthogonal to the uniqueness constraint.

Randomizer semantics (all scoped across **all 4 slots at once**):
- **Rnd Patch** — fully new patch: random Select + Amount + Macro + Drift on every slot (everything). Picks **4 distinct effects** (no repeats).
- **Rnd Effect** — random Select per slot, **keeps** current Amount/Macro/Drift values. Picks **4 distinct effects**.
- **Rnd Amount** — random Amount on all 4 slots only.
- **Rnd Macro** — random Macro on all 4 slots only.
- **Rnd Drift** — random Drift on all 4 slots only.

### Effect uniqueness constraint

Each of the 24 effects may occupy **at most one slot at a time** — if DRIVE is in FX1, it's
unavailable in FX2/3/4. **Off is exempt** (any number of slots may be Off). Enforcement lives in the
DSP `set_param("fxN_select", v)` handler, which maintains the invariant on every assignment path
(manual scroll, MIDI CC, preset load, randomizer). Random Patch / Random Effect sample **without
replacement** from the 24; factory presets are authored to respect it by construction.

**Conflict policy = SKIP** (taken effects are unselectable; the knob jumps over them to the next free
one). Implementation:

- The Select enum in `module.json` stays static (Off + all 24); Skip is a runtime remap in `set_param`.
- Track per-slot `select_idx` and `prev_idx`. On `set_param("fxN_select", target)`, infer scroll
  direction `dir = sign(target - prev_idx)` (host steps ±1 per detent).
- If `target` is Off **or** not held by another slot → accept. Otherwise keep walking in `dir`
  (clamp at the ends — don't wrap) until a free effect or Off is found, and land there. Result: the
  encoder visibly skips occupied effects.
- `get_param("fxN_select")` must return the **landed** index (not the requested one) so the Shadow UI
  re-syncs — same mechanism as the auto-revert triggers (enum type re-queries `get_param` after set).
- Randomizers and preset-load write the 4 selects **atomically** with pre-validated distinct values,
  bypassing the per-step skip walk (no transient-conflict glitches).
- When a slot frees an effect (→ Off or reassigned), it immediately becomes selectable again elsewhere.

Triggers are **wide-range `int` tap-to-fire** (`{"type":"int","min":0,"max":127,"step":1}`), firing
when `atoi(val)!=0` — NOT `enum ["0","1"]` (which sticks at 1 on the v0.9.10 knob engine). Use
bounded `rnd_int()` and helper-before-caller ordering. *[No Save knob in this layout — preset Save is assumed menu-only or
host-handled; confirm whether you want user-savable slots or factory-only + live randomizers in v1.]*

`get_param("ui_hierarchy")` must be implemented in the DSP and mirror `module.json` exactly (Schwung
reads the DSP response at runtime; if it returns -1 the module falls back to the preset browser).
The module opens focused on **Page 4 (Presets)**; exact preset-landing wiring to be matched against
`charlesvestal/schwung-ambiotica` when coding.

---

## 5. Meta-features (v1)

- **Per-slot DRIFT** — Chroma-style. Each slot owns its DRIFT knob; the *meaning* is defined per-effect
  (see palette tables). Implementation: each effect exposes a `drift` input 0..1; map it to that
  effect's instability target(s). Use bounded `rnd_int()` and NaN-guards on any modulated freq.
- **Input Volume** — replaces the Chroma's auto-listen calibration with a plain controllable
  input-gain/headroom knob (global, pre-slots). Sets the drive/comp breakup point.
- **Whole-plugin presets — 25, preset browser as the landing page** (Ambiotica-style: the module
  opens directly to presets). A preset snapshots: the 4 FX Select assignments + every per-slot
  Amount / Macro / Drift, plus global Mix / Input Volume / FX Reorder (chain order). Six live randomizers (Rnd Patch / Effect / Amount / Macro / Drift) plus Current Preset live on Page 4. Ship 25 factory presets that show
  off slot combinations the Chroma physically can't do (delay→reverb, FUZZ→PHASER→SPACE, FOLD→SPACE,
  FREEZE→REELS, etc.). (Rnd buttons are wide-range `int` tap-to-fire — fire on `atoi(val)!=0`; bounded `rnd_int()`;
  helper-before-caller ordering.)

**Dropped for v1:** CAPTURE (looper/sustainer) and GESTURE (knob-motion recorder). Both are candidates
for v2 — GESTURE overlaps with Move's own param automation, CAPTURE is RAM-heavy.

---

## 6. CPU / RAM budget (the real constraint)

- **CPU is bounded by 4 active effects**, not 24 — only one effect runs per slot. Worst case = the 4
  heaviest effects active at once (e.g. BLOOM + FREEZE + SPACE + COLLAGE = four spectral/granular
  engines). Keep each effect lean; consider a soft "spectral effects max 2 simultaneously" guidance in
  presets, or accept the ceiling and document it.
- **RAM:** each slot needs the buffers for its *current* effect. Delays/reverbs/loopers are the cost.
  Budget for up to 4 long delay/reverb buffers simultaneously (stereo float). Allocate per-slot
  scratch sized to the largest possible effect, reused on effect change (don't malloc per block).
- **Effect switching:** crossfade or ramp on Effect Select change to avoid clicks; clear/zero the
  slot's buffers on switch but ramp the output.

---

## 7. Build plan (pipeline stages)

1. **Stage 2 — Fetch:** pull the sibling kernels + the 4 external sources (Mutable Warps, Faust
   freqshifter, Airwindows Galactic, plus verify Verglas spectral). License-check each.
2. **Stage 3 — Scaffold:** `/scaffold-audio-fx palette` → repo at `palette-move/`. Verify api_version 2
   and top-level `component_type: "audio_fx"`.
3. **Stage 4 — Code:** build the slot host + effect dispatch table first (4 generic slots calling into
   a `palette_effect_t` vtable), then implement effects in batches (start with the all-sibling ones:
   DRIVE, REELS, CASSETTE, SPACE, then the 4 new originals, then the rest).
4. **Stage 5 — DSP review** → **Stage 6 — Build/deploy** → **Stage 7 — Test on device** (mandatory
   pause) → **Stage 8 — UI review** → **Stage 9–10 — Release + catalog PR**.

> Stages 6–7 (Docker ARM64 build + SSH deploy to `move.local`) run in Claude Code on the CachyOS
> machine — not reachable from this web session. This spec + the fetched DSP is the hand-off package.

---

## 8. Architecture sketch (effect vtable)

```c
typedef struct {
    void  (*init)(void *state, float sr);
    void  (*process)(void *state, float *L, float *R, int n,
                     float amount, float macro, float drift);
    void  (*reset)(void *state);   /* on effect switch / preset load */
    size_t state_size;
    const char *name;
    uint8_t group;                 /* Character/Movement/Diffusion/Texture — LED colour */
} palette_effect_t;

/* 24-entry table; each slot holds an index + a reused state buffer sized to max(state_size). */
```

This keeps the host trivial and each effect self-contained and individually testable.
