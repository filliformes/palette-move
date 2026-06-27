# PALETTE — DSP sourcing map (Stage 2 /dsp-fetch result)

Per-effect source for all 24 palette effects. **Sibling-first** (the simplexity rule):
Vincent's own Move plugins are extracted/re-voiced inline (not vendored wholesale — the 20
Chroma-inspired effects are "re-voiced as original DSP, not clones"). The 4 new originals +
the lush reverb come from two **MIT Mutable Instruments engines already adapted to the Move
ARM64 toolchain** in sibling repos (Deforme→Warps, Verglas→Clouds) and vendored here.

All sources are **MIT** (Mutable: © 2014 Émilie Gillet; Airwindows: © Chris Johnson;
Filliformes siblings: © Filliformes). PALETTE ships MIT.

---

## Vendored engines (pinned, local, compilable)

```
vendor/warps_engine/     ← from deforme-move/src  (Warps DSP + its own stmlib)
    warps/dsp/           modulator.{h,cc}, quadrature_transform.h, quadrature_oscillator.h,
                         oscillator.*, vocoder.*, filter_bank.*, parameters.h, limiter.h,
                         sample_rate_converter.*
    warps/resources.*    lookup tables (incl. lut_bipolar_fold — needed by FOLD)
    stmlib/              Warps-era stmlib (dsp.h, filter.h, units.*, atan.*, random.*, ...)

vendor/clouds_engine/    ← from verglas-move/src  (Clouds DSP + its own stmlib)
    clouds/dsp/fx/       reverb.h, fx_engine.h, diffuser.h, pitch_shifter.h   ← SPACE/BLOOM
    clouds/dsp/pvoc/     phase_vocoder.{h,cc}, stft.{h,cc}, frame_transformation.{h,cc} ← FREEZE
    clouds/dsp/          granular_processor.{h,cc}, grain.h, ...               ← COLLAGE/BLOOM grain
    clouds/resources.*   lookup tables (window, sine)
    stmlib/              Clouds-era stmlib (incl. fft/shy_fft.h for FREEZE)
```

### ⚠ KEY INTEGRATION CONSTRAINT (Stage 4)
The Warps stmlib and the Clouds stmlib **DIFFER** (dsp.h, filter.h, cosine_oscillator.h,
parameter_interpolator.h, units.h all changed between the two ports). PALETTE is the first
plugin to combine both engines in one `.so`. **Keep them isolated:**
- Compile Warps-derived TUs with `-Ivendor/warps_engine` only.
- Compile Clouds-derived TUs with `-Ivendor/clouds_engine` only.
- Never put both stmlib roots on the same TU's include path.
- Mutable stmlib is mostly inline/template classes (`stmlib::OnePole`, etc.); identical class
  names with differing bodies across TUs = technical ODR. Mitigations, in order of preference:
  1. **Minimal extraction** (preferred for FOLD/SHIFT): lift the ~15-line kernel into a plain
     C/C++ TU with no Mutable includes — sidesteps stmlib entirely.
  2. Wrap each engine's TUs in a distinct `namespace palette_warps { }` / `palette_clouds { }`.
  3. Only one engine per effect TU; rely on internal linkage where possible.
- `-fno-exceptions -fno-rtti` already set in the build for the C++ TUs.

The build (`scripts/build.sh`, `release.yml`) compiles `src/dsp/*.c` (gcc) + `src/dsp/*.cc`
(g++) and links with g++. To add a Mutable TU, drop a thin wrapper `.cc` in `src/dsp/` that
includes from the right vendor root, and add the engine's required `.cc` files (e.g.
`clouds/dsp/pvoc/*.cc`) to the compile list with the matching `-I`.

---

## The palette — per-effect sourcing

Sibling paths are repos under `C:/Users/filli/Projects/`. Line numbers are a guide — grep the
function name (files evolve). `audio_fx` siblings process float in-place; gain-staging matches.

### Character
| FX | Source | Notes |
|----|--------|-------|
| **Drive** | `super-boom-move` `superboom.c`: `sb_tanh` (Padé tanh, L39), `apply_dist` Tube/Boost (L~362) | amount=drive, macro=tone tilt, drift=bias wander. Worked stub already in palette.c. |
| **Sweeten** | `super-boom-move` `tape_sat` (L217) + `mello-move` `biquad_set_peaking` head-bump (L1170) | preamp: gentle EQ + comp + sat. macro=tone. |
| **Fuzz** | `super-boom-move` Fuzz mode (`apply_dist`, asym `sb_tanh`, L~238/269) + Airwindows `Fuzz`/`Edge` voicing | macro=bias/tone; drift=bias instability. |
| **Howl** | `mello-move` SVF (grep `svf`) self-osc + drive; ref Verglas SVF | resonant filter-fuzz. macro=res freq, drift=res wander. |
| **Swell** | from scratch: envelope follower → VCA gain | trivial; amount=attack/decay, macro=sensitivity, drift=threshold jitter. |
| **Fold** ★ | `vendor/warps_engine` `warps/dsp/modulator.cc` `Xmod<ALGORITHM_FOLD>` (L991) + `lut_bipolar_fold` in `warps/resources.cc` | West-Coast wavefolder. Kernel is ~8 lines (see below). **Minimal-extraction option:** replace `Interpolate(lut_bipolar_fold,…)` with an analytic triangle/sine fold to avoid pulling 248 KB resources.cc. macro=symmetry/offset, drift=fold-point wobble. |

**FOLD kernel (from modulator.cc, the LUT path):**
```c
// sum = x1 + x2 + x1*x2*0.25;  here x2=0 (single input) → sum = x; macro adds offset p2
sum *= 0.02f + amount;                 // fold depth
sum += macro_offset;                   // p2: asymmetry/offset
const float kScale = 2048.0f / (2.25f * 1.02f);
out = Interpolate(lut_bipolar_fold + 2048, sum, kScale);   // bipolar fold LUT, 4096 pts
```

### Movement
| FX | Source | Notes |
|----|--------|-------|
| **Doubler** | `krautdrums-move` `krautdrums.c` `delay_tap_read` (L~1474) + wow/flutter LFOs, short times | stereo double-track/slapback. macro=time. |
| **Vibrato** | `krautdrums-move` fractional delay + LFO | macro=rate; drift=sine→random waveshape. |
| **Phaser** | from scratch: N-stage allpass cascade (2–12) + LFO | macro=rate; amount=intensity/stages. Faust `pf.phaser2_stereo` as ref (don't vendor — GPL). |
| **Tremolo** | from scratch: LFO × gain, sine→square morph | worked stub already in palette.c. |
| **Pitch** | `mello-move` Hermite cubic interp + granular pitch (grep `Hermite`/`grain`) | ±1 oct + lo-fi grit. macro=pitch, drift=resolution drop. |
| **Shift** ★ | `vendor/warps_engine` `warps/dsp/quadrature_transform.h` (+ `quadrature_oscillator.h`) | Bode single-sideband freq shifter (inharmonic, NOT pitch). `quadrature_transform.h` needs only stmlib dsp.h+filter.h — **light, self-contained**. For the carrier, use stmlib `cosine_oscillator.h` (avoid `quadrature_oscillator.h`'s dependency on warps/resources). macro=shift Hz (±), drift=shift wander. |

### Diffusion
| FX | Source | Notes |
|----|--------|-------|
| **Cascade** | `krautdrums-move` delay chain + companding + dark LPF (`delay_saturate` L~1462, fb chain L~1526) | BBD analog delay, dark, self-osc. amount=feedback (`fb_cap` 0.95). |
| **Reels** | `krautdrums-move` RE-201 voicing (`fb_cap=1.05` self-osc, L836) | worn tape echo, gritty. The canonical regenerative-tape kernel. |
| **Space** | `vendor/clouds_engine` `clouds/dsp/fx/reverb.h` (+ `fx_engine.h`) | **lush** Clouds reverb (answers the "utilitarian reverb" complaint). **Clean & light — no resources.cc needed**; reverb.h + fx_engine.h + a few stmlib headers only. amount=wet, macro=size. |
| **Collage** | `verglas-move` `clouds/dsp/granular_processor` grain cloud + destructive delay edits | glitch/granular looping delay. drift=random double-speed loops. |
| **Reverse** | `krautdrums-move` delay buffer + reverse read + ±1 oct pitch | macro=speed/pitch. |
| **Bloom** ★ | `vendor/clouds_engine`: grain cloud (`granular_processor`) → `reverb.h` + octave-up shimmer via `fx/pitch_shifter.h` in the feedback | spectral/granular shimmer reverb. amount=wet, macro=size/shimmer, drift=grain scatter. |

### Texture
| FX | Source | Notes |
|----|--------|-------|
| **Filter** | `mello-move`/`verglas-move` SVF + tilt EQ; Airwindows `Tilt`/`Capacitor` ref | multimode Tilt/LP/HP. Worked LP stub already in palette.c — add tilt/HP + resonance. macro=mode+res. |
| **Squash** | `super-boom-move` drive + compressor; Airwindows `Pressure4` ref | heavy comp + OD. macro=tone. |
| **Cassette** | `mello-move` `apply_tape_stage` (L1253, per-style sat) + `krautdrums-move` wow/flutter | wow/flutter/degrade. macro=tone, drift=warble depth. |
| **Broken** | from scratch: periodic pitch-drop LFO + dropout gate + AM/FM; Airwindows `Desk` ref | motor-failure. amount=breakdown, macro=rate. |
| **Interference** | from scratch: bitcrush + noise + ring-mod; Airwindows `DeRez`/`Crunch`, ELSE `crackle~` ref | telecom/radio static. macro=tone, drift=static randomness. |
| **Freeze** ★ | `vendor/clouds_engine` `clouds/dsp/pvoc/{phase_vocoder,stft,frame_transformation}.cc` + `stmlib/fft/shy_fft.h` | spectral magnitude freeze/smear. Heaviest effect (FFT). amount=freeze/smear, macro=spectral blur, drift=spectral drift. See /move "Spectral/PaulStretch pitfalls" lessons. |

★ = the 4 new originals (FOLD, SHIFT, BLOOM, FREEZE) + SPACE's lush reverb.

---

## Airwindows reference (voicing only — fetch per-effect if used)
Self-contained MIT one-.cpp/.h pairs from `airwindows/*`. Not vendored; pull the specific
plugin during Stage 4 if its voicing is wanted: Fuzz/Edge (Fuzz), Pressure4 (Squash),
Tilt/Capacitor (Filter/Sweeten), Galactic/Verbity (Space alt — "lush"), DeRez/Crunch
(Interference), Desk (Broken). Fetch: `pichenettes`-style raw GitHub or the dsp-fetch script
`--browse airwindows`.

## What was NOT vendored (and why)
- **Siblings** (super-boom/mello/krautdrums) — already local repos; effects are re-voiced
  inline per the design ("not clones"), so no copy. Grep the functions above.
- **Airwindows** — pull on demand; each is tiny and standalone.
- **Faust phaser** — GPL; Phaser is written from scratch instead.

## Stage-4 build integration checklist
1. SPACE first (cleanest Mutable win): wrapper `src/dsp/fx_space.cc` includes
   `clouds/dsp/fx/reverb.h`, compiled with `-Ivendor/clouds_engine`. No extra .cc TUs.
2. SHIFT next: minimal `quadrature_transform.h` extraction, `-Ivendor/warps_engine`.
3. FOLD: extract the kernel (analytic fold to skip resources, or include `lut_bipolar_fold`).
4. BLOOM: SPACE reverb + grain + pitch_shifter shimmer feedback.
5. FREEZE last (FFT): add `clouds/dsp/pvoc/*.cc` + `stmlib/dsp/atan.cc`,`units.cc` to the
   Clouds compile list. Mind the OLA/overlap pitfalls in the /move lessons.
6. Namespace-isolate Warps vs Clouds TUs if any symbol clash appears at link.
