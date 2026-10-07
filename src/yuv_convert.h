#pragma once
/*
 * NV12/NV21 (YUV 4:2:0 semi-planar, limited range, BT.709) -> ABGR8888.
 * ABGR8888 as a 32-bit value is 0xAABBGGRR, i.e. bytes R,G,B,A in memory.
 *
 *   yuv_convert_1to1_neon()  960x544-style fast path: no scaling, width % 16 == 0,
 *                            even height. About 10x faster than the scalar path.
 *   yuv_convert_scaled()     any size, nearest-neighbour scaled and letterboxed.
 */
#include <stdint.h>
#include <stddef.h>

#ifndef VIDEO_SWAP_UV
#define VIDEO_SWAP_UV 1            /* 1: chroma bytes are U,V (NV12)   0: V,U (NV21) */
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define YUV_HAVE_NEON 1
#else
#define YUV_HAVE_NEON 0
#endif

#if YUV_HAVE_NEON
/* Fixed point with vqdmulh: (2*a*b)>>16, so a<<7 times k gives a*k/256. */
static inline void yuv_store8(uint32_t *out, int16x8_t yl, int16x8_t rc, int16x8_t gc, int16x8_t bc) {
    uint8x8x4_t px;
    px.val[0] = vqmovun_s16(vaddq_s16(yl, rc));      /* R */
    px.val[1] = vqmovun_s16(vsubq_s16(yl, gc));      /* G */
    px.val[2] = vqmovun_s16(vaddq_s16(yl, bc));      /* B */
    px.val[3] = vdup_n_u8(255);                      /* A */
    vst4_u8((uint8_t *)out, px);
}

static inline void yuv_convert_1to1_neon(const uint8_t *yp, const uint8_t *cp, int stride,
                                         int w, int h, uint32_t *dst, int dst_pitch) {
    for (int y = 0; y < h; y += 2) {
        const uint8_t *y0 = yp + (size_t)y * stride, *y1 = y0 + stride;
        const uint8_t *c  = cp + (size_t)(y >> 1) * stride;
        uint32_t *o0 = dst + (size_t)y * dst_pitch, *o1 = o0 + dst_pitch;
        for (int x = 0; x < w; x += 16) {
            uint8x8x2_t uv = vld2_u8(c + x);          /* 8 chroma pairs -> 16 pixels */
#if VIDEO_SWAP_UV
            uint8x8_t u8 = uv.val[0], v8 = uv.val[1];
#else
            uint8x8_t v8 = uv.val[0], u8 = uv.val[1];
#endif
            int16x8_t u = vshlq_n_s16(vreinterpretq_s16_u16(vsubl_u8(u8, vdup_n_u8(128))), 7);
            int16x8_t v = vshlq_n_s16(vreinterpretq_s16_u16(vsubl_u8(v8, vdup_n_u8(128))), 7);
            int16x8_t rc = vqdmulhq_n_s16(v, 459);                                   /* 1.793 V */
            int16x8_t bc = vqdmulhq_n_s16(u, 541);                                   /* 2.112 U */
            int16x8_t gc = vqaddq_s16(vqdmulhq_n_s16(u, 55), vqdmulhq_n_s16(v, 136)); /* .213 U + .533 V */
            int16x8x2_t rz = vzipq_s16(rc, rc), gz = vzipq_s16(gc, gc), bz = vzipq_s16(bc, bc);

            uint8x16_t ya = vld1q_u8(y0 + x), yb = vld1q_u8(y1 + x);
            int16x8_t a0 = vshlq_n_s16(vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(ya),  vdup_n_u8(16))), 7);
            int16x8_t a1 = vshlq_n_s16(vreinterpretq_s16_u16(vsubl_u8(vget_high_u8(ya), vdup_n_u8(16))), 7);
            int16x8_t b0 = vshlq_n_s16(vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(yb),  vdup_n_u8(16))), 7);
            int16x8_t b1 = vshlq_n_s16(vreinterpretq_s16_u16(vsubl_u8(vget_high_u8(yb), vdup_n_u8(16))), 7);
            yuv_store8(o0 + x,     vqdmulhq_n_s16(a0, 298), rz.val[0], gz.val[0], bz.val[0]);
            yuv_store8(o0 + x + 8, vqdmulhq_n_s16(a1, 298), rz.val[1], gz.val[1], bz.val[1]);
            yuv_store8(o1 + x,     vqdmulhq_n_s16(b0, 298), rz.val[0], gz.val[0], bz.val[0]);
            yuv_store8(o1 + x + 8, vqdmulhq_n_s16(b1, 298), rz.val[1], gz.val[1], bz.val[1]);
        }
    }
}
#endif /* YUV_HAVE_NEON */

static inline uint8_t yuv_clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

/* Generic path: any source size, scaled (nearest) and centred in a dst_w x dst_h buffer. */
static inline void yuv_convert_scaled(const uint8_t *yp, const uint8_t *cp, int stride, int w, int h,
                                      uint32_t *dst, int dst_w, int dst_h, int *xmap /* dst_w ints */) {
    int dw = dst_w, dh = (int)((int64_t)dst_w * h / w);
    if (dh > dst_h) { dh = dst_h; dw = (int)((int64_t)dst_h * w / h); }
    const int ox = (dst_w - dw) / 2, oy = (dst_h - dh) / 2;
    for (int x = 0; x < dw; x++) xmap[x] = (int)((int64_t)x * w / dw);
    for (int y = 0; y < dh; y++) {
        const int sy = (int)((int64_t)y * h / dh);
        const uint8_t *yrow = yp + (size_t)sy * stride;
        const uint8_t *crow = cp + (size_t)(sy >> 1) * stride;
        uint32_t *out = dst + (size_t)(oy + y) * dst_w + ox;
        for (int x = 0; x < dw; x++) {
            const int sx = xmap[x];
            const uint8_t *c = crow + (sx & ~1);
#if VIDEO_SWAP_UV
            const int u = c[0] - 128, v = c[1] - 128;
#else
            const int v = c[0] - 128, u = c[1] - 128;
#endif
            const int yy = (yrow[sx] - 16) * 298;
            const uint8_t r = yuv_clamp8((yy + 459 * v + 128) >> 8);
            const uint8_t g = yuv_clamp8((yy -  55 * u - 136 * v + 128) >> 8);
            const uint8_t b = yuv_clamp8((yy + 541 * u + 128) >> 8);
            out[x] = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
        }
    }
}
