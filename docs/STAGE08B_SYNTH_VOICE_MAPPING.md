# Stage 08B — Synth read decoder and voice specification

Physical evidence: user log captured on Stage 08A (17a61c0),
12 complete 0x63 reads, slot 1, sequence 84/93/98/102/106/109/112/115/118/121/124/127.
The eight normal block records are unchanged in this test. End of EQ=139,
chain footer 139..147, mode candidate byte=153 (EQ end+14), voice
header=154 (EQ end+15). Seq 84 and 93 differ only at byte 153: 0 -> 1,
while the voice enable remains 1. This explains the old false-positive
heuristic. Assumes the user's captures followed requested OFF/ON sequence;
repeat OFF/ON/OFF to confirm before using flag for UI/writes.
Voice header [on, raw id, count], float32 LE, ends exactly at payload end
in all 12 captures. Decoder validates chain, exact length, on/mode bits,
voice ID/count, and normalized finite values. Unknown layout stays UNKNOWN.
Mode and voice enable are independent. Normal UI/storage untouched.

Voice table (captured normalized vectors, not arbitrary factory defaults):

- 0 Analog Synth: 0.25, 0.08, 0.5, 0.25, 0.68, 1
- 1 Strings: 0.45, 0.72, 0.5, 0.55, 1
- 2 Pad: 1, 0.5, 1
- 3 Choir: 0.55, 0.7, 0.2, 0.6, 1
- 4 Brass: 0.24, 0.5, 0.6, 0.7, 1
- 5 Organ: 0.04, 0.25, 0.6, 0.85, 1
- 6 Flute: 0.31, 0.45, 0.35, 0.7, 1
- 7 Lead: 1, 0.5, 1
- 8 Bass: 0, 0.3, 0.7, 0.4, 1
- 9 Pluck: 1, 0.5, 1

Labels, ranges, waveform enum and quadratic Attack curve (2+398*t*t ms)
are from the external ToneCommand mapping:
https://github.com/lucanenni/livtra-nanocore-editor/blob/main/packages/protocol/src/data/blocks/synth.ts
Verify against screen before enabling edits; captured values alone do not
prove physical units or curves. Keep raw IDs 0..7 and NVS layout unchanged.
New Synth voice lookup is independent of the eight normal block table.

Next physical captures using the same Stage 08A mapper (TOP+TOP, ENC2).
Wait catalog DONE, use PRESET performance screen, same disposable preset.
1. Synth OFF, ON, OFF, ON (one read each, same voice and no other changes).
2. With Synth ON, sweep FX2 from Pitch through last type, record names
   in physical order, ENC2 each. Initial observed raw 0 remains in snapshot
   after enabling Synth; do not assume a visible type remaps automatically.
3. Sweep DELAY from first to last, record names and ENC2 each. Stage 07
   already has eight variants; verify exact Synth list.
4. Optional representative MOD/REV/EQ and FX1 reads to check counts.
5. Return NanoCore to normal mode and original preset before leaving mapper.

The discovery path remains read-only; no Synth mode toggle, write,
UI filtering, or persistent data changes in this commit.
