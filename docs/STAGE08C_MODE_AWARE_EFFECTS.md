# Stage 08C — mode-aware catalog, partial physical acceptance

Base c8d7645 (Stage 08B.1). User supplied 2451-line physical log.
31 complete manual captures and 31 MAIN_STACK_FREE reports of 6052 bytes.
No overflow, restart or timeout in supplied interval. Catalog DONE 60/60.
Seven normal LIVE reads after mapper exit, all mode=1, voice=0, count=6.
Mode byte is 1 in every manual capture; OFF/ON/OFF of physical Synth Mode
has not yet been repeated in this log. SYNMAP|OFF exits mapper, not Synth.

## Captured IDs and counts

- SYNTH: prior 10/10 (Stage 08A).
- FX1: prior 0/2,1/2,2/7,3/6,7/5,8/5,4/4,5/5,6/4; current
  9/5,10/5,11/5 complete the normal 12-type membership.
- DELAY: 0/5,1/4,2/9,3/5,4/4,5/6,6/6,7/6.
- MOD: 0/2,11/4,1/3,12/4,2/3,17/5,4/4,10/5,3/5.
  Chorus raw 0 baseline, subsequent eight explicitly changed.
- REV: 0/5,1/5,2/5,3/5,7/5,5/5,6/5 in capture order.
  Normal selector order is 0,1,2,3,6,7,5. The Synth sequence is
  provisional until user confirms consecutive physical order.
- EQ: 0/4,1/7,2/9. Last response 221 bytes, mode offset 193.
- FX2: all 31 snapshots raw 0/count 3, same normalized vector
  0.5/0.5/0.8. No capture of Pitch or later types. The mode flag
  alone does not prove whether this retained raw ID denotes a hidden
  normal drive or a mode-specific remap. Do not assume.

## Implementation

Add opt-in mode selector API for five shared blocks and hide AMP/CAB
from the Synth selector API, not from legacy storage. FX2 returns zero
selectable types in Synth until physical mapping is known. Normal mode
delegates exactly to Stage 07. Existing editor does not call these new
APIs yet. Voice lookup stays separate; no changes to wire IDs, transport,
UI, writes, NVS or SAVE. Parameter labels/ranges reused provisionally
based on same IDs/counts; this is not proof of identical physical scaling.

## Next physical test (can use installed c8d7645)

Use disposable preset, PRESET performance screen, wait CATALOG DONE.
Keep mapper on (TOP+TOP once), then click ENC2 after each physical change.
Do not use TOP+TOP between types. Verify physical Synth OFF/ON/OFF/ON
with same voice and record MODE_BYTE 0/1/0/1. Then select FX2 Pitch
physically and capture, next effect and capture; if ID stays 0, change
one visible parameter and capture, with its displayed label/value.
If Pitch and next return distinct IDs, sweep remaining FX2 types,
record physical names in order. Confirm REV physical order if needed.
Return to normal and original preset before exiting mapper.
No additional firmware flash needed for these remaining captures.
