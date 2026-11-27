# Stage 10H — Performance preset navigation and scene A start

User reports bottom preset +/- in SCENE leaves unknown '?' blocks;
also requests every song/mode entry starts at scene A, not remembered C.
No log of the failing navigation supplied, so the exact trigger is not proven.
Code inspection: PC recall clears mirrors; old poll did not match a unique
request sequence, accepted incomplete LIVE, and timeout abandoned cleared UI.
Base gain/metadata path delayed first render. Other input could enter during
pending PC. Fix these failure paths rather than fabricate device state.

- New preset, physical slot adoption, mode entry to SCENE and boot set cursor
  A (index0). No saved scene sound/name or NVS schema changes. Explicit
  scene navigation and same-slot discard retain requested destination.
  BLE reconnect restores current cursor in RAM (not a new song).
- Preset +/- in SCENE keeps controller mode and restores physical preset,
  matched full 0x63 with target slot, then existing base gain/mode and
  saved name/model sequence before applying target A. No extra PC in
  healthy path; one retry on recall timeout. Ignore stale sequence and
  partial LIVE. Render accepted physical blocks before metadata.
- Pending PC blocks edits and other navigation until ready. Persistent
  timeout sets fault, ENC2 retries current preset; +/- can select another.
  Stale name/footer ignored during pending recall. Disconnect clears
  pending transaction state; existing reconnect flow restores.
- Scene A missing: existing unsaved/legacy behavior; confirmed base stays
  visible. No fabricated scene or auto-save. Full snapshots unchanged.
- No edits to STOMP bottom switch mapping, transport retries, level draft,
  AMP redundancy, scene parameter verification, partition or storage.
- Test P64 C -> P01 A -> P64 A, both directions, saved and unsaved A,
  PRESET/STOMP -> SCENE A, dirty two-press discard, restart, Guitar/Synth.
  Collect Recall sync, RECALL|LIVE_OK, SCENE|START_A, RECALL|SCENE_START,
  SCENE|READY and timeout diagnostics. Compile and hardware pending.
