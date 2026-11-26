# Stage 10F — redundant scene model selection
Base daee188 (10E.1), user physically reports PRESET/STOMP good, saved guitar
scenes intermittently retain preceding amplifier despite correct FX changes.
No new physical serial log supplied for this symptom.

Existing 0x6D field 06 AMP / 07 CAB are absolute live selections. BLE
write-no-response ESP_OK is host acceptance, not proof of actual DSP selection.
0x63 verifies on/off and parameter vectors but not live AMP/CAB identities.
Old optimization skipped a model using a local TX target mirror; this can
propagate a missed selection. No independent model confirmation is claimed.

Changes confined to main/app_main.c:
- Each saved Guitar scene explicitly sends target AMP three times with fresh
  SysEx sequences, 20 ms between transmissions. CAB receives same triple when
  changed/unknown or after failed/cross-mode recall; unchanged CAB skipped.
- Require >=2 locally accepted sends of three; disconnect or fewer accepted
  sends fails scene. No blocking ACK wait or extra reads. Retain 40 ms
  settle after final send, then replay ALL model parameters and on/off
  as existing code does for model selection. Never repeat model after params.
- Rename scene model TX cache flag to reflect host TX only. Keep current
  0x63 effects/volume and 0x65/66 gain verification, NVS and manual PRESET/
  STOMP unchanged. No preset save, PC, or flash writes introduced.
- Log MODELS_SAVED (local target), MODELS_REQUESTED, MODEL_TX per attempt,
  MODEL_TX_RESULT, MODELS_TARGET (tx_only), existing SCENE LATENCY.
- Timing tradeoff: +40 ms requested spacing per model already being selected;
  same-AMP transitions now also pay three sends, settle and full AMP parameter/
  on-off replay (5 parameters plus on/off, existing 28 ms spacing). Actual BLE/
  DSP timing must be measured. No claim of zero added latency or guaranteed
  DSP execution; redundant sends mitigate missed commands, not proof.

Acceptance: existing slot1 A Intro clean, B Coro drive+OCD, C Solo clean+
Screamer+delay. Alternate A/B/C/B/A/C repeatedly at low master (initially
amp disconnected), check physical AMP model label and sound, all effects,
gain and volume. Collect log with SCENE|MODELS_REQUESTED and MODEL_TX_RESULT,
SCENE|LATENCY. Do not overwrite existing scenes unless saved model IDs
are wrong. No storage changes. If still intermittent, investigate live
active-model metadata (0x36/0x56) separately rather than guess.
