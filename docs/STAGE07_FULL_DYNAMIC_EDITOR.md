# Stage 07 — Full Dynamic Editor

## Goal

Convert all physically mapped normal-mode NanoCore blocks into the same live
editor architecture already validated on FX1.

Every block editor now follows:

```
ON/OFF
TYPE or MODEL
<live parameters for the current type only>
```

The UI is driven by the accepted opcode-0x63 live snapshot. Parameter writes use
ToneCommand's opcode-0x6D per-parameter mechanism. Continuous resolution remains
0.01 normalized per encoder detent, as physically approved during FX1 testing.

## Type/model authority

- FX1, FX2, DELAY, MOD, REVERB, EQ:
  - current type = live 0x63 raw variant
  - type change = 0x6D block-type SysEx with default parameter vector
- AMP/CAB:
  - saved model = authoritative 0x41 preset footer
  - controller-originated live model change = local authoritative mirror
  - model change = 0x6D scalar fields 0x06/0x07
  - 0x63 variant remains ignored for model identity

## Live parameter authority

All normal blocks use the normalized float arrays from 0x63.

- FX1: type-specific, 2..7 params
- FX2: type-specific, 2..7 params
- AMP: 5 common params
- CAB: 3 common params
- DELAY: 4..9 params on firmware 1.2
- MOD: 2..5 params
- REVERB: 5 params
- EQ: 4/7/9 params

The editor refuses to start a parameter move from a guessed zero. If no live
value exists yet, the field remains unknown until a 0x63 refresh arrives.

## Dynamic selector orders

### FX1
Gate -> Auto Gate -> Gate+Comp -> Compressor -> Peak Comp -> Parallel Comp ->
Env Wah -> Wah -> Motion Wah -> Classic Wah -> Retro Wah -> Bass Wah

### FX2
Raw ids 0..20 in physical order.

### DELAY
BBD -> Digital -> Duck -> Ice -> Reverse -> LoFi -> Smear -> Mod Delay

### MOD
Chorus -> Chorus II -> Phaser -> Phaser II -> Flanger -> Jet Flanger ->
Vibrato -> Velvet Vibrato -> Tremolo

### REVERB
Room -> Plate -> Hall -> Concert -> Spring -> Shimmer -> Cloud

### EQ
3-band -> 6-band -> 8-band

### AMP/CAB
Factory model order 0..29, already verified through 0x41 saved-preset reads.

## Discrete controls

Discrete parameters are quantized instead of moving continuously:
- Ping Pong: OFF / ON
- Wave (where the protocol exposes a four-state waveform):
  Sine / Triangle / Square / Saw

Fast encoder turns preserve the actual detent count.

## Recommended physical validation

Do not sweep every value. For each block:

### FX2
1. Change TYPE across at least one old and one new type.
2. Verify a normal parameter (e.g. Drive Gain).
3. Verify one inherited FX1 type (e.g. Peak Comp).

### AMP
1. Open editor on a saved preset and confirm MODEL name.
2. Change MODEL one step and confirm NanoCore screen follows.
3. Move Gain and one EQ control.

### CAB
1. Confirm MODEL name.
2. Change MODEL one step.
3. Move Low Cut and Level.

### DELAY
1. Change BBD -> Duck -> Mod Delay.
2. Verify Time.
3. Verify Duck Ping Pong toggles OFF/ON.
4. Verify Duck Mix is the final parameter.

### MOD
1. Change Chorus -> Chorus II -> Phaser II -> Jet Flanger.
2. Verify one normal parameter.
3. Verify a SysEx-only parameter such as Phaser II Depth or Jet Flanger Phase.

### REVERB
1. Confirm physical TYPE order Spring -> Shimmer -> Cloud.
2. Verify one Decay.
3. Verify Shimmer Pitch.

### EQ
1. Change 3-band -> 6-band -> 8-band.
2. Verify one band gain.
3. Verify Level.

If those representative tests pass, Stage 07 normal mode is considered
end-to-end complete and Stage 08 can focus exclusively on Synth Mode.
