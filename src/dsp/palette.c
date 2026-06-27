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
    /* Character (group 0) */ PFX_DRIVE, PFX_SWEETEN, PFX_FUZZ, PFX_HOWL, PFX_FOLD, PFX_SWELL,
    /* Movement  (group 1) */ PFX_DOUBLER, PFX_VIBRATO, PFX_PHASER, PFX_TREMOLO, PFX_PITCH, PFX_SHIFT,
    /* Diffusion (group 2) */ PFX_CASCADE, PFX_REELS, PFX_COLLAGE, PFX_REVERSE, PFX_SPACE, PFX_BLOOM,
    /* Texture   (group 3) */ PFX_FILTER, PFX_SQUASH, PFX_CASSETTE, PFX_BROKEN, PFX_INTERFERENCE, PFX_FREEZE,
    PFX_COUNT               /* = 25 (Off + 24) */
};
#define NUM_FX (PFX_COUNT - 1)   /* 24 selectable effects */

/* Display names — index 0..24. get_param for selects MUST return these strings. */
static const char *FX_NAMES[PFX_COUNT] = {
    "Off",
    "Drive","Sweeten","Fuzz","Howl","Fold","Swell",
    "Doubler","Vibrato","Phaser","Tremolo","Pitch","Shift",
    "Cascade","Reels","Collage","Reverse","Space","Bloom",
    "Filter","Squash","Cassette","Broken","Interference","Freeze"
};
static const uint8_t FX_GROUP[PFX_COUNT] = {
    0, 0,0,0,0,0,0, 1,1,1,1,1,1, 2,2,2,2,2,2, 3,3,3,3,3,3
};

/* ── Clouds C++ effects (fx_clouds.cc) — SPACE / BLOOM / FREEZE ───────────────
 * Fully isolated behind 3 opaque calls; palette.c never sees Clouds internals.
 * alloc returns heavy state (or NULL on OOM), freed on switch/destroy. */
extern void *pfx_clouds_alloc(int fx_id, float sr);
extern void  pfx_clouds_free(void *heavy);
extern void  pfx_clouds_process(int fx_id, void *heavy, float *l, float *r, int n,
                                float amount, float macro, float drift);

/* ── Per-slot DSP scratch (shared across the C effects; each uses a subset) ───
 * Allocated once in create_instance, never in process_block. Generous so every
 * C effect finds the state it needs; cleared in slot_reset() on effect switch. */
typedef struct {
    /* delay line (Cascade/Reels/Reverse/Collage/Doubler/Vibrato/Pitch/Broken) */
    float *dl_l, *dl_r;       /* MAX_DELAY each */
    int    wp;                /* write pointer */
    /* generic one-pole / cascaded filter states (stereo) */
    float  z1l,z1r, z2l,z2r, z3l,z3r, z4l,z4r;
    /* state-variable filter (Filter / Howl) stereo: lp & bp integrators */
    float  lp_l,bp_l, lp_r,bp_r;
    /* biquad (peaking/shelf) stereo, direct-form I */
    float  bx1l,bx2l,by1l,by2l, bx1r,bx2r,by1r,by2r;
    /* envelope followers */
    float  env, env2;
    /* parameter smoothing companions */
    float  sm1,sm2,sm3,sm4;
    /* LFO phases (0..1) */
    float  lfo,lfo2,lfo3;
    /* allpass chains: Phaser (≤12 stages, 1 state each) stereo */
    float  ap_l[16], ap_r[16], ap2_l[16], ap2_r[16];
    /* SHIFT: Warps QuadratureTransform Hilbert — 17 allpass filters × 2 states,
     * per channel: hil[ch*34 + filt*2 + (0=x,1=y)] */
    float  hil[68];
    /* misc per-effect scratch */
    float  f1,f2,f3,f4,f5,f6;
    int    i1,i2,i3,i4;
    uint32_t seed;            /* per-slot deterministic RNG (drift) */
    /* heavy C++ (Clouds) effect state — lazily alloc'd on select change */
    void  *heavy;
    int    heavy_kind;        /* PFX_* id the heavy ptr currently serves (0 = none) */
} slot_dsp_t;

typedef struct {
    int   select;             /* PFX_* id (0 = Off) */
    int   prev_select;        /* for Skip-walk direction inference */
    float amount, macro, drift;       /* knob targets */
    float amt_sm, mac_sm, drf_sm;     /* 20 ms analog-style smoothed values (fed to DSP) */
    float ramp;               /* 0..1 click-free fade-in after an effect switch */
    slot_dsp_t dsp;
} slot_t;

/* ── Effect process signature ────────────────────────────────────────────────
 * Stereo, n frames, in-place on l[]/r[] (float, ±1). amount/macro/drift ∈ [0,1].
 * OFF and Clouds effects are dispatched specially by the host (never here). */
typedef void (*fx_process_fn)(slot_dsp_t*, float*, float*, int,
                              float amount, float macro, float drift);
typedef void (*fx_reset_fn)(slot_dsp_t*);

typedef struct {
    fx_process_fn process;
    fx_reset_fn   reset;      /* may be NULL */
} palette_effect_t;

/* heavy (Clouds-backed) effects need lazy alloc + special dispatch */
static inline int is_clouds_fx(int id){
    return id==PFX_SPACE || id==PFX_BLOOM || id==PFX_FREEZE;
}

/* ── Instance ────────────────────────────────────────────────────────────────── */
typedef struct {
    slot_t slots[NUM_SLOTS];
    float  input_vol;         /* 0..2, unity = 1 */
    float  mix;               /* 0..1 global dry/wet */
    float  iv_sm, mix_sm;     /* 20 ms smoothed globals */
    int    fx_reorder;        /* 0..23 permutation index (0 = 1-2-3-4) */
    int    current_preset;    /* 1..50 */
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
 *  EFFECT IMPLEMENTATIONS  (21 pure-C effects; SPACE/BLOOM/FREEZE are in
 *  fx_clouds.cc). Signature: in-place stereo float ±1, n frames.
 *    amount = primary knob   macro = secondary (Chroma Tilt/Rate/Time role)
 *    drift  = instability macro — "musical wander", tasteful at every position.
 *  Sibling-sourced voicings (super-boom / mello / krautdrums) re-voiced inline.
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef TWO_PI
#define TWO_PI 6.28318530718f
#endif
#define DENORM 1e-20f
static inline float lerpf(float a,float b,float t){ return a+(b-a)*t; }
/* fractional delay read, `d` samples behind the next write index `wp` */
static inline float dlr(const float *b,int wp,float d){
    float rp=(float)wp-d;
    while(rp<0.0f) rp+=(float)MAX_DELAY;
    while(rp>=(float)MAX_DELAY) rp-=(float)MAX_DELAY;
    int i0=(int)rp; float fr=rp-(float)i0; int i1=i0+1; if(i1>=MAX_DELAY) i1=0;
    return b[i0]+(b[i1]-b[i0])*fr;
}
/* slow musical random-walk in [-1,1] stored in *st (drift LFO) */
static inline float wander(float *st,uint32_t *seed,float speed){
    *st += speed*((frand(seed)-0.5f)*2.0f - *st);
    return *st;
}

/* ── Sibling kernels (Filliformes, MIT) — extracted verbatim from shipped plugins ──
 * sb_tanh: super-boom-move Padé tanh.  delay_saturate: krautdrums-move feedback
 * saturator w/ DC removal.  tape_cubic/tape_asym: mello-move apply_tape_stage. */
static inline float sb_tanh(float x){                       /* super-boom-move */
    if(x>3.0f) return 1.0f; if(x<-3.0f) return -1.0f;
    float x2=x*x; return x*(27.0f+x2)/(27.0f+9.0f*x2);
}
static inline float sb_apply_dist(float x,int mode){        /* super-boom apply_dist */
    switch(mode){
        case 0: return sinf(clampf(x,-1.57f,1.57f));            /* Boost */
        case 1: return (x>0.0f)? sb_tanh(x) : (x*0.55f);       /* Tube  */
        case 2: return clampf(x*1.4f,-0.85f,0.85f);            /* Fuzz  */
        default: return x;
    }
}
static inline float delay_saturate(float x,float drive,float asym){ /* krautdrums-move */
    float clipped=sb_tanh(x*drive+asym);
    return (clipped - sb_tanh(asym))/drive;
}
static inline float tape_cubic(float x,float drive){        /* mello-move */
    float y=x*drive; if(y>1.5f) return 1.0f; if(y<-1.5f) return -1.0f;
    return y - y*y*y*(1.0f/6.75f);
}
static inline float tape_asym(float x,float drive,float asym){ /* mello-move */
    return sb_tanh(x*drive+asym)-sb_tanh(asym);
}

static void fx_passthrough(slot_dsp_t *s, float *l, float *r, int n,
                           float amount, float macro, float drift){
    (void)s;(void)l;(void)r;(void)n;(void)amount;(void)macro;(void)drift;
}

/* ── CHARACTER ─────────────────────────────────────────────────────────────── */

/* DRIVE — tube-ish overdrive. Uses super-boom-move sb_apply_dist Tube voicing
 * (asymmetric Padé tanh). amount=drive, macro=tone tilt, drift=slow bias wander. */
static void fx_drive(slot_dsp_t *s, float *l, float *r, int n,
                     float amount, float macro, float drift){
    float g    = 1.0f + amount*amount*23.0f;        /* quadratic: fine low end */
    float tilt = (macro-0.5f)*2.0f;                 /* −1..+1 */
    float comp = 0.85f/(0.3f+0.7f*g);               /* level compensation */
    float tc   = 0.18f;                              /* tilt one-pole coeff */
    for(int i=0;i<n;i++){
        float bias = drift>0.0f ? wander(&s->f1,&s->seed,0.0006f)*drift*0.12f : 0.0f;
        float xl=sb_apply_dist(l[i]*g+bias,1), xr=sb_apply_dist(r[i]*g+bias,1); /* Tube */
        s->z1l += tc*(xl-s->z1l)+DENORM; s->z1r += tc*(xr-s->z1r)+DENORM;
        float ll = (tilt>=0.0f)? lerpf(xl,(xl-s->z1l)*2.0f,tilt) : lerpf(xl,s->z1l*1.6f,-tilt);
        float rr = (tilt>=0.0f)? lerpf(xr,(xr-s->z1r)*2.0f,tilt) : lerpf(xr,s->z1r*1.6f,-tilt);
        l[i]=ll*comp; r[i]=rr*comp;
    }
}

/* SWEETEN — console preamp. Gentle comp + the Airwindows Density sin-fold
 * saturator (Chris Johnson, MIT) for body, + an Air-style HF tilt shelf.
 * amount=comp/sat, macro=tone (dark↔air), drift=subtle level drift. */
static void fx_sweeten(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float tilt=(macro-0.5f)*2.0f;
    float thr=0.5f, ratio=0.35f+amount*0.4f;         /* gentle comp */
    float density=amount*1.6f, dwet=clampf(density,0.0f,1.0f);  /* Density sat amount */
    float comp=1.0f/(1.0f+density*0.5f);             /* compensate sin-fold gain */
    for(int i=0;i<n;i++){
        float mono=0.5f*(fabsf(l[i])+fabsf(r[i]));
        s->env += (mono>s->env?0.02f:0.004f)*(mono-s->env)+DENORM;  /* fast atk slow rel */
        float gr=1.0f; if(s->env>thr) gr=1.0f-(1.0f-thr/s->env)*ratio;
        float dl=(drift>0.0f)? 1.0f+wander(&s->f1,&s->seed,0.0004f)*drift*0.03f : 1.0f;
        for(int ch=0;ch<2;ch++){
            float dry=(ch?r:l)[i];
            float x=dry*gr*dl;
            /* Airwindows Density sin-fold saturator (level-compensated) */
            float br=fabsf(x*(1.0f+density)); if(br>1.57079633f)br=1.57079633f; br=sinf(br);
            float sat=((x>0.0f)? x*(1.0f-dwet)+br*dwet : x*(1.0f-dwet)-br*dwet)*comp;
            /* Air-style HF tilt shelf (macro) */
            float *z=ch?&s->z1r:&s->z1l;
            *z += 0.10f*(sat-*z)+DENORM;              /* LP component */
            float wet=(tilt>=0.0f)? sat+(sat-*z)*tilt*0.8f      /* brighten = air */
                                  : lerpf(sat,*z,-tilt);        /* darken */
            (ch?r:l)[i]=lerpf(dry,wet,amount);        /* amount=0 → dry */
        }
    }
}

/* FUZZ — high-gain asymmetric clip with bias/gate. amount=intensity,
 * macro=tone/bias, drift=bias instability. */
static void fx_fuzz(slot_dsp_t *s, float *l, float *r, int n,
                    float amount, float macro, float drift){
    float g=1.0f+amount*amount*80.0f;
    float bias=(macro-0.5f)*0.7f;                    /* macro shifts symmetry */
    float tonec=0.08f+macro*0.5f;                    /* and post tone */
    for(int i=0;i<n;i++){
        float b=bias + (drift>0.0f? wander(&s->f1,&s->seed,0.002f)*drift*0.4f:0.0f);
        for(int ch=0;ch<2;ch++){
            float x=(ch?r:l)[i]*g + b;
            float y=sb_apply_dist(x,2) - sb_apply_dist(b,2);  /* super-boom Fuzz, DC-removed */
            float *z=ch?&s->z1r:&s->z1l;
            *z += tonec*(y-*z)+DENORM;
            (ch?r:l)[i]=*z*0.8f;
        }
    }
}

/* HOWL — resonant filter-fuzz / synth stab. Cytomic/TPT state-variable filter
 * (Andrew Simper, topology-preserving — unconditionally stable at high freq/Q,
 * unlike the Chamberlin SVF). Bandpass voiced into soft clip. amount=intensity
 * (drive + resonance), macro=resonant freq, drift=res-freq wander.
 * State per ch: lp_*=ic1eq, bp_*=ic2eq. */
static void fx_howl(slot_dsp_t *s, float *l, float *r, int n,
                    float amount, float macro, float drift){
    float base=120.0f*powf(40.0f,macro);             /* 120..4800 Hz */
    float fz=base*(drift>0.0f?(1.0f+wander(&s->f1,&s->seed,0.0008f)*drift*0.5f):1.0f);
    float g=tanf(3.14159265f*clampf(fz,40.0f,9000.0f)/SR);
    float k=1.0f-amount*0.93f;                        /* damping (1/Q) → high res near 0 */
    float a1=1.0f/(1.0f+g*(g+k)), a2=g*a1, a3=g*a2;
    float drive=1.0f+amount*amount*12.0f;
    float voice=0.8f+amount;
    for(int ch=0;ch<2;ch++){
        float *ic1=ch?&s->lp_r:&s->lp_l, *ic2=ch?&s->bp_r:&s->bp_l;
        float *src=ch?r:l;
        for(int i=0;i<n;i++){
            float x=tanhf(src[i]*drive);
            float v3=x-*ic2;
            float v1=a1*(*ic1)+a2*v3;
            float v2=*ic2+a2*(*ic1)+a3*v3;
            *ic1=2.0f*v1-*ic1+DENORM; *ic2=2.0f*v2-*ic2+DENORM;
            src[i]=tanhf(v1*voice)*0.9f;              /* v1 = bandpass */
        }
    }
}

/* SWELL — auto volume-swell. Hysteresis gate + Zeno-pole envelopes from Airwindows
 * Swell (Chris Johnson, MIT): separate on/off thresholds (no chatter) and separate
 * attack/release "Zeno arrow" poles. amount=swell time, macro=sensitivity,
 * drift=threshold jitter. State: i1/i2=louder L/R, sm1/sm2=swell gain L/R. */
static void fx_swell(slot_dsp_t *s, float *l, float *r, int n,
                     float amount, float macro, float drift){
    float sens=0.02f+(1.0f-macro)*0.28f;             /* on-threshold */
    float speedOn =0.00015f+(1.0f-amount)*0.0028f;   /* longer swell = slower rise */
    float speedOff=0.0008f;                           /* gentle fade-out */
    for(int i=0;i<n;i++){
        float thOn=sens*(drift>0.0f?(1.0f+wander(&s->f1,&s->seed,0.003f)*drift*0.5f):1.0f);
        float thOff=thOn*0.5f;                        /* hysteresis: off < on */
        for(int ch=0;ch<2;ch++){
            float x=(ch?r:l)[i]; float a=fabsf(x);
            int *louder=ch?&s->i2:&s->i1; float *sw=ch?&s->sm2:&s->sm1;
            if(a>thOn && !*louder) *louder=1;
            if(a<thOff && *louder) *louder=0;
            if(*louder) *sw = *sw*(1.0f-speedOn)+speedOn;   /* Zeno → 1 */
            else      { *sw = *sw*(1.0f-speedOff); if(*sw<1e-15f)*sw=0.0f; } /* Zeno → 0, denormal floor */
            (ch?r:l)[i]=x*(*sw);
        }
    }
}

/* Authentic Mutable Warps tables (warps_data.c, MIT) — see SOURCES. */
extern const float lut_bipolar_fold[];   /* 4096-pt, centred at +2048, range ±0.87 */
extern const float lut_ap_poles[];       /* 17 Hilbert allpass poles (SHIFT) */

/* FOLD — West-Coast wavefolder using the REAL Warps lut_bipolar_fold curve via
 * the ALGORITHM_FOLD interpolation. amount=fold drive, macro=offset/symmetry,
 * drift=fold-point wobble. */
static void fx_fold(slot_dsp_t *s, float *l, float *r, int n,
                    float amount, float macro, float drift){
    const float kScale=2048.0f/2.295f;               /* Warps: 2048/((2+0.25)*1.02) */
    float drive=0.5f+amount*amount*4.0f;
    float off=(macro-0.5f)*1.6f;
    float comp=0.55f/(0.6f+0.4f*drive);              /* level compensation — folding is loud */
    for(int i=0;i<n;i++){
        float w=(drift>0.0f? wander(&s->f1,&s->seed,0.0009f)*drift*0.25f:0.0f);
        for(int ch=0;ch<2;ch++){
            float dry=(ch?r:l)[i];
            float x=dry*drive + off + w;
            float idx=x*kScale + 2048.0f;             /* index into lut_bipolar_fold */
            int i0=(int)idx; if(i0<1)i0=1; else if(i0>4094)i0=4094;
            float fr=idx-(float)i0; if(fr<0.0f)fr=0.0f; else if(fr>1.0f)fr=1.0f;
            float y=lut_bipolar_fold[i0]+(lut_bipolar_fold[i0+1]-lut_bipolar_fold[i0])*fr;
            float *z=ch?&s->z1r:&s->z1l;
            *z += 0.5f*(y*comp-*z)+DENORM;             /* mild LP smooth, level-matched */
            (ch?r:l)[i]=lerpf(dry,*z,amount);          /* amount=0 → dry */
        }
    }
}

/* ── MOVEMENT ──────────────────────────────────────────────────────────────── */

/* DOUBLER — ADT stereo double-track / slapback. amount=mix, macro=time,
 * drift=random momentary detune. */
static void fx_doubler(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float baseL=SR*(0.012f+macro*0.045f);            /* 12..57 ms */
    float baseR=baseL*1.28f;
    for(int i=0;i<n;i++){
        s->lfo+=0.6f/SR; if(s->lfo>=1.0f)s->lfo-=1.0f;
        s->lfo2+=0.43f/SR; if(s->lfo2>=1.0f)s->lfo2-=1.0f;
        float wob=(drift>0.0f? wander(&s->f1,&s->seed,0.004f)*drift:0.0f);
        float dL=baseL*(1.0f+0.004f*sinf(s->lfo*TWO_PI)+0.01f*wob);
        float dR=baseR*(1.0f+0.004f*sinf(s->lfo2*TWO_PI)-0.01f*wob);
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        float eL=dlr(s->dl_l,s->wp,dL), eR=dlr(s->dl_r,s->wp,dR);
        s->wp=(s->wp+1)%MAX_DELAY;
        l[i]=lerpf(l[i],l[i]*0.7f+eL,amount);
        r[i]=lerpf(r[i],r[i]*0.7f+eR,amount);
    }
}

/* VIBRATO — fully-stereo modulated delay. L and R get the SAME LFO 90° apart
 * (true stereo vibrato/widening). 2nd LFO through-zero-FMs the sweep (Airwindows
 * character) + HF restore for interpolation dulling. amount=depth, macro=rate,
 * drift=sine→random waveshape + FM depth. lfo=main phase, lfo2=FM phase. */
static void fx_vibrato(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float rate=0.5f+macro*7.5f;                      /* 0.5..8 Hz */
    float depth=SR*0.0005f*(0.3f+amount*4.0f);       /* mod depth in samples */
    float base=SR*0.006f;
    float fmRate=rate*1.61f;                          /* incommensurate 2nd LFO */
    float fmDepth=0.4f+drift*0.6f;                    /* through-zero FM amount */
    for(int i=0;i<n;i++){
        s->lfo2+=fmRate/SR; if(s->lfo2>=1.0f)s->lfo2-=1.0f;
        s->lfo+=(rate*(1.0f+fmDepth*0.5f*sinf(s->lfo2*TWO_PI)))/SR;  /* through-zero FM */
        if(s->lfo>=1.0f)s->lfo-=1.0f; if(s->lfo<0.0f)s->lfo+=1.0f;
        float phR=s->lfo+0.25f; if(phR>=1.0f)phR-=1.0f;   /* R 90° offset = stereo */
        float rndL=(drift>0.0f? wander(&s->f1,&s->seed,0.02f):0.0f);
        float rndR=(drift>0.0f? wander(&s->f3,&s->seed,0.02f):0.0f);
        float modL=lerpf(sinf(s->lfo*TWO_PI),rndL,clampf(drift,0.0f,0.9f));
        float modR=lerpf(sinf(phR*TWO_PI),    rndR,clampf(drift,0.0f,0.9f));
        float dL=base+depth*(1.0f+modL), dR=base+depth*(1.0f+modR);
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        float oL=dlr(s->dl_l,s->wp,dL), oR=dlr(s->dl_r,s->wp,dR);
        /* HF restore: high-shelf the interpolation loss back in */
        s->z1l+=0.4f*(oL-s->z1l)+DENORM; oL+=(oL-s->z1l)*0.25f;
        s->z1r+=0.4f*(oR-s->z1r)+DENORM; oR+=(oR-s->z1r)*0.25f;
        l[i]=oL; r[i]=oR;
        s->wp=(s->wp+1)%MAX_DELAY;
    }
}

/* PHASER — N-stage allpass cascade + feedback. Frequency-accurate: the LFO sweeps
 * the notch frequency in Hz and the first-order allpass coefficient is derived as
 * a=(g-1)/(g+1), g=tan(π·fc/SR) — so notches sit at musical frequencies (Surge-style)
 * instead of a raw 0–1 coefficient. amount=intensity/stages, macro=rate,
 * drift=sweep randomness. */
static void fx_phaser(slot_dsp_t *s, float *l, float *r, int n,
                      float amount, float macro, float drift){
    int stages=2+(int)(amount*10.0f); if(stages>12)stages=12;  /* 2..12 */
    float rate=0.05f+macro*macro*4.0f;
    float fb=amount*0.6f;
    for(int i=0;i<n;i++){
        s->lfo+=rate/SR; if(s->lfo>=1.0f)s->lfo-=1.0f;
        float rnd=(drift>0.0f? wander(&s->f2,&s->seed,0.01f)*drift*0.4f:0.0f);
        float sweep=0.5f+0.5f*sinf(s->lfo*TWO_PI)+rnd;
        float fc=200.0f*powf(16.0f,clampf(sweep,0.0f,1.0f));    /* 200 Hz .. 3.2 kHz */
        float g=tanf(3.14159265f*clampf(fc,30.0f,12000.0f)/SR);
        float coef=(g-1.0f)/(g+1.0f);                           /* allpass coefficient */
        for(int ch=0;ch<2;ch++){
            float *ap=ch?s->ap_r:s->ap_l; float *fbz=ch?&s->z2r:&s->z2l;
            float x=(ch?r:l)[i]+ *fbz*fb;
            for(int k=0;k<stages;k++){
                float y=coef*x+ap[k];                 /* 1st-order allpass */
                ap[k]=x-coef*y; x=y;
            }
            *fbz=x;
            (ch?r:l)[i]=lerpf((ch?r:l)[i],0.5f*((ch?r:l)[i]+x),clampf(0.3f+amount,0.0f,1.0f));
        }
    }
}

/* TREMOLO — Airwindows Tremolo (Chris Johnson, MIT): chase-smoothed skew/density
 * waveshaping. amount=depth, macro=rate. DRIFT = AutoPan: morphs L/R from in-phase
 * (mono tremolo) to anti-phase (the channels duck oppositely → the modulation pans
 * across the stereo field). State: sm1/sm2=chased speed/depth, f4/f5=last targets. */
static void fx_tremolo(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float speedChase=macro*macro*macro*macro;          /* A^4 (Airwindows) */
    float depthChase=clampf(amount,0.0f,1.0f);
    float speedSpeed=300.0f/(fabsf(s->f4-speedChase)+1.0f);
    float depthSpeed=300.0f/(fabsf(s->f5-depthChase)+1.0f);
    s->f4=speedChase; s->f5=depthChase;
    float pan=clampf(drift,0.0f,1.0f);
    for(int i=0;i<n;i++){
        s->sm1=((s->sm1*speedSpeed)+speedChase)/(speedSpeed+1.0f);
        s->sm2=((s->sm2*depthSpeed)+depthChase)/(depthSpeed+1.0f);
        float speed=0.0001f+(s->sm1/1000.0f);
        float skew=1.0f+powf(s->sm2,9.0f);
        float density=((1.0f-s->sm2)*2.0f)-1.0f;
        float offset=sinf(s->lfo);
        s->lfo+=speed; if(s->lfo>TWO_PI)s->lfo-=TWO_PI;
        float control=fabsf(offset);
        if(density>0.0f) control=control*(1.0f-density)+sinf(control)*density;
        else             control=control*(1.0f+density)+(1.0f-cosf(control))*(-density);
        float ctl[2]={control, lerpf(control,1.0f-control,pan)};  /* R anti-phase at drift=1 */
        for(int ch=0;ch<2;ch++){
            float thickness=((ctl[ch]*2.0f)-1.0f)*skew;
            float out=fabsf(thickness);
            float x=(ch?r:l)[i];
            float br=fabsf(x); if(br>1.57079633f)br=1.57079633f;
            br=(thickness>0.0f)? sinf(br) : (1.0f-cosf(br));
            (ch?r:l)[i]=(x>0.0f)? x*(1.0f-out)+br*out : x*(1.0f-out)-br*out;
        }
    }
}

/* PITCH — delay-line pitch shifter ±1 oct + lo-fi grit. amount=wet mix,
 * macro=pitch, drift=resolution drop. Dual-tap crossfade window. */
static void fx_pitch(slot_dsp_t *s, float *l, float *r, int n,
                     float amount, float macro, float drift){
    float ratio=powf(2.0f,(macro-0.5f)*2.0f);        /* 0.5 .. 2.0 */
    float win=SR*0.040f;                              /* 40 ms window */
    float drate=(1.0f-ratio);                         /* read-ptr drift / sample */
    /* drift = gentle resolution drop: 9 bits (subtle) → 4 bits (gritty), blended in */
    float q=(drift>0.0f)? powf(2.0f, 9.0f-drift*5.0f) : 0.0f;
    for(int i=0;i<n;i++){
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        s->f1+=drate; if(s->f1<0)s->f1+=win; if(s->f1>=win)s->f1-=win;
        float p2=s->f1+win*0.5f; if(p2>=win)p2-=win;
        float w1=0.5f-0.5f*cosf(TWO_PI*s->f1/win);    /* triangular/raised-cos xfade */
        float w2=1.0f-w1;
        float oL=dlr(s->dl_l,s->wp,win-s->f1)*w1 + dlr(s->dl_l,s->wp,win-p2)*w2;
        float oR=dlr(s->dl_r,s->wp,win-s->f1)*w1 + dlr(s->dl_r,s->wp,win-p2)*w2;
        s->wp=(s->wp+1)%MAX_DELAY;
        if(q>0.0f){ /* round (no DC bias), blend by drift so low drift stays subtle */
            oL=lerpf(oL, floorf(oL*q+0.5f)/q, drift);
            oR=lerpf(oR, floorf(oR*q+0.5f)/q, drift);
        }
        l[i]=lerpf(l[i],oL,amount); r[i]=lerpf(r[i],oR,amount);
    }
}

/* SHIFT — Bode single-sideband frequency shifter using the REAL Mutable Warps
 * QuadratureTransform: a 17-stage first-order allpass network (lut_ap_poles) that
 * produces a broadband 90° quadrature pair, multiplied by a complex carrier.
 * amount=wet mix, macro=shift Hz (±), drift=shift wander.
 * State per channel: s->hil[ch*34 + filt*2 + {0:x,1:y}] (Warps AllPassFilter). */
static inline float warps_ap(float *st, float in, float coef){ /* y=coef*(in-y_)+x_ */
    float y = coef*(in - st[1]) + st[0];
    st[0]=in; st[1]=y; return y;
}
static void fx_shift(slot_dsp_t *s, float *l, float *r, int n,
                     float amount, float macro, float drift){
    float shift=(macro-0.5f)*1000.0f;                /* −500..+500 Hz */
    for(int i=0;i<n;i++){
        float w=(drift>0.0f? wander(&s->f1,&s->seed,0.0006f)*drift*40.0f:0.0f);
        s->lfo2 += (shift+w)/SR; if(s->lfo2>=1.0f)s->lfo2-=1.0f; if(s->lfo2<0.0f)s->lfo2+=1.0f;
        float cw=cosf(s->lfo2*TWO_PI), sw=sinf(s->lfo2*TWO_PI);
        for(int ch=0;ch<2;ch++){
            float *H=&s->hil[ch*34];                  /* 17 filters × 2 states */
            float x=(ch?r:l)[i];
            float iv=0.0f, qv=0.0f;                   /* i_out / q_out */
            for(int k=0;k<17;k++){                     /* QuadratureTransform.Process */
                float coef=-lut_ap_poles[k];
                float *dst=(k&1)?&qv:&iv;
                float src=(k<=1)?x:*dst;               /* first two read input, then cascade */
                *dst=warps_ap(&H[k*2], src, coef);
            }
            float sh=iv*cw - qv*sw;                    /* single-sideband shift */
            (ch?r:l)[i]=lerpf(x,sh,amount);
        }
    }
}

/* ── DIFFUSION ─────────────────────────────────────────────────────────────── */

/* delay-core helper. Differentiates BBD vs tape via: feedback cap, HF damping,
 * wow rate/depth, flutter, feedback saturation drive, and tape hiss. The wet tap
 * scales with amount so amount=0 → dry. */
static void delay_core(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift,
                       float fb_cap, float lp_amt, float wow_hz, float wow_depth,
                       float sat_drive, float flutter_depth, float hiss){
    float t=SR*(0.03f*powf(40.0f,macro));            /* 30 ms .. 1.2 s exp */
    float fb=amount*fb_cap;
    float wet=clampf(amount*1.4f,0.0f,1.0f);          /* amount=0 → dry */
    for(int i=0;i<n;i++){
        s->lfo+=wow_hz/SR;  if(s->lfo>=1.0f)s->lfo-=1.0f;     /* wow */
        s->lfo2+=6.3f/SR;   if(s->lfo2>=1.0f)s->lfo2-=1.0f;   /* flutter */
        float wob=wow_depth*sinf(s->lfo*TWO_PI)
                 + flutter_depth*sinf(s->lfo2*TWO_PI)
                 + (drift>0.0f? wander(&s->f1,&s->seed,0.001f)*drift*wow_depth*3.0f:0.0f);
        float d=t*(1.0f+wob);
        float tapL=dlr(s->dl_l,s->wp,d), tapR=dlr(s->dl_r,s->wp,d);
        /* 2-pole LP darkening in the feedback (more = warmer/tape) */
        s->z1l+=lp_amt*(tapL-s->z1l)+DENORM; s->z2l+=lp_amt*(s->z1l-s->z2l)+DENORM;
        s->z1r+=lp_amt*(tapR-s->z1r)+DENORM; s->z2r+=lp_amt*(s->z1r-s->z2r)+DENORM;
        float fl=delay_saturate(s->z2l*fb, sat_drive, 0.02f);
        float fr=delay_saturate(s->z2r*fb, sat_drive, 0.02f);
        if(hiss>0.0f){ float sig=fabsf(tapL)+fabsf(tapR);   /* gated tape hiss */
            if(sig>0.001f){ fl+=hiss*(frand(&s->seed)-0.5f); fr+=hiss*(frand(&s->seed)-0.5f); } }
        s->dl_l[s->wp]=l[i]+fl; s->dl_r[s->wp]=r[i]+fr;
        s->wp=(s->wp+1)%MAX_DELAY;
        l[i]+=tapL*0.9f*wet; r[i]+=tapR*0.9f*wet;
    }
}
/* CASCADE — BBD bucket-brigade: brighter-but-bandlimited repeats, fast subtle
 * clock warble, moderate saturation, no hiss. Clean-ish, metallic, can self-osc. */
static void fx_cascade(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    delay_core(s,l,r,n,amount,macro,drift,0.95f,0.22f,6.0f,0.00035f,1.3f,0.0f,0.0f);
}
/* REELS — worn tape echo (RE-201): warmer/darker, slow wow + flutter, heavier
 * tape saturation, gated hiss, higher feedback cap (self-oscillates). */
static void fx_reels(slot_dsp_t *s, float *l, float *r, int n,
                     float amount, float macro, float drift){
    delay_core(s,l,r,n,amount,macro,drift,1.05f,0.42f,0.7f,0.0016f,1.9f,0.0006f,0.0022f);
}

/* REVERSE — reverse delay. amount=wet mix, macro=segment time, drift=pitch mod.
 * Records forward; plays windowed segments backward. */
static void fx_reverse(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float seg=SR*(0.08f+macro*0.6f);                 /* 80..680 ms segment */
    float spd=1.0f+(drift>0.0f? wander(&s->f1,&s->seed,0.0008f)*drift*0.05f:0.0f);
    for(int i=0;i<n;i++){
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        s->f2-=spd; if(s->f2<=0.0f) s->f2+=seg;       /* reverse read offset grows */
        float d=s->f2;                                 /* read this far behind */
        float env=0.5f-0.5f*cosf(TWO_PI*(seg-s->f2)/seg);  /* window the segment */
        float oL=dlr(s->dl_l,s->wp,d)*env, oR=dlr(s->dl_r,s->wp,d)*env;
        s->wp=(s->wp+1)%MAX_DELAY;
        l[i]=lerpf(l[i],oL,amount); r[i]=lerpf(r[i],oR,amount);
    }
}

/* COLLAGE — glitch/granular looping delay. amount=feedback, macro=loop time,
 * drift=random double-speed/reverse grains. */
static void fx_collage(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float loop=SR*(0.05f+macro*1.2f);
    float fb=amount*0.9f;
    for(int i=0;i<n;i++){
        /* grain scheduler: f3=grain pos, f4=grain speed, i1=samples left */
        if(s->i1<=0){
            s->i1=(int)(SR*(0.04f+frand(&s->seed)*0.12f));
            s->f4=1.0f;
            if(drift>0.0f && frand(&s->seed)<drift*0.5f) s->f4=(frand(&s->seed)<0.5f?2.0f:-1.0f);
            s->f3=frand(&s->seed)*loop;
        }
        s->i1--;
        s->f3+=s->f4; if(s->f3<0)s->f3+=loop; if(s->f3>=loop)s->f3-=loop;
        float gL=dlr(s->dl_l,s->wp,loop-s->f3), gR=dlr(s->dl_r,s->wp,loop-s->f3);
        s->dl_l[s->wp]=l[i]+gL*fb; s->dl_r[s->wp]=r[i]+gR*fb;
        s->wp=(s->wp+1)%MAX_DELAY;
        l[i]=lerpf(l[i],gL,amount*0.9f); r[i]=lerpf(r[i],gR,amount*0.9f);
    }
}

/* ── TEXTURE ───────────────────────────────────────────────────────────────── */

/* FILTER — multimode Tilt/LP/HP (SVF) with resonance. amount=cutoff,
 * macro=mode (0 LP · 0.5 tilt · 1 HP), drift=cutoff wander. */
/* FILTER — single-knob DJ filter on a resonant Cytomic/TPT SVF (stable). amount
 * sweeps LP→through→HP (center = open/neutral, like a DJ isolator); macro = resonance;
 * drift = cutoff wander. State per ch: lp_*=ic1eq, bp_*=ic2eq. */
static void fx_filter(slot_dsp_t *s, float *l, float *r, int n,
                      float amount, float macro, float drift){
    float amt=clampf(amount,0.0f,1.0f);
    int hp; float fc;
    if(amt<0.5f){ hp=0; fc=200.0f*powf(90.0f, amt*2.0f); }        /* LP 200 Hz .. 18 kHz */
    else        { hp=1; fc=20.0f *powf(20.0f,(amt-0.5f)*2.0f); }  /* HP 20 Hz .. 400 Hz */
    if(drift>0.0f) fc*=1.0f+wander(&s->f1,&s->seed,0.0007f)*drift*0.3f;
    fc=clampf(fc,20.0f,18000.0f);
    float wetx=clampf(fabsf(amt-0.5f)/0.05f,0.0f,1.0f);           /* dry exactly at center */
    float g=tanf(3.14159265f*fc/SR);
    float k=1.0f-clampf(macro,0.0f,1.0f)*0.9f;                    /* resonance (1/Q) */
    float a1=1.0f/(1.0f+g*(g+k)), a2=g*a1, a3=g*a2;
    for(int ch=0;ch<2;ch++){
        float *ic1=ch?&s->lp_r:&s->lp_l, *ic2=ch?&s->bp_r:&s->bp_l;
        float *src=ch?r:l;
        for(int i=0;i<n;i++){
            float x=src[i];
            float v3=x-*ic2, v1=a1*(*ic1)+a2*v3, v2=*ic2+a2*(*ic1)+a3*v3;
            *ic1=2.0f*v1-*ic1+DENORM; *ic2=2.0f*v2-*ic2+DENORM;
            float filt=hp? (x-k*v1-v2) : v2;          /* HP or LP tap */
            src[i]=lerpf(x,filt,wetx);
        }
    }
}

/* SQUASH — vari-mu compressor + overdrive. PORTED from Airwindows Pressure4
 * (Chris Johnson, MIT). amount=compression, macro=mu character (punchy↔smooth),
 * drift=threshold jitter. State: f1/f2=muSpeed A/B, f3/f4=muCoeff A/B, i1=flip.
 * Faithful to the Pressure4 per-sample algorithm; VST/dither scaffolding dropped. */
static void fx_squash(slot_dsp_t *s, float *l, float *r, int n,
                      float amount, float macro, float drift){
    if(s->f1<1.0f){ s->f1=s->f2=10000.0f; s->f3=s->f4=1.0f; } /* Pressure4 ctor inits */
    float A=clampf(amount,0.0f,1.0f), B=0.4f;          /* B = release speed (fixed, musical) */
    float threshold=1.0f-(A*0.95f);
    float muMakeupGain=1.0f/threshold;
    float release=powf(1.28f-B,5.0f)*32768.0f;          /* overallscale = 1 @ 44.1k */
    float fastest=sqrtf(release);
    float mewiness=(macro*2.0f)-1.0f, unmew;
    int positivemu; if(mewiness>=0){positivemu=1;unmew=1.0f-mewiness;}
                    else{positivemu=0;mewiness=-mewiness;unmew=1.0f-mewiness;}
    float outGain=1.0f;
    float comp=0.5f+0.5f*threshold;                      /* gain-comp: cancels the 1/threshold makeup */
    for(int i=0;i<n;i++){
        float th=threshold*(drift>0.0f?(1.0f+wander(&s->f5,&s->seed,0.004f)*drift*0.2f):1.0f);
        float xl=l[i]*muMakeupGain, xr=r[i]*muMakeupGain;
        float sense=fabsf(xl); if(fabsf(xr)>sense) sense=fabsf(xr);
        float *spd = s->i1?&s->f1:&s->f2;                /* muSpeedA/B */
        float *cof = s->i1?&s->f3:&s->f4;                /* muCoefficientA/B */
        if(sense>th){
            float muVary=th/sense, muAttack=sqrtf(fabsf(*spd));
            *cof = *cof*(muAttack-1.0f);
            *cof += (muVary<th)? th : muVary;
            *cof /= muAttack;
        } else {
            *cof = *cof*((*spd)*(*spd)-1.0f) + 1.0f;
            *cof /= (*spd)*(*spd);
        }
        float ns=(*spd)*((*spd)-1.0f) + fabsf(sense*release)+fastest;
        *spd = ns/(*spd);
        float coeff = positivemu? (*cof)*(*cof) : sqrtf(fabsf(*cof));
        coeff = coeff*mewiness + (*cof)*unmew;
        xl*=coeff*outGain; xr*=coeff*outGain;
        /* Pressure4 second-stage sin() overdrive */
        float br=fabsf(xl); br=(br>1.57079633f)?1.0f:sinf(br); xl=(xl>0)?br:-br;
        br=fabsf(xr);      br=(br>1.57079633f)?1.0f:sinf(br); xr=(xr>0)?br:-br;
        l[i]=xl*comp; r[i]=xr*comp;                      /* level-matched output */
        s->i1^=1;
    }
}

/* CASSETTE — wow/flutter + tape sat + HF roll + hiss (mello apply_tape_stage
 * voicing). amount=degrade intensity, macro=tone, drift=warble depth. */
static void fx_cassette(slot_dsp_t *s, float *l, float *r, int n,
                        float amount, float macro, float drift){
    float wow=0.0006f+amount*0.0016f, flut=0.0002f+amount*0.0006f;
    float roll=0.06f+ (1.0f-macro)*0.5f;             /* tone: darker at low macro */
    float hiss=amount*0.004f;
    float base=SR*0.004f;
    for(int i=0;i<n;i++){
        s->lfo+=0.7f/SR; if(s->lfo>=1.0f)s->lfo-=1.0f;     /* wow ~0.7 Hz */
        s->lfo2+=6.3f/SR; if(s->lfo2>=1.0f)s->lfo2-=1.0f;  /* flutter ~6.3 Hz */
        float dd=(drift>0.0f? wander(&s->f1,&s->seed,0.002f)*drift:0.0f);
        float mod=wow*sinf(s->lfo*TWO_PI)*(1.0f+dd) + flut*sinf(s->lfo2*TWO_PI);
        float d=base*(1.0f+mod*200.0f);
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        float xL=dlr(s->dl_l,s->wp,d), xR=dlr(s->dl_r,s->wp,d);
        s->wp=(s->wp+1)%MAX_DELAY;
        /* mello-move tape sat: cubic + asymmetric 2nd-harmonic warmth + HF rolloff */
        xL=tape_cubic(xL,1.0f+amount*0.6f); xL=tape_asym(xL,1.0f,0.12f*amount);
        xR=tape_cubic(xR,1.0f+amount*0.6f); xR=tape_asym(xR,1.0f,0.12f*amount);
        s->z1l+=roll*(xL-s->z1l)+DENORM; s->z1r+=roll*(xR-s->z1r)+DENORM;
        float nz=hiss*((frand(&s->seed)-0.5f)*2.0f);
        l[i]=lerpf(l[i],s->z1l+nz,amount); r[i]=lerpf(r[i],s->z1r+nz,amount);
    }
}

/* BROKEN — motor-failure pitch drops + AM/FM wobble + dropouts. amount=breakdown,
 * macro=rate of failures, drift=dropout randomness. */
static void fx_broken(slot_dsp_t *s, float *l, float *r, int n,
                      float amount, float macro, float drift){
    float rate=0.1f+macro*macro*3.0f;                /* failure LFO Hz */
    float base=SR*0.01f;
    for(int i=0;i<n;i++){
        s->lfo+=rate/SR; if(s->lfo>=1.0f)s->lfo-=1.0f;
        /* sawtooth-ish motor slowdown: pitch dips then snaps back */
        float dip=amount*(0.5f-0.5f*cosf(s->lfo*TWO_PI));
        float vs=1.0f-dip*0.6f;                       /* varispeed read rate */
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        float rd=base*(2.0f-vs);                        /* slow read = lower pitch */
        float xL=dlr(s->dl_l,s->wp,rd), xR=dlr(s->dl_r,s->wp,rd);
        s->wp=(s->wp+1)%MAX_DELAY;
        /* dropout gate */
        if(drift>0.0f && frand(&s->seed)<drift*0.002f) s->i1=(int)(SR*0.02f*frand(&s->seed));
        float gate=1.0f; if(s->i1>0){ s->i1--; gate=0.0f; }
        /* AM wobble */
        float am=1.0f-amount*0.3f*(0.5f-0.5f*cosf(s->lfo*TWO_PI*2.0f));
        l[i]=lerpf(l[i],xL*gate*am,amount); r[i]=lerpf(r[i],xR*gate*am,amount);
    }
}

/* INTERFERENCE — lo-fi telecom/radio destruction. Core PORTED from Airwindows
 * DeRez2 (Chris Johnson, MIT): sample-rate reduction + soften + µ-law encode +
 * bit-depth quantize. Ring-mod carrier (macro) + static bursts (drift) on top.
 * amount=crush intensity, macro=carrier/tone, drift=static. State: z1=lastSample,
 * z2=heldSample, z3=lastDry, z4=lastOut (per ch); f1=position, f2/f3=incrA/incrB. */
static void fx_interference(slot_dsp_t *s, float *l, float *r, int n,
                            float amount, float macro, float drift){
    float A=clampf(amount,0.0f,1.0f);
    float targetA=A*A*A+0.0005f; if(targetA>1.0f)targetA=1.0f;
    float soften=(1.0f+targetA)/2.0f;
    float targetB=powf(1.0f-A,3.0f)/3.0f;             /* bit depth derez */
    float hard=0.45f;                                 /* more dry blend = less harsh */
    float carr=200.0f+macro*macro*3000.0f;
    const float L256=logf(256.0f);
    for(int i=0;i<n;i++){
        s->f2=((s->f2*999.0f)+targetA)/1000.0f;        /* incrementA (rate) */
        s->f3=((s->f3*999.0f)+targetB)/1000.0f;        /* incrementB (bits) */
        s->f1+=s->f2;                                  /* position */
        float in[2]={l[i],r[i]};
        float out[2]={s->z2l,s->z2r};                  /* outputSample = heldSample */
        if(s->f1>1.0f){
            s->f1-=1.0f;
            float last[2]={s->z1l,s->z1r};
            float h0=last[0]*s->f1+in[0]*(1.0f-s->f1);
            float h1=last[1]*s->f1+in[1]*(1.0f-s->f1);
            out[0]=out[0]*(1.0f-soften)+h0*soften;
            out[1]=out[1]*(1.0f-soften)+h1*soften;
            s->z2l=h0; s->z2r=h1;                      /* heldSample */
        }
        s->z1l=in[0]; s->z1r=in[1];                    /* lastSample */
        float lastOut[2]={s->z4l,s->z4r};
        float lastDry[2]={s->z3l,s->z3r};
        for(int c=0;c<2;c++){
            float x=out[c];
            if(x!=lastOut[c]){ float t=x; x=x*hard+lastDry[c]*(1.0f-hard); lastOut[c]=t; }
            else lastOut[c]=x;
            float temp=x;
            if(x>1.0f)x=1.0f; else if(x<-1.0f)x=-1.0f;  /* µ-law encode */
            if(x>0.0f) x= logf(1.0f+255.0f*fabsf(x))/L256;
            else if(x<0.0f) x=-logf(1.0f+255.0f*fabsf(x))/L256;
            x=temp*hard + x*(1.0f-hard);
            if(s->f3>0.0005f){                          /* bit-depth quantize (O(1)) */
                if(x>0.0f)      x=ceilf (x/s->f3)*s->f3;
                else if(x<0.0f) x=floorf(x/s->f3)*s->f3;
            }
            x=lerpf(x, x*sinf(s->lfo*TWO_PI), macro*0.4f);  /* ring-mod radio */
            if(drift>0.0f && frand(&s->seed)<drift*0.04f) x+=(frand(&s->seed)-0.5f)*drift;
            out[c]=x;
        }
        s->z3l=in[0]; s->z3r=in[1];                    /* lastDry = dry input */
        s->z4l=lastOut[0]; s->z4r=lastOut[1];
        s->lfo+=carr/SR; if(s->lfo>=1.0f)s->lfo-=1.0f;
        l[i]=lerpf(in[0], out[0]*0.85f, amount);       /* amount=0 → dry; trim µ-law boost */
        r[i]=lerpf(in[1], out[1]*0.85f, amount);
    }
}

/* ── Vtable: index by PFX_* id. OFF has no entry (host skips). ───────────────── */
static const palette_effect_t FX_TABLE[PFX_COUNT] = {
    [PFX_OFF]          = { fx_passthrough,   NULL },
    [PFX_DRIVE]        = { fx_drive,         NULL },
    [PFX_SWEETEN]      = { fx_sweeten,       NULL },
    [PFX_FUZZ]         = { fx_fuzz,          NULL },
    [PFX_HOWL]         = { fx_howl,          NULL },
    [PFX_SWELL]        = { fx_swell,         NULL },
    [PFX_FOLD]         = { fx_fold,          NULL },
    [PFX_DOUBLER]      = { fx_doubler,       NULL },
    [PFX_VIBRATO]      = { fx_vibrato,       NULL },
    [PFX_PHASER]       = { fx_phaser,        NULL },
    [PFX_TREMOLO]      = { fx_tremolo,       NULL },
    [PFX_PITCH]        = { fx_pitch,         NULL },
    [PFX_SHIFT]        = { fx_shift,         NULL },
    [PFX_CASCADE]      = { fx_cascade,       NULL },
    [PFX_REELS]        = { fx_reels,         NULL },
    [PFX_SPACE]        = { fx_passthrough,   NULL },  /* Clouds (heavy) — dispatched specially */
    [PFX_COLLAGE]      = { fx_collage,       NULL },
    [PFX_REVERSE]      = { fx_reverse,       NULL },
    [PFX_BLOOM]        = { fx_passthrough,   NULL },  /* Clouds (heavy) — dispatched specially */
    [PFX_FILTER]       = { fx_filter,        NULL },
    [PFX_SQUASH]       = { fx_squash,        NULL },
    [PFX_CASSETTE]     = { fx_cassette,      NULL },
    [PFX_BROKEN]       = { fx_broken,        NULL },
    [PFX_INTERFERENCE] = { fx_interference,  NULL },
    [PFX_FREEZE]       = { fx_passthrough,   NULL },  /* Clouds (heavy) — dispatched specially */
};

/* Clear all per-effect scratch on switch. Preserves the delay-line allocation,
 * the heavy (Clouds) pointer, and the RNG seed; zeroes the delay buffers. */
static void slot_reset(slot_t *sl){
    float *dl_l=sl->dsp.dl_l, *dl_r=sl->dsp.dl_r;
    void  *heavy=sl->dsp.heavy; int hk=sl->dsp.heavy_kind;
    uint32_t seed=sl->dsp.seed;
    memset(&sl->dsp, 0, sizeof(slot_dsp_t));
    sl->dsp.dl_l=dl_l; sl->dsp.dl_r=dl_r;
    sl->dsp.heavy=heavy; sl->dsp.heavy_kind=hk; sl->dsp.seed=seed;
    if(dl_l) memset(dl_l,0,MAX_DELAY*sizeof(float));
    if(dl_r) memset(dl_r,0,MAX_DELAY*sizeof(float));
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
/* Heavy-aware effect assignment — the single path for ALL select changes
 * (manual scroll, direct set, randomizer, preset load). Manages Clouds heavy
 * state (free on kind change, alloc on entering a Clouds effect) and arms the
 * click-free fade-in ramp. */
static void slot_apply_select(palette_t *p, int slot, int landed){
    slot_t *sl=&p->slots[slot];
    if(landed == sl->select) return;
    sl->prev_select = sl->select;
    sl->select = landed;
    if(sl->dsp.heavy && sl->dsp.heavy_kind != landed){
        pfx_clouds_free(sl->dsp.heavy);
        sl->dsp.heavy=NULL; sl->dsp.heavy_kind=0;
    }
    if(is_clouds_fx(landed) && !sl->dsp.heavy){
        sl->dsp.heavy = pfx_clouds_alloc(landed, SR);
        sl->dsp.heavy_kind = sl->dsp.heavy ? landed : 0;
    }
    slot_reset(sl);                  /* clear scratch (heavy preserved) */
    sl->ramp = 0.0f;                 /* fade in from silence to avoid clicks */
}
static void set_slot_select(palette_t *p, int slot, int requested, int dir){
    slot_apply_select(p, slot, resolve_select(p,slot,requested,dir));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  RANDOMIZERS  (design-spec §4) — all across the 4 slots at once.
 *  Effect picks are WITHOUT REPLACEMENT (distinct), bypassing the skip-walk.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void pick_distinct_effects(palette_t *p, uint32_t *rng){
    int pool[NUM_FX]; for(int i=0;i<NUM_FX;i++) pool[i]=i+1;   /* 1..24 */
    for(int i=NUM_FX-1;i>0;i--){ int j=rnd_int(rng,i+1); int t=pool[i];pool[i]=pool[j];pool[j]=t; }
    /* Free any current selections first so a Clouds effect can move slots without
     * a transient double-allocation, then assign the 4 distinct picks. */
    for(int s=0;s<NUM_SLOTS;s++) slot_apply_select(p,s,PFX_OFF);
    for(int s=0;s<NUM_SLOTS;s++) slot_apply_select(p,s,pool[s]);
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

/* ═══════════════════════════════════════════════════════════════════════════
 *  PRESETS — 25 factory patches. Each shows a chain the Chroma physically can't
 *  do (delay→reverb, stacked distinct effects, FOLD→SPACE…). amt/mac/drf are
 *  percent (0..100); ivol is percent of unity×2 (100 = unity); reorder is 0..23.
 *  The 4 effects per preset are DISTINCT (uniqueness); Off may repeat.
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    const char *name;
    uint8_t fx[4];                 /* PFX_* per slot */
    uint8_t amt[4], mac[4], drf[4];/* 0..100 */
    uint8_t mix, ivol, reorder;
} preset_t;

static const preset_t PRESETS[25] = {
/*  name              fx1..4                                  amt              mac              drf            mix ivol ord */
{"Init",          {PFX_OFF,PFX_OFF,PFX_OFF,PFX_OFF},          {0,0,0,0},       {0,0,0,0},       {0,0,0,0},     100,100,0},
{"Delay + Verb",  {PFX_CASCADE,PFX_SPACE,PFX_OFF,PFX_OFF},    {55,45,0,0},     {45,60,0,0},     {15,10,0,0},   100,100,0},
{"Tape Space",    {PFX_REELS,PFX_BLOOM,PFX_OFF,PFX_OFF},      {50,55,0,0},     {40,70,0,0},     {25,15,0,0},   100,100,0},
{"Fold Bloom",    {PFX_FOLD,PFX_SPACE,PFX_OFF,PFX_OFF},       {45,55,0,0},     {55,65,0,0},     {15,10,0,0},   100,95,0},
{"Freeze Reels",  {PFX_FREEZE,PFX_REELS,PFX_OFF,PFX_OFF},     {60,40,0,0},     {50,45,0,0},     {20,20,0,0},   100,100,0},
{"Shimmer Drive", {PFX_DRIVE,PFX_BLOOM,PFX_OFF,PFX_OFF},      {40,60,0,0},     {55,75,0,0},     {10,15,0,0},   100,100,0},
{"Fuzz Phaze Vrb",{PFX_FUZZ,PFX_PHASER,PFX_SPACE,PFX_OFF},    {50,55,45,0},    {45,50,60,0},    {20,15,10,0},  100,95,0},
{"West Coast",    {PFX_FOLD,PFX_FILTER,PFX_SPACE,PFX_OFF},    {55,50,40,0},    {50,55,60,0},    {15,10,10,0},  100,95,0},
{"Glitch Cloud",  {PFX_COLLAGE,PFX_FREEZE,PFX_SPACE,PFX_OFF}, {55,50,45,0},    {45,55,65,0},    {30,20,10,0},  100,100,0},
{"Vibe Verb",     {PFX_VIBRATO,PFX_SPACE,PFX_OFF,PFX_OFF},    {35,50,0,0},     {40,60,0,0},     {20,10,0,0},   100,100,0},
{"Slap Double",   {PFX_DOUBLER,PFX_CASCADE,PFX_OFF,PFX_OFF},  {55,45,0,0},     {40,35,0,0},     {15,15,0,0},   100,100,0},
{"Lo-fi Tape",    {PFX_CASSETTE,PFX_REELS,PFX_OFF,PFX_OFF},   {55,45,0,0},     {40,50,0,0},     {25,20,0,0},   100,100,0},
{"Broken Radio",  {PFX_BROKEN,PFX_INTERFERENCE,PFX_OFF,PFX_OFF},{45,50,0,0},   {40,45,0,0},     {25,30,0,0},   95,100,0},
{"Swell Pad",     {PFX_SWELL,PFX_SPACE,PFX_BLOOM,PFX_OFF},    {60,50,45,0},    {50,65,70,0},    {10,10,15,0},  100,100,0},
{"Octave Shift",  {PFX_SHIFT,PFX_BLOOM,PFX_OFF,PFX_OFF},      {45,55,0,0},     {60,70,0,0},     {15,15,0,0},   100,100,0},
{"Pitch Cloud",   {PFX_PITCH,PFX_COLLAGE,PFX_SPACE,PFX_OFF},  {50,45,45,0},    {65,50,60,0},    {15,25,10,0},  100,100,0},
{"Howl Stab",     {PFX_HOWL,PFX_REELS,PFX_OFF,PFX_OFF},       {55,40,0,0},     {50,45,0,0},     {20,15,0,0},   100,95,0},
{"Squash Drive",  {PFX_SQUASH,PFX_DRIVE,PFX_OFF,PFX_OFF},     {55,45,0,0},     {50,55,0,0},     {15,10,0,0},   100,100,0},
{"Tremolo Verb",  {PFX_TREMOLO,PFX_SPACE,PFX_OFF,PFX_OFF},    {45,50,0,0},     {45,60,0,0},     {15,10,0,0},   100,100,0},
{"Reverse Bloom", {PFX_REVERSE,PFX_BLOOM,PFX_OFF,PFX_OFF},    {50,55,0,0},     {45,70,0,0},     {15,15,0,0},   100,100,0},
{"Sweeten Tape",  {PFX_SWEETEN,PFX_CASSETTE,PFX_OFF,PFX_OFF}, {50,45,0,0},     {55,40,0,0},     {10,20,0,0},   100,100,0},
{"Filt Dly Verb", {PFX_FILTER,PFX_CASCADE,PFX_SPACE,PFX_OFF}, {50,50,45,0},    {50,45,60,0},    {15,15,10,0},  100,100,0},
{"Intf Freeze",   {PFX_INTERFERENCE,PFX_FREEZE,PFX_OFF,PFX_OFF},{50,55,0,0},   {45,55,0,0},     {30,20,0,0},   100,100,0},
{"Full Chain",    {PFX_DRIVE,PFX_PHASER,PFX_REELS,PFX_SPACE}, {40,45,45,40},   {50,50,45,60},   {10,15,15,10}, 100,95,0},
{"Ambient Wash",  {PFX_SWELL,PFX_SHIFT,PFX_BLOOM,PFX_FREEZE}, {60,40,55,45},   {50,60,70,55},   {10,15,15,20}, 100,100,0},
};

/* Apply a factory preset: heavy-aware select for all 4 slots, then snap params. */
static void load_preset(palette_t *p, int idx){
    idx=clampi(idx,1,25);
    const preset_t *pr=&PRESETS[idx-1];
    for(int s=0;s<NUM_SLOTS;s++) slot_apply_select(p,s,PFX_OFF);   /* clear (frees heavy) */
    for(int s=0;s<NUM_SLOTS;s++){
        slot_apply_select(p,s, clampi(pr->fx[s],0,NUM_FX));
        p->slots[s].amount=pr->amt[s]/100.0f;
        p->slots[s].macro =pr->mac[s]/100.0f;
        p->slots[s].drift =pr->drf[s]/100.0f;
    }
    p->mix=pr->mix/100.0f;
    p->input_vol=pr->ivol/100.0f;
    p->fx_reorder=clampi(pr->reorder,0,23);
    p->current_preset=idx;
}

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
    p->input_vol = 1.0f; p->iv_sm = 1.0f;
    p->mix = 1.0f; p->mix_sm = 1.0f;            /* design-spec: Mix default 100% */
    p->fx_reorder = 0;                          /* 1-2-3-4 */
    p->current_preset = 1;
    p->current_level = 1;                       /* LV_PALETTE — landing mirrors Console knobs */
    p->rng = 0x12345678u;
    if(g_host && g_host->log) g_host->log("[palette] instance created");
    return p;
}
static void destroy_instance(void *instance){
    palette_t *p=(palette_t*)instance; if(!p) return;
    for(int s=0;s<NUM_SLOTS;s++){
        if(p->slots[s].dsp.heavy) pfx_clouds_free(p->slots[s].dsp.heavy);
        free(p->slots[s].dsp.dl_l); free(p->slots[s].dsp.dl_r);
    }
    free(p);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PARAMS  (page-aware knob overlay + stable keys + chain_params)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Levels for page-aware knob overlay. Index 0 = root/Presets (landing). */
/* Knob-overlay pages. 0 = root/landing (preset+rnd knobs), 1 = Console (the
 * 8-knob amount/macro page), 2/3 = FX slot pages. */
enum { LV_PRESETS=0, LV_PALETTE, LV_FX12, LV_FX34, NUM_LEVELS };

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
        load_preset(p, clampi(p->current_preset+delta,1,25));
        return;
    }
    if(!strncmp(key,"rnd_",4)){ return; }  /* momentary enum: fires via direct set_param("1") only */
    /* floats */
    float step = (!strcmp(key,"input_vol")) ? 0.01f : 0.01f;
    set_float_key(p,key, get_float_key(p,key)+delta*step);
}

static void set_param(void *instance, const char *key, const char *val){
    palette_t *p=(palette_t*)instance; if(!p||!key||!val) return;

    /* active page for knob overlay. Host sends the level KEY or the module name;
     * "root"/"PALETTE" (module) → landing (presets), "Console" → page 1. */
    if(!strcmp(key,"_level") || !strcmp(key,"current_level")){
        if(!strcmp(val,"root")||!strcmp(val,"PALETTE")||!strcmp(val,"Console")) p->current_level=LV_PALETTE;
        else if(!strcmp(val,"Presets")) p->current_level=LV_PRESETS;
        else if(!strcmp(val,"FX12")) p->current_level=LV_FX12;
        else if(!strcmp(val,"FX34")) p->current_level=LV_FX34;
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

    if(!strcmp(key,"current_preset")){ load_preset(p, clampi(atoi(val),1,25)); return; }
    if(!strcmp(key,"fx_reorder")){
        /* accept "1-2-3-4" label or index */
        int idx=-1; for(int i=0;i<24;i++){ char lb[16]; perm_label(i,lb,sizeof lb);
            if(!strcmp(val,lb)){idx=i;break;} }
        if(idx<0) idx=clampi(atoi(val),0,23);
        p->fx_reorder=idx; return;
    }
    /* full-state restore (per-Set persistence). CSV: 4×(sel,amt,mac,drf),mix,ivol,ord,preset */
    if(!strcmp(key,"state")){
        int sel[4],ord=0,pre=1; float a[4],m[4],d[4],mix=1.0f,iv=1.0f;
        int got=sscanf(val,
            "%d,%f,%f,%f,%d,%f,%f,%f,%d,%f,%f,%f,%d,%f,%f,%f,%f,%f,%d,%d",
            &sel[0],&a[0],&m[0],&d[0], &sel[1],&a[1],&m[1],&d[1],
            &sel[2],&a[2],&m[2],&d[2], &sel[3],&a[3],&m[3],&d[3],
            &mix,&iv,&ord,&pre);
        if(got>=18){
            for(int s=0;s<NUM_SLOTS;s++) slot_apply_select(p,s,PFX_OFF);   /* frees heavy */
            for(int s=0;s<NUM_SLOTS;s++){
                slot_apply_select(p,s,clampi(sel[s],0,NUM_FX));
                p->slots[s].amount=clampf(a[s],0,1);
                p->slots[s].macro =clampf(m[s],0,1);
                p->slots[s].drift =clampf(d[s],0,1);
                p->slots[s].ramp=1.0f;            /* no fade on state restore */
            }
            p->mix=clampf(mix,0,1); p->input_vol=clampf(iv,0,2);
            p->fx_reorder=clampi(ord,0,23);
            if(got>=20) p->current_preset=clampi(pre,1,25);
        }
        return;
    }

    /* floats by stable key */
    set_float_key(p,key,atof(val));
}

static int get_param(void *instance, const char *key, char *buf, int buf_len){
    palette_t *p=(palette_t*)instance; if(!p||!key||!buf||buf_len<1) return -1;

    if(!strcmp(key,"name")) return snprintf(buf,buf_len,"PALETTE");

    /* ui_hierarchy — MUST mirror module.json; host reads this at runtime (priority).
     * Without it the module falls back to the preset browser and pages are unreachable.
     * Root = Presets landing (knobs = preset row; params = page links + menu-only FX Reorder).
     * Sub-level params are STRING arrays (Signal/Bobines lesson); metadata lives in chain_params. */
    if(!strcmp(key,"ui_hierarchy")){
        int o=0;
        o+=snprintf(buf+o,buf_len-o,
          "{\"modes\":null,\"levels\":{"
          "\"root\":{\"name\":\"PALETTE\","
          "\"knobs\":[\"fx1_amount\",\"fx1_macro\",\"fx2_amount\",\"fx2_macro\",\"fx3_amount\",\"fx3_macro\",\"fx4_amount\",\"fx4_macro\"],"
          "\"params\":["
          "{\"level\":\"Console\",\"label\":\"PALETTE\"},"
          "{\"level\":\"Presets\",\"label\":\"PRESETS&RND\"},"
          "{\"level\":\"FX12\",\"label\":\"FX 1&2\"},"
          "{\"level\":\"FX34\",\"label\":\"FX 3&4\"}"
          "]},"
          "\"Console\":{\"name\":\"PALETTE\","
          "\"knobs\":[\"fx1_amount\",\"fx1_macro\",\"fx2_amount\",\"fx2_macro\",\"fx3_amount\",\"fx3_macro\",\"fx4_amount\",\"fx4_macro\"],"
          "\"params\":[\"fx1_amount\",\"fx1_macro\",\"fx2_amount\",\"fx2_macro\",\"fx3_amount\",\"fx3_macro\",\"fx4_amount\",\"fx4_macro\"]},"
          "\"Presets\":{\"name\":\"PRESETS&RND\","
          "\"knobs\":[\"current_preset\",\"rnd_patch\",\"rnd_effect\",\"rnd_amount\",\"rnd_macro\",\"rnd_drift\",\"input_vol\",\"mix\"],"
          "\"params\":[\"current_preset\",\"rnd_patch\",\"rnd_effect\",\"rnd_amount\",\"rnd_macro\",\"rnd_drift\",\"input_vol\",\"mix\",\"fx_reorder\"]},"
          "\"FX12\":{\"name\":\"FX 1&2\","
          "\"knobs\":[\"fx1_select\",\"fx1_amount\",\"fx1_macro\",\"fx1_drift\",\"fx2_select\",\"fx2_amount\",\"fx2_macro\",\"fx2_drift\"],"
          "\"params\":[\"fx1_select\",\"fx1_amount\",\"fx1_macro\",\"fx1_drift\",\"fx2_select\",\"fx2_amount\",\"fx2_macro\",\"fx2_drift\"]},"
          "\"FX34\":{\"name\":\"FX 3&4\","
          "\"knobs\":[\"fx3_select\",\"fx3_amount\",\"fx3_macro\",\"fx3_drift\",\"fx4_select\",\"fx4_amount\",\"fx4_macro\",\"fx4_drift\"],"
          "\"params\":[\"fx3_select\",\"fx3_amount\",\"fx3_macro\",\"fx3_drift\",\"fx4_select\",\"fx4_amount\",\"fx4_macro\",\"fx4_drift\"]}"
          "}}");
        return o;
    }

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
          "{\"key\":\"current_preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":1,\"max\":50,\"step\":1},"
          "{\"key\":\"rnd_patch\",\"name\":\"Rnd Patch\",\"type\":\"enum\",\"options\":[\"0\",\"1\"]},"
          "{\"key\":\"rnd_effect\",\"name\":\"Rnd FX\",\"type\":\"enum\",\"options\":[\"0\",\"1\"]},"
          "{\"key\":\"rnd_amount\",\"name\":\"Rnd Amt\",\"type\":\"enum\",\"options\":[\"0\",\"1\"]},"
          "{\"key\":\"rnd_macro\",\"name\":\"Rnd Macro\",\"type\":\"enum\",\"options\":[\"0\",\"1\"]},"
          "{\"key\":\"rnd_drift\",\"name\":\"Rnd Drift\",\"type\":\"enum\",\"options\":[\"0\",\"1\"]},");
        /* FX Reorder — menu-only enum of all 24 chain permutations */
        o+=snprintf(buf+o,buf_len-o,"{\"key\":\"fx_reorder\",\"name\":\"FX Reorder\",\"type\":\"enum\",\"options\":[");
        for(int i=0;i<24;i++){ char lb[16]; perm_label(i,lb,sizeof lb);
            o+=snprintf(buf+o,buf_len-o,"%s\"%s\"",i?",":"",lb); }
        o+=snprintf(buf+o,buf_len-o,"]}");
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

    /* full-state serialize (per-Set persistence) — CSV, round-trips with set_param("state") */
    if(!strcmp(key,"state")){
        int o=0;
        for(int s=0;s<NUM_SLOTS;s++)
            o+=snprintf(buf+o,buf_len-o,"%d,%.4f,%.4f,%.4f,",
                p->slots[s].select,p->slots[s].amount,p->slots[s].macro,p->slots[s].drift);
        o+=snprintf(buf+o,buf_len-o,"%.4f,%.4f,%d,%d",
            p->mix,p->input_vol,p->fx_reorder,p->current_preset);
        return o;
    }

    return -1;   /* unknown key MUST be -1, never 0 */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  AUDIO  — slots processed in FX-Reorder order; equal-power global Mix.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void process_block(void *instance, int16_t *buf, int frames){
    palette_t *p=(palette_t*)instance; if(!p||frames<=0) return;
    if(frames>MAXFRAMES) frames=MAXFRAMES;

    const uint8_t *order = PERM[clampi(p->fx_reorder,0,23)];
    float gsm = 1.0f - expf(-(float)frames/(0.020f*SR));     /* 20 ms smooth for globals */
    p->iv_sm  += gsm*(p->input_vol - p->iv_sm);
    p->mix_sm += gsm*(p->mix       - p->mix_sm);
    float iv = p->iv_sm;

    /* de-interleave + input volume (dry kept for mix) */
    float dryL[MAXFRAMES], dryR[MAXFRAMES];
    for(int i=0;i<frames;i++){
        float l=buf[2*i]/32768.0f, r=buf[2*i+1]/32768.0f;
        dryL[i]=l; dryR[i]=r;
        p->L[i]=l*iv; p->R[i]=r*iv;
    }
    /* run the 4 slots in reorder sequence (Off = passthrough, never dispatched).
     * Clouds effects dispatch through the opaque heavy interface; C effects use the
     * vtable. Per-slot 20 ms analog-style smoothing of amount/macro/drift (no zipper
     * on knob moves or preset/random loads). A just-switched slot fades input→output
     * over ~25 ms with a smoothstep curve (click-free switching). */
    const float psm = 1.0f - expf(-(float)frames/(0.020f*SR));   /* 20 ms param smooth */
    const float ramp_inc = 1.0f / (SR * 0.025f);                 /* 25 ms switch fade */
    float preL[MAXFRAMES], preR[MAXFRAMES];
    for(int k=0;k<NUM_SLOTS;k++){
        slot_t *sl=&p->slots[order[k]];
        int fx=sl->select;
        if(fx==PFX_OFF) continue;
        sl->amt_sm += psm*(sl->amount - sl->amt_sm);
        sl->mac_sm += psm*(sl->macro  - sl->mac_sm);
        sl->drf_sm += psm*(sl->drift  - sl->drf_sm);
        int ramping = sl->ramp < 1.0f;
        if(ramping){ memcpy(preL,p->L,frames*sizeof(float)); memcpy(preR,p->R,frames*sizeof(float)); }
        if(is_clouds_fx(fx)){
            if(sl->dsp.heavy)
                pfx_clouds_process(fx, sl->dsp.heavy, p->L, p->R, frames,
                                   sl->amt_sm, sl->mac_sm, sl->drf_sm);
            /* heavy alloc failed → leave signal untouched (passthrough) */
        } else {
            FX_TABLE[fx].process(&sl->dsp, p->L, p->R, frames,
                                 sl->amt_sm, sl->mac_sm, sl->drf_sm);
        }
        if(ramping){
            float rr=sl->ramp;
            for(int i=0;i<frames;i++){
                float g=rr+ramp_inc*(float)i; if(g>1.0f)g=1.0f;
                g=g*g*(3.0f-2.0f*g);                  /* smoothstep — gentler crossfade */
                p->L[i]=preL[i]*(1.0f-g)+p->L[i]*g;
                p->R[i]=preR[i]*(1.0f-g)+p->R[i]*g;
            }
            sl->ramp = rr + ramp_inc*(float)frames; if(sl->ramp>1.0f) sl->ramp=1.0f;
        }
    }
    /* equal-power dry/wet + write back (NaN-guarded — many effects feed back) */
    float a=p->mix_sm*1.5707963f, dg=cosf(a), wg=sinf(a);
    for(int i=0;i<frames;i++){
        float ol=dg*dryL[i]+wg*p->L[i];
        float orr=dg*dryR[i]+wg*p->R[i];
        if(ol!=ol) ol=0.0f; if(orr!=orr) orr=0.0f;
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
