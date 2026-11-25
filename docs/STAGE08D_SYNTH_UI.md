# Stage 08C completion / 08D — read-only Synth UI

Base 4a52c83. Physical log 1158 lines, 13 complete FX2 reads,
no timeout or reboot, main stack high-water mark 6052 bytes.
FX2 order raw/count: 8/3,12/2,13/2,14/7,15/6,16/5,17/5,9/4,
10/5,11/4,18/5,19/5,20/5. All match existing Stage 07 definitions.
Excluded drive IDs 0..7. No renumbering. Previous retained raw 0
can still appear in an existing preset: display RAW 0 UNAVAILABLE,
do not auto-replace. 62 mapped voices/types total across seven blocks.
Mode value 1 throughout this log. Earlier A/B OFF/ON capture supports
the flag; repeat physical OFF/ON/OFF for acceptance. REV order remains
provisional as captured.

Stage 08D preview: authoritative accepted 0x63 snapshots drive
mode and voice state. SYNTH, FX1, FX2, DELAY, MOD, REV, EQ visible.
SYNTH is a separate UI selection, not a controller_block_t wire ID,
so eight-element arrays and NVS structure are unchanged. Legacy normal
editor remains functional. Synth preview is deliberately read-only;
no Synth/FX edits, save, scene overlay or stomp commands in preview.
Tuner and preset navigation in PRESET mode remain available.
No continuous read polling; ENC2 click on performance screen requests
one 0x63 when transport is not recalling or reading catalog. Editor
entry also refreshes. Physical voice/type changes may require explicit
refresh (push voice/type not yet decoded). No mode write yet.
Unknown tail retains previous mode as lock and clears voice known flag.

Test after build/flash: start physical Normal; confirm 8 blocks and
normal editor unchanged. Turn Synth on physically, ENC2 on performance,
confirm 7 blocks and SYNTH|UI|mode=1. Rotate ENC1 through all 7,
enter SYNTH using ENC1 click and inspect ON/OFF, TYPE and parameter
rows; rotate ENC1 to select parameters, ENC2 click to return.
Change voice physically, ENC2 refresh then enter voice again.
Compare display labels and values, particularly Analog Wave and
Attack/Release, whose unit and curve metadata are provisional.
Turn Synth off physically, ENC2 refresh, confirm 8 blocks.
Retest normal edit, no SAVE on disposable preset until verified.
Use existing mapper if needed. No full erase. Verify existing local
sdkconfig still has stack settings from 08B.1.
