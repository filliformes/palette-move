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
/* Seed RNG + entropy pool from OS entropy so randomizers are truly random from the
 * very first tap (Magneto pattern), with a fixed fallback if /dev/urandom is absent. */
static void seed_urandom(uint32_t *rng, uint32_t *ent){
    uint32_t r=0x12345678u, e=0xB5297A4Du; unsigned char b[8];
    FILE *u=fopen("/dev/urandom","rb");
    if(u){ if(fread(b,1,8,u)==8){
        r=((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
        e=((uint32_t)b[4]<<24)|((uint32_t)b[5]<<16)|((uint32_t)b[6]<<8)|b[7]; } fclose(u); }
    if(r==0u) r=0x9e3779b9u; if(e==0u) e=0xB5297A4Du; *rng=r; *ent=e;
}

/* ── Palette effect IDs (0 = Off; group order = LED colour) ──────────────────── */
enum {
    PFX_OFF = 0,
    /* Character (group 0) */ PFX_DRIVE, PFX_SWEETEN, PFX_FUZZ, PFX_HOWL, PFX_FOLD, PFX_SWELL,
    /* Movement  (group 1) */ PFX_DOUBLER, PFX_VIBRATO, PFX_PHASER, PFX_TREMOLO, PFX_PITCH, PFX_SHIFT,
    /* Diffusion (group 2) */ PFX_CASCADE, PFX_REELS, PFX_COLLAGE, PFX_REVERSE, PFX_SPACE, PFX_BLOOM,
    /* Texture   (group 3) */ PFX_FILTER, PFX_SQUASH, PFX_CASSETTE, PFX_BROKEN, PFX_INTERFERENCE, PFX_HALO,
    PFX_COUNT               /* = 25 (Off + 24) */
};
#define NUM_FX (PFX_COUNT - 1)   /* 24 selectable effects */

/* Display names — index 0..24. get_param for selects MUST return these strings. */
static const char *FX_NAMES[PFX_COUNT] = {
    "Off",
    "Drive","Sweeten","Fuzz","Howl","Fold","Swell",
    "Doubler","Vibrato","Phaser","Tremolo","Pitch","Shift",
    "Cascade","Reels","Collage","Reverse","Space","Bloom",
    "Filter","Squash","Cassette","Broken","Interference","Halo"
};

/* ── Clouds C++ effects (fx_clouds.cc) — SPACE / BLOOM / FREEZE ───────────────
 * Fully isolated behind 3 opaque calls; palette.c never sees Clouds internals.
 * alloc returns heavy state (or NULL on OOM), freed on switch/destroy. */
extern void *pfx_clouds_alloc(int fx_id, float sr);
extern void  pfx_clouds_free(void *heavy);
extern void  pfx_clouds_reset(void *heavy);
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
    /* HALO resonator bank: 6 voices × per-channel damping one-pole */
    float  halo_lp_l[6], halo_lp_r[6];
    uint32_t seed;            /* per-slot deterministic RNG (drift) */
    /* tempo sync (set per block by the host): >0 = use instead of Macro mapping */
    float  sync_time;         /* synced delay time in samples (0 = free) */
    float  sync_rate;         /* synced LFO rate in Hz (0 = free) */
    float  lvlDry, lvlWet, lvlGain;   /* Character-group level match (see process_block) */
    float  lvlW;                      /* how much of it applies (fades with Amount) */
    /* heavy C++ (Clouds) effect state — lazily alloc'd on select change */
    void  *heavy;
    int    heavy_kind;        /* PFX_* id the heavy ptr currently serves (0 = none) */
} slot_dsp_t;

typedef struct {
    int   select;             /* PFX_* id (0 = Off) — the TARGET (UI, uniqueness, state) */
    int   active;             /* PFX_* id actually running in the DSP. Lags `select`: on a
                               * change the running effect first fades out to dry, then
                               * the slot swaps (slot_engage) and the new one fades in. */
    int   prev_select;        /* for Skip-walk direction inference */
    float amount, macro, drift;       /* knob targets */
    float amt_sm, mac_sm, drf_sm;     /* 15 ms analog-style smoothed values (fed to DSP) */
    float ramp;               /* 0..1 click-free fade-in after an effect switch */
    float xgain;              /* 0..1 fade-out of the running effect before a switch */
    float gate, genv;         /* Character noise gate: gain + dry peak envelope */
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
    return id==PFX_SPACE || id==PFX_BLOOM;
}

/* ── Instance ────────────────────────────────────────────────────────────────── */
typedef struct {
    slot_t slots[NUM_SLOTS];
    float  input_vol;         /* 0..2, unity = 1 */
    float  mix;               /* 0..1 global dry/wet */
    float  iv_sm, mix_sm;     /* 20 ms smoothed globals */
    int    fx_reorder;        /* 0..23 permutation index (0 = 1-2-3-4) — target */
    int    order_cur;         /* permutation actually running (lags fx_reorder by a dip) */
    float  ro_gain;           /* 0..1 chain wet gain for the reorder dip */
    int    current_preset;    /* 1..50 */
    int    current_level;     /* page-aware knob overlay (see LEVELS) */
    uint32_t rng;
    uint32_t ent;             /* live entropy pool (stirred from audio + sample_pos) */
    /* randomize/preset DUCK: fade out -> apply at silence -> fade back in */
    int      pending_rnd;     /* 0 none, 1 patch, 2 effect (param-only rnds apply instantly) */
    int      pending_preset;  /* -1 none, else target preset index */
    float    rnd_gain;        /* output duck envelope (1 = full) */
    int      rnd_phase;       /* 0 idle, 1 fade-out, 2 fade-in, 3 silent-hold */
    int      rnd_hold;        /* samples of silence remaining */
    /* ── GLOBAL page ─────────────────────────────────────────────── */
    float  feedback, fb_sm;   /* global feedback send 0..1 (smoothed) */
    int    tempo_src;         /* 0 = Move (host clock), 1 = Int */
    int    tempo_bpm;         /* 10..500 internal BPM */
    int    time_div;          /* 0 = Free, else index into DIVS[] */
    float  fbL[MAXFRAMES], fbR[MAXFRAMES];  /* feedback buffer (prev block out) */
    float  fb_lp_l, fb_lp_r;  /* feedback-path damping LP state */
    float  fb_hp_l, fb_hp_r;  /* feedback-path DC/sub high-pass state */
    /* Move clock tracking (derive BPM from MIDI clock pulses) */
    uint32_t sample_pos;          /* running sample counter */
    uint32_t last_clock_sample;   /* sample index at last quarter-note boundary */
    int    clock_count;           /* MIDI clock pulses since last quarter (24/qn) */
    float  clock_interval_sm;     /* smoothed samples per quarter */
    int    clock_running;         /* transport running (0xFA/0xFB/0xFC) */
    float  move_bpm;              /* BPM derived from Move clock */
    /* pre-allocated heavy Clouds engines (SPACE/BLOOM) — one each per instance,
     * referenced by whichever slot holds them. No audio-thread malloc. */
    void  *space_pool, *bloom_pool;
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

/* ── Tempo-sync time divisions. npb = notes-per-beat (1/4 = 1). ──────────────── */
typedef struct { const char *name; float npb; } tdiv_t;
static const tdiv_t DIVS[] = {
    {"Free",0.0f}, {"1/1",0.25f}, {"1/2",0.5f}, {"1/2T",0.75f}, {"1/2D",0.33333f},
    {"1/4",1.0f}, {"1/4T",1.5f}, {"1/4D",0.66667f}, {"1/8",2.0f}, {"1/8T",3.0f},
    {"1/8D",1.33333f}, {"1/16",4.0f}, {"1/16T",6.0f}, {"1/16D",2.66667f}, {"1/32",8.0f}
};
#define NUM_DIVS 15

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

/* ── First-order ADAA (antiderivative anti-aliasing) ────────────────────────────
 * A hard-driven shaper folds its harmonics back below Nyquist as inharmonic fizz
 * (Drive measured more alias than harmonic energy at full Amount). ADAA outputs the
 * AVERAGE of the shaper over the segment between successive input samples,
 * (F(u1)-F(u0))/(u1-u0) with F the antiderivative, which suppresses most of it for
 * the cost of a half-sample delay. log(cosh) is the antiderivative of tanh; double
 * precision because F is large where tanh is flat and the difference is small. */
static inline double logcosh_d(double x){ double a=fabs(x); return a + log1p(exp(-2.0*a)) - 0.6931471805599453; }
static inline float tanh_adaa(float u, float *u_prev){
    float u0=*u_prev; *u_prev=u;
    double du=(double)u-(double)u0;
    if(fabs(du)<1e-3) return tanhf(0.5f*(u+u0));
    return (float)((logcosh_d(u)-logcosh_d(u0))/du);
}

static void fx_passthrough(slot_dsp_t *s, float *l, float *r, int n,
                           float amount, float macro, float drift){
    (void)s;(void)l;(void)r;(void)n;(void)amount;(void)macro;(void)drift;
}

/* ── CHARACTER ─────────────────────────────────────────────────────────────── */

/* DRIVE — tube-ish overdrive that KEEPS the low end. The bass band (<~200 Hz) is
 * split off and passed clean; only the harmonic band is driven, through an
 * asymmetric (tube-ish, even-harmonic) tanh with ADAA, DC-blocked after.
 * (Was the Airwindows Spiral sin(x|x|)/|x|: at the gains Amount reaches, x|x| runs
 * to ~80 rad, so it wavefolded dozens of times per cycle - more alias than signal.)
 * The operating point is subtracted, so silence in = silence out even with bias.
 * macro = pre-emphasis tilt (brightens INTO the shaper, never high-passes the
 * output), drift = slow bias wander. State: z1=bass one-pole, z2/z3=DC-block x/y,
 * sm1/sm2=ADAA history L/R, f2=per-sample drive glide. */
static void fx_drive(slot_dsp_t *s, float *l, float *r, int n,
                     float amount, float macro, float drift){
    float drv  = 1.0f + amount*6.0f + amount*amount*30.0f;
    float tilt = (macro-0.5f)*2.0f;                  /* −1..+1 brightness tilt */
    const float aBass=0.030f;                        /* ~200 Hz low/high split */
    const float asym=0.20f;                          /* tube-ish operating point */
    float himk = (0.7f + amount*0.5f)*0.8f;          /* harmonic-band makeup */
    if(s->f2<=0.0f) s->f2=drv;
    for(int i=0;i<n;i++){
        s->f2 += 0.01f*(drv - s->f2);                /* glide: no gain steps */
        float bias = drift>0.0f ? wander(&s->f1,&s->seed,0.0006f)*drift*0.10f : 0.0f;
        float c = asym + bias*4.0f;                  /* operating point (not scaled by drive) */
        float tc = tanhf(c);
        for(int ch=0;ch<2;ch++){
            float x=(ch?r:l)[i];
            float *lp=ch?&s->z1r:&s->z1l;
            *lp += aBass*(x-*lp)+DENORM;
            float bass=*lp, high=x-*lp;              /* clean sub + harmonic band */
            float pre=high*(1.0f+0.6f*tilt);         /* tilt = pre-emphasis, not HPF */
            float sh=tanh_adaa(pre*s->f2 + c, ch?&s->sm2:&s->sm1) - tc;
            float *dx=ch?&s->z2r:&s->z2l, *dy=ch?&s->z3r:&s->z3l;
            float y=sh - *dx + 0.9993f*(*dy); *dx=sh; *dy=y+DENORM;   /* DC block (asym) */
            float wet=bass + y*himk;                 /* bass FULL → low end intact */
            (ch?r:l)[i]=lerpf(x, wet, amount);       /* amount=0 → dry */
        }
    }
}

/* SWEETEN — console preamp. Gentle comp + the Airwindows Density sin-fold
 * saturator (Chris Johnson, MIT) for body, + an Air-style HF tilt shelf.
 * amount=comp/sat, macro=tone (dark↔air), drift=subtle level drift. */
static void fx_sweeten(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float tilt=(macro-0.5f)*2.0f;
    float thr=0.5f, ratio=0.35f+amount*0.4f;         /* gentle comp */
    float density=amount*3.0f, dwet=clampf(density,0.0f,1.0f);  /* Density sat amount */
    float comp=1.0f/(1.0f+density*0.5f);             /* compensate sin-fold gain */
    for(int i=0;i<n;i++){
        float mono=0.5f*(fabsf(l[i])+fabsf(r[i]));
        s->env += (mono>s->env?0.02f:0.004f)*(mono-s->env)+DENORM;  /* fast atk slow rel */
        float gr=1.0f; if(s->env>thr) gr=1.0f-(1.0f-thr/s->env)*ratio;
        float dl=(drift>0.0f)? 1.0f+wander(&s->f1,&s->seed,0.0004f)*drift*0.03f : 1.0f;
        float mdrive=1.0f+amount*2.2f;                /* density drive */
        float mcomp=1.0f/(0.55f+mdrive*0.45f);
        for(int ch=0;ch<2;ch++){
            float dry=(ch?r:l)[i];
            float x=dry*gr*dl;
            /* Airwindows Mojo density fold (Chris Johnson, MIT): pow(|x|,0.25)
             * thickens low-level detail before a very soft sin-fold → perceived
             * density/RMS ("louder without clipping"), the console-glue voice. */
            float xx=x*mdrive;
            float mojo=sqrtf(sqrtf(fabsf(xx)+1e-9f));  /* |x|^0.25 (two sqrts) */
            float sat=(sinf(xx*mojo*1.57079633f)/mojo)*0.987654f*mcomp;
            (void)dwet;(void)comp;(void)density;
            /* Air-style HF tilt shelf (macro) */
            float *z=ch?&s->z1r:&s->z1l;
            *z += 0.10f*(sat-*z)+DENORM;              /* LP component */
            float wet=(tilt>=0.0f)? sat+(sat-*z)*tilt*0.8f      /* brighten = air */
                                  : lerpf(sat,*z,-tilt);        /* darken */
            (ch?r:l)[i]=lerpf(dry,wet,amount);        /* amount=0 → dry */
        }
    }
}

/* FUZZ — Big-Muff-style TWO cascaded soft-clip stages. The cascade gives the
 * compressed, sustaining, "singing" voice at MODERATE output (not just loud):
 * input band-limited ~1.2 kHz → starved bias → two soft clips in series →
 * DC-block → post tone. amount=sustain/gain, macro=tone/bias, drift=bias drift.
 * State: z3=input LP, z4/bp=DC-block x/y, z2=tone LP. */
static void fx_fuzz(slot_dsp_t *s, float *l, float *r, int n,
                    float amount, float macro, float drift){
    float sustain=0.5f+amount*amount*2.6f;           /* pre-gain into the cascade */
    float bias=(macro-0.5f)*0.4f;                    /* starved bias → asymmetry */
    float tone=0.12f+macro*0.5f;                     /* post tone LP */
    const float aIn=0.157f;                          /* input LP ~1.2 kHz */
    float g=28.0f*sustain;
    for(int i=0;i<n;i++){
        /* bias drift: slow (~0.4 s) and modest. It was a ~11 ms random walk of +-0.3
         * through x87 gain - a loud rumble on its own (-6 dBFS out of SILENCE). */
        s->f2 += 0.004f*(bias - s->f2);              /* bias glides (~6 ms): Tone turns don't jump */
        float b=s->f2 + (drift>0.0f? wander(&s->f1,&s->seed,0.00006f)*drift*0.12f:0.0f);
        float tb=tanhf(g*b);                         /* operating point, subtracted */
        for(int ch=0;ch<2;ch++){
            float x=(ch?r:l)[i];
            float *ilp=ch?&s->z3r:&s->z3l;
            *ilp += aIn*(x-*ilp)+DENORM;
            float xi=*ilp + b;
            /* both stages ADAA (true tanh) — the near-square output aliased hard */
            float s1=-(tanh_adaa(g*xi, ch?&s->sm3:&s->sm1) - tb);          /* stage 1 (inverts) */
            float s2=-tanh_adaa(g*0.5f*s1, ch?&s->sm4:&s->sm2);            /* stage 2 → sustain */
            float *dcx=ch?&s->z4r:&s->z4l, *dcy=ch?&s->bp_r:&s->bp_l;
            float y=s2 - *dcx + 0.9995f*(*dcy);       /* DC blocker (bias offset) */
            *dcx=s2; *dcy=y;
            float *tz=ch?&s->z2r:&s->z2l;
            *tz += tone*(y-*tz)+DENORM;
            (ch?r:l)[i]=lerpf(x, *tz*0.6f, amount);   /* moderate output; amount=0 → dry */
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
    float base=120.0f*powf(40.0f,macro);             /* resonant freq 120..4800 Hz (TILT) */
    float fz=base*(drift>0.0f?(1.0f+wander(&s->f1,&s->seed,0.0008f)*drift*0.5f):1.0f);
    float g=tanf(3.14159265f*clampf(fz,40.0f,9000.0f)/SR);
    float k=1.0f-amount*0.985f; if(k<0.015f)k=0.015f; /* damping → near self-oscillation */
    float a1=1.0f/(1.0f+g*(g+k)), a2=g*a1, a3=g*a2;
    float drive=1.0f+amount*amount*20.0f;            /* fuzz drive feeding the resonator */
    float voice=0.6f+amount*0.9f;
    for(int ch=0;ch<2;ch++){
        float *ic1=ch?&s->lp_r:&s->lp_l, *ic2=ch?&s->bp_r:&s->bp_l;
        float *src=ch?r:l;
        for(int i=0;i<n;i++){
            /* Fuzz FRONT-END: harmonic-rich excitation so the high-Q filter sings,
             * blooms and stabs (manual: "resonant filter fuzz"), not a clean BP. */
            float fuzz=sb_apply_dist(src[i]*drive,2);
            float v3=fuzz-*ic2;
            float v1=a1*(*ic1)+a2*v3;
            float v2=*ic2+a2*(*ic1)+a3*v3;
            *ic1=2.0f*v1-*ic1+DENORM; *ic2=2.0f*v2-*ic2+DENORM;
            float bp=sb_tanh(v1*voice);              /* soft-clip resonance → self-limits, blooms */
            src[i]=lerpf(src[i],bp*0.9f,amount);     /* amount=0 → dry */
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
            (ch?r:l)[i]=lerpf(x,x*(*sw),amount); /* amount=0 → dry */
        }
    }
}

/* Authentic Mutable Warps tables (warps_data.c, MIT) — see SOURCES. */
extern const float lut_bipolar_fold[];   /* 4096-pt, centred at +2048, range ±0.87 */
extern const float lut_ap_poles[];       /* 17 Hilbert allpass poles (SHIFT) */

/* FOLD — West-Coast wavefolder using the REAL Warps lut_bipolar_fold curve via
 * the ALGORITHM_FOLD interpolation. amount=fold drive, macro=offset/symmetry,
 * drift=fold-point wobble. */
/* The fold curve as a continuous function of the LUT index (linear interpolation,
 * clamped flat outside [1,4095] exactly as the original lookup did) and its running
 * integral, so FOLD can use ADAA. fold_cum[] is built once in create_instance. */
static double fold_cum[4096];
static int    fold_cum_ok;
static void fold_adaa_init(void){
    if(fold_cum_ok) return;
    double c=0.0; fold_cum[0]=0.0;
    for(int i=1;i<4096;i++){ c+=0.5*((double)lut_bipolar_fold[i-1]+(double)lut_bipolar_fold[i]); fold_cum[i]=c; }
    fold_cum_ok=1;
}
static inline float fold_at(float idx){
    if(idx<1.0f) return lut_bipolar_fold[1];
    if(idx>=4095.0f) return lut_bipolar_fold[4095];
    int i=(int)idx; float f=idx-(float)i;
    return lut_bipolar_fold[i]+(lut_bipolar_fold[i+1]-lut_bipolar_fold[i])*f;
}
static inline double fold_F(double idx){                 /* integral of fold_at */
    if(idx<1.0) return fold_cum[1] + (idx-1.0)*lut_bipolar_fold[1];
    if(idx>=4095.0) return fold_cum[4095] + (idx-4095.0)*lut_bipolar_fold[4095];
    int i=(int)idx; double f=idx-(double)i, a=lut_bipolar_fold[i], b=lut_bipolar_fold[i+1];
    return fold_cum[i] + a*f + 0.5*(b-a)*f*f;
}
static void fx_fold(slot_dsp_t *s, float *l, float *r, int n,
                    float amount, float macro, float drift){
    const float kScale=2048.0f/2.295f;               /* Warps: 2048/((2+0.25)*1.02) */
    if(!fold_cum_ok) fold_adaa_init();
    float drive=0.5f+amount*amount*4.0f;
    float off=(macro-0.5f)*1.6f;
    float comp=0.55f/(0.6f+0.4f*drive);              /* level compensation — folding is loud */
    for(int i=0;i<n;i++){
        float w=(drift>0.0f? wander(&s->f1,&s->seed,0.0009f)*drift*0.25f:0.0f);
        float idx_op=(off+w)*kScale + 2048.0f;         /* operating point (no signal) */
        float y_op=fold_at(idx_op);                    /* subtracted: no DC, silence = silence */
        for(int ch=0;ch<2;ch++){
            float dry=(ch?r:l)[i];
            float idx=(dry*drive + off + w)*kScale + 2048.0f;   /* index into lut_bipolar_fold */
            float *ip=ch?&s->sm2:&s->sm1;              /* ADAA: average over the segment */
            if(*ip==0.0f) *ip=idx;
            double di=(double)idx-(double)*ip;
            float y=(fabs(di)<0.01)? fold_at(0.5f*(idx+*ip))
                                   : (float)((fold_F(idx)-fold_F(*ip))/di);
            *ip=idx;
            y-=y_op;
            float *dx=ch?&s->z2r:&s->z2l, *dy=ch?&s->z3r:&s->z3l;
            float yb=y - *dx + 0.9993f*(*dy); *dx=y; *dy=yb+DENORM;   /* DC block: offset folds */
            float *z=ch?&s->z1r:&s->z1l;
            *z += 0.5f*(yb*comp-*z)+DENORM;            /* mild LP smooth, level-matched */
            (ch?r:l)[i]=lerpf(dry,*z,amount);          /* amount=0 → dry */
        }
    }
}

/* ── MOVEMENT ──────────────────────────────────────────────────────────────── */

/* DOUBLER — ADT stereo double-track / slapback. amount=mix, macro=time,
 * drift=random momentary detune. */
static void fx_doubler(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float tgtL=(s->sync_time>0.0f)? s->sync_time : SR*(0.012f+macro*0.045f); /* 12..57ms / synced */
    if(tgtL>(float)(MAX_DELAY/2)) tgtL=(float)(MAX_DELAY/2);
    if(s->f2<1.0f) s->f2=tgtL;                        /* f2 = gliding delay time */
    for(int i=0;i<n;i++){
        s->f2 += clampf(0.001f*(tgtL-s->f2), -0.25f, 0.25f);   /* Time glides: no read-head jumps */
        float baseL=s->f2, baseR=baseL*1.28f;
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
    float rate=(s->sync_rate>0.0f)? s->sync_rate : 0.5f+macro*7.5f;  /* 0.5..8 Hz / synced */
    float dtarget=SR*0.0005f*(0.3f+amount*4.0f);     /* mod depth in samples */
    float base=SR*0.006f;
    float fmRate=rate*1.61f;                          /* incommensurate 2nd LFO */
    float fmDepth=0.2f+drift*0.35f;                   /* through-zero FM amount (tamed) */
    if(s->f2<=0.0f) s->f2=dtarget;
    for(int i=0;i<n;i++){
        s->f2 += 0.003f*(dtarget - s->f2);           /* depth glides: no read-head jumps */
        float depth=s->f2;
        s->lfo2+=fmRate/SR; if(s->lfo2>=1.0f)s->lfo2-=1.0f;
        s->lfo+=(rate*(1.0f+fmDepth*0.5f*sinf(s->lfo2*TWO_PI)))/SR;  /* through-zero FM */
        if(s->lfo>=1.0f)s->lfo-=1.0f; if(s->lfo<0.0f)s->lfo+=1.0f;
        float phR=s->lfo+0.25f; if(phR>=1.0f)phR-=1.0f;   /* R 90° offset = stereo */
        float rndL=(drift>0.0f? wander(&s->f1,&s->seed,0.008f):0.0f);
        float rndR=(drift>0.0f? wander(&s->f3,&s->seed,0.008f):0.0f);
        float modL=lerpf(sinf(s->lfo*TWO_PI),rndL,clampf(drift,0.0f,0.9f));
        float modR=lerpf(sinf(phR*TWO_PI),    rndR,clampf(drift,0.0f,0.9f));
        float dL=base+depth*(1.0f+modL), dR=base+depth*(1.0f+modR);
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        float oL=dlr(s->dl_l,s->wp,dL), oR=dlr(s->dl_r,s->wp,dR);
        /* HF restore: high-shelf the interpolation loss back in */
        s->z1l+=0.4f*(oL-s->z1l)+DENORM; oL+=(oL-s->z1l)*0.10f;
        s->z1r+=0.4f*(oR-s->z1r)+DENORM; oR+=(oR-s->z1r)*0.10f;
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
    /* 2..12 stages, CONTINUOUS: all 12 allpasses always run (their state stays live)
     * and the output blends between stage n and n+1. An integer stage count jumped
     * the phase response at every step of Amount - a click per step. */
    float stf=2.0f+amount*10.0f; int n0=(int)stf; if(n0>12)n0=12;
    float sfr=(n0<12)? stf-(float)n0 : 0.0f;
    float rate=(s->sync_rate>0.0f)? s->sync_rate : 0.05f+macro*macro*4.0f;  /* synced */
    float fb=amount*0.6f;
    for(int i=0;i<n;i++){
        s->lfo+=rate/SR; if(s->lfo>=1.0f)s->lfo-=1.0f;
        float rnd=(drift>0.0f? wander(&s->f2,&s->seed,0.01f)*drift*0.4f:0.0f);
        for(int ch=0;ch<2;ch++){
            /* quadrature LFO: R is 90° ahead of L → fully stereo sweep */
            float ph=s->lfo + (ch?0.25f:0.0f); if(ph>=1.0f)ph-=1.0f;
            float sweep=0.5f+0.5f*sinf(ph*TWO_PI)+rnd;
            float fc=200.0f*exp2f(4.0f*clampf(sweep,0.0f,1.0f)); /* 16^x = 2^(4x), exact + cheaper */
            float g=tanf(3.14159265f*clampf(fc,30.0f,12000.0f)/SR);
            float coef=(g-1.0f)/(g+1.0f);                       /* allpass coefficient */
            float *ap=ch?s->ap_r:s->ap_l; float *fbz=ch?&s->z2r:&s->z2l;
            float x=(ch?r:l)[i]+ *fbz*fb, xa=x, xb=x;
            for(int k=0;k<12;k++){
                float y=coef*x+ap[k];                 /* 1st-order allpass */
                ap[k]=x-coef*y+DENORM; x=y;
                if(k==n0-1) xa=x; else if(k==n0) xb=x;
            }
            x=xa+(xb-xa)*sfr;
            *fbz=x;
            (ch?r:l)[i]=lerpf((ch?r:l)[i],0.5f*((ch?r:l)[i]+x),amount); /* amount=0 → dry */
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
        float speed=(s->sync_rate>0.0f)? s->sync_rate*TWO_PI/SR : 0.0001f+(s->sm1/1000.0f);
        float s2=s->sm2*s->sm2, s4=s2*s2; float skew=1.0f+s->sm2*s4*s4; /* sm2^9 via mults */
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
    /* read delay = win - f1, so the read head moves 1 + (ratio-1) = ratio samples per
     * sample. (Was 1-ratio: speed 2-ratio, i.e. inverted, and FROZEN at Macro 1.) */
    float drate=(ratio-1.0f);                         /* read-ptr drift / sample */
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
    float t=(s->sync_time>0.0f)? s->sync_time : SR*(0.03f*powf(40.0f,macro)); /* 30ms..1.2s / synced */
    if(s->f6<1.0f || s->f6>SR*2.5f) s->f6=t;          /* init/repair smoothed time */
    float fb=amount*fb_cap;
    float wet=clampf(amount*1.4f,0.0f,1.0f);          /* amount=0 → dry */
    for(int i=0;i<n;i++){
        /* glide delay time, slew-limited to +-0.5 samples/sample (pitch 0.5x..1.5x while
         * it moves, like a tape machine) - unlimited, a fast Time turn read at -8x */
        s->f6 += clampf(0.0004f*(t-s->f6), -0.5f, 0.5f);
        s->lfo+=wow_hz/SR;  if(s->lfo>=1.0f)s->lfo-=1.0f;     /* wow */
        s->lfo2+=6.3f/SR;   if(s->lfo2>=1.0f)s->lfo2-=1.0f;   /* flutter */
        float wob=wow_depth*sinf(s->lfo*TWO_PI)
                 + flutter_depth*sinf(s->lfo2*TWO_PI)
                 + (drift>0.0f? wander(&s->f1,&s->seed,0.001f)*drift*wow_depth*3.0f:0.0f);
        float d=s->f6*(1.0f+wob);
        float tapL=dlr(s->dl_l,s->wp,d), tapR=dlr(s->dl_r,s->wp,d);
        /* 2-pole LP darkening in the feedback (more = warmer/tape) */
        s->z1l+=lp_amt*(tapL-s->z1l)+DENORM; s->z2l+=lp_amt*(s->z1l-s->z2l)+DENORM;
        s->z1r+=lp_amt*(tapR-s->z1r)+DENORM; s->z2r+=lp_amt*(s->z1r-s->z2r)+DENORM;
        float fl=delay_saturate(s->z2l*fb, sat_drive, 0.02f);
        float fr=delay_saturate(s->z2r*fb, sat_drive, 0.02f);
        /* hiss stays OUT of the feedback path: at REELS's >unity feedback it accumulated
         * into loud noise, and held its own gate open (ported from Loopex). */
        s->dl_l[s->wp]=l[i]+fl; s->dl_r[s->wp]=r[i]+fr;
        s->wp=(s->wp+1)%MAX_DELAY;
        float hsL=0.0f,hsR=0.0f;
        if(hiss>0.0f){
            /* tape hiss on the wet output only, riding a smoothed echo envelope (2 ms up,
             * ~150 ms down) and low-passed ~6 kHz. It used to be switched per SAMPLE on
             * the instantaneous tap level, so it chattered on/off at every zero crossing. */
            float mag=0.5f*(fabsf(tapL)+fabsf(tapR));
            s->env += (mag>s->env? 0.01f : 0.00015f)*(mag-s->env) + DENORM;
            float hg=hiss*clampf(s->env*30.0f,0.0f,1.0f);
            s->z3l += 0.575f*(hg*(frand(&s->seed)-0.5f)-s->z3l) + DENORM;
            s->z3r += 0.575f*(hg*(frand(&s->seed)-0.5f)-s->z3r) + DENORM;
            hsL=s->z3l; hsR=s->z3r;
        }
        l[i]+=(tapL*0.9f+hsL)*wet; r[i]+=(tapR*0.9f+hsR)*wet;
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
    delay_core(s,l,r,n,amount,macro,drift,1.05f,0.42f,0.7f,0.0016f,1.9f,0.0006f,0.0006f);
}

/* REVERSE — reverse delay. amount=wet mix, macro=segment time, drift=pitch mod.
 * Records forward; two read heads play the recent past BACKWARD (delay grows by 2
 * samples per sample = reverse at 1x), half a segment apart, each under a sin^2
 * window, so the windows sum to 1 and the reversed stream is continuous. Each head
 * latches the segment length when it wraps, so turning Time never jumps a head.
 * (Was one head whose delay SHRANK by 1/sample: forward at 2x, not reverse.)
 * State: f2=phase, f3/f4=latched segment A/B, f6=smoothed segment. */
static void fx_reverse(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float seg=(s->sync_time>0.0f)? s->sync_time : SR*(0.08f+macro*0.6f); /* 80..680ms / synced */
    if(seg>(float)(MAX_DELAY/2-8)) seg=(float)(MAX_DELAY/2-8);
    float spd=1.0f+(drift>0.0f? wander(&s->f1,&s->seed,0.0008f)*drift*0.05f:0.0f);
    if(s->f6<1.0f){ s->f6=seg; s->f3=seg; s->f4=seg; s->f2=0.0f; }
    for(int i=0;i<n;i++){
        s->f6 += 0.0005f*(seg-s->f6);
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        float ph=s->f2, phb=ph+0.5f; if(phb>=1.0f) phb-=1.0f;
        float sa=sinf(3.14159265f*ph),  wa=sa*sa;
        float sb=sinf(3.14159265f*phb), wb=sb*sb;
        float da=2.0f+2.0f*s->f3*ph, db=2.0f+2.0f*s->f4*phb;
        float oL=dlr(s->dl_l,s->wp,da)*wa + dlr(s->dl_l,s->wp,db)*wb;
        float oR=dlr(s->dl_r,s->wp,da)*wa + dlr(s->dl_r,s->wp,db)*wb;
        s->wp=(s->wp+1)%MAX_DELAY;
        float nph=ph+spd/s->f6;
        if(ph<0.5f && nph>=0.5f) s->f4=s->f6;          /* head B wraps (its window is 0) */
        if(nph>=1.0f){ nph-=1.0f; s->f3=s->f6; }       /* head A wraps */
        s->f2=nph;
        l[i]=lerpf(l[i],oL,amount); r[i]=lerpf(r[i],oR,amount);
    }
}

/* COLLAGE — glitch/granular looping delay. amount=feedback, macro=loop time,
 * drift=random double-speed/reverse grains.
 * A grain is a read head at delay f3 inside the loop; the delay moves by (1-speed)
 * per sample, so speed 1 = steady repeat, 2 = octave up, -1 = reverse. (It used to
 * move by -speed: every grain played at 2x and "reverse" grains froze on one sample.)
 * Every grain change - new grain or a wrap inside the loop - hands the old head a
 * ~7 ms fade-out while the new one fades in, instead of jumping. Feedback is
 * soft-clipped. State: f3/f4=head delay/speed, f1/f2=fading head, i1=samples left
 * in grain, i2=crossfade left, f5=smoothed loop length. */
#define COL_XF 300
static void fx_collage(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float loop=(s->sync_time>0.0f)? s->sync_time : SR*(0.05f+macro*1.2f);
    if(loop>(float)(MAX_DELAY-COL_XF*4)) loop=(float)(MAX_DELAY-COL_XF*4);
    if(s->f5<1.0f || s->f5>SR*1.3f) s->f5=loop;       /* init/repair smoothed loop time */
    float fb=amount*0.9f;
    for(int i=0;i<n;i++){
        s->f5 += clampf(0.0004f*(loop-s->f5), -0.5f, 0.5f);   /* glide loop time, slew-limited */
        float lps=s->f5;
        /* one crossfade at a time: starting another mid-fade would drop the fading head */
        if(s->i2<=0 && (s->i1<=0 || s->f3<1.0f || s->f3>lps)){
            s->f1=s->f3; s->f2=s->f4; s->i2=(s->f4!=0.0f)? COL_XF : 0;   /* old head fades
                                                         (f4==0 only before the first grain) */
            if(s->i1<=0){                              /* new grain */
                s->i1=(int)(SR*(0.04f+frand(&s->seed)*0.12f));
                s->f4=1.0f;
                if(drift>0.0f && frand(&s->seed)<drift*0.5f) s->f4=(frand(&s->seed)<0.5f?2.0f:-1.0f);
                s->f3=1.0f+frand(&s->seed)*(lps-2.0f);
            } else {                                   /* wrapped inside the loop */
                s->f3 += (s->f3<1.0f)? lps-1.0f : -(lps-1.0f);
                s->f3=clampf(s->f3,1.0f,lps);
            }
        }
        s->i1--;
        float hd=s->f3<1.0f? 1.0f : s->f3;            /* past the edge while a fade finishes */
        float gL=dlr(s->dl_l,s->wp,hd), gR=dlr(s->dl_r,s->wp,hd);
        s->f3 += 1.0f - s->f4;
        if(s->i2>0){                                   /* crossfade from the old head */
            float w=(float)s->i2/(float)COL_XF;
            float od=s->f1<1.0f? 1.0f : s->f1;
            gL=gL*(1.0f-w)+dlr(s->dl_l,s->wp,od)*w;
            gR=gR*(1.0f-w)+dlr(s->dl_r,s->wp,od)*w;
            s->f1 += 1.0f - s->f2; s->i2--;
        }
        s->dl_l[s->wp]=sb_tanh(l[i]+gL*fb); s->dl_r[s->wp]=sb_tanh(r[i]+gR*fb);
        s->wp=(s->wp+1)%MAX_DELAY;
        l[i]=lerpf(l[i],gL,amount*0.9f); r[i]=lerpf(r[i],gR,amount*0.9f);
    }
}

/* ── TEXTURE ───────────────────────────────────────────────────────────────── */

/* FILTER — single-knob DJ filter on resonant Cytomic/TPT SVFs (stable). amount
 * sweeps LP→through→HP (center = open/neutral, like a DJ isolator); macro = resonance;
 * drift = cutoff wander. A dedicated LP and a dedicated HP filter both run all the
 * time (each parked wide open on the other side), and the output blends to each one
 * continuously around the center. A single SVF switching between its LP and HP taps
 * at the center jumped in phase there - the loudest click in the audit.
 * State per ch: LP = lp_x and bp_x (ic1, ic2), HP = z1x and z2x; f1 = wander. */
static inline float svf_tpt(float x, float *ic1, float *ic2, float a1, float a2, float a3, float k, int hp){
    float v3=x-*ic2, v1=a1*(*ic1)+a2*v3, v2=*ic2+a2*(*ic1)+a3*v3;
    *ic1=2.0f*v1-*ic1+DENORM; *ic2=2.0f*v2-*ic2+DENORM;
    return hp? (x-k*v1-v2) : v2;
}
static void fx_filter(slot_dsp_t *s, float *l, float *r, int n,
                      float amount, float macro, float drift){
    float amt=clampf(amount,0.0f,1.0f);
    float wf=(drift>0.0f)? 1.0f+wander(&s->f1,&s->seed,0.0007f)*drift*0.3f : 1.0f;
    float fl=(amt<0.5f)? 200.0f*powf(90.0f, amt*2.0f) : 18000.0f;          /* LP 200 Hz .. 18 kHz */
    float fh=(amt>0.5f)? 20.0f*powf(20.0f,(amt-0.5f)*2.0f) : 20.0f;        /* HP 20 Hz .. 400 Hz */
    fl=clampf(fl*wf,20.0f,18000.0f); fh=clampf(fh*wf,15.0f,18000.0f);
    float wl=clampf((0.5f-amt)/0.05f,0.0f,1.0f);                          /* dry exactly at center */
    float wh=clampf((amt-0.5f)/0.05f,0.0f,1.0f);
    if(s->f4==0.0f){ s->f2=wl; s->f3=wh; s->f4=1.0f; }                     /* f2/f3 = last weights */
    float wl0=s->f2, wh0=s->f3, dwl=(wl-wl0)/(float)n, dwh=(wh-wh0)/(float)n;
    s->f2=wl; s->f3=wh;
    float k=1.0f-clampf(macro,0.0f,1.0f)*0.9f;                            /* resonance (1/Q) */
    float gl=tanf(3.14159265f*fl/SR), gh=tanf(3.14159265f*fh/SR);
    float l1=1.0f/(1.0f+gl*(gl+k)), l2=gl*l1, l3=gl*l2;
    float h1=1.0f/(1.0f+gh*(gh+k)), h2=gh*h1, h3=gh*h2;
    for(int ch=0;ch<2;ch++){
        float *src=ch?r:l;
        float *lc1=ch?&s->lp_r:&s->lp_l, *lc2=ch?&s->bp_r:&s->bp_l;
        float *hc1=ch?&s->z1r:&s->z1l,   *hc2=ch?&s->z2r:&s->z2l;
        for(int i=0;i<n;i++){
            float x=src[i];
            float lp=svf_tpt(x,lc1,lc2,l1,l2,l3,k,0);
            float hp=svf_tpt(x,hc1,hc2,h1,h2,h3,k,1);
            float a=(float)(i+1);
            src[i]=x+(lp-x)*(wl0+dwl*a)+(hp-x)*(wh0+dwh*a);
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
    float At=clampf(amount,0.0f,1.0f), B=0.4f;         /* B = release speed (fixed, musical) */
    if(s->z1l<=0.0f) s->z1l=At+1e-6f;                    /* z1l = per-sample Amount (makeup is up to 20x) */
    float release=powf(1.28f-B,5.0f)*32768.0f;          /* overallscale = 1 @ 44.1k */
    float fastest=sqrtf(release);
    float mewiness=(macro*2.0f)-1.0f, unmew;
    int positivemu; if(mewiness>=0){positivemu=1;unmew=1.0f-mewiness;}
                    else{positivemu=0;mewiness=-mewiness;unmew=1.0f-mewiness;}
    float outGain=1.0f;
    for(int i=0;i<n;i++){
        s->z1l += 0.01f*(At+1e-6f - s->z1l);
        float A=s->z1l;
        float threshold=1.0f-(A*0.95f);
        float muMakeupGain=1.0f/threshold;
        float comp=0.5f+0.5f*threshold;                  /* gain-comp: cancels the 1/threshold makeup */
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
        l[i]=lerpf(l[i],xl*comp,A); r[i]=lerpf(r[i],xr*comp,A); /* level-matched; amount=0 → dry */
        s->i1^=1;
    }
}

/* CASSETTE — worn cassette: WOW (slow sine) + FLUTTER (Airwindows ToTape6-style
 * random-walk pitch wobble) + tape COMPRESSION + mello tape saturation + HF roll.
 * Minimal hiss. amount=degrade intensity, macro=tone, drift=warble depth.
 * State: lfo=wow phase; f2=flutter rate, f3=flutter sweep, f4=flutter target;
 * z1=tone LP; env=compressor follower; base delay. */
static void fx_cassette(slot_dsp_t *s, float *l, float *r, int n,
                        float amount, float macro, float drift){
    float wow_depth =(0.4f+amount*1.2f)*(1.0f+drift*1.2f); /* samples of wow */
    float flut_depth=(0.3f+amount*0.9f)*(1.0f+drift*1.6f); /* samples of flutter */
    float roll=0.06f+(1.0f-macro)*0.5f;              /* tone: darker at low macro */
    float hiss=amount*0.0005f;                        /* tape hiss (see below) */
    float base=SR*0.006f;
    float sat_d=1.0f+amount*0.7f, asym=0.10f*amount;
    float comp_amt=amount*0.6f;                       /* tape compression depth */
    for(int i=0;i<n;i++){
        s->lfo+=0.55f/SR; if(s->lfo>=1.0f)s->lfo-=1.0f;            /* wow ~0.55 Hz */
        /* ToTape6 random-walk flutter: rate eases toward a new random target */
        s->f2 = s->f2*0.9985f + s->f4*0.0015f;
        s->f3 += (5.0f + s->f2*8.0f)/SR;
        if(s->f3>=1.0f){ s->f3-=1.0f; s->f4=0.24f+frand(&s->seed)*0.74f; }
        float d=base + wow_depth*sinf(s->lfo*TWO_PI) + flut_depth*sinf(s->f3*TWO_PI);
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        float xL=dlr(s->dl_l,s->wp,d), xR=dlr(s->dl_r,s->wp,d);
        s->wp=(s->wp+1)%MAX_DELAY;
        /* tape compression (gentle, program-dependent gain reduction) */
        float mono=0.5f*(fabsf(xL)+fabsf(xR));
        s->env+=(mono>s->env?0.05f:0.002f)*(mono-s->env)+DENORM;
        float gr=1.0f/(1.0f+s->env*comp_amt*2.0f);
        xL*=gr; xR*=gr;
        /* mello tape sat + HF rolloff */
        xL=tape_asym(tape_cubic(xL,sat_d),1.0f,asym);
        xR=tape_asym(tape_cubic(xR,sat_d),1.0f,asym);
        s->z1l+=roll*(xL-s->z1l)+DENORM; s->z1r+=roll*(xR-s->z1r)+DENORM;
        /* hiss rides the smoothed level (env), low-passed ~6 kHz. It was switched per
         * sample on the instantaneous level, so it chattered at every zero crossing. */
        float hg=hiss*clampf(s->env*30.0f,0.0f,1.0f);
        s->z2l += 0.575f*(hg*((frand(&s->seed)-0.5f)*2.0f)-s->z2l) + DENORM;
        s->z2r += 0.575f*(hg*((frand(&s->seed)-0.5f)*2.0f)-s->z2r) + DENORM;
        l[i]=lerpf(l[i],s->z1l+s->z2l,amount); r[i]=lerpf(r[i],s->z1r+s->z2r,amount);
    }
}

/* BROKEN — motor-failure pitch drops + AM/FM wobble + dropouts. amount=breakdown,
 * macro=rate of failures, drift=dropout randomness. */
static void fx_broken(slot_dsp_t *s, float *l, float *r, int n,
                      float amount, float macro, float drift){
    float rate=0.1f+macro*macro*3.0f;                /* motor failure LFO Hz */
    for(int i=0;i<n;i++){
        /* Amount and Drift scale the READ DELAY: glide them per sample, or a knob jump
         * moves the read head tens of samples at once */
        s->f5 += 0.002f*(amount-s->f5); s->f6 += 0.002f*(drift-s->f6);
        float amt=s->f5, drf=s->f6;
        s->lfo+=rate/SR; if(s->lfo>=1.0f)s->lfo-=1.0f;
        s->lfo2+=rate*1.7f/SR; if(s->lfo2>=1.0f)s->lfo2-=1.0f;   /* own phase: x1.7 of a
                                     * wrapping phase jumped the read head ~60 samples */
        /* periodic motor stall: read offset is ~0 at rest (so wet≈dry, NO doubling)
         * and grows during the dip (pitch drops, then recovers each cycle). */
        float dip=amt*(0.5f-0.5f*cosf(s->lfo*TWO_PI));
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        float rd=3.0f + dip*SR*0.03f                  /* baseline ~0 + up to ~30 ms drop */
               + (drf>0.0f? (0.5f+0.5f*sinf(s->lfo2*TWO_PI))*drf*SR*0.003f : 0.0f);
        float xL=dlr(s->dl_l,s->wp,rd), xR=dlr(s->dl_r,s->wp,rd);
        s->wp=(s->wp+1)%MAX_DELAY;
        /* DRIFT = occasional SMOOTHED dropouts (gate ramps → no clicks) + the warble above */
        if(drift>0.0f && frand(&s->seed)<drift*0.0004f) s->i1=(int)(SR*0.05f*frand(&s->seed));
        float gtarget=1.0f; if(s->i1>0){ s->i1--; gtarget=0.0f; }
        s->f4 += 0.008f*(gtarget - s->f4) + DENORM;   /* two-pole ~5 ms gate: a one-pole */
        s->f3 += 0.008f*(s->f4    - s->f3) + DENORM;   /* starts with a corner (a tick)   */
        float am=1.0f-amt*0.3f*(0.5f-0.5f*cosf(s->lfo*TWO_PI*2.0f));      /* AM wobble */
        l[i]=lerpf(l[i],xL*s->f3*am,amount); r[i]=lerpf(r[i],xR*s->f3*am,amount);
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
    /* crush scales WITH amount (was inverted → heavy crush + noise at low amounts).
     * targetA = sample-rate increment: ~1 (clean) at A=0 → 0.03 (heavy SR reduction) at A=1.
     * targetB = bit-quant step: 0 (no quantize) at A=0 → coarse at A=1. */
    float targetA=1.0f - A*0.92f; if(targetA<0.08f)targetA=0.08f;   /* tamed (Loopex) */
    float soften=(1.0f+targetA)/2.0f;
    float targetB=A*A*0.22f;                          /* bit-depth derez (scales up with amount) */
    float hard=0.6f;                                  /* more dry blend = less harsh */
    float carr=200.0f+macro*macro*3000.0f;
    const float L256=logf(256.0f);
    for(int i=0;i<n;i++){
        s->f2=((s->f2*999.0f)+targetA)/1000.0f;        /* incrementA (rate) */
        s->f3=((s->f3*999.0f)+targetB)/1000.0f;        /* incrementB (bits) */
        s->f1+=s->f2;                                  /* position */
        float in[2]={l[i],r[i]};
        { float m=0.5f*(fabsf(in[0])+fabsf(in[1])); s->env+=(m>s->env?0.01f:0.00008f)*(m-s->env)+DENORM; }
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
            if(s->f3>0.0005f)                           /* bit-depth quantize (O(1)), ROUNDED: */
                x=floorf(x/s->f3+0.5f)*s->f3;           /* ceil-away-from-0 blew any hiss up to a full step */
            x=lerpf(x, x*sinf(s->lfo*TWO_PI), macro*0.3f);  /* ring-mod radio */
            /* static bursts ride the input level (fade ~0.3 s after it stops) and are
             * ~12 dB lower: at -29 dBFS they crackled even with no signal at all */
            if(drift>0.0f && frand(&s->seed)<drift*0.04f) x+=(frand(&s->seed)-0.5f)*drift*0.25f*clampf(s->env*8.0f,0.0f,1.0f);
            out[c]=x;
        }
        s->z3l=in[0]; s->z3r=in[1];                    /* lastDry = dry input */
        s->z4l=lastOut[0]; s->z4r=lastOut[1];
        s->lfo+=carr/SR; if(s->lfo>=1.0f)s->lfo-=1.0f;
        l[i]=lerpf(in[0], out[0]*0.6f, amount);        /* amount=0 → dry; trim µ-law boost */
        r[i]=lerpf(in[1], out[1]*0.6f, amount);
    }
}

/* HALO — ethereal harmonic resonator pad (Walrus Qi / OBNE Dark-Star vibe).
 * A 6-voice Karplus-Strong / tuned-comb bank: the chain audio sympathetically
 * excites delay lines tuned to a chord (root, 5th, oct, +oct-3rd, +oct-5th, 2-oct),
 * each with a damping one-pole and soft-clipped feedback so they ring/bloom into a
 * frozen harmonic drone. Pure C (replaces the FFT FREEZE). amount = resonance/
 * sustain (feedback), macro = root pitch + brightness, drift = per-voice detune.
 * Comb buffers reuse dl_l/dl_r (6 regions × 1024); shared region index = i1;
 * per-voice damping = halo_lp_l/r; drift walk = f1. */
#define HALO_VLEN 1024
static void fx_halo(slot_dsp_t *s, float *l, float *r, int n,
                    float amount, float macro, float drift){
    static const float ratio[6]={1.0f,1.49831f,2.0f,2.51984f,2.99661f,4.0f}; /* R 5 8ve +M3 +5 +2-8ve */
    float rtarget=55.0f*powf(2.0f,macro*2.0f);       /* 55..220 Hz root (A1..A3) */
    if(s->f2<=0.0f) s->f2=rtarget;
    float fb=0.90f+amount*0.099f;                     /* 0.90..0.999 ring time */
    float damp=0.12f+macro*0.55f;                     /* brighter as macro rises */
    float exc=amount*0.5f;                            /* excitation into the bank */
    for(int i=0;i<n;i++){
        float drf=(drift>0.0f)? wander(&s->f1,&s->seed,0.0006f)*drift*0.012f : 0.0f;
        s->f2 += 0.0015f*(rtarget-s->f2);              /* root glides: comb taps never jump */
        float root=s->f2;
        for(int ch=0;ch<2;ch++){
            float *buf=ch?s->dl_r:s->dl_l;
            float *lp =ch?s->halo_lp_r:s->halo_lp_l;
            float in=(ch?r:l)[i];
            float sum=0.0f;
            for(int v=0;v<6;v++){
                float dl=(SR/(root*ratio[v]))*(1.0f+drf*(float)v);
                if(dl>HALO_VLEN-2) dl=HALO_VLEN-2; if(dl<2.0f) dl=2.0f;
                float *vb=buf+v*HALO_VLEN;
                float rp=(float)s->i1-dl; while(rp<0.0f) rp+=HALO_VLEN;
                int i0=(int)rp; float fr=rp-(float)i0; int i1n=i0+1; if(i1n>=HALO_VLEN)i1n=0;
                float y=vb[i0]+(vb[i1n]-vb[i0])*fr;
                lp[v]+=damp*(y-lp[v])+DENORM;          /* loop damping */
                vb[s->i1]=in*exc + sb_tanh(lp[v]*fb);  /* excite + soft-clipped regen */
                sum+=y;
            }
            (ch?r:l)[i]=lerpf(in, sum*0.32f, amount);  /* amount=0 → dry */
        }
        s->i1++; if(s->i1>=HALO_VLEN) s->i1=0;
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
    [PFX_HALO]         = { fx_halo,          NULL },  /* pure-C resonator pad (replaces FFT Freeze) */
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
/* The single path for ALL select changes (manual scroll, direct set, randomizer,
 * preset load). Only sets the TARGET: the audio thread fades the running effect out
 * to dry and then calls slot_engage() (see process_block), so switching never cuts
 * the old effect off mid-waveform. Preset load / randomize run inside the duck's
 * silence and engage at once (slots_engage_now). */
static void slot_apply_select(palette_t *p, int slot, int landed){
    slot_t *sl=&p->slots[slot];
    if(landed == sl->select) return;
    sl->prev_select = sl->select;
    sl->select = landed;
}
/* Swap the DSP over to `select`: reference the pre-allocated Clouds pool (no runtime
 * alloc), reset it fresh so no tail carries in, clear the scratch, arm the fade-in. */
static void slot_engage(palette_t *p, int slot){
    slot_t *sl=&p->slots[slot];
    int landed=sl->select;
    void *want = (landed==PFX_SPACE)?p->space_pool : (landed==PFX_BLOOM)?p->bloom_pool : NULL;
    if(want) pfx_clouds_reset(want);
    sl->dsp.heavy=want; sl->dsp.heavy_kind = want ? landed : 0;
    slot_reset(sl);                  /* clear scratch (heavy preserved) */
    sl->active = landed;
    sl->ramp  = 0.0f;                /* new effect fades in from dry */
    sl->xgain = 1.0f;
    sl->gate = 1.0f; sl->genv = 0.0f;
}
/* A Clouds pool is shared by whichever slot holds it: don't engage it while another
 * slot is still fading the same engine out (a few ms). */
static int heavy_busy(palette_t *p, int slot, int fx){
    if(!is_clouds_fx(fx)) return 0;
    for(int i=0;i<NUM_SLOTS;i++) if(i!=slot && p->slots[i].active==fx) return 1;
    return 0;
}
/* Engage every pending slot immediately — only when the output is silent (duck). */
static void slots_engage_now(palette_t *p){
    for(int s=0;s<NUM_SLOTS;s++) if(p->slots[s].active!=p->slots[s].select) p->slots[s].active=PFX_OFF;
    for(int s=0;s<NUM_SLOTS;s++) if(p->slots[s].active!=p->slots[s].select) slot_engage(p,s);
}
static void set_slot_select(palette_t *p, int slot, int requested, int dir){
    slot_apply_select(p, slot, resolve_select(p,slot,requested,dir));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  RANDOMIZERS  (design-spec §4) — all across the 4 slots at once.
 *  Effect picks are WITHOUT REPLACEMENT (distinct), bypassing the skip-walk.
 * ═══════════════════════════════════════════════════════════════════════════ */
static int is_drive_fx(int fx){   /* drive/distortion set — capped at 2 per patch */
    return fx==PFX_DRIVE||fx==PFX_FUZZ||fx==PFX_HOWL||fx==PFX_FOLD||fx==PFX_SQUASH;
}
static int is_harsh_fx(int fx){   /* screech/destruction — skew amount lower */
    return fx==PFX_FUZZ||fx==PFX_HOWL||fx==PFX_FOLD||fx==PFX_BROKEN||fx==PFX_INTERFERENCE;
}
/* Clear the global feedback loop so a hot / self-oscillating tail doesn't carry into
 * a freshly randomized patch or loaded preset. */
static void clear_feedback(palette_t *p){
    p->fb_lp_l=p->fb_lp_r=p->fb_hp_l=p->fb_hp_r=0.0f;
    for(int i=0;i<MAXFRAMES;i++){ p->fbL[i]=0.0f; p->fbR[i]=0.0f; }
}
/* Category-weighted patch picker: ~70% "balanced" (one distinct effect from each of
 * the 4 groups, random slot order — the Chroma feel), ~30% fully free distinct picks
 * (weird stacks). Hard rule either way: never more than TWO drive/distortion effects. */
static void pick_patch(palette_t *p, uint32_t *rng){
    int picks[NUM_SLOTS];
    if(frand(rng) < 0.70f){
        static const int GSTART[4]={PFX_DRIVE,PFX_DOUBLER,PFX_CASCADE,PFX_FILTER};
        for(int g=0;g<4;g++) picks[g]=GSTART[g]+rnd_int(rng,6);   /* one per category (6 each) */
        for(int i=3;i>0;i--){ int j=rnd_int(rng,i+1); int t=picks[i];picks[i]=picks[j];picks[j]=t; }
    } else {
        int pool[NUM_FX]; for(int i=0;i<NUM_FX;i++) pool[i]=i+1;
        for(int i=NUM_FX-1;i>0;i--){ int j=rnd_int(rng,i+1); int t=pool[i];pool[i]=pool[j];pool[j]=t; }
        for(int s=0;s<NUM_SLOTS;s++) picks[s]=pool[s];
    }
    /* enforce <=2 drives: replace extras with a distinct non-drive effect */
    for(int guard=0; guard<32; guard++){
        int drives=0; for(int s=0;s<NUM_SLOTS;s++) if(is_drive_fx(picks[s])) drives++;
        if(drives<=2) break;
        for(int s=0;s<NUM_SLOTS;s++){
            if(is_drive_fx(picks[s])){
                int repl,dup; do { repl=1+rnd_int(rng,NUM_FX); dup=0;
                    for(int q=0;q<NUM_SLOTS;q++) if(q!=s && picks[q]==repl) dup=1;
                } while(is_drive_fx(repl) || dup);
                picks[s]=repl; break;
            }
        }
    }
    for(int s=0;s<NUM_SLOTS;s++) slot_apply_select(p,s,PFX_OFF);
    for(int s=0;s<NUM_SLOTS;s++) slot_apply_select(p,s,picks[s]);
}
/* Weighted per-slot params: amounts present but rarely maxed, harsher effects tamed,
 * drift skewed low (tasteful instability). */
static void rnd_slot_params(palette_t *p, int s){
    float amt=0.30f+0.60f*frand(&p->rng);            /* 0.30..0.90 */
    if(is_harsh_fx(p->slots[s].select)) amt*=0.6f;   /* keep screech in check */
    p->slots[s].amount=amt;
    p->slots[s].macro =frand(&p->rng);
    p->slots[s].drift =frand(&p->rng)*frand(&p->rng)*0.7f;   /* skew low */
}
static void rnd_patch(palette_t *p){                 /* effects + weighted params */
    pick_patch(p,&p->rng);
    for(int s=0;s<NUM_SLOTS;s++) rnd_slot_params(p,s);
}
static void rnd_effect(palette_t *p){ pick_patch(p,&p->rng); }   /* effects only, keep params */
static void rnd_amount(palette_t *p){ for(int s=0;s<NUM_SLOTS;s++){
    float a=0.30f+0.60f*frand(&p->rng); if(is_harsh_fx(p->slots[s].select))a*=0.6f; p->slots[s].amount=a; } }
static void rnd_macro (palette_t *p){ for(int s=0;s<NUM_SLOTS;s++) p->slots[s].macro =frand(&p->rng); }
/* Rnd Values — randomize Amount+Macro+Drift on every slot, KEEPING the current
 * effects (the "tweak the random chain" workflow). */
static void rnd_values(palette_t *p){ for(int s=0;s<NUM_SLOTS;s++) rnd_slot_params(p,s); }

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

#define NUM_PRESETS 50
static const preset_t PRESETS[NUM_PRESETS] = {
{"Init",           {PFX_DRIVE,PFX_DOUBLER,PFX_CASCADE,PFX_FILTER},    {0,0,0,50}, {50,40,45,30}, {0,0,0,0}, 100,100,0},
{"Delay + Verb",   {PFX_CASCADE,PFX_SPACE,PFX_VIBRATO,PFX_HALO},    {50,45,0,0}, {45,55,50,50}, {12,8,0,0}, 100,100,0},
{"Tape Echo",      {PFX_REELS,PFX_SHIFT,PFX_SWELL,PFX_COLLAGE},       {50,0,0,0}, {45,50,50,50}, {18,0,0,0}, 100,100,0},
{"Concert Hall",   {PFX_SPACE,PFX_CASSETTE,PFX_HOWL,PFX_PITCH},       {55,0,0,0}, {70,50,50,50}, {8,0,0,0}, 100,100,0},
{"Plate Shimmer",  {PFX_BLOOM,PFX_SQUASH,PFX_BROKEN,PFX_INTERFERENCE},{50,0,0,0}, {55,50,50,50}, {10,0,0,0}, 100,100,0},
{"Doubler Width",  {PFX_DOUBLER,PFX_TREMOLO,PFX_SPACE,PFX_REELS},     {55,0,0,0}, {40,50,50,50}, {12,0,0,0}, 100,100,0},
{"Slapback",       {PFX_DOUBLER,PFX_CASCADE,PFX_HALO,PFX_PHASER},   {50,35,0,0}, {35,30,50,50}, {10,10,0,0}, 100,100,0},
{"Warm Drive",     {PFX_DRIVE,PFX_SWELL,PFX_COLLAGE,PFX_FUZZ},        {45,0,0,0}, {55,50,50,50}, {8,0,0,0}, 100,100,0},
{"Console Glue",   {PFX_SWEETEN,PFX_SQUASH,PFX_HOWL,PFX_PITCH},       {45,40,0,0}, {55,50,50,50}, {6,8,0,0}, 100,100,0},
{"Vibrato Verb",   {PFX_VIBRATO,PFX_SPACE,PFX_BROKEN,PFX_INTERFERENCE},{35,45,0,0}, {35,60,50,50}, {15,8,0,0}, 100,100,0},
{"Tape Slap",      {PFX_CASSETTE,PFX_CASCADE,PFX_TREMOLO,PFX_SPACE},  {45,40,0,0}, {45,35,50,50}, {20,10,0,0}, 100,100,0},
{"LP Sweep",       {PFX_FILTER,PFX_PHASER,PFX_BLOOM,PFX_CASCADE},     {28,0,0,0}, {45,50,50,50}, {6,0,0,0}, 100,100,0},
{"HP Air",         {PFX_FILTER,PFX_SPACE,PFX_COLLAGE,PFX_FUZZ},       {72,40,0,0}, {35,55,50,50}, {6,8,0,0}, 100,100,0},
{"Tremolo Pan",    {PFX_TREMOLO,PFX_PITCH,PFX_SWEETEN,PFX_FILTER},    {45,0,0,50}, {45,50,50,30}, {60,0,0,0}, 100,100,0},
{"Octave Up",      {PFX_PITCH,PFX_SPACE,PFX_INTERFERENCE,PFX_FOLD},   {45,45,0,0}, {75,55,50,50}, {10,8,0,0}, 100,100,0},
{"Phaser Verb",    {PFX_PHASER,PFX_SPACE,PFX_REELS,PFX_VIBRATO},      {45,45,0,0}, {45,55,50,50}, {12,8,0,0}, 100,100,0},
{"Fuzz Phaze Vrb", {PFX_FUZZ,PFX_PHASER,PFX_SPACE,PFX_BLOOM},         {50,50,40,0}, {45,50,55,50}, {15,12,8,0}, 100,95,0},
{"West Coast",     {PFX_FOLD,PFX_FILTER,PFX_SPACE,PFX_FUZZ},          {50,35,40,0}, {50,50,55,50}, {12,8,8,0}, 100,95,0},
{"Howl Stab",      {PFX_HOWL,PFX_REELS,PFX_SWEETEN,PFX_FILTER},       {55,40,0,50}, {50,45,50,30}, {18,12,0,0}, 100,95,0},
{"Shimmer Drive",  {PFX_DRIVE,PFX_BLOOM,PFX_FOLD,PFX_DOUBLER},        {40,55,0,0}, {55,70,50,50}, {8,12,0,0}, 100,100,0},
{"Swell Pad",      {PFX_SWELL,PFX_SPACE,PFX_BLOOM,PFX_REELS},         {60,45,45,0}, {50,65,65,50}, {8,8,12,0}, 100,100,0},
{"Pitch Cloud",    {PFX_PITCH,PFX_COLLAGE,PFX_SPACE,PFX_CASCADE},     {45,40,45,0}, {65,50,60,50}, {12,20,8,0}, 100,100,0},
{"Lo-fi Tape",     {PFX_CASSETTE,PFX_REELS,PFX_REVERSE,PFX_HOWL},     {55,45,0,0}, {40,50,50,50}, {30,18,0,0}, 100,100,0},
{"Squash Drive",   {PFX_SQUASH,PFX_DRIVE,PFX_FILTER,PFX_BROKEN},      {55,40,50,0}, {50,55,30,50}, {10,8,0,0}, 100,100,0},
{"Reverse Bloom",  {PFX_REVERSE,PFX_BLOOM,PFX_DRIVE,PFX_DOUBLER},     {50,50,0,0}, {45,65,50,50}, {12,12,0,0}, 100,100,0},
{"Fold Space",     {PFX_FOLD,PFX_SPACE,PFX_VIBRATO,PFX_HALO},       {45,50,0,0}, {55,60,50,50}, {12,8,0,0}, 100,95,0},
{"Shift Bloom",    {PFX_SHIFT,PFX_BLOOM,PFX_SWELL,PFX_COLLAGE},       {40,55,0,0}, {58,70,50,50}, {12,12,0,0}, 100,100,0},
{"Dub Filter",     {PFX_FILTER,PFX_REELS,PFX_CASSETTE,PFX_HOWL},      {30,50,0,0}, {55,55,50,50}, {8,20,0,0}, 100,100,0},
{"Ambient Swell",  {PFX_SWELL,PFX_BLOOM,PFX_SQUASH,PFX_BROKEN},       {65,55,0,0}, {50,72,50,50}, {8,15,0,0}, 100,100,0},
{"Vibe Drive Vrb", {PFX_VIBRATO,PFX_DRIVE,PFX_SPACE,PFX_DOUBLER},     {35,40,45,0}, {35,55,55,50}, {15,8,8,0}, 100,100,0},
{"Tape Phaser",    {PFX_PHASER,PFX_CASSETTE,PFX_HALO,PFX_BLOOM},    {50,45,0,0}, {45,45,50,50}, {12,22,0,0}, 100,100,0},
{"Crush Verb",     {PFX_INTERFERENCE,PFX_SPACE,PFX_SWELL,PFX_COLLAGE},{40,50,0,0}, {45,60,50,50}, {18,8,0,0}, 100,100,0},
{"Grain Hall",     {PFX_COLLAGE,PFX_SPACE,PFX_HOWL,PFX_PITCH},        {45,55,0,0}, {50,65,50,50}, {20,8,0,0}, 100,100,0},
{"Octave Drive",   {PFX_SHIFT,PFX_DRIVE,PFX_SPACE,PFX_BROKEN},        {35,40,45,0}, {60,55,55,50}, {10,8,8,0}, 100,95,0},
{"Broken Radio",   {PFX_BROKEN,PFX_INTERFERENCE,PFX_TREMOLO,PFX_SPACE},{45,45,0,0}, {40,45,50,50}, {25,30,0,0}, 95,100,0},
{"Halo Reels",   {PFX_HALO,PFX_REELS,PFX_PHASER,PFX_BLOOM},       {60,40,0,0}, {50,45,50,50}, {20,18,0,0}, 100,100,0},
{"Glitch Cloud",   {PFX_COLLAGE,PFX_HALO,PFX_SPACE,PFX_FUZZ},       {50,50,45,0}, {45,55,60,50}, {28,18,8,0}, 100,100,0},
{"Intf Halo",    {PFX_INTERFERENCE,PFX_HALO,PFX_PITCH,PFX_SWEETEN},{45,55,0,0}, {45,55,50,50}, {28,18,0,0}, 100,100,0},
{"Reverse Shift",  {PFX_REVERSE,PFX_SHIFT,PFX_INTERFERENCE,PFX_FOLD}, {50,40,0,0}, {45,60,50,50}, {15,15,0,0}, 100,100,0},
{"Fold Fuzz",      {PFX_FOLD,PFX_FUZZ,PFX_SPACE,PFX_REELS},           {45,40,0,0}, {55,45,50,50}, {15,18,0,0}, 100,90,0},
{"Broken Tape",    {PFX_BROKEN,PFX_CASSETTE,PFX_BLOOM,PFX_CASCADE},   {45,50,0,0}, {40,45,50,50}, {25,30,0,0}, 100,100,0},
{"Crush Bloom",    {PFX_INTERFERENCE,PFX_BLOOM,PFX_FUZZ,PFX_REVERSE}, {40,55,0,0}, {50,70,50,50}, {20,15,0,0}, 100,100,0},
{"Stutter Verb",   {PFX_COLLAGE,PFX_REVERSE,PFX_SPACE,PFX_SWEETEN},   {50,45,45,0}, {45,50,60,50}, {28,15,8,0}, 100,100,0},
{"Howl Halo",    {PFX_HOWL,PFX_HALO,PFX_FOLD,PFX_DRIVE},          {50,55,0,0}, {55,50,50,50}, {20,18,0,0}, 100,95,0},
{"Warble Shift",   {PFX_CASSETTE,PFX_SHIFT,PFX_REELS,PFX_VIBRATO},    {55,40,0,0}, {45,58,50,50}, {35,15,0,0}, 100,100,0},
{"Ghost Pitch",    {PFX_PITCH,PFX_HALO,PFX_BLOOM,PFX_CASCADE},      {45,55,50,0}, {70,50,70,50}, {15,18,12,0}, 100,100,0},
{"Full Chain",     {PFX_DRIVE,PFX_PHASER,PFX_REELS,PFX_SPACE},        {40,45,45,40}, {50,50,45,55}, {8,12,12,8}, 100,95,0},
{"Ambient Wash",   {PFX_SWELL,PFX_SHIFT,PFX_BLOOM,PFX_HALO},        {60,40,55,45}, {50,60,70,55}, {8,12,12,18}, 100,100,0},
{"Chaos Engine",   {PFX_FOLD,PFX_BROKEN,PFX_COLLAGE,PFX_HALO},      {45,45,50,50}, {50,45,50,55}, {15,25,25,18}, 95,90,0},
{"Total Destroy",  {PFX_FUZZ,PFX_INTERFERENCE,PFX_BROKEN,PFX_HALO}, {50,45,45,50}, {45,45,40,55}, {20,28,28,20}, 90,90,0},
};

/* Apply a factory preset: heavy-aware select for all 4 slots, then snap params. */
static void load_preset(palette_t *p, int idx){
    idx=clampi(idx,1,NUM_PRESETS);
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
    p->order_cur=p->fx_reorder;          /* silent here (duck/create): no reorder dip */
    p->current_preset=idx;
    /* runs only in silence (duck bottom / create): swap every slot now and reset its
     * buffers fresh, even if the effect is unchanged */
    for(int s=0;s<NUM_SLOTS;s++) p->slots[s].active=PFX_OFF;
    for(int s=0;s<NUM_SLOTS;s++) if(p->slots[s].select!=PFX_OFF) slot_engage(p,s);
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
    p->feedback = 0.0f; p->fb_sm = 0.0f;        /* GLOBAL defaults */
    p->tempo_src = 0;                           /* 0 = Move clock */
    p->tempo_bpm = 120;
    p->time_div = 0;                            /* Free */
    p->move_bpm = 120.0f; p->clock_interval_sm = (60.0f*SR)/(120.0f*24.0f);
    p->fx_reorder = 0;                          /* 1-2-3-4 */
    p->order_cur = 0; p->ro_gain = 1.0f;
    p->current_preset = 1;
    p->current_level = 1;                       /* LV_PALETTE — landing mirrors Console knobs */
    seed_urandom(&p->rng, &p->ent);             /* true random from the first tap */
    p->pending_rnd=0; p->pending_preset=-1; p->rnd_gain=1.0f; p->rnd_phase=0; p->rnd_hold=0;
    fold_adaa_init();                                  /* FOLD's ADAA integral table */
    p->space_pool = pfx_clouds_alloc(PFX_SPACE, SR);   /* pre-allocate heavy engines */
    p->bloom_pool = pfx_clouds_alloc(PFX_BLOOM, SR);   /* (NULL-safe: slots passthrough) */
    load_preset(p, 1);                          /* start on Init (Drive→Doubler→Cascade→Filter) */
    if(g_host && g_host->log) g_host->log("[palette] instance created");
    return p;
}
static void destroy_instance(void *instance){
    palette_t *p=(palette_t*)instance; if(!p) return;
    for(int s=0;s<NUM_SLOTS;s++){
        free(p->slots[s].dsp.dl_l); free(p->slots[s].dsp.dl_r);   /* heavy is a shared pool */
    }
    if(p->space_pool) pfx_clouds_free(p->space_pool);
    if(p->bloom_pool) pfx_clouds_free(p->bloom_pool);
    free(p);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PARAMS  (page-aware knob overlay + stable keys + chain_params)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Levels for page-aware knob overlay. Index 0 = root/Presets (landing). */
/* Knob-overlay pages. 0 = root/landing (preset+rnd knobs), 1 = Console (the
 * 8-knob amount/macro page), 2/3 = FX slot pages. */
enum { LV_PRESETS=0, LV_PALETTE, LV_FX12, LV_FX34, LV_GLOBAL, NUM_LEVELS };

/* knob (1..8) → param key, per level. NULL = unused knob. */
static const char *LEVEL_KNOBS[NUM_LEVELS][8] = {
  /* Presets */ {"current_preset","rnd_patch","rnd_effect","rnd_amount","rnd_macro","rnd_values","input_vol","mix"},
  /* PALETTE */ {"fx1_amount","fx1_macro","fx2_amount","fx2_macro","fx3_amount","fx3_macro","fx4_amount","fx4_macro"},
  /* FX12    */ {"fx1_select","fx1_amount","fx1_macro","fx1_drift","fx2_select","fx2_amount","fx2_macro","fx2_drift"},
  /* FX34    */ {"fx3_select","fx3_amount","fx3_macro","fx3_drift","fx4_select","fx4_amount","fx4_macro","fx4_drift"},
  /* GLOBAL  */ {"feedback","tempo_src","tempo_bpm","time_div",NULL,NULL,NULL,NULL},
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
    else if(!strcmp(key,"feedback")) p->feedback=clampf(v,0,1);
}
static float get_float_key(palette_t *p, const char *key){
    int s=slot_of(key);
    if(s>=0){ const char *f=key+4;
        if(!strcmp(f,"amount")) return p->slots[s].amount;
        if(!strcmp(f,"macro"))  return p->slots[s].macro;
        if(!strcmp(f,"drift"))  return p->slots[s].drift; }
    if(!strcmp(key,"mix")) return p->mix;
    if(!strcmp(key,"input_vol")) return p->input_vol;
    if(!strcmp(key,"feedback")) return p->feedback;
    return 0;
}

/* Applied from the audio thread at the silent bottom of the duck: the heavy Clouds
 * swap (if any) happens here, unheard. Preset first, then the pending randomize. */
static void apply_pending(palette_t *p){
    int had_preset = p->pending_preset>=0;
    if(had_preset){ load_preset(p, p->pending_preset); p->pending_preset=-1; }
    int r=p->pending_rnd; p->pending_rnd=0;
    if(r==1) rnd_patch(p); else if(r==2) rnd_effect(p);
    slots_engage_now(p);                                 /* we're at the duck's silence */
    if(had_preset || r==1 || r==2) clear_feedback(p);   /* patch-level -> reset feedback loop */
}

static void fire_trigger(palette_t *p, const char *key){
    /* fold audio entropy + exact tap timing into the RNG (extra entropy on top of
     * the urandom seed) so every press is genuinely random. */
    p->rng ^= p->ent + p->sample_pos*2654435761u + 0x9e3779b9u;
    p->rng = p->rng*1664525u + 1013904223u;
    if(p->rng==0u) p->rng=0x9e3779b9u;
    /* param-only randomizers are already 15 ms-smoothed -> apply instantly, no duck */
    if(!strcmp(key,"rnd_amount")){ rnd_amount(p); return; }
    if(!strcmp(key,"rnd_macro")){  rnd_macro(p);  return; }
    if(!strcmp(key,"rnd_values")){ rnd_values(p); return; }
    /* effect-changing randomizers duck (fade out -> swap at silence -> fade in) */
    int which = !strcmp(key,"rnd_patch") ? 1 : !strcmp(key,"rnd_effect") ? 2 : 0;
    if(!which || p->rnd_phase!=0) return;   /* ignore re-fire while a duck runs */
    p->pending_rnd=which; p->rnd_phase=1;
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
        int idx=clampi(p->current_preset+delta,1,NUM_PRESETS);
        p->current_preset=idx; p->pending_preset=idx;      /* display now, apply at duck silence */
        if(p->rnd_phase==0||p->rnd_phase==2) p->rnd_phase=1;
        return;
    }
    if(!strncmp(key,"rnd_",4)){ return; }  /* momentary enum: fires via direct set_param("1") only */
    /* GLOBAL discrete params */
    if(!strcmp(key,"tempo_src")){ p->tempo_src = (delta>0)?1:(delta<0?0:p->tempo_src); return; }
    if(!strcmp(key,"tempo_bpm")){ p->tempo_bpm = clampi(p->tempo_bpm+delta,10,500); return; }
    if(!strcmp(key,"time_div")){ p->time_div = clampi(p->time_div+delta,0,NUM_DIVS-1); return; }
    /* floats — accelerated: fine (~1.3%) when turned slowly, big jumps on a fast
     * spin (the host sends larger |delta| when spun quickly) → full sweep in one brisk turn. */
    float d=(float)delta, mag=fabsf(d);
    float step=0.013f + 0.006f*(mag-1.0f); if(step>0.07f) step=0.07f;
    set_float_key(p,key, get_float_key(p,key)+d*step);
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
        else if(!strcmp(val,"Global")) p->current_level=LV_GLOBAL;
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

    if(!strcmp(key,"current_preset")){ int idx=clampi(atoi(val),1,NUM_PRESETS);
        p->current_preset=idx; p->pending_preset=idx;
        if(p->rnd_phase==0||p->rnd_phase==2) p->rnd_phase=1; return; }
    if(!strcmp(key,"fx_reorder")){
        /* accept "1-2-3-4" label or index */
        int idx=-1; for(int i=0;i<24;i++){ char lb[16]; perm_label(i,lb,sizeof lb);
            if(!strcmp(val,lb)){idx=i;break;} }
        if(idx<0) idx=clampi(atoi(val),0,23);
        p->fx_reorder=idx; return;
    }
    /* GLOBAL page */
    if(!strcmp(key,"tempo_src")){ p->tempo_src = (!strcmp(val,"Int")||atoi(val)==1)?1:0; return; }
    if(!strcmp(key,"tempo_bpm")){ p->tempo_bpm = clampi(atoi(val),10,500); return; }
    if(!strcmp(key,"time_div")){
        int idx=-1; for(int i=0;i<NUM_DIVS;i++) if(!strcmp(val,DIVS[i].name)){idx=i;break;}
        if(idx<0) idx=clampi(atoi(val),0,NUM_DIVS-1);
        p->time_div=idx; return;
    }
    /* full-state restore (per-Set persistence). CSV: 4×(sel,amt,mac,drf),mix,ivol,ord,preset */
    if(!strcmp(key,"state")){
        int sel[4],ord=0,pre=1,tsrc=0,tbpm=120,tdiv=0; float a[4],m[4],d[4],mix=1.0f,iv=1.0f,fb=0.0f;
        int got=sscanf(val,
            "%d,%f,%f,%f,%d,%f,%f,%f,%d,%f,%f,%f,%d,%f,%f,%f,%f,%f,%d,%d,%f,%d,%d,%d",
            &sel[0],&a[0],&m[0],&d[0], &sel[1],&a[1],&m[1],&d[1],
            &sel[2],&a[2],&m[2],&d[2], &sel[3],&a[3],&m[3],&d[3],
            &mix,&iv,&ord,&pre, &fb,&tsrc,&tbpm,&tdiv);
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
            if(got>=20) p->current_preset=clampi(pre,1,NUM_PRESETS);
            if(got>=24){ p->feedback=clampf(fb,0,1); p->tempo_src=clampi(tsrc,0,1);
                         p->tempo_bpm=clampi(tbpm,10,500); p->time_div=clampi(tdiv,0,NUM_DIVS-1); }
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
          "{\"level\":\"FX34\",\"label\":\"FX 3&4\"},"
          "{\"level\":\"Global\",\"label\":\"GLOBAL\"}"
          "]},"
          "\"Console\":{\"name\":\"PALETTE\","
          "\"knobs\":[\"fx1_amount\",\"fx1_macro\",\"fx2_amount\",\"fx2_macro\",\"fx3_amount\",\"fx3_macro\",\"fx4_amount\",\"fx4_macro\"],"
          "\"params\":[\"fx1_amount\",\"fx1_macro\",\"fx2_amount\",\"fx2_macro\",\"fx3_amount\",\"fx3_macro\",\"fx4_amount\",\"fx4_macro\"]},"
          "\"Presets\":{\"name\":\"PRESETS&RND\","
          "\"knobs\":[\"current_preset\",\"rnd_patch\",\"rnd_effect\",\"rnd_amount\",\"rnd_macro\",\"rnd_values\",\"input_vol\",\"mix\"],"
          "\"params\":[\"current_preset\",\"rnd_patch\",\"rnd_effect\",\"rnd_amount\",\"rnd_macro\",\"rnd_values\",\"input_vol\",\"mix\",\"fx_reorder\"]},"
          "\"FX12\":{\"name\":\"FX 1&2\","
          "\"knobs\":[\"fx1_select\",\"fx1_amount\",\"fx1_macro\",\"fx1_drift\",\"fx2_select\",\"fx2_amount\",\"fx2_macro\",\"fx2_drift\"],"
          "\"params\":[\"fx1_select\",\"fx1_amount\",\"fx1_macro\",\"fx1_drift\",\"fx2_select\",\"fx2_amount\",\"fx2_macro\",\"fx2_drift\"]},"
          "\"FX34\":{\"name\":\"FX 3&4\","
          "\"knobs\":[\"fx3_select\",\"fx3_amount\",\"fx3_macro\",\"fx3_drift\",\"fx4_select\",\"fx4_amount\",\"fx4_macro\",\"fx4_drift\"],"
          "\"params\":[\"fx3_select\",\"fx3_amount\",\"fx3_macro\",\"fx3_drift\",\"fx4_select\",\"fx4_amount\",\"fx4_macro\",\"fx4_drift\"]},"
          "\"Global\":{\"name\":\"GLOBAL\","
          "\"knobs\":[\"feedback\",\"tempo_src\",\"tempo_bpm\",\"time_div\"],"
          "\"params\":[\"feedback\",\"tempo_src\",\"tempo_bpm\",\"time_div\"]}"
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
              "{\"key\":\"fx%d_amount\",\"name\":\"FX%d Amount\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
              "{\"key\":\"fx%d_macro\",\"name\":\"FX%d Macro\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
              "{\"key\":\"fx%d_drift\",\"name\":\"FX%d Drift\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},",
              s,s,s,s,s,s);
        }
        o+=snprintf(buf+o,buf_len-o,
          "{\"key\":\"input_vol\",\"name\":\"Input Vol\",\"type\":\"float\",\"min\":0,\"max\":2,\"step\":0.01},"
          "{\"key\":\"mix\",\"name\":\"Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},");
        /* Preset — named enum ("N Name"); set_param uses atoi() to load by number */
        o+=snprintf(buf+o,buf_len-o,"{\"key\":\"current_preset\",\"name\":\"Preset\",\"type\":\"enum\",\"options\":[");
        for(int i=0;i<NUM_PRESETS;i++)
            o+=snprintf(buf+o,buf_len-o,"%s\"%d %s\"",i?",":"",i+1,PRESETS[i].name);
        o+=snprintf(buf+o,buf_len-o,"]},");
        o+=snprintf(buf+o,buf_len-o,
          "{\"key\":\"rnd_patch\",\"name\":\"Rnd Patch\",\"type\":\"enum\",\"options\":[\"0\",\"1\"]},"
          "{\"key\":\"rnd_effect\",\"name\":\"Rnd FX\",\"type\":\"enum\",\"options\":[\"0\",\"1\"]},"
          "{\"key\":\"rnd_amount\",\"name\":\"Rnd Amt\",\"type\":\"enum\",\"options\":[\"0\",\"1\"]},"
          "{\"key\":\"rnd_macro\",\"name\":\"Rnd Macro\",\"type\":\"enum\",\"options\":[\"0\",\"1\"]},"
          "{\"key\":\"rnd_values\",\"name\":\"Rnd Values\",\"type\":\"enum\",\"options\":[\"0\",\"1\"]},");
        /* FX Reorder — menu-only enum of all 24 chain permutations */
        o+=snprintf(buf+o,buf_len-o,"{\"key\":\"fx_reorder\",\"name\":\"FX Reorder\",\"type\":\"enum\",\"options\":[");
        for(int i=0;i<24;i++){ char lb[16]; perm_label(i,lb,sizeof lb);
            o+=snprintf(buf+o,buf_len-o,"%s\"%s\"",i?",":"",lb); }
        o+=snprintf(buf+o,buf_len-o,"]},");
        /* ── GLOBAL page ── */
        o+=snprintf(buf+o,buf_len-o,
          "{\"key\":\"feedback\",\"name\":\"Feedback\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"tempo_src\",\"name\":\"Tempo Src\",\"type\":\"enum\",\"options\":[\"Move\",\"Int\"]},"
          "{\"key\":\"tempo_bpm\",\"name\":\"Tempo\",\"type\":\"int\",\"min\":10,\"max\":500,\"step\":1},");
        o+=snprintf(buf+o,buf_len-o,"{\"key\":\"time_div\",\"name\":\"Time Div\",\"type\":\"enum\",\"options\":[");
        for(int i=0;i<NUM_DIVS;i++) o+=snprintf(buf+o,buf_len-o,"%s\"%s\"",i?",":"",DIVS[i].name);
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
    if(!strcmp(key,"feedback"))      return snprintf(buf,buf_len,"%.4f",p->feedback);  /* raw */
    if(!strcmp(key,"tempo_src"))     return snprintf(buf,buf_len,"%s",p->tempo_src?"Int":"Move");
    if(!strcmp(key,"tempo_bpm"))     return snprintf(buf,buf_len,"%d",p->tempo_bpm);
    if(!strcmp(key,"time_div"))      return snprintf(buf,buf_len,"%s",DIVS[clampi(p->time_div,0,NUM_DIVS-1)].name);
    if(!strcmp(key,"current_preset")){ int pi=clampi(p->current_preset-1,0,NUM_PRESETS-1);
        return snprintf(buf,buf_len,"%d %s",pi+1,PRESETS[pi].name); }
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
                if(!strcmp(f,"select")) return snprintf(buf,buf_len,"FX%d Select",sl);
                if(!strcmp(f,"amount")) return snprintf(buf,buf_len,"FX%d Amount",sl);
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
            if(!strcmp(pk,"rnd_values"))      return snprintf(buf,buf_len,"Rnd Values");
            if(!strcmp(pk,"feedback"))       return snprintf(buf,buf_len,"Feedback");
            if(!strcmp(pk,"tempo_src"))      return snprintf(buf,buf_len,"Tempo Src");
            if(!strcmp(pk,"tempo_bpm"))      return snprintf(buf,buf_len,"Tempo");
            if(!strcmp(pk,"time_div"))       return snprintf(buf,buf_len,"Time Div");
            return snprintf(buf,buf_len,"%s",pk);
        } else { /* _value */
            if(slot_of(pk)>=0 && !strcmp(pk+4,"select"))
                return snprintf(buf,buf_len,"%s",FX_NAMES[clampi(p->slots[slot_of(pk)].select,0,NUM_FX)]);
            if(!strcmp(pk,"current_preset")){ int pi=clampi(p->current_preset-1,0,NUM_PRESETS-1);
                return snprintf(buf,buf_len,"%d %s",pi+1,PRESETS[pi].name); }
            if(!strncmp(pk,"rnd_",4))        return snprintf(buf,buf_len,"tap");
            if(!strcmp(pk,"tempo_src"))      return snprintf(buf,buf_len,"%s",p->tempo_src?"Int":"Move");
            if(!strcmp(pk,"tempo_bpm"))      return snprintf(buf,buf_len,"%d BPM",p->tempo_bpm);
            if(!strcmp(pk,"time_div"))       return snprintf(buf,buf_len,"%s",DIVS[clampi(p->time_div,0,NUM_DIVS-1)].name);
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
        o+=snprintf(buf+o,buf_len-o,",%.4f,%d,%d,%d",
            p->feedback,p->tempo_src,p->tempo_bpm,p->time_div);  /* GLOBAL */
        return o;
    }

    return -1;   /* unknown key MUST be -1, never 0 */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  AUDIO  — slots processed in FX-Reorder order; equal-power global Mix.
 * ═══════════════════════════════════════════════════════════════════════════ */
/* Character-group level match (ported from Loopex 9bc1243). Drive, Sweeten, Fuzz,
 * Howl and Fold are LEVEL-NORMALISING distortions: quiet material gets enormous gain
 * (+19..+22 dB on a -24 dBFS source) while loud material barely moves, and each hits
 * its levelling point at a different Amount. A static makeup can't fix that because
 * the error tracks input level, so track dry and wet power (~150 ms) and pull the wet
 * PARTWAY back toward the dry level: tame the extremes and line the five up without
 * flattening them to unity. The pull is done in dB (gain = (dry/wet)^(LVL_MATCH/2)),
 * so "75 % of the way" holds for big errors too - Loopex interpolates the linear gain,
 * which only corrects ~11 of a 26 dB error. SWELL is deliberately excluded (Loopex
 * includes it): its envelope is as slow as the matcher, which would undo the swell. */
#define LVL_MATCH 0.75f
static int is_character_fx(int fx){
    return fx==PFX_DRIVE||fx==PFX_SWEETEN||fx==PFX_FUZZ||fx==PFX_HOWL||fx==PFX_FOLD;
}
/* Gain-heavy effects turn a line-in noise floor into loud hiss (Fuzz +62 dB on a
 * -80 dBFS floor). A soft gate keyed on the DRY peak (instant attack, ~150 ms
 * release) closes the wet below ~-72 dBFS and is fully open above ~-60 dBFS. */
static int is_gated_fx(int fx){ return is_character_fx(fx) || fx==PFX_SQUASH; }

/* NaN / Inf / runaway test by bit pattern - immune to -ffast-math, which deletes
 * `x != x` and isfinite(). Exponent 0xFF = Inf/NaN; >= 2^5 = |x| >= 32 (runaway). */
static inline int bad_sample(float x){
    union { float f; uint32_t u; } v; v.f=x;
    uint32_t e=(v.u>>23)&0xFFu;
    return e==0xFFu || e>=127u+5u;
}
/* Output safety: transparent below 0.89 (-1 dBFS), soft knee into a 1.0 ceiling so
 * hot patches saturate gently instead of hard-clipping the int16 rail. */
static inline float soft_limit(float x){
    float a=fabsf(x); if(a<=0.89f) return x;
    float y=0.89f+0.11f*sb_tanh((a-0.89f)/0.11f);
    return x<0.0f? -y : y;
}
static inline float smooth01(float g){ g=clampf(g,0.0f,1.0f); return g*g*(3.0f-2.0f*g); }

#define SUBBLK 16          /* params are smoothed per 16 samples, not per 128 */

static void process_block(void *instance, int16_t *buf, int frames){
    palette_t *p=(palette_t*)instance; if(!p||frames<=0) return;
    if(frames>MAXFRAMES) frames=MAXFRAMES;

    float gsm = 1.0f - expf(-(float)frames/(0.015f*SR));     /* 15 ms smooth for globals */
    float iv0 = p->iv_sm, mix0 = p->mix_sm;
    p->iv_sm  += gsm*(p->input_vol - p->iv_sm);
    p->mix_sm += gsm*(p->mix       - p->mix_sm);
    p->fb_sm  += gsm*(p->feedback  - p->fb_sm);

    /* ── tempo sync: effective BPM → synced delay time + LFO rate for this block ── */
    float bpm = (p->tempo_src==1) ? (float)p->tempo_bpm
              : ((p->clock_running && p->move_bpm>=10.0f) ? p->move_bpm : (float)p->tempo_bpm);
    bpm = clampf(bpm,10.0f,500.0f);
    float sync_time=0.0f, sync_rate=0.0f;
    if(p->time_div>0){
        float npb=DIVS[clampi(p->time_div,0,NUM_DIVS-1)].npb;
        float tsec=(60.0f/bpm)/npb;
        sync_time=tsec*SR; if(sync_time>(float)(MAX_DELAY-4)) sync_time=(float)(MAX_DELAY-4);
        sync_rate=1.0f/tsec;
    }
    p->sample_pos += (uint32_t)frames;
    /* stir the entropy pool from the LIVE audio (never identical) + sample counter,
     * so the randomizers are truly non-deterministic (not a fixed seeded sequence). */
    if(frames>0){
        uint32_t e = p->ent ^ p->sample_pos
                   ^ ((uint32_t)(uint16_t)buf[0])
                   ^ ((uint32_t)(uint16_t)buf[2*frames-1] << 16);
        e ^= e<<13; e ^= e>>17; e ^= e<<5;            /* xorshift mix */
        p->ent = e;
    }
    /* randomize/preset DUCK: once the output has faded out, apply the deferred change
     * during silence (the heavy Clouds swap happens here, unheard), hold, then fade in. */
    if(p->rnd_phase==1 && p->rnd_gain<=0.02f){
        apply_pending(p); p->rnd_gain=0.0f;
        p->rnd_hold=(int)(SR*0.020f); p->rnd_phase=3;
    } else if(p->rnd_phase==3){
        p->rnd_hold-=frames; if(p->rnd_hold<=0) p->rnd_phase=2;
    }

    /* feedback gain curve: fb_sm^2 keeps the low range gentle; ×(0.85..1.15)
     * pushes just past unity at the top so it self-oscillates (soft-clipped). */
    float fgain = p->fb_sm*p->fb_sm*(0.85f + 0.30f*p->fb_sm);
    /* de-interleave + input volume (per-sample ramp: no block-rate zipper) + global
     * feedback send (damped + soft-clipped) */
    float dryL[MAXFRAMES], dryR[MAXFRAMES], chL[MAXFRAMES], chR[MAXFRAMES];
    float ivstep=(p->iv_sm-iv0)/(float)frames;
    for(int i=0;i<frames;i++){
        float l=buf[2*i]/32768.0f, r=buf[2*i+1]/32768.0f;
        float iv=iv0+ivstep*(float)(i+1);
        dryL[i]=l; dryR[i]=r;
        /* DC/sub high-pass first (stops rumble building to a rail or cancelling to
         * silence), then tone-damping LP, then a curved gain: gentle across the low
         * range, crossing unity at the top for a soft-limited self-oscillating drone. */
        p->fb_hp_l += 0.0012f*(p->fbL[i]-p->fb_hp_l)+DENORM; float hpl=p->fbL[i]-p->fb_hp_l;
        p->fb_hp_r += 0.0012f*(p->fbR[i]-p->fb_hp_r)+DENORM; float hpr=p->fbR[i]-p->fb_hp_r;
        p->fb_lp_l += 0.35f*(hpl-p->fb_lp_l)+DENORM;        /* tone damping */
        p->fb_lp_r += 0.35f*(hpr-p->fb_lp_r)+DENORM;
        float fbl=sb_tanh(p->fb_lp_l*fgain);                /* soft-limited regeneration */
        float fbr=sb_tanh(p->fb_lp_r*fgain);
        p->L[i]=l*iv+fbl; p->R[i]=r*iv+fbr;
        chL[i]=p->L[i]; chR[i]=p->R[i];                    /* chain input (reorder dip) */
    }

    /* FX Reorder: changing the path mid-signal is a step in the waveform - at the
     * output, and inside every delay line whose input suddenly comes from a different
     * slot (heard again one echo later). So over ~5 ms dip EVERY slot's input to zero
     * and the chain output to its own input, swap the order at the bottom, rise back. */
    if(p->order_cur!=p->fx_reorder && p->ro_gain<=0.0f) p->order_cur=clampi(p->fx_reorder,0,23);
    const uint8_t *order = PERM[clampi(p->order_cur,0,23)];
    float rog[MAXFRAMES]; int ro_active=0;
    {
        float rt = (p->order_cur==p->fx_reorder)? 1.0f : 0.0f;
        const float ro_inc = 1.0f/(SR*0.005f);
        if(rt<1.0f || p->ro_gain<1.0f){
            float g=p->ro_gain; ro_active=1;
            for(int i=0;i<frames;i++){ g += clampf(rt-g, -ro_inc, ro_inc); rog[i]=smooth01(g); }
            p->ro_gain=g;
        }
    }

    /* run the 4 slots in reorder sequence (Off = passthrough, never dispatched).
     * Clouds effects dispatch through the opaque heavy interface; C effects use the
     * vtable. Amount/Macro/Drift are smoothed (15 ms) per 16-sample sub-block, so knob
     * moves arrive as small steps. A Select change first fades the RUNNING effect out
     * to dry (~6 ms), then swaps (slot_engage) and fades the new one in (~25 ms,
     * smoothstep) - no step in the waveform either way. */
    const float psm = 1.0f - expf(-(float)SUBBLK/(0.015f*SR));   /* 15 ms param smooth */
    const float ramp_inc = 1.0f / (SR * 0.025f);                 /* 25 ms fade in */
    const float out_inc  = 1.0f / (SR * 0.006f);                 /* 6 ms fade out */
    const float grel = expf(-(float)frames/(0.15f*SR));          /* gate release ~150 ms */
    float preL[MAXFRAMES], preR[MAXFRAMES];
    for(int k=0;k<NUM_SLOTS;k++){
        int si=order[k];
        slot_t *sl=&p->slots[si];
        if(sl->active!=sl->select && (sl->active==PFX_OFF || sl->xgain<=0.0f)
           && !heavy_busy(p,si,sl->select))
            slot_engage(p,si);                       /* faded out (or was Off): swap now */
        int fx=sl->active;
        if(fx==PFX_OFF) continue;
        sl->dsp.sync_time = sync_time;     /* tempo sync (0 = free-running) */
        sl->dsp.sync_rate = sync_rate;
        float xt = (sl->active==sl->select)? 1.0f : 0.0f;          /* fade-out target */
        int blending = sl->ramp < 1.0f || sl->xgain < 1.0f || xt < 1.0f;
        memcpy(preL,p->L,frames*sizeof(float)); memcpy(preR,p->R,frames*sizeof(float));
        /* a just-engaged slot also fades its INPUT in: its delay lines start empty, so
         * signal recorded abruptly would come back as a hard edge one echo later */
        if(sl->ramp<1.0f || ro_active){
            float rr=sl->ramp;
            for(int i=0;i<frames;i++){
                float gi=1.0f;
                if(sl->ramp<1.0f){ rr+=ramp_inc; if(rr>1.0f) rr=1.0f; gi=smooth01(rr); }
                if(ro_active) gi*=rog[i];
                p->L[i]*=gi; p->R[i]*=gi;
            }
        }
        int match = is_character_fx(fx);
        int gated = is_gated_fx(fx);
        float dp = 0.0f, dpk = 0.0f;
        if(match||gated) for(int i=0;i<frames;i++){
            dp += p->L[i]*p->L[i] + p->R[i]*p->R[i];
            float a=fabsf(p->L[i]); if(a>dpk)dpk=a; a=fabsf(p->R[i]); if(a>dpk)dpk=a; }
        for(int o=0;o<frames;o+=SUBBLK){
            int m=frames-o; if(m>SUBBLK) m=SUBBLK;
            sl->amt_sm += psm*(sl->amount - sl->amt_sm);
            sl->mac_sm += psm*(sl->macro  - sl->mac_sm);
            sl->drf_sm += psm*(sl->drift  - sl->drf_sm);
            if(is_clouds_fx(fx)){
                if(sl->dsp.heavy)
                    pfx_clouds_process(fx, sl->dsp.heavy, p->L+o, p->R+o, m,
                                       sl->amt_sm, sl->mac_sm, sl->drf_sm);
                /* heavy alloc failed → leave signal untouched (passthrough) */
            } else {
                FX_TABLE[fx].process(&sl->dsp, p->L+o, p->R+o, m,
                                     sl->amt_sm, sl->mac_sm, sl->drf_sm);
            }
        }
        /* health: a NaN/Inf/runaway would otherwise live in the slot's state forever
         * (a dead slot until reload). Replace the block with dry and reset the slot. */
        int bad=0;
        for(int i=0;i<frames;i++) bad |= bad_sample(p->L[i]) | bad_sample(p->R[i]);
        if(bad){
            memcpy(p->L,preL,frames*sizeof(float)); memcpy(p->R,preR,frames*sizeof(float));
            if(sl->dsp.heavy) pfx_clouds_reset(sl->dsp.heavy);
            slot_reset(sl); sl->ramp=0.0f;
            continue;
        }
        if(match){
            slot_dsp_t *d=&sl->dsp;
            float wp = 0.0f;
            for(int i=0;i<frames;i++) wp += p->L[i]*p->L[i] + p->R[i]*p->R[i];
            float inv = 1.0f/(float)(frames*2);
            d->lvlDry += ((dp*inv) - d->lvlDry) * 0.02f;   /* ~150 ms at block rate */
            d->lvlWet += ((wp*inv) - d->lvlWet) * 0.02f;
            if(d->lvlDry < 1e-15f) d->lvlDry = 0.0f;        /* denormal floor in long silence */
            if(d->lvlWet < 1e-15f) d->lvlWet = 0.0f;
            float tg = 1.0f;
            if(d->lvlWet > 1e-9f && d->lvlDry > 1e-9f){
                tg = powf(d->lvlDry / d->lvlWet, 0.5f*LVL_MATCH);   /* dB-domain partial pull */
                tg = clampf(tg, 0.05f, 4.0f);
            }
            if(d->lvlGain <= 0.0f) d->lvlGain = tg;        /* fresh slot: snap, don't glide */
            /* applied in proportion to Amount (full from 0.1 up): switching it off at a
             * threshold snapped the gain back to 1 - a click as Amount reached 0 */
            float w1 = clampf(sl->amt_sm*10.0f, 0.0f, 1.0f), w0=d->lvlW, ws=(w1-w0)/(float)frames;
            for(int i=0;i<frames;i++){                      /* ramp across the block */
                d->lvlGain += (tg - d->lvlGain) * 0.002f;
                float g = 1.0f + (d->lvlGain - 1.0f)*(w0+ws*(float)(i+1));
                p->L[i] *= g; p->R[i] *= g;
            }
            d->lvlW = w1;
        }
        if(gated){
            sl->genv = (dpk > sl->genv)? dpk : sl->genv*grel;
            float db = 20.0f*log10f(sl->genv + 1e-9f);
            float gt = smooth01((db + 72.0f)/12.0f);         /* -72 closed .. -60 open */
            float g0 = sl->gate, gs=(gt-g0)/(float)frames;
            for(int i=0;i<frames;i++){                      /* gate the WET: blend to dry */
                float g=g0+gs*(float)(i+1);
                p->L[i]=preL[i]+(p->L[i]-preL[i])*g;
                p->R[i]=preR[i]+(p->R[i]-preR[i])*g;
            }
            sl->gate=gt;
        }
        if(blending){
            float rr=sl->ramp, xg=sl->xgain;
            for(int i=0;i<frames;i++){
                rr += ramp_inc; if(rr>1.0f) rr=1.0f;
                xg += clampf(xt-xg, -out_inc, out_inc);
                float g=smooth01(rr)*smooth01(xg);        /* in-fade × out-fade */
                p->L[i]=preL[i]+(p->L[i]-preL[i])*g;
                p->R[i]=preR[i]+(p->R[i]-preR[i])*g;
            }
            sl->ramp=rr; sl->xgain=xg;
        }
    }
    /* reorder dip: crossfade the chain output toward the chain input */
    if(ro_active)
        for(int i=0;i<frames;i++){
            p->L[i]=chL[i]+(p->L[i]-chL[i])*rog[i];
            p->R[i]=chR[i]+(p->R[i]-chR[i])*rog[i];
        }
    /* equal-power dry/wet (Mix ramped per sample) + output soft limit + write back.
     * The wet chain output is stashed as next block's feedback source. */
    float rtgt=(p->rnd_phase==1||p->rnd_phase==3)?0.0f:1.0f;   /* duck target */
    float rcoef=(p->rnd_phase==2)?0.0016f:0.006f;              /* fade-in slower than out */
    float rgn=p->rnd_gain;
    float mstep=(p->mix_sm-mix0)/(float)frames;
    int mramp = fabsf(mstep) > 1e-7f;                /* per-sample law only while Mix moves */
    float dg=cosf(p->mix_sm*1.5707963f), wg=sinf(p->mix_sm*1.5707963f);
    for(int i=0;i<frames;i++){
        float wl=p->L[i], wr=p->R[i];
        if(bad_sample(wl)) wl=0.0f;
        if(bad_sample(wr)) wr=0.0f;
        p->fbL[i]=wl; p->fbR[i]=wr;                 /* feedback source for next block */
        if(mramp){ float a=(mix0+mstep*(float)(i+1))*1.5707963f; dg=cosf(a); wg=sinf(a); }
        float ol=soft_limit(dg*dryL[i]+wg*wl);
        float orr=soft_limit(dg*dryR[i]+wg*wr);
        rgn += rcoef*(rtgt-rgn); ol*=rgn; orr*=rgn; /* duck envelope */
        int32_t il=(int32_t)(ol*32767.0f), ir=(int32_t)(orr*32767.0f);
        if(il>32767)il=32767; if(il<-32768)il=-32768;
        if(ir>32767)ir=32767; if(ir<-32768)ir=-32768;
        buf[2*i]=(int16_t)il; buf[2*i+1]=(int16_t)ir;
    }
    p->rnd_gain=rgn;
    if(p->rnd_phase==2 && rgn>0.995f){ p->rnd_gain=1.0f; p->rnd_phase=0; }
}

/* ── MIDI: derive Move tempo from host clock (24 PPQN). Only used when GLOBAL
 *  Tempo Src = Move; Int always works without this. ───────────────────────────── */
static void on_midi(void *instance, const uint8_t *msg, int len, int source){
    palette_t *p=(palette_t*)instance; if(!p||!msg||len<1) return;
    (void)source;
    uint8_t st=msg[0];
    if(st==0xFA||st==0xFB){ p->clock_running=1; p->clock_count=0; p->last_clock_sample=p->sample_pos; }
    else if(st==0xFC){ p->clock_running=0; }
    else if(st==0xF8){ /* timing clock — measure samples per quarter (24 pulses) */
        p->clock_running=1;
        if(++p->clock_count>=24){
            uint32_t now=p->sample_pos, dq=now - p->last_clock_sample;
            p->last_clock_sample=now; p->clock_count=0;
            if(dq>2000 && dq<SR*6){                    /* sane: 10..~440 BPM */
                p->clock_interval_sm += 0.25f*((float)dq - p->clock_interval_sm);
                float b=60.0f*SR/p->clock_interval_sm;
                p->move_bpm = clampf(b,10.0f,500.0f);
            }
        }
    }
}

/* ── API v2 export ───────────────────────────────────────────────────────────── */
static audio_fx_api_v2_t g_api = {
    .api_version=2, .create_instance=create_instance, .destroy_instance=destroy_instance,
    .process_block=process_block, .set_param=set_param, .get_param=get_param, .on_midi=on_midi,
};
__attribute__((visibility("default")))
audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host){
    g_host=host; if(host&&host->log) host->log("[palette] loaded"); return &g_api;
}
/* Also export on_midi by name (some host builds dlsym it). */
__attribute__((visibility("default")))
void move_audio_fx_on_midi(void *instance, const uint8_t *msg, int len, int source){
    on_midi(instance,msg,len,source);
}
