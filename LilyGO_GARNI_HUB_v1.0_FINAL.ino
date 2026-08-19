#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

// ============================================================
// HUB ARCHITECTURE STEP 1 - WIFI CONFIG + API V1
// V1JA COMPILE FIX - TLS STATUS SCOPE:
// - strona WWW pokazuje stan Adafruit IO / TLS, HTTP, OK/ERR i date CA,
// - JavaScript liczy dni do 2027-11-02 z daty telefonu/komputera,
// - <=90 dni: ostrzezenie "CA WYGASA WKROTCE", po terminie: "CA PO TERMINIE",
// - /diag dostaje AIO_TLS, AIO_HTTP, AIO_OK, AIO_ERR, TLS_CA_EXPIRES,
// - sama transmisja TLS, radio Garni, BME280, OLED, ESP-NOW, NVS i captive portal bez zmian.
//
// V1I SECURITY STEP 3 - TLS VERIFY:
// - Adafruit IO HTTPS nie używa już setInsecure().
// - WiFiClientSecure weryfikuje certyfikat io.adafruit.com przez CA
//   zgodny z oficjalnym przykładem Adafruit dla ESP32.
// - Bez zmian radia Garni, BME280, OLED, ESP-NOW, API, NVS i captive portalu.
//
// V1H SECURITY STEP 2 - CONFIG AUTH:
// - /wifi i /aio wymagaja HTTP Basic Auth w normalnej sieci.
// - login: admin; haslo: aktualne haslo Wi-Fi zapisane w NVS.
// - w LilyGO-Setup autoryzacja jest wylaczona, captive portal dziala jak dotychczas.
//
// V1G SECURITY STEP 1 - NO SECRETS IN SOURCE:
// - usuniete prywatne SSID/hasla Wi-Fi i AIO Key z fallbackow.
// - istniejace wartosci NVS pozostaja bez zmian.
// - po pustym/wyczyszczonym NVS urzadzenie przechodzi do LilyGO-Setup.
//
// V1F AIO CONFIG:
// - Adafruit IO: enable, username, key, feed i interwal sa w Preferences "garni_cfg".
// - /aio udostepnia konfigurator; klucz nigdy nie jest wyswietlany.
// - puste pole AIO Key zachowuje poprzedni klucz.
// - obecne stale AIO pozostaja tylko jako fallback przy pierwszym uruchomieniu.
//
// V1E ESPNOW DYNAMIC CHANNEL:
// peer.channel = 0 -> ESP-NOW zawsze uzywa aktualnego kanalu interfejsu STA/AP.
// Chroni LOCAL po reconnect/roamingu, gdy router zmieni kanal.
// Zgodne z API Espressif; bez zmian formatu pakietu i whitelisty CYD.
//
// V1D RUNTIME RECONNECT:
// - wykrywa utrate Wi-Fi podczas normalnej pracy,
// - probuje reconnectu co 10 s bez restartu ESP32,
// - LOCAL radio + ESP-NOW dzialaja w czasie awarii,
// - po odzyskaniu Wi-Fi raportuje IP/kanal i wznawia uslugi internetowe,
// - jesli router wroci na innym kanale, ESP-NOW automatycznie pracuje
//   na kanale aktualnego polaczenia STA (wspolne radio ESP32).
//
// V1C CAPTIVE PORTAL FIX:
// Jawny adres AP 192.168.4.1/24 + wildcard DNS.
// Telefony/Windows sa kierowane na /wifi przez captive portal.
// AP_STA pozostaje, aby nie naruszyc dzialajacego ESP-NOW na WIFI_IF_STA.
// dnsServer.processNextRequest() obslugiwany w loop() tylko w setup mode.
//
// V1B SETUP AP FIX:
// W trybie awaryjnym SoftAP sam ustawia kanal 8.
// Usunieto reczne esp_wifi_set_channel() po starcie AP, aby nie rozjezdzac TCP/IP.
// Log WWW pokazuje softAPIP() zamiast 0.0.0.0 w trybie konfiguracji.
// ESP-NOW nadal inicjalizowany po uruchomieniu AP.
//
// V1A COMPILE FIX:
// Dodano <esp_wifi.h> wymagane przez esp_wifi_set_channel()
// i esp_wifi_set_promiscuous() w Arduino-ESP32 2.0.14.
// Bez zmian logiki Wi-Fi, API, ESP-NOW i radia.
//
// - /live-data pozostaje kompatybilne z obecnym CYD/AIO.
// - /api/v1/live to wersjonowany JSON dla Raspberry/Mega/Enigma2.
// - Wi-Fi SSID/haslo sa w Preferences "garni_cfg".
// - Po braku polaczenia LilyGO uruchamia AP LilyGO-Setup (kanal 8)
//   i strone /wifi do zapisania nowych danych.
// ============================================================

// ============================================================
// ARDUINO IDE
//
// Board: TTGO LoRa32-OLED
// Board Revision: TTGO LoRa32 V2.1 (1.6.1)
//
// SX1276: konfiguracja z wariantu płytki.
// Nie definiujemy ręcznie pinów radia.
// ============================================================

#define USE_SX1276
#undef USE_CC1101

#define WIND_DATA_FLOATINGPOINT 1
#define DECODE_BRESSER_6_IN_1 true
#undef DECODE_WEATHER1

#define NUM_SENSORS 1
#define MAX_SENSORS_DEFAULT 1
#define SCAN_CHANNELS false

#include "WeatherSensor.h"

// ============================================================
// WIFI
// ============================================================

const char* WIFI_SSID_FALLBACK = "";
const char* WIFI_PASS_FALLBACK = "";

String wifi_ssid = "";
String wifi_pass = "";
bool wifiConfigMode = false;

const char* WIFI_SETUP_AP_SSID = "LilyGO-Setup";
const char* WIFI_SETUP_AP_PASS = "garni1234";
const uint32_t WIFI_BOOT_CONNECT_TIMEOUT_MS = 15000UL;
const uint32_t WIFI_RUNTIME_RETRY_MS = 10000UL;
const uint32_t WIFI_RUNTIME_LOST_LOG_MS = 30000UL;

bool runtimeWifiWasConnected = false;
uint32_t runtimeWifiLastRetryMs = 0;
uint32_t runtimeWifiLastLostLogMs = 0;
uint32_t runtimeWifiReconnectCount = 0;

// Adafruit IO - wartosci fabryczne sa tylko fallbackiem dla pustego NVS.
const bool GARNI_AIO_ENABLE_FALLBACK = true;
const char* GARNI_AIO_USERNAME_FALLBACK = "";
const char* GARNI_AIO_KEY_FALLBACK      = "";
const char* GARNI_AIO_FEED_FALLBACK     = "garni-live";
const uint32_t GARNI_AIO_PUSH_MS_FALLBACK = 15000UL;

bool aio_enabled = GARNI_AIO_ENABLE_FALLBACK;
String aio_username = GARNI_AIO_USERNAME_FALLBACK;
String aio_key = GARNI_AIO_KEY_FALLBACK;
String aio_feed = GARNI_AIO_FEED_FALLBACK;
uint32_t aio_push_ms = GARNI_AIO_PUSH_MS_FALLBACK;

const uint32_t GARNI_AIO_TIMEOUT_MS = 5000UL;

// CA dla io.adafruit.com zgodny z oficjalnym przykładem Adafruit ESP32.
// Uwaga serwisowa: certyfikat w tym łańcuchu jest ważny do 2027-11-02,
// więc przy przyszłej aktualizacji projektu należy ponownie sprawdzić CA.
static const char ADAFRUIT_IO_ROOT_CA[] PROGMEM =
"-----BEGIN CERTIFICATE-----\n"
"MIIEjTCCA3WgAwIBAgIQDQd4KhM/xvmlcpbhMf/ReTANBgkqhkiG9w0BAQsFADBh\n"
"MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
"d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
"MjAeFw0xNzExMDIxMjIzMzdaFw0yNzExMDIxMjIzMzdaMGAxCzAJBgNVBAYTAlVT\n"
"MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
"b20xHzAdBgNVBAMTFkdlb1RydXN0IFRMUyBSU0EgQ0EgRzEwggEiMA0GCSqGSIb3\n"
"DQEBAQUAA4IBDwAwggEKAoIBAQC+F+jsvikKy/65LWEx/TMkCDIuWegh1Ngwvm4Q\n"
"yISgP7oU5d79eoySG3vOhC3w/3jEMuipoH1fBtp7m0tTpsYbAhch4XA7rfuD6whU\n"
"gajeErLVxoiWMPkC/DnUvbgi74BJmdBiuGHQSd7LwsuXpTEGG9fYXcbTVN5SATYq\n"
"DfbexbYxTMwVJWoVb6lrBEgM3gBBqiiAiy800xu1Nq07JdCIQkBsNpFtZbIZhsDS\n"
"fzlGWP4wEmBQ3O67c+ZXkFr2DcrXBEtHam80Gp2SNhou2U5U7UesDL/xgLK6/0d7\n"
"6TnEVMSUVJkZ8VeZr+IUIlvoLrtjLbqugb0T3OYXW+CQU0kBAgMBAAGjggFAMIIB\n"
"PDAdBgNVHQ4EFgQUlE/UXYvkpOKmgP792PkA76O+AlcwHwYDVR0jBBgwFoAUTiJU\n"
"IBiV5uNu5g/6+rkS7QYXjzkwDgYDVR0PAQH/BAQDAgGGMB0GA1UdJQQWMBQGCCsG\n"
"AQUFBwMBBggrBgEFBQcDAjASBgNVHRMBAf8ECDAGAQH/AgEAMDQGCCsGAQUFBwEB\n"
"BCgwJjAkBggrBgEFBQcwAYYYaHR0cDovL29jc3AuZGlnaWNlcnQuY29tMEIGA1Ud\n"
"HwQ7MDkwN6A1oDOGMWh0dHA6Ly9jcmwzLmRpZ2ljZXJ0LmNvbS9EaWdpQ2VydEds\n"
"b2JhbFJvb3RHMi5jcmwwPQYDVR0gBDYwNDAyBgRVHSAAMCowKAYIKwYBBQUHAgEW\n"
"HGh0dHBzOi8vd3d3LmRpZ2ljZXJ0LmNvbS9DUFMwDQYJKoZIhvcNAQELBQADggEB\n"
"AIIcBDqC6cWpyGUSXAjjAcYwsK4iiGF7KweG97i1RJz1kwZhRoo6orU1JtBYnjzB\n"
"c4+/sXmnHJk3mlPyL1xuIAt9sMeC7+vreRIF5wFBC0MCN5sbHwhNN1JzKbifNeP5\n"
"ozpZdQFmkCo+neBiKR6HqIA+LMTMCMMuv2khGGuPHmtDze4GmEGZtYLyF8EQpa5Y\n"
"jPuV6k2Cr/N3XxFpT3hRpt/3usU/Zb9wfKPtWpoznZ4/44c1p9rzFcZYrWkj3A+7\n"
"TNBJE0GmP2fhXhP1D/XVfIW/h0yCJGEiV9Glm/uGOa3DXHlmbAcxSyCRraG+ZBkA\n"
"7h4SeM6Y8l/7MBRpPCz6l8Y=\n"
"-----END CERTIFICATE-----\n";

static const char* TLS_CA_EXPIRES_DATE = "2027-11-02";
static const char* TLS_CA_EXPIRES_DISPLAY = "02.11.2027";

unsigned long lastAioPushMs = 0;
uint32_t aioPushOK = 0;
uint32_t aioPushErrors = 0;
int lastAioHttpCode = 0;
String lastAioError = "OFF";

String aioTlsRuntimeStatus() {
    if (!aio_enabled)
        return "OFF";

    if (lastAioHttpCode >= 200 && lastAioHttpCode < 300 && lastAioError == "OK")
        return "OK";

    if (lastAioHttpCode < 0)
        return "TLS/NET";

    if (lastAioError == "BEGIN")
        return "TLS/BEGIN";

    if (lastAioError == "CFG")
        return "CFG";

    if (lastAioHttpCode > 0)
        return "HTTP " + String(lastAioHttpCode);

    return "WAIT";
}


// ============================================================
// OLED TTGO LoRa32 V2.1
//
// SSD1306 128x64
// I2C: SDA 21 / SCL 22
//
// Sterownik OLED jest w tym pliku - bez dodatkowej biblioteki.
// ============================================================

#define OLED_ADDR 0x3C
#define OLED_W 128
#define OLED_H 64

bool oled_found = false;
unsigned long lastOledUpdate = 0;
unsigned long oledPageChanged = 0;
uint8_t oledPage = 0;

// ============================================================
// BME280
// ============================================================

Adafruit_BME280 bme;

bool bme_found = false;
uint8_t bme_address = 0;

unsigned long lastBmeRead = 0;
unsigned long lastBmeRetry = 0;

uint8_t bmeErrorCount = 0;

// ============================================================
// PAKIET CYD
// ============================================================

typedef struct __attribute__((packed)) WeatherPacket {
    float temperatura;
    float wilgotnosc;
    float cisnienie;
    float predkosc_wiatru;
    float poryw_wiatru;
    float opady;
    float opady_godzina;
    float uv_index;
    float swiatlo_lux;
    float kierunek_wiatru;
    float temp_wewnetrzna;
    float wilg_wewnetrzna;
} WeatherPacket;

static_assert(sizeof(WeatherPacket) == 48, "WeatherPacket must be 48 bytes");
static_assert(offsetof(WeatherPacket, temperatura)       == 0,  "WeatherPacket layout mismatch: temperatura");
static_assert(offsetof(WeatherPacket, wilgotnosc)       == 4,  "WeatherPacket layout mismatch: wilgotnosc");
static_assert(offsetof(WeatherPacket, cisnienie)        == 8,  "WeatherPacket layout mismatch: cisnienie");
static_assert(offsetof(WeatherPacket, predkosc_wiatru)  == 12, "WeatherPacket layout mismatch: predkosc_wiatru");
static_assert(offsetof(WeatherPacket, poryw_wiatru)     == 16, "WeatherPacket layout mismatch: poryw_wiatru");
static_assert(offsetof(WeatherPacket, opady)             == 20, "WeatherPacket layout mismatch: opady");
static_assert(offsetof(WeatherPacket, opady_godzina)     == 24, "WeatherPacket layout mismatch: opady_godzina");
static_assert(offsetof(WeatherPacket, uv_index)          == 28, "WeatherPacket layout mismatch: uv_index");
static_assert(offsetof(WeatherPacket, swiatlo_lux)       == 32, "WeatherPacket layout mismatch: swiatlo_lux");
static_assert(offsetof(WeatherPacket, kierunek_wiatru)   == 36, "WeatherPacket layout mismatch: kierunek_wiatru");
static_assert(offsetof(WeatherPacket, temp_wewnetrzna)   == 40, "WeatherPacket layout mismatch: temp_wewnetrzna");
static_assert(offsetof(WeatherPacket, wilg_wewnetrzna)   == 44, "WeatherPacket layout mismatch: wilg_wewnetrzna");

// V1JA: jawne prototypy po pełnej definicji WeatherPacket.
// Chroni kompilator Arduino 1.8.x / preprocessor przed błędną kolejnością prototypów.
void oledWeatherPage(const WeatherPacket& d);
void oledWindPage(const WeatherPacket& d);
String buildApiV1Json(const WeatherPacket &d);



WeatherPacket myData = {};

// ============================================================
// OBIEKTY
// ============================================================

WeatherSensor ws;
WebServer server(80);
DNSServer dnsServer;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

Preferences prefs;

// ============================================================
// SYNCHRONIZACJA
// ============================================================

SemaphoreHandle_t dataMutex = NULL;
volatile bool hasNewRadioData = false;

// Znaczniki swiezosci danych dla WWW.
volatile uint32_t lastRadioPacketMs = 0;
uint32_t lastBmeGoodMs = 0;
uint32_t webPageCounter = 0;
uint32_t webRequestCounter = 0;

// ============================================================
// OPAD Z OSTATNICH 60 MINUT
// ============================================================
// Bresser rain_mm traktujemy jako licznik narastajacy.
// Raz na minute zapamietujemy jego wartosc. Z 61 punktow wyliczamy
// przyrost z maksymalnie ostatniej godziny bez zmiany formatu pakietu.
struct RainMinuteSample {
    uint32_t ms;
    float totalMm;
};

static const uint8_t RAIN60_POINTS = 61;
RainMinuteSample rain60History[RAIN60_POINTS] = {};
uint8_t rain60Head = 0;
uint8_t rain60Count = 0;
uint32_t lastRain60StoreMs = 0;
float lastRainTotalSeen = NAN;

void resetRain60History(float totalMm) {
    rain60Head = 0;
    rain60Count = 0;
    lastRain60StoreMs = 0;

    if (isfinite(totalMm) && totalMm >= 0.0f) {
        rain60History[0].ms = millis();
        rain60History[0].totalMm = totalMm;
        rain60Head = 1;
        rain60Count = 1;
        lastRain60StoreMs = millis();
        lastRainTotalSeen = totalMm;
    } else {
        lastRainTotalSeen = NAN;
    }
}

float updateRain60(float totalMm) {
    if (!isfinite(totalMm) || totalMm < 0.0f)
        return 0.0f;

    const uint32_t now = millis();

    // Reset / wymiana baterii / restart licznika stacji:
    // nie pozwalamy, aby powstal ujemny godzinowy opad.
    if (isfinite(lastRainTotalSeen) && totalMm + 0.05f < lastRainTotalSeen) {
        Serial.print("[RAIN60] counter reset old=");
        Serial.print(lastRainTotalSeen, 2);
        Serial.print(" new=");
        Serial.println(totalMm, 2);
        resetRain60History(totalMm);
        return 0.0f;
    }

    lastRainTotalSeen = totalMm;

    if (rain60Count == 0) {
        resetRain60History(totalMm);
        return 0.0f;
    }

    // Wystarczy jeden punkt na minute; pakiety radiowe moga przychodzic czesciej.
    if (now - lastRain60StoreMs >= 60000UL) {
        rain60History[rain60Head].ms = now;
        rain60History[rain60Head].totalMm = totalMm;
        rain60Head = (rain60Head + 1) % RAIN60_POINTS;

        if (rain60Count < RAIN60_POINTS)
            rain60Count++;

        lastRain60StoreMs = now;
    }

    // Najstarszy punkt w buforze jest punktem odniesienia.
    uint8_t oldest =
        (rain60Head + RAIN60_POINTS - rain60Count) % RAIN60_POINTS;

    float base = rain60History[oldest].totalMm;
    float delta = totalMm - base;

    if (!isfinite(delta) || delta < 0.0f)
        delta = 0.0f;

    return delta;
}

// Statyczny bufor WWW - bez wielokrotnego String.replace() i bez fragmentacji sterty.
static char webBuffer[10800];

// ============================================================
// ADAFRUIT IO RELAY
// ============================================================
//
// RAIN60 SAFE:
//   opady          = surowy licznik rain_mm ze stacji
//   opady_godzina  = przyrost rain_mm z ostatnich do 60 minut
//
// Format WeatherPacket i JSON AIO pozostaje BEZ ZMIAN.
// ============================================================

// Forward declarations dla AIO
WeatherPacket snapshotWeatherPacket();
String buildLiveJson(const WeatherPacket &d);
String jsonEscapeForStringValue(const String &s);


WeatherPacket snapshotWeatherPacket() {
    WeatherPacket d = {};

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(&d, &myData, sizeof(d));
        xSemaphoreGive(dataMutex);
    }

    return d;
}

String buildLiveJson(const WeatherPacket &d) {
    char json[380];

    float t   = isfinite(d.temperatura)      ? d.temperatura      : 0.0f;
    float h   = isfinite(d.wilgotnosc)      ? d.wilgotnosc      : 0.0f;
    float b   = isfinite(d.cisnienie)       ? d.cisnienie       : 0.0f;
    float w   = isfinite(d.predkosc_wiatru) ? d.predkosc_wiatru : 0.0f;
    float g   = isfinite(d.poryw_wiatru)    ? d.poryw_wiatru    : 0.0f;
    float r   = isfinite(d.opady)            ? d.opady            : 0.0f;
    float rh  = isfinite(d.opady_godzina)    ? d.opady_godzina    : 0.0f;
    float uv  = isfinite(d.uv_index)         ? d.uv_index         : 0.0f;
    float lx  = isfinite(d.swiatlo_lux)      ? d.swiatlo_lux      : 0.0f;
    float dir = isfinite(d.kierunek_wiatru)  ? d.kierunek_wiatru  : 0.0f;
    float it  = isfinite(d.temp_wewnetrzna)  ? d.temp_wewnetrzna  : 0.0f;
    float ih  = isfinite(d.wilg_wewnetrzna)  ? d.wilg_wewnetrzna  : 0.0f;

    snprintf(
        json, sizeof(json),
        "{\"t\":%.1f,\"h\":%.1f,\"b\":%.1f,"
        "\"w\":%.1f,\"g\":%.1f,\"d\":%.1f,"
        "\"r\":%.1f,\"rh\":%.1f,\"uv\":%.1f,\"lx\":%.0f,"
        "\"it\":%.1f,\"ih\":%.1f}",
        t,h,b,w,g,dir,r,rh,uv,lx,it,ih
    );

    return String(json);
}

String jsonEscapeForStringValue(const String &s) {
    String out;
    out.reserve(s.length() + 24);

    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];

        if (c == '\\' || c == '"')
            out += '\\';

        if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out += c;
        }
    }

    return out;
}

void serviceAdafruitRelay() {
    static bool bootInfoPrinted = false;

    if (!bootInfoPrinted) {
        bootInfoPrinted = true;
        Serial.print("[AIO] ENABLE=");
        Serial.print(aio_enabled ? 1 : 0);
        Serial.print(" userLen=");
        Serial.print(aio_username.length());
        Serial.print(" keyLen=");
        Serial.print(aio_key.length());
        Serial.print(" feed=");
        Serial.println(aio_feed);
    }

    if (!aio_enabled) {
        lastAioError = "OFF";
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        static unsigned long lastWifiWarn = 0;
        if (millis() - lastWifiWarn >= 5000UL) {
            lastWifiWarn = millis();
            Serial.println("[AIO] WAIT WIFI");
        }
        return;
    }

    if (millis() - lastAioPushMs < aio_push_ms)
        return;

    lastAioPushMs = millis();

    Serial.print("[AIO] TICK ms=");
    Serial.println(millis());

    if (aio_username.length() == 0 ||
        aio_key.length() == 0 ||
        aio_feed.length() == 0) {
        lastAioError = "CFG";
        Serial.println("[AIO] ERR CFG");
        return;
    }

    WeatherPacket d = snapshotWeatherPacket();
    String liveJson = buildLiveJson(d);

    String url = "https://io.adafruit.com/api/v2/";
    url += aio_username;
    url += "/feeds/";
    url += aio_feed;
    url += "/data";

    String body = "{\"value\":\"";
    body += jsonEscapeForStringValue(liveJson);
    body += "\"}";

    Serial.print("[AIO] POST bytes=");
    Serial.println(body.length());

    WiFiClientSecure secure;
    secure.setCACert(ADAFRUIT_IO_ROOT_CA);

    HTTPClient http;
    http.setConnectTimeout(GARNI_AIO_TIMEOUT_MS);
    http.setTimeout(GARNI_AIO_TIMEOUT_MS);

    if (!http.begin(secure,url)) {
        lastAioError = "BEGIN";
        aioPushErrors++;
        Serial.println("[AIO] ERR BEGIN");
        return;
    }

    http.addHeader("X-AIO-Key",aio_key);
    http.addHeader("Content-Type","application/json");

    int code = http.POST(body);
    lastAioHttpCode = code;

    if (code >= 200 && code < 300) {
        aioPushOK++;
        lastAioError = "OK";
    } else {
        aioPushErrors++;
        lastAioError = "HTTP " + String(code);
    }

    http.end();

    Serial.print("[AIO] code=");
    Serial.print(code);
    Serial.print(" ok=");
    Serial.print(aioPushOK);
    Serial.print(" err=");
    Serial.print(aioPushErrors);
    Serial.print(" status=");
    Serial.println(lastAioError);
}

// ============================================================
// MQTT / DOMOTICZ
// ============================================================

bool mqtt_enabled = false;
bool domo_enabled = false;

String mqtt_broker = "";
int mqtt_port = 1883;

String mqtt_user = "";
String mqtt_pass = "";

int idx_temp = 0;
int idx_wind = 0;
int idx_rain = 0;

bool ha_discovery = false;

// ============================================================
// ESP-NOW
// ============================================================

uint8_t cydBroadcastAddress[] = {
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF
};

// ============================================================
// PROSTY FONT 5x7 DLA OLED
//
// Zakres ASCII 32..90 oraz cyfry/znaki potrzebne w projekcie.
// ============================================================

const uint8_t font5x7[][5] PROGMEM = {
{0x00,0x00,0x00,0x00,0x00}, // space 32
{0x00,0x00,0x5F,0x00,0x00}, // !
{0x00,0x07,0x00,0x07,0x00}, // "
{0x14,0x7F,0x14,0x7F,0x14}, // #
{0x24,0x2A,0x7F,0x2A,0x12}, // $
{0x23,0x13,0x08,0x64,0x62}, // %
{0x36,0x49,0x55,0x22,0x50}, // &
{0x00,0x05,0x03,0x00,0x00}, // '
{0x00,0x1C,0x22,0x41,0x00}, // (
{0x00,0x41,0x22,0x1C,0x00}, // )
{0x14,0x08,0x3E,0x08,0x14}, // *
{0x08,0x08,0x3E,0x08,0x08}, // +
{0x00,0x50,0x30,0x00,0x00}, // ,
{0x08,0x08,0x08,0x08,0x08}, // -
{0x00,0x60,0x60,0x00,0x00}, // .
{0x20,0x10,0x08,0x04,0x02}, // /
{0x3E,0x51,0x49,0x45,0x3E}, // 0
{0x00,0x42,0x7F,0x40,0x00}, // 1
{0x42,0x61,0x51,0x49,0x46}, // 2
{0x21,0x41,0x45,0x4B,0x31}, // 3
{0x18,0x14,0x12,0x7F,0x10}, // 4
{0x27,0x45,0x45,0x45,0x39}, // 5
{0x3C,0x4A,0x49,0x49,0x30}, // 6
{0x01,0x71,0x09,0x05,0x03}, // 7
{0x36,0x49,0x49,0x49,0x36}, // 8
{0x06,0x49,0x49,0x29,0x1E}, // 9
{0x00,0x36,0x36,0x00,0x00}, // :
{0x00,0x56,0x36,0x00,0x00}, // ;
{0x08,0x14,0x22,0x41,0x00}, // <
{0x14,0x14,0x14,0x14,0x14}, // =
{0x00,0x41,0x22,0x14,0x08}, // >
{0x02,0x01,0x51,0x09,0x06}, // ?
{0x32,0x49,0x79,0x41,0x3E}, // @
{0x7E,0x11,0x11,0x11,0x7E}, // A
{0x7F,0x49,0x49,0x49,0x36}, // B
{0x3E,0x41,0x41,0x41,0x22}, // C
{0x7F,0x41,0x41,0x22,0x1C}, // D
{0x7F,0x49,0x49,0x49,0x41}, // E
{0x7F,0x09,0x09,0x09,0x01}, // F
{0x3E,0x41,0x49,0x49,0x7A}, // G
{0x7F,0x08,0x08,0x08,0x7F}, // H
{0x00,0x41,0x7F,0x41,0x00}, // I
{0x20,0x40,0x41,0x3F,0x01}, // J
{0x7F,0x08,0x14,0x22,0x41}, // K
{0x7F,0x40,0x40,0x40,0x40}, // L
{0x7F,0x02,0x0C,0x02,0x7F}, // M
{0x7F,0x04,0x08,0x10,0x7F}, // N
{0x3E,0x41,0x41,0x41,0x3E}, // O
{0x7F,0x09,0x09,0x09,0x06}, // P
{0x3E,0x41,0x51,0x21,0x5E}, // Q
{0x7F,0x09,0x19,0x29,0x46}, // R
{0x46,0x49,0x49,0x49,0x31}, // S
{0x01,0x01,0x7F,0x01,0x01}, // T
{0x3F,0x40,0x40,0x40,0x3F}, // U
{0x1F,0x20,0x40,0x20,0x1F}, // V
{0x3F,0x40,0x38,0x40,0x3F}, // W
{0x63,0x14,0x08,0x14,0x63}, // X
{0x07,0x08,0x70,0x08,0x07}, // Y
{0x61,0x51,0x49,0x45,0x43}  // Z
};

// ============================================================
// OLED LOW LEVEL
// ============================================================

void oledCmd(uint8_t c) {
    Wire.beginTransmission(OLED_ADDR);
    Wire.write(0x00);
    Wire.write(c);
    Wire.endTransmission();
}

bool oledPing() {
    Wire.beginTransmission(OLED_ADDR);
    return Wire.endTransmission() == 0;
}

void oledInit() {
    if (!oledPing()) {
        oled_found = false;
        Serial.println("[OLED] Nie znaleziono 0x3C");
        return;
    }

    oled_found = true;

    const uint8_t initSeq[] = {
        0xAE,
        0xD5,0x80,
        0xA8,0x3F,
        0xD3,0x00,
        0x40,
        0x8D,0x14,
        0x20,0x00,
        0xA1,
        0xC8,
        0xDA,0x12,
        0x81,0x8F,
        0xD9,0xF1,
        0xDB,0x40,
        0xA4,
        0xA6,
        0xAF
    };

    for (uint8_t c : initSeq)
        oledCmd(c);

    Serial.println("[OLED] OK 0x3C");
}

void oledSetPos(uint8_t x, uint8_t page) {
    oledCmd(0xB0 + page);
    oledCmd(0x00 + (x & 0x0F));
    oledCmd(0x10 + (x >> 4));
}

void oledClear() {
    if (!oled_found) return;

    for (uint8_t page=0; page<8; page++) {
        oledSetPos(0,page);

        for (uint8_t block=0; block<8; block++) {
            Wire.beginTransmission(OLED_ADDR);
            Wire.write(0x40);

            for (uint8_t i=0; i<16; i++)
                Wire.write(0x00);

            Wire.endTransmission();
        }
    }
}

void oledChar(char ch, uint8_t x, uint8_t page, uint8_t scale=1) {
    if (!oled_found) return;

    if (ch >= 'a' && ch <= 'z')
        ch -= 32;

    if (ch < 32 || ch > 90)
        ch = '?';

    uint8_t idx = ch - 32;

    for (uint8_t col=0; col<5; col++) {
        uint8_t bits = pgm_read_byte(&font5x7[idx][col]);

        if (scale == 1) {
            oledSetPos(x + col, page);

            Wire.beginTransmission(OLED_ADDR);
            Wire.write(0x40);
            Wire.write(bits);
            Wire.endTransmission();
        }
        else {
            // skala 2x w pionie i poziomie, wykorzystuje dwie strony OLED
            uint16_t expanded = 0;

            for (uint8_t b=0; b<7; b++) {
                if (bits & (1 << b)) {
                    expanded |= (3UL << (b*2));
                }
            }

            for (uint8_t h=0; h<2; h++) {
                oledSetPos(x + col*2 + h, page);

                Wire.beginTransmission(OLED_ADDR);
                Wire.write(0x40);
                Wire.write(expanded & 0xFF);
                Wire.endTransmission();

                oledSetPos(x + col*2 + h, page+1);

                Wire.beginTransmission(OLED_ADDR);
                Wire.write(0x40);
                Wire.write((expanded >> 8) & 0xFF);
                Wire.endTransmission();
            }
        }
    }

    uint8_t blankX = x + (scale == 1 ? 5 : 10);
    oledSetPos(blankX,page);
    Wire.beginTransmission(OLED_ADDR);
    Wire.write(0x40);
    Wire.write(0x00);
    if (scale == 2) Wire.write(0x00);
    Wire.endTransmission();
}

void oledText(const String& text, uint8_t x, uint8_t page, uint8_t scale=1) {
    uint8_t step = scale == 1 ? 6 : 12;

    for (size_t i=0; i<text.length(); i++) {
        if (x > 127-step) break;
        oledChar(text[i], x, page, scale);
        x += step;
    }
}

void oledCentered(const String& text, uint8_t page, uint8_t scale=1) {
    uint8_t width = text.length() * (scale == 1 ? 6 : 12);
    uint8_t x = width < OLED_W ? (OLED_W-width)/2 : 0;
    oledText(text,x,page,scale);
}

// ============================================================
// OLED EKRANY
// ============================================================

String windDir(float deg) {
    if (!isfinite(deg)) return "--";

    const char* dirs[16] = {
        "N","NNE","NE","ENE",
        "E","ESE","SE","SSE",
        "S","SSW","SW","WSW",
        "W","WNW","NW","NNW"
    };

    int i = ((int)round(deg / 22.5f)) & 15;
    return String(dirs[i]);
}

void oledWeatherPage(const WeatherPacket& d) {
    oledClear();

    oledCentered("GARNI WEATHER",0,1);

    oledCentered(String(d.temperatura,1) + " C",2,2);

    String line = "H " + String(d.wilgotnosc,0) + "%";
    oledText(line,0,5,1);

    if (d.cisnienie > 100.0f)
        oledText("P " + String(d.cisnienie,0) + " HPA",64,5,1);
    else
        oledText("P --",76,5,1);

    oledText("WIFI " + String(WiFi.channel()),0,7,1);
}

void oledWindPage(const WeatherPacket& d) {
    oledClear();

    oledCentered("WIATR / OPAD",0,1);

    oledText("WIND",0,2,1);
    oledText(String(d.predkosc_wiatru,1) + " M/S",44,2,1);

    oledText("GUST",0,3,1);
    oledText(String(d.poryw_wiatru,1) + " M/S",44,3,1);

    oledText("DIR",0,4,1);
    oledText(windDir(d.kierunek_wiatru) + " " + String(d.kierunek_wiatru,0),44,4,1);

    oledText("RAIN",0,5,1);
    oledText(String(d.opady_godzina,1) + " MM",44,5,1);

    oledText("LUX",0,6,1);
    oledText(String(d.swiatlo_lux,0),44,6,1);

    oledText("UV",0,7,1);
    oledText(String(d.uv_index,1),44,7,1);
}

void serviceOLED() {
    if (!oled_found)
        return;

    // Pierwszy ekran po ok. 2 sekundach, potem zmiana co 8 sekund.
    if (lastOledUpdate != 0 && millis() - lastOledUpdate < 12000UL)
        return;

    lastOledUpdate = millis();

    if (lastOledUpdate > 3000UL)
        oledPage ^= 1;

    WeatherPacket d = {};

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(&d, &myData, sizeof(d));
        xSemaphoreGive(dataMutex);
    }

    if (oledPage == 0)
        oledWeatherPage(d);
    else
        oledWindPage(d);
}

// ============================================================
// I2C DIAGNOSTYKA
// ============================================================

void scanI2C() {
    Serial.println("[I2C] Skanowanie...");

    uint8_t found = 0;

    for (uint8_t a=1; a<127; a++) {
        Wire.beginTransmission(a);

        if (Wire.endTransmission() == 0) {
            Serial.print("[I2C] Urzadzenie 0x");

            if (a < 16) Serial.print("0");

            Serial.println(a,HEX);
            found++;
        }
    }

    if (!found)
        Serial.println("[I2C] Brak urzadzen.");
}

// ============================================================
// BME280
// ============================================================

bool initBME() {
    Serial.println("[BME280] Inicjalizacja...");

    bool ok = false;

    if (bme.begin(0x77,&Wire)) {
        bme_address = 0x77;
        ok = true;
    }
    else if (bme.begin(0x76,&Wire)) {
        bme_address = 0x76;
        ok = true;
    }

    if (!ok) {
        bme_found = false;
        Serial.println("[BME280] Nie znaleziono 0x76/0x77.");
        Serial.println("[BME280] Jesli skaner widzi tylko 0x3C, sprawdz zasilanie i przewody BME280.");
        return false;
    }

    bme_found = true;
    bmeErrorCount = 0;

    Serial.print("[BME280] OK 0x");
    Serial.println(bme_address,HEX);

    delay(100);

    return true;
}

bool readBME(float &t,float &h,float &p) {
    if (!bme_found)
        return false;

    // Dwie próby. Nie resetujemy BME po jednym błędzie.
    for (uint8_t attempt=0; attempt<2; attempt++) {
        t = bme.readTemperature();
        h = bme.readHumidity();
        p = bme.readPressure()/100.0f;

        if (isfinite(t) &&
            isfinite(h) &&
            isfinite(p) &&
            p > 300.0f &&
            p < 1200.0f) {
            return true;
        }

        delay(30);
    }

    return false;
}

void serviceBME() {
    if (!bme_found) {
        if (millis()-lastBmeRetry >= 30000UL) {
            lastBmeRetry = millis();

            // Nie resetujemy całej magistrali co 10 sekund.
            initBME();
        }
        return;
    }

    if (millis()-lastBmeRead < 5000UL)
        return;

    lastBmeRead = millis();

    float t,h,p;

    if (readBME(t,h,p)) {
        bmeErrorCount = 0;

        if (xSemaphoreTake(dataMutex,pdMS_TO_TICKS(100)) == pdTRUE) {
            myData.temp_wewnetrzna = t;
            myData.wilg_wewnetrzna = h;
            myData.cisnienie = p;
            xSemaphoreGive(dataMutex);
        }

        lastBmeGoodMs = millis();
        return;
    }

    bmeErrorCount++;

    Serial.print("[BME280] Blad odczytu ");
    Serial.print(bmeErrorCount);
    Serial.println("/3 - zachowuje ostatnie dobre dane.");

    // Dopiero trzy kolejne złe odczyty powodują ponowną inicjalizację.
    if (bmeErrorCount >= 3) {
        bme_found = false;
        bmeErrorCount = 0;
        lastBmeRetry = millis()-25000UL;
        Serial.println("[BME280] Trzy bledy - ponowna inicjalizacja za kilka sekund.");
    }
}

// ============================================================
// USTAWIENIA
// ============================================================

void loadSettings() {
    prefs.begin("garni_cfg",true);

    wifi_ssid = prefs.getString("wifi_ssid", WIFI_SSID_FALLBACK);
    wifi_pass = prefs.getString("wifi_pass", WIFI_PASS_FALLBACK);

    aio_enabled = prefs.getBool("aio_en", GARNI_AIO_ENABLE_FALLBACK);
    aio_username = prefs.getString("aio_user", GARNI_AIO_USERNAME_FALLBACK);
    aio_key = prefs.getString("aio_key", GARNI_AIO_KEY_FALLBACK);
    aio_feed = prefs.getString("aio_feed", GARNI_AIO_FEED_FALLBACK);
    aio_push_ms = prefs.getULong("aio_ms", GARNI_AIO_PUSH_MS_FALLBACK);
    if (aio_push_ms < 10000UL) aio_push_ms = 10000UL;
    if (aio_push_ms > 3600000UL) aio_push_ms = 3600000UL;

    mqtt_enabled = prefs.getBool("mqtt_en",false);
    domo_enabled = prefs.getBool("domo_en",false);

    mqtt_broker = prefs.getString("broker","");
    mqtt_port = prefs.getInt("port",1883);

    mqtt_user = prefs.getString("user","");
    mqtt_pass = prefs.getString("pass","");

    idx_temp = prefs.getInt("idx_temp",0);
    idx_wind = prefs.getInt("idx_wind",0);
    idx_rain = prefs.getInt("idx_rain",0);

    ha_discovery = prefs.getBool("ha_disc",false);

    prefs.end();
}


// ============================================================
// WIFI RUNTIME RECONNECT
// ============================================================

void serviceRuntimeWifi() {
    // Tryb captive portal ma wlasna logike. Nie probujemy wtedy
    // laczyc sie w tle z zapisanym SSID.
    if (wifiConfigMode)
        return;

    const wl_status_t st = WiFi.status();
    const uint32_t now = millis();

    if (st == WL_CONNECTED) {
        if (!runtimeWifiWasConnected) {
            runtimeWifiWasConnected = true;
            runtimeWifiReconnectCount++;

            Serial.print("[WiFi-RUN] RECOVERED IP=");
            Serial.print(WiFi.localIP());
            Serial.print(" channel=");
            Serial.print(WiFi.channel());
            Serial.print(" count=");
            Serial.println(runtimeWifiReconnectCount);

            WiFi.setSleep(false);

            // MQTT mogl byc rozlaczony podczas awarii. Ustawienie serwera
            // jest bezpieczne i nie ingeruje w LOCAL/ESP-NOW.
            if (mqtt_enabled && mqtt_broker.length())
                mqttClient.setServer(mqtt_broker.c_str(), mqtt_port);
        }
        return;
    }

    if (runtimeWifiWasConnected) {
        runtimeWifiWasConnected = false;
        runtimeWifiLastRetryMs = 0;
        runtimeWifiLastLostLogMs = now;

        Serial.print("[WiFi-RUN] LOST status=");
        Serial.println((int)st);
    } else if (now - runtimeWifiLastLostLogMs >= WIFI_RUNTIME_LOST_LOG_MS) {
        runtimeWifiLastLostLogMs = now;
        Serial.print("[WiFi-RUN] STILL OFFLINE status=");
        Serial.println((int)st);
    }

    if (runtimeWifiLastRetryMs == 0 ||
        now - runtimeWifiLastRetryMs >= WIFI_RUNTIME_RETRY_MS) {
        runtimeWifiLastRetryMs = now;

        Serial.print("[WiFi-RUN] RECONNECT SSID=");
        Serial.println(wifi_ssid);

        // reconnect() korzysta z konfiguracji STA zapisanej przez WiFi.begin()
        // i nie restartuje ESP32 ani ESP-NOW.
        WiFi.reconnect();
    }
}

String htmlEscapeCfg(const String &s) {
    String out;
    out.reserve(s.length() + 16);
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '&') out += F("&amp;");
        else if (c == '<') out += F("&lt;");
        else if (c == '>') out += F("&gt;");
        else if (c == '"') out += F("&quot;");
        else if (c == '\'') out += F("&#39;");
        else out += c;
    }
    return out;
}

bool ensureConfigAuth() {
    // W awaryjnym LilyGO-Setup konfigurator musi pozostac dostepny bez logowania.
    if (wifiConfigMode) return true;

    // W normalnej sieci uzywamy aktualnego hasla Wi-Fi zapisanego w NVS.
    // Nie dodajemy nowego sekretu do kodu.
    if (wifi_pass.length() == 0) {
        server.send(503, "text/plain; charset=utf-8",
                    "Konfiguracja WWW zablokowana: brak hasla Wi-Fi w NVS.");
        return false;
    }

    if (server.authenticate("admin", wifi_pass.c_str())) return true;

    Serial.println("[SEC] CONFIG AUTH required");
    server.requestAuthentication();
    return false;
}

void handleAioSettings() {
    if (!ensureConfigAuth()) return;
    String page;
    page.reserve(3000);

    page += F("<!doctype html><html lang='pl'><head><meta charset='utf-8'>");
    page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
    page += F("<title>LilyGO Adafruit IO</title><style>");
    page += F("body{font-family:Arial;background:#0c1118;color:#eef3f8;margin:0;padding:20px}");
    page += F(".box{max-width:560px;margin:auto;background:#151d27;padding:20px;border-radius:12px}");
    page += F("input{box-sizing:border-box;width:100%;padding:12px;margin:6px 0 14px;background:#0c1118;color:white;border:1px solid #445;border-radius:7px}");
    page += F("button{width:100%;padding:13px;background:#37c6d3;border:0;border-radius:8px;font-weight:bold}");
    page += F("a{color:#37c6d3}.note{color:#9aabba;font-size:12px}.check{width:auto;margin-right:8px}</style></head><body><div class='box'>");
    page += F("<h2>LilyGO-Weather - Adafruit IO</h2>");
    page += F("<form method='POST' action='/aio-save'>");
    page += F("<label><input class='check' type='checkbox' name='enabled' value='1' ");
    if (aio_enabled) page += F("checked");
    page += F(">Wysylanie do Adafruit IO wlaczone</label><br><br>");
    page += F("<label>Username</label><input name='user' maxlength='64' value='");
    page += htmlEscapeCfg(aio_username);
    page += F("'>");
    page += F("<label>AIO Key</label><input name='key' type='password' maxlength='128' placeholder='pozostaw puste aby nie zmieniac'>");
    page += F("<label>Feed</label><input name='feed' maxlength='64' value='");
    page += htmlEscapeCfg(aio_feed);
    page += F("'>");
    page += F("<label>Interwal wysylania [s]</label><input name='interval' type='number' min='10' max='3600' value='");
    page += String(aio_push_ms / 1000UL);
    page += F("'>");
    page += F("<button type='submit'>ZAPISZ I URUCHOM PONOWNIE</button></form>");
    page += F("<p class='note'>Klucz AIO nie jest wyswietlany. Puste pole zachowuje obecny klucz.</p>");
    page += F("<p>Status: <b>");
    page += aio_enabled ? "ON" : "OFF";
    page += F("</b>, ostatni wynik: ");
    page += htmlEscapeCfg(lastAioError);
    page += F("</p><p><a href='/wifi'>Wi-Fi</a> | <a href='/'>Powrot</a></p>");
    page += F("</div></body></html>");

    server.send(200, "text/html; charset=utf-8", page);
}

void handleAioSave() {
    if (!ensureConfigAuth()) return;
    bool newEnabled = server.hasArg("enabled");
    String newUser = server.arg("user");
    String newKey = server.arg("key");
    String newFeed = server.arg("feed");
    String intervalText = server.arg("interval");

    newUser.trim();
    newFeed.trim();
    intervalText.trim();

    long intervalSec = intervalText.toInt();

    if (newUser.length() > 64 || newKey.length() > 128 ||
        newFeed.length() > 64 || intervalSec < 10 || intervalSec > 3600) {
        server.send(400, "text/plain; charset=utf-8", "Nieprawidlowe dane Adafruit IO");
        return;
    }

    prefs.begin("garni_cfg", false);
    prefs.putBool("aio_en", newEnabled);
    prefs.putString("aio_user", newUser);
    prefs.putString("aio_feed", newFeed);
    prefs.putULong("aio_ms", (uint32_t)intervalSec * 1000UL);
    if (newKey.length() > 0)
        prefs.putString("aio_key", newKey);
    prefs.end();

    Serial.print("[AIO-CFG] SAVED enable=");
    Serial.print(newEnabled ? 1 : 0);
    Serial.print(" userLen=");
    Serial.print(newUser.length());
    Serial.print(" key=");
    Serial.print(newKey.length() ? "UPDATED" : "KEEP");
    Serial.print(" feed=");
    Serial.print(newFeed);
    Serial.print(" interval=");
    Serial.print(intervalSec);
    Serial.println("s");

    server.send(200, "text/html; charset=utf-8",
                "<html><body style='font-family:Arial'><h2>Zapisano Adafruit IO</h2>"
                "<p>LilyGO uruchomi sie ponownie.</p></body></html>");
    delay(500);
    ESP.restart();
}

// ============================================================
// WIFI CONFIG + API V1
// ============================================================

void handleWifiSettings() {
    if (!ensureConfigAuth()) return;
    String page;
    page.reserve(2200);

    page += F("<!doctype html><html lang='pl'><head><meta charset='utf-8'>");
    page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
    page += F("<title>LilyGO Wi-Fi</title><style>");
    page += F("body{font-family:Arial;background:#0c1118;color:#eef3f8;margin:0;padding:20px}");
    page += F(".box{max-width:520px;margin:auto;background:#151d27;padding:20px;border-radius:12px}");
    page += F("input{width:100%;padding:12px;margin:6px 0 14px;background:#0c1118;color:white;border:1px solid #445;border-radius:7px}");
    page += F("button{width:100%;padding:13px;background:#37c6d3;border:0;border-radius:8px;font-weight:bold}");
    page += F("a{color:#37c6d3}.note{color:#9aabba;font-size:12px}</style></head><body><div class='box'>");
    page += F("<h2>LilyGO-Weather - Wi-Fi</h2>");
    page += F("<p>Aktualne SSID: <b>");
    page += wifi_ssid;
    page += F("</b></p>");
    page += F("<form method='POST' action='/wifi-save'>");
    page += F("<label>SSID</label><input name='ssid' maxlength='32' required value='");
    page += wifi_ssid;
    page += F("'>");
    page += F("<label>Haslo</label><input name='pass' type='password' maxlength='64' placeholder='nowe haslo'>");
    page += F("<button type='submit'>ZAPISZ I URUCHOM PONOWNIE</button></form>");
    page += F("<p class='note'>Puste pole hasla zachowuje obecne haslo.</p>");
    page += F("<p><a href='/aio'>Adafruit IO</a> | <a href='/'>Powrot</a></p>");
    page += F("</div></body></html>");

    server.send(200, "text/html; charset=utf-8", page);
}

void handleWifiSave() {
    if (!ensureConfigAuth()) return;
    String newSsid = server.arg("ssid");
    String newPass = server.arg("pass");
    newSsid.trim();

    if (newSsid.length() == 0 || newSsid.length() > 32 || newPass.length() > 64) {
        server.send(400, "text/plain; charset=utf-8", "Nieprawidlowe dane Wi-Fi");
        return;
    }

    prefs.begin("garni_cfg", false);
    prefs.putString("wifi_ssid", newSsid);
    if (newPass.length() > 0)
        prefs.putString("wifi_pass", newPass);
    prefs.end();

    server.send(200, "text/html; charset=utf-8",
                "<html><body style='font-family:Arial'><h2>Zapisano Wi-Fi</h2>"
                "<p>LilyGO uruchomi sie ponownie.</p></body></html>");
    delay(500);
    ESP.restart();
}

void startWifiSetupAP() {
    wifiConfigMode = true;

    // Zatrzymujemy nieudana probe STA, ale zostawiamy interfejs STA aktywny
    // dla ESP-NOW. SoftAP dostaje jawny, stabilny adres 192.168.4.1.
    WiFi.disconnect(false, false);
    delay(50);
    WiFi.mode(WIFI_AP_STA);

    IPAddress apIP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);

    bool cfgOk = WiFi.softAPConfig(apIP, gateway, subnet);
    bool apOk = WiFi.softAP(WIFI_SETUP_AP_SSID, WIFI_SETUP_AP_PASS, 8);

    bool dnsOk = false;
    if (apOk) {
        dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
        dnsOk = dnsServer.start(53, "*", WiFi.softAPIP());
    }

    Serial.print("[WiFi] SETUP AP=");
    Serial.print(apOk ? "OK " : "FAIL ");
    Serial.print(WIFI_SETUP_AP_SSID);
    Serial.print(" IP=");
    Serial.print(WiFi.softAPIP());
    Serial.print(" CFG=");
    Serial.print(cfgOk ? "OK" : "FAIL");
    Serial.print(" DNS=");
    Serial.println(dnsOk ? "OK" : "FAIL");
}

String buildApiV1Json(const WeatherPacket &d) {
    char json[560];

    float t   = isfinite(d.temperatura)      ? d.temperatura      : 0.0f;
    float h   = isfinite(d.wilgotnosc)      ? d.wilgotnosc      : 0.0f;
    float b   = isfinite(d.cisnienie)        ? d.cisnienie       : 0.0f;
    float w   = isfinite(d.predkosc_wiatru) ? d.predkosc_wiatru : 0.0f;
    float g   = isfinite(d.poryw_wiatru)    ? d.poryw_wiatru    : 0.0f;
    float r   = isfinite(d.opady)            ? d.opady            : 0.0f;
    float rh  = isfinite(d.opady_godzina)    ? d.opady_godzina    : 0.0f;
    float uv  = isfinite(d.uv_index)         ? d.uv_index         : 0.0f;
    float lx  = isfinite(d.swiatlo_lux)      ? d.swiatlo_lux      : 0.0f;
    float dir = isfinite(d.kierunek_wiatru)  ? d.kierunek_wiatru  : 0.0f;
    float it  = isfinite(d.temp_wewnetrzna)  ? d.temp_wewnetrzna  : 0.0f;
    float ih  = isfinite(d.wilg_wewnetrzna)  ? d.wilg_wewnetrzna  : 0.0f;

    uint32_t radioAge = lastRadioPacketMs ? (millis() - lastRadioPacketMs) / 1000UL : 0xFFFFFFFFUL;

    snprintf(
        json, sizeof(json),
        "{\"schema\":1,\"device\":\"LilyGO-Weather\","
        "\"uptime_ms\":%lu,\"radio_age_s\":%lu,"
        "\"weather\":{\"t\":%.1f,\"h\":%.1f,\"b\":%.1f,"
        "\"w\":%.1f,\"g\":%.1f,\"d\":%.1f,"
        "\"r\":%.1f,\"rh\":%.1f,\"uv\":%.1f,\"lx\":%.0f,"
        "\"it\":%.1f,\"ih\":%.1f}}",
        (unsigned long)millis(), (unsigned long)radioAge,
        t,h,b,w,g,dir,r,rh,uv,lx,it,ih
    );

    return String(json);
}

void handleApiV1Live() {
    WeatherPacket d = snapshotWeatherPacket();
    String json = buildApiV1Json(d);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}

// ============================================================
// STRONA WWW - WBUDOWANA
// ============================================================

const char WEB_TEMPLATE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="pl">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Garni Weather Hub</title>
<style>
*{box-sizing:border-box}body{font-family:Arial,sans-serif;background:#0c1118;color:#eef3f8;margin:0}
main{max-width:980px;margin:auto;padding:16px}h1{text-align:center;color:#37c6d3;margin:6px 0 3px}
.sub{text-align:center;color:#93a6b7;margin-bottom:16px}.section{font-size:13px;color:#37c6d3;font-weight:bold;margin:18px 2px 8px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(145px,1fr));gap:10px}
.c{background:#151d27;border:1px solid #29394a;border-radius:12px;padding:14px;text-align:center;min-height:84px}
.l{font-size:10px;color:#91a3b5;font-weight:bold}.v{font-size:24px;font-weight:bold;margin-top:7px}
.u{font-size:12px;color:#37c6d3}.status{margin-top:16px;padding:11px;background:#151d27;border-radius:10px;text-align:center}
.ok{color:#70df93}.warn{color:#ffd166}.bad{color:#ff7272}
.meta{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:7px;margin-top:9px;color:#9aabba;font-size:12px}
.meta div{background:#121922;border-radius:8px;padding:7px;text-align:center}
.note{text-align:center;color:#6f8191;font-size:11px;margin-top:8px}
.tlsbox{margin-top:12px;padding:12px;background:#151d27;border:1px solid #29394a;border-radius:10px;text-align:center;font-size:12px}
.tlsok{color:#70df93}.tlswarn{color:#ffd166}.tlserr{color:#ff7272}.tlsmuted{color:#9aabba}
</style>
</head>
<body><main>
<h1>Garni Weather Hub</h1><div class="sub">TTGO LoRa32 V2.1</div>

<div class="section">NA ZEWNATRZ</div><div class="grid">
<div class="c"><div class="l">TEMPERATURA</div><div class="v">%.1f <span class="u">C</span></div></div>
<div class="c"><div class="l">WILGOTNOSC</div><div class="v">%.0f <span class="u">%%</span></div></div>
<div class="c"><div class="l">WIATR</div><div class="v">%.1f <span class="u">m/s</span></div></div>
<div class="c"><div class="l">PORYW</div><div class="v">%.1f <span class="u">m/s</span></div></div>
<div class="c"><div class="l">KIERUNEK</div><div class="v">%s %.0f&deg;</div></div>
<div class="c"><div class="l">OPAD</div><div class="v">%.1f <span class="u">mm</span></div></div>
<div class="c"><div class="l">UV</div><div class="v">%.1f</div></div>
<div class="c"><div class="l">SWIATLO</div><div class="v">%.0f <span class="u">lx</span></div></div>
</div>

<div class="section">WEWNATRZ / BME280</div><div class="grid">
<div class="c"><div class="l">TEMPERATURA WEW.</div><div class="v">%s <span class="u">C</span></div></div>
<div class="c"><div class="l">WILGOTNOSC WEW.</div><div class="v">%s <span class="u">%%</span></div></div>
<div class="c"><div class="l">CISNIENIE</div><div class="v">%s <span class="u">hPa</span></div></div>
</div>

<div class="status %s">%s</div>
<div class="meta">
<div>Pakiet stacji: <b>%s</b></div>
<div>BME280: <b>%s</b></div>
<div>Strona: <b>#%lu</b></div>
<div>WiFi/ESP-NOW: <b>kanal %u</b></div>
</div>
<div class="tlsbox">
<b>Adafruit IO / TLS</b><br>
Stan: <b id="tlsState">%s</b> &nbsp; HTTP: <b>%d</b><br>
OK: <b>%lu</b> &nbsp; ERR: <b>%lu</b><br>
CA wazny do: <b>%s</b><br>
<span id="tlsExpiry" class="tlsmuted">Sprawdzanie daty certyfikatu...</span>
</div>
<div class="note">Automatyczne odswiezanie co 10 s. Wersja bez fetch() i bez duzych operacji String.</div>
</main>
<script>
(function(){
 var exp=new Date("2027-11-02T23:59:59Z");
 var now=new Date();
 var days=Math.ceil((exp-now)/86400000);
 var el=document.getElementById("tlsExpiry");
 if(el){
   if(days<0){el.className="tlserr";el.textContent="UWAGA: CA PO TERMINIE - zaktualizuj ADAFRUIT_IO_ROOT_CA.";}
   else if(days<=90){el.className="tlswarn";el.textContent="CA WYGASA WKROTCE - pozostalo "+days+" dni.";}
   else{el.className="tlsok";el.textContent="CA OK - pozostalo okolo "+days+" dni.";}
 }
 var st=document.getElementById("tlsState");
 if(st){
   var t=st.textContent.trim();
   st.className=(t==="OK")?"tlsok":((t==="WAIT"||t==="OFF")?"tlswarn":"tlserr");
 }
})();
setTimeout(function(){
 var p=location.pathname||"/";
 location.replace(p+"?v="+Date.now());
},10000);
</script>
</body></html>
)HTML";

String ageText(uint32_t stamp) {
    if (stamp == 0) return "brak";
    uint32_t sec = (millis() - stamp) / 1000UL;
    if (sec < 60) return String(sec) + " s temu";
    uint32_t min = sec / 60UL;
    if (min < 60) return String(min) + " min temu";
    return String(min / 60UL) + " h temu";
}

void handleRoot() {
    WeatherPacket d = {};

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(&d, &myData, sizeof(d));
        xSemaphoreGive(dataMutex);
    }

    webPageCounter++;
    webRequestCounter++;

    char inTemp[20], inHum[20], press[20];

    if (lastBmeGoodMs != 0) {
        snprintf(inTemp, sizeof(inTemp), "%.1f", d.temp_wewnetrzna);
        snprintf(inHum,  sizeof(inHum),  "%.0f", d.wilg_wewnetrzna);
        snprintf(press,  sizeof(press),  "%.1f", d.cisnienie);
    } else {
        strcpy(inTemp, "--");
        strcpy(inHum, "--");
        strcpy(press, "--");
    }

    uint32_t radioAgeSec =
        lastRadioPacketMs == 0 ? 999999UL : (millis() - lastRadioPacketMs) / 1000UL;

    const char* statusClass;
    const char* statusText;

    if (radioAgeSec <= 30) {
        statusClass = "ok";
        statusText = "Dane stacji sa swieze";
    } else if (radioAgeSec <= 120) {
        statusClass = "warn";
        statusText = "Stacja dawno nie nadala nowego pakietu";
    } else {
        statusClass = "bad";
        statusText = "Brak swiezego pakietu ze stacji";
    }

    String ra = ageText(lastRadioPacketMs);
    String ba = ageText(lastBmeGoodMs);
    String wd = windDir(d.kierunek_wiatru);

    // snprintf_P czyta szablon bezposrednio z PROGMEM i wypelnia statyczny bufor.
    int len = snprintf_P(
        webBuffer,
        sizeof(webBuffer),
        WEB_TEMPLATE,
        d.temperatura,
        d.wilgotnosc,
        d.predkosc_wiatru,
        d.poryw_wiatru,
        wd.c_str(),
        d.kierunek_wiatru,
        d.opady_godzina,
        d.uv_index,
        d.swiatlo_lux,
        inTemp,
        inHum,
        press,
        statusClass,
        statusText,
        ra.c_str(),
        ba.c_str(),
        (unsigned long)webPageCounter,
        (unsigned int)WiFi.channel(),
        aioTlsRuntimeStatus().c_str(),
        lastAioHttpCode,
        (unsigned long)aioPushOK,
        (unsigned long)aioPushErrors,
        TLS_CA_EXPIRES_DISPLAY
    );

    if (len <= 0 || len >= (int)sizeof(webBuffer)) {
        Serial.println("[WWW] BLAD: bufor HTML za maly");
        server.send(500, "text/plain", "HTML buffer error");
        return;
    }

    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    server.send(200, "text/html; charset=utf-8", webBuffer);

    Serial.print("[WWW] GET / #");
    Serial.print(webRequestCounter);
    Serial.print(" bytes=");
    Serial.println(len);
}

void handleLiveData() {
    WeatherPacket d = {};

    if (xSemaphoreTake(dataMutex,pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(&d,&myData,sizeof(d));
        xSemaphoreGive(dataMutex);
    }

    #define SAFEVAL(x,def) if(!isfinite(x)) x=def
    SAFEVAL(d.temperatura,0);
    SAFEVAL(d.wilgotnosc,0);
    SAFEVAL(d.cisnienie,0);
    SAFEVAL(d.predkosc_wiatru,0);
    SAFEVAL(d.poryw_wiatru,0);
    SAFEVAL(d.opady,0);
    SAFEVAL(d.opady_godzina,0);
    SAFEVAL(d.uv_index,0);
    SAFEVAL(d.swiatlo_lux,0);
    SAFEVAL(d.kierunek_wiatru,0);
    SAFEVAL(d.temp_wewnetrzna,0);
    SAFEVAL(d.wilg_wewnetrzna,0);
    #undef SAFEVAL

    char json[380];

    snprintf(
        json,sizeof(json),
        "{\"t\":%.1f,\"h\":%.1f,\"b\":%.1f,"
        "\"w\":%.1f,\"g\":%.1f,\"d\":%.1f,"
        "\"r\":%.1f,\"rh\":%.1f,\"uv\":%.1f,\"lx\":%.0f,"
        "\"it\":%.1f,\"ih\":%.1f}",
        d.temperatura,d.wilgotnosc,d.cisnienie,
        d.predkosc_wiatru,d.poryw_wiatru,d.kierunek_wiatru,
        d.opady,d.opady_godzina,d.uv_index,d.swiatlo_lux,
        d.temp_wewnetrzna,d.wilg_wewnetrzna
    );

    server.sendHeader("Cache-Control","no-store, no-cache, must-revalidate, max-age=0");
    server.sendHeader("Pragma","no-cache");
    server.sendHeader("Expires","0");
    server.send(200,"application/json; charset=utf-8",json);
}

void handleDiag() {
    String s;

    s += "Garni Hub\n";
    s += "IP: " + WiFi.localIP().toString() + "\n";
    s += "Channel: " + String(WiFi.channel()) + "\n";
    s += "BME: " + String(bme_found ? "OK" : "NO") + "\n";
    s += "OLED: " + String(oled_found ? "OK" : "NO") + "\n";
    s += "Packet: " + String(sizeof(WeatherPacket)) + " B\n";
    s += "AIO_TLS=" + aioTlsRuntimeStatus() + "\n";
    s += "AIO_HTTP=" + String(lastAioHttpCode) + "\n";
    s += "AIO_OK=" + String(aioPushOK) + "\n";
    s += "AIO_ERR=" + String(aioPushErrors) + "\n";
    s += "TLS_CA_EXPIRES=" + String(TLS_CA_EXPIRES_DATE) + "\n";

    server.send(200,"text/plain",s);
}

// ============================================================
// RADIO
// ============================================================

void printRadioData() {
    Serial.print("[RADIO] TEMP=");
    Serial.print(ws.sensor[0].w.temp_c,1);

    Serial.print(" HUM=");
    Serial.print(ws.sensor[0].w.humidity,0);

    Serial.print(" WIND=");
    Serial.print(ws.sensor[0].w.wind_avg_meter_sec,1);

    Serial.print(" GUST=");
    Serial.print(ws.sensor[0].w.wind_gust_meter_sec,1);

    Serial.print(" DIR=");
    Serial.print(ws.sensor[0].w.wind_direction_deg,0);

    Serial.print(" RAIN_TOTAL=");
    Serial.print(ws.sensor[0].w.rain_mm,1);

    WeatherPacket dbg = snapshotWeatherPacket();
    Serial.print(" RAIN_60M=");
    Serial.print(dbg.opady_godzina,1);

    Serial.print(" UV=");
    Serial.print(ws.sensor[0].w.uv,1);

    Serial.print(" LUX=");
    Serial.println(ws.sensor[0].w.light_lux,0);
}

void garniRadioTask(void* p) {
    Serial.println("[RADIO TASK] START");

    for(;;) {
        ws.clearSlots();

        int status = ws.getMessage();

        if (status == DECODE_OK && ws.sensor[0].valid) {
            if (xSemaphoreTake(dataMutex,pdMS_TO_TICKS(200)) == pdTRUE) {
                if (ws.sensor[0].w.temp_ok)
                    myData.temperatura = ws.sensor[0].w.temp_c;

                if (ws.sensor[0].w.humidity_ok)
                    myData.wilgotnosc = ws.sensor[0].w.humidity;

                if (ws.sensor[0].w.wind_ok) {
                    myData.predkosc_wiatru = ws.sensor[0].w.wind_avg_meter_sec;
                    myData.poryw_wiatru = ws.sensor[0].w.wind_gust_meter_sec;
                    myData.kierunek_wiatru = ws.sensor[0].w.wind_direction_deg;
                }

                if (ws.sensor[0].w.rain_ok) {
                    const float rainTotal = ws.sensor[0].w.rain_mm;
                    myData.opady = rainTotal;
                    myData.opady_godzina = updateRain60(rainTotal);
                }

                if (ws.sensor[0].w.uv_ok)
                    myData.uv_index = ws.sensor[0].w.uv;

                if (ws.sensor[0].w.light_ok)
                    myData.swiatlo_lux = ws.sensor[0].w.light_lux;

                hasNewRadioData = true;
                lastRadioPacketMs = millis();

                xSemaphoreGive(dataMutex);
            }

            printRadioData();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ============================================================
// ESP-NOW
// ============================================================

volatile uint32_t espNowSendOk = 0;
volatile uint32_t espNowSendFail = 0;

void onEspNowSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS)
        espNowSendOk++;
    else
        espNowSendFail++;
}

bool initEspNow() {
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESP-NOW] BLAD init");
        return false;
    }

    esp_now_register_send_cb(onEspNowSent);

    esp_now_peer_info_t peer = {};

    memcpy(peer.peer_addr,cydBroadcastAddress,6);

    // 0 = aktualny kanal interfejsu STA/AP.
    // Nie blokujemy peer-a na kanale z momentu startu.
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    esp_err_t r = esp_now_add_peer(&peer);

    if (r != ESP_OK && r != ESP_ERR_ESPNOW_EXIST) {
        Serial.print("[ESP-NOW] BLAD peer ");
        Serial.println((int)r);
        return false;
    }

    Serial.print("[ESP-NOW] OK kanal ");
    Serial.print(WiFi.channel());
    Serial.println(" peer=AUTO");

    return true;
}

void sendToCYD() {
    if (!hasNewRadioData)
        return;

    WeatherPacket d = {};
    bool got = false;

    if (xSemaphoreTake(dataMutex,pdMS_TO_TICKS(300)) == pdTRUE) {
        memcpy(&d,&myData,sizeof(d));
        hasNewRadioData = false;
        xSemaphoreGive(dataMutex);
        got = true;
    }

    if (!got)
        return;

    esp_err_t r = esp_now_send(
        cydBroadcastAddress,
        (uint8_t*)&d,
        sizeof(d)
    );

    Serial.print("[CYD TX] T=");
    Serial.print(d.temperatura,1);
    Serial.print(" H=");
    Serial.print(d.wilgotnosc,0);
    Serial.print(" P=");
    Serial.print(d.cisnienie,1);
    Serial.print(" DIR=");
    Serial.print(d.kierunek_wiatru,0);
    Serial.print(" LUX=");
    Serial.print(d.swiatlo_lux,0);
    Serial.print(" SEND=");
    Serial.print((int)r);
    Serial.print(" CB_OK=");
    Serial.print(espNowSendOk);
    Serial.print(" CB_FAIL=");
    Serial.println(espNowSendFail);
}

// ============================================================
// MQTT
// ============================================================

void sendHomeAssistantDiscovery() {
    if (!mqttClient.connected() || !ha_discovery || !mqtt_enabled)
        return;

    String topic = "homeassistant/sensor/garni_outdoor_temp/config";

    String payload =
        "{\"stat_t\":\"garni/state\","
        "\"name\":\"Garni Temperatura\","
        "\"unit_of_meas\":\"°C\","
        "\"val_tpl\":\"{{value_json.temp}}\","
        "\"dev_cla\":\"temperature\","
        "\"uniq_id\":\"garni_temp\"}";

    mqttClient.publish(topic.c_str(),payload.c_str(),true);
}

void serviceMQTT() {
    if (!mqtt_enabled ||
        mqtt_broker.length()==0 ||
        WiFi.status()!=WL_CONNECTED)
        return;

    if (!mqttClient.connected()) {
        mqttClient.connect(
            "GarniLilyGO",
            mqtt_user.length() ? mqtt_user.c_str() : NULL,
            mqtt_pass.length() ? mqtt_pass.c_str() : NULL
        );
    }

    if (mqttClient.connected())
        mqttClient.loop();
}

// ============================================================
// SETUP
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" GARNI HUB + OLED + AIO + RAIN60 SAFE");
    Serial.println(" TTGO LoRa32 V2.1 (1.6.1)");
    Serial.println("======================================");

    dataMutex = xSemaphoreCreateMutex();

    if (!dataMutex) {
        Serial.println("[SYSTEM] BLAD mutex");
        while(1) delay(1000);
    }

    myData.swiatlo_lux = -1.0f;

    loadSettings();

    // ========================================================
    // I2C
    // 100 kHz dla stabilności BME + OLED
    // ========================================================

    Wire.begin(21,22,100000);
    delay(100);

    scanI2C();

    oledInit();
    oledClear();

    if (oled_found) {
        oledCentered("GARNI HUB",2,2);
        oledCentered("START",5,1);
    }

    initBME();

    // Pierwszy odczyt BME
    if (bme_found) {
        float t,h,p;

        if (readBME(t,h,p)) {
            myData.temp_wewnetrzna = t;
            myData.wilg_wewnetrzna = h;
            myData.cisnienie = p;
            lastBmeGoodMs = millis();

            Serial.print("[BME280] P=");
            Serial.println(p,1);
        }
    }

    // ========================================================
    // RADIO
    // ========================================================

    Serial.println("[RADIO] WeatherSensor start");
    ws.begin(MAX_SENSORS_DEFAULT,false,0.0);
    Serial.println("[RADIO] WeatherSensor OK");

    // ========================================================
    // WIFI - tylko STA, bez dodatkowego AP
    // ========================================================

    // Arduino-ESP32 2.x: hostname ustawiamy PRZED uruchomieniem STA.
    bool hostnameOK = WiFi.setHostname("LilyGO-Weather");
    Serial.print("[WiFi] Hostname set=");
    Serial.print(hostnameOK ? "OK " : "FAIL ");
    Serial.println(WiFi.getHostname());

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());

    Serial.print("[WiFi] Laczenie SSID=");
    Serial.print(wifi_ssid);

    uint32_t wifiStartMs = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - wifiStartMs < WIFI_BOOT_CONNECT_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[WiFi] Hostname=");
        Serial.println(WiFi.getHostname());

        Serial.print("[WiFi] IP=");
        Serial.println(WiFi.localIP());

        Serial.print("[WiFi] Kanal=");
        Serial.println(WiFi.channel());
    } else {
        Serial.println("[WiFi] Brak polaczenia -> tryb konfiguracji");
        startWifiSetupAP();
    }

    // Dla serwera WWW i ESP-NOW wyłączamy oszczędzanie energii WiFi.
    WiFi.setSleep(false);
    Serial.println("[WiFi] Power save = OFF");

    runtimeWifiWasConnected = (WiFi.status() == WL_CONNECTED);
    runtimeWifiLastRetryMs = millis();
    runtimeWifiLastLostLogMs = millis();

    // ========================================================
    // ESP-NOW
    // ========================================================

    // W trybie SETUP AP SoftAP zostal juz uruchomiony na kanale 8.
    // Nie przestawiamy ponownie radia niskopoziomowo, bo mogloby to
    // zerwac warstwe TCP/IP SoftAP.
    initEspNow();

    // ========================================================
    // WWW
    // ========================================================

    server.on("/", HTTP_GET, []() {
        if (wifiConfigMode) handleWifiSettings();
        else handleRoot();
    });
    server.on("/live-data",HTTP_GET,handleLiveData);          // legacy - bez zmian
    server.on("/api/v1/live",HTTP_GET,handleApiV1Live);       // przyszli klienci
    server.on("/wifi",HTTP_GET,handleWifiSettings);
    server.on("/wifi-save",HTTP_POST,handleWifiSave);
    server.on("/aio",HTTP_GET,handleAioSettings);
    server.on("/aio-save",HTTP_POST,handleAioSave);
    server.on("/diag",HTTP_GET,handleDiag);

    // Typowe testy captive portal systemow mobilnych/desktopowych.
    server.on("/generate_204", HTTP_GET, handleWifiSettings);        // Android
    server.on("/gen_204", HTTP_GET, handleWifiSettings);             // Android
    server.on("/hotspot-detect.html", HTTP_GET, handleWifiSettings); // Apple
    server.on("/library/test/success.html", HTTP_GET, handleWifiSettings);
    server.on("/connecttest.txt", HTTP_GET, handleWifiSettings);     // Windows
    server.on("/ncsi.txt", HTTP_GET, handleWifiSettings);            // Windows
    server.on("/fwlink", HTTP_GET, handleWifiSettings);

    server.onNotFound([](){
        if (wifiConfigMode) {
            // DNS kieruje dowolna nazwe tutaj; pokazujemy od razu konfiguracje.
            handleWifiSettings();
        } else {
            server.sendHeader("Location","/",true);
            server.send(302,"text/plain","");
        }
    });

    server.begin();

    Serial.println("[WWW] Serwer OK");
    if (wifiConfigMode)
        Serial.println("[WWW] http://" + WiFi.softAPIP().toString());
    else
        Serial.println("[WWW] http://" + WiFi.localIP().toString());

    // ========================================================
    // MQTT
    // ========================================================

    if (!wifiConfigMode && mqtt_enabled && mqtt_broker.length())
        mqttClient.setServer(mqtt_broker.c_str(),mqtt_port);

    // ========================================================
    // RADIO TASK
    // ========================================================

    BaseType_t tr = xTaskCreatePinnedToCore(
        garniRadioTask,
        "garniRadioTask",
        4096,
        NULL,
        1,
        NULL,
        0
    );

    Serial.println(tr==pdPASS ? "[SYSTEM] Radio task OK" : "[SYSTEM] Radio task BLAD");

    Serial.println("# SYSTEM GOTOWY");
}

// ============================================================
// LOOP
// ============================================================

void loop() {
    if (wifiConfigMode)
        dnsServer.processNextRequest();

    serviceRuntimeWifi();
    server.handleClient();

    serviceBME();

    // Po obsludze I2C ponownie dajemy szanse serwerowi.
    server.handleClient();

    serviceOLED();

    // OLED wykonuje wiele transmisji I2C, wiec po nim znow obslugujemy HTTP.
    server.handleClient();

    serviceMQTT();
    sendToCYD();

    // Internet relay jest dodatkiem; LOCAL/ESP-NOW pozostaje bez zmian.
    serviceAdafruitRelay();

    server.handleClient();

    delay(2);
}