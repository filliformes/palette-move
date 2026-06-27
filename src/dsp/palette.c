/**
 * PALETTE — Schwung audio FX module  (HOST SKELETON)
 * 4 serial slots over a shared palette of 24 effects. Reinterpretation of the
 * Hologram Chroma Console. Author: Filliformes. License: MIT.
 *
 * API: audio_fx_api_v2 (in-place stereo int16 interleaved, 44100 Hz, 128 frames/block)
 *
 * ── WHAT THIS FILE IS ────────────────────────────────────────────────────────
 *   The full host architecture: slot dispatch (in FX-Reorder order), the
 *   palette_effect_t vtable, the 25-way Skip-walk Select, the 6 randomizers,
 *   FX Reorder, Input Volume, equal-power Mix, page-aware knob overlay, and
 *   chain_params. The 24 effect DSPs are STUBBED as passthrough (the module
 *   builds and runs as clean passthrough today); fill them in batches per the
 *   design-spec §3 sourcing map. DRIVE / TREMOLO / FILTER are worked examples
 *   showing the per-effect process signature.
 *
 * ── VERIFY IN CLAUDE CODE (against super-boom-move / boris-move / ambiotica) ──
 *   1. Copy the REAL `plugin_api_v1.h` + `audio_fx_api_v2.h` from super-boom-move
 *      into src/dsp/ — the inline struct defs below match the canonical ABI, but
 *      NEVER ship stub headers. If you keep the inline defs, confirm field order.
 *   2. Multi-page knob overlay relies on set_param("_level", "<LevelName>") to
 *      track the active page. Confirm boris-move uses the same "_level" key/values.
 *   3. "Open to Presets" = root level IS the Presets page here. Confirm ambiotica's
 *      landing mechanism (it may set a default level differently).
 *   4. fx_reorder is intended as menu-only on the Presets level — confirm a level
 *      can carry both nav-link params and an editable menu-only param.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>

/* ── host_api_v1_t — MUST match chain_host ABI (13 fields, exact order) ──────── */
typedef int  (*move_mod_emit_value_fn)(void*, const char*, const char*, const char*,
                                       float, float, float, int, int);
typedef void (*move_mod_clear_source_fn)(void*, const char*);

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
    int (*get_clock_status)(void);
    move_mod_emit_value_fn mod_emit_value;
    move_mod_clear_source_fn mod_clear_source;
    void *mod_host_ctx;
} host_api_v1_t;

typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void  (*destroy_instance)(void *instance);
    void  (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void  (*set_param)(void *instance, const char *key, const char *val);
    int   (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void  (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
} audio_fx_api_v2_t;

static const host_api_v1_t *g_host = NULL;

#define SR        44100.0f
#define MAXFRAMES 128
#define NUM_SLOTS 4
#define MAX_DELAY (44100 * 2)   /* 2 s stereo delay line per slot (skeleton) */

static inline float clampf(float x, float lo, float hi){ return x<lo?lo:(x>hi?hi:x); }
static inline int   clampi(int x, int lo, int hi){ return x<lo?lo:(x>hi?hi:x); }

static inline float frand(uint32_t *s){               /* xorshift32 → [0,1) */
    uint32_t x=*s; x^=x<<13; x^=x>>17; x^=x<<5; *s=x;
    return (x>>8)*(1.0f/16777216.0f);
}
static inline int rnd_int(uint32_t *s, int count){    /* bounded 0..count-1 */
    int v=(int)(frand(s)*(float)count); if(v<0)v=0; if(v>=count)v=count-1; return v;
}

/* ── Palette effect IDs (0 = Off; group order = LED colour) ──────────────────── */
enum {
    PFX_OFF = 0,
    /* Character (group 0) */ PFX_DRIVE, PFX_SWEETEN, PFX_FUZZ, PFX_HOWL, PFX_SWELL, PFX_FOLD,
    /* Movement  (group 1) */ PFX_DOUBLER, PFX_VIBRATO, PFX_PHASER, PFX_TREMOLO, PFX_PITCH, PFX_SHIFT,
    /* Diffusion (group 2) */ PFX_CASCADE, PFX_REELS, PFX_SPACE, PFX_COLLAGE, PFX_REVERSE, PFX_BLOOM,
    /* Texture   (group 3) */ PFX_FILTER, PFX_SQUASH, PFX_CASSETTE, PFX_BROKEN, PFX_INTERFERENCE, PFX_FREEZE,
    PFX_COUNT               /* = 25 (Off + 24) */
};
#define NUM_FX (PFX_COUNT - 1)   /* 24 selectable effects */

/* Display names — index 0..24. get_param for selects MUST return these strings. */
static const char *FX_NAMES[PFX_COUNT] = {
    "Off",
    "Drive","Sweeten","Fuzz","Howl","Swell","Fold",
    "Doubler","Vibrato","Phaser","Tremolo","Pitch","Shift",
    "Cascade","Reels","Space","Collage","Reverse","Bloom",
    "Filter","Squash","Cassette","Broken","Interference","Freeze"
};
static const uint8_t FX_GROUP[PFX_COUNT] = {
    0, 0,0,0,0,0,0, 1,1,1,1,1,1, 2,2,2,2,2,2, 3,3,3,3,3,3
};

/* ── Per-slot DSP scratch (shared across effects; each effect uses a subset) ──
 * Allocated once in create_instance, never in process_block. Effect impls add
 * fields here as needed and clear them in slot_reset(). */
typedef struct {
    /* delay-class effects (Cascade/Reels/Collage/Reverse/Doubler/Space-ish) */
    float *dl_l, *dl_r;       /* MAX_DELAY each */
    int    wp;                /* write pointer */
    /* modulation / filters */
    float  lfo;               /* 0..1 phase */
    float  z1l, z1r, z2l, z2r;/* generic one-pole / biquad state */
    float  env;               /* envelope follower (Swell/Squash) */
    uint32_t seed;            /* per-slot deterministic RNG for drift */
    /* TODO: BLOOM/FREEZE spectral + grain state, etc. */
} slot_dsp_t;

typedef struct {
    int   select;             /* PFX_* id (0 = Off) */
    int   prev_select;        /* for Skip-walk direction inference */
    float amount, macro, drift;
    slot_dsp_t dsp;
} slot_t;

/* ── Effect process signature ────────────────────────────────────────────────
 * Stereo, n frames, in-place on l[]/r[] (float, ±1). amount/macro/drift ∈ [0,1].
 * OFF is handled by the host (never dispatched). */
typedef void (*fx_process_fn)(slot_dsp_t*, float*, float*, int,
                              float amount, float macro, float drift);
typedef void (*fx_reset_fn)(slot_dsp_t*);

typedef struct {
    fx_process_fn process;
    fx_reset_fn   reset;      /* may be NULL */
} palette_effect_t;

/* ── Instance ────────────────────────────────────────────────────────────────── */
typedef struct {
    slot_t slots[NUM_SLOTS];
    float  input_vol;         /* 0..2, unity = 1 */
    float  mix;               /* 0..1 global dry/wet */
    int    fx_reorder;        /* 0..23 permutation index (0 = 1-2-3-4) */
    int    current_preset;    /* 1..25 */
    int    current_level;     /* page-aware knob overlay (see LEVELS) */
    uint32_t rng;
    /* scratch buffers for slot processing (per block) */
    float  L[MAXFRAMES], R[MAXFRAMES];
} palette_t;

/* ── 24 chain permutations of {0,1,2,3} in lexicographic order ───────────────── */
static const uint8_t PERM[24][4] = {
    {0,1,2,3},{0,1,3,2},{0,2,1,3},{0,2,3,1},{0,3,1,2},{0,3,2,1},
    {1,0,2,3},{1,0,3,2},{1,2,0,3},{1,2,3,0},{1,3,0,2},{1,3,2,0},
    {2,0,1,3},{2,0,3,1},{2,1,0,3},{2,1,3,0},{2,3,0,1},{2,3,1,0},
    {3,0,1,2},{3,0,2,1},{3,1,0,2},{3,1,2,0},{3,2,0,1},{3,2,1,0}
};
/* "1-2-3-4" style label for the current reorder index into buf */
static void perm_label(int idx, char *buf, int n){
    idx = clampi(idx,0,23);
    snprintf(buf,n,"%d-%d-%d-%d",PERM[idx][0]+1,PERM[idx][1]+1,PERM[idx][2]+1,PERM[idx][3]+1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  EFFECT IMPLEMENTATIONS
 *  Stubs pass through. Three are worked examples (DRIVE, TREMOLO, FILTER).
 *  Fill the rest from design-spec §3 (sibling-first: Mello/KrautDrums/Verglas/
 *  Super Boum), keeping each lean. amount/macro/drift are the slot's 3 knobs.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fx_passthrough(slot_dsp_t *s, float *l, float *r, int n,
                           float amount, float macro, float drift){
    (void)s;(void)l;(void)r;(void)n;(void)amount;(void)macro;(void)drift; /* TODO */
}

/* DRIVE — tube-ish overdrive. amount=intensity, macro=tone (tilt), drift=bias wander.
 * (Placeholder voicing; replace with super-boom-move apply_dist Tube + sb_tanh.) */
static void fx_drive(slot_dsp_t *s, float *l, float *r, int n,
                     float amount, float macro, float drift){
    float g    = 1.0f + amount*8.0f;
    float tilt = (macro - 0.5f)*2.0f;            /* −1..+1 */
    for(int i=0;i<n;i++){
        float bias = drift>0 ? (frand(&s->seed)-0.5f)*drift*0.1f : 0.0f;
        float xl = tanhf(l[i]*g + bias), xr = tanhf(r[i]*g + bias);
        /* simple tilt: blend toward a one-pole LP/HP using z1 as LP state */
        s->z1l += 0.2f*(xl - s->z1l); s->z1r += 0.2f*(xr - s->z1r);
        l[i] = (tilt>=0) ? xl*(1.0f-tilt)+ (xl-s->z1l)*tilt    /* bright */
                         : xl*(1.0f+tilt) - s->z1l*tilt;       /* dark   */
        r[i] = (tilt>=0) ? xr*(1.0f-tilt)+ (xr-s->z1r)*tilt
                         : xr*(1.0f+tilt) - s->z1r*tilt;
        l[i]*=0.7f; r[i]*=0.7f;                   /* level comp */
    }
}

/* TREMOLO — amp mod. amount=depth, macro=rate, drift=instability. */
static void fx_tremolo(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float rate = 0.5f + macro*11.5f;             /* 0.5..12 Hz */
    float inc  = rate / SR;
    for(int i=0;i<n;i++){
        s->lfo += inc; if(s->lfo>=1.0f) s->lfo-=1.0f;
        float tri = 2.0f*fabsf(2.0f*s->lfo-1.0f)-1.0f;          /* −1..1 */
        if(drift>0){ s->z1l += 0.001f*((frand(&s->seed)-0.5f) - s->z1l);
                     tri += s->z1l*drift*2.0f; tri=clampf(tri,-1,1); }
        float g = 1.0f - amount*(0.5f*(1.0f+tri));  /* depth */
        l[i]*=g; r[i]*=g;
    }
}

/* FILTER — tilt/LP/HP one-pole. amount=cutoff, macro=mode+res, drift=cutoff wander.
 * (Skeleton: LP only; add tilt/HP modes + resonance from Mello/Verglas SVF.) */
static void fx_filter(slot_dsp_t *s, float *l, float *r, int n,
                      float amount, float macro, float drift){
    (void)macro;
    float cutoff = 60.0f * powf(300.0f, amount);     /* 60..18k Hz exp */
    if(drift>0){ cutoff *= 1.0f + (frand(&s->seed)-0.5f)*drift; }
    float c = 1.0f - expf(-6.2831853f*clampf(cutoff,20,20000)/SR);
    for(int i=0;i<n;i++){
        s->z1l += c*(l[i]-s->z1l); s->z1r += c*(r[i]-s->z1r);
        l[i]=s->z1l; r[i]=s->z1r;
    }
}

/* NOTE: fx_filter references a placeholder rng — effects that need randomness
 * should carry their own seed in slot_dsp_t. Left intentionally as a TODO marker
 * so each effect owns deterministic, click-free drift. */

/* ── Vtable: index by PFX_* id. OFF has no entry (host skips). ───────────────── */
static const palette_effect_t FX_TABLE[PFX_COUNT] = {
    [PFX_OFF]          = { fx_passthrough, NULL },
    [PFX_DRIVE]        = { fx_drive,       NULL },
    [PFX_SWEETEN]      = { fx_passthrough, NULL },
    [PFX_FUZZ]         = { fx_passthrough, NULL },
    [PFX_HOWL]         = { fx_passthrough, NULL },
    [PFX_SWELL]        = { fx_passthrough, NULL },
    [PFX_FOLD]         = { fx_passthrough, NULL },  /* Warps ALGORITHM_FOLD */
    [PFX_DOUBLER]      = { fx_passthrough, NULL },
    [PFX_VIBRATO]      = { fx_passthrough, NULL },
    [PFX_PHASER]       = { fx_passthrough, NULL },
    [PFX_TREMOLO]      = { fx_tremolo,     NULL },
    [PFX_PITCH]        = { fx_passthrough, NULL },
    [PFX_SHIFT]        = { fx_passthrough, NULL },  /* Warps quadrature SSB shift */
    [PFX_CASCADE]      = { fx_passthrough, NULL },
    [PFX_REELS]        = { fx_passthrough, NULL },  /* KrautDrums RE-201 */
    [PFX_SPACE]        = { fx_passthrough, NULL },  /* Clouds reverb */
    [PFX_COLLAGE]      = { fx_passthrough, NULL },
    [PFX_REVERSE]      = { fx_passthrough, NULL },
    [PFX_BLOOM]        = { fx_passthrough, NULL },  /* grain cloud + shimmer */
    [PFX_FILTER]       = { fx_filter,      NULL },
    [PFX_SQUASH]       = { fx_passthrough, NULL },
    [PFX_CASSETTE]     = { fx_passthrough, NULL },  /* Mello apply_tape_stage */
    [PFX_BROKEN]       = { fx_passthrough, NULL },
    [PFX_INTERFERENCE] = { fx_passthrough, NULL },
    [PFX_FREEZE]       = { fx_passthrough, NULL },  /* Verglas spectral freeze */
};

static void slot_reset(slot_t *sl){
    sl->dsp.wp=0; sl->dsp.lfo=0;
    sl->dsp.z1l=sl->dsp.z1r=sl->dsp.z2l=sl->dsp.z2r=0; sl->dsp.env=0;
    if(sl->dsp.dl_l) memset(sl->dsp.dl_l,0,MAX_DELAY*sizeof(float));
    if(sl->dsp.dl_r) memset(sl->dsp.dl_r,0,MAX_DELAY*sizeof(float));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  UNIQUENESS — Skip-walk Select  (design-spec §4)
 *  Each of the 24 effects may occupy ≤1 slot. Off is exempt. Selecting a taken
 *  effect skips over it to the next free one in the scroll direction (clamp ends).
 * ═══════════════════════════════════════════════════════════════════════════ */
static int effect_taken(palette_t *p, int fx, int except_slot){
    if(fx==PFX_OFF) return 0;
    for(int i=0;i<NUM_SLOTS;i++)
        if(i!=except_slot && p->slots[i].select==fx) return 1;
    return 0;
}
/* land on requested if free, else walk in `dir` (+1/−1) to next free or Off */
static int resolve_select(palette_t *p, int slot, int requested, int dir){
    requested = clampi(requested, PFX_OFF, NUM_FX);
    if(requested==PFX_OFF || !effect_taken(p,requested,slot)) return requested;
    if(dir==0) dir=1;
    for(int v=requested+dir; v>=PFX_OFF && v<=NUM_FX; v+=dir)
        if(v==PFX_OFF || !effect_taken(p,v,slot)) return v;
    /* nothing free in that direction → walk the other way */
    for(int v=requested-dir; v>=PFX_OFF && v<=NUM_FX; v-=dir)
        if(v==PFX_OFF || !effect_taken(p,v,slot)) return v;
    return PFX_OFF;
}
static void set_slot_select(palette_t *p, int slot, int requested, int dir){
    int landed = resolve_select(p,slot,requested,dir);
    if(landed != p->slots[slot].select){
        p->slots[slot].prev_select = p->slots[slot].select;
        p->slots[slot].select = landed;
        slot_reset(&p->slots[slot]);     /* clear buffers on effect change */
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  RANDOMIZERS  (design-spec §4) — all across the 4 slots at once.
 *  Effect picks are WITHOUT REPLACEMENT (distinct), bypassing the skip-walk.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void pick_distinct_effects(palette_t *p, uint32_t *rng){
    int pool[NUM_FX]; for(int i=0;i<NUM_FX;i++) pool[i]=i+1;   /* 1..24 */
    for(int i=NUM_FX-1;i>0;i--){ int j=rnd_int(rng,i+1); int t=pool[i];pool[i]=pool[j];pool[j]=t; }
    for(int s=0;s<NUM_SLOTS;s++){
        p->slots[s].prev_select=p->slots[s].select;
        p->slots[s].select=pool[s];
        slot_reset(&p->slots[s]);
    }
}
static void rnd_patch(palette_t *p){                 /* everything new */
    pick_distinct_effects(p,&p->rng);
    for(int s=0;s<NUM_SLOTS;s++){
        p->slots[s].amount=frand(&p->rng);
        p->slots[s].macro =frand(&p->rng);
        p->slots[s].drift =frand(&p->rng);
    }
}
static void rnd_effect(palette_t *p){ pick_distinct_effects(p,&p->rng); }   /* keep params */
static void rnd_amount(palette_t *p){ for(int s=0;s<NUM_SLOTS;s++) p->slots[s].amount=frand(&p->rng); }
static void rnd_macro (palette_t *p){ for(int s=0;s<NUM_SLOTS;s++) p->slots[s].macro =frand(&p->rng); }
static void rnd_drift (palette_t *p){ for(int s=0;s<NUM_SLOTS;s++) p->slots[s].drift =frand(&p->rng); }

/* ── Lifecycle ───────────────────────────────────────────────────────────────── */
static void *create_instance(const char *module_dir, const char *json){
    (void)module_dir;(void)json;
    palette_t *p = calloc(1,sizeof(palette_t));
    if(!p) return NULL;
    for(int s=0;s<NUM_SLOTS;s++){
        p->slots[s].select = PFX_OFF;          /* all slots default Off */
        p->slots[s].prev_select = PFX_OFF;
        p->slots[s].amount=0; p->slots[s].macro=0; p->slots[s].drift=0;
        p->slots[s].dsp.dl_l = calloc(MAX_DELAY,sizeof(float));
        p->slots[s].dsp.dl_r = calloc(MAX_DELAY,sizeof(float));
        p->slots[s].dsp.seed = 0x9e3779b9u + (uint32_t)s*0x6d2b79f5u + 1u;
    }
    p->input_vol = 1.0f;
    p->mix = 1.0f;                              /* design-spec: Mix default 100% */
    p->fx_reorder = 0;                          /* 1-2-3-4 */
    p->current_preset = 1;
    p->current_level = 0;                       /* Presets (landing) */
    p->rng = 0x12345678u;
    if(g_host && g_host->log) g_host->log("[palette] instance created");
    return p;
}
static void destroy_instance(void *instance){
    palette_t *p=(palette_t*)instance; if(!p) return;
    for(int s=0;s<NUM_SLOTS;s++){ free(p->slots[s].dsp.dl_l); free(p->slots[s].dsp.dl_r); }
    free(p);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PARAMS  (page-aware knob overlay + stable keys + chain_params)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Levels for page-aware knob overlay. Index 0 = root/Presets (landing). */
enum { LV_PRESETS=0, LV_PALETTE, LV_FX12, LV_FX34, NUM_LEVELS };
static const char *LEVEL_NAMES[NUM_LEVELS] = { "Presets","PALETTE","FX12","FX34" };

/* knob (1..8) → param key, per level. NULL = unused knob. */
static const char *LEVEL_KNOBS[NUM_LEVELS][8] = {
  /* Presets */ {"current_preset","rnd_patch","rnd_effect","rnd_amount","rnd_macro","rnd_drift","input_vol","mix"},
  /* PALETTE */ {"fx1_amount","fx1_macro","fx2_amount","fx2_macro","fx3_amount","fx3_macro","fx4_amount","fx4_macro"},
  /* FX12    */ {"fx1_select","fx1_amount","fx1_macro","fx1_drift","fx2_select","fx2_amount","fx2_macro","fx2_drift"},
  /* FX34    */ {"fx3_select","fx3_amount","fx3_macro","fx3_drift","fx4_select","fx4_amount","fx4_macro","fx4_drift"},
};

/* parse "fxN_field" → slot index 0..3 (returns -1 if not a slot key) */
static int slot_of(const char *key){ return (key[0]=='f'&&key[1]=='x'&&key[2]>='1'&&key[2]<='4')?(key[2]-'1'):-1; }

/* apply a float param by key (used by direct set + knob delta) */
static void set_float_key(palette_t *p, const char *key, float v){
    int s=slot_of(key);
    if(s>=0){
        const char *f=key+4;                                  /* after "fxN_" */
        if(!strcmp(f,"amount")) p->slots[s].amount=clampf(v,0,1);
        else if(!strcmp(f,"macro")) p->slots[s].macro=clampf(v,0,1);
        else if(!strcmp(f,"drift")) p->slots[s].drift=clampf(v,0,1);
        return;
    }
    if(!strcmp(key,"mix")) p->mix=clampf(v,0,1);
    else if(!strcmp(key,"input_vol")) p->input_vol=clampf(v,0,2);
}
static float get_float_key(palette_t *p, const char *key){
    int s=slot_of(key);
    if(s>=0){ const char *f=key+4;
        if(!strcmp(f,"amount")) return p->slots[s].amount;
        if(!strcmp(f,"macro"))  return p->slots[s].macro;
        if(!strcmp(f,"drift"))  return p->slots[s].drift; }
    if(!strcmp(key,"mix")) return p->mix;
    if(!strcmp(key,"input_vol")) return p->input_vol;
    return 0;
}

static void fire_trigger(palette_t *p, const char *key){
    if(!strcmp(key,"rnd_patch"))  rnd_patch(p);
    else if(!strcmp(key,"rnd_effect")) rnd_effect(p);
    else if(!strcmp(key,"rnd_amount")) rnd_amount(p);
    else if(!strcmp(key,"rnd_macro"))  rnd_macro(p);
    else if(!strcmp(key,"rnd_drift"))  rnd_drift(p);
}

/* knob delta dispatch (page-aware) */
static void apply_knob_delta(palette_t *p, const char *key, int delta){
    if(!key) return;
    int s=slot_of(key);
    /* selects: skip-walk in scroll direction */
    if(s>=0 && !strcmp(key+4,"select")){
        int dir = delta>=0?1:-1;
        int steps = abs(delta); if(steps<1) steps=1;
        for(int k=0;k<steps;k++)
            set_slot_select(p,s, p->slots[s].select+dir, dir);
        return;
    }
    if(!strcmp(key,"current_preset")){
        p->current_preset = clampi(p->current_preset+delta,1,25);
        /* TODO: load_preset(p, p->current_preset); */
        return;
    }
    if(!strncmp(key,"rnd_",4)){ if(delta!=0) fire_trigger(p,key); return; }
    /* floats */
    float step = (!strcmp(key,"input_vol")) ? 0.01f : 0.01f;
    set_float_key(p,key, get_float_key(p,key)+delta*step);
}

static void set_param(void *instance, const char *key, const char *val){
    palette_t *p=(palette_t*)instance; if(!p||!key||!val) return;

    /* active page for knob overlay */
    if(!strcmp(key,"_level")){
        for(int i=0;i<NUM_LEVELS;i++) if(!strcmp(val,LEVEL_NAMES[i])){ p->current_level=i; break; }
        return;
    }
    /* knob_N_adjust — resolve via current level's knob map */
    if(!strncmp(key,"knob_",5) && strstr(key,"_adjust")){
        int n=atoi(key+5)-1;
        if(n>=0 && n<8){ const char *pk=LEVEL_KNOBS[p->current_level][n];
                         if(pk) apply_knob_delta(p,pk,atoi(val)); }
        return;
    }
    /* direct selects (preset load / MIDI) — no skip-walk needed if value distinct,
     * but still pass through resolve to keep the invariant safe */
    {
        int s=slot_of(key);
        if(s>=0 && !strcmp(key+4,"select")){
            int req=PFX_OFF;
            for(int i=0;i<PFX_COUNT;i++) if(!strcmp(val,FX_NAMES[i])){req=i;break;}
            if(req==PFX_OFF && val[0]>='0'&&val[0]<='9') req=clampi(atoi(val),0,NUM_FX);
            set_slot_select(p,s,req,+1);
            return;
        }
    }
    /* triggers: wide-range int tap-to-fire (NOT enum["0","1"]) */
    if(!strncmp(key,"rnd_",4)){ if(atoi(val)!=0) fire_trigger(p,key); return; }

    if(!strcmp(key,"current_preset")){ p->current_preset=clampi(atoi(val),1,25);
        /* TODO load_preset */ return; }
    if(!strcmp(key,"fx_reorder")){
        /* accept "1-2-3-4" label or index */
        int idx=-1; for(int i=0;i<24;i++){ char lb[16]; perm_label(i,lb,sizeof lb);
            if(!strcmp(val,lb)){idx=i;break;} }
        if(idx<0) idx=clampi(atoi(val),0,23);
        p->fx_reorder=idx; return;
    }
    /* floats by stable key */
    set_float_key(p,key,atof(val));

    /* TODO: if(!strcmp(key,"state")) deserialize_all(p,val);  (preset save/restore) */
}

static int get_param(void *instance, const char *key, char *buf, int buf_len){
    palette_t *p=(palette_t*)instance; if(!p||!key||!buf||buf_len<1) return -1;

    if(!strcmp(key,"name")) return snprintf(buf,buf_len,"PALETTE");

    if(!strcmp(key,"chain_params")){
        /* ALL params from ALL pages. selects/reorder are enums (name strings);
         * rnd_* are int tap-to-fire; amount/macro/drift/mix float; input_vol float. */
        int o=0;
        o+=snprintf(buf+o,buf_len-o,"[");
        for(int s=1;s<=NUM_SLOTS;s++){
            o+=snprintf(buf+o,buf_len-o,
              "{\"key\":\"fx%d_select\",\"name\":\"FX%d\",\"type\":\"enum\",\"options\":[", s,s);
            for(int i=0;i<PFX_COUNT;i++) o+=snprintf(buf+o,buf_len-o,"%s\"%s\"",i?",":"",FX_NAMES[i]);
            o+=snprintf(buf+o,buf_len-o,"]},");
            o+=snprintf(buf+o,buf_len-o,
              "{\"key\":\"fx%d_amount\",\"name\":\"FX%d Amt\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
              "{\"key\":\"fx%d_macro\",\"name\":\"FX%d Macro\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
              "{\"key\":\"fx%d_drift\",\"name\":\"FX%d Drift\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},",
              s,s,s,s,s,s);
        }
        o+=snprintf(buf+o,buf_len-o,
          "{\"key\":\"input_vol\",\"name\":\"Input Vol\",\"type\":\"float\",\"min\":0,\"max\":2,\"step\":0.01},"
          "{\"key\":\"mix\",\"name\":\"Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"current_preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":1,\"max\":25,\"step\":1},"
          "{\"key\":\"rnd_patch\",\"name\":\"Rnd Patch\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
          "{\"key\":\"rnd_effect\",\"name\":\"Rnd FX\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
          "{\"key\":\"rnd_amount\",\"name\":\"Rnd Amt\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
          "{\"key\":\"rnd_macro\",\"name\":\"Rnd Macro\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
          "{\"key\":\"rnd_drift\",\"name\":\"Rnd Drift\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1}");
        o+=snprintf(buf+o,buf_len-o,"]");
        return o;
    }

    /* slot params */
    int s=slot_of(key);
    if(s>=0){ const char *f=key+4;
        if(!strcmp(f,"select")) return snprintf(buf,buf_len,"%s",FX_NAMES[clampi(p->slots[s].select,0,NUM_FX)]);
        if(!strcmp(f,"amount")) return snprintf(buf,buf_len,"%.2f",p->slots[s].amount);
        if(!strcmp(f,"macro"))  return snprintf(buf,buf_len,"%.2f",p->slots[s].macro);
        if(!strcmp(f,"drift"))  return snprintf(buf,buf_len,"%.2f",p->slots[s].drift);
    }
    if(!strcmp(key,"mix"))           return snprintf(buf,buf_len,"%.2f",p->mix);
    if(!strcmp(key,"input_vol"))     return snprintf(buf,buf_len,"%.2f",p->input_vol);
    if(!strcmp(key,"current_preset"))return snprintf(buf,buf_len,"%d",p->current_preset);
    if(!strcmp(key,"fx_reorder")){ char lb[16]; perm_label(p->fx_reorder,lb,sizeof lb);
                                   return snprintf(buf,buf_len,"%s",lb); }
    if(!strncmp(key,"rnd_",4))       return snprintf(buf,buf_len,"0");   /* triggers read 0 */

    /* knob_N_name / knob_N_value — page-aware via current level */
    if(!strncmp(key,"knob_",5) && (strstr(key,"_name")||strstr(key,"_value"))){
        int n=atoi(key+5)-1; if(n<0||n>=8) return -1;
        const char *pk=LEVEL_KNOBS[p->current_level][n]; if(!pk) return -1;
        if(strstr(key,"_name")){
            /* friendly labels */
            if(slot_of(pk)>=0){ const char*f=pk+4; int sl=slot_of(pk)+1;
                if(!strcmp(f,"select")) return snprintf(buf,buf_len,"FX%d",sl);
                if(!strcmp(f,"amount")) return snprintf(buf,buf_len,"FX%d Amt",sl);
                if(!strcmp(f,"macro"))  return snprintf(buf,buf_len,"FX%d Macro",sl);
                if(!strcmp(f,"drift"))  return snprintf(buf,buf_len,"FX%d Drift",sl);
            }
            if(!strcmp(pk,"current_preset")) return snprintf(buf,buf_len,"Preset");
            if(!strcmp(pk,"input_vol"))      return snprintf(buf,buf_len,"Input Vol");
            if(!strcmp(pk,"mix"))            return snprintf(buf,buf_len,"Mix");
            if(!strcmp(pk,"rnd_patch"))      return snprintf(buf,buf_len,"Rnd Patch");
            if(!strcmp(pk,"rnd_effect"))     return snprintf(buf,buf_len,"Rnd FX");
            if(!strcmp(pk,"rnd_amount"))     return snprintf(buf,buf_len,"Rnd Amt");
            if(!strcmp(pk,"rnd_macro"))      return snprintf(buf,buf_len,"Rnd Macro");
            if(!strcmp(pk,"rnd_drift"))      return snprintf(buf,buf_len,"Rnd Drift");
            return snprintf(buf,buf_len,"%s",pk);
        } else { /* _value */
            if(slot_of(pk)>=0 && !strcmp(pk+4,"select"))
                return snprintf(buf,buf_len,"%s",FX_NAMES[clampi(p->slots[slot_of(pk)].select,0,NUM_FX)]);
            if(!strcmp(pk,"current_preset")) return snprintf(buf,buf_len,"%d",p->current_preset);
            if(!strncmp(pk,"rnd_",4))        return snprintf(buf,buf_len,"tap");
            return snprintf(buf,buf_len,"%d%%",(int)(get_float_key(p,pk)*100));
        }
    }

    /* TODO: if(!strcmp(key,"state")) return serialize_all(p,buf,buf_len); */

    return -1;   /* unknown key MUST be -1, never 0 */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  AUDIO  — slots processed in FX-Reorder order; equal-power global Mix.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void process_block(void *instance, int16_t *buf, int frames){
    palette_t *p=(palette_t*)instance; if(!p||frames<=0) return;
    if(frames>MAXFRAMES) frames=MAXFRAMES;

    const uint8_t *order = PERM[clampi(p->fx_reorder,0,23)];
    float iv = p->input_vol;

    /* de-interleave + input volume (dry kept for mix) */
    float dryL[MAXFRAMES], dryR[MAXFRAMES];
    for(int i=0;i<frames;i++){
        float l=buf[2*i]/32768.0f, r=buf[2*i+1]/32768.0f;
        dryL[i]=l; dryR[i]=r;
        p->L[i]=l*iv; p->R[i]=r*iv;
    }
    /* run the 4 slots in reorder sequence (Off = passthrough, never dispatched) */
    for(int k=0;k<NUM_SLOTS;k++){
        slot_t *sl=&p->slots[order[k]];
        if(sl->select==PFX_OFF) continue;
        FX_TABLE[sl->select].process(&sl->dsp, p->L, p->R, frames,
                                     sl->amount, sl->macro, sl->drift);
    }
    /* equal-power dry/wet + write back */
    float a=p->mix*1.5707963f, dg=cosf(a), wg=sinf(a);
    for(int i=0;i<frames;i++){
        float ol=dg*dryL[i]+wg*p->L[i];
        float orr=dg*dryR[i]+wg*p->R[i];
        int32_t il=(int32_t)(ol*32767.0f), ir=(int32_t)(orr*32767.0f);
        if(il>32767)il=32767; if(il<-32768)il=-32768;
        if(ir>32767)ir=32767; if(ir<-32768)ir=-32768;
        buf[2*i]=(int16_t)il; buf[2*i+1]=(int16_t)ir;
    }
}

/* ── API v2 export ───────────────────────────────────────────────────────────── */
static audio_fx_api_v2_t g_api = {
    .api_version=2, .create_instance=create_instance, .destroy_instance=destroy_instance,
    .process_block=process_block, .set_param=set_param, .get_param=get_param, .on_midi=NULL,
};
__attribute__((visibility("default")))
audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host){
    g_host=host; if(host&&host->log) host->log("[palette] loaded"); return &g_api;
}
