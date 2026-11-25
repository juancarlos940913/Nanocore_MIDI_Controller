# Stage 09.2 — Unsaved edit navigation

Base: d2565a1, user confirmed battery percent glyph and normal operation.
User log: cached names loaded, 60/60 fresh names, preset 05 scene B
restored after boot (628 ms), later scene B 680 ms, scene A 1310 ms.

- Clean navigation: immediate normal action once transport is ready.
- Dirty navigation (preset +/- outside STOMP, scene +/- in SCENE,
  MODE short outside tap capture): first press shows persistent
  "UNSAVED - PRESS [action] AGAIN TO DISCARD". Same action confirms;
  no short double-click deadline. Any other input cancels only,
  except SAVE chord cancels and enters normal save editor.
- Confirmation reloads persisted local preset and recalls physical
  preset before applying selected scene if needed. No 0x46/save
  on discard. Destination with no snapshot starts from base, not
  discarded edits. A preset change loads target instead.
- Track mode-only edits using a RAM snapshot of the eight live block
  headers/float vectors and Synth voice. When returning to original
  mode, clear dirty only if all match within 1e-5 and no other
  edit occurred. Other edits invalidate snapshot, remain dirty.
  Model IDs are not present in 0x63, so no independent model verification.
  Unknown/incomplete snapshots remain conservative (dirty).
- Disconnect/preset recall cancels pending confirmation. Transport
  readiness guard prevents discarding mid-transaction. A failed recall
  uses existing error path; do not assume pedal state restored.
- No new flash schema, partition, hardware or protocol changes.
- Test PRESET/STOMP/SCENE G->S->G without edits; star should clear if
  state unchanged; G->S plus edit then return must retain star.
  First navigation press does not move, second same press discards,
  ENC2 cancels, SAVE still works. Test preset +/-, scene +/- including
  a blank destination, and each mode transition with deliberate edits.
  Confirm old stored scenes/presets unchanged after reboot.
