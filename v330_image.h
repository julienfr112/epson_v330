/*
 * v330_image - resolution and colour-mode post-processing for the V330 driver.
 *
 * The scanner is driven at its native 300 dpi optical profile (validated
 * pixel-for-pixel against Epson's own driver).  Lower resolutions are produced
 * here by area-average downscaling of that optical scan, and grayscale /
 * lineart by converting the RGB data.  Shared by the CLI and the SANE backend.
 */
#ifndef V330_IMAGE_H
#define V330_IMAGE_H

#include <stdint.h>

#define V330_MODE_COLOR    0
#define V330_MODE_GRAY     1
#define V330_MODE_LINEART  2

/* Native optical resolution the hardware is scanned at. */
#define V330_BASE_DPI 300

/*
 * Turn a base-DPI RGB scan into the requested output.
 *
 *   src        packed RGB, src_w * src_h * 3 bytes
 *   ox,oy,cw,ch   crop rectangle in source (base-DPI) pixels
 *   out_dpi    desired resolution (<= V330_BASE_DPI)
 *   mode       V330_MODE_*
 *   threshold  lineart threshold 0..255 (ignored otherwise)
 *
 * Returns a freshly malloc'd buffer (caller frees) and the output geometry:
 *   *ow, *oh          pixels
 *   *obytes_per_line  packed row stride
 *   *odepth           1 (lineart) or 8 (gray/colour)
 *   *ochannels        1 (gray/lineart) or 3 (colour)
 * Returns NULL on allocation failure.
 */
uint8_t *v330_process(const uint8_t *src, int src_w, int src_h,
                      int ox, int oy, int cw, int ch,
                      int out_dpi, int mode, int threshold,
                      int *ow, int *oh, int *obytes_per_line,
                      int *odepth, int *ochannels);

#endif /* V330_IMAGE_H */
