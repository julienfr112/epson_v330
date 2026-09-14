# EPSON Perfection V33/V330 native Linux driver

A from-scratch user-space driver for the EPSON Perfection V33/V330
(GT-F730 / GT-S630, USB `04b8:0142`). It speaks the scanner's proprietary
native protocol directly over libusb: uploads the firmware, runs the ASIC
calibration (ADC offset, dark level, per-pixel white shading), and scans the
full flatbed at 300 dpi in 24-bit colour. No Epson binaries are needed at run
time.

## Contents

| File | Purpose |
|------|---------|
| `v330_core.c` / `.h` | The scan engine (USB protocol, firmware, calibration, image build) |
| `v330_image.c` / `.h` | Resolution scaling and colour-mode conversion |
| `v330scan.c` | Standalone command-line tool |
| `v330_backend.c` | SANE backend, so Simple Scan / XSane / scanimage can use the scanner |
| `tables.h` | Gamma and motor lookup tables captured from the scanner |
| `esfwad.bin` | Scanner firmware, uploaded at power-on |
| `99-epson-v330.rules` | udev rule granting scanner access |
| `Makefile` | Build and install |

## Build

    make

This needs `gcc`, the libusb-1.0 headers (`libusb-1.0-0-dev`) and the SANE
headers (`libsane-dev`). It produces `v330scan` (CLI) and `libsane-v330.so.1`
(SANE backend).

## Firmware

The scanner needs Epson's firmware blob `esfwad.bin` uploaded at power-on. It is
**not included** in this repository (it is Epson proprietary and cannot be
redistributed). Obtain it from Epson's Linux driver package:

1. Download the `epsonscan2` bundle from
   <https://support.epson.net/linux/en/epsonscan2.php> (or the
   `epsonscan2-non-free-plugin` package).
2. Extract it and copy `usr/share/epsonscan2/esfwad.bin` next to this driver, or
   to `/usr/share/v330/` (where `make install` looks for it).

The tool finds it via `$V330_FIRMWARE_DIR`, the `-f` flag, or a list of standard
directories.

## Use the command-line tool

    ./v330scan -o page.ppm                    # 300 dpi colour, full page
    ./v330scan -r 150 -m gray -o page.pgm     # 150 dpi grayscale
    ./v330scan -m lineart -t 45 -o page.pbm   # black & white, 45% threshold
    ./v330scan -f . -o page.ppm               # look for esfwad.bin in the current dir

Options: `-r` resolution (75/100/150/200/300), `-m` mode (color/gray/lineart),
`-t` lineart threshold in percent, `-f` firmware directory, `-v` verbose.

## Install for Simple Scan / GNOME Document Scanner

    sudo make install

This installs the SANE backend, the firmware (to `/usr/share/v330`), the udev
rule, and registers the backend with SANE. Replug the scanner, then:

    scanimage -L                    # should list the V33/V330

The scanner now appears in Simple Scan, GNOME Document Scanner, XSane, etc.

To remove everything:

    sudo make uninstall

## Modes and resolutions

The scanner is driven at its native **300 dpi optical** profile, which was
validated pixel-for-pixel against Epson's own driver. From that scan the driver
produces:

- **Modes:** colour, grayscale, lineart (black & white). The sensor is always
  RGB in hardware, so grayscale and lineart are derived in software.
- **Resolutions:** 75, 100, 150, 200, 300 dpi, by area-average downscaling of
  the optical scan (real optical detail, correct dimensions).

Resolutions above 300 dpi are not offered: the hardware sensor supports up to
1200 dpi optical, but each higher-resolution mode needs its own captured
calibration profile, which is future work. Requesting more than 300 dpi falls
back to 300 rather than upscaling.

All of this is exposed through the standard SANE options (mode, resolution,
threshold, and scan-area geometry), so Simple Scan shows the usual controls.

## License

MIT (see `LICENSE`). The bundled lookup tables in `tables.h` are small numeric
calibration/gamma tables measured from the device. `esfwad.bin` is Epson's and
is not distributed here.
