# iDX6011 Pro – LED-Protokoll: Reverse-Engineering-Ergebnisse

Reverse-engineered aus `leds-mcu.ko` (Kernel 6.12.30+, ELF64, nicht gestripped)  
Gerät: UGREEN NASync iDX6011 Pro  
Erstellt: 2026-05-18

---

## Zusammenfassung

Das Gerät nutzt einen **Holtek HT32F52231** Microcontroller als LED-Treiber, der über I²C
Bus 0, Adresse **0x3a** kommuniziert. Das Protokoll ist dem der DXP-Serie sehr ähnlich, aber
mit zwei entscheidenden Unterschieden:

| Eigenschaft | DXP-Serie | iDX6011 Pro |
|---|---|---|
| I²C-Schreibfunktion | `i2c_smbus_write_i2c_block_data` | `i2c_smbus_write_block_data` |
| i2cset-Modus | `i` (kein Count-Byte) | `s` (SMBus, Count-Byte wird automatisch vorangestellt) |
| Write-Register | `led_id` (doppelt: im Reg + buf[0]) | `led_id` (nur einmal als SMBus-Command) |
| Anzahl LEDs | je nach Modell bis 8 | **9 LEDs** (Power, 2× LAN, 6× Disk) |
| Lese-Antwort | ~12 Byte | 32 Byte (davon 11 Byte pro LED-Read genutzt) |

Der Grund, warum `i2cset ... i` bisher nicht funktionierte: der Treiber schickt einen
**Count-Byte** (Länge der Nutzdaten), den der MCU erwartet. `i` = I²C-Block-Write ohne
Count-Byte; `s` = SMBus-Block-Write **mit** Count-Byte.

---

## LED-Mapping

| Index | Name (sysfs) | Schreib-Register | Lese-Register |
|-------|-------------|-----------------|---------------|
| 0 | power | 0x00 | 0x81 |
| 1 | network_stat (LAN 1) | 0x01 | 0x82 |
| 2 | network_stat2 (LAN 2) | 0x02 | 0x83 |
| 3 | disk1 | 0x03 | 0x84 |
| 4 | disk2 | 0x04 | 0x85 |
| 5 | disk3 | 0x05 | 0x86 |
| 6 | disk4 | 0x06 | 0x87 |
| 7 | disk5 | 0x07 | 0x88 |
| 8 | disk6 | 0x08 | 0x89 |

**Herleitung aus dem Binary:**
- Lese-Register = `(byte_at_state+0xc0 - 0x7F) & 0xFF` = `0x81 + led_index`
- Schreib-Register = `byte_at_state+0xc0` = `led_index` (direkt)
- `leal -0x7f(%rax), %esi` in `i2c_iDX601x_read_led` bei Offset 0x28e

---

## Schreib-Frame-Format

```
i2c_smbus_write_block_data(client, reg=led_index, length=11, data[11])
```

```
Byte  0:  0xA0  (fester Header)
Byte  1:  0x01  (fester Header)
Byte  2:  0x00  (fester Header)
Byte  3:  0x00  (fester Header)
Byte  4:  cmd   (Befehl: 0x01=Helligkeit, 0x02=Farbe, 0x03=An/Aus, ...)
Byte  5:  p1    (Parameter 1)
Byte  6:  p2    (Parameter 2)
Byte  7:  p3    (Parameter 3)
Byte  8:  p4    (Parameter 4)
Byte  9:  ck_hi (Checksumme, High-Byte)
Byte 10:  ck_lo (Checksumme, Low-Byte)
```

**Checksumme:**
```
sum = 0xA0 + 0x01 + 0x00 + 0x00 + cmd + p1 + p2 + p3 + p4
    = 0xA1 + cmd + p1 + p2 + p3 + p4

ck_hi = (sum >> 8) & 0xFF
ck_lo = sum & 0xFF
```

Die Checksumme wird Big-Endian gespeichert (High-Byte zuerst).

Quelle: Disassembly von `i2c_iDX601x_write_led` bei Offset 0x3b0 mit
Relocation `R_X86_64_PLT32 i2c_smbus_write_block_data` bei Offset 0x043f.

---

## Lese-Frame-Format (unverändert wie DXP)

```
i2c_smbus_read_i2c_block_data(client, reg=0x81+led_index, length=11, buf[11])
```

```
buf[0]:  Status (0x00=aus, 0x01=an)
buf[1]:  Helligkeit (0–255)
buf[2]:  R (Rot)
buf[3]:  G (Grün)
buf[4]:  B (Blau)
buf[5]:  t_cycle High-Byte
buf[6]:  t_cycle Low-Byte
buf[7]:  t_on High-Byte
buf[8]:  t_on Low-Byte
buf[9]:  Checksumme High-Byte
buf[10]: Checksumme Low-Byte
```

Checksumme = Summe von buf[0..8], Big-Endian.

Verifiziert mit gemessenen Werten:
```
i2cget -y 0 0x3a 0x81 i
→ 0x01 0xb4 0xff 0xff 0xff 0x00 0x00 0x00 0x00 0x03 0xb2
  ↑    ↑    ↑    ↑    ↑                            ↑
  on  180  255  255  255                     0x01+0xB4+0xFF*3 = 0x03B2 ✓
```

---

## Befehle

| Befehl | cmd | p1 | p2 | p3 | p4 |
|--------|-----|----|----|----|-----|
| Helligkeit setzen | 0x01 | Helligkeit (0=aus, 255=max) | 0 | 0 | 0 |
| Farbe setzen (RGB) | 0x02 | R | G | B | 0 |
| Ein/Aus | 0x03 | 1=an, 0=aus | 0 | 0 | 0 |
| Blinken | 0x04 | t_cycle_hi | t_cycle_lo | t_on_hi | t_on_lo |
| Atmen | 0x05 | t_cycle_hi | t_cycle_lo | t_on_hi | t_on_lo |

**Hinweis zu cmd=0x01:** `brightness_work` aus `leds-mcu.ko` verwendet ausschließlich
`cmd=0x01` mit p1=Helligkeit (0=aus, 0xFF=voll an). Der Treiber schaltet über die
Helligkeit (0 = LED aus), nicht über cmd=0x03.

---

## Init-Sequenz (Lauflicht stoppen / Host-Übernahme)

Der MCU läuft nach dem Einschalten autonom (Lauflicht). Um ihn zu stoppen und die
Steuerung zu übernehmen, muss die `probe`-Init-Sequenz aus dem Treiber nachgebildet werden.

Aus `leds_ugreen_probe` Offset 0x1720–0x180d (5 Aufrufe an Register 0x00):

```bash
# Init-Sequenz – muss EINMALIG vor jeder LED-Steuerung gesendet werden
# Alle Befehle an Register 0x00 (Power LED index)

# Schritt 1: Modus-Reset / Normal-Modus (cmd=0x04, alle Params=0)
# sum = 0xA1 + 0x04 = 0xA5
i2cset -y 0 0x3a 0x00 0xa0 0x01 0x00 0x00 0x04 0x00 0x00 0x00 0x00 0x00 0xa5 s
sleep 0.05

# Schritt 2: Helligkeit auf 255 (cmd=0x01, p1=0xFF)
# sum = 0xA1 + 0x01 + 0xFF = 0x01A1
i2cset -y 0 0x3a 0x00 0xa0 0x01 0x00 0x00 0x01 0xff 0x00 0x00 0x00 0x01 0xa1 s
sleep 0.05

# Schritt 3: Farbe auf Weiß (cmd=0x02, p1=R=0xFF, p2=G=0xFF, p3=B=0xFF)
# sum = 0xA1 + 0x02 + 0xFF + 0xFF + 0xFF = 0x03A0
i2cset -y 0 0x3a 0x00 0xa0 0x01 0x00 0x00 0x02 0xff 0xff 0xff 0x00 0x03 0xa0 s
sleep 0.05

# Schritt 4: Einschalten (cmd=0x03, p1=0xFF)
# sum = 0xA1 + 0x03 + 0xFF = 0x01A3
i2cset -y 0 0x3a 0x00 0xa0 0x01 0x00 0x00 0x03 0xff 0x00 0x00 0x00 0x01 0xa3 s
sleep 0.05

# Schritt 5: Helligkeit auf 255 (wie Schritt 2)
i2cset -y 0 0x3a 0x00 0xa0 0x01 0x00 0x00 0x01 0xff 0x00 0x00 0x00 0x01 0xa1 s
sleep 0.05
```

---

## Test-Befehle: Einzelne LEDs steuern

Nach der Init-Sequenz können einzelne LEDs gesteuert werden.

### LED-Status lesen

```bash
# Syntax: i2cget -y 0 0x3a <0x81+led_index> i
i2cget -y 0 0x3a 0x81 i   # power
i2cget -y 0 0x3a 0x82 i   # LAN 1
i2cget -y 0 0x3a 0x83 i   # LAN 2
i2cget -y 0 0x3a 0x84 i   # disk1
i2cget -y 0 0x3a 0x85 i   # disk2
i2cget -y 0 0x3a 0x86 i   # disk3
i2cget -y 0 0x3a 0x87 i   # disk4
i2cget -y 0 0x3a 0x88 i   # disk5
i2cget -y 0 0x3a 0x89 i   # disk6
```

### LEDs einschalten (volle Helligkeit, weiß)

```bash
# Syntax: i2cset -y 0 0x3a <led_index> 0xa0 0x01 0x00 0x00 <cmd> <p1> <p2> <p3> <p4> <ck_hi> <ck_lo> s
# cmd=0x01, p1=0xFF → Helligkeit=255 (voll an)
# sum = 0xA1 + 0x01 + 0xFF = 0x01A1

i2cset -y 0 0x3a 0x00 0xa0 0x01 0x00 0x00 0x01 0xff 0x00 0x00 0x00 0x01 0xa1 s  # power
i2cset -y 0 0x3a 0x01 0xa0 0x01 0x00 0x00 0x01 0xff 0x00 0x00 0x00 0x01 0xa1 s  # LAN 1
i2cset -y 0 0x3a 0x02 0xa0 0x01 0x00 0x00 0x01 0xff 0x00 0x00 0x00 0x01 0xa1 s  # LAN 2
i2cset -y 0 0x3a 0x03 0xa0 0x01 0x00 0x00 0x01 0xff 0x00 0x00 0x00 0x01 0xa1 s  # disk1
i2cset -y 0 0x3a 0x04 0xa0 0x01 0x00 0x00 0x01 0xff 0x00 0x00 0x00 0x01 0xa1 s  # disk2
i2cset -y 0 0x3a 0x05 0xa0 0x01 0x00 0x00 0x01 0xff 0x00 0x00 0x00 0x01 0xa1 s  # disk3
i2cset -y 0 0x3a 0x06 0xa0 0x01 0x00 0x00 0x01 0xff 0x00 0x00 0x00 0x01 0xa1 s  # disk4
i2cset -y 0 0x3a 0x07 0xa0 0x01 0x00 0x00 0x01 0xff 0x00 0x00 0x00 0x01 0xa1 s  # disk5
i2cset -y 0 0x3a 0x08 0xa0 0x01 0x00 0x00 0x01 0xff 0x00 0x00 0x00 0x01 0xa1 s  # disk6
```

### LEDs ausschalten

```bash
# cmd=0x01, p1=0x00 → Helligkeit=0 (aus)
# sum = 0xA1 + 0x01 + 0x00 = 0x00A2

i2cset -y 0 0x3a 0x03 0xa0 0x01 0x00 0x00 0x01 0x00 0x00 0x00 0x00 0x00 0xa2 s  # disk1 aus
i2cset -y 0 0x3a 0x04 0xa0 0x01 0x00 0x00 0x01 0x00 0x00 0x00 0x00 0x00 0xa2 s  # disk2 aus
# usw. – Register entsprechend anpassen
```

### Farbe setzen

```bash
# cmd=0x02, p1=R, p2=G, p3=B
# Rot: sum = 0xA1 + 0x02 + 0xFF = 0x01A2
i2cset -y 0 0x3a 0x03 0xa0 0x01 0x00 0x00 0x02 0xff 0x00 0x00 0x00 0x01 0xa2 s  # disk1 rot

# Grün: sum = 0xA1 + 0x02 + 0x00 + 0xFF = 0x01A2
i2cset -y 0 0x3a 0x03 0xa0 0x01 0x00 0x00 0x02 0x00 0xff 0x00 0x00 0x01 0xa2 s  # disk1 grün

# Blau: sum = 0xA1 + 0x02 + 0x00 + 0x00 + 0xFF = 0x01A2
i2cset -y 0 0x3a 0x03 0xa0 0x01 0x00 0x00 0x02 0x00 0x00 0xff 0x00 0x01 0xa2 s  # disk1 blau

# Weiß: sum = 0xA1 + 0x02 + 0xFF + 0xFF + 0xFF = 0x03A0
i2cset -y 0 0x3a 0x03 0xa0 0x01 0x00 0x00 0x02 0xff 0xff 0xff 0x00 0x03 0xa0 s  # disk1 weiß
```

### Blinken (cmd=0x04)

```bash
# cmd=0x04, p1=t_cycle_hi, p2=t_cycle_lo, p3=t_on_hi, p4=t_on_lo
# Beispiel: t_cycle=1200ms (0x04B0), t_on=300ms (0x012C)
# sum = 0xA1 + 0x04 + 0x04 + 0xB0 + 0x01 + 0x2C = 0x182
i2cset -y 0 0x3a 0x03 0xa0 0x01 0x00 0x00 0x04 0x04 0xb0 0x01 0x2c 0x01 0x82 s  # disk1 blinkt
```

### Vollständiges Test-Skript

```bash
#!/bin/bash
# test_leds_idx6011pro.sh – LED-Test für UGREEN iDX6011 Pro

I2CBUS=0
ADDR=0x3a

send_led() {
    # $1=led_index $2=cmd $3=p1 $4=p2 $5=p3 $6=p4
    local reg=$1 cmd=$2 p1=$3 p2=$4 p3=$5 p4=$6
    local sum=$(( 0xA1 + cmd + p1 + p2 + p3 + p4 ))
    local ck_hi=$(( (sum >> 8) & 0xFF ))
    local ck_lo=$(( sum & 0xFF ))
    i2cset -y $I2CBUS $ADDR \
        $(printf "0x%02x" $reg) \
        0xa0 0x01 0x00 0x00 \
        $(printf "0x%02x" $cmd) \
        $(printf "0x%02x" $p1) \
        $(printf "0x%02x" $p2) \
        $(printf "0x%02x" $p3) \
        $(printf "0x%02x" $p4) \
        $(printf "0x%02x" $ck_hi) \
        $(printf "0x%02x" $ck_lo) \
        s
}

echo "=== Init-Sequenz (Host übernimmt Kontrolle) ==="
send_led 0 0x04 0 0 0 0; sleep 0.05
send_led 0 0x01 255 0 0 0; sleep 0.05
send_led 0 0x02 255 255 255 0; sleep 0.05
send_led 0 0x03 255 0 0 0; sleep 0.05
send_led 0 0x01 255 0 0 0; sleep 0.05

echo "=== Test: alle Disks einzeln einschalten ==="
for idx in 3 4 5 6 7 8; do
    echo "  disk$((idx-2)) einschalten..."
    send_led $idx 0x01 255 0 0 0
    sleep 1
    send_led $idx 0x01 0 0 0 0
    sleep 0.3
done

echo "=== Test: Farben auf disk1 ==="
echo "  Rot..."
send_led 3 0x02 255 0 0 0; sleep 1
echo "  Grün..."
send_led 3 0x02 0 255 0 0; sleep 1
echo "  Blau..."
send_led 3 0x02 0 0 255 0; sleep 1
echo "  Weiß (alle aus)..."
send_led 3 0x02 255 255 255 0
send_led 3 0x01 0 0 0 0   # aus

echo "=== Fertig ==="
```

---

## Änderungen am Open-Source-Treiber (led-ugreen.c)

Um den iDX6011 Pro zu unterstützen, sind folgende Änderungen nötig:

### 1. Schreibfunktion: `write_i2c_block_data` → `write_block_data`

In `kmod/led-ugreen.c`, Funktion `ugreen_led_change_state`:

```c
// DXP (aktuell):
s32 rc = i2c_smbus_write_i2c_block_data(client, led_id, 12, buf);

// iDX6011 Pro (neu):
// Daten ohne led_id in buf[0], da led_id nun im SMBus-Register steht
u8 buf_idx[11] = {
    0xa0, 0x01, 0x00, 0x00,
    command, param1, param2, param3, param4,
    (u8)((cksum >> 8) & 0xff), (u8)(cksum & 0xff)
};
s32 rc = i2c_smbus_write_block_data(client, led_id, 11, buf_idx);
```

### 2. LED-Anzahl und -Namen für iDX6011 Pro

In `kmod/led-ugreen.c`, Funktion `ugreen_led_probe`:

```c
// Ersetze das bestehende led_name[] array durch modellspezifische Arrays:
const char *led_name_dxp[] = {
    "power", "netdev", "disk1", "disk2", "disk3", "disk4", "disk5", "disk6", "disk7", "disk8"
};
const char *led_name_idx6011[] = {
    "power", "network_stat", "network_stat2",
    "disk1", "disk2", "disk3", "disk4", "disk5", "disk6"
};
```

### 3. Modell-Erkennung

Neue Helper-Funktion für das DMI-Product-Name-Matching:

```c
static bool is_idx6011_pro(void) {
    const char *product = dmi_get_system_info(DMI_PRODUCT_NAME);
    return product && strstr(product, "iDX6011 Pro") != NULL;
}
```

### 4. Init-Sequenz für iDX6011 Pro

In `ugreen_led_probe` nach der i2c-Client-Initialisierung:

```c
if (is_idx6011_pro()) {
    // Host-Übernahme: MCU aus Lauflicht-Modus holen
    ugreen_led_change_state(client, 0, 0x04, 0, 0, 0, 0); msleep(50);
    ugreen_led_change_state(client, 0, 0x01, 255, 0, 0, 0); msleep(50);
    ugreen_led_change_state(client, 0, 0x02, 255, 255, 255, 0); msleep(50);
    ugreen_led_change_state(client, 0, 0x03, 255, 0, 0, 0); msleep(50);
    ugreen_led_change_state(client, 0, 0x01, 255, 0, 0, 0); msleep(50);
}
```

---

## Checksummen-Tabelle (häufige Befehle)

| Befehl | cmd | p1 | p2 | p3 | p4 | sum | ck_hi | ck_lo |
|--------|-----|----|----|----|----|-----|-------|-------|
| An (voll weiß) | 0x01 | 0xFF | 0 | 0 | 0 | 0x01A1 | 0x01 | 0xA1 |
| Aus | 0x01 | 0x00 | 0 | 0 | 0 | 0x00A2 | 0x00 | 0xA2 |
| Rot | 0x02 | 0xFF | 0 | 0 | 0 | 0x01A2 | 0x01 | 0xA2 |
| Grün | 0x02 | 0 | 0xFF | 0 | 0 | 0x01A2 | 0x01 | 0xA2 |
| Blau | 0x02 | 0 | 0 | 0xFF | 0 | 0x01A2 | 0x01 | 0xA2 |
| Weiß (Farbe) | 0x02 | 0xFF | 0xFF | 0xFF | 0 | 0x03A0 | 0x03 | 0xA0 |
| An explizit | 0x03 | 0x01 | 0 | 0 | 0 | 0x00A5 | 0x00 | 0xA5 |
| Aus explizit | 0x03 | 0x00 | 0 | 0 | 0 | 0x00A4 | 0x00 | 0xA4 |
| Init cmd=4 | 0x04 | 0 | 0 | 0 | 0 | 0x00A5 | 0x00 | 0xA5 |
| Init Farbe weiß | 0x02 | 0xFF | 0xFF | 0xFF | 0 | 0x03A0 | 0x03 | 0xA0 |
| Init cmd=3+0xFF | 0x03 | 0xFF | 0 | 0 | 0 | 0x01A3 | 0x01 | 0xA3 |

---

## Bekannte Unsicherheiten

1. **Init zwingend notwendig?** – Der Treiber schickt die 5-Befehls-Init-Sequenz beim Laden.
   Ob der MCU Schreibbefehle ohne vorherige Init akzeptiert, wurde noch nicht getestet.
   Empfehlung: Init immer zuerst senden.

2. **Register-Offset korrekt?** – Die Analyse ergibt Write-Register = LED-Index (0–8),
   nicht 0x81+Index. Dies unterscheidet sich von der DXP-Serie. Noch nicht live getestet.

3. **cmd=0x03 vs cmd=0x01 für An/Aus** – Der Treiber verwendet intern cmd=0x01 mit
   p1=Helligkeit. cmd=0x03 (explizites An/Aus) funktioniert möglicherweise ebenfalls.

---

## Quelldateien der Analyse

- `kernel_modules/leds-mcu.ko` – Haupttreiber (Holtek HT32F52231 LED-Controller)
  - `i2c_iDX601x_write_led` bei Offset 0x3b0 – bestätigt `i2c_smbus_write_block_data`
  - `i2c_iDX601x_read_led` bei Offset 0x240 – bestätigt `i2c_smbus_read_i2c_block_data`
  - `brightness_work` bei Offset 0x10 – zeigt Befehlsformat für An/Aus
  - `leds_ugreen_probe` bei Offset 0x1440 – Init-Sequenz ab Offset 0x1720
- `kernel_modules/ug_idx6011pro-sio.ko` – Super-I/O für Watchdog, Fan, Power
- `kernel_modules/FlowingLeds` – Shell-Skript für DXP-Modelle (kein iDX6011 Pro-Eintrag)
