# Stages 01–03: prueba BLE-MIDI con NANOCORE

## Requisitos

- ESP32-S3 N16R8 de la PCB existente.
- ESP-IDF 5.3–5.x.
- NANOCORE con firmware 1.04+.
- Para la primera prueba, configurar el **MIDI Channel del NANOCORE en 0 (Omni)**.
- Abrir primero la app oficial del NANOCORE si el dispositivo todavía no ha realizado su primer emparejamiento y `Nanocore Midi` permanece oculto.

## Preparación

```powershell
git clone https://github.com/juancarlos940913/Nanocore_MIDI_Controller.git
cd Nanocore_MIDI_Controller
idf.py set-target esp32s3
```

El Component Manager descargará automáticamente las dependencias de Espressif durante la configuración/build.

---

## Stage 01 — BLE Scan

```powershell
git checkout stage-01-ble-scan
idf.py fullclean
idf.py build
idf.py -p COMx flash monitor
```

### Criterio de éxito

En el monitor debe aparecer un anuncio similar a:

```text
BLE addr=xx:xx:xx:xx:xx:xx RSSI=-.. name="Nanocore Midi" MIDI_UUID=YES
```

Puede aparecer por nombre aunque el UUID no esté incluido en el advertising; por eso Stage 02 acepta cualquiera de los dos criterios.

Stage 01 **no conecta**.

---

## Stage 02 — Connect + GATT

```powershell
git checkout stage-02-nanocore-connect
idf.py fullclean
idf.py build
idf.py -p COMx flash monitor
```

### Criterio de éxito

Secuencia esperada:

```text
NANOCORE candidate...
Requesting connection...
CONNECTED: conn_handle=...
GATT discovery complete
BLE-MIDI characteristic found: handle=0x....
Subscribed to BLE-MIDI notifications
```

El Stage 02 intenta iniciar seguridad/bonding porque BLE-MIDI puede usar enlace cifrado.

---

## Stage 03 — Program Change

```powershell
git checkout stage-03-midi-program-change
idf.py fullclean
idf.py build
idf.py -p COMx flash monitor
```

### Controles

- **BTN_PROGRAM / GPIO18:** preset anterior.
- **BTN_STOP / GPIO21:** preset siguiente.
- **BTN_FUNCTION / GPIO47:** reenvía el preset actual.

La prueba utiliza Program Change MIDI en canal 1. Si el NANOCORE está en **MIDI Channel 0 = Omni**, lo aceptará independientemente del canal enviado.

El firmware limita la navegación a **0–63**, correspondientes a los 64 slots del NANOCORE.

### Resultado esperado

Al presionar BTN_STOP:

```text
Request PC -> 1
Program Change -> ch=1 program=1
```

y el NANOCORE debe cambiar al preset correspondiente.

## Notas

BLE-MIDI empaqueta los mensajes MIDI con timestamp de 13 bits. El componente `nanocore_midi` genera ese paquete y escribe a la característica remota usando **GATT Write Without Response**.

Si Stage 01 detecta el NANOCORE pero Stage 02 no conecta, conservar el log completo. Si Stage 02 conecta y descubre GATT pero Stage 03 no cambia presets, conservar especialmente las líneas de seguridad, discovery y cualquier `BLE MIDI write failed`.
