# Stage 06.5 — Assisted Mapper

## Purpose

Stage 06.5 changes the mapping workflow from manual reverse engineering to
candidate validation.

Candidate catalogs are preloaded from captured ToneCommand/NanoCore protocol
work. The user's physical NanoCore remains the source of truth.

For each capture, the controller compares:
- target block;
- raw type id from live opcode 0x63;
- candidate type name;
- expected parameter count;
- actual parameter count;
- raw normalized parameter values.

The mapper reports:
- `MATCH`: candidate raw id + parameter count agree with the device;
- `MISMATCH`: candidate exists but live parameter count differs;
- `UNKNOWN`: raw id is not in the candidate database;
- `PROFILE_ONLY`: AMP/CAB live profile is visible, but the current 0x63
  raw variant byte is not treated as an authoritative model id.

## Controls

From the normal PERFORMANCE screen:

1. Rotate ENC1 until the block you want to map is selected.
2. Hold TOP LEFT + TOP RIGHT to enable Assisted Mapper.
3. The selected block becomes the mapper target and stays fixed for that
   session.
4. One baseline 0x63 capture is queued automatically.
5. Change one TYPE on the physical NanoCore.
6. Wait briefly for the NanoCore screen to settle.
7. Press ENC2 once to capture the new live profile.
8. Repeat steps 5–7 for every type.
9. Hold TOP LEFT + TOP RIGHT again to stop the session and print the summary.

Do not continuously press ENC2. One press per settled physical type is enough.

## Example

```
NC_MAP: ASSIST|START|block=EQ|candidate_types=3|mode=manual_capture

NC_MAP: ASSIST|CAPTURE|block=EQ|raw=0|candidate=3-band EQ|expected_count=4|got_count=4|status=MATCH
NC_MAP: ASSIST|CANDIDATE_PARAMS|block=EQ|raw=0|Low,Mid,High,Level
NC_MAP: ASSIST|RAW|block=EQ|variant=0|count=4|p0=...|p1=...|p2=...|p3=...

NC_MAP: ASSIST|CAPTURE|block=EQ|raw=1|candidate=6-band EQ|expected_count=7|got_count=7|status=MATCH

NC_MAP: ASSIST|SUMMARY|block=EQ|unique=3|match=3|mismatch=0|unknown=0|candidate_types=3
```

## Candidate type counts

### EQ
- 3-band EQ
- 6-band EQ
- 8-band EQ

### Reverb
- Room
- Plate
- Hall
- Concert
- Shimmer
- Cloud
- Spring

### Delay
- BBD
- Digital
- Duck
- Ice
- Reverse
- LoFi
- Smear
- Mod Delay

### MOD
Candidate ids currently include:
- Chorus
- Phaser
- Flanger
- Tremolo
- Vibrato
- Velvet Vibrato
- Chorus II
- Phaser II
- Jet Flanger

Raw ids are intentionally validated rather than assumed sequential.

### FX2
Candidate ids include the original drives/boosts, Pitch/Wah family, and the
newer Gate/Compressor/Wah types through raw id 20.

Many FX2 types reuse effects already physically mapped in FX1. If raw id and
parameter count match, their parameter model can later inherit the verified FX1
metadata instead of being re-measured.

## Recommended mapping order

1. EQ
2. REVERB
3. DELAY
4. MOD
5. FX2
6. AMP common profile
7. CAB common profile

This order starts with small catalogs and ends with the largest block.

## What to do on MATCH

Do not move every parameter.

Record only the serial capture. A MATCH means the candidate type and live
parameter count agree.

For a few representative types per block, visually compare the candidate
parameter names printed by the mapper with the physical NanoCore screen.

If the names/order agree, no min/mid/max sweep is needed unless later editor
testing reveals a bad scale.

## What to do on MISMATCH

For that type only:
1. photograph or write the physical parameter names in order;
2. record min / middle / max only for parameters whose range is uncertain;
3. send the serial log plus the physical values.

## What to do on UNKNOWN

Record:
- physical type name;
- physical parameter names;
- one ENC2 capture;
- if practical, min/mid/max for any obviously new parameter.

## AMP and CAB

Do NOT manually sweep all ~30 models yet.

Current live opcode 0x63 exposes their common parameter profiles, but its raw
variant byte is not treated as an authoritative model id in Stage 06.5.

For AMP:
- make one Assisted Mapper capture;
- note the physical model name;
- optionally change to one second model and capture again.

For CAB:
- do the same.

The model lists are already known from the official-app protocol work; a
dedicated authoritative model-read/write path will be added after the effect
blocks are validated.

## Safety

Assisted Mapper is read-only.
It never saves or renames the preset.
Use a preset you are comfortable changing temporarily and do not press SAVE
during characterization unless persistence itself is being tested.


## Stage 06.6 physical-validation results

### EQ — VERIFIED
Physical firmware-1.2 session:
- raw 0 = 3-band EQ, count 4
- raw 1 = 6-band EQ, count 7
- raw 2 = 8-band EQ, count 9

Session summary: 3 unique / 3 match / 0 mismatch / 0 unknown.

### MOD — VERIFIED
Physical session confirms all 9 candidate ids/counts:
- 0 Chorus
- 11 Chorus II
- 1 Phaser
- 12 Phaser II
- 2 Flanger
- 17 Jet Flanger
- 4 Vibrato
- 10 Velvet Vibrato
- 3 Tremolo

Session summary: 9 unique / 9 match.

### FX2 — VERIFIED TYPE/COUNT CATALOG
Physical session confirms all 21 candidate raw ids 0..20 with their expected
parameter counts. The newer Gate/Compressor/Wah-family defaults also match the
already verified FX1 normalized defaults.

Session summary: 21 unique / 21 match.

### REVERB — firmware 1.2 correction
Physical session confirms:
- 0 Room
- 1 Plate
- 2 Hall
- 3 Concert
- 7 Shimmer
- 5 Cloud
- 6 Spring

Important: Shimmer live raw id is 7 on firmware 1.2, not legacy id/CC value 4.

### DELAY — firmware 1.2 Ping Pong extension
Official firmware 1.2 adds a Ping Pong option to every delay type. Physical
0x63 captures confirm one additional final parameter on the current firmware.

Current live counts:
- BBD: 5
- Digital: 4
- Duck: 9
- Ice: 5
- Reverse: 4
- LoFi: 6
- Smear: 6
- Mod Delay: 6

The final parameter is treated as Ping Pong. Duck's 9-parameter profile required
raising the Stage 06 live parser from its old per-block limit of 8 to the full
NC_PROTO_MAX_PARAMS=9.


## Stage 06.7 selector-order confirmation

Physical selector order on firmware 1.2:

### REVERB
```
Room -> Plate -> Hall -> Concert -> Spring -> Shimmer -> Cloud
raw: 0 -> 1 -> 2 -> 3 -> 6 -> 7 -> 5
```

### DELAY
```
BBD -> Digital -> Duck -> Ice -> Reverse -> LoFi -> Smear -> Mod Delay
raw: 0 -> 1 -> 2 -> 3 -> 4 -> 5 -> 6 -> 7
```

The second DELAY sweep produced 8/8 MATCH.

Duck's 9 live values also clarify the firmware-1.2 parameter order:
```
Time, Feedback, Sensitivity, Depth, Attack, Release, Filter, Ping Pong, Mix
```
The observed capture had p7=0.0 and p8=0.4, consistent with Ping Pong OFF and
Mix=40%. Other delay types expose Ping Pong as the final parameter because
their legacy Mix index does not need to move.


## Stage 06.8 — authoritative AMP/CAB model read

Physical 0x63 captures confirmed the expected limitation:
- AMP live variant stays 0 while changing models, but exposes 5 live params.
- CAB live variant stays 0 while changing models, but exposes 3 live params.

Therefore AMP/CAB model identity is no longer inferred from 0x63.

The controller now reads the saved-preset 0x41 footer:

```
... <AMP_INDEX> 21 01 <CAB_INDEX> 24 01 ...
```

Both indices are direct 0..29 model ids.

Assisted Mapper behavior for AMP/CAB:
1. Select AMP or CAB in PERFORMANCE.
2. Enable mapper with TOP LEFT + TOP RIGHT.
3. Change the model on the physical NanoCore.
4. Press ENC2 once.
5. Serial prints:
   `MODEL|slot=N|AMP=<name>(id)|CAB=<name>(id)`
6. Repeat with one or two additional models only.

There is no need to sweep all 30 AMP and 30 CAB models if the direct indices
track the physical selections correctly.

Common live profiles remain:
- AMP: 5 params = Gain, Bass, Mid, Treble, Level
- CAB: 3 params = Low Cut, High Cut, Level
