/* fx_audit.c — offline quality-of-life audit for PALETTE's 24 effects.
 *
 * Compiles the REAL src/dsp/palette.c into this TU (so the static internals are
 * reachable) and drives it exactly like the Move does: int16 interleaved blocks of
 * 128 frames through process_block(), with knob turns sent as set_param("knob_N_adjust")
 * deltas through the page overlay -> accelerated step -> 15 ms smoothing path.
 *
 * Scenarios per effect (slot 1, other slots Off, Mix 100 %):
 *   grid   static Amount x Macro x Drift on a -12 dBFS 110 Hz sine: level, peak, clips,
 *          DC, NaN, steady-state clicks (an effect that clicks on its own).
 *   quiet  same at -36 dBFS (how much louder quiet material gets).
 *   silence digital zero in: anything coming out is self-generated (hiss/static/crackle).
 *   floor  -80 dBFS white "line-in floor" in: how much the effect amplifies it.
 *   tail   music 3 s then silence: level 3-5 s after the music stops.
 *   sweeps slow (one detent per ~12 ms) and fast-spin turns of Amount / Macro / Drift
 *          on the sine; clicks counted against the static context around the sweep.
 *   switch stepping fx1 Select through every effect (sine + music): click per transition.
 *   cpu    native us/block at Amount 1, Drift 1 (relative only — not Move numbers).
 *
 * Click detector: 2nd difference (HF emphasis) energy in 512-sample windows (hop 256),
 * compared to the LARGER of the median of the 20 windows before and the 20 after, so a
 * level ramp is never flagged but an isolated burst is. Flag = > +6 dB above context
 * AND an absolute 2nd-diff RMS > 2.5e-4 (~ a 0.004 step, -48 dBFS).
 */
#include "../src/dsp/palette.c"
#include <time.h>

#define FS   44100
#define BLK  128

typedef struct { float *l, *r; int n; } abuf_t;

static abuf_t mkbuf(int n){ abuf_t b; b.n=n; b.l=calloc(n,sizeof(float)); b.r=calloc(n,sizeof(float)); return b; }
static void freebuf(abuf_t *b){ free(b->l); free(b->r); b->l=b->r=NULL; }

static uint32_t trng=0x2545F491u;
static float tnoise(void){ trng^=trng<<13; trng^=trng>>17; trng^=trng<<5; return (float)(trng>>8)*(1.0f/8388608.0f)-1.0f; }

static void gen_sine(abuf_t *b, int n0, int n1, float f, float amp){
    for(int i=n0;i<n1&&i<b->n;i++){ double t=(double)i/FS; float fi=(i-n0)<441? (float)(i-n0)/441.0f : 1.0f;
        b->l[i]+=fi*amp*(float)sin(2*M_PI*f*t); b->r[i]+=fi*amp*(float)sin(2*M_PI*f*t+0.3); }
}
static void gen_noise(abuf_t *b, int n0, int n1, float amp){
    for(int i=n0;i<n1&&i<b->n;i++){ b->l[i]+=amp*tnoise(); b->r[i]+=amp*tnoise(); }
}
/* plucked bass/keys line + kick + hats: broadband, transient, musical */
static void gen_music(abuf_t *b, int n0, int n1, float gain){
    static const float notes[8]={110.0f,164.81f,130.81f,196.0f,110.0f,146.83f,130.81f,98.0f};
    for(int i=n0;i<n1&&i<b->n;i++){
        double t=(double)(i-n0)/FS; float l=0,r=0;
        int k=(int)(t/0.25); double tn=t-k*0.25; float f=notes[k&7];
        float env=(float)exp(-tn/0.18);
        for(int h=1;h<=5;h++){ float a=env*0.22f/(float)h;
            l+=a*(float)sin(2*M_PI*f*h*tn); r+=a*(float)sin(2*M_PI*f*h*tn+0.2*h); }
        double tk=fmod(t,0.5); float ke=(float)exp(-tk/0.12);
        float kick=0.35f*ke*(float)sin(2*M_PI*(50.0*tk+70.0*0.04*(1-exp(-tk/0.04))));
        l+=kick; r+=kick;
        double th=fmod(t+0.125,0.25); if(th<0.03){ float he=(float)exp(-th/0.006)*0.06f; l+=he*tnoise(); r+=he*tnoise(); }
        b->l[i]+=l*gain; b->r[i]+=r*gain;
    }
}

/* ── run the real module ───────────────────────────────────────────────────── */
typedef void (*ctl_fn)(palette_t*, int blk, void *ctx);
static inline int16_t f2s(float x){ float y=x*32768.0f; y=y<0?y-0.5f:y+0.5f;
    if(y>32767.0f)y=32767.0f; if(y<-32768.0f)y=-32768.0f; return (int16_t)y; }
static int g_clip;
static void run(palette_t *p, const abuf_t *in, abuf_t *out, ctl_fn ctl, void *ctx){
    int16_t buf[BLK*2];
    for(int b=0; b*BLK<in->n; b++){
        int off=b*BLK, nf=in->n-off; if(nf>BLK)nf=BLK;
        if(ctl) ctl(p,b,ctx);
        for(int i=0;i<nf;i++){ buf[2*i]=f2s(in->l[off+i]); buf[2*i+1]=f2s(in->r[off+i]); }
        process_block(p,buf,nf);
        for(int i=0;i<nf;i++){
            if(buf[2*i]>=32767||buf[2*i]<=-32768||buf[2*i+1]>=32767||buf[2*i+1]<=-32768) g_clip++;
            out->l[off+i]=buf[2*i]/32768.0f; out->r[off+i]=buf[2*i+1]/32768.0f; }
    }
}
static palette_t *mk(int fx, float a, float m, float d){
    palette_t *p=create_instance(NULL,NULL);
    for(int s=0;s<NUM_SLOTS;s++){ slot_apply_select(p,s,PFX_OFF); p->slots[s].active=PFX_OFF;
        p->slots[s].amount=p->slots[s].amt_sm=0; p->slots[s].macro=p->slots[s].mac_sm=0;
        p->slots[s].drift=p->slots[s].drf_sm=0; p->slots[s].ramp=1.0f; }
    p->feedback=p->fb_sm=0; p->mix=p->mix_sm=1; p->input_vol=p->iv_sm=1;
    p->time_div=0; p->fx_reorder=0; p->order_cur=0; p->ro_gain=1.0f; p->rnd_phase=0; p->rnd_gain=1; p->pending_preset=-1; p->pending_rnd=0;
    clear_feedback(p);
    if(fx){ slot_apply_select(p,0,fx); slot_engage(p,0); }
    slot_t *sl=&p->slots[0];
    sl->amount=sl->amt_sm=a; sl->macro=sl->mac_sm=m; sl->drift=sl->drf_sm=d; sl->ramp=1.0f;
    return p;
}

/* ── metrics ───────────────────────────────────────────────────────────────── */
static float rms_db(const abuf_t *b, int n0, int n1){
    double e=0; int c=0; for(int i=n0;i<n1&&i<b->n;i++){ e+=b->l[i]*b->l[i]+b->r[i]*b->r[i]; c+=2; }
    return c? 10.0f*(float)log10(e/c+1e-20):-200.0f;
}
static float peak(const abuf_t *b, int n0, int n1){
    float p=0; for(int i=n0;i<n1&&i<b->n;i++){ float a=fabsf(b->l[i]); if(a>p)p=a; a=fabsf(b->r[i]); if(a>p)p=a; } return p;
}
static float dc(const abuf_t *b, int n0, int n1){
    double s=0; int c=0; for(int i=n0;i<n1&&i<b->n;i++){ s+=b->l[i]+b->r[i]; c+=2; } return c?(float)(s/c):0;
}
static int nan_count(const abuf_t *b){ int c=0; for(int i=0;i<b->n;i++){ if(!isfinite(b->l[i])||!isfinite(b->r[i]))c++; } return c; }
static int cmpf(const void *a, const void *b){ float x=*(const float*)a, y=*(const float*)b; return (x>y)-(x<y); }

#define CW 512
#define CH 256
#define CCTX 20
#define CLICK_ABS 4e-5f      /* HP>4k window RMS floor (calibrated in detector_selftest) */
typedef struct { int events, windows; float worst_db; int worst_at; } click_t;
/* energy profile above 4 kHz (2 cascaded RBJ high-pass biquads = 4th order), both
 * channels. Clicks are broadband; a sine, a resonant sweep at a few hundred Hz or a
 * slow fade are not, so they don't trip it. */
typedef struct { double b0,b1,b2,a1,a2, x1,x2,y1,y2; } bq_t;
static void bq_hp(bq_t *q, double fc){ double w=2*M_PI*fc/FS, c=cos(w), al=sin(w)/(2*0.7071), a0=1+al;
    q->b0=(1+c)/2/a0; q->b1=-(1+c)/a0; q->b2=(1+c)/2/a0; q->a1=-2*c/a0; q->a2=(1-al)/a0; q->x1=q->x2=q->y1=q->y2=0; }
static inline double bq_run(bq_t *q, double x){ double y=q->b0*x+q->b1*q->x1+q->b2*q->x2-q->a1*q->y1-q->a2*q->y2;
    q->x2=q->x1; q->x1=x; q->y2=q->y1; q->y1=y; return y; }
static int hf_profile(const abuf_t *b, float **E){
    int nw=(b->n-CW-2)/CH; if(nw<1){ *E=NULL; return 0; }
    float *hl=malloc(sizeof(float)*b->n), *hr=malloc(sizeof(float)*b->n);
    bq_t q[4]; for(int k=0;k<4;k++) bq_hp(&q[k],4000.0);
    for(int i=0;i<b->n;i++){ hl[i]=(float)bq_run(&q[1],bq_run(&q[0],b->l[i])); hr[i]=(float)bq_run(&q[3],bq_run(&q[2],b->r[i])); }
    float *e=malloc(sizeof(float)*nw);
    for(int w=0;w<nw;w++){ double s=0; int i0=w*CH+2;
        for(int i=i0;i<i0+CW;i++) s+=hl[i]*hl[i]+hr[i]*hr[i];
        e[w]=(float)(s/(2*CW)); }
    free(hl); free(hr); *E=e; return nw;
}
static float med(const float *e, int a, int b){ /* median of e[a..b) */
    float tmp[64]; int n=0; for(int i=a;i<b;i++) tmp[n++]=e[i];
    if(!n) return 0; qsort(tmp,n,sizeof(float),cmpf); return tmp[n/2];
}
/* scan windows whose START sample lies in [s0,s1) */
static click_t clickscan(const abuf_t *b, int s0, int s1){
    click_t c={0,0,0.0f,-1}; float *E; int nw=hf_profile(b,&E); if(!nw) return c;
    int prev=0;
    for(int w=CCTX+2; w<nw-CCTX-2; w++){
        int st=w*CH; if(st<s0||st>=s1){ prev=0; continue; }
        float ref=fmaxf(med(E,w-CCTX-1,w-1), med(E,w+2,w+CCTX+2));
        float ratio=E[w]/(ref+1e-14f);
        int hit = ratio>4.0f && sqrtf(E[w])>CLICK_ABS;
        if(hit){ c.windows++; if(!prev)c.events++;
            float db=10.0f*log10f(ratio); if(db>c.worst_db){ c.worst_db=db; c.worst_at=st; } }
        prev=hit;
    }
    free(E); return c;
}

/* ── WAV writer (16-bit stereo) for listening ──────────────────────────────── */
static void wav_write(const char *path, const abuf_t *b){
    FILE *f=fopen(path,"wb"); if(!f) return;
    uint32_t n=(uint32_t)b->n, data=n*4, riff=36+data; uint16_t u16; uint32_t u32;
    fwrite("RIFF",1,4,f); fwrite(&riff,4,1,f); fwrite("WAVEfmt ",1,8,f);
    u32=16; fwrite(&u32,4,1,f); u16=1; fwrite(&u16,2,1,f); u16=2; fwrite(&u16,2,1,f);
    u32=FS; fwrite(&u32,4,1,f); u32=FS*4; fwrite(&u32,4,1,f); u16=4; fwrite(&u16,2,1,f); u16=16; fwrite(&u16,2,1,f);
    fwrite("data",1,4,f); fwrite(&data,4,1,f);
    for(uint32_t i=0;i<n;i++){ int16_t s=f2s(b->l[i]); fwrite(&s,2,1,f); s=f2s(b->r[i]); fwrite(&s,2,1,f); }
    fclose(f);
}

/* ── knob-turn control ─────────────────────────────────────────────────────── */
typedef struct {
    const char *level; int knob;       /* page + knob number (1..8) */
    int start;                         /* first block of the turn */
    int mode;                          /* 0 slow detents, 1 fast spin, 2 select stepping */
    int period;                        /* blocks between ticks */
    int ticks;                         /* ticks per direction */
    int delta;                         /* delta per tick */
    int reps;                          /* up/down repetitions */
} turn_t;
static void turn_ctl(palette_t *p, int blk, void *vctx){
    turn_t *t=(turn_t*)vctx;
    if(blk==0) set_param(p,"_level",t->level);
    int rel=blk-t->start; if(rel<0||rel%t->period) return;
    int tick=rel/t->period, per=2*t->ticks;
    if(tick>=per*t->reps) return;
    int k=tick%per; int d=(k<t->ticks)? t->delta : -t->delta;
    char key[24], val[16]; snprintf(key,sizeof key,"knob_%d_adjust",t->knob); snprintf(val,sizeof val,"%d",d);
    set_param(p,key,val);
}

/* ── report ─────────────────────────────────────────────────────────────────── */
static FILE *R;
#define P(...) do{ fprintf(R,__VA_ARGS__); fprintf(stdout,__VA_ARGS__); }while(0)

typedef struct { float lvl_hi, lvl_lo, gain_q; float pk; int clips, nans; float dcmax;
                 float sil_db, floor_db, tail_db; click_t steady, sw[3][2], swm; float cpu;
                 char steady_cfg[24]; float alias_db, noise_db; char where[3][32];
                 click_t rv; float rv_jump, rv_peak; } fxrep_t;

/* Aliasing: 1496.5 Hz sine (bin 139 of 4096, so every harmonic lands on a bin) at
 * -12 dBFS, Amount 1 / Macro .5 / Drift 0. Energy off the harmonic series = aliasing
 * (+ any noise), relative to total. Hann window, +-2 bins of tolerance per harmonic. */
static float alias_probe(int fx){
    const int N=4096, K=139; int n=FS+N;
    abuf_t in=mkbuf(n), out=mkbuf(n);
    for(int i=0;i<n;i++){ float s=0.25f*(float)sin(2*M_PI*(double)K*i/N); in.l[i]=s; in.r[i]=s; }
    palette_t *p=mk(fx,1.0f,0.5f,0.0f); run(p,&in,&out,NULL,NULL); destroy_instance(p);
    double harm=0, tot=0; const float *x=out.l+FS;
    for(int b=1;b<N/2;b++){
        double re=0, im=0;
        for(int i=0;i<N;i++){ double w=0.5-0.5*cos(2*M_PI*i/N), a=2*M_PI*(double)b*i/N; re+=x[i]*w*cos(a); im-=x[i]*w*sin(a); }
        double pw=re*re+im*im; tot+=pw;
        int r=b%K; if(r<=2||r>=K-2) harm+=pw;
    }
    freebuf(&in); freebuf(&out);
    double al=tot-harm; return (float)(10*log10(al/(tot+1e-30)+1e-12));
}
/* Random noise an effect ADDS on top of the music (hiss, static, crackle): run the same
 * input twice with different slot seeds (Drift 0, so only noise sources use the RNG);
 * the difference is pure added noise. dBFS RMS during the music. */
static float noise_probe(int fx){
    int n=3*FS; abuf_t in=mkbuf(n), o1=mkbuf(n), o2=mkbuf(n); gen_music(&in,0,n,0.7f);
    palette_t *p=mk(fx,0.7f,0.5f,0.0f); run(p,&in,&o1,NULL,NULL); destroy_instance(p);
    p=mk(fx,0.7f,0.5f,0.0f); p->slots[0].dsp.seed^=0xA5A5F00Du; run(p,&in,&o2,NULL,NULL); destroy_instance(p);
    double e=0; int c=0;
    for(int i=FS;i<n;i++){ float dl=o1.l[i]-o2.l[i], dr=o1.r[i]-o2.r[i]; e+=dl*dl+dr*dr; c+=2; }
    freebuf(&in); freebuf(&o1); freebuf(&o2);
    return (float)(10*log10(e/c/2.0+1e-20));      /* /2: difference of two independent draws */
}

static const char *PNAME[3]={"Amount","Macro","Drift"};

static void audit_fx(int fx, const char *outdir, fxrep_t *rp){
    memset(rp,0,sizeof *rp); rp->sil_db=-200; rp->floor_db=-200; rp->tail_db=-200; rp->lvl_hi=-200; rp->lvl_lo=-200;
    const float A[3]={0.0f,0.5f,1.0f}, M[3]={0.0f,0.5f,1.0f}, D[2]={0.0f,1.0f};
    int n=(int)(1.5*FS);
    abuf_t sine=mkbuf(n), quiet=mkbuf(n), out=mkbuf(n);
    gen_sine(&sine,0,n,110.0f,0.25f); gen_sine(&quiet,0,n,110.0f,0.25f/16.0f);
    float in_hi=rms_db(&sine,FS/2,n), in_lo=rms_db(&quiet,FS/2,n);
    rp->lvl_hi=-200; rp->lvl_lo=-200;
    /* grid */
    for(int ai=0;ai<3;ai++)for(int mi=0;mi<3;mi++)for(int di=0;di<2;di++){
        palette_t *p=mk(fx,A[ai],M[mi],D[di]); g_clip=0;
        run(p,&sine,&out,NULL,NULL); destroy_instance(p);
        float lv=rms_db(&out,FS/2,n)-in_hi; if(lv>rp->lvl_hi) rp->lvl_hi=lv;
        float pk=peak(&out,FS/2,n); if(pk>rp->pk) rp->pk=pk;
        rp->clips+=g_clip; rp->nans+=nan_count(&out);
        float d=fabsf(dc(&out,FS/2,n)); if(d>rp->dcmax) rp->dcmax=d;
        click_t c=clickscan(&out,FS/2,n);
        if(c.events>rp->steady.events || (c.events==rp->steady.events && c.worst_db>rp->steady.worst_db)){
            rp->steady=c; snprintf(rp->steady_cfg,sizeof rp->steady_cfg,"A%.1f M%.1f D%.0f",A[ai],M[mi],D[di]); }
        p=mk(fx,A[ai],M[mi],D[di]); run(p,&quiet,&out,NULL,NULL); destroy_instance(p);
        float lq=rms_db(&out,FS/2,n)-in_lo; if(lq>rp->lvl_lo) rp->lvl_lo=lq;
        rp->nans+=nan_count(&out);
    }
    freebuf(&sine); freebuf(&quiet); freebuf(&out);

    /* silence / floor: worst over Amount {.5,1} x Drift {0,1}, Macro .5 */
    int ns=4*FS; abuf_t z=mkbuf(ns), fl=mkbuf(ns), o2=mkbuf(ns);
    gen_noise(&fl,0,ns,0.0001f*1.732f);             /* -80 dBFS RMS white */
    float fl_in=rms_db(&fl,2*FS,ns);
    for(int ai=1;ai<3;ai++)for(int di=0;di<2;di++){
        palette_t *p=mk(fx,A[ai],0.5f,D[di]); run(p,&z,&o2,NULL,NULL); destroy_instance(p);
        float s=rms_db(&o2,2*FS,ns); if(s>rp->sil_db) rp->sil_db=s;
        p=mk(fx,A[ai],0.5f,D[di]); run(p,&fl,&o2,NULL,NULL); destroy_instance(p);
        float f=rms_db(&o2,2*FS,ns)-fl_in; if(f>rp->floor_db) rp->floor_db=f;
    }
    freebuf(&z); freebuf(&fl); freebuf(&o2);

    /* tail: music 3 s, then silence; level 3..5 s after stop, Amount .5 / Macro .5 */
    int nt=8*FS; abuf_t mu=mkbuf(nt), ot=mkbuf(nt); gen_music(&mu,0,3*FS,0.7f);
    { palette_t *p=mk(fx,0.5f,0.5f,0.3f); run(p,&mu,&ot,NULL,NULL); destroy_instance(p);
      rp->tail_db=rms_db(&ot,6*FS,8*FS); }
    freebuf(&mu); freebuf(&ot);

    /* knob sweeps on the sine: 0.5 s still, turn, 0.5 s still */
    for(int pi=0;pi<3;pi++)for(int mode=0;mode<2;mode++){
        turn_t t={"FX12", 2+pi, (int)(0.5*FS/BLK), mode, mode?2:4, mode?15:77, mode?3:1, mode?3:1};
        int turn_blocks=t.period*t.ticks*2*t.reps;
        int nb=(int)(0.5*FS/BLK)+turn_blocks+(int)(0.5*FS/BLK);
        int nn=nb*BLK; abuf_t si=mkbuf(nn), so=mkbuf(nn); gen_sine(&si,0,nn,110.0f,0.25f);
        palette_t *p = (pi==0)? mk(fx,0.0f,0.5f,0.0f) : (pi==1)? mk(fx,0.7f,0.0f,0.0f) : mk(fx,0.7f,0.5f,0.0f);
        run(p,&si,&so,turn_ctl,&t); destroy_instance(p);
        rp->nans+=nan_count(&so);
        rp->sw[pi][mode]=clickscan(&so,t.start*BLK,(t.start+turn_blocks)*BLK+CW);
        if(mode==0 && rp->sw[pi][0].events){          /* where in the slow sweep: knob value */
            float tk=(float)(rp->sw[pi][0].worst_at+CW/2 - t.start*BLK)/(float)(t.period*BLK);
            float v=(tk<t.ticks)? tk*0.013f : (2*t.ticks-tk)*0.013f; v=clampf(v,0,1);
            snprintf(rp->where[pi],sizeof rp->where[pi],"%s %s~%.2f",tk<t.ticks?"up":"down",PNAME[pi],v);
        }
        { char path[512]; snprintf(path,sizeof path,"%s/%02d_%s_%s_%s.wav",outdir,fx,FX_NAMES[fx],mode?"spin":"slow",PNAME[pi]); wav_write(path,&so); }
        freebuf(&si); freebuf(&so);
    }
    /* musical sweep render (all three knobs, for listening) */
    {   int nm=10*FS; abuf_t mi=mkbuf(nm), mo=mkbuf(nm); gen_music(&mi,0,nm,0.7f);
        palette_t *p=mk(fx,0.0f,0.0f,0.0f); set_param(p,"_level","FX12");
        int blocks=nm/BLK;
        for(int b=0;b<blocks;b++){
            /* 0-3.3 s Amount up, 3.3-6.6 s Macro up, 6.6-10 s Drift up (slow detents) */
            int seg=b/(blocks/3); int knob=2+(seg>2?2:seg);
            if(b%4==0 && (b%(blocks/3))<4*77){ char key[24]; snprintf(key,sizeof key,"knob_%d_adjust",knob); set_param(p,key,"1"); }
            int16_t buf[BLK*2]; int off=b*BLK;
            for(int i=0;i<BLK;i++){ buf[2*i]=f2s(mi.l[off+i]); buf[2*i+1]=f2s(mi.r[off+i]); }
            process_block(p,buf,BLK);
            for(int i=0;i<BLK;i++){ mo.l[off+i]=buf[2*i]/32768.0f; mo.r[off+i]=buf[2*i+1]/32768.0f; }
        }
        destroy_instance(p);
        char path[512]; snprintf(path,sizeof path,"%s/%02d_%s_music.wav",outdir,fx,FX_NAMES[fx]); wav_write(path,&mo);
        freebuf(&mi); freebuf(&mo);
    }
    /* random value jumps: all three params re-rolled every 300 ms (like Rnd Values) */
    {   int nn=7*FS; abuf_t si=mkbuf(nn), so=mkbuf(nn); gen_sine(&si,0,nn,110.0f,0.25f);
        palette_t *p=mk(fx,0.5f,0.5f,0.0f); uint32_t rs=0x1234567u+fx;
        int16_t buf[BLK*2]; int blocks=nn/BLK;
        for(int b=0;b<blocks;b++){
            if(b>=172 && (b-172)%103==0){ slot_t *sl=&p->slots[0];
                rs^=rs<<13; rs^=rs>>17; rs^=rs<<5; sl->amount=0.3f+0.6f*(rs>>8)/16777216.0f;
                rs^=rs<<13; rs^=rs>>17; rs^=rs<<5; sl->macro=(rs>>8)/16777216.0f;
                rs^=rs<<13; rs^=rs>>17; rs^=rs<<5; float d=(rs>>8)/16777216.0f; sl->drift=d*d*0.7f; }
            int off=b*BLK; for(int i=0;i<BLK;i++){ buf[2*i]=f2s(si.l[off+i]); buf[2*i+1]=f2s(si.r[off+i]); }
            process_block(p,buf,BLK);
            for(int i=0;i<BLK;i++){ so.l[off+i]=buf[2*i]/32768.0f; so.r[off+i]=buf[2*i+1]/32768.0f; }
        }
        destroy_instance(p);
        int ne=blocks*BLK;                              /* processed length (whole blocks) */
        rp->rv=clickscan(&so,172*BLK,ne-2*CW); rp->rv_peak=peak(&so,0,ne);
        float mj=0; for(int i=172*BLK;i<ne;i++){ float d=fabsf(so.l[i]-so.l[i-1]); if(d>mj)mj=d; } rp->rv_jump=mj;
        char path[512]; snprintf(path,sizeof path,"%s/%02d_%s_rndvalues.wav",outdir,fx,FX_NAMES[fx]); wav_write(path,&so);
        freebuf(&si); freebuf(&so);
    }
    rp->alias_db=alias_probe(fx);
    rp->noise_db=noise_probe(fx);
    /* cpu */
    {   palette_t *p=mk(fx,1.0f,0.5f,1.0f); int16_t buf[BLK*2];
        for(int i=0;i<BLK*2;i++) buf[i]=(int16_t)(8000*sin(i*0.05));
        struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
        for(int k=0;k<3000;k++){ process_block(p,buf,BLK); for(int i=0;i<BLK*2;i++) buf[i]=(int16_t)(8000*sin((i+k)*0.05)); }
        clock_gettime(CLOCK_MONOTONIC,&t1); destroy_instance(p);
        rp->cpu=(float)((t1.tv_sec-t0.tv_sec)*1e6+(t1.tv_nsec-t0.tv_nsec)/1e3)/3000.0f;
    }
}

/* Select stepping: walk fx1 Select up through every effect then back, one detent per
 * ~150 ms, on sine+music; report the worst transition(s). */
static void audit_switch(const char *outdir){
    int per=52, steps=24; int start=(int)(0.5*FS/BLK);
    int nb=start+per*steps*2+start; int nn=nb*BLK;
    abuf_t in=mkbuf(nn), out=mkbuf(nn); gen_sine(&in,0,nn,110.0f,0.25f);
    palette_t *p=mk(PFX_OFF,0.6f,0.5f,0.3f);
    turn_t t={"FX12",1,start,2,per,steps,1,1};
    run(p,&in,&out,turn_ctl,&t); destroy_instance(p);
    char path[512]; snprintf(path,sizeof path,"%s/select_sweep.wav",outdir); wav_write(path,&out);
    /* reference = the same run's music-only HF profile is busy; so scan each switch
     * point against its own surroundings */
    P("\n== Select switching (Amount .6 / Macro .5 / Drift .3, one detent per 150 ms) ==\n");
    int bad=0;
    for(int k=0;k<steps*2;k++){
        int up=k<steps; int from=up?k:(2*steps-k), to=up?k+1:(2*steps-k-1);
        int at=(start+k*per)*BLK;
        click_t c=clickscan(&out,at-CW,at+CW);
        if(c.events){ bad++; P("  %-12s -> %-12s  click +%.1f dB\n",FX_NAMES[from],FX_NAMES[to],c.worst_db); }
    }
    if(!bad) P("  no clicks at any transition\n");
    freebuf(&in); freebuf(&out);
}

/* FX Reorder: two order-sensitive slots, flip the order every 150 ms */
static void reorder_ctl(palette_t *p, int blk, void *ctx){ (void)ctx;
    if(blk>=172 && blk%52==0) p->fx_reorder = (p->fx_reorder==0)? 6 : 0; }  /* 1-2-3-4 <-> 2-1-3-4 */
static void audit_reorder(const char *outdir){
    int nn=4*FS; abuf_t in=mkbuf(nn), out=mkbuf(nn); gen_sine(&in,0,nn,110.0f,0.25f);
    palette_t *p=mk(PFX_PITCH,0.8f,0.8f,0.0f);
    slot_apply_select(p,1,PFX_CASCADE); slot_engage(p,1); slot_t *s=&p->slots[1]; s->amount=s->amt_sm=0.6f; s->macro=s->mac_sm=0.3f; s->ramp=1.0f;
    run(p,&in,&out,reorder_ctl,NULL); destroy_instance(p);
    click_t c=clickscan(&out,172*BLK,nn);
    P("\n== FX Reorder flips (Pitch <-> Cascade, every 150 ms) ==\n  %d click events, worst +%.1f dB\n",c.events,c.worst_db);
    char path[512]; snprintf(path,sizeof path,"%s/reorder_flips.wav",outdir); wav_write(path,&out);
    freebuf(&in); freebuf(&out);
}

/* Global knobs: Mix / Input Volume fast spins; preset browsing; randomizers */
static void audit_globals(const char *outdir){
    P("\n== Global knobs (Drive .6 in slot 1) ==\n");
    const char *nm[2]={"Input Volume","Mix"}; int kn[2]={7,8};
    for(int g=0;g<2;g++){
        turn_t t={"Presets",kn[g],172,1,2,15,-3,3};
        int nn=4*FS; abuf_t in=mkbuf(nn), out=mkbuf(nn); gen_sine(&in,0,nn,110.0f,0.25f);
        palette_t *p=mk(PFX_DRIVE,0.6f,0.5f,0.0f); run(p,&in,&out,turn_ctl,&t); destroy_instance(p);
        click_t c=clickscan(&out,172*BLK,nn-CW);
        P("  %-13s fast spin : %d click events, worst +%.1f dB\n",nm[g],c.events,c.worst_db);
        freebuf(&in); freebuf(&out);
    }
    {   /* presets: one detent every 300 ms through all presets */
        turn_t t={"Presets",1,172,2,103,NUM_PRESETS-1,1,1};
        int nn=(172+103*(NUM_PRESETS-1)*2+172)*BLK; abuf_t in=mkbuf(nn), out=mkbuf(nn);
        gen_sine(&in,0,nn,110.0f,0.25f);
        palette_t *p=create_instance(NULL,NULL); g_clip=0; run(p,&in,&out,turn_ctl,&t); destroy_instance(p);
        P("  Preset browse (all %d, up+down, sine): %d clipped samples, %d NaN\n",NUM_PRESETS,g_clip,nan_count(&out));
        click_t c=clickscan(&out,172*BLK,nn-CW);
        P("  Preset browse clicks: %d events, worst +%.1f dB\n",c.events,c.worst_db);
        char path[512]; snprintf(path,sizeof path,"%s/preset_browse.wav",outdir); wav_write(path,&out);
        freebuf(&in); freebuf(&out);
    }
}

/* Randomizers: fire each one repeatedly on music + sine, count clicks */
typedef struct { const char *key; int every; } rnd_t;
static void rnd_ctl(palette_t *p, int blk, void *ctx){ rnd_t *r=(rnd_t*)ctx;
    if(blk>=172 && (blk-172)%r->every==0) set_param(p,r->key,"1"); }
static void audit_randomizers(void){
    P("\n== Randomizers (fired every 400 ms for 8 s, sine) ==\n");
    const char *keys[5]={"rnd_patch","rnd_effect","rnd_amount","rnd_macro","rnd_values"};
    for(int k=0;k<5;k++){
        int nn=9*FS; abuf_t in=mkbuf(nn), out=mkbuf(nn); gen_sine(&in,0,nn,110.0f,0.25f);
        palette_t *p=create_instance(NULL,NULL);
        for(int s=0;s<NUM_SLOTS;s++){ slot_t *sl=&p->slots[s]; sl->amt_sm=sl->amount; sl->mac_sm=sl->macro; sl->drf_sm=sl->drift; sl->ramp=1; }
        p->rnd_phase=0; p->rnd_gain=1; p->pending_preset=-1;
        rnd_t r={keys[k],138}; g_clip=0; run(p,&in,&out,rnd_ctl,&r); destroy_instance(p);
        { char path[512]; snprintf(path,sizeof path,"/build/tests/out/%s.wav",keys[k]); wav_write(path,&out); }
        click_t c=clickscan(&out,172*BLK,nn-CW);
        P("  %-11s %3d click events, worst +%5.1f dB, %d clipped, %d NaN\n",keys[k],c.events,c.worst_db,g_clip,nan_count(&out));
        freebuf(&in); freebuf(&out);
    }
}

/* every preset, static, music in: level + clipping + self-noise after the music */
static void audit_presets(void){
    P("\n== Presets (music at -9 dBFS RMS for 3 s, then 4 s of silence) ==\n");
    P("  %-4s %-28s %8s %8s %8s %6s\n","#","slots","out dB","peak","tail dB","clips");
    int nn=7*FS; abuf_t in=mkbuf(nn), out=mkbuf(nn); gen_music(&in,0,3*FS,0.7f);
    float in_db=rms_db(&in,FS,3*FS);
    for(int k=1;k<=NUM_PRESETS;k++){
        palette_t *p=create_instance(NULL,NULL); load_preset(p,k);
        for(int s=0;s<NUM_SLOTS;s++){ slot_t *sl=&p->slots[s]; sl->amt_sm=sl->amount; sl->mac_sm=sl->macro; sl->drf_sm=sl->drift; sl->ramp=1; }
        p->mix_sm=p->mix; p->iv_sm=p->input_vol; p->fb_sm=p->feedback; p->rnd_phase=0; p->rnd_gain=1; p->pending_preset=-1;
        g_clip=0; run(p,&in,&out,NULL,NULL);
        char sl[64]; snprintf(sl,sizeof sl,"%s>%s>%s>%s",FX_NAMES[p->slots[0].select],FX_NAMES[p->slots[1].select],FX_NAMES[p->slots[2].select],FX_NAMES[p->slots[3].select]);
        destroy_instance(p);
        float o=rms_db(&out,FS,3*FS)-in_db, pk=peak(&out,0,nn), tl=rms_db(&out,5*FS,7*FS);
        int flag = g_clip>0 || o>6.0f || o<-9.0f;
        P("  %-4d %-28.28s %+8.1f %8.2f %8.1f %6d%s\n",k,sl,o,pk,tl,g_clip,flag?"  <--":"");
    }
    freebuf(&in); freebuf(&out);
}

/* Detector calibration: a -12 dBFS 110 Hz sine with one injected step of each size. */
static void detector_selftest(void){
    P("Detector self-test (step injected into a -12 dBFS 110 Hz sine): ");
    const float steps[5]={0.0005f,0.001f,0.002f,0.004f,0.016f};
    for(int k=0;k<5;k++){
        int n=2*FS; abuf_t b=mkbuf(n); gen_sine(&b,0,n,110.0f,0.25f);
        for(int i=FS;i<n;i++){ b.l[i]+=steps[k]; b.r[i]+=steps[k]; }
        click_t c=clickscan(&b,FS/2,3*FS/2);
        P("%.4f->%s(%+.0f dB)  ",steps[k],c.events?"HIT":"miss",c.worst_db); freebuf(&b);
    }
    { int n=2*FS; abuf_t b=mkbuf(n); gen_sine(&b,0,n,110.0f,0.25f); click_t c=clickscan(&b,FS/2,3*FS/2);
      P("| clean sine: %d events\n\n",c.events); freebuf(&b); }
}

int main(int argc, char **argv){
    const char *outdir = argc>1? argv[1] : "tests/out";
    char rpath[512]; snprintf(rpath,sizeof rpath,"%s/report.txt",outdir);
    R=fopen(rpath,"w"); if(!R){ perror(rpath); return 1; }
    /* probe mode: fx_audit <outdir> probe <fx> <amount> <macro> <drift> -> probe.wav */
    if(argc>6 && !strcmp(argv[2],"probe")){
        int fx=atoi(argv[3]); int n=3*FS; abuf_t in=mkbuf(n), out=mkbuf(n); gen_sine(&in,0,n,110.0f,0.25f);
        palette_t *p=mk(fx,(float)atof(argv[4]),(float)atof(argv[5]),(float)atof(argv[6]));
        run(p,&in,&out,NULL,NULL); destroy_instance(p);
        char path[512]; snprintf(path,sizeof path,"%s/probe.wav",outdir); wav_write(path,&out);
        click_t c=clickscan(&out,FS/2,n); printf("probe %s: %d events worst +%.1f dB at %.3f s\n",FX_NAMES[fx],c.events,c.worst_db,c.worst_at/(float)FS);
        return 0;
    }
    int only = argc>2? atoi(argv[2]) : 0;
    detector_selftest();
    P("PALETTE QoL audit — slot 1, Mix 100%%, int16 I/O, 128-frame blocks\n");
    P("lvl: worst output vs input on a -12 dBFS sine / -36 dBFS sine (all Amount/Macro/Drift)\n");
    P("sil: self-generated output with digital-zero input   floor: gain on a -80 dBFS noise floor\n");
    P("tail: level 3-5 s after music stops (Amt .5)   clicks: events / worst dB over context\n\n");
    P("%-3s %-12s %6s %6s %5s %5s %3s %6s %6s %6s %6s %6s %6s | %-15s | %-9s %-9s | %-9s %-9s | %-9s %-9s | %5s\n",
      "id","effect","lvl-12","lvl-36","peak","clips","NaN","DC","alias","noise","sil","floor","tail","steady (cfg)",
      "Amt slow","Amt spin","Mac slow","Mac spin","Drf slow","Drf spin","us");
    fxrep_t rep[PFX_COUNT];
    for(int fx=1;fx<PFX_COUNT;fx++){
        if(only && fx!=only) continue;     /* only<0: skip to the global tests */
        audit_fx(fx,outdir,&rep[fx]); fxrep_t *r=&rep[fx];
        char c[7][24];
        snprintf(c[0],24,"%d/%+.0f %s",r->steady.events,r->steady.worst_db,r->steady.events?r->steady_cfg:"");
        for(int pi=0;pi<3;pi++)for(int m=0;m<2;m++) snprintf(c[1+pi*2+m],24,"%d/%+.0f",r->sw[pi][m].events,r->sw[pi][m].worst_db);
        P("%-3d %-12s %+6.1f %+6.1f %5.2f %5d %3d %6.3f %6.1f %6.1f %6.1f %+6.1f %6.1f | %-15s | %-9s %-9s | %-9s %-9s | %-9s %-9s | %5.1f\n",
          fx,FX_NAMES[fx],r->lvl_hi,r->lvl_lo,r->pk,r->clips,r->nans,r->dcmax,r->alias_db,r->noise_db,r->sil_db,r->floor_db,r->tail_db,
          c[0],c[1],c[2],c[3],c[4],c[5],c[6],r->cpu);
        for(int pi=0;pi<3;pi++) if(r->where[pi][0]) P("      worst slow-sweep click: %s\n",r->where[pi]);
        P("      random values: %d events (worst %+.0f dB), largest 1-sample jump %.3f, peak %.2f\n",r->rv.events,r->rv.worst_db,r->rv_jump,r->rv_peak);
        fflush(R); fflush(stdout);
    }
    if(only<=0){ audit_switch(outdir); audit_reorder(outdir); audit_globals(outdir); audit_randomizers(); audit_presets(); }
    fclose(R); return 0;
}
