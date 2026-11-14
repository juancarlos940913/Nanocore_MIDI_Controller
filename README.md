# Nanocore MIDI Controller

Controlador MIDI dedicado para **NANOCORE Mini Multi-Effect Processor** basado en ESP32-S3 N16R8 y ESP-IDF.

## Objetivo

Construir de forma incremental un controlador para directo con:

- BLE-MIDI hacia NANOCORE.
- Cambio de presets mediante Program Change.
- Control de bloques y parámetros mediante CC.
- Snapshots/escenas.
- Motor de tempo, BPM, subdivisiones y Tap Tempo.
- Canciones y setlists.
- Almacenamiento en SD/NVS.
- TFT + 2 encoders + 3 botones.
- USB-MIDI como transporte alternativo futuro.

## Hardware base

- ESP32-S3 N16R8
- TFT SPI
- SD SPI
- 2 encoders con pulsador
- BTN_PROGRAM GPIO18
- BTN_STOP GPIO21
- BTN_FUNCTION GPIO47

## Roadmap inicial

- **Stage 01:** BLE scan / identificación de NANOCORE.
- **Stage 02:** conexión + descubrimiento GATT BLE-MIDI.
- **Stage 03:** Program Change por BLE-MIDI.
- Stage 04: Control Change.
- Stage 05: UI y edición.
- Stage 06: Tempo Engine / Tap Tempo.
- Stage 07: Snapshots.
- Stage 08: Songs.
- Stage 09: Setlists.

El desarrollo se mantiene por stages para que cada etapa pueda compilarse y probarse de forma aislada.
