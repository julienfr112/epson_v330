/*
 * v330scan - standalone command-line scan tool for the EPSON Perfection
 *            V33/V330.  Scans an A4 page and writes a PNM (PPM/PGM/PBM).
 *
 * Build:  make v330scan
 * Usage:  v330scan [-f fw_dir] [-o out] [-r dpi] [-m color|gray|lineart]
 *                  [-t threshold%] [-v]
 */
#define _GNU_SOURCE
#include "v330_core.h"
#include "v330_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void logcb(void *ctx, int level, const char *msg)
{
    int verbose = *(int *)ctx;
    if (level == 0 || verbose) fprintf(stderr, "%s\n", msg);
}

int main(int argc, char **argv)
{
    const char *fw_dir = NULL, *out = "scan.pnm";
    int verbose = 0, dpi = V330_BASE_DPI, mode = V330_MODE_COLOR, thr = 50, opt;
    while ((opt = getopt(argc, argv, "f:o:r:m:t:vh")) != -1) {
        switch (opt) {
        case 'f': fw_dir = optarg; break;
        case 'o': out = optarg; break;
        case 'r': dpi = atoi(optarg); break;
        case 't': thr = atoi(optarg); break;
        case 'm':
            if (!strcmp(optarg, "gray")) mode = V330_MODE_GRAY;
            else if (!strcmp(optarg, "lineart")) mode = V330_MODE_LINEART;
            else mode = V330_MODE_COLOR;
            break;
        case 'v': verbose++; break;
        default:
            fprintf(stderr, "usage: %s [-f fw_dir] [-o out] [-r dpi] "
                            "[-m color|gray|lineart] [-t threshold%%] [-v]\n", argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }
    if (dpi > V330_BASE_DPI)
        fprintf(stderr, "note: %d dpi requested; %d dpi is the optical maximum, using that\n",
                dpi, V330_BASE_DPI);

    char err[256];
    v330 *s = v330_open(fw_dir, logcb, &verbose, err, sizeof err);
    if (!s) { fprintf(stderr, "v330scan: %s\n", err); return 1; }

    uint8_t *rgb = NULL;
    int w = 0, h = 0;
    if (v330_scan_page(s, &rgb, &w, &h, err, sizeof err) != 0) {
        fprintf(stderr, "v330scan: %s\n", err);
        v330_close(s);
        return 1;
    }
    v330_close(s);

    int ow, oh, bpl, depth, ch;
    uint8_t *img = v330_process(rgb, w, h, 0, 0, w, h, dpi, mode, thr * 255 / 100,
                                &ow, &oh, &bpl, &depth, &ch);
    free(rgb);
    if (!img) { fprintf(stderr, "v330scan: out of memory\n"); return 1; }

    FILE *f = strcmp(out, "-") ? fopen(out, "wb") : stdout;
    if (!f) { fprintf(stderr, "v330scan: cannot create %s\n", out); free(img); return 1; }
    if (depth == 1)            fprintf(f, "P4\n%d %d\n", ow, oh);           /* lineart -> PBM */
    else if (ch == 1)          fprintf(f, "P5\n%d %d\n255\n", ow, oh);      /* gray -> PGM */
    else                       fprintf(f, "P6\n%d %d\n255\n", ow, oh);      /* colour -> PPM */
    fwrite(img, 1, (size_t)bpl * oh, f);
    if (f != stdout) fclose(f);
    free(img);
    fprintf(stderr, "Wrote %s (%d x %d, %s)\n", out, ow, oh,
            mode == V330_MODE_LINEART ? "lineart" : mode == V330_MODE_GRAY ? "gray" : "colour");
    return 0;
}
