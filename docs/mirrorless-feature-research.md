# Mirrorless Camera Feature-Complete Research

**Target project:** `picamera` — C++20 libcamera front-end for Raspberry Pi HQ
Camera (IMX477, 4056x3040) on a Pi Zero 2 W, with a 1.44" 128x128 SPI LCD
(Waveshare HAT), 4 buttons (shutter joystick + 3 keys), and an ADS1115 battery
monitor.

**Date:** 2026-09-04 — generated from web search + codebase inspection.

## Sources & methodology

- Brand-specific feature summaries were compiled from official user manuals,
  support sites, and reputable review/setting guides discovered via web search.
- Feasibility on the Pi platform is based on the IMX477 sensor, the Raspberry
  Pi/VC4 libcamera pipeline, and the constraints of the Waveshare 128x128 SPI
  LCD + 4-button input.
- Existing project features were verified by inspecting `src/settings_menu.cpp`,
  `src/camera_mode.h`, `src/camera_config.h`, `src/controls.cpp`, `README.md`,
  and `AGENTS.md`.

References are listed at the end of this document.

## Per-brand feature summary

### Canon (EOS R5 / R6 / R3 / M6 Mark II)

Canon's still-photo menus are organized in Shooting tabs and a Custom tab.

Major categories:

- **Shooting modes:** P, Tv (S), Av (A), M, B, Custom (C1-C3), and numerous
  Scene / Creative Assist modes on consumer bodies.
- **Image quality:** RAW / RAW+JPEG / JPEG (with quality levels),
  Dual Pixel RAW (on R5), cropping / aspect ratio (1:1, 16:9, etc.),
  color space (sRGB / Adobe RGB).
- **Exposure / metering:** AEB, ISO speed settings / auto ISO,
  exposure compensation, metering modes (Evaluative, Partial, Spot,
  Center-weighted), AE lock, flicker reduction.
- **White balance:** AWB (ambience / white priority), daylight, shade, cloudy,
  tungsten, fluorescent, flash, custom, color temperature, WB correction /
  bracketing.
- **Picture style:** Standard, Portrait, Landscape, Fine Detail, Neutral,
  Faithful, Monochrome, User-defined; plus Sharpness, Contrast, Saturation,
  Color tone, Clarity, and Lens aberration correction (distortion, vignetting,
  CA).
- **Noise / dynamic range:** Long exposure NR, high ISO NR, Auto Lighting
  Optimizer, Highlight Tone Priority, HDR, Multiple Exposure.
- **Drive modes:** Single, Continuous (Lo/Hi), Self-timer, Bracketing (AE / WB
  / focus), Interval timer, Bulb timer.
- **Focus:** One-Shot AF, Servo AF, Manual Focus; AF methods including
  Face+Tracking, Spot, Zone, 1-point; AF point selection, focus peaking.
- **Display / UI:** Image review, histogram, grids, high-speed display,
  exposure simulation, shooting info display, viewfinder display format.
- **Setup / power:** Battery info, auto power off, screen / viewfinder
  brightness, file numbering, folder structure, language, date/time,
  Wi-Fi / Bluetooth.
- **Custom modes:** C1-C3 custom shooting modes and My Menu.

Canon R5 still-photo menu reference:
<https://support.usa.canon.com/kb/s/article/ART178244>
Canon EOS M6 Mark II manual:
<https://gdlp01.c-wss.com/gds/2/0300036082/03/eosm6-mk2-ug3-en.pdf>

### Sony (A7 IV / A7R V / A1 / ZV-E10)

Sony's newer menu system uses tabbed pages (Shooting, Exposure/Color, Focus,
Playback, Network, Setup, etc.) plus a customizable Fn / My Menu.

Major categories:

- **Shooting modes:** P, A, S, M; Auto, Scene, S&Q, Movie.
- **Image quality:** JPEG / HEIF (4:2:0 or 4:2:2), RAW, RAW+JPEG / RAW+HEIF,
  RAW compression (uncompressed / compressed / lossless compressed),
  image size L/M/S.
- **Creative Look / Picture Effect:** ST, PT, NT, VV, VV2, FL, IN, SH, BW, SE,
  plus 6 custom looks; fine-tuning for contrast, highlights, shadows, fade,
  saturation, sharpness, sharpness range, clarity.
- **Exposure / metering:** ISO (auto/manual with range), exposure compensation
  (+/-5 EV), metering (Multi, Center, Spot, Entire Screen Avg, Highlight),
  anti-flicker, zebra, D-Range Optimizer.
- **White balance:** AWB, presets (daylight, shade, cloudy, incandescent,
  fluorescent, flash, underwater), color temperature, custom WB, WB shift.
- **Drive modes:** Single, Continuous (Lo/Hi/Hi+), Self-timer, Bracket
  (AE/WB/DRO), Time-lapse, Bulb, Silent/electronic shutter.
- **Focus:** AF-S, AF-C, AF-A, DMF, MF; Focus Area (Wide, Zone, Center, Spot,
  Expand Flexible Spot, Tracking), face/eye/animal/bird/insect detection,
  focus peaking, focus magnifier.
- **Display / UI:** Histogram, zebra, grid, level gauge, aspect markers,
  live-view exposure/WB simulation, info overlay, image review.
- **Setup / connectivity:** Battery, auto power off, display brightness,
  language, file numbering, Wi-Fi, Bluetooth, USB, remote control, GPS
  (via phone).
- **Custom:** My Menu, custom keys, memory recall (1-3/4).

Sony A7 IV menu guide:
<https://www.wimarys.com/sony-a7-iv-advanced-manual/>
Sony A7R V Creative Look:
<https://helpguide.sony.net/ilc/2230/v1/en/contents/TP0002911200.html>

### Nikon (Z9 / Z8 / Z6 II / Z50)

Nikon uses a Photo Shooting Menu, Custom Settings Menu, and the `i` quick menu.

Major categories:

- **Shooting modes:** P, S, A, M, U1-U3 user settings, auto / scene.
- **Image quality:** JPEG/RAW/HEIF, image size L/M/S, RAW compression /
  bit depth, color space (sRGB / Adobe RGB).
- **Picture controls:** Standard, Neutral, Vivid, Monochrome, Portrait,
  Landscape, Flat, plus custom picture controls with fine-tuning.
- **Exposure / metering:** ISO sensitivity (auto with range), exposure comp,
  metering (Matrix, Center-weighted, Spot, Highlight-weighted), AE-L,
  exposure delay, flicker reduction.
- **White balance:** Auto, natural light auto, direct sunlight, cloudy, shade,
  incandescent, fluorescent, flash, custom presets (d-1 to d-6),
  color temperature, fine-tuning on A-B / G-M axes, WB bracketing.
- **Drive modes:** Single, Continuous Lo/Hi, self-timer, bracketing
  (AE/WB/Active D-Lighting/focus), interval, time-lapse, multiple exposure.
- **Focus:** AF-S, AF-C, AF-F, MF; AF-area modes (Pinpoint, Single, Dynamic,
  Wide-area S/L/C1/C2, 3D-tracking, Auto-area AF, subject tracking,
  face/eye detection), focus peaking.
- **Image quality tools:** Active D-Lighting, long exposure NR, high ISO NR,
  vignette control, diffraction compensation, auto distortion control.
- **Display / UI:** Live view, histogram, grid, level gauge, focus peaking,
  blinking highlights, info display, image review, auto review.
- **Setup / connectivity:** Battery, auto power off, monitor brightness,
  language, date/time, file numbering, folder management, Wi-Fi / Bluetooth /
  USB tether, remote, GPS.
- **Custom:** Shooting menu banks, custom `i` menu, custom controls.

Nikon Z9 menus:
<https://onlinemanual.nikonimglib.com/z9/en/the_menus_8.html>
Nikon Z6 II / Z7 II menu guide:
<https://onlinemanual.nikonimglib.com/z7II_z6II/en/09_menu_guide_03.html>

### Fujifilm (X-T5 / X-H2 / X-S20)

Fujifilm's still-photo menus are grouped into IMAGE QUALITY SETTING and
SHOOTING SETTING tabs.

Major categories:

- **Exposure / shooting modes:** P, S, A, M; program shift.
- **Image quality:** Image size, RAW recording, JPEG/HEIF, film simulation
  (Provia, Velvia, Astia, Classic Chrome, Acros, etc.),
  monochromatic color, grain effect, color chrome effect / FX blue,
  smooth skin effect, tone curve, color, sharpness, high ISO NR,
  long exposure NR, clarity, lens modulation optimizer.
- **White balance:** AWB (white / ambience priority), daylight, shade,
  cloudy, fluorescent (x3), incandescent, underwater, custom, color
  temperature.
- **Dynamic range:** Dynamic Range (DR100/200/400), D Range Priority,
  dynamic range bracketing.
- **Drive modes:** Single, continuous, bracket (AE / WB / ISO / film sim /
  DR / focus), self-timer, interval, multiple exposure, HDR.
- **Focus:** AF-S, AF-C, MF, AF area modes, face/eye detection, pre-AF,
  focus peaking.
- **Metering / exposure:** Photometry (multi, spot, center, average),
  exposure compensation, AE-L, ISO, shutter type (mechanical / electronic /
  EFCS).
- **Display / UI:** Histogram, grid, live view, focus peaking, info overlay,
  image review.
- **Setup / connectivity:** Battery, power save, date/time, file numbering,
  language, connection to smartphone, etc.

Fujifilm X-T5 manual:
<https://fujifilm-dsc.com/en/manual/x-t5/x-t5_manual_en_s_f.pdf>
Fujifilm X-T5 shooting settings:
<https://fujifilm-dsc.com/en/manual/x-t5/menu_shooting/shooting_setting/index.html>

### Panasonic (Lumix S5 II / GH6)

Panasonic uses a [Photo] menu, [Custom] menu, and a Quick menu.

Major categories:

- **Shooting modes:** P, A, S, M, iA, custom C1-C3.
- **Image quality:** JPEG/RAW/RAW+JPEG, image size / aspect,
  photo styles (Standard, Vivid, Natural, Monochrome, L. Monochrome,
  Cinelike, V-Log L, plus 10 custom "My Photo Styles"), filter settings.
- **Exposure / metering:** ISO (auto with range), exposure compensation,
  metering (Multiple / Center / Spot / Highlight-weighted), AE-L,
  anti-flicker, constant preview.
- **White balance:** AWB, presets, color temperature, custom WB, fine-tuning.
- **Drive modes:** Single, burst (SH / H / L), 4K/6K photo, self-timer,
  bracket (AE / WB / focus / aperture), time-lapse.
- **Focus:** AFS / AFF / AFC, MF, face/eye/animal, area modes, focus peaking.
- **Image stabilization / shutter:** Electronic / mechanical shutter,
  shutter delay, image stabilizer.
- **Display / UI:** Histogram, zebra, photo grid, level gauge,
  blinking highlights, framing outline, live view boost, night mode,
  auto review, info overlay.
- **Setup / connectivity:** Battery, auto power off, display brightness,
  language, date/time, file numbering, Wi-Fi, Bluetooth, USB, remote.

Panasonic S5II manual:
<https://tda.panasonic-europe-service.com/GetDoc.aspx?did=271552&fmt=pdf&lang=en&src=1>
Panasonic GH6 manual:
<https://tda.panasonic-europe-service.com/docs/2z67ae4753z1z41d87z656ez706466z21zde72901cf322c7cff1740366c0b71c427639b63b/tsn3/data/ALL/DCGH6P/OI/1003003/dvqp2441za.pdf>

## Unified "feature-complete" mirrorless checklist

The table below lists common mirrorless feature categories and individual
features, then marks whether each is feasible on the Pi HQ Camera + libcamera
setup, and whether it is already implemented in `picamera`.

| Feature | Category | Feasible on Pi HW? | Notes |
|---|---|---:|---|
| P, S, M exposure modes | Exposure | **Yes** | Done. S/M selectable; P mode via both shutter and gain auto. |
| Aperture priority (A) | Exposure | **No** | No electronic aperture control on the HQ Camera C/CS mount. |
| Auto / Scene modes | Exposure | **Partial** | Auto can be emulated; dedicated scene modes (portrait, landscape, etc.) are mostly post-processing recipes. |
| Exposure compensation (+/-EV) | Exposure | **Yes** | Already implemented (`EV` in `src/settings_menu.cpp:602`). |
| AE lock | Exposure | **Yes** | Already implemented via long joystick hold (`src/preview.cpp:600-606`). |
| AE bracketing | Exposure | **Yes** | Done (`bracketEv` array, `captureBracket()` in `src/camera.h:26`). |
| Metering: multi/spot/center | Exposure | **Yes** | Already mapped to libcamera `AeMeteringMode` (`src/camera_config.h:19-23`). |
| Histogram (live) | Exposure | **Yes** | Already implemented (`src/camera_mode.h:128-132`). |
| Zebra overexposure warning | Exposure | **Yes** | Already implemented (`src/camera_mode.h:37-42`, `drawZebra`). |
| ISO auto | Exposure | **Yes** | Already supported by setting `analogueGain = 0` (`src/settings_menu.cpp:247-253`). |
| ISO manual with range | Exposure | **Partial** | Manual gain steps exist; true auto-ISO range limits would need tuning. |
| Shutter speed manual | Exposure | **Yes** | `kShutterSteps` array (`src/settings_menu.cpp:43-68`) maps to `ExposureTime`. |
| Bulb / timed long exposure | Exposure | **Partial** | Electronic shutter only; long manual times are supported up to the sensor/ISP limit. |
| Mechanical/electronic/flash sync | Exposure | **No** | Only electronic rolling shutter on IMX477. |
| AF-S / AF-C / AF-A | Focus | **No** | No on-sensor phase-detect AF; libcamera's AF controls are not usable for IMX477. |
| AF area modes | Focus | **No** | No true autofocus hardware. |
| Face/eye detection | Focus | **No** | Not exposed by libcamera for IMX477; too CPU-expensive to run on the viewfinder stream. |
| AF point selection | Focus | **No** | No AF points. |
| Focus peaking | Focus | **Yes** | Already implemented (`src/camera_mode.h:138-141`, `drawFocusPeaking`). |
| Focus magnifier | Focus | **Yes** | Implemented as a digital crop/zoom in live view (`focusMagnify` in `CameraSettings`, panned in `src/preview.cpp`). |
| Depth-of-field preview | Focus | **No** | Aperture is mechanically set by the lens. |
| AWB | White balance | **Yes** | Already supported (`awbEnable`, `awbMode`). |
| WB presets (daylight/shade/cloudy/fluorescent/etc.) | White balance | **Yes** | libcamera supports auto, incandescent, tungsten, fluorescent, indoor, daylight, cloudy, custom (`AwbModeEnum` reference). Project currently exposes most of these. |
| Kelvin manual | White balance | **Yes** | Already implemented (`wbKelvin`, `src/settings_menu.cpp:369-371`). |
| WB shift / fine-tune | White balance | **Yes** | Already partially implemented via `wbRedGain` / `wbBlueGain` (`src/camera_mode.h:89-90`). |
| WB bracketing | White balance | **Yes** | Implemented (`BracketType::WB`; varies R/B gains per frame in `src/preview.cpp`). |
| Custom WB | White balance | **Yes** | Done (`AwbMode::Custom` + `ColourGains`; one-touch WBSET measures live frame chroma). |
| JPEG quality / compression | Image quality | **Yes** | Already implemented (`jpegQuality`, `src/settings_menu.cpp:334-337`). |
| RAW+JPEG simultaneous | Image quality | **Yes** | Done (`OutputFormat::RawJpeg` + `DngJpeg`; `RawJpegWriter` in `src/output_writer.cpp`, `captureDngJpegAsync()` in `src/preview.cpp`). |
| RAW (DNG) | Image quality | **Yes** | Already implemented (`src/dng.cpp`, `OutputFormat::DNG`). |
| Image size L/M/S | Image quality | **Partial** | Feasible by resizing/cropping the full-res stream; native 12 MP is always read out. |
| Aspect ratio (3:2/16:9/1:1/4:3) | Image quality | **Yes** | `AspectRatio` enum exists (`src/camera_mode.h:44-49`); implementation is mostly a crop/mask. |
| Color space sRGB / AdobeRGB | Image quality | **Partial** | JPEGs are sRGB; tagging AdobeRGB is possible but the Pi JPEG pipeline does not actually encode in a wider gamut. |
| Picture styles / film simulations / looks | Image quality | **Partial** | Done via B/C/S/Sharp preset tuples (`PictureStyleParams`); full "Creative Look" tables not loaded. |
| Sharpness, contrast, saturation | Image quality | **Yes** | Already implemented (`src/settings_menu.cpp:381-395`). |
| Long exposure NR / high ISO NR | Image quality | **Partial** | libcamera `NoiseReductionMode` exists; heavy CPU NR would be too slow on a Pi Zero. |
| Lens corrections (distortion/CA/vignette) | Image quality | **No** | No lens profile database; cannot identify attached lens. |
| Dynamic range expansion | Image quality | **Partial** | No single-shot DRO; HDR can be emulated with AE bracketing. |
| Single shot | Drive / capture | **Yes** | Already the default. |
| Continuous shooting (Lo/Hi) | Drive / capture | **Partial** | Possible but limited by full-res NV12 buffer write time and Pi Zero CPU. A small burst is realistic. |
| Self-timer (2/5/10 s) | Drive / capture | **Yes** | Done (`timerDuration` preset enum with 2/5/10 s options). |
| Bracketing (AE/WB/ISO/focus/DR) | Drive / capture | **Partial** | AE, WB, ISO bracket done (`BracketType` enum; per-frame overrides in `src/preview.cpp`; ISO via `DualStream::setStillGainOverride()`). Focus/DR bracket not feasible. |
| Interval / timelapse | Drive / capture | **Yes** | Already implemented (`src/timelapse.cpp`, `src/timelapse_runner.cpp`). |
| Bulb mode | Drive / capture | **Partial** | Long `ExposureTime` only; no physical bulb shutter. |
| Quiet/electronic shutter | Drive / capture | **Yes** | Only electronic rolling shutter is available. |
| Multi-shot / pixel shift | Drive / capture | **No** | Sensor lacks pixel-shift and in-body stabilization for alignment. |
| Live view | Display / UI | **Yes** | Already implemented in preview mode. |
| RGB/luminance histogram | Display / UI | **Yes** | Live luminance histogram from NV12 Y plane (`drawHistogram()` in `src/camera_mode.cpp`). RGB histogram feasible but triples per-frame cost. |
| Zebra / overexposure warning | Display / UI | **Yes** | Already implemented. |
| Focus peaking | Display / UI | **Yes** | Already implemented. |
| Grid lines (rule of thirds, square, diagonal) | Display / UI | **Yes** | Done (thirds/square/diagonal/golden-ratio via `GridType` enum and `drawGrid()`). |
| Electronic level | Display / UI | **No** | No IMU/accelerometer in the current hardware. |
| Aspect ratio mask | Display / UI | **Yes** | Feasible with the existing `AspectRatio` support. |
| Overexposure "blinkies" | Display / UI | **Yes** | Implemented in both live view (zebra) and playback (`drawImageViewHistogramAndBlinkies()`). |
| Info overlay (settings on screen) | Display / UI | **Partial** | Basic overlay exists; richer settings readout can be added. |
| Image review duration | Display / UI | **Yes** | Already in Review mode. |
| Review zoom / focus check | Display / UI | **Partial** | Zoom is limited by the 128x128 display; a 2x/4x magnifier is feasible. |
| Playback browser | Playback | **Yes** | Already implemented (`Playback`/`ImageView` modes). |
| Delete / protect | Playback | **Partial** | Delete implemented (two-press confirmation in ImageView mode); protect is a file attribute. |
| Slideshow | Playback | **Yes** | Implemented (auto-advance through playback images in Playback mode). |
| Histogram in playback | Playback | **Yes** | Done (`drawImageViewHistogramAndBlinkies()` in `src/camera_mode.cpp`; decodes saved JPEG, draws histogram + blinkies in ImageView mode). |
| Highlight warning in playback | Playback | **Yes** | Done (per-channel clipping detection in `drawImageViewHistogramAndBlinkies()`). |
| Rotate / crop / rating | Playback | **No / partial** | Crop can be simulated at view time; rating requires metadata storage. |
| Battery percentage icon | Power / setup | **Yes** | Already implemented (`src/battery.cpp`, `src/font.cpp`). |
| Auto power off / sleep | Power / setup | **Yes** | Already implemented (`powerSaveTimeout`, `src/settings_menu.cpp:426-436`). |
| Display brightness | Power / setup | **Yes** | Already implemented. |
| Beep / shutter sound | Power / setup | **No** | No speaker/buzzer in the listed hardware. |
| Language selection | Power / setup | **Partial** | Feasible for strings; no current translation framework. |
| Date / time | Power / setup | **Partial** | Can use Linux system clock; no dedicated RTC. |
| File numbering | Power / setup | **Yes** | Done (sequential `IMG_xxxx` + timestamp modes; `FileNamingMode`, `makeSequentialFilename()`). |
| Folder structure | Power / setup | **Yes** | Done (date-based subfolders via `ensureDateSubfolder()` in `src/preview.cpp`; toggle with `useDateSubfolders`). |
| USB mode | Power / setup | **No** | Standard RPi USB (host/gadget) is a system setting, not the camera firmware. |
| Storage format | Power / setup | **No** | Filesystem format must be done at the OS level. |
| Wi-Fi transfer / remote | Connectivity | **Yes** | Done (HTTP server on port 8080; `src/wifi_server.cpp`). |
| Bluetooth | Connectivity | **Yes** | Done (serial SPP server, RFCOMM channel 1, optional via BlueZ). |
| USB tethering | Connectivity | **Partial** | Possible via Pi gadget mode; large integration effort. |
| GPIO / wired remote | Connectivity | **Yes** | Already uses joystick/buttons; could add dedicated remote input. |
| GPS | Connectivity | **No** | No GPS receiver in current hardware. |

## Implemented features

All items below are done. Implementation references are to the current codebase.

1. **RAW+JPEG simultaneous capture** — `OutputFormat::RawJpeg` (NV12 + JPEG) and
   `OutputFormat::DngJpeg` (DNG + JPEG); `RawJpegWriter` in `src/output_writer.cpp`,
   `captureDngJpegAsync()` in `src/preview.cpp`.
2. **Exposure mode selection (P / S / M)** — `ExposureMode` enum drives `aeEnable`,
   `exposureTime`, and `analogueGain` in `settingsToCameraConfig()`.
3. **Self-timer presets (2 s / 5 s / 10 s)** — `timerDuration` uses a preset enum.
4. **Custom white balance + AWB presets** — `AwbMode::Custom` with measured colour
   gains and named presets (including "shade" and "flash").
5. **Image size / crop (L / M / S)** — center crop and/or downscale before saving
   JPEG via image size presets.
6. **Continuous / burst shooting** — queues several still requests with
   buffer/CPU awareness on the Pi Zero.
7. **Histogram and highlight warning in playback** —
   `drawImageViewHistogramAndBlinkies()` in `src/camera_mode.cpp` decodes the
   saved JPEG and draws a histogram + blinkie overlay in ImageView mode.
8. **Focus magnifier / zoom in live view** — digital crop/zoom of the viewfinder
   via `focusMagnify` in `CameraSettings`, panned in `src/preview.cpp`.
9. **Additional grid options (diagonal / golden ratio)** — extended `GridType`
   enum and `drawGrid()`.
10. **Wi-Fi image transfer / remote control** — HTTP server on port 8080
    (`src/wifi_server.cpp`) serving file listings, status/settings JSON, and
    capture triggers.
11. **Picture-style / film-simulation presets** — B/C/S/Sharp preset tuples
    (`PictureStyleParams` in `src/settings_menu.cpp`), applied to existing image
    tuning sliders.
12. **File numbering and date-based folder structure** — sequential `IMG_xxxx`
    and timestamp naming modes (`FileNamingMode`, `makeSequentialFilename()`);
    date-based subfolders via `ensureDateSubfolder()` in `src/preview.cpp`.

## References

1. Canon EOS R5 still-photo tab menus:
   <https://support.usa.canon.com/kb/s/article/ART178244>
2. Canon EOS M6 Mark II advanced user guide:
   <https://gdlp01.c-wss.com/gds/2/0300036082/03/eosm6-mk2-ug3-en.pdf>
3. Sony A7 IV advanced menu guide:
   <https://www.wimarys.com/sony-a7-iv-advanced-manual/>
4. Sony A7R V Creative Look help guide:
   <https://helpguide.sony.net/ilc/2230/v1/en/contents/TP0002911200.html>
5. Nikon Z9 online menu manual:
   <https://onlinemanual.nikonimglib.com/z9/en/the_menus_8.html>
6. Nikon Z6 II / Z7 II shooting menu guide:
   <https://onlinemanual.nikonimglib.com/z7II_z6II/en/09_menu_guide_03.html>
7. Fujifilm X-T5 manual PDF:
   <https://fujifilm-dsc.com/en/manual/x-t5/x-t5_manual_en_s_f.pdf>
8. Fujifilm X-T5 shooting settings:
   <https://fujifilm-dsc.com/en/manual/x-t5/menu_shooting/shooting_setting/index.html>
9. Panasonic Lumix S5II manual:
   <https://tda.panasonic-europe-service.com/GetDoc.aspx?did=271552&fmt=pdf&lang=en&src=1>
10. Panasonic Lumix GH6 manual:
    <https://tda.panasonic-europe-service.com/docs/2z67ae4753z1z41d87z656ez706466z21zde72901cf322c7cff1740366c0b71c427639b63b/tsn3/data/ALL/DCGH6P/OI/1003003/dvqp2441za.pdf>
11. libcamera controls namespace reference:
    <https://docs.libcamera.org/master/public-api/namespacelibcamera_1_1controls.html>
12. libcamera `AwbModeEnum` / `AeMeteringModeEnum` definitions:
    <https://github.com/raspberrypi/libcamera/blob/main/src/libcamera/control_ids_core.yaml>
