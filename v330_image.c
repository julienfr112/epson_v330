/* v330_image - see v330_image.h */
#define _GNU_SOURCE
#include "v330_image.h"
#include <stdlib.h>
#include <string.h>

/* Rec.601 luma, fixed point (/256): 0.299 0.587 0.114 -> 77 150 29 */
static inline int luma(int r, int g, int b) { return (r * 77 + g * 150 + b * 29) >> 8; }

/*
 * Area-average downscale of the crop [ox,oy,cw,ch] of an RGB source to
 * out_w x out_h, writing `channels` interleaved bytes per pixel (3 = RGB,
 * 1 = luma).  out_w/out_h must be <= cw/ch.
 */
static void downscale(const uint8_t *src, int src_w,
                      int ox, int oy, int cw, int ch,
                      int out_w, int out_h, int channels, uint8_t *dst)
{
    for (int y = 0; y < out_h; y++) {
        int sy0 = oy + (int)((int64_t)y * ch / out_h);
        int sy1 = oy + (int)((int64_t)(y + 1) * ch / out_h);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < out_w; x++) {
            int sx0 = ox + (int)((int64_t)x * cw / out_w);
            int sx1 = ox + (int)((int64_t)(x + 1) * cw / out_w);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned rs = 0, gs = 0, bs = 0, cnt = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t *row = src + ((size_t)sy * src_w + sx0) * 3;
                for (int sx = sx0; sx < sx1; sx++) {
                    rs += row[0]; gs += row[1]; bs += row[2];
                    row += 3;
                    cnt++;
                }
            }
            if (!cnt) cnt = 1;
            uint8_t *o = dst + ((size_t)y * out_w + x) * channels;
            if (channels == 3) {
                o[0] = (uint8_t)(rs / cnt);
                o[1] = (uint8_t)(gs / cnt);
                o[2] = (uint8_t)(bs / cnt);
            } else {
                o[0] = (uint8_t)luma(rs / cnt, gs / cnt, bs / cnt);
            }
        }
    }
}

uint8_t *v330_process(const uint8_t *src, int src_w, int src_h,
                      int ox, int oy, int cw, int ch,
                      int out_dpi, int mode, int threshold,
                      int *ow, int *oh, int *obytes_per_line,
                      int *odepth, int *ochannels)
{
    (void)src_h;
    if (out_dpi > V330_BASE_DPI) out_dpi = V330_BASE_DPI;
    if (out_dpi < 1) out_dpi = V330_BASE_DPI;

    int out_w = (int)((int64_t)cw * out_dpi / V330_BASE_DPI);
    int out_h = (int)((int64_t)ch * out_dpi / V330_BASE_DPI);
    if (out_w < 1) out_w = 1;
    if (out_h < 1) out_h = 1;

    int channels = (mode == V330_MODE_COLOR) ? 3 : 1;

    if (mode == V330_MODE_LINEART) {
        /* build 8-bit luma first, then pack to 1 bit (1 = black) */
        uint8_t *gray = malloc((size_t)out_w * out_h);
        if (!gray) return NULL;
        downscale(src, src_w, ox, oy, cw, ch, out_w, out_h, 1, gray);
        int stride = (out_w + 7) / 8;
        uint8_t *bits = calloc((size_t)stride * out_h, 1);
        if (!bits) { free(gray); return NULL; }
        for (int y = 0; y < out_h; y++)
            for (int x = 0; x < out_w; x++)
                if (gray[(size_t)y * out_w + x] < threshold)         /* dark -> set bit */
                    bits[(size_t)y * stride + (x >> 3)] |= 0x80 >> (x & 7);
        free(gray);
        *ow = out_w; *oh = out_h; *obytes_per_line = stride;
        *odepth = 1; *ochannels = 1;
        return bits;
    }

    uint8_t *dst = malloc((size_t)out_w * out_h * channels);
    if (!dst) return NULL;
    downscale(src, src_w, ox, oy, cw, ch, out_w, out_h, channels, dst);
    *ow = out_w; *oh = out_h; *obytes_per_line = out_w * channels;
    *odepth = 8; *ochannels = channels;
    return dst;
}
