rdzTTGOsonde
============

This a decoder for radiosonde RS41, RS92, DFM06/09/17, M10/M20, and MP3H
based on a TTGO LoRa ESP32 board.

It supports OLED displays (SSD1306, SH1106) and TFT displays (ILI9225, ILI9341/9342).

It also supports feeding data to external applications using WiFi (NOT bluetooth):
- Android app by dl9rdz (see https://github.com/dl9rdz/rdzwx-go for apk download)
- AXUDP (for aprsmap application by oe5dxl, among others)
- KISS TNC (aprs format, mainly useful for APRSdroid app)
- MQTT
- SondeHub tracker
- Chasemapper UDP (experimental)


Please consult the Wiki at https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/Supported-boards
for details on supported boards, and additional setup instructions.

NOTE: Older boards with 26 MHz crystal (TTGO LoRa32 v1, Heltec v1/v2) are not supported by newer dev/main firmware images.


### Radiosonde Support Matrix

Manufacturer | Model | Position | Temperature | Humidity | Pressure
-------------|-------|----------|-------------|----------|----------
Vaisala | RS92-SGP | :heavy_check_mark: | :heavy_check_mark: | :x: | :x:
Vaisala | RS41-SG/SGP/SGM | :heavy_check_mark: | :heavy_check_mark: | :heavy_check_mark: | :heavy_check_mark: (for -SGP)
Graw | DFM06/09/17 | :heavy_check_mark: | :heavy_check_mark: | :x: | :x:
Meteomodem | M10 | :heavy_check_mark: | :heavy_check_mark: | :heavy_check_mark: | Not Sent
Meteomodem | M20 | :heavy_check_mark: | :x: | :x: | Not Sent
Meteo-Radiy | MP3-H1 (MRZ-H1) | :heavy_check_mark: | :x: | :x: | :x: 

SondeHub integration has mainly been tested with RS41 and DFM. 


Support for other radiosondes that use AFSK modulation is not feasible with the TTGO hardware.
In particular, decoding iMet-1/iMet-4 radiosondes is not practical (iMet-5x seems to use FSK,
so should be feasible to implement).

Adding support for LMS6 (see issue #48) and ims100 (see branch ims100) could be feasible,
but currently I don't have plans to do add this myself. Well-tested pull requests will of
course be considered for inclusion :-).

## Installation

You can download the latest binary automated build for the development and testing branches [here](http://rdzsonde.org/download.html), the binary includes everything including configuration files so any existing settings will be reset. 

To update an existing installation to the latest development or master version you can use the [OTA](https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/Other-features#over-the-air-updates) update feature.

The downloaded .bin file can be flashed to your ESP32 board using [esptool](https://github.com/espressif/esptool) or [ESP32 Download Tool](https://www.espressif.com/en/support/download/other-tools)

### esptool

You can run the following command replacing `<filename.bin>` with the path to the downloaded .bin file. 

If you encounter errors with the device COM not automatically being detected replace `/dev/cu.SLAB_USBtoUART` with `COM<X>`.

```
esptool --chip esp32 --port /dev/cu.SLAB_USBtoUART --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m --flash_size detect 0x1000 <filename.bin>
```

### ESP32 Download Tool

The binary file can also be installed using the GUI application with the [following](http://rdzsonde.mooo.com/) settings.

## Button commands

You can use the button on the board (not the reset button, the second one) to
issue some commands. The software distinguishes between several inputs:

- SHORT	Short button press (<1.5 seconds)
- DOUBLE  Short button press, followed by another button press within 0.5 seconds
- MID	Medium-length button press (2-4 seconds)
- LONG	Long button press (>5 seconds)

You can optionally use a second button, which you have to add manually to your board.
See https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/Hardware-configuration for details.


## Wireless configuration

On startup, as well as after a LONG button press, the WiFI configuration will
be started.  The board will scan available WiFi networks, if the scan results
contains a WiFi network configured with ID and Password in networks.txt, it
will connect to that network in station mode. If no known network is found, or
the connection does not suceed after 5 seconds, it instead starts in access point
mode. In both cases, the ESP32's IP address will be shown in tiny letters in the
bottom line. Then the board will switch to scanning mode.

## Scanning mode

In the scanning mode, the board will iterate over all channels configured in
channels.txt, trying to decode a radio sonde on each channel for about 1 second.
If a valid signal is found, the board switches to receiving mode on that channel.
A SHORT buttong press will also switch to receiving mode.

## Receiving mode

In receiving mode, a single frequency will be decoded, and sonde info (ID, GPS
coordinates, RSSI) will be displayed. The bar above the IP address indicates,
for the last 18 frames, if reception was successfull (|) or failed (.), or had
some errors (E), e.g., CRC check failed.
 
A DOUBLE press will switch to scanning mode.

A SHORT press will switch to the next channel in channels.txt

A SHORT press on the second button will switch to a different display screen.

## Spectrum mode

A medium press will active scan the whole band (400..406 MHz) and display a
spectrum diagram (each line == 50 kHz)
For TTGO boards without configurable button there are some new parameter in config.txt:
- spectrum=10       // 0=off / 1-99 number of seconds to show spectrum after restart
- timer=1           // 0=off / 1= show spectrum countdown timer in spectrum display
- marker=1          // 0=off / 1= show channel edge freq in spectrum display

## Setup

see [Wiki](https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/Installation)

## Enhancements in this fork (PY5OL)

This fork (version id `py5ol-*`) extends the upstream firmware with a reworked,
multi-user web interface, an auto_rx-style scanner, a browser spectrum plot, richer
live-map tooling, an offline upload cache and a large round of fixes to long-standing
upstream bugs. The new features are listed first, followed by the upstream bugs
this fork fixes.

### Install / update this fork

The latest complete image for this fork is available at
[`https://rdzttgo.nano.dev.br/firmware-image.bin`](https://rdzttgo.nano.dev.br/firmware-image.bin).
It is a full USB-flashable image (bootloader + partitions + app + LittleFS data), so flashing
it resets any existing settings and files. Flash it over USB with `esptool` (see the
[Installation](#installation) section above), or update an existing install over the air by
pointing the OTA update page at the PY5OL server.

### New features

**Web interface & security**
- Multi-user authentication: stateless JWT sessions with device-key–hashed passwords,
  per-user access levels (admin/level-2 gates actions such as SD-card format), anonymous
  access derived from the user count, and a sliding idle-session timeout.
- User-management UI (add/edit/delete users, confirm-password field), header user menu,
  and tabs shown only for your access level.
- Modernized, consistent input/form styling across all pages.
- Login returns you to the last page/tab and survives a device reboot.
- Upload/download of `config.txt` and `qrg.txt`, plus confirmation for destructive
  actions (Format SD, Reboot).
- **Dark mode for the web UI** — follows the OS by default, falls back to dark between 20:00 and
  08:00, and a sun/moon switch in the nav pins an explicit choice per browser.

**Live map**
- Restores the last sonde and its flight track from browser session storage after a
  device reboot.
- Clickable flight-path trail: opens a popup with that point's position, altitude,
  speed + direction, climb, signal (dBm) and GPS time.
- Extra popup links, alongside GMaps/OSM/GeoApp: a "Topo" (topographic-map.com) link and
  a "Route" link (Google Maps driving directions from the receiver to the point).
- Shows the RS41 power-off (kill-timer) time; timestamps rendered in the browser's local
  timezone.

**Spectrum scan-plot (web)**
- New browser spectrum page fed by the radio's idle sweeps (only while a browser is
  watching; the first sweep runs immediately).
- Crosshair readout of frequency/power, frequency zoom & pan, touch-drag, and configured
  noise-floor and signal-threshold lines.

**Auto-scan (auto_rx-style)**
- Finds sondes on spectrum peaks and tries active frequency-list QRGs before those peaks.
- Configurable scan dwell, iteration count and per-sonde-type decode time; decimal start
  frequencies (e.g. `400.2`); and an exclusion list for known noise frequencies.

**OTA / firmware updates**
- Redesigned update page that validates online updates by filesystem version.
- Upload firmware and filesystem images directly from the update page.
- Optional PY5OL update server plus self-hosted server tooling; `FS_MINOR` auto-bumps
  when the filesystem image content changes.
- **Cross-fork update warning** — the update page detects when a chosen update source
  (Official vs PY5OL) differs from the installed firmware's fork and requires a
  type-to-confirm before switching, so you can't change forks by an accidental tap.

**Receiver**
- Internal LNA-boost configuration (SX1278 receiver gain).
- External LNA support: reported RSSI corrected by subtracting the configured external
  LNA gain.
- SondeHub: reports measured frequency plus RS41 `tx_frequency`/mainboard firmware.

**Networking**
- Wi-Fi mode 5 (client, or AP + retry keeping the AP up while retrying to connect).

**Notifications**
- **Landing-near-me push notifications** — sends a phone push when a sonde is descending, below
  a configurable altitude, and within a configurable distance of the station.
- **New-sonde push notifications** — pushes once per serial the first time a sonde is decoded
  with a position, with type, frequency, distance and signal strength.
- Both alerts link to the SondeHub tracker and share one ntfy server, topic and token, set under
  "Sonde notifications (ntfy)" on the config page (plain-HTTP endpoint — the firmware has no TLS).
  `notify.active` selects which alerts are sent: 0=off, 1=landing, 2=new sonde, 3=both.

**Offline upload cache**
- Decoded frames are buffered in a shared RAM ring so each network uploader (SondeHub,
  APRS, MQTT, Chasemapper, SondeSeeker) independently backfills what it missed once its
  own link recovers — no more lost frames when the internet drops while Wi-Fi stays up.
  Configurable via `cachesize` (frames to buffer; `0` disables, default 120). Live uploads
  keep full fidelity; only backfilled gap frames omit RS41 calibration fields.

**Tooling & build**
- `Makefile` wrapping the common PlatformIO targets.
- Web-UI preview server (`scripts/preview_server.py`) for editing pages without hardware.
- Mock SondeHub server (`scripts/mock_sondehub.py`, `make mock-sondehub`): a local stand-in
  for the SondeHub telemetry API with a live browser dashboard, for exercising the offline
  upload cache — including its outage/backfill behaviour — without hardware or the real service.

**Serial data output**
- **Serial position output** — write decoded sonde position (and the RX station position) to
  `Serial1`/UART1 on a configurable TX pin and baud rate, in NMEA / JSON / CSV / Chasemapper
  `PAYLOAD_SUMMARY` format (configurable; disabled by default).

### Upstream bug fixes

Bugs present in the upstream firmware that this fork fixes:

- **SondeHub connection reliability** — the SondeHub client state machine could hang
  forever: a lost lwIP DNS callback or a stalled non-blocking `connect()` left it stuck
  until a hardware restart, silently killing all SondeHub uploads. Added connection
  watchdogs (DNS lookup and TCP connect) and hardened the socket `fd_set` re-init and
  close/reset handling, so a failed attempt now retries instead of freezing.
- **Wi-Fi auto-reconnect** — Wi-Fi never recovered after an established station link
  dropped: the disconnect handler powered the radio down and the reconnect state machine
  could park permanently. It now keeps the radio up and drives recovery in station modes
  (1/3/5) instead of requiring a reboot.
- **MQTT** — fixed an overlapping `snprintf` when building the uptime payload.
- **Decoders** — M10/M20 (FEC repair buffers reset on every retune to prevent false locks;
  M20 frame length clamped to avoid a negative CRC index), MP3H (operator precedence in the
  receive timeout), RS41 (subframe number bounded to prevent a heap overflow), RS92 (scan
  the full PRN-indexed ephemeris array).
- **Data / export** — GPX export now writes real telemetry instead of clobbering it with
  test data; the live map no longer plots a stale position when the frame counter advances
  without a fresh GPS fix; fixed a display memory leak on layout reload.
- **Memory-safety hardening** — buffer-bounds, null-check and use-after-free fixes across
  ~35 code paths (display, APRS, SondeHub, ShImport, MQTT, SondeSeeker, `config.txt`/`qrg.txt`
  parsing, GPS/NMEA, ephemeris download, OTA/`execOTA`, the file editor and SD-card paths) to
  prevent overflows and crashes under low heap, including a format-string fix in ShImport.

### Other changes

Fork-internal polish and follow-ups: UI/CSS refinements (selects, scrollbars, button
layouts, header-menu vs. tab overlap), reboot-aware progress dialogs, scan-plot rendering
tweaks, auto-scan and OTA follow-ups, Wi-Fi reconnect refinements, auth follow-ups
(login-error alerts, wrong-password reload), cross-version browser cache-busting.

