# FX1 physical map — NanoCore

Source priority:
1. physical NanoCore screen values recorded during Stage 06;
2. live opcode 0x63 normalized floats;
3. independent ToneCommand captures only to corroborate names/raw ids.

Physical selector order is NOT raw protocol order.

| Selector | Raw ID | Type | Count |
|---:|---:|---|---:|
| 1 | 0 | Gate | 2 |
| 2 | 1 | Auto Gate | 2 |
| 3 | 2 | Gate + Compressor | 7 |
| 4 | 3 | Compressor | 6 |
| 5 | 7 | Peak Comp | 5 |
| 6 | 8 | Parallel Comp | 5 |
| 7 | 4 | Env Wah | 4 |
| 8 | 5 | Wah | 5 |
| 9 | 6 | Motion Wah | 4 |
| 10 | 9 | Classic Wah | 5 |
| 11 | 10 | Retro Wah | 5 |
| 12 | 11 | Bass Wah | 5 |

## Parameters

### Gate — raw 0
- Threshold: -100..0 dB, linear.
- Release: 5..100 ms, linear.

### Auto Gate — raw 1
- Sensitivity: -6..+6 dB, linear.
- Release: 20..300 ms, linear.

### Gate + Compressor — raw 2
- Gate: -100..0 dB, linear.
- Threshold: -60..0 dB, linear.
- Ratio: 1.0:1..20.0:1, linear.
- Knee: 0..24 dB, linear.
- Attack: 0..200 ms, linear.
- Release: 0..1000 ms, linear.
- Makeup: -24..+24 dB, linear.

### Compressor — raw 3
Same compressor section as Gate + Compressor, without Gate.

### Peak Comp — raw 7
- Threshold: -60..0 dB, linear.
- Ratio: 1.0:1..20.0:1, linear.
- Attack: 0.1..200 ms, exponential.
- Release: 1..2000 ms, exponential.
- Makeup: -24..+24 dB, linear.

### Parallel Comp — raw 8
- Threshold: -60..0 dB, linear.
- Attack: 0.01..250 ms, approximately cubic from live normalized data.
- Release: 10..2500 ms, approximately cubic from live normalized data.
- Mix: 0..200, linear.
- Level: -24..+24 dB, linear.

### Env Wah — raw 4
- Freq: 10..20000 Hz, exponential.
- Env: 0..20000 Hz, linear.
- Q: 0.1..20, linear.
- Mix: 0..100, linear.

### Wah — raw 5
- Freq: 163..3500 Hz, exponential.
- Sweep: 0..100, linear.
- Rate: 0..8 Hz, linear.
- Shape: 0..100, linear.
- Mix: 0..100, linear.

### Motion Wah — raw 6
- Rate: 0..15 Hz, linear.
- Voice: 0..100, linear.
- Sweep: 0..100, linear.
- Mix: 0..100, linear.

### Classic Wah — raw 9
- Position: 0..100, linear.
- Low: 20..500 Hz, linear.
- High: 500..5000 Hz, linear.
- Mix: 0..100, linear.
- Level: -60..+6 dB, linear.

### Retro Wah — raw 10
Same parameter ranges/layout as Classic Wah.

### Bass Wah — raw 11
- Position: 0..100.
- Q Trim: 0..100.
- Volume Trim: 0..100.
- Mix: 0..100.
- Level: -60..+6 dB.

Bass Wah raw ID/count were confirmed in the earlier type sweep and the physical
ranges were recorded from the screen. The long all-parameter sweep stopped
during Retro Wah because BLE diagnostic polling exhausted buffers, so Bass Wah
did not receive a second full 0x63 parameter sweep in that session.

## Mapper transport note

The original automatic 800 ms mapper poller was intentionally removed after a
long mapping run produced BLE allocation failures. Stage 06.2 uses manual
capture:
- Top-left + Top-right: toggle mapper.
- One baseline capture occurs on enable.
- Change the physical NanoCore.
- ENC2 click: request exactly one new 0x63 capture.
