# Stage 08E — Independent guitar/synth scene snapshots

User validated Stage 08D physical mode switching (8/7 UI blocks) and
type/voice display. Scene mode now supports a separate sound mode per
scene and full live snapshot storage for 64 presets x 4 scenes.

## Controls (SCENE performance)
- B3 short: existing PRESET/STOMP/SCENE cycle.
- TL/TR: previous/next scene. B1/B2: preset +/-.
- Rotate ENC2 left: GUITAR; right: SYNTH. 0x6D field 0x0A; request
  0x63 after settle, sequence-match and verify mode before proceeding.
- ENC1 rotation selects visible block; ENC1 click enters editor, ENC1
  selects row, click toggles edit, ENC2 adjusts, ENC2 click backs out.
- SYNTH voice type and params use wire block ID 10, same 0x6D framing.
  Voice ON/OFF row is read-only because no independent bypass write
  beyond mode has been validated. Guitar/Synth mode set on performance.
- Existing SAVE chord -> scene name editor -> SAVE chord confirms.
  Fresh sequence-matched 0x63 snapshot, validated count/type/value,
  persisted locally. No 0x46 or physical preset overwrite for scene save.
- New/legacy scenes without snapshots inherit current live sound
  (legacy guitar CC overlays still apply). Save before switching scene.
- Saved scene switches mode first, then restores voice and visible
  effect types with exact normalized vectors, model IDs and params,
  bypass states; verify with fresh 0x63. Failure visible; ENC2 click
  retries a saved scene when failed. No periodic polling.
- After physical preset recall in SCENE wait for saved 0x41 model
  response before restoring scene. No reapply on ordinary live refresh.

## Storage
Original nvs (0x9000/0x10000), phy and app unchanged, legacy blob
layout/version unchanged. New scene_data NVS partition at
0xF80000 size 0x80000, carved from tail of reserved unused storage
SPIFFS (all 14 tracked C source files inspected; no filesystem mounting).
256 records with names, mode, voice and float32 visible profiles.
Do not erase flash/NVS. If user manually populated SPIFFS outside this
firmware, back up before flashing the updated partition table.
Partition init error leaves legacy storage intact; scene save fails
visibly rather than erasing. Names and snapshots atomic in one blob.

## Limitations/acceptance
External ToneCommand capture documents 0x0A mode, block 10 voice/type/
param. TX on the user's firmware 1.2 still needs physical validation.
Readback verifies mode, type, param counts/values (tolerance .002), on/off.
0x63 does NOT identify live AMP/CAB model IDs, so choose them from
controller, not physical panel, when authoring normal scenes; model
TX/cache not independent verification. Synth saves require a valid
voice ON; hidden AMP/CAB retained by device, not part of Synth snapshot.
Preset BPM/subdivision global as before, actual delay parameters stored
per scene; chain order remains device preset's chain. Synth base preset
editing/SAVE outside SCENE stays read-only. Scene snapshots do not claim
gapless switching. Verify timing on hardware; don't use live on stage
until tested. Firmware build/physical validation pending.

Mode confirmation retries up to two readbacks (no redundant mode writes);
verification retries one read. Normal scene recalls explicitly transmit
AMP/CAB model IDs, even if the local mirror matches, and restore their
parameters. Physical block-10 param pushes are accepted as candidates
through a separate event; wire behavior still requires validation.
Save rejection leaves editing available. Scene recall failure is not
reported READY; no persistent scene overwrite on failed readback.
