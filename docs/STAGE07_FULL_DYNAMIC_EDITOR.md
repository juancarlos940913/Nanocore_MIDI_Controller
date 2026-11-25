# Stage 07 — Full Dynamic Normal-Mode Editor

## Goal

Convert the Stage 06 hardware-verified normal-mode map into one generic live
editor for all eight NanoCore blocks before starting Synth Mode.

Stage 07 deliberately does **not** implement Synth Mode.

## Editor model

Every normal block uses the same UI layout:

1. ON/OFF
2. TYPE, or MODEL for AMP/CAB
3. only the parameters that exist for the selected type/model
4. DELAY additionally appends TEMPO BPM and NOTE utilities

Parameter values come from the accepted opcode-0x63 live profile.

One encoder detent remains 0.01 normalized travel for ordinary numeric
parameters, matching the FX1 resolution already approved on hardware.

Special controls:
- enum: one option per detent, wrapping around
- boolean: positive direction = ON, negative direction = OFF

All block parameters are written with the native opcode-0x6D parameter edit,
not legacy parameter CCs.

## Type/model writes

FX1 / FX2 / DELAY / MOD / REVERB / EQ:
- ToneCommand-compatible generic set-block-type 0x6D
- target type's normalized default parameter vector is included

AMP:
- scalar 0x6D field 0x06, direct model id 0..29

CAB:
- scalar 0x6D field 0x07, direct model id 0..29

Legacy TYPE CC assignments remain only as compact controller/NVS mirrors.

## Authoritative state

### Effects
0x63:
- block on/off
- raw type id
- parameter count
- normalized float parameters

### AMP/CAB
0x63:
- on/off
- common live parameters

0x41 saved footer:
- AMP model id
- CAB model id

After preset recall:
0x63 -> preset name 0x41 -> AMP/CAB model footer 0x41

This automatically populates MODEL before the user edits AMP/CAB.

## Verified selector orders

### FX1
```
Gate
Auto Gate
Gate + Compressor
Compressor
Peak Comp
Parallel Comp
Env Wah
Wah
Motion Wah
Classic Wah
Retro Wah
Bass Wah
```

raw:
```
0,1,2,3,7,8,4,5,6,9,10,11
```

### FX2
Raw/physical order 0..20, hardware-verified count catalog.

### DELAY
```
BBD
Digital
Duck
Ice
Reverse
LoFi
Smear
Mod Delay
```

raw 0..7.

Firmware 1.2 Ping Pong is part of every type. Duck's order is:
```
Time, Feedback, Sensitivity, Depth, Attack, Release, Filter, Ping Pong, Mix
```

### MOD
```
Chorus
Chorus II
Phaser
Phaser II
Flanger
Jet Flanger
Vibrato
Velvet Vibrato
Tremolo
```

raw:
```
0,11,1,12,2,17,4,10,3
```

### REVERB
```
Room
Plate
Hall
Concert
Spring
Shimmer
Cloud
```

raw:
```
0,1,2,3,6,7,5
```

### EQ
```
3-band EQ
6-band EQ
8-band EQ
```

raw 0,1,2.

### AMP/CAB
30 factory models each, direct selector ids 0..29.

## Real-unit scaling

The editor database retains Stage 06 physical measurements wherever available.

Examples:
- FX1/FX2 Gate Threshold: -100..0 dB
- FX1/FX2 Gate Release: 5..100 ms
- Peak Comp Attack/Release: physical exponential ranges from Stage 06
- Parallel Comp Attack/Release: Stage 06 approximate cubic curves
- CAB Low/High Cut: exponential frequency scaling
- EQ bands: -15..+15 dB
- EQ Level: -12..+12 dB
- AMP Bass/Mid/Treble: -10..+10 dB

## Delay tempo utilities

The old controller functionality is preserved after the real Delay parameters:
- TEMPO BPM: 30..300
- NOTE: controller delay subdivision

Changing either continues to update Delay Time through the existing tap-tempo
path.

## Test matrix before Synth

Use a normal saved preset. No NVS erase is required.

### FX1 sanity
Gate:
- TYPE displays Gate
- Threshold starts at live value
- Release changes pedal

### FX2
Test one drive plus one copied Stage-06 effect:
- Scream Gain
- Peak Comp or Wah

### AMP
- MODEL matches saved preset
- change one model from controller
- Gain and one EQ control move the NanoCore

### CAB
- MODEL matches saved preset
- change one model
- Low Cut / High Cut / Level move the NanoCore

### DELAY
Use Duck:
- nine physical parameters appear in mapped order
- Ping Pong toggles
- Mix moves independently
- BPM and NOTE still work

### MOD
Use Tremolo:
- Wave moves one enum choice per detent
- Rate/Depth/Mix update

Also verify one sparse-id type such as Jet Flanger.

### REVERB
Cycle:
Concert -> Spring -> Shimmer -> Cloud

Verify the physical pedal follows that exact selector order.

### EQ
Use 8-band:
- eight bands + Level visible
- Level writes even though it has no public legacy CC

## Save

Preset SAVE remains NanoCore-authoritative:
- live edits are made on the physical NanoCore
- rename + NanoCore SAVE persists them
- controller NVS mirror follows only after device ACK

Synth Mode is the next stage only after this matrix passes.


## Stage 07.1 follow-up: real AMP/CAB catalog

See [STAGE07_1_MODEL_CATALOG.md](STAGE07_1_MODEL_CATALOG.md) for the new read-only model-name synchronization and physical validation plan. Stage 07's type/model IDs, parameter ranges, SAVE and NVS semantics remain unchanged.
