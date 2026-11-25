# Stage 10A — Verified physical slot and LEVELS discovery

Base 2ff3cf1. User reports output knob can cause false preset selection
and all blocks unknown. Current 0x01 push was treated as a slot before
live confirmation. External livtra-nanocore-editor docs/MIDI_MAPPING_NOTES.md
document same-shape wireless push and unreliable PC slot pushes; the
specific volume push is not yet captured on user's firmware.

Treat every valid-shape 0x01 as hint, including values beyond 63.
Coalesce and issue fresh sequence-matched 0x63. Do not update preset,
NVS, dirty flag or block state until actual slot is confirmed. Require
valid chain and decoded Synth tail. ENC2 manual refresh uses same path.
Fix recall timeout clearing in-flight flag. Existing PC and scene
verification retained. No automatic volume write.

LEVELS virtual selection after EQ, no new protocol chain block.
ENC1 scroll, press to open. Input gain uses documented global 0x65 read,
0x66 field04 signed7 dB (external editor range -20..20), separate
short response decoder (status0 or single BLE status11 total7 offset0).
Wait matched readback before allowing subsequent manual 1 dB changes.
No input adjustment until recognized reply. Global input is not a preset
or scene parameter. USB/BT volumes are NOT analog output. 0x63 header
byte p[2] is logged/displayed RAW only; its meaning is not established.
Analog output LOCKED, do not guess write opcode. Capture with amplifier
muted/disconnected, enter LEVELS, choose output row, ENC1 click read;
move physical input and output separately, capture after each. Log
LEVEL|PUSH, LEVEL|GLOBAL_RAW, LEVEL|LIVE_HEADER, SLOT|HINT/UNCHANGED.
Do not automatically increase gain, save or restore levels on recall.
Source archive re-nanocore docs/protocol.txt API27 global 0x65;
external editor GlobalSettingsPanel.tsx and midi/sysex.ts for 0x66.
Physical master knob could be analog-only; verify before enabling output.
