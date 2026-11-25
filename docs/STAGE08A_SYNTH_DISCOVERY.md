# Stage 08A — Synth Mode read-only discovery

Base: Stage 07.1B commit 8382ac4. User confirmed five custom AMP names
and successful 60/60 catalog read; normal dynamic editor is preserved.
This branch does not merge Stage 06/07 or change their refs.

## Goal

Physical visible order described by user: SYNTH, FX1, FX2, DELAY, MOD,
REV, EQ. AMP/CAB hidden in Synth Mode, not deleted from normal storage.
FX2 begins with Pitch and excludes most drive/distortion types. DELAY
has 8 types according to user (last normal type removed; count/order must
be physically reconciled because Stage 07 normal has 8 mapped variants).
MOD 9, REV 7, EQ same as normal. Verify exact physical ordering and wire
IDs rather than compacting the existing normal IDs.

External candidate reference:
https://github.com/lucanenni/livtra-nanocore-editor/blob/main/docs/MIDI_MAPPING_NOTES.md
Synth section: 0x6D field 0x0A mode toggle; block ID 10 for type/param;
0x63 candidate header 15 bytes after EQ. Voice IDs 0..9:
Analog Synth, Strings, Pad, Choir, Brass, Organ, Flute, Lead, Bass, Pluck.
Expected counts 6,5,3,5,5,5,5,3,5,3. These are hypotheses on our device.

## Capture procedure

1. Use PRESET performance screen, not SCENE, tuner or tap mode. Connect,
   wait until initial catalog DONE. Use a disposable preset, do not SAVE.
2. On controller press top-left + top-right together (existing mapper
   chord). SYNMAP|ON. In this screen, ENC2 click sends a single 0x63 read;
   other controller inputs are locked except the chord to exit. No polling,
   normal-state updates, or preset writes from this capture path.
3. Capture normal mode once. On physical NanoCore enable Synth Mode in
   Settings, same preset, capture once again. Wait SYNMAP|END each time.
4. On physical device select each of ten voices, record physical name
   and parameter labels/values if possible, click ENC2 once per voice.
   Capture one changed parameter (e.g. Attack at min/mid/max) if desired.
5. In Synth Mode sweep FX2, DELAY, MOD, REV, EQ; FX1 if order differs.
   Each physical type selected once, capture each with ENC2. No need to
   sweep all parameters already verified in normal mode yet.
6. Return physical device to normal mode and same preset before leaving
   discovery using the same chord. Disconnect aborts discovery.

Serial records: SYNMAP|BEGIN/HEX/BLOCK/PARAM/TAIL/CANDIDATE/VOICE_PARAM/END.
The candidate is informational, not an authoritative enable flag. Raw
hex is always retained even if the normal 8-block walk fails.
No mode toggle, voice write, editor UI or SAVE changes in this stage.
Next: decode on/off and IDs, mode-aware editor specs, synth-only
parameters, visibility and encoder navigation, then validated writes,
physical push and SAVE tests.
