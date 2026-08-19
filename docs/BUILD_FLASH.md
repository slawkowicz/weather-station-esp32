# Kompilacja i wgrywanie

## CYD

Profil: `esp32:esp32:esp32`

```bash
~/bin/arduino-cli compile \
  --fqbn esp32:esp32:esp32 \
  ~/Arduino/CYD_Weather_UI_v1.0_FINAL
```

## LilyGO TTGO LoRa32 V2.1

Profil: `esp32:esp32:ttgo-lora32`

```bash
~/bin/arduino-cli compile \
  --fqbn esp32:esp32:ttgo-lora32 \
  ~/Arduino/LilyGO_GARNI_HUB_v1.0_FINAL
```

Wgrywanie (w testowanej konfiguracji LilyGO było na `/dev/ttyACM0`):

```bash
~/bin/arduino-cli upload \
  -p /dev/ttyACM0 \
  --fqbn esp32:esp32:ttgo-lora32 \
  ~/Arduino/LilyGO_GARNI_HUB_v1.0_FINAL
```

Monitor:

```bash
~/bin/arduino-cli monitor \
  -p /dev/ttyACM0 \
  -c baudrate=115200
```

## Minimalny test LilyGO

Po wgraniu sprawdź w logu:

```text
[RADIO] WeatherSensor OK
[ESP-NOW] OK
[AIO] code=200
CB_FAIL=0
```

BME280 w testowanej konfiguracji pracuje na `0x77` i może poprawnie wrócić po automatycznej ponownej inicjalizacji.
