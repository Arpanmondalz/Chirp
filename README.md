# Chirp

A DIY portable MP3 player built around the **Seeed XIAO ESP32-S3**.

Chirp reads MP3 files from a microSD card, indexes music into folders, plays tracks through an external I2S DAC and headphone amplifier, shows playback/battery information on a 128×128 SH1107 OLED, and includes a USB serial sync mode for transferring files to the SD card without removing it.


> **Known Issue:** Battery monitoring is not accurate and fluctuates quite a bit.

## Features

- MP3 playback from microSD
- Folder-based music playlists
- Previous / next track controls
- Play / pause
- Adjustable volume
- 128×128 SH1107 OLED UI
- Battery voltage and percentage estimate
- Low-battery playback lockout
- Screen sleep after inactivity
- USB serial SD-card sync mode
- Safe temporary-file handling during uploads
- PSRAM music index and shuffle buffer

## Hardware

### Core

- Seeed Studio XIAO ESP32-S3
- 1S 3.7 V LiPo battery
- microSD SPI breakout
- 128×128 SH1107 OLED
- UDA1334A I2S stereo DAC
- LM4863 headphone amplifier module
- 3 tactile buttons
- 10 kΩ resistors for the button ladder
- 10 kΩ + 10 kΩ resistors for the battery divider

### Audio chain

```text
XIAO ESP32-S3
      │
      │ I2S
      ▼
 UDA1334A DAC
      │
      │ L/R line output
      ▼
  LM4863 amp (can be any headphone amp)
      │
      ▼
 Headphone jack
```

The UDA1334A is used as the line-level DAC and the LM4863 provides the headphone amplification.

## Wiring

All pin numbers below are **GPIO numbers**. The corresponding XIAO D-numbers are shown in comments.

### XIAO ↔ UDA1334A

| UDA1334A | XIAO |
|---|---|
| VIN | 3V3 |
| GND | GND |
| BCLK | GPIO1 / D0 |
| WSEL / LRCK | GPIO2 / D1 |
| DIN | GPIO44 / D7 |
| Lout | LM4863 LIN |
| Rout | LM4863 RIN |
| AGND | LM4863 G |

The UDA1334A is operated in normal I2S mode using its onboard clock/PLL arrangement; no separate MCLK connection is used in this build.

### XIAO ↔ microSD

| microSD | XIAO |
|---|---|
| VCC | 3V3 |
| GND | GND |
| CS | GPIO43 / D6 |
| SCK | GPIO7 / D8 |
| MOSI | GPIO8 / D9 |
| MISO | GPIO9 / D10 |

The SD wiring deliberately uses the actual GPIO mapping defined in `config.h`.

### XIAO ↔ OLED

| OLED | XIAO |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO5 / D4 |
| SCL | GPIO6 / D5 |

### Buttons

The three buttons use a single ADC input with a resistor ladder.

```text
D3 / GPIO4
   │
   ├──────── LEFT/BTN level
   │
   └── resistor ladder ── MID/RIGHT levels
                         │
                        GND
```

This build uses a **physical 10 kΩ pull-up** and keeps the ESP32 internal pull-up disabled.

The currently calibrated raw 12-bit ADC levels are:

| State | Measured ADC |
|---|---:|
| Left | 0 |
| Middle | 1933 |
| Right | 2618 |
| No button | 4095 |

`config.h` defines midpoint thresholds between these levels.

### Battery monitor

A 10 kΩ / 10 kΩ divider is connected from battery positive to ground:

```text
BAT+
 │
10kΩ
 │
 ├──────── GPIO3 / D2 (ADC)
 │
10kΩ
 │
GND
```

The firmware multiplies the measured divider voltage by `BAT_DIVIDER` and applies the optional `BAT_CAL_GAIN` calibration factor.

### Battery and amplifier power

The LiPo is connected to the XIAO battery input:

```text
LiPo +
   └── XIAO BAT+

LiPo -
   └── XIAO GND
```

For the headphone amplifier:

```text
LM4863 V+  → battery positive / BAT+
LM4863 G   → common GND
```

The XIAO's onboard LiPo charging circuit can therefore be used when the battery is connected through the XIAO battery input.

### What each module does

| File | Purpose |
|---|---|
| `chirp.ino` | Main startup sequence and main loop |
| `config.h` | Pins, timing, limits, display, battery and audio settings |
| `player.*` | Audio task, playback commands, shuffle queue, ID3 title/artist handling |
| `library.*` | SD-card scan and PSRAM-backed folder/track index |
| `buttons.*` | ADC ladder classification, debounce, short/long press events |
| `battery.*` | ADC sampling, smoothing, voltage-to-percentage conversion |
| `ui.*` | OLED drawing, navigation and screen state |
| `sync.*` | USB serial SD-card transfer protocol |
| `chirp_bitmaps.h` | Logo and UI bitmap data |

## Software setup

### Arduino IDE

Open `chirp.ino` in Arduino IDE.

The source configuration expects a **Seeed XIAO ESP32-S3** with these board settings:

```text
PSRAM:          OPI PSRAM
USB CDC On Boot: Enabled
USB Mode:       USB-OTG (TinyUSB)
CPU Frequency:  240 MHz
Flash:          QIO 80 MHz
```

These values are documented in `config.h`.

### Libraries

Install:

- **ESP32-audioI2S**
- **U8g2**

The following are supplied by the Arduino ESP32 core and do not need separate installation:

- `Arduino`
- `SPI`
- `SD`
- `Wire`

The project uses an external I2S DAC rather than an onboard audio codec.

## SD-card layout

Music is organized into folders. For example:

```text
/
├── Rock/
│   ├── track01.mp3
│   ├── track02.mp3
│   └── track03.mp3
│
├── Jazz/
│   ├── album1/
│   │   ├── song01.mp3
│   │   └── song02.mp3
│   └── album2/
│       └── song01.mp3
│
└── Singles/
    └── song.mp3
```

The library scanner indexes `.mp3` files case-insensitively and ignores common filesystem metadata such as hidden entries and `System Volume Information`.

The virtual **All Songs** folder spans the complete indexed track list.

## Controls

The UI has four screens:

```text
0: Now Playing
1: Folders
2: Sync
3: Volume
```

### Now Playing

| Button | Short press | Long press |
|---|---|---|
| Left | Previous track | Previous screen |
| Middle | Play / Pause | Sleep |
| Right | Next track | Next screen |

### Folders

| Button | Short press | Long press |
|---|---|---|
| Left | Previous folder | Previous screen |
| Middle | Play selected folder | — |
| Right | Next folder | Next screen |

Selecting a folder creates a shuffled track queue and starts playback.

### Sync

Long-press the centre button while the Sync screen is selected to arm USB serial sync mode.

While sync is armed, normal playback/navigation is locked.

Long-press centre again to exit sync mode. The card is rescanned before normal playback resumes.

### Volume

| Button | Short press |
|---|---|
| Left | Volume down |
| Right | Volume up |

## Shuffle playback

Tracks belonging to the selected folder are copied into a shuffle buffer and shuffled using the Fisher–Yates algorithm.

The playback queue is reshuffled when it reaches the end.

The random-number generator is seeded at boot from the ESP32 random source in `chirp.ino`.

The shuffle buffer is allocated in PSRAM, allowing large libraries without consuming the smaller internal RAM budget.

## Battery monitoring

Battery voltage is sampled through the external 10 kΩ / 10 kΩ divider.

The firmware:

1. Takes multiple ADC samples.
2. Converts the divider reading back to battery voltage.
3. Applies `BAT_CAL_GAIN`.
4. Smooths the result using an exponential moving average.
5. Converts voltage to a percentage using the Li-ion OCV table in `battery.cpp`.

The displayed percentage is therefore an **estimate**, not a coulomb-counter measurement.

Playback is blocked below:

```cpp
#define BAT_LOCKOUT_PCT 10
```

## Battery calibration

Set:

```cpp
#define CHIRP_CALIBRATE 1
```

in `config.h`.

The calibration screen continuously prints:

```text
BTN raw=...
BAT ...V
```

Use a multimeter to measure the actual battery voltage and adjust:

```cpp
#define BAT_CAL_GAIN 1.0f
```

according to:

```text
BAT_CAL_GAIN = actual battery voltage / reported battery voltage
```

Set `CHIRP_CALIBRATE` back to `0` after calibration.

## USB sync mode

The firmware includes a simple line-based JSON command protocol over USB serial.

Supported commands are:

```text
hello
dirs
list
put
del
bye
```

The protocol is implemented in `sync.cpp`.

### Transfer safety

Uploads are written first to:

```text
/.chirptmp
```

Only after the transfer length and CRC32 match is the temporary file renamed to its requested destination.

An interrupted or failed upload therefore does not leave a partially written playable file at the final path.

Existing files are not overwritten by `put`.

The firmware also rejects unsafe paths and ignores filesystem metadata directories.

### Important

This repository contains the **device-side sync firmware**. A PC-side sync application/client is not included in the current source tree.

## Display

The OLED is a 128×128 SH1107 panel.

The UI uses partial display updates where possible to reduce I2C traffic. Long titles, artist names and selected folder names use a marquee/scrolling display.

The display is intentionally mounted without rotation:

```cpp
U8G2_R0
```

`config.h` also contains the display contrast and I2C bus-speed settings used by this build.

## Audio notes

The audio path is:

```text
MP3 on SD
   ↓
ESP32-audioI2S decoder
   ↓
ESP32-S3 I2S
   ↓
UDA1334A
   ↓
LM4863
   ↓
Headphones
```

The UDA1334A is a DAC/line-output device, so the LM4863 is used for headphone amplification.

With very sensitive in-ear headphones, an audible amplifier hiss may be present even with the DAC input disconnected and the LM4863 inputs tied to ground. This is a characteristic of the particular low-cost LM4863 module/headphone stage used in the prototype; higher-impedance over-ear headphones may not make the noise audible.

## Configuration

Most build tuning lives in `config.h`:

- GPIO assignments
- SD SPI speed
- OLED settings
- button ADC thresholds
- battery calibration
- battery smoothing
- low-battery cutoff
- volume limits
- library limits
- sync protocol buffer sizes

The current library limits are:

```cpp
#define LIB_MAX_TRACKS  4000
#define LIB_MAX_FOLDERS 128
#define LIB_ARENA_BYTES (256 * 1024)
#define LIB_MAX_DEPTH   6
```

## Troubleshooting

### SD card error

Check:

- SD VCC is connected to the tested 3.3 V rail
- common ground is present
- CS/SCK/MOSI/MISO match `config.h`
- the card contains a readable FAT filesystem

### No audio

Check:

- UDA1334A VIN/GND
- BCLK, WSEL and DIN
- DAC L/R/AGND wiring to the amplifier
- headphone amplifier power
- SD card playback itself

### Buttons behave incorrectly

Enable calibration mode:

```cpp
#define CHIRP_CALIBRATE 1
```

Then watch the raw ADC value while pressing each button.

Update the thresholds in `config.h` if the physical resistor network changes.

### Battery percentage looks wrong

Use calibration mode with a multimeter and adjust `BAT_CAL_GAIN`.

The percentage is based on a Li-ion voltage curve and will not behave like a precision fuel gauge.
