# Interactive Sign Post (tip-to-play cosplay sign)

A palm-sized sign that plays a random sound clip when tipped over. Firmware is
PlatformIO / Arduino for the Adafruit QT Py ESP32-S3.

## What it does

1. At boot it records the current orientation as "upright".
2. It reads the accelerometer ~50x/sec and computes the tilt angle from upright.
3. When tipped past ~57 deg it plays a random `.wav` from the microSD card.
4. It re-arms once returned near upright (hysteresis stops repeat triggering).

## Hardware

| Part | Product | Role |
|------|---------|------|
| QT Py ESP32-S3, 8 MB flash | 5426 | microcontroller |
| Audio BFF | 5769 | MAX98357 I2S amp + microSD slot |
| LiPo Charger BFF | 5397 | battery charging |
| ADXL343 (STEMMA QT) | 4097 | tip detection |
| Mini Oval Speaker 8 ohm 1 W | 4227 | sound |
| LiPo 1200 mAh | 258 | power |

## Wiring

The Audio BFF mounts **flush on the back** of the QT Py (it uses the most pins).
The Charger BFF is **hand-wired** so the two BFFs do not have to stack:

- Charger BFF `5V`  -> QT Py `5V`   (delivers battery power to the board)
- Charger BFF `GND` -> QT Py `GND`
- Charger BFF monitor tap -> QT Py `TX`  (battery voltage sense)
- LiPo JST -> Charger BFF

Keep the Charger BFF's `A2` monitor jumper **closed**. Because the monitor is
wired to `TX` instead of `A2`, it no longer clashes with the Audio BFF's I2S
word-select clock, and battery sensing still works.

Pin assignments (set in `src/main.cpp`):

| Function | Pin |
|----------|-----|
| I2S bit clock (BCLK) | A3 |
| I2S word select (LRC) | A2 |
| I2S data (DIN) | A1 |
| microSD chip select | A0 |
| microSD SPI | SCK / MOSI / MISO |
| Accelerometer (I2C) | STEMMA QT port = `Wire1` (SDA1/SCL1) |
| Battery voltage sense | TX |

## SD card prep

Format the microSD as **FAT32** and copy your clips to the root. Any file
ending in `.wav` is picked up; names do not matter.

Files must be **16-bit PCM WAV**. Mono or stereo both work; 22050 or 44100 Hz
are good choices. Convert anything with ffmpeg:

```bash
ffmpeg -i input.mp3 -ac 1 -ar 22050 -acodec pcm_s16le 01.wav
```

## Build

Three scripts, run in order. Each is idempotent and re-usable.

```bash
./scripts/setup_venv.sh     # create .venv, install PlatformIO
./scripts/install_deps.sh   # install the ESP32 toolchain + libraries
./scripts/build.sh          # compile
```

Flash it to the board and watch the logs:

```bash
./scripts/upload.sh         # build + flash over USB-C
./scripts/monitor.sh        # serial monitor at 115200 baud
```

If flashing fails to auto-reset, hold `BOOT`, tap `RESET`, release `BOOT` to
enter the bootloader, then run `upload.sh`. Press `RESET` after upload.

## Tuning

Edit the constants at the top of `src/main.cpp`:

- `AUDIO_GAIN` (0.0-1.0) loudness
- `TIP_TRIGGER` cosine threshold to fire (lower = must tip further)
- `TIP_REARM` cosine threshold to re-arm near upright

## Notes

- Using the 4 MB / 2 MB PSRAM board (5700) instead? Change `board` in
  `platformio.ini` to `adafruit_qtpy_esp32s3_n4r2`.
- Serial runs over native USB (`ARDUINO_USB_CDC_ON_BOOT=1`), which also frees
  the TX pad for the battery ADC reading.
