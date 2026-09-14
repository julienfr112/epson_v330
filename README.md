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

The scanner ships with only a minimal bootloader and needs the firmware blob
`esfwad.bin` uploaded into its RAM at power-on before it can scan. The file is
**included in this repository** so the driver works out of the box, and because
Epson's own download links for it are not guaranteed to stay available.

**`esfwad.bin` is NOT covered by this project's MIT licence.** It is proprietary
firmware, Copyright (c) Seiko Epson Corporation, extracted from Epson's Linux
driver package (`epsonscan2` / `epsonscan2-non-free-plugin`,
<https://support.epson.net/linux/en/epsonscan2.php>). It is redistributed here
solely so that owners of the hardware can use their scanner on Linux. All rights
remain with Epson, and it will be removed on request from the rights holder.

The driver finds it via `$V330_FIRMWARE_DIR`, the `-f` flag, or a list of
standard directories (including `/usr/share/v330/`, where `make install` puts
it). It is uploaded only when the scanner is in its bootloader state; if the
firmware is already resident from a previous scan, the file is not touched.

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

The driver code is MIT-licensed (see `LICENSE`). The lookup tables in `tables.h`
are small numeric calibration/gamma tables measured from the device. The bundled
`esfwad.bin` is Epson's proprietary firmware and is **not** under the MIT licence
(see the Firmware section above).
