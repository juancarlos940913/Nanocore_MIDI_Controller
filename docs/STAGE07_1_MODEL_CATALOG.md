# Stage 07.1 — Dynamic AMP/CAB Model Catalog Sync

## Why

Stage 07 physically validated AMP/CAB selection by direct slot ID. Custom
ToneCommand imports replace the contents and name of the existing slot, so
the factory name is no longer authoritative. The preset footer (0x41)
still supplies the model ID; the catalog supplies the real stored name.

## Read-only wire protocol

Reference: [independent ToneCommand captures and hardware research](https://github.com/lucanenni/livtra-nanocore-editor/blob/main/docs/MIDI_MAPPING_NOTES.md)
(section on profile/IR catalogs). The USB catalog record format is reported
as verified on NanoCore hardware. Its BLE envelope and behavior on this
controller's firmware 1.2 still require physical validation.

* AMP: opcode 0x36, one-byte slot 0..29.
* CAB/IR: opcode 0x56, one-byte slot 0..29.
* Replies: 30-byte decoded record. Byte 0 slot; byte 1 present flag;
  byte 2 last-written flag; bytes 3..13 metadata; bytes 14..29
  ASCII name padded with NUL, maximum 16 characters.
* Accept status-0 direct replies or 0x10/0x11 segmented replies. The
  catalog has independent bounded reassembly, separate from 0x63/0x41.
  Validate sequence, opcode, requested slot, length, status and ASCII
  before accepting a name.
* The AMP profile space includes FX2 slots 30..37, but this stage only
  reads AMP/CAB slots 0..29. No profile/IR upload, download or erase.

## Controller behavior

* One cache in RAM per BLE connection. A new link epoch invalidates every
  previous catalog entry, preventing stale names on reconnect.
* Wait for initial preset recall and a quiet interval, then read 30 AMP
  and 30 CAB entries in the main loop, one outstanding catalog request
  at a time. Minimum 150 ms between completed requests. New input pauses
  subsequent catalog requests for at least 950 ms; recall, mapper, tuner,
  and name editing take priority. An idle block editor can still receive
  names; encoder/footswitch activity pauses subsequent catalog requests.
* 900 ms reply timeout, one retry on timeouts/transport failures, and
  skip the rest of an opcode's group after three consecutive failed
  records. No continuous polling and no retry forever.
* The MODEL row uses the real cached name, preserving the raw model ID
  and common AMP/CAB parameter definitions. An unread entry displays
  the factory fallback followed by "?"; a confirmed empty slot displays
  SLOT nn EMPTY. A valid response for the currently displayed model
  refreshes the UI.
* No changes to NVS layout, SAVE, normal effect editing, 0x41 model
  identity, or 0x6D model selection. No Synth Mode implementation yet.

## Physical acceptance on NanoCore firmware 1.2

Use an existing test preset, with known custom AMP and IR slot names.

1. Boot and connect. Monitor should show CATALOG|START, 30 AMP and 30 CAB
   name records and CATALOG|DONE|read=60/60|empty=...|unread=0.
   If any timeout/rejection/partial sweep occurs, capture the full log;
   do not treat the factory fallback as a verified name.
2. Check at least one custom AMP and CAB, plus factory slots. Verify
   names and IDs match the NanoCore physical display. Test a 16-character
   name if available.
3. Select models with ENC2 in the editor and compare both screens.
   Verify Gain, Low Cut, High Cut and Level still update correctly.
4. During a fresh sweep, change presets and toggle/edit blocks. Confirm
   correct 0x63/0x41 recall and stable BLE; the sweep resumes when idle.
5. Disconnect/reconnect after importing a different name via ToneCommand.
   Confirm the new catalog replaces the previous name.
6. Confirm SAVE/recall of a disposable test preset is unchanged. Do not
   erase NVS or overwrite any valuable profile or IR.
7. Only after successful physical verification close Stage 07 and
   reconcile the existing Stage 06/07 divergence before merging PR #5.
   Then begin Stage 08 Synth Mode.

## Stage 07.1A — AMP diagnostics

First physical test on firmware 1.2: 30/30 CAB records, 0/30 AMP records;
all six attempted 0x36 requests timed out. Recall 0x63/0x41 remained stable.
This revision sends one read-only 0x30 storage-info probe before 0x36,
waits 3 s before starting AMP catalog reads, and uses 2500 ms AMP reply
windows instead of 900 ms. CAB keeps the verified 900 ms timeout and
unchanged parser. Logs AMP_TX, AMP_RX, AMP_DECODE, AMP_INFO_TX and
AMP_INFO_RX (when received) expose the actual wire frames and response
headers. No claim that 0x30 is required until tested; the probe and
increased timeout are experimental. No writing of profiles or IRs.

## Stage 07.1B — accept the physically captured 31-byte AMP record

The Stage 07.1A serial log confirms 0x30 replies with a 20-byte payload
and 0x36 replies with status 0x11, body length 35 and total record
length 31 (4-byte segment header + 31-byte record). All six observed
AMP replies were rejected by the previous 30-byte length gate, causing
misleading timeouts. Independent 7-bit decoding of the actual capture
produced slot 0 BogXTC1, slot 1 BogXTC2, and slot 2 ENGP1 twice each.
The first 30 bytes have the known record layout: name at bytes 14..29.
Byte 30 was 0x01 in all six samples; its meaning is not yet known.
Accept AMP records of 30 or 31 bytes, CAB records of exactly 30 bytes,
and reassemble against each validated declared total in a 31-byte buffer.
Keep the name limited to 16 bytes and never interpret the extra byte
as part of the name. Keep the 0x30 probe, bounded retries and AMP trace
until full catalog acceptance is confirmed. No changes to model writes,
CAB layout, NVS, SAVE or the normal editor.
