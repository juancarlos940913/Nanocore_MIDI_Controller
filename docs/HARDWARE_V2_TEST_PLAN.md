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

- Bottom FS1 = preset up.
- Bottom FS2 = preset down.
- Bottom FS3 short = mode cycle.
- Bottom FS3 long = Tap Capture.
- Top-left/top-right = Scene -/+ in SCENE.
- STOMP: Top-left = FX2.
- STOMP: Top-right = Delay.
- STOMP: Bottom FS1 = Mod.
- STOMP: Bottom FS2 = Reverb.

## 5. LEDs

- PRESET = one green pulse.
- STOMP = two green pulses.
- SCENE = three green pulses.
- Blue LED follows BPM.
- Long Bottom FS3 arms Tap Capture; blue LED responds immediately to taps.

## 6. Chords

Press both switches nearly together and keep them down briefly.

- Bottom FS3 + Top-right = tuner toggle.
- Physical Bottom FS1 + Top-left = SAVE/name editor.
- Top-left + Top-right = toggle Stage 06 Live Mapper.

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


## Stage 06 — Live Mapper

Purpose: characterize the exact live data exposed by the user's NanoCore firmware
instead of assuming that the legacy MIDI guide is complete.

### Toggle
Hold both TOP footswitches together briefly.

Expected:
- `NC_MAP: Live Mapper ON`
- screen notice `LIVE MAPPER ON - EDIT NANOCORE`

Repeat the chord to disable.

### Polling behavior
While enabled and BLE-MIDI is ready:
- opcode 0x63 is requested approximately every 800 ms;
- only one mapper request may be in flight;
- Program Change recall synchronization has priority over mapper polling;
- identical snapshots are suppressed.

### Serial format
When live state changes, expect:

```
NC_MAP: MAP|BEGIN|slot=...
NC_MAP: MAP|slot=...|block=FX1|on=1|variant=...|count=...|p0=...|p1=...
...
NC_MAP: MAP|slot=...|block=EQ|...
NC_MAP: MAP|slot=...|chain=...
NC_MAP: MAP|slot=...|block=SYNTH|on=...|variant=...|count=...|...
NC_MAP: MAP|END|slot=...
```

`variant` is intentionally RAW device data. Do not assume AMP/CAB's live
variant byte is their model number. Parameter values are raw normalized float32
values from the device.

### Recommended mapping procedure
Use a preset you are willing to edit live and DO NOT press SAVE during
characterization unless explicitly testing persistence.

For each block:
1. enable Live Mapper;
2. select one effect/type on the physical NanoCore;
3. wait for a MAP snapshot;
4. record the physical type name and its reported `variant/count`;
5. move one parameter at a time to clearly different positions;
6. capture the MAP snapshots;
7. move to the next type.

Send the resulting serial log for conversion into the firmware-specific
metadata table (type names, parameter names, ranges, units and scaling curves).
