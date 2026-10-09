# Music Assistant Controller – MaTouch ESP32-S3 RotaryIPS 1.28" (GC9A01)

Ein physischer Music-Assistant-Controller für das
[MaTouch ESP32-S3 RotaryIPS-Display1.28" GC9A01](https://www.makerfabs.com/matouch-esp32-s3-rotaryips-display1-28-gc9a01.html)
(Makerfabs, Modell **MTRO128G**).

Das runde 240×240-IPS-Display mit Drehgeber und Kapazitiv-Touch verbindet sich per
WebSocket direkt mit dem [Music Assistant Server](https://music-assistant.io) und
steuert dort Player (Play/Pause, Next/Previous, Lautstärke, Power, Player-Auswahl).

![Board](docs/board.jpg)

## Features

- **Verbindung** zum Music Assistant Server über die WebSocket-API (`/ws`),
  inkl. Token-Auth (API-Schema ≥ 28) und automatischem Reconnect
- **Live-Status**: Player-Name, Titel/Artist, Wiedergabezustand, Lautstärke –
  Updates kommen als `player_updated`-Events (kein Polling nötig)
- **Volume-Arc** rund ums Display, Status-Ring farbcodiert
  (grün = playing, amber = paused, rot = stopped)
- **Steuerung**:
  - Drehen = Lautstärke, Drücken = Volume übernehmen / Play-Pause
  - Touch Mitte = Play/Pause, rechts/links = Next/Previous,
    oben = Playerliste, unten = Menü
  - Menü: Volume, Players, Power an/aus, WiFi-Setup
- **WiFiManager-Captive-Portal** bei Erstboot (AP `MA-Controller`) mit Feldern für
  MA-Server-Host, Port (Default 8095) und API-Token; speichert in NVS/Preferences
- **CST816-Touchtreiber** (I2C) und **EC11-Drehgeber** (Quadratur) integriert

## Hardware / Pinout

Verifiziert gegen das Makerfabs-Beispiel ([fw_test.ino](https://github.com/Makerfabs/MaTouch-ESP32-S3-RotaryIPS-Display1.28-GC9A01)):

| Funktion | GPIO |
|---|---|
| GC9A01 SCLK / MOSI / MISO | 14 / 13 / 12 |
| GC9A01 CS / DC / RES / BL | 15 / 21 / 11 / 7 |
| CST816 SDA / SCL / RST / INT | 40 / 39 / 16 / 38 |
| Encoder CLK / DT / SW | 9 / 10 / 6 |
| SD CS | 2 (SPI-Bus geteilt mit LCD) |

Alle Pins zentral in [`firmware/src/config.h`](firmware/src/config.h).

## Software-Aufbau

```
firmware/
├── platformio.ini        PlatformIO-Projekt (espressif32 6.9.0, Arduino)
├── src/
│   ├── config.h          Pin-Definitionen & App-Konstanten
│   ├── cst816.{h,cpp}     Minimal-Treiber CST816 (I2C, IRQ-basiert)
│   ├── ma_client.{h,cpp}  MA-WebSocket-Client (ArduinoJson, links2004/WebSockets)
│   └── main.cpp           UI, Eingabe, WiFiManager, Screens
└── lib/                   (reserviert für lokale Libs)
```

## Bauen & Flashen

```bash
cd firmware
pio run                # nur bauen
pio run -t upload      # flashen (USB-C an den USB-Native-Port!)
pio device monitor     # Serial-Log @115200
```

Arduino-IDE-Alternativpath: Board „ESP32-S3 Dev module", Flash 16MB, PSRAM OPI.

## Einrichtung

1. Firmware flashen → Display zeigt „Connecting WiFi..."
2. Mit dem AP **MA-Controller** verbinden, WiFi auswählen und
   **MA Server (IP/Hostname)**, **MA Port** (Standard 8095) und
   **MA Token** eintragen (Token in der MA-WebUI unter Settings → Security)
3. Speichern → Gerät verbindet sich und zeigt nach Auth die Playerliste

Zurück ins Setup: Menü (Touch unten) → „WiFi setup" → Config-Portal startet neu.

## Bedienung

| Aktion | Ergebnis |
|---|---|
| Drehen (im Player-Screen) | Volume-Arc anzeigen & ändern |
| Drücken (im Volume-Screen) | Lautstärke übernehmen, zurück zum Player |
| Drücken (im Player-Screen) | Play/Pause |
| Touch Mitte | Play/Pause |
| Touch rechts / links | Next / Previous Track |
| Touch oben / unten | Playerliste / Menü |

## Music Assistant API

Der Client implementiert das Wire-Format der MA-WebSocket-API:

```json
{"message_id": "1", "command": "players/cmd/play_pause", "args": {"player_id": "..."}}
{"message_id": "2", "command": "auth", "args": {"token": "..."}}
```

Empfangen werden `result`-Nachrichten (für `players/all` etc.) und
`event`-Nachrichten (`player_updated`, `queue_time_updated`, `player_added/removed`).

Referenzen: [music-assistant/client](https://github.com/music-assistant/client),
[MA-WebSocket-Doku](https://music-assistant.io/).

## Lizenz / Credits

- Board: Makerfabs MaTouch ESP32-S3 RotaryIPS-Display1.28" (MTRO128G)
- Libs: Arduino_GFX, ArduinoJson, WebSockets, WiFiManager, ESP32Encoder
  ( jeweilige eigene Lizenzen der Bibliotheken )

Getestet: Build mit PlatformIO, espressif32 6.9.0 (Arduino Core 2.0.17),
RAM 14.6 % / Flash 15.8 %.
