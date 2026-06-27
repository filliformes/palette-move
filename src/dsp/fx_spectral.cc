/* PALETTE — FREEZE: true spectral magnitude freeze.
 * Author: Filliformes. License: MIT.
 *
 * Built on Signalsmith-Audio/dsp WindowedFFT (Geraint Luff, MIT, header-only) —
 * a Modified-Real-FFT block transform with built-in windowing. We drive our own
 * 75%-overlap OLA (per the Schwung spectral lessons: hop = N/4, window applied on
 * analysis+synthesis, COLA-normalized) and hold per-bin magnitudes with fresh
 * random phase each frame (PaulStretch-style) for a smooth frozen cloud.
 *
 * Isolated TU: only the signalsmith include root is on this file's path
 * (-Ivendor/signalsmith). Exposed to palette.c via fx_clouds.cc's heavy dispatch.
 *
 *   amount = freeze depth (0 = dry, 1 = fully frozen)
 *   macro  = spectral blur (smear magnitudes across bins)
 *   drift  = spectral drift (slow magnitude wander)
 */

#include <new>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex>
#include <vector>

#include "spectral.h"

using signalsmith::spectral::WindowedFFT;
typedef std::complex<float> Cplx;

enum { FX_FREEZE = 24 };
#define SP_N    1024
#define SP_HOP  (SP_N/4)
#define SP_BINS (SP_N/2)
#ifndef SP_TWO_PI
#define SP_TWO_PI 6.28318530718f
#endif

static inline uint32_t sprng(uint32_t *s){ uint32_t x=*s; x^=x<<13; x^=x>>17; x^=x<<5; *s=x; return x; }
static inline float sprandf(uint32_t *s){ return (sprng(s)>>8)*(1.0f/16777216.0f); }
static inline float spclampf(float x,float lo,float hi){ return x<lo?lo:(x>hi?hi:x); }
static inline float splerp(float a,float b,float t){ return a+(b-a)*t; }

struct SpectralState {
    int            kind;
    WindowedFFT<float> wfft;
    float          inHist[2][SP_N];
    float          olaBuf[2][SP_N];
    float          heldMag[2][SP_BINS];
    std::vector<Cplx> spec[2];
    int            iw, rp, hopCount;
    int            frozen;
    float          olaNorm;
    uint32_t       seed[2];
    float          tw[SP_N];                /* time-domain frame scratch */
};

static SpectralState *spectral_new(){
    SpectralState *st = new(std::nothrow) SpectralState;
    if(!st) return NULL;
    st->kind=FX_FREEZE;
    st->wfft.setSize(SP_N);                  /* Blackman-Harris window */
    memset(st->inHist,0,sizeof(st->inHist));
    memset(st->olaBuf,0,sizeof(st->olaBuf));
    memset(st->heldMag,0,sizeof(st->heldMag));
    st->spec[0].assign(SP_BINS, Cplx(0,0));
    st->spec[1].assign(SP_BINS, Cplx(0,0));
    st->iw=0; st->rp=0; st->hopCount=0; st->frozen=0;
    st->seed[0]=0x13572468u; st->seed[1]=0x2468aceFu;   /* decorrelated L/R */
    /* COLA normalization: steady-state sum of window² over the 4 hop shifts */
    const std::vector<float> &w = st->wfft.window();
    float acc=0.0f;
    for(int m=0;m<SP_N/SP_HOP;m++){ int idx=(SP_HOP/2 + m*SP_HOP)%SP_N; acc += w[idx]*w[idx]; }
    st->olaNorm = (acc>1e-6f)? 1.0f/acc : 1.0f;
    return st;
}
static void spectral_free(SpectralState *st){ if(st) delete st; }

static void spectral_process(SpectralState *st, float *l, float *r, int n,
                             float amount, float macro, float drift){
    int freeze_on = amount > 0.02f;
    int blur = (int)(macro*macro*6.0f);            /* magnitude blur passes */
    for(int i=0;i<n;i++){
        /* push input into per-channel history */
        st->inHist[0][st->iw]=l[i];
        st->inHist[1][st->iw]=r[i];
        st->iw=(st->iw+1)%SP_N;

        if(++st->hopCount >= SP_HOP){
            st->hopCount=0;
            for(int c=0;c<2;c++){
                /* assemble chronological frame (oldest→newest) */
                for(int j=0;j<SP_N;j++) st->tw[j]=st->inHist[c][(st->iw+j)%SP_N];
                st->wfft.fft(st->tw, st->spec[c]);
                if(!st->frozen){
                    for(int k=0;k<SP_BINS;k++) st->heldMag[c][k]=std::abs(st->spec[c][k]);
                } else {
                    /* spectral drift: slow magnitude wander */
                    if(drift>0.0f){
                        for(int k=0;k<SP_BINS;k++){
                            float w2=(sprandf(&st->seed[c])-0.5f)*drift*0.04f;
                            st->heldMag[c][k]*=(1.0f+w2);
                        }
                    }
                    /* magnitude blur (smear across bins) */
                    for(int b=0;b<blur;b++){
                        float prev=st->heldMag[c][0];
                        for(int k=1;k<SP_BINS-1;k++){
                            float cur=st->heldMag[c][k];
                            st->heldMag[c][k]=(prev+cur+st->heldMag[c][k+1])*0.3333333f;
                            prev=cur;
                        }
                    }
                    /* rebuild spectrum: held magnitude + fresh random phase */
                    for(int k=0;k<SP_BINS;k++){
                        float th=SP_TWO_PI*sprandf(&st->seed[c]);
                        st->spec[c][k]=Cplx(st->heldMag[c][k]*cosf(th),
                                            st->heldMag[c][k]*sinf(th));
                    }
                    /* IFFT (windowed, 1/N scaled) + overlap-add */
                    st->wfft.ifft(st->spec[c], st->tw);
                    for(int j=0;j<SP_N;j++)
                        st->olaBuf[c][(st->rp+j)%SP_N] += st->tw[j]*st->olaNorm;
                }
            }
        }
        st->frozen = freeze_on;                    /* hold from now on */

        float oL=st->olaBuf[0][st->rp], oR=st->olaBuf[1][st->rp];
        st->olaBuf[0][st->rp]=0.0f; st->olaBuf[1][st->rp]=0.0f;
        st->rp=(st->rp+1)%SP_N;
        if(oL!=oL) oL=0.0f; if(oR!=oR) oR=0.0f;    /* NaN guard */
        oL=spclampf(oL,-1.5f,1.5f); oR=spclampf(oR,-1.5f,1.5f);
        l[i]=splerp(l[i],oL,amount);
        r[i]=splerp(r[i],oR,amount);
    }
}

/* ── interface used by fx_clouds.cc heavy dispatch ──────────────────────────── */
extern "C" void *pfx_spectral_alloc(float sr){ (void)sr; return (void*)spectral_new(); }
extern "C" void  pfx_spectral_free(void *h){ spectral_free((SpectralState*)h); }
extern "C" void  pfx_spectral_process(void *h, float *l, float *r, int n,
                                      float amount, float macro, float drift){
    if(h) spectral_process((SpectralState*)h, l, r, n, amount, macro, drift);
}
