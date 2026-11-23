# Stage 05 — Hardware V2

## Target hardware

- ESP32-S3 N16R8 (16 MB flash / 8 MB Octal PSRAM)
- ST7796S 3.5" SPI TFT, native 320x480, used 480x320 landscape
- 2 rotary encoders with push
- 5 footswitches
- Blue Tap/BPM LED
- Green Mode LED

## Provisional pin map

All wiring is centralized in `components/board/include/board_pins.h`.

| Function | GPIO |
|---|---:|
| TFT SCK | 12 |
| TFT MOSI | 11 |
| TFT MISO | 13 |
| TFT CS | 10 |
| TFT DC | 9 |
| TFT RST | 8 |
| TFT BL | 7 |
| ENC1 A/B/SW | 4 / 5 / 6 |
| ENC2 A/B/SW | 15 / 16 / 17 |
| Bottom FS1 | 18 |
| Bottom FS2 | 21 |
| Bottom FS3 | 47 |
| Top-left FS | 40 |
| Top-right FS | 41 |
| Blue Tap LED | 42 |
| Green Mode LED | 39 |

GPIO19/20 remain free for native USB-JTAG. GPIO33-37 are not used because N16R8 Octal PSRAM can occupy them.

## Footswitch behavior

### Always

- Bottom FS1: previous preset.
- Bottom FS2: next preset.
- Bottom FS3 short: PRESET -> STOMP -> SCENE -> PRESET.
- Bottom FS3 long: enter/exit Tap Capture.
- Bottom FS3 + Top-right: tuner ON/OFF.
- Bottom FS1 + Top-left: SAVE / name editor.

### SCENE

- Top-left: previous scene.
- Top-right: next scene.

### STOMP

- Top-left: toggle currently highlighted block.
- Top-right: quick Delay toggle.

## Encoders

### Performance

- ENC1 rotate: highlight FX1 / FX2 / AMP / CAB / MOD / DELAY / REVERB / EQ.
- ENC1 click: enter selected block editor.
- ENC2 click: Back.

### Block editor

- ENC1 rotate: select parameter.
- ENC1 click: enter/leave value edit.
- ENC2 rotate while editing: change value and send MIDI immediately.
- ENC2 click: leave value edit / Back.

### Name editor

- ENC1 rotate: keyboard left/right.
- ENC2 rotate: keyboard row up/down.
- ENC1 click: insert selected character.
- ENC2 click: lower/UPPER case.
- Top-left / Top-right: text cursor left/right.
- Bottom FS1: delete.
- Bottom FS2: cancel.
- Bottom FS1 + Top-left: save name and persist preset/scene.

## LED behavior

- Green LED: 1 pulse = PRESET, 2 pulses = STOMP, 3 pulses = SCENE.
- Blue LED: flashes at the current quarter-note BPM.
- Long-press Bottom FS3 arms Tap Capture; subsequent short presses set BPM.

## Persistence

NVS stores:

- last selected preset
- last mode
- last scene
- preset names
- preset BPM/subdivision
- preset MIDI edits
- four scene names and scene MIDI overrides per preset

Unsaved edits are marked with `*` and preset switching is blocked until SAVE is used.

## Delay tempo

The public NanoCore guide defines Delay Time as CC74 but does not publish the milliseconds-to-CC transfer curve.

Stage 05 therefore isolates the conversion in:

`tempo_engine_delay_ms_to_cc74()`

The current mapping is intentionally provisional and can be calibrated later without changing UI, storage, or scene logic.
