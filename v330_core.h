/*
 * v330_core - native USB scan engine for the EPSON Perfection V33/V330
 *             (GT-F730 / GT-S630, USB 04b8:0142).
 *
 * This library talks the scanner's proprietary native protocol directly over
 * libusb: it uploads the firmware if needed, runs the ASIC calibration the
 * device requires, scans the flatbed at 300 dpi in 24-bit colour and returns
 * a packed RGB image buffer.  It is used both by the standalone CLI
 * (v330scan) and by the SANE backend (libsane-v330).
 */
#ifndef V330_CORE_H
#define V330_CORE_H

#include <stddef.h>
#include <stdint.h>

/* Fixed 300 dpi full-flatbed colour geometry. */
#define V330_DPI      300
#define V330_OUT_W    2481      /* 8.27 in  */
#define V330_OUT_H    3507      /* 11.69 in */

typedef struct v330 v330;

/* Optional logging: level 0 = user-visible progress, 1+ = debug/trace. */
typedef void (*v330_log_fn)(void *ctx, int level, const char *msg);

/*
 * Open the scanner.  Uploads firmware (esfwad.bin) if the device is still in
 * its bootloader.  `fw_dir` may be NULL (then $V330_FIRMWARE_DIR and a list of
 * standard directories are searched).  On failure returns NULL and writes a
 * message into `err` (if err != NULL).
 */
v330 *v330_open(const char *fw_dir, v330_log_fn log, void *log_ctx,
                char *err, size_t errlen);

/* True if a V330 (04b8:0142) is present on the USB bus. */
int v330_present(void);

/*
 * Scan one full page.  On success returns 0 and stores a freshly malloc'd
 * packed RGB buffer (V330_OUT_W * V330_OUT_H * 3 bytes, top-to-bottom,
 * R,G,B per pixel) in *out_rgb; the caller must free() it.  On failure
 * returns -1 and fills `err`.
 */
int v330_scan_page(v330 *s, uint8_t **out_rgb, int *w, int *h,
                   char *err, size_t errlen);

void v330_close(v330 *s);

#endif /* V330_CORE_H */
