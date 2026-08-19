# Weather Station v1.0 FINAL

Stabilna, zamrożona baza projektu stacji pogodowej ESP32.

## Architektura

`Garni/Bresser -> LilyGO TTGO LoRa32 V2.1 -> ESP-NOW -> CYD`

LilyGO obsługuje odbiór radiowy, BME280, OLED, WWW, konfigurację Wi-Fi/NVS, Adafruit IO przez zweryfikowany TLS i diagnostykę. CYD obsługuje ESP-NOW, kartę SD, historię, wykresy, FOCUS oraz interfejs PL/EN/DE/CZ.

## Firmware v1.0 FINAL

- [CYD_Weather_UI_v1.0_FINAL.ino](CYD_Weather_UI_v1.0_FINAL.ino) — `RC15.256 FINAL GLOBAL COMPASS`
- [LilyGO_GARNI_HUB_v1.0_FINAL.ino](LilyGO_GARNI_HUB_v1.0_FINAL.ino) — `V1JA TLS STATUS SCOPE FIX`

Firmware v1.0 traktujemy jako punkt powrotu i nie dodajemy do niego nowych funkcji. APRS i dalsze integracje rozwijamy na osobnej gałęzi `v2.0-development`.

## TLS / Adafruit IO

LilyGO używa `WiFiClientSecure` z weryfikacją CA. Panel WWW pokazuje stan TLS, kod HTTP, liczniki OK/ERR, datę CA i ostrzeżenie serwisowe. W tej wersji data serwisowa CA to **02.11.2027**.

## Dokumentacja

- [docs/SERVICE.md](docs/SERVICE.md) — diagnostyka i serwis
- [docs/BUILD_FLASH.md](docs/BUILD_FLASH.md) — kompilacja i wgrywanie
- [CHANGELOG.md](CHANGELOG.md) — zakres v1.0
- [VERSION.txt](VERSION.txt) — zamrożone wersje firmware

## Rozwój

`main` = stabilna v1.0 FINAL.  
`v2.0-development` = przyszły rozwój, m.in. APRS.
