# PALETTE — Claude Code context

## What this is
A 4-slot serial multi-effect for Ableton Move, **inspired by the Hologram Electronics
Chroma Console**. Keeps the Chroma's reorderable-module soul + DRIFT instability
philosophy, but **drops the rigid one-category-per-module rule**: there are 4 generic
serial slots, each loading any of **24 effects** from a shared palette (or Off). This
dissolves the Chroma's #1 documented limitation (no delay+reverb at once) by construction.

Schwung audio FX module. API: `audio_fx_api_v2`. Language: C (+ C++ for the Mutable-derived
effects). Repo: `filliformes/palette-move`. Author/attribution: **Filliformes** (never the
real name). License: **MIT** (all sources are MIT-compatible — Mutable, Airwindows, siblings).

Processes `audio_inout` (the chain signal at this slot), stereo, 44100 Hz, 128 frames/block.

## Repo structure
- `design-spec.md` — the locked design (full effect roster, UI, sourcing, budget)
- `presets.md` — the 25 factory preset definitions (authored in Stage 4)
- `src/dsp/palette.c` — host + all 21 pure-C effects (slot dispatch in FX-Reorder order,
  `palette_effect_t` vtable, 25-way Skip-walk Select, 6 randomizers, 25 presets, state
  round-trip, page-aware knob overlay, equal-power Mix, ui_hierarchy getter). API typedefs
  inline. All 24 effects implemented (SPACE/BLOOM/FREEZE dispatched to the C++ TUs below).
- `src/dsp/fx_clouds.cc` — SPACE/BLOOM (Mutable Clouds `reverb.h`), C++, `-Ivendor/clouds_engine`.
  Routes FREEZE to fx_spectral.cc. Opaque `extern "C"` heavy interface; palette.c holds a void*.
- `src/dsp/fx_spectral.cc` — FREEZE (Signalsmith `WindowedFFT` spectral OLA), `-Ivendor/signalsmith`.
- `src/dsp/warps_data.c` — authentic Warps `lut_bipolar_fold` + `lut_ap_poles` (plain C arrays,
  MIT) for FOLD/SHIFT. No Warps engine compiled (avoids the Warps↔Clouds stmlib ODR clash).
- `src/dsp/audio_fx_api_v2.h`, `plugin_api_v1.h` — canonical ABI headers (reference; palette.c
  uses inline typedefs and does NOT include them).
- `vendor/` — `airwindows/` (MIT, Chris Johnson), `signalsmith/` (MIT, Geraint Luff),
  `warps_engine/` + `clouds_engine/` (Mutable MIT ports). See `vendor/SOURCES.md`.
- `src/module.json` — metadata + ui_hierarchy + chain_params (authoritative; CI copies it).
- `module.json` — root copy, **kept in sync** with src/module.json.
- `scripts/build.sh` — Windows/MSYS-safe Docker ARM64 build (docker create + docker cp).
  Compiles `.c` (gcc) + `.cc` (g++, `-Ivendor/clouds_engine -Ivendor/signalsmith`), links g++.
  **4 TUs:** palette.c, warps_data.c, fx_clouds.cc, fx_spectral.cc.
- `scripts/install.sh` — flat SCP to `ableton@move.local`.
- `scripts/Dockerfile` — aarch64 toolchain (gcc + g++ + dos2unix).
- `.github/workflows/release.yml` — CI: version check, C/C++ build, release, release.json.

## The palette — 24 effects (+ Off)
Grouped only for LED-colour theming; slots are generic. Each effect gets Amount / Macro /
Drift. `PFX_*` ids and `FX_NAMES[]` order in palette.c is authoritative — keep module.json
enum options identical.

- **Character:** Drive, Sweeten, Fuzz, Howl, Swell, **Fold**★
- **Movement:** Doubler, Vibrato, Phaser, Tremolo, Pitch, **Shift**★
- **Diffusion:** Cascade, Reels, Space, Collage, Reverse, **Bloom**★
- **Texture:** Filter, Squash, Cassette, Broken, Interference, **Freeze**★

★ = the 4 new originals (not in the Chroma): FOLD (West-Coast wavefolder), SHIFT (Bode
linear frequency shifter), BLOOM (granular shimmer reverb), FREEZE (spectral magnitude freeze).

## DSP sourcing map — AS BUILT
**`vendor/SOURCES.md` is the authoritative per-effect record.** Summary of what each effect
actually uses (13/24 on genuinely fetched/ported OSS, 11 original C):
- **Sibling kernels (verbatim, MIT):** DRIVE/FUZZ ← super-boom `sb_apply_dist`; CASCADE/REELS ←
  krautdrums `delay_saturate`; CASSETTE ← mello `tape_cubic`/`tape_asym`.
- **Mutable Clouds (compiled, `fx_clouds.cc`):** SPACE = `reverb.h`; BLOOM = `reverb.h` + shimmer.
- **Signalsmith FFT (`fx_spectral.cc`):** FREEZE = `WindowedFFT` spectral OLA magnitude-freeze.
- **Airwindows (ported per-sample, MIT):** SQUASH ← Pressure4; FILTER ← Capacitor; INTERFERENCE ← DeRez2.
- **Mutable Warps DATA (`warps_data.c`, no engine compiled):** FOLD ← `lut_bipolar_fold`;
  SHIFT ← `lut_ap_poles` 17-pole QuadratureTransform. (Warps engine NOT compiled — its stmlib
  would ODR-clash with Clouds; only the LUTs are extracted as plain C.)
- **Original C (not ported):** SWEETEN, HOWL, SWELL, DOUBLER, VIBRATO, PHASER, TREMOLO, PITCH,
  COLLAGE, REVERSE, BROKEN. (v2 re-sourcing candidates listed in SOURCES.md.)
- **From scratch / trivial:** SWELL (env→VCA), TREMOLO (LFO×gain), PHASER (N-stage allpass),
  BROKEN (pitch-drop LFO + dropout + AM/FM), INTERFERENCE (bitcrush + noise + ring-mod).
- Airwindows refs (MIT) for voicing: Fuzz/Edge, Pressure4, Tilt/Capacitor, Galactic, DeRez.

## UI — 4 pages (api_v2 ui_hierarchy in module.json; DSP mirrors via get_param)
Opens to **Presets** (root level = Page 4, Ambiotica-style landing).
- **Root / Presets (Page 4):** knobs = Current Preset (1–25), Rnd Patch, Rnd Effect,
  Rnd Amount, Rnd Macro, Rnd Drift, Input Volume, Global Mix. **FX Reorder** is menu-only
  here (enum of all 24 chain permutations, default `1-2-3-4`).
- **PALETTE (Page 1):** the Chroma 8-knob console = FX1–4 Amount/Macro pairs (all default 0%).
- **FX 1&2 (Page 2):** FX1 Select/Amount/Macro/Drift, FX2 Select/Amount/Macro/Drift.
- **FX 3&4 (Page 3):** same for slots 3 & 4.
Amount/Macro are the **same params** mirrored on Page 1 and the FX pages. No per-slot Effect
Vol (Amount + global Mix cover balance). Output knob dropped for v1.
Page-aware knob overlay: `set_param("_level","<LevelName>")` → `current_level`; `LEVEL_KNOBS`
maps knob→key per level. Level names: `Presets`, `PALETTE`, `FX12`, `FX34`.

## Effect uniqueness — SKIP policy
Each of the 24 effects occupies **≤1 slot** at a time; **Off is exempt**. Selecting a
taken effect **skips** it (the encoder walks past, clamps at the ends — no wrap) to the next
free effect, in the scroll direction. Implemented in `resolve_select()`/`set_slot_select()`;
`get_param("fxN_select")` returns the **landed** index so the Shadow UI re-syncs. Randomizers
and preset-load write the 4 selects **atomically** with distinct pre-validated values
(sampling without replacement — `pick_distinct_effects()`), bypassing the per-step skip walk.

## Meta-features
- **Per-slot DRIFT** (Chroma-style): each slot owns a `drift` 0..1 → that effect's instability
  target. Use bounded `rnd_int()` + NaN-guard any modulated freq (powf/log2f).
- **Input Volume** (global, pre-slots): replaces the Chroma's auto-calibration with a plain
  input-gain knob (0..2, unity 1). Sets the drive/comp breakup point.
- **Mix** (global, equal-power dry/wet, default 100%).
- **6 randomizers** (all across the 4 slots): Rnd Patch (everything, distinct effects),
  Rnd Effect (types only, keep params, distinct), Rnd Amount / Macro / Drift (that param only).
  Triggers are **wide-range `int` tap-to-fire** (`{"type":"int","min":0,"max":127}`), fire on
  `atoi(val)!=0`. (Spec chose int over enum["0","1"]; if the v0.9.x knob engine sticks, the
  Signal lesson's enum["0","1"] auto-revert is the fallback — revisit at Stage 8 UI review.)

## Presets — DECISION: factory-only in v1
25 **factory** presets, browseable via the Current Preset knob; the 6 randomizers mutate the
live patch. **No user Save in v1** (Page 4 has no Save knob — confirmed with Vincent's layout).
A preset snapshots: 4 FX Selects + per-slot Amount/Macro/Drift + global Mix/Input Volume/FX
Reorder. Still implement `get_param("state")`/`set_param("state")` round-trip so the host
persists the **live patch** per Set (separate from preset save). User-savable slots = v2.
Author the 25 in `presets.md` showing combos the Chroma can't do (delay→reverb,
FUZZ→PHASER→SPACE, FOLD→SPACE, FREEZE→REELS). `load_preset()` must snap all values (no slow
interpolation) and reset slot buffers.

## Critical Schwung rules (from /move + /move-schwung + /scaffold-audio-fx)
- `"audio_in": true` in capabilities — **required** or the module may silently fail to load.
  (Already added.) No `midi_in` in v1 (`on_midi` is NULL).
- `component_type` at **root level AND inside capabilities** (both present).
- NO non-standard root fields (license/source_url, min_host_version) — host JSON parser may
  reject. (Already removed min_host_version.)
- ui_hierarchy lives in module.json; DSP also returns `get_param("ui_hierarchy")` mirroring it
  exactly — **TODO: palette.c does not yet implement `get_param("ui_hierarchy")`**; without it
  the module may fall back to the preset browser / pages inaccessible. Add it in Stage 4.
- chain_params lists ALL params from ALL pages; clean `name`s (page gives context). Present in
  both module.json and DSP `get_param("chain_params")`.
- Root level needs a `knobs` array (it has the Presets knobs) or knobs are inert there.
- Init symbol MUST be `move_audio_fx_init_v2`. get_param returns **-1** for unknown keys
  (never 0); enums return name strings; floats return **raw** values (e.g. "0.5000", not "50%")
  so the state round-trip doesn't corrupt them — display "%" only in `knob_N_value`.
- No malloc/printf/locks in process_block; per-slot buffers calloc'd in create_instance, sized
  to the largest effect, reused on effect switch. Ramp/crossfade output on Select change.
- `-ffast-math` + denormal guards (1e-20f) on one-pole/filter states.
- Power cycle (or remove/re-add FX) required after module.json changes — host caches it.
- No systemctl/schwung service; modules dlopen'd by MoveOriginal.

## CPU / RAM budget
CPU bounded by **4 active effects** (one per slot), not 24. Worst case = 4 spectral/granular
engines at once (BLOOM+FREEZE+SPACE+COLLAGE) — keep each lean; consider preset guidance of
≤2 spectral effects. RAM: budget up to 4 long stereo-float delay/reverb buffers. Skeleton
allocates a 2 s stereo delay line + RNG seed per slot once in create_instance.

## Coding plan (Stage 4)
1. Add `get_param("ui_hierarchy")` to palette.c (mirror module.json) — gates page navigation.
2. Implement effects in batches, sibling-first: DRIVE→(real super-boom voicing), REELS,
   CASSETTE, SPACE, then the 4 originals (FOLD, SHIFT, BLOOM, FREEZE), then the rest.
3. When the first C++ (Mutable) effect lands: refactor palette.c to include the shared headers
   instead of inline structs, split effects into TUs, confirm the multi-TU build links.
4. Wire `load_preset()` + the 25 presets + `state` round-trip.
5. `/dsp-review` → `/move-build` → test on device → `/move-review` → release.

## Build & deploy
```bash
./scripts/build.sh          # Docker ARM64 cross-compile (needs Docker Desktop running)
./scripts/install.sh        # flat SCP to move.local; then power-cycle
```

## Pipeline
Use `/move` to advance stages. First release also needs a module-catalog.json PR to
charlesvestal/schwung (catalog entry: component_type audio_fx, github_repo
filliformes/palette-move, default_branch master, asset palette-module.tar.gz).
