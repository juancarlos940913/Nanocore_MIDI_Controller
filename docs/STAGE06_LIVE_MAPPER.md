# Stage 06 — NanoCore Live Mapper

## Goal

Build a firmware-specific specification from the physical NanoCore itself.

Stage 05 proved that opcode 0x63 reflects the device's live state. Stage 06
extends that decode from ON/OFF only to:

- active preset slot;
- ON/OFF for all normal blocks;
- raw effect/model variant byte;
- live parameter count;
- normalized float32 values for every reported parameter;
- effect-chain order when the footer is recognizable;
The normal controller UI remains an 8-block controller during this stage.
Dedicated Synth discovery/editing is deferred until a real physical Synth-mode
capture is available; Stage 06.2 deliberately does not infer Synth from normal
0x63 trailing bytes.

## Why normalized values first

The device sends parameter values normalized to approximately 0.0..1.0.
The physical screen provides the human meaning: milliseconds, Hz, dB,
semitones, percentages, enum labels, etc.

Capturing both gives enough evidence to derive:
- parameter order;
- min/max;
- units;
- linear/nonlinear curves;
- type-specific parameters;
- firmware-only controls not represented in the old MIDI CC guide.

## Activating mapper mode

Hold **Top-left + Top-right** together.

The chord is consumed, so FX2/Delay stomp actions do not leak through.

Serial:
```
NC_MAP: Live Mapper ON
```

Enabling mapper queues one baseline capture. After that the mapper is
**capture-on-demand**:

1. change a type/parameter on the physical NanoCore;
2. press **ENC2** on the controller;
3. one opcode 0x63 snapshot is requested and compared against the previous one.

This replaced the original ~800 ms continuous poller after a long FX1 mapping
session exhausted the BLE write-no-response buffer pool and produced
`BLE_INIT: Malloc failed`.

## Example

```
NC_MAP: MAP|BEGIN|slot=4|hash=1234ABCD
NC_MAP: MAP|slot=4|block=FX1|on=1|variant=3|count=6|p0=0.500000|p1=...
NC_MAP: MAP|slot=4|block=FX2|on=0|variant=0|count=3|p0=...
NC_MAP: MAP|slot=4|block=AMP|on=1|variant=0|count=5|p0=...
NC_MAP: MAP|slot=4|block=CAB|on=1|variant=0|count=3|p0=...
NC_MAP: MAP|slot=4|block=DELAY|on=1|variant=1|count=...
NC_MAP: MAP|slot=4|block=MOD|on=0|variant=0|count=...
NC_MAP: MAP|slot=4|block=REVERB|on=1|variant=...
NC_MAP: MAP|slot=4|block=EQ|on=0|variant=...|count=...
NC_MAP: MAP|slot=4|chain=0,1,2,3,4,5,6,7
NC_MAP: MAP|END|slot=4
```

## Stage 06 Stomp layout

Physical controls:

- Top-left: FX2 ON/OFF
- Top-right: Delay ON/OFF
- Bottom #1: Mod ON/OFF
- Bottom #2: Reverb ON/OFF
- Bottom #3: mode / tap behavior unchanged

Outside STOMP:
- Bottom #1: next preset
- Bottom #2: previous preset

## Safety / persistence

Live Mapper only READS opcode 0x63.
It does not save, rename or overwrite a preset.

Continuous background polling is intentionally disabled. ENC2 requests one
capture, preventing diagnostic sessions from flooding BLE over time.

Do not press the SAVE chord during mapping unless the goal is specifically to
test persistence. Changing preset recalls the saved version and therefore
provides an easy way to discard live characterization edits.


## FX1 mapping status

The complete physical selector has been identified.

Physical selector order -> raw 0x63 variant:

```
1 Gate          -> 0
2 Auto Gate     -> 1
3 NG+Comp       -> 2
4 Compressor    -> 3
5 Peak Comp     -> 7
6 Parallel Comp -> 8
7 Env Wah       -> 4
8 Wah           -> 5
9 Motion Wah    -> 6
10 Classic Wah  -> 9
11 Retro Wah    -> 10
12 Bass Wah     -> 11
```

Ranges and curve metadata are implemented in
`components/nanocore_spec/nanocore_spec.c`.

Bass Wah's physical ranges are recorded from the NanoCore screen and its raw
id/count were confirmed in the earlier type sweep. The long all-parameter
session reached Retro Wah (variant 10) before BLE allocation failure, so Bass
Wah did not receive a second live-parameter confirmation in that session.
