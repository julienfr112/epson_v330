/*
 * libsane-v330 - SANE backend for the EPSON Perfection V33/V330.
 *
 * A SANE wrapper around v330_core: exposes the scanner to any SANE frontend
 * (Simple Scan / GNOME Document Scanner, XSane, scanimage, ...).  The hardware
 * is scanned at its native 300 dpi optical profile; lower resolutions and the
 * grayscale / lineart modes are produced in software (see v330_image.c).  The
 * scan area is croppable (full A4 by default).
 *
 * The backend exports the plain sane_* entry points and name-mangled aliases;
 * the SANE dll loader dlopen()s libsane-v330.so.1 and resolves them by name.
 */
#define _GNU_SOURCE
#include <sane/sane.h>
#include <sane/saneopts.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "v330_core.h"
#include "v330_image.h"

#define V330_MM_PER_IN 25.4
#define MAX_X_MM (V330_OUT_W * V330_MM_PER_IN / V330_BASE_DPI)   /* ~210.06 mm */
#define MAX_Y_MM (V330_OUT_H * V330_MM_PER_IN / V330_BASE_DPI)   /* ~296.93 mm */

enum {
    OPT_NUM_OPTIONS = 0,
    OPT_MODE_GROUP,
    OPT_MODE,
    OPT_RESOLUTION,
    OPT_THRESHOLD,
    OPT_GEOMETRY_GROUP,
    OPT_TL_X,
    OPT_TL_Y,
    OPT_BR_X,
    OPT_BR_Y,
    NUM_OPTIONS
};

typedef struct {
    v330 *dev;
    SANE_Option_Descriptor opt[NUM_OPTIONS];
    SANE_Word resolution;                 /* dpi */
    int mode;                             /* V330_MODE_* */
    SANE_Word threshold;                  /* percent 0..100 (lineart) */
    SANE_Fixed tl_x, tl_y, br_x, br_y;    /* mm */
    /* active frame */
    SANE_Byte *img;
    size_t img_size, img_pos;
    SANE_Frame frame;
    int depth, width, height, bpl;
    int scanning, page_done;
} V330_Handle;

static const SANE_Word resolution_list[] = { 5, 75, 100, 150, 200, 300 };
static const SANE_String_Const mode_list[] = {
    SANE_VALUE_SCAN_MODE_COLOR, SANE_VALUE_SCAN_MODE_GRAY, SANE_VALUE_SCAN_MODE_LINEART, NULL
};
static const SANE_Range x_range = { 0, SANE_FIX(MAX_X_MM), 0 };
static const SANE_Range y_range = { 0, SANE_FIX(MAX_Y_MM), 0 };
static const SANE_Range threshold_range = { 0, 100, 1 };

static SANE_Device g_device = { "v330", "Epson", "Perfection V33/V330", "flatbed scanner" };
static const SANE_Device *g_device_list[] = { &g_device, NULL };

/* ---------------------------------------------------------------- helpers */
static int mode_from_string(const char *s)
{
    if (!strcmp(s, SANE_VALUE_SCAN_MODE_GRAY)) return V330_MODE_GRAY;
    if (!strcmp(s, SANE_VALUE_SCAN_MODE_LINEART)) return V330_MODE_LINEART;
    return V330_MODE_COLOR;
}
static const char *mode_to_string(int m)
{
    return m == V330_MODE_GRAY ? SANE_VALUE_SCAN_MODE_GRAY :
           m == V330_MODE_LINEART ? SANE_VALUE_SCAN_MODE_LINEART :
           SANE_VALUE_SCAN_MODE_COLOR;
}

static void update_caps(V330_Handle *h)
{
    if (h->mode == V330_MODE_LINEART)
        h->opt[OPT_THRESHOLD].cap &= ~SANE_CAP_INACTIVE;
    else
        h->opt[OPT_THRESHOLD].cap |= SANE_CAP_INACTIVE;
}

static void init_options(V330_Handle *h)
{
    SANE_Option_Descriptor *o = h->opt;

    o[OPT_NUM_OPTIONS].name = "";
    o[OPT_NUM_OPTIONS].title = SANE_TITLE_NUM_OPTIONS;
    o[OPT_NUM_OPTIONS].desc = SANE_DESC_NUM_OPTIONS;
    o[OPT_NUM_OPTIONS].type = SANE_TYPE_INT;
    o[OPT_NUM_OPTIONS].cap = SANE_CAP_SOFT_DETECT;
    o[OPT_NUM_OPTIONS].size = sizeof(SANE_Word);

    o[OPT_MODE_GROUP].title = "Scan mode";
    o[OPT_MODE_GROUP].type = SANE_TYPE_GROUP;

    o[OPT_MODE].name = SANE_NAME_SCAN_MODE;
    o[OPT_MODE].title = SANE_TITLE_SCAN_MODE;
    o[OPT_MODE].desc = SANE_DESC_SCAN_MODE;
    o[OPT_MODE].type = SANE_TYPE_STRING;
    o[OPT_MODE].size = 16;
    o[OPT_MODE].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    o[OPT_MODE].constraint_type = SANE_CONSTRAINT_STRING_LIST;
    o[OPT_MODE].constraint.string_list = mode_list;

    o[OPT_RESOLUTION].name = SANE_NAME_SCAN_RESOLUTION;
    o[OPT_RESOLUTION].title = SANE_TITLE_SCAN_RESOLUTION;
    o[OPT_RESOLUTION].desc = SANE_DESC_SCAN_RESOLUTION;
    o[OPT_RESOLUTION].type = SANE_TYPE_INT;
    o[OPT_RESOLUTION].unit = SANE_UNIT_DPI;
    o[OPT_RESOLUTION].size = sizeof(SANE_Word);
    o[OPT_RESOLUTION].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    o[OPT_RESOLUTION].constraint_type = SANE_CONSTRAINT_WORD_LIST;
    o[OPT_RESOLUTION].constraint.word_list = resolution_list;

    o[OPT_THRESHOLD].name = SANE_NAME_THRESHOLD;
    o[OPT_THRESHOLD].title = SANE_TITLE_THRESHOLD;
    o[OPT_THRESHOLD].desc = SANE_DESC_THRESHOLD;
    o[OPT_THRESHOLD].type = SANE_TYPE_INT;
    o[OPT_THRESHOLD].unit = SANE_UNIT_PERCENT;
    o[OPT_THRESHOLD].size = sizeof(SANE_Word);
    o[OPT_THRESHOLD].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT | SANE_CAP_INACTIVE;
    o[OPT_THRESHOLD].constraint_type = SANE_CONSTRAINT_RANGE;
    o[OPT_THRESHOLD].constraint.range = &threshold_range;

    o[OPT_GEOMETRY_GROUP].title = "Geometry";
    o[OPT_GEOMETRY_GROUP].type = SANE_TYPE_GROUP;

    o[OPT_TL_X].name = SANE_NAME_SCAN_TL_X;
    o[OPT_TL_X].title = SANE_TITLE_SCAN_TL_X;
    o[OPT_TL_X].desc = SANE_DESC_SCAN_TL_X;
    o[OPT_TL_X].type = SANE_TYPE_FIXED;
    o[OPT_TL_X].unit = SANE_UNIT_MM;
    o[OPT_TL_X].size = sizeof(SANE_Fixed);
    o[OPT_TL_X].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    o[OPT_TL_X].constraint_type = SANE_CONSTRAINT_RANGE;
    o[OPT_TL_X].constraint.range = &x_range;

    o[OPT_TL_Y] = o[OPT_TL_X];
    o[OPT_TL_Y].name = SANE_NAME_SCAN_TL_Y;
    o[OPT_TL_Y].title = SANE_TITLE_SCAN_TL_Y;
    o[OPT_TL_Y].desc = SANE_DESC_SCAN_TL_Y;
    o[OPT_TL_Y].constraint.range = &y_range;

    o[OPT_BR_X] = o[OPT_TL_X];
    o[OPT_BR_X].name = SANE_NAME_SCAN_BR_X;
    o[OPT_BR_X].title = SANE_TITLE_SCAN_BR_X;
    o[OPT_BR_X].desc = SANE_DESC_SCAN_BR_X;

    o[OPT_BR_Y] = o[OPT_TL_Y];
    o[OPT_BR_Y].name = SANE_NAME_SCAN_BR_Y;
    o[OPT_BR_Y].title = SANE_TITLE_SCAN_BR_Y;
    o[OPT_BR_Y].desc = SANE_DESC_SCAN_BR_Y;

    h->resolution = V330_BASE_DPI;
    h->mode = V330_MODE_COLOR;
    h->threshold = 50;
    h->tl_x = SANE_FIX(0);
    h->tl_y = SANE_FIX(0);
    h->br_x = SANE_FIX(MAX_X_MM);
    h->br_y = SANE_FIX(MAX_Y_MM);
    update_caps(h);
}

static int mm_to_px(SANE_Fixed mm) { return (int)(SANE_UNFIX(mm) / V330_MM_PER_IN * V330_BASE_DPI + 0.5); }

/* crop window in base-DPI (300) source pixels */
static void compute_window(V330_Handle *h, int *ox, int *oy, int *cw, int *ch)
{
    int x0 = mm_to_px(h->tl_x), x1 = mm_to_px(h->br_x);
    int y0 = mm_to_px(h->tl_y), y1 = mm_to_px(h->br_y);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > V330_OUT_W) x1 = V330_OUT_W;
    if (y1 > V330_OUT_H) y1 = V330_OUT_H;
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    *ox = x0; *oy = y0; *cw = x1 - x0; *ch = y1 - y0;
}

/* output geometry for the current option values (must match v330_process) */
static void compute_params(V330_Handle *h, SANE_Frame *frame, int *depth,
                           int *w, int *lines, int *bpl)
{
    int ox, oy, cw, ch;
    compute_window(h, &ox, &oy, &cw, &ch);
    int ow = (int)((int64_t)cw * h->resolution / V330_BASE_DPI);
    int oh = (int)((int64_t)ch * h->resolution / V330_BASE_DPI);
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    if (h->mode == V330_MODE_COLOR) { *frame = SANE_FRAME_RGB;  *depth = 8; *bpl = ow * 3; }
    else if (h->mode == V330_MODE_GRAY) { *frame = SANE_FRAME_GRAY; *depth = 8; *bpl = ow; }
    else { *frame = SANE_FRAME_GRAY; *depth = 1; *bpl = (ow + 7) / 8; }
    *w = ow; *lines = oh;
}

/* ---------------------------------------------------------------- SANE API */
SANE_Status sane_init(SANE_Int *version_code, SANE_Auth_Callback authorize)
{
    (void)authorize;
    if (version_code) *version_code = SANE_VERSION_CODE(1, 0, 0);
    return SANE_STATUS_GOOD;
}

void sane_exit(void) { }

SANE_Status sane_get_devices(const SANE_Device ***device_list, SANE_Bool local_only)
{
    (void)local_only;
    static const SANE_Device *empty[] = { NULL };
    *device_list = v330_present() ? g_device_list : empty;
    return SANE_STATUS_GOOD;
}

SANE_Status sane_open(SANE_String_Const name, SANE_Handle *handle)
{
    (void)name;
    V330_Handle *h = calloc(1, sizeof *h);
    if (!h) return SANE_STATUS_NO_MEM;
    char err[256];
    h->dev = v330_open(NULL, NULL, NULL, err, sizeof err);
    if (!h->dev) {
        fprintf(stderr, "libsane-v330: %s\n", err);
        free(h);
        return SANE_STATUS_IO_ERROR;
    }
    init_options(h);
    *handle = h;
    return SANE_STATUS_GOOD;
}

void sane_close(SANE_Handle handle)
{
    V330_Handle *h = handle;
    if (!h) return;
    free(h->img);
    if (h->dev) v330_close(h->dev);
    free(h);
}

const SANE_Option_Descriptor *sane_get_option_descriptor(SANE_Handle handle, SANE_Int option)
{
    V330_Handle *h = handle;
    if (option < 0 || option >= NUM_OPTIONS) return NULL;
    return &h->opt[option];
}

SANE_Status sane_control_option(SANE_Handle handle, SANE_Int option, SANE_Action action,
                                void *value, SANE_Int *info)
{
    V330_Handle *h = handle;
    if (option < 0 || option >= NUM_OPTIONS) return SANE_STATUS_INVAL;
    if (info) *info = 0;

    if (action == SANE_ACTION_GET_VALUE) {
        switch (option) {
        case OPT_NUM_OPTIONS: *(SANE_Word *)value = NUM_OPTIONS; return SANE_STATUS_GOOD;
        case OPT_RESOLUTION:  *(SANE_Word *)value = h->resolution; return SANE_STATUS_GOOD;
        case OPT_MODE:        strcpy(value, mode_to_string(h->mode)); return SANE_STATUS_GOOD;
        case OPT_THRESHOLD:   *(SANE_Word *)value = h->threshold; return SANE_STATUS_GOOD;
        case OPT_TL_X: *(SANE_Fixed *)value = h->tl_x; return SANE_STATUS_GOOD;
        case OPT_TL_Y: *(SANE_Fixed *)value = h->tl_y; return SANE_STATUS_GOOD;
        case OPT_BR_X: *(SANE_Fixed *)value = h->br_x; return SANE_STATUS_GOOD;
        case OPT_BR_Y: *(SANE_Fixed *)value = h->br_y; return SANE_STATUS_GOOD;
        default: return SANE_STATUS_INVAL;
        }
    }

    if (action == SANE_ACTION_SET_VALUE) {
        switch (option) {
        case OPT_RESOLUTION: {
            SANE_Word want = *(SANE_Word *)value, best = resolution_list[1];
            for (int i = 1; i <= resolution_list[0]; i++)
                if (abs(resolution_list[i] - want) < abs(best - want)) best = resolution_list[i];
            h->resolution = best;
            if (best != want && value) *(SANE_Word *)value = best;
            if (info) *info |= SANE_INFO_RELOAD_PARAMS | (best != want ? SANE_INFO_INEXACT : 0);
            return SANE_STATUS_GOOD;
        }
        case OPT_MODE:
            h->mode = mode_from_string(value);
            update_caps(h);
            if (info) *info |= SANE_INFO_RELOAD_PARAMS | SANE_INFO_RELOAD_OPTIONS;
            return SANE_STATUS_GOOD;
        case OPT_THRESHOLD:
            h->threshold = *(SANE_Word *)value;
            return SANE_STATUS_GOOD;
        case OPT_TL_X: h->tl_x = *(SANE_Fixed *)value; if (info) *info |= SANE_INFO_RELOAD_PARAMS; return SANE_STATUS_GOOD;
        case OPT_TL_Y: h->tl_y = *(SANE_Fixed *)value; if (info) *info |= SANE_INFO_RELOAD_PARAMS; return SANE_STATUS_GOOD;
        case OPT_BR_X: h->br_x = *(SANE_Fixed *)value; if (info) *info |= SANE_INFO_RELOAD_PARAMS; return SANE_STATUS_GOOD;
        case OPT_BR_Y: h->br_y = *(SANE_Fixed *)value; if (info) *info |= SANE_INFO_RELOAD_PARAMS; return SANE_STATUS_GOOD;
        default: return SANE_STATUS_INVAL;
        }
    }

    if (action == SANE_ACTION_SET_AUTO) return SANE_STATUS_GOOD;
    return SANE_STATUS_INVAL;
}

SANE_Status sane_get_parameters(SANE_Handle handle, SANE_Parameters *params)
{
    V330_Handle *h = handle;
    if (h->scanning) {
        params->format = h->frame;
        params->depth = h->depth;
        params->pixels_per_line = h->width;
        params->lines = h->height;
        params->bytes_per_line = h->bpl;
    } else {
        SANE_Frame frame; int depth, w, lines, bpl;
        compute_params(h, &frame, &depth, &w, &lines, &bpl);
        params->format = frame;
        params->depth = depth;
        params->pixels_per_line = w;
        params->lines = lines;
        params->bytes_per_line = bpl;
    }
    params->last_frame = SANE_TRUE;
    return SANE_STATUS_GOOD;
}

SANE_Status sane_start(SANE_Handle handle)
{
    V330_Handle *h = handle;
    if (h->page_done) return SANE_STATUS_NO_DOCS;      /* flatbed: one page per scan */

    int ox, oy, cw, ch;
    compute_window(h, &ox, &oy, &cw, &ch);

    uint8_t *full = NULL;
    char err[256];
    if (v330_scan_page(h->dev, &full, NULL, NULL, err, sizeof err) != 0) {
        fprintf(stderr, "libsane-v330: %s\n", err);
        return SANE_STATUS_IO_ERROR;
    }

    int ow, oh, bpl, depth, channels;
    uint8_t *proc = v330_process(full, V330_OUT_W, V330_OUT_H, ox, oy, cw, ch,
                                 h->resolution, h->mode, h->threshold * 255 / 100,
                                 &ow, &oh, &bpl, &depth, &channels);
    free(full);
    if (!proc) return SANE_STATUS_NO_MEM;

    free(h->img);
    h->img = proc;
    h->img_size = (size_t)bpl * oh;
    h->img_pos = 0;
    h->frame = (channels == 3) ? SANE_FRAME_RGB : SANE_FRAME_GRAY;
    h->depth = depth;
    h->width = ow;
    h->height = oh;
    h->bpl = bpl;
    h->scanning = 1;
    return SANE_STATUS_GOOD;
}

SANE_Status sane_read(SANE_Handle handle, SANE_Byte *data, SANE_Int max_length, SANE_Int *length)
{
    V330_Handle *h = handle;
    *length = 0;
    if (!h->scanning || !h->img) return SANE_STATUS_CANCELLED;
    if (h->img_pos >= h->img_size) {
        h->scanning = 0;
        h->page_done = 1;
        return SANE_STATUS_EOF;
    }
    size_t remain = h->img_size - h->img_pos;
    size_t n = (size_t)max_length < remain ? (size_t)max_length : remain;
    memcpy(data, h->img + h->img_pos, n);
    h->img_pos += n;
    *length = (SANE_Int)n;
    return SANE_STATUS_GOOD;
}

void sane_cancel(SANE_Handle handle)
{
    V330_Handle *h = handle;
    free(h->img);
    h->img = NULL;
    h->img_size = h->img_pos = 0;
    h->scanning = 0;
    h->page_done = 0;
}

SANE_Status sane_set_io_mode(SANE_Handle handle, SANE_Bool non_blocking)
{
    (void)handle;
    return non_blocking ? SANE_STATUS_UNSUPPORTED : SANE_STATUS_GOOD;
}

SANE_Status sane_get_select_fd(SANE_Handle handle, SANE_Int *fd)
{
    (void)handle; (void)fd;
    return SANE_STATUS_UNSUPPORTED;
}

SANE_String_Const sane_strstatus(SANE_Status status)
{
    switch (status) {
    case SANE_STATUS_GOOD: return "everything A-OK";
    case SANE_STATUS_UNSUPPORTED: return "operation not supported";
    case SANE_STATUS_CANCELLED: return "operation cancelled";
    case SANE_STATUS_DEVICE_BUSY: return "device busy";
    case SANE_STATUS_INVAL: return "invalid argument";
    case SANE_STATUS_EOF: return "end of file reached";
    case SANE_STATUS_JAMMED: return "document jammed";
    case SANE_STATUS_NO_DOCS: return "no more documents";
    case SANE_STATUS_COVER_OPEN: return "cover open";
    case SANE_STATUS_IO_ERROR: return "I/O error";
    case SANE_STATUS_NO_MEM: return "out of memory";
    case SANE_STATUS_ACCESS_DENIED: return "access denied";
    default: return "unknown status";
    }
}

/* name-mangled aliases resolved by the SANE dll loader */
SANE_Status sane_v330_init(SANE_Int *, SANE_Auth_Callback) __attribute__((alias("sane_init")));
void sane_v330_exit(void) __attribute__((alias("sane_exit")));
SANE_Status sane_v330_get_devices(const SANE_Device ***, SANE_Bool) __attribute__((alias("sane_get_devices")));
SANE_Status sane_v330_open(SANE_String_Const, SANE_Handle *) __attribute__((alias("sane_open")));
void sane_v330_close(SANE_Handle) __attribute__((alias("sane_close")));
const SANE_Option_Descriptor *sane_v330_get_option_descriptor(SANE_Handle, SANE_Int) __attribute__((alias("sane_get_option_descriptor")));
SANE_Status sane_v330_control_option(SANE_Handle, SANE_Int, SANE_Action, void *, SANE_Int *) __attribute__((alias("sane_control_option")));
SANE_Status sane_v330_get_parameters(SANE_Handle, SANE_Parameters *) __attribute__((alias("sane_get_parameters")));
SANE_Status sane_v330_start(SANE_Handle) __attribute__((alias("sane_start")));
SANE_Status sane_v330_read(SANE_Handle, SANE_Byte *, SANE_Int, SANE_Int *) __attribute__((alias("sane_read")));
void sane_v330_cancel(SANE_Handle) __attribute__((alias("sane_cancel")));
SANE_Status sane_v330_set_io_mode(SANE_Handle, SANE_Bool) __attribute__((alias("sane_set_io_mode")));
SANE_Status sane_v330_get_select_fd(SANE_Handle, SANE_Int *) __attribute__((alias("sane_get_select_fd")));
SANE_String_Const sane_v330_strstatus(SANE_Status) __attribute__((alias("sane_strstatus")));
