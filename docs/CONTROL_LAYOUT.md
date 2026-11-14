# Proposed final control layout

The final pedal is designed around **modes**, not one permanent function per switch.

## Recommended physical layout

```text
        [ ENC 1 ]   [  320 x 240 TFT  ]   [ ENC 2 ]

             [ MODE / TUNER ]   [ TAP / TEMPO ]

        [ FS1 ]    [ FS2 ]    [ FS3 ]    [ FS4 ]
```

The four lower footswitches are contextual:

| Mode | FS1 | FS2 | FS3 | FS4 |
|---|---|---|---|---|
| STOMP | FX2 | MOD | DELAY | REVERB |
| SCENE | Scene A | Scene B | Scene C | Scene D |
| PRESET | Preset - | Preset + | Song - | Song + |

Utility switches:

- **MODE / TUNER**: short press changes PRESET → STOMP → SCENE. Long press opens/closes tuner.
- **TAP / TEMPO**: tap tempo. Long press can later open tempo/subdivision settings.

## Why these four stomp blocks?

NanoCore exposes eight block ON/OFF CCs: FX1, FX2, Amp, Cab, Mod, Delay, Reverb and EQ.

For live operation the most useful dedicated stomp targets are normally:

- FX2: Drive / Boost / Pitch / Wah
- Mod
- Delay
- Reverb

Amp, Cab and EQ are normally part of the base tone and are edited from the screen/encoders rather than repeatedly stomped. FX1 (Gate/Compressor) can be placed on a secondary stomp page if needed.

## Stage 04 temporary controls on the existing PCB

No new switches are required yet.

- Hold **BTN_PROGRAM + BTN_STOP** for ~0.7 s: cycle PRESET → STOMP → SCENE.
- BTN_FUNCTION short press: also cycles mode as a development shortcut.
- PRESET:
  - BTN_PROGRAM = previous preset.
  - BTN_STOP = next preset.
- STOMP:
  - BTN_PROGRAM = FX2 ON/OFF (CC21).
  - BTN_STOP = Delay ON/OFF (CC26).
- SCENE:
  - BTN_PROGRAM = previous demo scene.
  - BTN_STOP = next demo scene.

Demo scenes are deliberately deterministic and send full ON/OFF states:

| Scene | FX2 | Mod | Delay | Reverb |
|---|---:|---:|---:|---:|
| A Clean | OFF | OFF | OFF | ON |
| B Rhythm | ON | OFF | OFF | ON |
| C Lead | ON | OFF | ON | ON |
| D Ambient | OFF | ON | ON | ON |

These are only Stage 04 test scenes. Persistent user snapshots will be implemented later.


CI target: ESP32-S3 / ESP-IDF v5.5.6.
