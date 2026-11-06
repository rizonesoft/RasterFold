/* Integer-equivalent fallback for the x86 intrinsics used by method 1.
 * RF_PORTABLE_SIMD exercises this path on x64 too. Unsigned operations retain
 * the exact 32-bit wrap behavior. The compiler may vectorize these C loops. */
#ifndef RF_SIMD_H
#define RF_SIMD_H
#if (defined(__x86_64__) || defined(__i386__)) && !defined(RF_PORTABLE_SIMD)
#include <smmintrin.h>
#else
typedef union { uint32_t u[4]; int32_t i[4]; int16_t h[8]; uint8_t b[16]; } __m128i;
#define _MM_SHUFFLE(a,b,c,d) (((a)<<6)|((b)<<4)|((c)<<2)|(d))
#define _MM_HINT_T0 0
RF_INLINE __m128i _mm_setr_epi32(int a, int b, int c, int d) { __m128i r = {.i={a,b,c,d}}; return r; }
RF_INLINE __m128i _mm_set1_epi32(int a) { return _mm_setr_epi32(a,a,a,a); }
RF_INLINE __m128i _mm_setzero_si128(void) { return _mm_set1_epi32(0); }
RF_INLINE __m128i _mm_setr_epi16(int a,int b,int c,int d,int e,int f,int g,int h) {
    __m128i r = {.h={(int16_t)a,(int16_t)b,(int16_t)c,(int16_t)d,(int16_t)e,(int16_t)f,(int16_t)g,(int16_t)h}}; return r;
}
RF_INLINE __m128i _mm_set1_epi16(int a) { return _mm_setr_epi16(a,a,a,a,a,a,a,a); }
RF_INLINE __m128i _mm_setr_epi8(int a,int b,int c,int d,int e,int f,int g,int h,
                               int i,int j,int k,int l,int m,int n,int o,int p) {
    __m128i r = {.b={(uint8_t)a,(uint8_t)b,(uint8_t)c,(uint8_t)d,(uint8_t)e,(uint8_t)f,(uint8_t)g,(uint8_t)h,
                    (uint8_t)i,(uint8_t)j,(uint8_t)k,(uint8_t)l,(uint8_t)m,(uint8_t)n,(uint8_t)o,(uint8_t)p}}; return r;
}
RF_INLINE __m128i _mm_loadu_si128(const __m128i *p) { __m128i r; memcpy(&r,p,16); return r; }
RF_INLINE __m128i _mm_load_si128(const __m128i *p) { return _mm_loadu_si128(p); }
RF_INLINE void _mm_storeu_si128(__m128i *p, __m128i a) { memcpy(p,&a,16); }
RF_INLINE void _mm_store_si128(__m128i *p, __m128i a) { _mm_storeu_si128(p,a); }
RF_INLINE __m128i _mm_add_epi32(__m128i a,__m128i b) { for(int i=0;i<4;i++) a.u[i]+=b.u[i]; return a; }
RF_INLINE __m128i _mm_sub_epi32(__m128i a,__m128i b) { for(int i=0;i<4;i++) a.u[i]-=b.u[i]; return a; }
RF_INLINE __m128i _mm_mullo_epi32(__m128i a,__m128i b) { for(int i=0;i<4;i++) a.u[i]*=b.u[i]; return a; }
RF_INLINE __m128i _mm_cmpgt_epi32(__m128i a,__m128i b) { for(int i=0;i<4;i++) a.i[i]=a.i[i]>b.i[i]?-1:0; return a; }
RF_INLINE __m128i _mm_min_epi32(__m128i a,__m128i b) { for(int i=0;i<4;i++) if(b.i[i]<a.i[i]) a.i[i]=b.i[i]; return a; }
RF_INLINE __m128i _mm_max_epi32(__m128i a,__m128i b) { for(int i=0;i<4;i++) if(b.i[i]>a.i[i]) a.i[i]=b.i[i]; return a; }
RF_INLINE __m128i _mm_sign_epi32(__m128i a,__m128i b) { for(int i=0;i<4;i++) a.u[i]=b.i[i]<0?0u-a.u[i]:b.i[i]?a.u[i]:0; return a; }
RF_INLINE __m128i _mm_abs_epi32(__m128i a) { for(int i=0;i<4;i++) if(a.i[i]<0) a.u[i]=0u-a.u[i]; return a; }
RF_INLINE __m128i _mm_slli_epi32(__m128i a,int n) { for(int i=0;i<4;i++) a.u[i]<<=n; return a; }
RF_INLINE __m128i _mm_srli_epi32(__m128i a,int n) { for(int i=0;i<4;i++) a.u[i]>>=n; return a; }
RF_INLINE __m128i _mm_srai_epi32(__m128i a,int n) { for(int i=0;i<4;i++) a.i[i]>>=n; return a; }
RF_INLINE __m128i _mm_unpacklo_epi16(__m128i a,__m128i b) { return _mm_setr_epi16(a.h[0],b.h[0],a.h[1],b.h[1],a.h[2],b.h[2],a.h[3],b.h[3]); }
RF_INLINE __m128i _mm_unpackhi_epi16(__m128i a,__m128i b) { return _mm_setr_epi16(a.h[4],b.h[4],a.h[5],b.h[5],a.h[6],b.h[6],a.h[7],b.h[7]); }
RF_INLINE __m128i _mm_shuffle_epi32(__m128i a,int n) { __m128i r; for(int i=0;i<4;i++) r.u[i]=a.u[(n>>(2*i))&3]; return r; }
RF_INLINE __m128i _mm_shuffle_epi8(__m128i a,__m128i b) { __m128i r; for(int i=0;i<16;i++) r.b[i]=b.b[i]&128?0:a.b[b.b[i]&15]; return r; }
RF_INLINE __m128i _mm_madd_epi16(__m128i a,__m128i b) {
    __m128i r; for(size_t i=0;i<4;i++) r.u[i]=(uint32_t)((int32_t)a.h[i*2]*b.h[i*2])+(uint32_t)((int32_t)a.h[i*2+1]*b.h[i*2+1]); return r;
}
RF_INLINE int _mm_cvtsi128_si32(__m128i a) { return a.i[0]; }
RF_INLINE __m128i _mm_loadl_epi64(const __m128i *p) { __m128i r = _mm_setzero_si128(); memcpy(&r, p, 8); return r; }
RF_INLINE void _mm_storel_epi64(__m128i *p, __m128i a) { memcpy(p, &a, 8); }
RF_INLINE __m128i _mm_cvtepu16_epi32(__m128i a) {
    return _mm_setr_epi32((uint16_t)a.h[0], (uint16_t)a.h[1], (uint16_t)a.h[2], (uint16_t)a.h[3]);
}
RF_INLINE __m128i _mm_min_epu16(__m128i a, __m128i b) {
    for (int i = 0; i < 8; i++) if ((uint16_t)b.h[i] < (uint16_t)a.h[i]) a.h[i] = b.h[i];
    return a;
}
RF_INLINE __m128i _mm_xor_si128(__m128i a, __m128i b) { for (int i = 0; i < 4; i++) a.u[i] ^= b.u[i]; return a; }
RF_INLINE __m128i _mm_and_si128(__m128i a, __m128i b) { for (int i = 0; i < 4; i++) a.u[i] &= b.u[i]; return a; }
RF_INLINE __m128i _mm_mulhi_epu16(__m128i a, __m128i b) {
    for (int i = 0; i < 8; i++) a.h[i] = (int16_t)(((uint32_t)(uint16_t)a.h[i] * (uint16_t)b.h[i]) >> 16);
    return a;
}
RF_INLINE __m128i _mm_mullo_epi16(__m128i a, __m128i b) {
    for (int i = 0; i < 8; i++) a.h[i] = (int16_t)((uint32_t)(uint16_t)a.h[i] * (uint16_t)b.h[i]);
    return a;
}
RF_INLINE __m128i _mm_cmpeq_epi16(__m128i a, __m128i b) {
    for (int i = 0; i < 8; i++) a.h[i] = (a.h[i] == b.h[i]) ? -1 : 0;
    return a;
}
RF_INLINE __m128i _mm_add_epi16(__m128i a, __m128i b) {
    for (int i = 0; i < 8; i++) a.h[i] = (int16_t)((uint16_t)a.h[i] + (uint16_t)b.h[i]);
    return a;
}
RF_INLINE __m128i _mm_sub_epi16(__m128i a, __m128i b) {
    for (int i = 0; i < 8; i++) a.h[i] = (int16_t)((uint16_t)a.h[i] - (uint16_t)b.h[i]);
    return a;
}
RF_INLINE __m128i _mm_adds_epu16(__m128i a, __m128i b) {
    for (int i = 0; i < 8; i++) {
        uint32_t s = (uint32_t)(uint16_t)a.h[i] + (uint16_t)b.h[i];
        a.h[i] = (int16_t)(s > 65535u ? 65535u : s);
    }
    return a;
}
RF_INLINE __m128i _mm_packus_epi32(__m128i a, __m128i b) {
    __m128i r;
    for (int i = 0; i < 4; i++) {
        int32_t va = a.i[i], vb = b.i[i];
        r.h[i] = (int16_t)(va < 0 ? 0 : va > 65535 ? 65535 : va);
        r.h[i + 4] = (int16_t)(vb < 0 ? 0 : vb > 65535 ? 65535 : vb);
    }
    return r;
}
RF_INLINE int _mm_extract_epi16(__m128i a, int imm) { return (uint16_t)a.h[imm & 7]; }
RF_INLINE void _mm_prefetch(const char *p,int hint) { (void)hint; __builtin_prefetch(p); }
RF_INLINE void _mm_pause(void) { __atomic_signal_fence(__ATOMIC_SEQ_CST); }
#endif
#endif
