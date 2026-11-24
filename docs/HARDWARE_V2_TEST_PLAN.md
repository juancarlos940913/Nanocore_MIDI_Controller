# Hardware V2 — Bring-up / validation plan

Run these tests in order. Do not debug multiple subsystems at once.

## 1. Boot / memory

Expected serial:
- Stage 05 banner
- NVS storage ready
- Hardware V2 GPIO initialized
- ST7796S ready 480x320 landscape

Confirm there is no PSRAM or partition error.

## 2. Display only

Confirm:
- full 480x320 landscape image
- black/dark background
- white text
- cyan selection border
- green/red/yellow/blue are visually correct
- no offset, clipping or mirrored text

If orientation/color is wrong, edit only:
`components/board/include/board_pins.h`

- BOARD_LCD_INVERT_COLOR
- BOARD_LCD_MIRROR_X
- BOARD_LCD_MIRROR_Y
- BOARD_LCD_SWAP_XY

## 3. Encoders

Performance screen:
- ENC1 left/right moves block highlight both directions.
- ENC1 click opens block editor.
- ENC2 click returns.

Block editor:
- ENC1 chooses parameter.
- ENC1 click enters value edit.
- ENC2 changes value.
- ENC2 click exits edit / returns.

## 4. Five footswitches

- Bottom FS1 = preset down.
- Bottom FS2 = preset up.
- Bottom FS3 short = mode cycle.
- Bottom FS3 long = Tap Capture.
- Top-left/top-right = Scene -/+ in SCENE.
- Top-left = selected block stomp in STOMP.
- Top-right = Delay stomp in STOMP.

## 5. LEDs

- PRESET = one green pulse.
- STOMP = two green pulses.
- SCENE = three green pulses.
- Blue LED follows BPM.
- Long Bottom FS3 arms Tap Capture; blue LED responds immediately to taps.

## 6. Chords

Press both switches nearly together and keep them down briefly.

- Bottom FS3 + Top-right = tuner toggle.
- Bottom FS1 + Top-left = SAVE/name editor.

No individual footswitch action should leak through a recognized chord.

## 7. Name editor

- ENC1 horizontal.
- ENC2 vertical.
- ENC1 click inserts.
- ENC2 click toggles lowercase/uppercase.
- Top-left/right move text cursor.
- Bottom FS1 deletes.
- Bottom FS2 cancels.
- SAVE chord confirms.

Power cycle and verify the saved name remains.

## 8. BLE-MIDI

Expected:
- Nanocore Midi discovered.
- BLE connects.
- MIDI characteristic resolves.
- current Program Change is restored.

## 9. Runtime persistence

Set:
- a non-zero preset
- SCENE mode
- a non-A scene

Power cycle controller. After reconnect it must restore:
- preset
- mode
- scene

## 10. Preset/scene editing persistence

Edit a CC in PRESET, SAVE, power cycle, verify it re-applies.

Edit an Amp ID or another CC in SCENE, SAVE, power cycle, verify the scene override re-applies.

## 11. Delay tempo

In DELAY editor:
- edit TEMPO BPM
- edit NOTE (1/2, 1/4, 1/8, 1/16, dotted 1/4, dotted 1/8)
- verify calculated delay changes

Then calibrate `tempo_engine_delay_ms_to_cc74()` against real NanoCore delay times. The current public MIDI guide does not define this conversion.


## Stage 05.1 validation — controller behavior

### Fast encoder
- Spin ENC1 rapidly through the name keyboard.
- Cursor must jump to the current physical position without continuing for seconds afterward.
- Slow one-detent movement must remain one logical step.
- If a single detent is skipped/doubled, tune only `BOARD_ENCODER_EDGES_PER_DETENT`.

### Device live-state sync
After BLE-MIDI becomes READY, expect serial messages from `NC_PROTO`:
- heartbeat/ping traffic
- reassembled opcode 0x63 live-state reply
- `LIVE slot=... FX1=... FX2=... AMP=... CAB=... DEL=... MOD=... REV=... EQ=...`

The eight block cards should then be:
- green outline = ON
- yellow outline = OFF
- gray = not yet known

Change a block physically on the NanoCore. The controller should update from the unsolicited block-state push.

### Device preset name
After recall, the controller requests the NanoCore's saved preset name. The performance screen should use the device name after the response arrives.

### Save to NanoCore
In PRESET or STOMP:
1. make a live edit
2. invoke SAVE
3. enter a name (NanoCore write limit: 8 chars)
4. confirm SAVE chord

Expected:
- rename SysEx
- opcode 0x46 save
- NanoCore ACK
- NVS mirror written only after the device ACK
- notice: PRESET SAVED TO NANOCORE

Recall another preset and return. The saved name/block state must already be present in the NanoCore without a second base-CC replay.

SCENE saves remain controller/NVS-only by design.

### Dirty preset guard
Make an edit and press Preset +/- without saving.
Expected on-screen notice:
`SAVE CHANGES BEFORE PRESET +/-`

### Delay calibration
CC74 is now calibrated linearly:
- CC0 = 0 ms
- CC127 = 2000 ms
- 300 BPM quarter = 200 ms target -> nearest 7-bit CC13
- 30 BPM quarter = 2000 ms -> CC127

### Block order
Performance layout and protocol block IDs:
`FX1, FX2, AMP, CAB, DELAY, MOD, REVERB, EQ`
