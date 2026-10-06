/*
 * felgpu - Dolby Vision FEL composition on OpenGL ES 3.2
 *
 * Composes a decoded base layer (dvfel linear 10-bit 4:4:4 buffer, see
 * dvfel_uapi.h) with the enhancement layer and the RPU mapping into the
 * same buffer format:
 *
 *   VDR = predict(BL) + NLQ^-1(upsample(EL))
 *
 * followed by quantization to 10 bits for display through a profile 8.1
 * RPU with no-op mapping. The math follows fel_compose.c (Dolby layered HDR
 * patent US10701399B2); the EL upsampling filter is Lanczos-3.
 *
 * All calls need the caller's GLES 3.2 context to be current.
 */
#ifndef FELGPU_H
#define FELGPU_H

#include <stdint.h>
#include <GLES3/gl32.h>

#define FELGPU_MAX_PIECES 8

#ifdef __cplusplus
extern "C" {
#endif

/* composition metadata of one frame, as carried by the RPU */
struct felgpu_meta {
    int bl_bits;                       /* bl_bit_depth */
    int el_bits;                       /* el_bit_depth */
    int use_el;                        /* residual present and EL available */
    int npiv[3];                       /* per component Y, Cb, Cr */
    int pivots[3][FELGPU_MAX_PIECES + 1];              /* BL codewords */
    int mapping[3][FELGPU_MAX_PIECES];                 /* 0 polynomial, 1 MMR */
    double poly[3][FELGPU_MAX_PIECES][3];              /* x^0, x^1, x^2 */
    int mmr_order[3][FELGPU_MAX_PIECES];
    double mmr_const[3][FELGPU_MAX_PIECES];
    double mmr[3][FELGPU_MAX_PIECES][3][7];            /* [order][term] */
    int nlq_offset[3];                 /* EL codewords */
    double nlq_slope[3], nlq_thresh[3], nlq_max[3];    /* normalized */
};

struct felgpu;

/* w, h: base layer size; the EL is w/2 x h/2 */
/* taps: EL upsampling filter, 6 (Lanczos-3) or 4 (Lanczos-2) */
struct felgpu *felgpu_create(int w, int h, int taps);
void felgpu_destroy(struct felgpu *f);

/* EL planes, yuv420p10 (16-bit samples), strides in samples */
void felgpu_set_el(struct felgpu *f, const uint16_t *y, int y_stride,
                   const uint16_t *cb, const uint16_t *cr, int c_stride);
/*
 * hardware decoded EL: the EL picture as a (w/2 x h/2) texture of packed
 * words laid out like the BL (see dvfel_uapi.h), instead of felgpu_set_el
 */
void felgpu_set_el_tex(struct felgpu *f, GLuint el);
void felgpu_set_meta(struct felgpu *f, const struct felgpu_meta *m);

/* compose bl_tex (dma-buf import, RGBA8) into out_fbo (dma-buf, RGBA8) */
void felgpu_run(struct felgpu *f, GLuint bl_tex, GLuint out_fbo);

/*
 * 10-bit output quantization: 0 rounding (default), 1 static blue noise
 * dither of one code value, 2 dither on the left half only (comparison)
 */
void felgpu_set_dither(struct felgpu *f, int mode);

/*
 * demo (1): the composition on the left half only; the right half shows the
 * untouched base layer, as the display shows it after a P7 -> P8.1 conversion
 * with a no-op mapping (CoreELEC without FEL composition)
 */
void felgpu_set_demo(struct felgpu *f, int on);

/*
 * test pattern instead of the composition (1): 12-bit grey ramps in 4 bands,
 * dark (10-bit codes 160-240) and mid (330-410); with dither on, the upper
 * band of each pair is dithered and the lower one rounded
 */
void felgpu_set_pattern(struct felgpu *f, int pattern);

/* per pass timings (ms, averaged) if profiling is enabled; forces glFinish */
void felgpu_set_profile(struct felgpu *f, int on);
void felgpu_print_profile(struct felgpu *f);

#ifdef __cplusplus
}
#endif

#endif
