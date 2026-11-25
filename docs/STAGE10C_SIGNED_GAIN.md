# Stage 10C — signed input packing, master capture

Base 8e51a7d, physically booted. Logs 20261125-205232 and 205831.
Physical input panel notifications use 7-bit PACK of signed 8-bit raw value:
-1: second mask 02, bytes 01 7f; -5: 02 01 7b; -20: 02 01 6c.
The old input write omitted the high-bit mask and sent 127 for -1,
which the device clamped to +20. Old push decoder also ignored the mask;
old reply decoder rejected unpacked negative bytes. Decode full push and
reply as signed 8-bit, accept only -20..20. Use existing pack7 to encode
field04 and gain byte. Expected candidate -1 wire group 02 04 7f.
Remote negative write is not yet physically validated. Existing matched
sequence readback and mismatch lockout retained. Test with amplifier
disconnected: from 0, one ENC2 step to -1; require matching reply before
further adjustment. No automatic volume or gain restoration.

Master test: OctRiff slot23 starts 85, preset volume adjusted to 49.
Subsequent 0x63 polls keep 49 and global reads keep 0 dB, no further
pushes after volume adjustment. Master not identified in current fields.
Add full 0x63 raw snapshot logging while LEVELS diagnostic is enabled
only; compare captures at fixed preset, no guitar or effect edits, master
0/25/50. No guessed write. Preset volume stays read-only.
No partition, scene schema, CMake or hardware changes.
