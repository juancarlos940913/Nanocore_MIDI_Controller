# Stage 10G — BLE backpressure, scene corrective pass, LEVELS drafts
Physical log 20261126-171609 (2675 lines), preset 64 (wire 63):
A Intro DEL=0 AMP0, B Coro DEL=0 AMP1, C Solo DEL=1 AMP0, CAB4,
volume28 gain0. 17 apply attempts, 14 READY, mean 1070.7 ms, range890..1320.
All model triple TX accepted 3/3, not independent model confirmation.
At 676813 BLE SysEx rc6 off18 on AMP Level, at708083 rc6 off36 on DELAY
type during C->B: aborted before CC26=0. At680723/681423 FX1 mismatch
after failed scene; previous code polled twice without corrective write.
No per-detent level writes in supplied log; UX issue described by user.
- NimBLE BLE_HS_ENOMEM (rc6): retry rejected ATT packet only, up to8
  retries nominal20ms each, original link, no added sleep on healthy TX.
  Applies to CC/PC/SysEx. Other errors unchanged. Does not prove DSP ACK.
- Scene DELAY OFF prioritized when needed; final on/off still follows type.
- Scene TX failure on live link refreshes 0x63 then one corrective pass,
  force model reassert if partial TX. Ordinary mismatch uses refreshed
  state to repair differences without repeating already sent model IDs.
  Final 0x63 required; no extra query in healthy path; one bounded repair.
  No change to gain verification or model identity limitation.
- LEVELS ENC1 starts draft, ENC2 rotation consumes full delta clamped
  [-20,20] or [0,100]. Orange provisional value, no MIDI until ENC1
  apply. One write with existing matched readback; no-op no write.
  ENC2 click cancels and exits. Confirmed mirrors separate. Block
  navigation while pending; concurrent observed physical changes cancel
  relevant draft. Existing SAVE chord persists confirmed values only.
- No partitions, NVS formats, preset/scene storage changes. Actual
  firmware build and physical validation pending. Test C->B repeated,
  compare DEL=0, scene repair and BLE backpressure logs. Test 100->0
  draft with no intermediate writes, one apply; test cancel and gain.
  Start amp disconnected/low master, avoid large live volume jumps.
