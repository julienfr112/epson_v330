/*
 * v330_core - native USB scan engine for the EPSON Perfection V33/V330.
 * See v330_core.h for the public interface and protocol summary.
 *
 * Error handling: the low-level USB helpers longjmp() back to the public
 * entry point on any failure, so the protocol code below reads linearly.
 */
#define _GNU_SOURCE
#include "v330_core.h"

#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <setjmp.h>

#include "tables.h"

#define VID 0x04b8
#define PID 0x0142
#define EP_IN  0x81
#define EP_OUT 0x02
#define ACK 0x06

/* firmware image: 256-byte "EDLA" header, 65536-byte payload, 1 checksum byte */
#define FW_FILE_SIZE 65793
#define FW_HDR       0x100
#define FW_SIZE      0x10000

/* image stream geometry */
#define RAW_W          2688
#define RAW_LINES      3519      /* V330_OUT_H + 12 lines for colour line offsets */
#define LINES_PER_CHUNK 6
#define CHUNK_TRAILER  8
#define XSCALE         (2688.0 / 2640.0)
#define DY_R 12
#define DY_G 6
#define DY_B 0

/* calibration geometry (400 dpi readout, 16-bit RGB) */
#define CAL_W        3584
#define ADC_PX       16
#define ADC_LINES    4
#define WHITE_LINES  16
#define DARK_LINES   64
#define SHADE_VALID  3368
#define OFFS_RANGE   2688
#define WHITE_GAIN   1.0065
#define SHADE_K      290

struct v330 {
    libusb_device_handle *h;
    v330_log_fn log;
    void *log_ctx;
    jmp_buf jb;
    char err[256];
};

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void emit(v330 *s, int level, const char *fmt, ...)
{
    if (!s->log) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    s->log(s->log_ctx, level, buf);
}

/* fail: record message and jump back to the entry point */
static void fail(v330 *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->err, sizeof s->err, fmt, ap);
    va_end(ap);
    longjmp(s->jb, 1);
}

/* ---------------------------------------------------------------- USB primitives */
static void usb_write(v330 *s, const void *buf, int len)
{
    int done = 0;
    int r = libusb_bulk_transfer(s->h, EP_OUT, (uint8_t *)buf, len, &done, 30000);
    if (r) fail(s, "USB write failed: %s", libusb_error_name(r));
    if (done != len) fail(s, "short USB write (%d/%d)", done, len);
}

static int usb_read_to(v330 *s, void *buf, int len, unsigned timeout_ms)
{
    int got = 0;
    while (got < len) {
        int n = 0;
        int r = libusb_bulk_transfer(s->h, EP_IN, (uint8_t *)buf + got, len - got, &n, timeout_ms);
        got += n;
        if (r == LIBUSB_ERROR_TIMEOUT) {
            if (got == 0) return 0;
            if (n == 0) fail(s, "USB read timed out (%d/%d bytes)", got, len);
            continue;
        }
        if (r == LIBUSB_ERROR_PIPE) { libusb_clear_halt(s->h, EP_IN); fail(s, "USB read: endpoint stalled"); }
        if (r) fail(s, "USB read failed: %s", libusb_error_name(r));
    }
    return got;
}
static void usb_read(v330 *s, void *buf, int len)
{
    if (usb_read_to(s, buf, len, 30000) != len) fail(s, "USB read: no data (expected %d bytes)", len);
}

/* ---------------------------------------------------------------- protocol helpers */
static uint16_t get_status(v330 *s)
{
    uint8_t c[2] = { 0x1b, 0x03 }, r[2];
    usb_write(s, c, 2);
    usb_read(s, r, 2);
    return (uint16_t)(r[0] << 8 | r[1]);
}

static void wait_ready(v330 *s, int timeout_s)
{
    double t0 = now();
    uint16_t st;
    while ((st = get_status(s)) != 0x1801) {
        if (st == 0x2800) fail(s, "scanner dropped back to bootloader");
        if (now() - t0 > timeout_s) fail(s, "timeout waiting for ready (status %04x)", st);
        usleep(50000);
    }
}

static void expect_ack(v330 *s, const char *what)
{
    uint8_t a = 0;
    usb_read(s, &a, 1);
    if (a != ACK) fail(s, "%s: expected ACK, got %02x", what, a);
}

static void cmd(v330 *s, uint8_t op)
{
    uint8_t c[2] = { 0x1e, op };
    usb_write(s, c, 2);
    char what[16];
    snprintf(what, sizeof what, "cmd 1e %02x", op);
    expect_ack(s, what);
}
static void cmd_w(v330 *s, uint8_t op, const void *p, int n)
{
    cmd(s, op);
    usb_write(s, p, n);
    char what[24];
    snprintf(what, sizeof what, "cmd 1e %02x data", op);
    expect_ack(s, what);
}
static void cmd_r(v330 *s, uint8_t op, void *out, int n)
{
    uint8_t c[2] = { 0x1e, op };
    usb_write(s, c, 2);
    usb_read(s, out, n);
}
static void cmd_wr(v330 *s, uint8_t op, const void *p, int n, void *out, int m)
{
    cmd(s, op);
    usb_write(s, p, n);
    usb_read(s, out, m);
}
static void upload_table(v330 *s, uint8_t type, uint16_t addr, uint8_t flag, const void *data, uint32_t len)
{
    uint8_t hdr[8] = { type, 0, addr & 0xff, addr >> 8, flag, len & 0xff, (len >> 8) & 0xff, (len >> 16) & 0xff };
    cmd(s, 0x84);
    usb_write(s, hdr, 8);
    usb_write(s, data, len);
    expect_ack(s, "table upload");
}
static void read_stream(v330 *s, uint8_t *buf, size_t total, int req)
{
    uint8_t c[2] = { 0x1e, 0x47 };
    usb_write(s, c, 2);
    size_t got = 0;
    while (got < total) {
        int want = (total - got) < (size_t)req ? (int)(total - got) : req;
        usb_read(s, buf + got, want);
        got += want;
    }
}
static void put16(uint8_t *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }

static void mean_lines_u16(const uint8_t *buf, int lines, int px, double *mean)
{
    const uint16_t *src = (const uint16_t *)buf;
    for (int i = 0; i < px * 3; i++) mean[i] = 0;
    for (int l = 0; l < lines; l++)
        for (int i = 0; i < px * 3; i++) mean[i] += src[(size_t)l * px * 3 + i];
    for (int i = 0; i < px * 3; i++) mean[i] /= lines;
}

/* ---------------------------------------------------------------- firmware */
static int find_firmware(const char *fw_dir, char *path, size_t pathlen)
{
    const char *env = getenv("V330_FIRMWARE_DIR");
    const char *dirs[] = { fw_dir, env, "/usr/share/v330", "/usr/local/share/v330",
                           "/usr/share/esci", ".", NULL };
    for (int i = 0; dirs[i] || i == 0; i++) {
        if (!dirs[i]) continue;
        snprintf(path, pathlen, "%s/esfwad.bin", dirs[i]);
        if (access(path, R_OK) == 0) return 0;
    }
    return -1;
}

static void upload_firmware(v330 *s, const char *fw_dir)
{
    char path[512];
    if (find_firmware(fw_dir, path, sizeof path) != 0)
        fail(s, "firmware esfwad.bin not found (set V330_FIRMWARE_DIR or install to /usr/share/v330)");

    FILE *f = fopen(path, "rb");
    if (!f) fail(s, "cannot open firmware %s: %s", path, strerror(errno));
    static uint8_t fw[FW_FILE_SIZE];
    size_t n = fread(fw, 1, sizeof fw, f);
    fclose(f);
    if (n != FW_FILE_SIZE || memcmp(fw, "EDLA", 4))
        fail(s, "%s is not a valid esfwad.bin (%zu bytes)", path, n);

    uint8_t sum = 0;
    for (int i = 0; i < FW_SIZE; i++) sum += fw[FW_HDR + i];

    emit(s, 0, "Uploading firmware");
    uint8_t c[2] = { 0x1b, 0x06 };
    usb_write(s, c, 2); expect_ack(s, "firmware download request");
    uint8_t hdr[4] = { 0x01, 0x00, 0x01, 0x00 };
    usb_write(s, hdr, 4);
    usb_write(s, fw + FW_HDR, FW_SIZE);
    usb_write(s, &sum, 1); expect_ack(s, "firmware checksum");
    c[1] = 0x16;
    usb_write(s, c, 2); expect_ack(s, "firmware start request");
    uint8_t go = 0x80;
    usb_write(s, &go, 1); expect_ack(s, "firmware start");
    sleep(3);
    uint8_t one = 0x01, p8e[6] = { 0x0b, 0x00, 0x98, 0x5c, 0x00, 0x00 }, p53[42];
    cmd_w(s, 0x93, &one, 1);
    cmd_w(s, 0x8e, p8e, 6);
    cmd_r(s, 0x53, p53, 42);
    emit(s, 0, "Firmware running, homing carriage");
    wait_ready(s, 120);
}

/* ---------------------------------------------------------------- image reconstruction */
static uint8_t *reconstruct_rgb(const uint8_t *raw)
{
    uint8_t *img = malloc((size_t)V330_OUT_W * V330_OUT_H * 3);
    if (!img) return NULL;
    static int x0[V330_OUT_W];
    static float fr[V330_OUT_W];
    for (int x = 0; x < V330_OUT_W; x++) {
        double sx = x * XSCALE;
        x0[x] = (int)floor(sx);
        if (x0[x] > RAW_W - 2) x0[x] = RAW_W - 2;
        fr[x] = (float)(sx - x0[x]);
    }
    const int dy[3] = { DY_R, DY_G, DY_B };
    for (int y = 0; y < V330_OUT_H; y++) {
        uint8_t *row = img + (size_t)y * V330_OUT_W * 3;
        for (int c = 0; c < 3; c++) {
            const uint8_t *l = raw + ((size_t)(y + dy[c]) * RAW_W) * 3 + c;
            for (int x = 0; x < V330_OUT_W; x++) {
                float v = l[x0[x] * 3] * (1.0f - fr[x]) + l[(x0[x] + 1) * 3] * fr[x];
                int iv = (int)(v + 0.5f);
                row[x * 3 + c] = (uint8_t)(iv < 0 ? 0 : iv > 255 ? 255 : iv);
            }
        }
    }
    return img;
}

/* ---------------------------------------------------------------- scan sequence */
static const uint8_t p57_scanparams[42] = {
    0x2c,0x01,0x00,0x00, 0x2c,0x01,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0xb1,0x09,0x00,0x00, 0xb3,0x0d,0x00,0x00, 0x13,0x08,0x00,0x00, 0x8c,0x04,0x00,0x00,
    0x00,0x80,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00 };
static const uint8_t p21_base[28] = { 0x00,0x0d,0,0,0,0,0,0,0,0,0,0, 0x2a,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
static const uint8_t p22_base[12] = { 0x00,0x0d,0,0,0,0,0,0,0,0,0,0 };
static const uint8_t p43_cal[12]  = { 0x00,0x80,0x00,0x80,0x00,0x80, 0x06,0x79,0xca,0x7a,0x40,0x7b };
static const uint8_t p43_scan[12] = { 0x06,0x79,0xca,0x7a,0x40,0x7b, 0x06,0x79,0xca,0x7a,0x40,0x7b };

/* Runs the full protocol.  Returns malloc'd raw sensor buffer via *out_raw. */
static void scan_sequence(v330 *s, uint8_t **out_raw)
{
    uint8_t b[64], one = 0x01, zero = 0x00;
    double t0 = now();

    wait_ready(s, 60);
    cmd_w(s, 0x93, &one, 1);
    { uint8_t p[6] = { 0x0b, 0x00, 0x98, 0x5c, 0x00, 0x00 }; cmd_w(s, 0x8e, p, 6); }
    wait_ready(s, 60);
    cmd_r(s, 0x53, b, 42);
    wait_ready(s, 60);
    { uint8_t c[2] = { 0x1b, 0x13 }; uint8_t name[28]; usb_write(s, c, 2); usb_read(s, name, 28);
      emit(s, 0, "Scanner: %.28s", name); }
    wait_ready(s, 60);
    cmd_r(s, 0xa1, b, 1); cmd_r(s, 0x65, b, 1);
    wait_ready(s, 60);
    cmd_r(s, 0x53, b, 42);
    wait_ready(s, 60);
    cmd_r(s, 0xa1, b, 1); cmd_r(s, 0x65, b, 1); cmd_r(s, 0xa1, b, 1);
    cmd(s, 0x77);
    wait_ready(s, 60);
    cmd_r(s, 0x53, b, 42); cmd_r(s, 0x53, b, 42);
    cmd_w(s, 0x57, p57_scanparams, 42);
    cmd_r(s, 0x97, b, 12);
    cmd(s, 0x9b);
    wait_ready(s, 60);
    cmd_r(s, 0xa1, b, 1);
    cmd_w(s, 0xa2, &one, 1);
    wait_ready(s, 60);
    cmd_wr(s, 0x87, &zero, 1, b, 10);
    cmd_r(s, 0x53, b, 42);
    cmd_w(s, 0x57, p57_scanparams, 42);
    cmd_r(s, 0x86, b, 4);
    { uint8_t p[8] = { 0x04, 0x01, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x00 }; cmd_w(s, 0x46, p, 8); }
    for (int i = 0; ; i++) {
        cmd_r(s, 0x04, b, 1);
        if (b[0] == 0x01) break;
        if (i > 200) fail(s, "lamp did not warm up (1e 04 -> %02x)", b[0]);
        usleep(100000);
    }
    { uint8_t r92[12], rff[4];
      cmd_r(s, 0x92, r92, 12);
      cmd_r(s, 0xff, rff, 4); expect_ack(s, "cmd 1e ff");
      rff[3] = 0x00; cmd_w(s, 0x99, rff, 4);
      cmd_w(s, 0x31, r92, 12); }
    { uint8_t v = 0x04; cmd_w(s, 0x11, &v, 1); }
    upload_table(s, 0x09, 0x0000, 0x00, tbl9_256, 256);

    /* calibration 1: ADC offset probe */
    cmd_w(s, 0x21, p21_base, 28);
    cmd_w(s, 0x22, p22_base, 12);
    { uint8_t p41[22] = { 0x13,0x00,0x23,0x00, 0x04,0x00,0x00,0x00, 0x00,0x01,0x08,0x00, 0,0,0,0, 0x80,0x00,0x04,0x00,0x00,0x00 };
      cmd_w(s, 0x41, p41, 22); }
    { uint8_t z[18] = { 0 }; cmd_w(s, 0x42, z, 18); }
    cmd_w(s, 0x43, p43_cal, 12);
    wait_ready(s, 60);
    static uint8_t adc[ADC_LINES * ADC_PX * 6 + CHUNK_TRAILER];
    read_stream(s, adc, sizeof adc, sizeof adc);
    double adc_mean[ADC_PX * 3];
    mean_lines_u16(adc, ADC_LINES, ADC_PX, adc_mean);
    double adc_off[3] = { 0, 0, 0 };
    for (int x = 0; x < ADC_PX; x++) for (int c = 0; c < 3; c++) adc_off[c] += adc_mean[x * 3 + c] / ADC_PX;

    /* calibration 2: white reference */
    wait_ready(s, 60);
    { uint8_t p[12] = { 0x14,0,0,0, 0x04,0,0,0, 0,0, 0x18,0 }; cmd_w(s, 0x01, p, 12); }
    cmd_w(s, 0x22, p22_base, 12);
    { uint8_t p41[22] = { 0x49,0x00,0x49,0x0e, 0x10,0x00,0x00,0x00, 0x00,0x01,0x01,0x40, 0,0,0,0, 0x80,0x80,0x10,0x00,0x00,0x00 };
      cmd_w(s, 0x41, p41, 22); }
    { uint8_t p42[18] = { 0 };
      for (int c = 0; c < 3; c++) { unsigned v = (unsigned)(adc_off[c] + 0.5); put16(p42 + 6 + 4 * c, v); put16(p42 + 8 + 4 * c, v); }
      cmd_w(s, 0x42, p42, 18); }
    cmd_w(s, 0x43, p43_cal, 12);
    { uint8_t v[2] = { 0xd0, 0x00 }; upload_table(s, 0x04, 0x0100, 0x02, v, 2); }
    wait_ready(s, 60);
    size_t white_len = (size_t)WHITE_LINES * CAL_W * 6 + CHUNK_TRAILER;
    uint8_t *white = malloc(white_len);
    if (!white) fail(s, "out of memory");
    read_stream(s, white, white_len, 51200);
    wait_ready(s, 60);

    /* calibration 3: dark reference */
    { uint8_t p21[28]; memcpy(p21, p21_base, 28); p21[26] = 0x01; cmd_w(s, 0x21, p21, 28); }
    { uint8_t p22[12]; memcpy(p22, p22_base, 12); p22[9] = 0x01; cmd_w(s, 0x22, p22, 12); }
    { uint8_t p41[22] = { 0x49,0x00,0x49,0x0e, 0x40,0x00,0x00,0x00, 0x00,0x01,0x08,0x00, 0,0,0,0, 0x80,0x00,0x40,0x00,0x00,0x00 };
      uint8_t z[18] = { 0 };
      cmd_w(s, 0x41, p41, 22); cmd_w(s, 0x42, z, 18); cmd_w(s, 0x43, p43_cal, 12); cmd_w(s, 0x41, p41, 22); }
    wait_ready(s, 60);
    size_t dark_len = (size_t)DARK_LINES * CAL_W * 6 + CHUNK_TRAILER;
    uint8_t *dark = malloc(dark_len);
    if (!dark) { free(white); fail(s, "out of memory"); }
    read_stream(s, dark, dark_len, 51200);
    wait_ready(s, 60);
    cmd_r(s, 0x86, b, 4);

    /* derive dark offsets, white reference and per-pixel shading */
    static double wm[CAL_W * 3], dm[CAL_W * 3];
    mean_lines_u16(white, WHITE_LINES, CAL_W, wm);
    mean_lines_u16(dark, DARK_LINES, CAL_W, dm);
    double offs[3], wref[3];
    for (int c = 0; c < 3; c++) {
        double dmin = 1e9, wmin = 1e9;
        for (int x = 0; x < OFFS_RANGE; x++)  if (dm[x * 3 + c] < dmin) dmin = dm[x * 3 + c];
        for (int x = 0; x < SHADE_VALID; x++) if (wm[x * 3 + c] < wmin) wmin = wm[x * 3 + c];
        offs[c] = floor(dmin + 0.5);
        wref[c] = floor(wmin * WHITE_GAIN + 0.5);
    }
    emit(s, 1, "dark %.0f/%.0f/%.0f white %.0f/%.0f/%.0f",
         offs[0], offs[1], offs[2], wref[0], wref[1], wref[2]);
    static uint8_t shade[CAL_W * 6];
    for (int x = 0; x < CAL_W; x++)
        for (int c = 0; c < 3; c++) {
            double v = (x < SHADE_VALID) ? wm[x * 3 + c] - (wref[c] - SHADE_K) : 0;
            if (v < 0) v = 0;
            if (v > 65535) v = 65535;
            put16(shade + (x * 3 + c) * 2, (unsigned)(v + 0.5));
        }
    free(white);
    free(dark);

    /* gamma / motor tables, move carriage to scan start */
    upload_table(s, 0x02, 0x3800, 0x00, gamma_8192, 8192);
    upload_table(s, 0x02, 0x3820, 0x00, gamma_8192, 8192);
    upload_table(s, 0x02, 0x3840, 0x00, gamma_8192, 8192);
    upload_table(s, 0x04, 0x0100, 0x02, motor_a_2048, 2048);
    { uint8_t p[12] = { 0xf0,0x02,0,0, 0,0, 0x3f,0, 0x3f,0, 0x18,0 }; cmd_w(s, 0x01, p, 12); }
    cmd(s, 0x05);
    wait_ready(s, 60);

    /* the scan: RAW_LINES x RAW_W, 8-bit RGB, 6 lines per chunk */
    { uint8_t p[12] = { 0x60,0x6e,0,0, 0x68,0, 0x3f,0, 0x3f,0, 0x08,0 }; cmd_w(s, 0x01, p, 12); }
    cmd_w(s, 0x21, p21_base, 28);
    cmd_w(s, 0x22, p22_base, 12);
    { uint8_t p41[22] = { 0x49,0x00,0x49,0x0e, 0xbf,0x0d,0x00,0x00, 0xa1,0x01,0x08,0x80, 0x80,0,0,0x82, 0x80,0xa0,0x06,0x00,0x00,0x00 };
      cmd_w(s, 0x41, p41, 22); }
    { uint8_t p42[18];
      for (int c = 0; c < 3; c++) { put16(p42 + 2 * c, (unsigned)wref[c]); put16(p42 + 6 + 4 * c, (unsigned)offs[c]); put16(p42 + 8 + 4 * c, (unsigned)offs[c]); }
      cmd_w(s, 0x42, p42, 18); }
    cmd_w(s, 0x43, p43_scan, 12);
    upload_table(s, 0x05, 0x0108, 0x02, shade, sizeof shade);
    upload_table(s, 0x04, 0x0100, 0x02, motor_b_2048, 2048);
    wait_ready(s, 60);
    emit(s, 0, "Scanning %dx%d @%d dpi", V330_OUT_W, V330_OUT_H, V330_DPI);

    size_t line_bytes = RAW_W * 3;
    uint8_t *raw = malloc((size_t)RAW_LINES * line_bytes);
    uint8_t *chunk = malloc(LINES_PER_CHUNK * line_bytes + CHUNK_TRAILER);
    if (!raw || !chunk) { free(raw); free(chunk); fail(s, "out of memory"); }
    { uint8_t c[2] = { 0x1e, 0x47 }; usb_write(s, c, 2); }
    for (int l = 0; l < RAW_LINES; ) {
        int nlines = RAW_LINES - l < LINES_PER_CHUNK ? RAW_LINES - l : LINES_PER_CHUNK;
        usb_read(s, chunk, nlines * line_bytes + CHUNK_TRAILER);
        memcpy(raw + (size_t)l * line_bytes, chunk, nlines * line_bytes);
        l += nlines;
        if (l % 600 == 0 || l == RAW_LINES) emit(s, 1, "%d/%d lines", l, RAW_LINES);
    }
    free(chunk);

    /* carriage return, end session */
    wait_ready(s, 120);
    { uint8_t c[2] = { 0x1b, 0x40 }; usb_write(s, c, 2); expect_ack(s, "cmd 1b 40"); }
    cmd_r(s, 0x53, b, 42);
    wait_ready(s, 60);
    cmd_w(s, 0x93, &zero, 1);
    emit(s, 0, "Scan complete (%.1f s)", now() - t0);

    *out_raw = raw;
}

/* ---------------------------------------------------------------- public API */
int v330_present(void)
{
    if (libusb_init(NULL)) return 0;
    libusb_device_handle *hh = libusb_open_device_with_vid_pid(NULL, VID, PID);
    int present = hh != NULL;
    if (hh) libusb_close(hh);
    libusb_exit(NULL);
    return present;
}

v330 *v330_open(const char *fw_dir, v330_log_fn log, void *log_ctx, char *err, size_t errlen)
{
    v330 *volatile s = calloc(1, sizeof *s);
    if (!s) { if (err) snprintf(err, errlen, "out of memory"); return NULL; }
    s->log = log;
    s->log_ctx = log_ctx;

    if (setjmp(s->jb)) {                 /* error landing pad for the block below */
        if (err) snprintf(err, errlen, "%s", s->err);
        if (s->h) { libusb_close(s->h); }
        libusb_exit(NULL);
        free(s);
        return NULL;
    }

    if (libusb_init(NULL)) fail(s, "libusb_init failed");
    s->h = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!s->h) fail(s, "EPSON Perfection V33/V330 (04b8:0142) not found or no permission");
    libusb_set_auto_detach_kernel_driver(s->h, 1);
    if (libusb_claim_interface(s->h, 0)) fail(s, "cannot claim USB interface (scanner busy?)");

    uint16_t st = get_status(s);
    if (st == 0x2800)
        upload_firmware(s, fw_dir);
    else if ((st >> 8) != 0x18 && (st >> 8) != 0x1a && (st >> 8) != 0x58 && (st >> 8) != 0x5a)
        fail(s, "unexpected scanner status %04x", st);
    return s;
}

int v330_scan_page(v330 *s, uint8_t **out_rgb, int *w, int *h, char *err, size_t errlen)
{
    if (setjmp(s->jb)) {
        if (err) snprintf(err, errlen, "%s", s->err);
        return -1;
    }
    uint8_t *raw = NULL;
    scan_sequence(s, &raw);
    uint8_t *rgb = reconstruct_rgb(raw);
    free(raw);
    if (!rgb) { if (err) snprintf(err, errlen, "out of memory building image"); return -1; }
    *out_rgb = rgb;
    if (w) *w = V330_OUT_W;
    if (h) *h = V330_OUT_H;
    return 0;
}

void v330_close(v330 *s)
{
    if (!s) return;
    if (s->h) {
        libusb_release_interface(s->h, 0);
        libusb_close(s->h);
    }
    libusb_exit(NULL);
    free(s);
}
