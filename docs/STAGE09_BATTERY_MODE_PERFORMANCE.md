# Stage 09 — all-mode sound engine, battery, faster scene/catalog

Base physically validated: 4ae5d31 (08E.1). No change to partition
layout, existing preset/scene binary formats or GPIO wiring except GPIO1 ADC.

## 09A Battery
1S Li-ion only. BAT+ (not 5V boost output) -> 100k 1% -> GPIO1 -> 100k
1% -> GND; 100nF GPIO1 to GND. Use protected cell and common system
ground; do not bypass low-side protection. Disconnect divider with
power when ESP32 off (prevent back-power). Separate charger status
not connected; no charging icon inferred from voltage. Calibrated
ADC1 oneshot, 16 samples per 5 s, IIR, estimated SoC, 82x32 TFT partial
update. Unknown if missing/invalid; no automatic shutdown.

## 09B Sound modes and scene latency
All performance screens ENC2 left guitar/right synth, click refresh.
Normal existing mode cycle and stomps, Synth voice/block edits allowed
in all modes. SAVE in PRESET/STOMP uses existing physical 0x46 ACK and
writes per-preset mode preference in scene_data namespace nc_scene1.
Recall base mode explicitly if mismatch; remember old base mode in RAM
before scene overlay. Legacy scenes and snapshots retained.
No unverified independent Synth voice bypass write.
Scene sends only changed blocks, 1–2 differing same-type params by
scalar command, same-mode trusted model mirror avoids repeated model TX.
Cross-mode always explicitly restores both models in guitar. Independent
model readback not established. First settle reduced 160 -> 50 ms,
retry 160 ms; one final render instead of multiple blocking TFT redraws.
Keep sequence/slot matched mode verification and final 0x63; retain
safe BLE 20-byte packets and MIDI_ACTION_GAP_MS. Ignore unused 0x6D ACK
in common segmented reassembly to avoid interference. SCENE|LATENCY
logs before final redraw; compare prior APPLY-to-READY metric.
No guaranteed audio-gap or scene timing; physical acceptance pending.

## 09C Catalog / re-nanocore evidence
User-supplied re-nanocore-audit(1).zip commit 1e9299f618e8a59ab6f5079f8cff193fdea47662.
docs/protocol.txt API4 0x30 AMP summary/0x36 slot-info fallback;
API9 0x50 IR summary/0x56 fallback; populated fixture incomplete.
docs/physical-investigation.txt and evidence/physical-runtime-summary.json
report physical FW1.10 (user-reported) 38 AMP slots including non-AMP
asset slots, 30 IR, active indices; raw complete captures not included.
Prior physical FW1.2 AMP summary was 20 bytes, no names. Do not
interpret summary as a proven names bulk command. 0x35/55 are name
writes (firmware/protocol/amp_commands.c and ir_commands.c), not reads.
0x41 USB max 200 in source; current BLE cap 59 retained.
Implemented 0x30/0x50 read-only summary probe + logging, same individual
0x36/0x56 validated reads, reduce inter-record idle 150 -> 35 ms,
preserve AMP priming 3s and failure retry limits. Persistent cache only
on complete sweep, provisional names marked ~ until revalidated on
connection, unknown factory fallback ?. Cache not per-device keyed;
never treat ~ as fresh names. Initial complete refresh still needed.
No speculative bulk opcode. User's requested single bulk list not
established by supplied archive. Bounded saved model footer window
fallback uses reported total, then offset180; unknown still visible.
Queue scene movement while a catalog record is in flight.

## Acceptance
Build on IDF 5.5.5. Do not erase flash. Test normal and synth mode in
all three controller modes; save disposable base presets and recall
after reboot, switch to SCENE then back, verify existing A/B/C/D.
Record SCENE|APPLY/LATENCY/READY and any VERIFY_MISMATCH.
On first boot with battery connected compare BAT|LEVEL mv to DMM;
without divider expect unknown (floating input can be unpredictable).
Capture SUMMARY_RX for both types and CATALOG DONE, second boot
CACHE_LOADED and names lose ~ as verified. No flash changes to pedal
except explicit SAVE command.

Base SAVE now requests a fresh sequence-matched 0x63 preflight (slot and
mode) before sending physical rename/save. If mode changes during the
preflight, reject without writing. Scene movement during verified recall
or a catalog request is queued; no synchronous notice redraw mid-recall.
If saved base model footer read times out after accepted preset LIVE,
allow a full stored scene to apply using its own IDs instead of failing.
