# Instrukcja serwisowa — Weather Station v1.0 FINAL

## Wersja zamrożona

`main` jest stabilną bazą v1.0. APRS i kolejne funkcje należy rozwijać na `v2.0-development`, bez modyfikowania punktu odniesienia v1.0.

## Adafruit IO / TLS

LilyGO używa `WiFiClientSecure` z `setCACert(...)`, a nie `setInsecure()`. Publiczny certyfikat CA zapisany w firmware nie jest sekretem.

W v1.0 zapisana data serwisowa CA to **02.11.2027**. Strona LilyGO pokazuje stan TLS, HTTP, liczniki OK/ERR, datę CA i liczbę dni do terminu. Poniżej 90 dni pojawia się ostrzeżenie, a po terminie komunikat o konieczności aktualizacji CA.

`/diag` zawiera m.in.:

```text
AIO_TLS=OK
AIO_HTTP=200
AIO_OK=...
AIO_ERR=...
TLS_CA_EXPIRES=2027-11-02
```

Przed 02.11.2027 należy sprawdzić aktualny łańcuch certyfikatów `io.adafruit.com`, zweryfikować aktualne zalecenia/przykład Adafruit, w razie potrzeby podmienić `ADAFRUIT_IO_ROOT_CA`, skompilować i po wgraniu potwierdzić `[AIO] code=200`, `status=OK` oraz `TLS OK` na WWW.

Problem TLS/AIO nie powinien zatrzymywać lokalnego toru `Garni -> LilyGO -> ESP-NOW -> CYD`.

## Wi-Fi i sekrety

Konfiguracja jest przechowywana w NVS. Prywatne hasła i AIO Key nie powinny trafiać do publicznego repozytorium jako fallbacki.

LilyGO ma awaryjny AP `LilyGO-Setup`, a CYD `CYD-Setup`. Konfiguracja w normalnej sieci jest chroniona.

## ESP-NOW

Pakiet LilyGO -> CYD ma 48 bajtów. Finalny kod kontroluje jego rozmiar i układ pól przez `static_assert`.

Prawidłowy log:

```text
[ESP-NOW] OK kanal 8 peer=AUTO
[CYD TX] ... SEND=0 CB_OK=... CB_FAIL=0
```

## BME280

OLED: `0x3C`. BME280: `0x76` lub `0x77`; w testowanej konfiguracji `0x77`.

Zaakceptowany przypadek recovery:

```text
[BME280] Nie znaleziono 0x76/0x77.
...
[BME280] Inicjalizacja...
[BME280] OK 0x77
```

Jeśli BME nie wraca, sprawdzić zasilanie, SDA, SCL i połączenia.

## Serial

Poszatkowane fragmenty linii mogą wynikać z równoczesnego drukowania przez task radia i główną pętlę. Jeżeli pełne `[CYD TX]` są poprawne, `CB_FAIL=0`, a AIO zwraca HTTP 200, nie oznacza to samo w sobie uszkodzenia danych.

## CYD / SD

Końcowo zaakceptowano: SD init/RW OK, historię i archiwum, `bad=0`, `seqGaps=0`, synchronizację MAIN/INDOOR/WIND-DIR oraz CSV tail OK.

## Aktualizacje

v1.0 była testowana z Arduino-ESP32 2.0.14. Aktualizacje core i bibliotek należy wykonywać w gałęzi rozwojowej i po nich powtarzać pełny test.
