#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SD.h>
#include <time.h>
#include <esp_sntp.h>

#include <XPT2046_Touchscreen.h>

// ============================================================
// RC15.245A - Arduino IDE 1.8.x prototype-generator compatibility
// Forward declarations must precede Arduino-generated prototypes.
// ============================================================
enum LedWeatherLevel : uint8_t;
enum WifiProfile : uint8_t;
struct WeatherPacket;
struct ChartSample;
struct ChartDiskRecord;
struct IndoorDiskRecord;
struct WindDirDiskRecord;
struct WindDirSample;
struct HistoryBundleJournal;
enum ChartRange : uint8_t;
enum ChartPage : uint8_t;
enum SkinId : uint8_t;
enum AlertStyle : uint8_t;
enum LanguageId : uint8_t;
enum DataSourceMode : uint8_t;
enum SourceUiState : uint8_t;
enum TextId : uint8_t;
enum Tone : uint8_t;
struct VisualState;
struct SkinPalette;
struct AlertTimeWindow;
struct TouchCalibrationPoint;
enum FocusMetric : uint8_t;
struct SafeGlyph;

// Biblioteka jest wymagana w tej wersji.
// Nie uzywamy __has_include(), bo Arduino/arduino-cli moze nie dodac
// sciezki biblioteki do kompilacji, gdy include jest ukryty w warunku.
#define HAS_TOUCH 1

// ============================================================
// CYD WEATHER UI PRO
// Board: ESP32 Dev Module
// Display: 320x240
// ESP-NOW channel: 8
//
// Optional touch library:
//   XPT2046_Touchscreen by Paul Stoffregen
//
// Touch pins for ESP32-2432S028R:
//   CLK  25
//   MOSI 32
//   MISO 39
//   CS   33
//   IRQ  36
// ============================================================

#define WIFI_CHANNEL 8
uint8_t activeRadioChannel = WIFI_CHANNEL;
String activeWifiSsid;

// 0 = normalna praca (cichy Serial), 1 = pelna diagnostyka
#define CYD_DEBUG 0


// ============================================================
// CYD RGB LED - STATUS POGODY / LACZNOSCI
// LED aktywna stanem LOW.
// ============================================================

#define CYD_LED_ENABLE 1
#define CYD_LED_RED_PIN   4
#define CYD_LED_GREEN_PIN 16
#define CYD_LED_BLUE_PIN  17


// ============================================================
// CYD microSD - SD ASSETS
// Typowy ESP32-2432S028R: SD_CS = GPIO 5.
// SD pracuje na HSPI i przechowuje fonty/zasoby interfejsu.
// ============================================================
#define CYD_SD_ENABLE 1
#define CYD_SD_CS_PIN 5

bool sdReady = false;
uint64_t sdCardBytes = 0;
uint64_t sdUsedBytes = 0;
String sdTypeName = "NONE";

bool fontRegularReady = false;
bool fontBoldReady = false;
bool fontNumbersReady = false;
bool smoothFontSupported = false;
bool smoothFontRuntimeOK = false;

// RC15.134: po wielu PANIC LoadProhibited w drawGlyph/File::read
#define CYD_SD_SMOOTH_RUNTIME_ENABLE 0

// RC15.135 RETURN FREEZE:
// podczas waitTouchRelease UI nie wykonuje żadnych okresowych redrawów.
// Smooth font runtime pozostaje w kwarantannie.

// RC15.136 FOCUS TOUCH SPLIT:
// hitbox WYBOR odsuniety od MENU; routing i pozostale UI bez zmian.

// RC15.137 TOUCH MATRIX35 AFFINE:
// wspolrzedne dotyku liczone z modelu 35-punktowego zamiast starej
// kalibracji 5-punktowej z Preferences. Preferences pozostaja zapisane,
// ale nie sa uzywane przez readTouch() w tej wersji testowej.

// RC15.138 TOUCH GEOMETRY AUDIT:
// hitboxy przyciskow dopasowane do rzeczywistych ramek po MATRIX35.
// Bez zmian w filtrze dotyku, RETURN FREEZE i smooth-font quarantine.

// RC15.139A SAFE BUILTIN FONT7 FOCUS:
// duze liczby FOCUS korzystaja wylacznie z wbudowanego TFT_eSPI font 7.
// Brak FreeSans/Adafruit_GFX i brak runtime .vlw z SD.

// RC15.140 FOCUS BIG CHART FOUNDATION:
// FOCUS ma WYBOR / WYKRES / MENU; WYKRES otwiera duzy overlay 6H
// dla aktualnie wyswietlanej metryki. Bez nowego duzego bufora RAM.

// RC15.141 FOCUS CHART RANGES:
// duzy wykres ma 1H / 6H / 24H / POWROT.
// Zakres zmienia tylko overlay i korzysta z tego samego chartHistory.

// RC15.142 FOCUS CHART POLISH:
// pelniejsza siatka, srodkowa etykieta czasu, znacznik ostatniego punktu
// i kompaktowa aktualna wartosc. Touch/footer pozostaja bez zmian.

// RC15.142A COMPILE FIX:
// jawne (unsigned int) dla liczby miejsc po przecinku w String(float,...).
// Logika i wyglad RC15.142 bez zmian.

// RC15.143 FOCUS CHART VALUE SAFE ZONE:
// aktualna wartosc przeniesiona poza pole przebiegu, nad wykres.
// Nie moze juz zaslonic ostatniej kropki dla zadnej metryki.

// RC15.144 FOCUS CHART NO DUPLICATE VALUE:
// usunieta mala ramka z duplikatem aktualnego wyniku.
// Pozostaje duzy wynik nad wykresem + kropka ostatniego punktu.

// RC15.145 FOCUS CHART 7D:
// duzy wykres dostaje zakres 7D z istniejacych godzinowych chart7d/indoor7d.
// Format weather_history.bin i zapis SD pozostaja bez zmian.

// RC15.146 FOCUS WIND DIRECTION HISTORY:
// RC15.147 FOCUS WIND DIR STATS:
// RC15.148: czyste przejscie przez N/0/360 - bez dzielonej kreski
// miedzy dolna i gorna krawedzia wykresu kierunku.
// wykres kierunku pokazuje dominujacy kierunek (srednia kolowa)
// oraz stabilnosc kierunku 0..100% dla aktualnie wybranego zakresu.
// osobny sidecar SD dla kierunku wiatru + FOCUS 1H/6H/24H/7D.
// weather_history.bin pozostaje binarnie zgodny; przejscie 359/0 stopni jest rysowane przez krawedzie.

// Docelowy system fontow jezykowych.
// Male fonty UI sa osobne dla PL/EN/DE/CZ, aby kazdy plik zawieral
// tylko potrzebne glify i byl lekki dla ESP32.
bool langFontRegularReady[4] = {false, false, false, false};
bool langFontBoldReady[4]    = {false, false, false, false};
bool langFontSmallReady[4]   = {false, false, false, false};

// TFT_eSPI smooth font loader uses a path WITHOUT ".vlw".
// Example: "CYD/fonts/ui_regular" -> "/CYD/fonts/ui_regular.vlw".





void scanFontAssets();
String languageFontCode(uint8_t lang);
String languageFontPath(uint8_t lang, bool bold);
String languageSmallFontPath(uint8_t lang);
bool languageFontReady(uint8_t lang, bool bold);
bool languageSmallFontReady(uint8_t lang);
bool useLanguageUiFont(bool bold);
bool useLanguageSmallFont();
bool useUiRegularFont();
bool useUiBoldFont();
bool useNumbersLargeFont();
void drawLangHeader(const String &textUtf8, const String &fallbackAscii,
                    int x, int y, uint16_t fg, uint16_t bg);
void drawLangButtonCentered(const String &textUtf8, const String &fallbackAscii,
                            int cx, int cy, uint16_t fg, uint16_t bg, bool bold);
void drawLangLabel(const String &textUtf8, const String &fallbackAscii,
                   int x, int y, uint16_t fg, uint16_t bg, bool bold = false);
void drawMenuButtonLoadedFont(int x, int y, int w, int h,
                              const String &label, const String &value,
                              bool smoothLoaded);
void drawDiagnosticsStatic();
void refreshDiagnosticsValues();
void refreshHeaderStatusOnly();
void refreshSimpleFooterRxOnly();
void refreshDataFreshnessFrame();






enum LedWeatherLevel : uint8_t {
  LED_LEVEL_OK = 0,
  LED_LEVEL_INFO = 1,
  LED_LEVEL_WARNING = 2,
  LED_LEVEL_DANGER = 3,
  LED_LEVEL_OFFLINE = 4
};


// ============================================================
// ============================================================
// RC15.256 - FINAL GLOBAL COMPASS
// - w trybie GLOBAL także osie kompasu używają standardu N/E/S/W,
// - w trybie LOKALNY osie kompasu pozostają zgodne z językiem UI,
// - reszta logiki i wyglądu bez zmian.
// - wersja finalna.
//
// RC15.255 - WIND DIR MODE + I18N MENU AUDIT
// - niezależny tryb oznaczeń kierunku wiatru: GLOBAL / LOKALNY,
// - GLOBAL = klasyczne 16 sektorów N/NNE/NE/ENE/E/... niezależnie od języka UI,
// - LOKALNY = PL/EN/DE/CZ zgodnie z aktualnym językiem,
// - ustawienie zapisane w NVS (cyd_ui / wind_dir),
// - MENU 2/2 ma trzeci przełącznik kierunku wiatru,
// - audyt mieszania języków: FOCUS footer, alert style, MENU 2/2 i źródło,
// - zaakceptowane fonty PL, kompas, dane, SD, sieć, touch, AIO i ESP-NOW bez zmian.
//
// RC15.254D - COMPASS PN SYMMETRY + USTAW FIX
// - górna etykieta kompasu (PN/N/etc.) ma taki sam wizualny odstęp od okręgu
//   jak dolna etykieta (PD/S/etc.), z uwzględnieniem wysokości fontu,
// - FOCUS: USTAW. -> USTAW bez kropki,
// - hitboxy, ramki, kompas, dane, SD, sieć, touch, AIO i ESP-NOW bez zmian.
//
// RC15.254C - SMALL/MEDIUM Ł + PN + ONOFF FIX
// - duże Ł w najmniejszym foncie: przekreślenie zaczyna się na pionowej belce,
// - duże Ł w średnim foncie: krótsze i naturalniej osadzone przekreślenie,
// - WŁ./WYŁ. -> WŁ/WYŁ bez kropek,
// - PN w kompasie minimalnie wyżej (2 px),
// - pozostałe diakrytyki, fonty, dane i geometria UI bez zmian.
//
// RC15.254B - FONT SCALE + POLISH MENU
// - znaki narodowe maja 3 geometrie: tiny / medium / large,
// - font 2 dostaje osobne, subtelne akcenty Ź/Ó/Ś/Ć i krótkie Ł,
// - font 1 (CIŚNIENIE) pozostaje bez zmian,
// - FOCUS footer: MENU -> USTAW. i szerszy przycisk,
// - polskie ON/OFF -> WŁ./WYŁ. w ustawieniach,
// - techniczne funkcje i geometria działających ekranów bez zmian.
//
// RC15.254A - UI GEOMETRY + DIACRITICS AUDIT FIX
// - naprawione WYKRES/MENU w FOCUS footer: po WYBÓR przywracany MC_DATUM,
// - Ł ma krótskie, bardziej proporcjonalne przekreślenie,
// - większe acute (Ś/Ź/Ć/Ń) delikatniejsze; font 1 CIŚNIENIE bez zmian,
// - większy ogonek Ą/Ę krótszy,
// - FOCUS-WYBÓR nie usuwa już polskich znaków z nazw metryk,
// - etykiety wykresów w PL/DE/CZ używają bezpiecznego renderera zamiast CISN fallback,
// - bez zmian kompasu, danych, SD, sieci, touch, AIO i ESP-NOW.
//
// RC15.254 - FULL POLISH UI AUDIT
// - pełne polskie słowa tam, gdzie geometria pozwala,
// - statusy komfortu: BARDZO ... zamiast skrótów B....,
// - CIŚNIENIE zamiast CIŚN/CISN na wykresach, gdzie mieści się pełna nazwa,
// - poprawione: WYBÓR, POWRÓT, GŁÓWNA, ZA MAŁO DANYCH, Dotknij krzyżyka,
// - FOCUS status tag korzysta z bezpiecznego renderera UTF-8,
// - polskie opisy trendu ciśnienia używają poprawnych znaków,
// - techniczne nazwy ESP-NOW/HTTP/AIO/SD/CSV/NTP pozostają bez tłumaczenia.
// - logika danych, SD, sieć, touch, AIO, ESP-NOW bez zmian.
//
// RC15.253D - FOCUS VECTOR I18N
// - FOCUS kierunku wraca do duzego, grubego stylu wektorowego jak dawniej,
// - renderer rozszerzony z N/E/S/W o P/D/Z/O/V/J,
// - obsluga 1/2/3-literowych skrotow kierunku,
// - PL/EN/DE/CZ korzystaja z tego samego stylu FOCUS,
// - POWRÓT, CIŚNIENIE, WSKAŹNIKI i kompas bez zmian.
//
// RC15.253C - FOCUS SIZE + ACCENT POLISH
// - kierunek w FOCUS jest wiekszy i wizualnie blizszy staremu wygladowi,
// - akcenty nad Ó oraz przy polskich wielkich literach w wiekszych fontach
//   sa mniejsze i nizej, aby POWRÓT/WSKAŹNIKI wygladaly jak CIŚNIENIE,
// - maly font 1 oraz kompas pozostaja bez zmian.
//
// RC15.253B - COMPASS / FOCUS / Ó ACUTE FIX
// - kompas przesuniety 7 px w lewo, aby prawa etykieta osi nie wchodzila pod panel,
// - srodkowy polski kierunek w kompasie ma mniejszy, staly font 2,
// - FOCUS dla PL/DE/CZ uzywa zwyklego adaptacyjnego fontu zamiast
//   starego wektorowego N/E/S/W (ktory nie umial rysowac P/Z/V/J/O),
// - akcent nad Ś i innymi literami wraca do wygladu RC15.252B,
// - tylko Ó/ó dostaje osobny, nizszy akcent dla POWRÓT.
// - CIŚNIENIE w malym foncie pozostaje bez zmian.
//
// RC15.253A - WIND DIR LAYOUT + ACCENT FIX
// - skrót kierunku w kafelkach i kompasie dobiera font do dostępnej szerokości,
// - osie polskiego kompasu: PN / PD / Z / W, wszystkie tym samym fontem,
// - kreska nad Ó w większym foncie obniżona o kolejne 2 px,
// - mały font 1 (np. CIŚNIENIE) pozostaje bez zmian.
//
// RC15.253 - WIND DIRECTION I18N + ACCENT TUNE
// - kierunki wiatru lokalizowane wg PL/EN/DE/CZ,
// - PL uzywa 8 czytelnych skrotow: PN, PNW, W, PDW, PD, PDZ, Z, PNZ;
//   dokladne stopnie pozostaja wyswietlane osobno,
// - akcent acute w wiekszych fontach obnizony blizej litery (np. POWRÓT),
// - maly font 1 pozostaje bez zmian, aby nie zepsuc poprawnego CIŚNIENIE.
//
// RC15.252B - SMALL DIACRITICS VISIBILITY FIX
// - znaki diakrytyczne w malym wbudowanym foncie sa rysowane NAD glifem,
//   grubiej i z wiekszym kontrastem; szczegolnie Ś w CIŚNIENIE.
// - bez zmian geometrii kafelkow, partial refresh i danych.
//
// RC15.252A - COMPILE FIX
// - usuniety zduplikowany case 0x87 w dekoderze UTF-8.
// - brak zmian logiki renderowania.
//
// RC15.252 - SAFE I18N RENDERER PL/DE/CZ
// - centralny renderer UTF-8 bez runtime .vlw z SD,
// - obsluga polskich, niemieckich i czeskich znakow narodowych,
// - drawLocalizedLabelAuto(), drawSmallInfo(), FOCUS i etykiety wykresow
//   nie omijaja juz lokalizacji przy smooth-font quarantine,
// - fallback ASCII pozostaje tylko awaryjny dla nieobslugiwanych znakow.
// - siec, SD, historia, touch, AIO i ESP-NOW bez zmian.
//
// RC15.251A - Arduino IDE 1.8.x SCOPE FIX
// - forward declaration struct PlGlyph przed prototypami Arduino.
// - brak zmian logiki renderera i i18n.
//
// RC15.251 - SAFE PL FONT + MENU I18N
// - polskie znaki na TFT bez runtime .vlw z SD (smooth-font quarantine pozostaje),
// - lekki renderer UTF-8 dla polskich znakow na bazie bezpiecznych fontow TFT_eSPI,
// - menu 1/2 odzyskuje lokalizowane etykiety zamiast pustych pol,
// - nazwy skinow w menu sa lokalizowane PL/EN/DE/CZ.
// - siec, SD historia, ESP-NOW, AIO, touch i CONFIG AUTH bez zmian.
//
// RC15.250 - SECURITY STEP 2 / CONFIG AUTH
// - /wifi i /aio wymagaja HTTP Basic Auth w normalnej sieci.
// - login: admin; haslo: aktualne haslo HOME zapisane w NVS.
// - w CYD-Setup autoryzacja jest wylaczona, captive portal dziala jak dotychczas.
//
// RC15.248 - SECURITY STEP 1 / NO SECRETS
// - prywatne SSID/hasla Wi-Fi i AIO Key usuniete z fallbackow kodu.
// - istniejace wartosci w NVS pozostaja bez zmian.
// - po pustym NVS CYD uruchamia CYD-Setup.
//
// RC15.247 - AIO NVS CONFIG
// - Adafruit IO username/key/feed ladowane z Preferences "cyd_aio".
// - dotychczasowe dane w kodzie pozostaja fallbackiem dla zgodnosci po aktualizacji.
// - /aio udostepnia konfigurator; klucz AIO nigdy nie jest wyswietlany.
// - puste pole klucza zachowuje obecny klucz.
// - polling AIO, HOME/REMOTE, ESP-NOW, CYD-Setup i historia SD bez zmian.
// ============================================================

// ============================================================
// RC15.246 - SETUP AP + CAPTIVE PORTAL
// - gdy HOME i REMOTE sa niedostepne przy starcie, CYD uruchamia AP CYD-Setup,
// - AP pracuje na kanale 8, aby LOCAL ESP-NOW mogl dalej odbierac LilyGO,
// - captive DNS kieruje telefon/komputer na strone konfiguracji,
// - manager HOME/REMOTE jest wstrzymany tylko podczas aktywnego CYD-Setup,
// - zapis /wifi/save konczy sie restartem i ponowna proba HOME/REMOTE.
// ============================================================

// RC15.243 - WIFI NVS FOUNDATION
// - HOME i REMOTE maja wartosci runtime ladowane z Preferences "cyd_wifi".
// - Obecne dane w kodzie zostaja tylko jako fallback przy pierwszym uruchomieniu.
// - Nie zmieniamy jeszcze UI, captive portal, dotyku, SD ani ESP-NOW.
// - RC15.245: dodana strona /wifi do konfiguracji HOME/REMOTE i zapisu do NVS.
// ============================================================

// ============================================================
// INTERNET / NTP - FUNDAMENT POD PRZYSZLA SIEC STACJI
// Domyslnie WYLACZONE. ESP-NOW pozostaje zrodlem lokalnym.
// CYD polaczy sie tylko z Wi-Fi na tym samym kanale co ESP-NOW.
// ============================================================

#define CYD_INTERNET_ENABLE 1

// ============================================================
// RC15.92 - DWA STALE PROFILE WIFI
// ============================================================
// Wartosci fabryczne sa tylko fallbackiem przy pustym NVS.
const char* CYD_WIFI_HOME_SSID_FALLBACK = "";
const char* CYD_WIFI_HOME_PASS_FALLBACK = "";
// Faktyczny SSID nadawany przez hotspot zawiera koncowa spacje.
const char* CYD_WIFI_REMOTE_SSID_FALLBACK = "";
const char* CYD_WIFI_REMOTE_PASS_FALLBACK = "";

// Aktywne dane runtime. Nastepny etap doda ich zmiane przez WWW/captive portal.
String cydWifiHomeSsid;
String cydWifiHomePass;
String cydWifiRemoteSsid;
String cydWifiRemotePass;

// Adafruit IO - wartosci fabryczne sa tylko fallbackiem przy pustym NVS.
const char* CYD_AIO_USERNAME_FALLBACK = "";
const char* CYD_AIO_KEY_FALLBACK      = "";
const char* CYD_AIO_FEED_FALLBACK     = "garni-live";

String cydAioUsername = CYD_AIO_USERNAME_FALLBACK;
String cydAioKey      = CYD_AIO_KEY_FALLBACK;
String cydAioFeed     = CYD_AIO_FEED_FALLBACK;

void loadWifiConfigNvs() {
  Preferences p;
  p.begin("cyd_wifi", true);
  cydWifiHomeSsid   = p.getString("home_ssid", CYD_WIFI_HOME_SSID_FALLBACK);
  cydWifiHomePass   = p.getString("home_pass", CYD_WIFI_HOME_PASS_FALLBACK);
  cydWifiRemoteSsid = p.getString("rem_ssid", CYD_WIFI_REMOTE_SSID_FALLBACK);
  cydWifiRemotePass = p.getString("rem_pass", CYD_WIFI_REMOTE_PASS_FALLBACK);
  p.end();

  if (cydWifiHomeSsid.length() == 0) cydWifiHomeSsid = CYD_WIFI_HOME_SSID_FALLBACK;
  if (cydWifiRemoteSsid.length() == 0) cydWifiRemoteSsid = CYD_WIFI_REMOTE_SSID_FALLBACK;

  Serial.print("[NET-CFG] NVS HOME=");
  Serial.print(cydWifiHomeSsid);
  Serial.print(" REMOTE=");
  Serial.println(cydWifiRemoteSsid);
}


void loadAioConfigNvs() {
  Preferences p;
  p.begin("cyd_aio", true);
  cydAioUsername = p.getString("user", CYD_AIO_USERNAME_FALLBACK);
  cydAioKey      = p.getString("key", CYD_AIO_KEY_FALLBACK);
  cydAioFeed     = p.getString("feed", CYD_AIO_FEED_FALLBACK);
  p.end();

  // Username/feed nie moga byc puste, bo wtedy URL AIO bylby niepoprawny.
  if (cydAioUsername.length() == 0) cydAioUsername = CYD_AIO_USERNAME_FALLBACK;
  if (cydAioFeed.length() == 0) cydAioFeed = CYD_AIO_FEED_FALLBACK;

  Serial.print("[AIO-CFG] NVS user=");
  Serial.print(cydAioUsername);
  Serial.print(" key=");
  Serial.print(cydAioKey.length() ? "SET" : "EMPTY");
  Serial.print(" feed=");
  Serial.println(cydAioFeed);
}


WebServer cydConfigServer(80);
DNSServer cydDnsServer;
bool cydConfigServerStarted = false;
bool cydSetupApMode = false;

const char* CYD_SETUP_AP_SSID = "CYD-Setup";
const char* CYD_SETUP_AP_PASS = "garni1234";

bool ensureCydConfigAuth() {
  // W awaryjnym CYD-Setup konfigurator musi pozostac dostepny bez logowania.
  if (cydSetupApMode) return true;

  // W normalnej sieci uzywamy hasla HOME juz zapisanego w NVS.
  // Nie tworzymy ani nie zapisujemy dodatkowego sekretu w firmware.
  if (cydWifiHomePass.length() == 0) {
    cydConfigServer.send(503, "text/plain; charset=utf-8",
      "Konfiguracja WWW zablokowana: brak hasla HOME w NVS.");
    return false;
  }

  if (cydConfigServer.authenticate("admin", cydWifiHomePass.c_str())) return true;

  Serial.println("[SEC] CONFIG AUTH required");
  cydConfigServer.requestAuthentication();
  return false;
}

String htmlEscape(const String &s) {
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

void sendWifiConfigPage(const String &message = "") {
  if (!ensureCydConfigAuth()) return;
  String h;
  h.reserve(2600);
  h += F("<!doctype html><html><head><meta charset='utf-8'>");
  h += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>CYD-Weather Wi-Fi</title><style>");
  h += F("body{font-family:Arial,sans-serif;max-width:620px;margin:24px auto;padding:0 14px;background:#111;color:#eee}");
  h += F("h1{font-size:24px}fieldset{border:1px solid #555;border-radius:10px;margin:16px 0;padding:14px}");
  h += F("label{display:block;margin-top:10px}input{box-sizing:border-box;width:100%;padding:10px;margin-top:4px;border-radius:7px;border:1px solid #777}");
  h += F("button{padding:11px 18px;border:0;border-radius:8px;font-weight:bold}small{color:#aaa}.ok{padding:10px;background:#173b20;border-radius:8px}");
  h += F("</style></head><body><h1>CYD-Weather - Wi-Fi</h1>");
  if (message.length()) {
    h += F("<div class='ok'>");
    h += htmlEscape(message);
    h += F("</div>");
  }
  h += F("<form method='POST' action='/wifi/save'>");
  h += F("<fieldset><legend>HOME</legend><label>SSID<input name='home_ssid' maxlength='32' value='");
  h += htmlEscape(cydWifiHomeSsid);
  h += F("'></label><label>Haslo<input type='password' name='home_pass' maxlength='63' placeholder='pozostaw puste aby nie zmieniac'></label></fieldset>");
  h += F("<fieldset><legend>REMOTE</legend><label>SSID<input name='rem_ssid' maxlength='32' value='");
  h += htmlEscape(cydWifiRemoteSsid);
  h += F("'></label><label>Haslo<input type='password' name='rem_pass' maxlength='63' placeholder='pozostaw puste aby nie zmieniac'></label></fieldset>");
  h += F("<button type='submit'>Zapisz i uruchom ponownie</button></form>");
  h += F("<p><small>Hasla nie sa wyswietlane. Puste pole hasla zachowuje dotychczasowe haslo.</small></p>");
  h += F("<p><a href='/aio' style='color:#7fd7ff'>Adafruit IO</a></p>");
  h += F("</body></html>");
  cydConfigServer.send(200, "text/html; charset=utf-8", h);
}

void handleWifiConfigSave() {
  if (!ensureCydConfigAuth()) return;
  if (!cydConfigServer.hasArg("home_ssid") || !cydConfigServer.hasArg("rem_ssid")) {
    cydConfigServer.send(400, "text/plain; charset=utf-8", "Brak wymaganych pol.");
    return;
  }

  String newHomeSsid = cydConfigServer.arg("home_ssid");
  String newRemoteSsid = cydConfigServer.arg("rem_ssid");
  String newHomePass = cydConfigServer.arg("home_pass");
  String newRemotePass = cydConfigServer.arg("rem_pass");

  if (newHomeSsid.length() == 0 || newHomeSsid.length() > 32 ||
      newRemoteSsid.length() == 0 || newRemoteSsid.length() > 32 ||
      newHomePass.length() > 63 || newRemotePass.length() > 63) {
    cydConfigServer.send(400, "text/plain; charset=utf-8", "Nieprawidlowe dane Wi-Fi.");
    return;
  }

  Preferences p;
  if (!p.begin("cyd_wifi", false)) {
    cydConfigServer.send(500, "text/plain; charset=utf-8", "Blad otwarcia NVS.");
    return;
  }

  bool ok = true;
  ok &= p.putString("home_ssid", newHomeSsid) > 0;
  ok &= p.putString("rem_ssid", newRemoteSsid) > 0;
  if (newHomePass.length()) ok &= p.putString("home_pass", newHomePass) > 0;
  if (newRemotePass.length()) ok &= p.putString("rem_pass", newRemotePass) > 0;
  p.end();

  if (!ok) {
    cydConfigServer.send(500, "text/plain; charset=utf-8", "Blad zapisu NVS.");
    return;
  }

  Serial.print("[NET-CFG] SAVED HOME=");
  Serial.print(newHomeSsid);
  Serial.print(" REMOTE=");
  Serial.println(newRemoteSsid);
  cydConfigServer.send(200, "text/html; charset=utf-8",
    "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<body style='font-family:Arial;background:#111;color:#eee;padding:24px'>"
    "<h2>Zapisano Wi-Fi</h2><p>CYD uruchomi sie ponownie.</p></body>");
  delay(700);
  ESP.restart();
}

void sendAioConfigPage(const String &message = "") {
  if (!ensureCydConfigAuth()) return;
  String h;
  h.reserve(2500);
  h += F("<!doctype html><html><head><meta charset='utf-8'>");
  h += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>CYD-Weather Adafruit IO</title><style>");
  h += F("body{font-family:Arial,sans-serif;max-width:620px;margin:24px auto;padding:0 14px;background:#111;color:#eee}");
  h += F("h1{font-size:24px}fieldset{border:1px solid #555;border-radius:10px;margin:16px 0;padding:14px}");
  h += F("label{display:block;margin-top:10px}input{box-sizing:border-box;width:100%;padding:10px;margin-top:4px;border-radius:7px;border:1px solid #777}");
  h += F("button{padding:11px 18px;border:0;border-radius:8px;font-weight:bold}small{color:#aaa}.ok{padding:10px;background:#173b20;border-radius:8px}a{color:#7fd7ff}");
  h += F("</style></head><body><h1>CYD-Weather - Adafruit IO</h1>");
  if (message.length()) {
    h += F("<div class='ok'>");
    h += htmlEscape(message);
    h += F("</div>");
  }
  h += F("<form method='POST' action='/aio/save'><fieldset><legend>Adafruit IO</legend>");
  h += F("<label>Username<input name='user' maxlength='64' value='");
  h += htmlEscape(cydAioUsername);
  h += F("'></label>");
  h += F("<label>AIO Key<input type='password' name='key' maxlength='128' placeholder='pozostaw puste aby nie zmieniac'></label>");
  h += F("<label>Feed<input name='feed' maxlength='64' value='");
  h += htmlEscape(cydAioFeed);
  h += F("'></label></fieldset>");
  h += F("<button type='submit'>Zapisz i uruchom ponownie</button></form>");
  h += F("<p><small>Klucz AIO nie jest wyswietlany. Puste pole zachowuje dotychczasowy klucz.</small></p>");
  h += F("<p><a href='/wifi'>Wi-Fi</a></p></body></html>");
  cydConfigServer.send(200, "text/html; charset=utf-8", h);
}

void handleAioConfigSave() {
  if (!ensureCydConfigAuth()) return;
  if (!cydConfigServer.hasArg("user") || !cydConfigServer.hasArg("feed")) {
    cydConfigServer.send(400, "text/plain; charset=utf-8", "Brak wymaganych pol.");
    return;
  }

  String newUser = cydConfigServer.arg("user");
  String newKey  = cydConfigServer.arg("key");
  String newFeed = cydConfigServer.arg("feed");
  newUser.trim();
  newFeed.trim();

  if (newUser.length() == 0 || newUser.length() > 64 ||
      newFeed.length() == 0 || newFeed.length() > 64 ||
      newKey.length() > 128) {
    cydConfigServer.send(400, "text/plain; charset=utf-8", "Nieprawidlowe dane Adafruit IO.");
    return;
  }

  Preferences p;
  if (!p.begin("cyd_aio", false)) {
    cydConfigServer.send(500, "text/plain; charset=utf-8", "Blad otwarcia NVS.");
    return;
  }

  bool ok = true;
  ok &= p.putString("user", newUser) > 0;
  ok &= p.putString("feed", newFeed) > 0;
  if (newKey.length()) ok &= p.putString("key", newKey) > 0;
  p.end();

  if (!ok) {
    cydConfigServer.send(500, "text/plain; charset=utf-8", "Blad zapisu NVS.");
    return;
  }

  Serial.print("[AIO-CFG] SAVED user=");
  Serial.print(newUser);
  Serial.print(" key=");
  Serial.print(newKey.length() ? "UPDATED" : "KEEP");
  Serial.print(" feed=");
  Serial.println(newFeed);

  cydConfigServer.send(200, "text/html; charset=utf-8",
    "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<body style='font-family:Arial;background:#111;color:#eee;padding:24px'>"
    "<h2>Zapisano Adafruit IO</h2><p>CYD uruchomi sie ponownie.</p></body>");
  delay(700);
  ESP.restart();
}

void startCydSetupAp() {
  if (cydSetupApMode) return;

  Serial.println("[NET-CFG] HOME/REMOTE unavailable -> SETUP AP");

  // AP_STA zostawia interfejs STA dostepny dla ESP-NOW, ale nie uruchamiamy
  // w tle reconnect managera dopoki konfigurator jest aktywny.
  WiFi.disconnect(false, false);
  delay(50);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  IPAddress apIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);

  bool cfgOk = WiFi.softAPConfig(apIP, gateway, subnet);
  bool apOk = WiFi.softAP(CYD_SETUP_AP_SSID, CYD_SETUP_AP_PASS, WIFI_CHANNEL);

  bool dnsOk = false;
  if (apOk) {
    cydDnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsOk = cydDnsServer.start(53, "*", WiFi.softAPIP());
  }

  cydSetupApMode = apOk;

  // setupInternetSafe() juz przed tym miejscem ustawia stan OFFLINE:
  // internetConnected=false, internetChannel=0, WIFI_PROFILE_NONE.
  // Nie dotykamy tych zmiennych tutaj, bo sa deklarowane nizej w duzym .ino.
  activeRadioChannel = WIFI_CHANNEL;
  activeWifiSsid = "";

  Serial.print("[NET-CFG] SETUP AP=");
  Serial.print(apOk ? "OK " : "FAIL ");
  Serial.print(CYD_SETUP_AP_SSID);
  Serial.print(" IP=");
  Serial.print(WiFi.softAPIP());
  Serial.print(" CFG=");
  Serial.print(cfgOk ? "OK" : "FAIL");
  Serial.print(" DNS=");
  Serial.println(dnsOk ? "OK" : "FAIL");
}

void startWifiConfigWeb() {
  if (cydConfigServerStarted) return;

  cydConfigServer.on("/", HTTP_GET, []() { sendWifiConfigPage(); });
  cydConfigServer.on("/wifi", HTTP_GET, []() { sendWifiConfigPage(); });
  cydConfigServer.on("/wifi/save", HTTP_POST, handleWifiConfigSave);
  cydConfigServer.on("/aio", HTTP_GET, []() { sendAioConfigPage(); });
  cydConfigServer.on("/aio/save", HTTP_POST, handleAioConfigSave);

  // Typowe testy captive portal: Android / Apple / Windows.
  cydConfigServer.on("/generate_204", HTTP_GET, []() { sendWifiConfigPage(); });
  cydConfigServer.on("/gen_204", HTTP_GET, []() { sendWifiConfigPage(); });
  cydConfigServer.on("/hotspot-detect.html", HTTP_GET, []() { sendWifiConfigPage(); });
  cydConfigServer.on("/library/test/success.html", HTTP_GET, []() { sendWifiConfigPage(); });
  cydConfigServer.on("/connecttest.txt", HTTP_GET, []() { sendWifiConfigPage(); });
  cydConfigServer.on("/ncsi.txt", HTTP_GET, []() { sendWifiConfigPage(); });
  cydConfigServer.on("/fwlink", HTTP_GET, []() { sendWifiConfigPage(); });

  cydConfigServer.onNotFound([]() {
    if (cydSetupApMode) {
      sendWifiConfigPage();
    } else {
      cydConfigServer.sendHeader("Location", "/wifi", true);
      cydConfigServer.send(302, "text/plain", "");
    }
  });

  cydConfigServer.begin();
  cydConfigServerStarted = true;

  Serial.print("[NET-CFG] WWW=http://");
  if (cydSetupApMode)
    Serial.print(WiFi.softAPIP());
  else
    Serial.print(WiFi.localIP());
  Serial.println("/wifi");
}

void serviceWifiConfigWeb() {
  if (cydSetupApMode)
    cydDnsServer.processNextRequest();

  if (cydConfigServerStarted)
    cydConfigServer.handleClient();
}

enum WifiProfile : uint8_t {
  WIFI_PROFILE_NONE = 0,
  WIFI_PROFILE_HOME,
  WIFI_PROFILE_REMOTE
};

WifiProfile activeWifiProfile = WIFI_PROFILE_NONE;
unsigned long lastWifiRoamCheckMs = 0;
const uint32_t WIFI_ROAM_CHECK_MS = 60000UL;
bool wifiRoamScanPending = false;
unsigned long wifiRoamScanStartedMs = 0;
const uint32_t WIFI_ROAM_SCAN_TIMEOUT_MS = 12000UL;
unsigned long wifiKnownHomeUntilMs = 0;
const uint32_t WIFI_KNOWN_HOME_MS = 30000UL;
bool wifiHomeRoamInProgress = false;
unsigned long wifiHomeRoamStartedMs = 0;
const uint32_t WIFI_HOME_ROAM_TIMEOUT_MS = 30000UL;

// RC15.250: gdy podczas normalnej pracy znikna jednoczesnie HOME i REMOTE,
// nie probuj bez konca przelaczac profili. Po 90 s bez polaczenia uruchom
// awaryjny CYD-Setup. Przy starcie brak sieci nadal obsluguje setup().
unsigned long wifiOfflineSinceMs = 0;
const uint32_t WIFI_RUNTIME_SETUP_AP_TIMEOUT_MS = 90000UL;


// Adafruit IO - jeden feed przechowuje caly JSON /live-data jako "value".
// Dane dostepowe sa ladowane z NVS przez loadAioConfigNvs().

const uint32_t INTERNET_POLL_MS = 15000UL;
const uint32_t INTERNET_CONNECT_RETRY_MS = 30000UL;
const uint32_t INTERNET_HTTP_TIMEOUT_MS = 5000UL;
const uint16_t INTERNET_MAX_PAYLOAD = 320;

const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.google.com";

bool internetConnected = false;
bool ntpReady = false;

// RC15.237: aktualizowane tylko przez callback prawdziwej synchronizacji SNTP.
// uint32_t zachowuje poprawne liczenie wieku takze przez rollover millis().
volatile uint32_t lastNtpSyncMs = 0;
volatile uint32_t lastNtpSyncEpoch = 0;
volatile bool ntpSyncSeen = false;

unsigned long lastNtpRetryMs = 0;
const uint32_t NTP_RETRY_MS = 30000UL;
uint8_t internetChannel = 0;

unsigned long lastInternetPollMs = 0;
unsigned long lastInternetConnectTryMs = 0;
bool wifiReconnectPending = false;
unsigned long wifiReconnectStartedMs = 0;
const uint32_t WIFI_RECONNECT_ATTEMPT_TIMEOUT_MS = 15000UL;
uint32_t internetHttpOK = 0;
uint32_t internetHttpErrors = 0;
int lastInternetHttpCode = 0;
String lastInternetError = "OFF";
String lastInternetRecordId = "";

// Standard CYD backlight pin.
// Do NOT use GPIO15 as backlight - on classic CYD it is TFT_CS.
#define TFT_BL 21

#define SCREEN_W 320
#define SCREEN_H 240

// Touch
#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25
#define TOUCH_CS   33

// Common raw calibration range for classic CYD.
// If touch is mirrored on your unit, change FLIP_X / FLIP_Y below.
#define TOUCH_RAW_MIN_X 200
#define TOUCH_RAW_MAX_X 3700
#define TOUCH_RAW_MIN_Y 240
#define TOUCH_RAW_MAX_Y 3800

#define TOUCH_FLIP_X 0
#define TOUCH_FLIP_Y 0

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;

#if HAS_TOUCH
SPIClass touchSPI = SPIClass(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
#endif

SPIClass sdSPI = SPIClass(HSPI);

// ============================================================
// DATA PACKET - must remain identical to LilyGO
// ============================================================

struct __attribute__((packed)) WeatherPacket {
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
};

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


// LOCAL = pakiety ESP-NOW z nadajnika w domu.
// INTERNET = osobny bufor przyszlego backendu.
WeatherPacket liveData = {};
WeatherPacket pendingLocalData = {};
WeatherPacket internetData = {};
portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

volatile bool newData = false;
volatile uint32_t packetCount = 0;
volatile uint32_t localBadPacketCount = 0;
volatile uint32_t foreignEspNowPacketCount = 0;
volatile int lastPacketLen = 0;

static const uint8_t LILYGO_ESPNOW_MAC[6] = {
  0xF0, 0x24, 0xF9, 0xAE, 0x1D, 0x04
};

bool haveData = false;
unsigned long lastPacketMs = 0;

bool haveInternetData = false;
unsigned long lastInternetPacketMs = 0;
uint32_t internetPacketCount = 0;
bool newInternetData = false;

TaskHandle_t internetHttpTaskHandle = nullptr;
portMUX_TYPE internetHttpMux = portMUX_INITIALIZER_UNLOCKED;

volatile bool internetHttpRequest = false;
volatile bool internetHttpBusy = false;
volatile bool internetHttpDone = false;
volatile bool internetHttpOk = false;

WeatherPacket internetHttpData = {};
char internetHttpRecordId[48] = {0};


// Internet uznajemy za swiezy tylko po faktycznym odebraniu danych,
// nie po samym polaczeniu Wi-Fi.
const uint32_t INTERNET_SOURCE_TIMEOUT_S = 90;

// ============================================================
// HISTORY / CHARTS - 24H RAM + TRWALY ZAPIS NA SD
// 288 probek x 5 min = 24 godziny widoczne po restarcie.
// Pelna historia jest dopisywana do pliku binarnego na SD.
// ============================================================

#define CHART_HISTORY_POINTS 288
#define CHART_SAMPLE_MS 300000UL
#define CHART_HISTORY_FILE "/CYD/data/weather_history.bin"
#define CHART_RECORD_MAGIC 0x43594448UL   // "CYDH"
#define CHART_RECORD_VERSION 1

// Ochrona karty: historia rosnie bardzo wolno (~5.5 MB/rok),
// wiec nie kasujemy starych danych. Wstrzymujemy zapis dopiero,
// gdy na karcie zostanie mniej niz 32 MB wolnego miejsca.
#define CHART_MIN_FREE_BYTES (32ULL * 1024ULL * 1024ULL)

struct ChartSample {
  uint32_t epoch;
  float temp;
  float hum;
  float press;
  float wind;
  float gust;
  float uv;
  float rain;
  float lux;

  // Dane wewnetrzne sa przechowywane w osobnym pliku SD,
  // aby nie zmieniac zgodnego formatu weather_history.bin.
  float tempIn;
  float humIn;
};

struct __attribute__((packed)) ChartDiskRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t sequence;
  uint32_t epoch;
  float temp;
  float hum;
  float press;
  float wind;
  float gust;
  float uv;
  float rain;
  float lux;
  uint32_t checksum;
};

#define INDOOR_HISTORY_FILE "/CYD/data/indoor_history.bin"
#define WEATHER_CSV_FILE "/CYD/data/weather_history.csv"
#define INDOOR_RECORD_MAGIC 0x494E4448UL  // "INDH"
#define INDOOR_RECORD_VERSION 1

struct __attribute__((packed)) IndoorDiskRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t epoch;
  float tempIn;
  float humIn;
  uint32_t checksum;
};

// RC15.146: kierunek wiatru ma osobny, zgodny wstecznie sidecar.
// Nie zmieniamy ChartDiskRecord ani weather_history.bin.
#define WIND_DIR_HISTORY_FILE "/CYD/data/wind_direction_history.bin"
#define WIND_DIR_RECORD_MAGIC 0x57444952UL  // "WDIR"
#define WIND_DIR_RECORD_VERSION 1

struct __attribute__((packed)) WindDirDiskRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t epoch;
  float directionDeg;
  uint32_t checksum;
};

struct WindDirSample {
  uint32_t epoch;
  float directionDeg;
};

// RC15.231: maly journal transakcji historii. Istniejace formaty archiwow bez zmian.
#define HISTORY_BUNDLE_JOURNAL_FILE "/CYD/data/history_bundle.jrn"
#define HISTORY_BUNDLE_JOURNAL_MAGIC 0x48424A52UL  // "HBJR"
#define HISTORY_BUNDLE_JOURNAL_VERSION 1

struct __attribute__((packed)) HistoryBundleJournal {
  uint32_t magic;
  uint16_t version;
  uint16_t flags;
  uint32_t sequence;
  uint32_t epoch;
  WeatherPacket data;
  uint32_t checksum;
};

#define HBJ_FLAG_DIR_REQUIRED 0x0001

ChartSample chartHistory[CHART_HISTORY_POINTS];
uint16_t chartHead = 0;
uint16_t chartCount = 0;
unsigned long lastChartSampleMs = (unsigned long)(0UL - CHART_SAMPLE_MS);
uint32_t chartSequence = 0;

// RC15.204: czas ostatniego zatwierdzonego rekordu MAIN na SD.
// Uzywany do zachowania rzeczywistego rytmu 5 min po restarcie.
uint32_t lastCommittedHistoryEpoch = 0;

// RC15.229: kotwica monotoniczna dla ochrony przed absurdalnym skokiem NTP do przodu.
// Epoch moze normalnie wyprzedzic millis o niewielka korekte, ale nie o godziny/dni.
uint32_t historyClockAnchorEpoch = 0;
uint32_t historyClockAnchorMs = 0;
uint32_t historyClockJumpBlocks = 0;
#define HISTORY_MAX_FORWARD_JUMP_SEC 3600UL

uint32_t chartSdWritesOK = 0;
uint32_t chartSdWriteErrors = 0;
uint64_t chartHistoryFileBytes = 0;
uint64_t indoorHistoryFileBytes = 0;
uint32_t indoorSdWritesOK = 0;
uint32_t indoorSdWriteErrors = 0;

// Spójność pojedynczego cyklu zapisu MAIN + WEW + CSV.
uint32_t historyBundleWritesOK = 0;
uint32_t historyBundleWriteErrors = 0;
uint32_t historyBundleRecoveries = 0;
uint32_t historyBundleJournalErrors = 0;

uint64_t weatherCsvFileBytes = 0;
uint32_t weatherCsvWritesOK = 0;
uint32_t weatherCsvWriteErrors = 0;

uint32_t historyCheckScanned = 0;
uint32_t historyCheckBad = 0;
uint32_t historySequenceGaps = 0;
uint32_t indoorCheckScanned = 0;
uint32_t indoorCheckBad = 0;
bool historyTailAligned = true;
bool indoorTailAligned = true;
bool csvTailOK = true;
bool archiveIntegrityOK = true;

// RC15.200: synchronizacja czasowa koncowki MAIN <-> INDOOR.
uint32_t historyTailEpoch = 0;
uint32_t indoorTailEpoch = 0;
bool archiveTailEpochSync = true;

// RC15.202: integralnosc sidecara kierunku wiatru.
uint32_t windDirCheckScanned = 0;
uint32_t windDirCheckBad = 0;
bool windDirTailAligned = true;
uint32_t windDirTailEpoch = 0;
bool windDirTailEpochSync = true;

bool indoorBootstrapPending = false;
bool chartHistoryLoaded = false;
bool chartSdPausedLowSpace = false;

// RC15.233: most z serviceSDHotplug() do lokalnych flag loop().
bool sdHotplugRamSyncAdded = false;

// RC15.236: recovery pending journala wykonane wewnatrz commitHistoryBundleSD()
// moze dodac starszy punkt do RAM przed zapisem biezacego bundle.
bool historyPendingRamSyncAdded = false;

enum ChartRange : uint8_t {
  CHART_RANGE_1H = 0,
  CHART_RANGE_6H = 1,
  CHART_RANGE_24H = 2,
  CHART_RANGE_7D = 3
};

uint8_t chartRange = CHART_RANGE_24H;

enum ChartPage : uint8_t {
  CHART_PAGE_BASIC = 0,
  CHART_PAGE_EXTRA = 1,
  CHART_PAGE_INOUT = 2,
  CHART_PAGE_COMFORT = 3,
  CHART_PAGE_TREND = 4,
  CHART_PAGE_STATS = 5,
  CHART_PAGE_ALERTS = 6
};

uint8_t chartPage = CHART_PAGE_BASIC;

#define CHART_7D_POINTS 168
ChartSample chart7d[CHART_7D_POINTS];
uint16_t chart7dCount = 0;
bool chart7dLoaded = false;

// RC15.207: jak kompletne sa godzinowe koszyki 7D.
// FULL = 12/12, PART = 6..11, LOW = 1..5.
uint16_t chart7dFullHours = 0;
uint16_t chart7dPartialHours = 0;
uint16_t chart7dLowHours = 0;
uint8_t chart7dSamples[CHART_7D_POINTS] = {};

ChartSample indoor7d[CHART_7D_POINTS];
uint16_t indoor7dCount = 0;
bool indoor7dLoaded = false;
uint16_t indoor7dFullHours = 0;
uint16_t indoor7dPartialHours = 0;
uint16_t indoor7dLowHours = 0;
uint8_t indoor7dSamples[CHART_7D_POINTS] = {};

// RC15.146: 24H kierunku w RAM + godzinowy widok 7D.
WindDirSample windDirHistory[CHART_HISTORY_POINTS];
uint16_t windDirHead = 0;
uint16_t windDirCount = 0;
WindDirSample windDir7d[CHART_7D_POINTS];
uint16_t windDir7dCount = 0;
bool windDir7dLoaded = false;
uint16_t windDir7dFullHours = 0;
uint16_t windDir7dPartialHours = 0;
uint16_t windDir7dLowHours = 0;
uint8_t windDir7dSamples[CHART_7D_POINTS] = {};
uint64_t windDirHistoryFileBytes = 0;
uint32_t windDirSdWritesOK = 0;
uint32_t windDirSdWriteErrors = 0;


// ============================================================
// SETTINGS
// ============================================================

enum SkinId : uint8_t {
  SKIN_GRID = 0,
  SKIN_DASH = 1,
  SKIN_MINIMAL = 2,
  SKIN_COMPASS = 3,
  SKIN_INSTRUMENT = 4,
  SKIN_FOCUS = 5,
  SKIN_CHARTS = 6
};

enum AlertStyle : uint8_t {
  ALERT_VALUE = 0,   // only value color
  ALERT_BORDER = 1,  // value + border
  ALERT_PANEL = 2    // value + border + subtle panel fill
};

enum LanguageId : uint8_t {
  LANG_PL = 0,
  LANG_EN = 1,
  LANG_DE = 2,
  LANG_CZ = 3
};

uint8_t currentLanguage = LANG_PL;

enum WindDirNotationMode : uint8_t {
  WIND_DIR_LOCAL = 0,
  WIND_DIR_GLOBAL = 1
};

// Niezależne od języka UI. Domyślnie GLOBAL = klasyczne N/E/S/W.
uint8_t windDirNotationMode = WIND_DIR_GLOBAL;

enum DataSourceMode : uint8_t {
  SOURCE_AUTO = 0,
  SOURCE_LOCAL = 1,
  SOURCE_INTERNET = 2
};

enum SourceUiState : uint8_t {
  SOURCE_UI_WAIT = 0,
  SOURCE_UI_LIVE,
  SOURCE_UI_STALE,
  SOURCE_UI_OFFLINE
};

uint8_t dataSourceMode = SOURCE_AUTO;
uint8_t activeDataSource = SOURCE_LOCAL;

// Po tylu sekundach bez pakietu ESP-NOW tryb AUTO uznaje lokalne dane za niedostepne.
const uint32_t LOCAL_SOURCE_TIMEOUT_S = 30;







// Prosty system lokalizacji.
// Wszystkie teksty interfejsu przechodza przez tr().
enum TextId : uint8_t {
  TXT_WEATHER_GRID,
  TXT_WEATHER_DASH,
  TXT_WEATHER_MINIMAL,
  TXT_WIND_COMPASS,
  TXT_WEATHER_INSTRUMENT,
  TXT_TEMP_OUT,
  TXT_TEMP_IN,
  TXT_HUM_OUT,
  TXT_HUM_IN,
  TXT_PRESSURE,
  TXT_WIND,
  TXT_GUST,
  TXT_DIRECTION,
  TXT_LIGHT,
  TXT_RAIN,
  TXT_SETTINGS,
  TXT_SKIN,
  TXT_ALERTS,
  TXT_BRIGHTNESS,
  TXT_AUTO_INOUT,
  TXT_CHANGE,
  TXT_VIEW_NOW,
  TXT_BACK,
  TXT_LANGUAGE,
  TXT_SOURCE,
  TXT_LIVE,
  TXT_WAIT,
  TXT_STALE,
  TXT_OFFLINE,
  TXT_OK,
  TXT_COLD,
  TXT_VERY_COLD,
  TXT_WARM,
  TXT_HOT,
  TXT_VERY_HOT,
  TXT_DRY,
  TXT_VERY_DRY,
  TXT_HUMID,
  TXT_VERY_HUMID,
  TXT_LOW,
  TXT_VERY_LOW,
  TXT_HIGH,
  TXT_VERY_HIGH,
  TXT_STRONG,
  TXT_VERY_STRONG,
  TXT_MEDIUM,
  TXT_EXTREME,
  TXT_NIGHT,
  TXT_DARK,
  TXT_DAY,
  TXT_SUN,
  TXT_BRIGHT,
  TXT_RAINING,
  TXT_HEAVY_RAIN
};

// Typy musza byc przed pierwsza funkcja dla Arduino IDE 1.8.x.
enum Tone : uint8_t {
  TONE_NEUTRAL = 0,
  TONE_OK,
  TONE_INFO,
  TONE_COLD,
  TONE_VERY_COLD,
  TONE_WARM,
  TONE_HOT,
  TONE_WARNING,
  TONE_DANGER,
  TONE_DIM,
  TONE_BRIGHT,
  TONE_RAIN
};

struct VisualState {
  Tone tone;
  uint8_t severity;   // 0..3
  const char* tag;
};

struct SkinPalette {
  uint16_t bg;
  uint16_t panel;
  uint16_t panel2;
  uint16_t text;
  uint16_t muted;
  uint16_t accent;
  uint16_t border;
  uint16_t footer;
};


// Jawne prototypy: omijamy problem automatycznego generatora prototypow
// w Arduino IDE 1.8.19 przy funkcjach korzystajacych z wlasnych typow.
SkinPalette paletteFor(uint8_t skin);
uint16_t toneColor(Tone tone);
uint16_t softFillForTone(Tone tone, uint16_t fallback);

VisualState stateTemperature(float v);
VisualState stateHumidity(float v);
VisualState statePressure(float v);
VisualState stateWind(float v, bool gust);
VisualState stateUV(float v);
VisualState stateLux(float v);
VisualState stateRain(float v);

void drawStateDot(int x, int y, VisualState st);
void drawSmallInfo(int x, int y, int w, int h,
                   const String &label, const String &value, VisualState st);
void drawGaugeBar(int x, int y, int w, int h,
                  float value, float minV, float maxV,
                  VisualState st, const String &label, const String &valueText);

String dataSourceModeName();
String activeDataSourceName();
SourceUiState activeSourceUiState();
String activeSourceUiText();
uint16_t activeSourceUiColor(const SkinPalette &p);
bool sourceUsingFallbackLocal();
String activeSourceUiMark();
void updateActiveDataSource();

String formatBytesShort(uint64_t bytes);
void initCYDSD();
void serviceSDHotplug();
bool syncHistoryTailStateFromSD(const char *tag);
bool loadSmoothFontSD(const String &fontBasePath);
void unloadSmoothFontSafe();
void scanFontAssets();

extern bool rgbEnabled;
extern bool freshnessFrameEnabled;
void rgbLedWrite(bool redOn, bool greenOn, bool blueOn);
uint8_t maxWeatherSeverity(const WeatherPacket &d);
uint8_t focusDisplayedWeatherSeverity(const WeatherPacket &d);
uint8_t currentFocusMetric();
extern uint8_t lastDrawnFocusMetric;
extern uint8_t currentSkin;
LedWeatherLevel currentLedLevel();
void serviceStatusLed();

WeatherPacket snapshotLocalData();
WeatherPacket snapshotInternetData();
WeatherPacket snapshotData();
void drawCharts(const WeatherPacket &d);
void refreshChartsFooterDynamic();
void refreshChartsRxOnly();
void drawChartsPageIndicator();
void pushChartSample(const WeatherPacket &d, uint32_t epoch = 0);
bool historySampleDue(uint32_t nowMs, uint32_t nowEpoch);
bool historyEpochTrajectoryOK(uint32_t nowMs, uint32_t nowEpoch);
void printHistoryTimeDiagnostics(const char *tag);
int chartIndexOldest(int logicalIndex);
uint32_t chartRecordChecksum(const ChartDiskRecord &rec);
bool appendChartSampleSD(const WeatherPacket &d, uint32_t epoch);
void loadChartHistorySD();
uint16_t chartRangeSamples();
String chartRangeName();
String chartRangeLeftLabel();
String chartEffectiveLeftLabel(uint16_t visibleCount);
String chartMidLabel(uint16_t visibleCount);
String chartTrendSymbol(float firstV, float lastV, uint8_t metric);
String chartNowText();
bool pressureTendency3H(float &deltaOut, uint16_t &samplesOut);
String pressureTendency3HText(float delta);
String pressureTendency3HLabel(float delta);
String weatherChangeSignal(const WeatherPacket &d, float pressure3h);
uint16_t weatherChangeSignalColor(const WeatherPacket &d, float pressure3h, const SkinPalette &p);
void drawFocusPressureContext(const WeatherPacket &d);
void drawFocusWindContext(const WeatherPacket &d, bool gustView);
void drawFocusHumidityContext(const WeatherPacket &d, bool indoorView);
void drawFocusTemperatureContext(const WeatherPacket &d, bool indoorView);
void drawFocusRainContext(const WeatherPacket &d);
uint16_t pressureTendency3HColor(float delta, const SkinPalette &p);
uint8_t chartRangeFillPercent();
uint8_t chartRangeFillPercentCurrentView(bool indoorView);
float dewPointC(float tempC, float rh);
float absoluteHumidityGM3(float tempC, float rh);
bool chartMetricUsesIndoor(uint8_t metric);
String moistureCompareLabel(float ahOut, float ahIn);
uint16_t moistureCompareColor(float ahOut, float ahIn);
String ventilationAdvice(float ahOut, float ahIn);
uint16_t ventilationAdviceColor(float ahOut, float ahIn);

bool pressureTendency3H(float &deltaOut, uint16_t &samplesOut) {
  deltaOut = NAN;
  samplesOut = 0;

  if (chartCount < 2)
    return false;

  const uint32_t windowSec = 3UL * 60UL * 60UL;

  uint32_t newestEpoch = 0;
  for (uint16_t i = 0; i < chartCount; ++i) {
    int idx = chartIndexOldest(i);
    const ChartSample &s = chartHistory[idx];
    if (s.epoch > newestEpoch)
      newestEpoch = s.epoch;
  }

  float firstP = NAN;
  float lastP = NAN;

  if (newestEpoch > 100000UL) {
    uint32_t axisEnd = historyEpochNow();
    if (axisEnd <= 100000UL || axisEnd < newestEpoch)
      axisEnd = newestEpoch;

    uint32_t axisStart = (axisEnd > windowSec) ? (axisEnd - windowSec) : 0;

    for (uint16_t i = 0; i < chartCount; ++i) {
      int idx = chartIndexOldest(i);
      const ChartSample &s = chartHistory[idx];

      if (s.epoch <= 100000UL ||
          s.epoch < axisStart ||
          s.epoch > axisEnd ||
          !isfinite(s.press))
        continue;

      if (!isfinite(firstP))
        firstP = s.press;

      lastP = s.press;
      samplesOut++;
    }
  } else {
    uint16_t n = chartCount;
    if (n > 36)
      n = 36;

    uint16_t logicalStart = chartCount - n;

    for (uint16_t i = 0; i < n; ++i) {
      int idx = chartIndexOldest(logicalStart + i);
      const ChartSample &s = chartHistory[idx];

      if (!isfinite(s.press))
        continue;

      if (!isfinite(firstP))
        firstP = s.press;

      lastP = s.press;
      samplesOut++;
    }
  }

  if (samplesOut < 2 || !isfinite(firstP) || !isfinite(lastP))
    return false;

  deltaOut = lastP - firstP;
  return isfinite(deltaOut);
}

String pressureTendency3HText(float delta) {
  if (!isfinite(delta))
    return "3H --";

  String s = "3H ";
  s += fmtSigned(delta, 1);
  s += "hPa ";

  // RC15.187: znaki ASCII sa bezpieczne dla wbudowanego fontu TFT_eSPI.
  // /^ = wyrazny wzrost, / = wzrost, > = stabilnie, \\ = spadek, v = wyrazny spadek.
  if (delta >= 1.5f)       s += "^";
  else if (delta >= 0.5f)  s += "/";
  else if (delta <= -1.5f) s += "v";
  else if (delta <= -0.5f) s += "\\";
  else                     s += ">";

  return s;
}

String pressureTendency3HLabel(float delta) {
  if (!isfinite(delta))
    return "--";

  if (currentLanguage == LANG_EN) {
    if (delta >= 1.5f)  return "PRESS RISING FAST";
    if (delta >= 0.5f)  return "PRESS RISING";
    if (delta <= -1.5f) return "PRESS FALLING FAST";
    if (delta <= -0.5f) return "PRESS FALLING";
    return "PRESS STABLE";
  }

  if (currentLanguage == LANG_DE) {
    if (delta >= 1.5f)  return "DRUCK STEIGT SCHN";
    if (delta >= 0.5f)  return "DRUCK STEIGT";
    if (delta <= -1.5f) return "DRUCK FAELLT SCHN";
    if (delta <= -0.5f) return "DRUCK FAELLT";
    return "DRUCK STABIL";
  }

  if (currentLanguage == LANG_CZ) {
    if (delta >= 1.5f)  return "TLAK RYCHLE STOUPA";
    if (delta >= 0.5f)  return "TLAK STOUPA";
    if (delta <= -1.5f) return "TLAK RYCHLE KLESA";
    if (delta <= -0.5f) return "TLAK KLESA";
    return "TLAK STABIL";
  }

  if (delta >= 1.5f)  return "CIŚN. SZYBKO ROŚNIE";
  if (delta >= 0.5f)  return "CIŚN. ROŚNIE";
  if (delta <= -1.5f) return "CIŚN. SZYBKO SPADA";
  if (delta <= -0.5f) return "CIŚN. SPADA";
  return "CIŚN. STABILNE";
}

String weatherChangeSignal(const WeatherPacket &d, float pressure3h) {
  if (!isfinite(pressure3h) ||
      !isfinite(d.wilgotnosc) ||
      !isfinite(d.opady_godzina))
    return "--";

  // Najpierw faktyczny opad - to nie jest prognoza, tylko stan teraz.
  if (d.opady_godzina >= 0.2f) {
    if (currentLanguage == LANG_EN) return "RAIN NOW";
    if (currentLanguage == LANG_DE) return "REGEN JETZT";
    if (currentLanguage == LANG_CZ) return "DEST TED";
    return "TERAZ OPAD";
  }

  // Spadajace cisnienie + wysoka wilgotnosc = ostrozny sygnal pogorszenia.
  if (pressure3h <= -1.0f && d.wilgotnosc >= 75.0f) {
    if (currentLanguage == LANG_EN) return "WORSE POSSIBLE";
    if (currentLanguage == LANG_DE) return "VERSCHL MOEGL";
    if (currentLanguage == LANG_CZ) return "ZHORSENI MOZNE";
    return "MOŻLIWE POGORSZENIE";
  }

  // Wyrazny wzrost cisnienia i brak bardzo wysokiej wilgotnosci.
  if (pressure3h >= 1.0f && d.wilgotnosc <= 85.0f) {
    if (currentLanguage == LANG_EN) return "BETTER POSSIBLE";
    if (currentLanguage == LANG_DE) return "BESSER MOEGL";
    if (currentLanguage == LANG_CZ) return "ZLEPSENI MOZNE";
    return "MOŻLIWA POPRAWA";
  }

  if (currentLanguage == LANG_EN) return "CONDITIONS STABLE";
  if (currentLanguage == LANG_DE) return "LAGE STABIL";
  if (currentLanguage == LANG_CZ) return "PODMINKY STABIL";
  return "WARUNKI STABILNE";
}

uint16_t weatherChangeSignalColor(const WeatherPacket &d, float pressure3h, const SkinPalette &p) {
  if (!isfinite(pressure3h) ||
      !isfinite(d.wilgotnosc) ||
      !isfinite(d.opady_godzina))
    return p.muted;

  if (d.opady_godzina >= 0.2f)
    return toneColor(TONE_RAIN);

  if (pressure3h <= -1.0f && d.wilgotnosc >= 75.0f)
    return toneColor(TONE_WARNING);

  if (pressure3h >= 1.0f && d.wilgotnosc <= 85.0f)
    return toneColor(TONE_OK);

  return p.muted;
}

uint16_t pressureTendency3HColor(float delta, const SkinPalette &p) {
  if (!isfinite(delta))
    return p.muted;

  if (delta >= 0.5f)
    return toneColor(TONE_OK);

  if (delta <= -0.5f)
    return toneColor(TONE_WARNING);

  return p.muted;
}

bool chartMetricStats(uint8_t metric,
                      float &firstV,
                      float &lastV,
                      float &avgV,
                      float &minV,
                      float &maxV,
                      uint16_t &countV);
void drawTrendCard(int x, int y, int w, int h, const String &label, uint8_t metric, const String &unit);
void drawStatsCard(int x, int y, int w, int h, const String &label, uint8_t metric, const String &unit);
void drawWindStatsCard(int x, int y, int w, int h);
void redrawStatsPageOnly();
void redrawTrendPageOnly();
void drawAlertRow(int y, const String &label, const String &value, Tone tone);
void drawAlertsPage(const WeatherPacket &d);
struct AlertTimeWindow {
  bool timeAxisAvailable;
  uint32_t axisStart;
  uint32_t axisEnd;
};

AlertTimeWindow alertTimeWindowForMetric(uint8_t metric);
bool alertSampleVisibleInTime(uint32_t epoch, const AlertTimeWindow &w);
bool alert7dSampleQualityOK(uint8_t metric, uint16_t sourceIndex);

VisualState alertVisualState(uint8_t metric, float v, bool gustMode);
bool alertStateIsActive(uint8_t metric, const VisualState &st);

uint16_t countAlertSamples(uint8_t metric, bool gustMode = false);
uint16_t visibleAlertSampleCount(uint8_t metric);
uint16_t currentAlertStreak(uint8_t metric, bool gustMode = false);
String alertStreakText(uint16_t streak);
uint16_t countAlertEpisodes(uint8_t metric, bool gustMode = false);
uint16_t longestAlertStreak(uint8_t metric, bool gustMode = false);
bool alertSampleGap(uint32_t prevEpoch, uint32_t currEpoch);
Tone worstAlertToneForRange(String &labelOut, String &valueOut);
void nextChartRange();
bool loadChart7DFromSD();
String chartPageName();
void nextChartPage();
uint32_t indoorRecordChecksum(const IndoorDiskRecord &rec);
bool appendIndoorSampleSD(const WeatherPacket &d, uint32_t epoch);
void loadIndoor24HFromSD();
bool loadIndoor7DFromSD();
uint32_t windDirRecordChecksum(const WindDirDiskRecord &rec);
float normalizeWindDirDeg(float deg);
void pushWindDirSample(float directionDeg, uint32_t epoch = 0);
int windDirIndexOldest(int logicalIndex);
bool appendWindDirSampleSD(float directionDeg, uint32_t epoch);
void loadWindDir24HFromSD();
bool loadWindDir7DFromSD();
bool appendWeatherCSV(const WeatherPacket &d, uint32_t epoch, uint32_t sequence);
uint32_t historyBundleJournalChecksum(const HistoryBundleJournal &j);
bool writeHistoryBundleJournal(const WeatherPacket &d, uint32_t epoch, uint32_t sequence);
bool recoverHistoryBundleJournal(WeatherPacket *committedData = nullptr,
                                 uint32_t *committedEpoch = nullptr,
                                 bool countRecovery = true);
bool commitHistoryBundleSD(const WeatherPacket &candidate, uint32_t epoch,
                           WeatherPacket &committedData, uint32_t &committedEpoch);
void initWeatherCSVInfo();
void checkArchiveIntegrity();
bool repairBinaryTailSafe(const char *path, size_t recSize, const char *tag,
                          const char *tmpPath, const char *bakPath);
bool repairCsvTailSafe(const char *path, const char *tmpPath, const char *bakPath);

// Implementacje historii sa celowo DOPIERO tutaj.
// Arduino IDE 1.8.x generuje automatyczne prototypy przed pierwsza funkcja.
// Gdy te funkcje sa wyzej, generator potrafi wstawic prototypy zanim istnieja
// TextId / VisualState i wywolac lawine bledow kompilacji.

uint32_t chartRecordChecksum(const ChartDiskRecord &rec) {
  // FNV-1a po calym rekordzie z pominieciem pola checksum.
  const uint8_t *p = reinterpret_cast<const uint8_t*>(&rec);
  const size_t n = sizeof(ChartDiskRecord) - sizeof(uint32_t);
  uint32_t h = 2166136261UL;

  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619UL;
  }

  return h;
}

// RC15.252: centralny renderer etykiet lokalizowanych.
void drawLocalizedLabelAuto(const String &label, int x, int y, uint8_t fallbackFont = 1) {
  bool hasUtf8 = false;
  for (size_t i = 0; i < label.length(); ++i) {
    if ((uint8_t)label[i] >= 0x80) {
      hasUtf8 = true;
      break;
    }
  }

  if (hasUtf8 && useLanguageUiFont(false)) {
    tft.drawString(label, x, y);
    unloadSmoothFontSafe();
    return;
  }

  tft.drawString(label, x, y, fallbackFont);
}
void drawMenuLocalizedSmallCentered(const String &utf8,
                                    const String &fallbackAscii,
                                    int cx, int cy,
                                    uint16_t fg, uint16_t bg) {
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);

  if (useLanguageSmallFont()) {
    tft.drawString(utf8, cx, cy);
    unloadSmoothFontSafe();
  } else {
    tft.drawCentreString(fallbackAscii, cx, cy - 4, 1);
  }

  tft.setTextDatum(TL_DATUM);
}




uint16_t currentAlertStreak(uint8_t metric, bool gustMode) {
  bool indoorMetric = chartMetricUsesIndoor(metric);
  bool use7d = (chartRange == CHART_RANGE_7D);

  if (use7d) {
    if (indoorMetric && !indoor7dLoaded)
      loadIndoor7DFromSD();
    else if (!indoorMetric && !chart7dLoaded)
      loadChart7DFromSD();
  }

  uint16_t sourceCount;
  if (use7d)
    sourceCount = indoorMetric ? indoor7dCount : chart7dCount;
  else
    sourceCount = chartCount;

  if (sourceCount == 0)
    return 0;

  uint16_t wanted = chartRangeSamples();
  uint16_t visibleCount = sourceCount;
  if (visibleCount > wanted)
    visibleCount = wanted;

  uint16_t logicalStart = sourceCount - visibleCount;
  AlertTimeWindow tw = alertTimeWindowForMetric(metric);
  uint16_t streak = 0;
  uint32_t newerEpoch = 0;

  // Liczymy od najnowszej próbki wstecz aż do pierwszej próbki bez alertu
  // albo do przerwy w historii.
  for (int i = (int)visibleCount - 1; i >= 0; i--) {
    float v;
    uint32_t epoch = 0;

    if (use7d) {
      const ChartSample &s = indoorMetric ?
                             indoor7d[logicalStart + i] :
                             chart7d[logicalStart + i];
      v = chartSampleValue(s, metric);
      epoch = s.epoch;
    } else {
      int idx = chartIndexOldest(logicalStart + i);
      const ChartSample &s = chartHistory[idx];
      v = chartSampleValue(s, metric);
      epoch = s.epoch;
    }

    if (!alertSampleVisibleInTime(epoch, tw))
      break;

    if (use7d && !alert7dSampleQualityOK(metric, logicalStart + i))
      break;

    if (newerEpoch > 100000UL &&
        epoch > 100000UL &&
        alertSampleGap(epoch, newerEpoch))
      break;

    if (epoch > 100000UL)
      newerEpoch = epoch;

    if (!isfinite(v))
      break;

    VisualState ast = alertVisualState(metric, v, gustMode);

    if (alertStateIsActive(metric, ast))
      streak++;
    else
      break;
  }

  return streak;
}

String alertStreakText(uint16_t streak) {
  if (streak == 0)
    return "T:0";

  if (chartRange == CHART_RANGE_7D) {
    // Punkty 7D są agregowane godzinowo.
    return "T:" + String(streak) + "h";
  }

  uint32_t minutes = (uint32_t)streak * 5UL;

  if (minutes < 60UL)
    return "T:" + String(minutes) + "m";

  uint32_t hours = minutes / 60UL;
  uint32_t remM = minutes % 60UL;

  if (remM == 0)
    return "T:" + String(hours) + "h";

  return "T:" + String(hours) + "h" + String(remM) + "m";
}


String ventilationAdvice(float ahOut, float ahIn) {
  if (!isfinite(ahOut) || !isfinite(ahIn))
    return "--";

  float d = ahIn - ahOut;

  // To jest wyłącznie wskazówka wilgotnościowa:
  // dodatnie d oznacza, że na zewnątrz jest mniej pary wodnej.
  if (d > 1.0f)
    return "WIETRZ";
  if (d < -1.0f)
    return "NIE WIETRZ";
  return "OBOJETNE";
}

uint16_t ventilationAdviceColor(float ahOut, float ahIn) {
  if (!isfinite(ahOut) || !isfinite(ahIn))
    return TFT_DARKGREY;

  float d = ahIn - ahOut;

  if (d > 1.0f)
    return toneColor(TONE_OK);
  if (d < -1.0f)
    return toneColor(TONE_WARNING);
  return toneColor(TONE_INFO);
}


String moistureCompareLabel(float ahOut, float ahIn) {
  if (!isfinite(ahOut) || !isfinite(ahIn))
    return "--";

  float d = ahIn - ahOut;

  if (d > 0.5f)
    return "ZEW SUCHSZE";
  if (d < -0.5f)
    return "ZEW WILG.";
  return "PODOBNIE";
}

uint16_t moistureCompareColor(float ahOut, float ahIn) {
  if (!isfinite(ahOut) || !isfinite(ahIn))
    return TFT_DARKGREY;

  float d = ahIn - ahOut;

  if (d > 0.5f)
    return toneColor(TONE_OK);
  if (d < -0.5f)
    return toneColor(TONE_WARNING);
  return toneColor(TONE_INFO);
}


float dewPointC(float tempC, float rh) {
  if (!isfinite(tempC) || !isfinite(rh) || rh <= 0.0f)
    return NAN;

  if (rh > 100.0f)
    rh = 100.0f;

  // Wzór Magnusa, wystarczająco dokładny dla zakresu pogodowego.
  const float a = 17.62f;
  const float b = 243.12f;
  float gamma = log(rh / 100.0f) + (a * tempC) / (b + tempC);
  return (b * gamma) / (a - gamma);
}

float absoluteHumidityGM3(float tempC, float rh) {
  if (!isfinite(tempC) || !isfinite(rh) || rh < 0.0f)
    return NAN;

  if (rh > 100.0f)
    rh = 100.0f;

  // Wilgotność bezwzględna [g/m3].
  float es = 6.112f * exp((17.67f * tempC) / (tempC + 243.5f));
  float e = (rh / 100.0f) * es;
  return (216.7f * e) / (273.15f + tempC);
}

bool chartMetricUsesIndoor(uint8_t metric) {
  return metric == 8 || metric == 9 || metric == 11 || metric == 13;
}


uint8_t chartRangeFillPercent() {
  uint16_t wanted = chartRangeSamples();
  if (wanted == 0) return 0;

  uint16_t actual = (chartRange == CHART_RANGE_7D)
                      ? chart7dReliableHourCount(false)
                      : chartCount;
  if (actual > wanted) actual = wanted;

  uint32_t pct = ((uint32_t)actual * 100UL) / wanted;
  if (pct > 100UL) pct = 100UL;
  return (uint8_t)pct;
}

uint8_t chartRangeFillPercentCurrentView(bool indoorView) {
  uint16_t wanted = chartRangeSamples();
  if (wanted == 0)
    return 0;

  uint16_t actual;

  // Helper nie odwołuje się bezpośrednio do showIndoor, bo ta zmienna
  // jest zadeklarowana niżej w pliku. Stan widoku przekazujemy parametrem.
  if (chartRange == CHART_RANGE_7D) {
    bool useIndoorCoverage =
        (chartPage == CHART_PAGE_STATS || chartPage == CHART_PAGE_TREND) &&
        indoorView;
    actual = chart7dReliableHourCount(useIndoorCoverage);
  } else {
    actual = chartCount;
  }

  if (actual > wanted)
    actual = wanted;

  uint32_t pct = ((uint32_t)actual * 100UL) / wanted;
  if (pct > 100UL)
    pct = 100UL;

  return (uint8_t)pct;
}



void pushChartSample(const WeatherPacket &d, uint32_t epoch) {
  ChartSample &s = chartHistory[chartHead];

  s.epoch = epoch;
  s.temp  = d.temperatura;
  s.hum   = d.wilgotnosc;
  s.press = d.cisnienie;
  s.wind  = d.predkosc_wiatru;
  s.gust  = d.poryw_wiatru;
  s.uv    = d.uv_index;
  s.rain  = d.opady_godzina;
  s.lux   = d.swiatlo_lux;
  s.tempIn = d.temp_wewnetrzna;
  s.humIn  = d.wilg_wewnetrzna;

  chartHead = (chartHead + 1) % CHART_HISTORY_POINTS;
  if (chartCount < CHART_HISTORY_POINTS)
    chartCount++;
}

int chartIndexOldest(int logicalIndex) {
  int start = (chartHead + CHART_HISTORY_POINTS - chartCount) % CHART_HISTORY_POINTS;
  return (start + logicalIndex) % CHART_HISTORY_POINTS;
}

uint16_t chartRangeSamples() {
  switch (chartRange) {
    case CHART_RANGE_1H:  return 12;
    case CHART_RANGE_6H:  return 72;
    case CHART_RANGE_24H: return 288;
    default:              return CHART_7D_POINTS;
  }
}

uint16_t chart7dReliableHourCount(bool indoorView) {
  uint16_t count = indoorView ? indoor7dCount : chart7dCount;
  uint16_t reliable = 0;
  for (uint16_t i = 0; i < count; ++i) {
    uint8_t q = indoorView ? indoor7dSamples[i] : chart7dSamples[i];
    if (q >= 6)
      reliable++;
  }
  return reliable;
}

String chartRangeName() {
  switch (chartRange) {
    case CHART_RANGE_1H: return "1H";
    case CHART_RANGE_6H: return "6H";
    case CHART_RANGE_24H: return "24H";
    default: return "7D";
  }
}

String chartRangeLeftLabel() {
  switch (chartRange) {
    case CHART_RANGE_1H: return "-1H";
    case CHART_RANGE_6H: return "-6H";
    case CHART_RANGE_24H: return "-24H";
    default: return "-7D";
  }
}

String chartEffectiveLeftLabel(uint16_t visibleCount) {
  if (visibleCount == 0)
    return "--";

  // 7D ma punkty godzinowe po agregacji.
  if (chartRange == CHART_RANGE_7D) {
    if (visibleCount <= 1)
      return "START";

    uint16_t hours = visibleCount - 1;

    if (hours < 24)
      return "-" + String(hours) + "H";

    uint16_t days = hours / 24;
    uint16_t remH = hours % 24;

    if (remH == 0)
      return "-" + String(days) + "D";

    return "-" + String(days) + "D" + String(remH) + "H";
  }

  // 1H/6H/24H: jedna próbka co 5 minut.
  if (visibleCount <= 1)
    return "START";

  uint32_t minutes = (uint32_t)(visibleCount - 1) * 5UL;

  if (minutes < 60UL)
    return "-" + String(minutes) + "M";

  uint32_t hours = minutes / 60UL;
  uint32_t remM = minutes % 60UL;

  if (remM == 0)
    return "-" + String(hours) + "H";

  return "-" + String(hours) + "H" + String(remM) + "M";
}

String chartMidLabel(uint16_t visibleCount) {
  if (visibleCount < 3)
    return "";

  // Liczymy połowę rzeczywistego zebranego zakresu.
  if (chartRange == CHART_RANGE_7D) {
    uint16_t hours = (visibleCount - 1) / 2;
    if (hours == 0)
      return "";
    if (hours < 24)
      return "-" + String(hours) + "H";

    uint16_t days = hours / 24;
    uint16_t remH = hours % 24;
    if (remH == 0)
      return "-" + String(days) + "D";
    return "-" + String(days) + "D" + String(remH) + "H";
  }

  uint32_t minutes = ((uint32_t)(visibleCount - 1) * 5UL) / 2UL;
  if (minutes == 0)
    return "";
  if (minutes < 60UL)
    return "-" + String(minutes) + "M";

  uint32_t hours = minutes / 60UL;
  uint32_t remM = minutes % 60UL;
  if (remM == 0)
    return "-" + String(hours) + "H";
  return "-" + String(hours) + "H" + String(remM) + "M";
}

String chartTrendSymbol(float firstV, float lastV, uint8_t metric) {
  if (!isfinite(firstV) || !isfinite(lastV))
    return "";

  float diff = lastV - firstV;
  float eps = 0.1f;

  // Progi dobrane do sensownej zmiany dla danego parametru.
  if (metric == 1 || metric == 9) eps = 1.0f;      // wilgotnosc %
  else if (metric == 2) eps = 0.5f;                // hPa
  else if (metric == 3 || metric == 4) eps = 0.2f; // wiatr
  else if (metric == 5) eps = 0.2f;                // UV
  else if (metric == 6) eps = 0.1f;                // opad
  else if (metric == 7) eps = 50.0f;               // lux
  else if (metric == 8) eps = 0.1f;                // temp wew
  else if (metric == 10 || metric == 11) eps = 0.1f; // punkt rosy
  else if (metric == 12 || metric == 13) eps = 0.2f; // g/m3

  if (diff > eps) return "+";
  if (diff < -eps) return "-";
  return "=";
}


void nextChartRange() {
  if (chartRange == CHART_RANGE_1H)
    chartRange = CHART_RANGE_6H;
  else if (chartRange == CHART_RANGE_6H)
    chartRange = CHART_RANGE_24H;
  else if (chartRange == CHART_RANGE_24H) {
    chartRange = CHART_RANGE_7D;
    chart7dLoaded = false;
  } else
    chartRange = CHART_RANGE_1H;
}

String chartPageName() {
  if (chartPage == CHART_PAGE_EXTRA) return "EXTRA";
  if (chartPage == CHART_PAGE_INOUT) return "WEW";
  if (chartPage == CHART_PAGE_COMFORT) return "KOMF";
  if (chartPage == CHART_PAGE_TREND) return "TREND";
  if (chartPage == CHART_PAGE_STATS) return "STAT";
  if (chartPage == CHART_PAGE_ALERTS) return "ALERT";
  return "METEO";
}

void nextChartPage() {
  if (chartPage == CHART_PAGE_BASIC)
    chartPage = CHART_PAGE_EXTRA;
  else if (chartPage == CHART_PAGE_EXTRA)
    chartPage = CHART_PAGE_INOUT;
  else if (chartPage == CHART_PAGE_INOUT)
    chartPage = CHART_PAGE_COMFORT;
  else if (chartPage == CHART_PAGE_COMFORT)
    chartPage = CHART_PAGE_TREND;
  else if (chartPage == CHART_PAGE_TREND)
    chartPage = CHART_PAGE_STATS;
  else if (chartPage == CHART_PAGE_STATS)
    chartPage = CHART_PAGE_ALERTS;
  else
    chartPage = CHART_PAGE_BASIC;
}


uint32_t indoorRecordChecksum(const IndoorDiskRecord &rec) {
  const uint8_t *p = reinterpret_cast<const uint8_t*>(&rec);
  const size_t n = sizeof(IndoorDiskRecord) - sizeof(uint32_t);
  uint32_t h = 2166136261UL;

  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619UL;
  }
  return h;
}


// ============================================================
// RC15.146 - WIND DIRECTION HISTORY SIDECAR
// ============================================================
uint32_t windDirRecordChecksum(const WindDirDiskRecord &rec) {
  const uint8_t *p = reinterpret_cast<const uint8_t*>(&rec);
  const size_t n = sizeof(WindDirDiskRecord) - sizeof(uint32_t);
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619UL;
  }
  return h;
}

float normalizeWindDirDeg(float deg) {
  if (!isfinite(deg)) return NAN;
  while (deg < 0.0f) deg += 360.0f;
  while (deg >= 360.0f) deg -= 360.0f;
  return deg;
}

void pushWindDirSample(float directionDeg, uint32_t epoch) {
  float d = normalizeWindDirDeg(directionDeg);
  if (!isfinite(d)) return;

  WindDirSample &s = windDirHistory[windDirHead];
  s.epoch = epoch;
  s.directionDeg = d;
  windDirHead = (windDirHead + 1) % CHART_HISTORY_POINTS;
  if (windDirCount < CHART_HISTORY_POINTS)
    windDirCount++;
}

int windDirIndexOldest(int logicalIndex) {
  int start = (windDirHead + CHART_HISTORY_POINTS - windDirCount) % CHART_HISTORY_POINTS;
  return (start + logicalIndex) % CHART_HISTORY_POINTS;
}

bool appendWindDirSampleSD(float directionDeg, uint32_t epoch) {
#if CYD_SD_ENABLE
  if (!sdReady || chartSdPausedLowSpace)
    return false;

  float d = normalizeWindDirDeg(directionDeg);
  if (!isfinite(d))
    return false;

  WindDirDiskRecord rec = {};
  rec.magic = WIND_DIR_RECORD_MAGIC;
  rec.version = WIND_DIR_RECORD_VERSION;
  rec.epoch = epoch;
  rec.directionDeg = d;
  rec.checksum = windDirRecordChecksum(rec);

  File f = SD.open(WIND_DIR_HISTORY_FILE, FILE_APPEND);
  if (!f) {
    windDirSdWriteErrors++;
    return false;
  }

  size_t written = f.write(reinterpret_cast<const uint8_t*>(&rec), sizeof(rec));
  f.flush();
  windDirHistoryFileBytes = f.size();
  f.close();

  if (written != sizeof(rec)) {
    windDirSdWriteErrors++;
    return false;
  }

  windDirSdWritesOK++;
  return true;
#else
  (void)directionDeg;
  (void)epoch;
  return false;
#endif
}

void loadWindDir24HFromSD() {
#if CYD_SD_ENABLE
  windDirHead = 0;
  windDirCount = 0;
  windDirHistoryFileBytes = 0;

  if (!sdReady || !SD.exists(WIND_DIR_HISTORY_FILE))
    return;

  File f = SD.open(WIND_DIR_HISTORY_FILE, FILE_READ);
  if (!f) return;

  const uint32_t recSize = sizeof(WindDirDiskRecord);
  const uint32_t fileSize = (uint32_t)f.size();
  windDirHistoryFileBytes = fileSize;
  const uint32_t fullRecords = fileSize / recSize;
  const uint32_t wanted = fullRecords > CHART_HISTORY_POINTS ? CHART_HISTORY_POINTS : fullRecords;
  const uint32_t startRecord = fullRecords - wanted;

  if (!f.seek(startRecord * recSize)) {
    f.close();
    return;
  }

  for (uint32_t i = startRecord; i < fullRecords; i++) {
    WindDirDiskRecord rec = {};
    if (f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) != sizeof(rec))
      break;
    if (rec.magic != WIND_DIR_RECORD_MAGIC ||
        rec.version != WIND_DIR_RECORD_VERSION ||
        rec.checksum != windDirRecordChecksum(rec))
      continue;
    pushWindDirSample(rec.directionDeg, rec.epoch);
  }
  f.close();

  Serial.print("[HIST-DIR] loaded=");
  Serial.print(windDirCount);
  Serial.print(" file=");
  Serial.println(formatBytesShort(windDirHistoryFileBytes));
#endif
}


// RC15.216: wyznacza najnowszy poprawny epoch w ogonie pliku historii.
// 7D ma oznaczac ostatnie 604800 s, a nie po prostu ostatnie 2016 rekordow.
template <typename RecT>
uint32_t newestEpochInTail(File &f, uint32_t fullRecords, uint32_t startRecord,
                           bool (*validFn)(const RecT &)) {
  uint32_t newest = 0;
  const uint32_t recSize = sizeof(RecT);
  if (!f.seek(startRecord * recSize))
    return 0;

  for (uint32_t i = startRecord; i < fullRecords; ++i) {
    RecT rec = {};
    if (f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) != sizeof(rec))
      break;
    if (!validFn(rec))
      continue;
    if (rec.epoch > 100000UL && rec.epoch > newest)
      newest = rec.epoch;
  }
  return newest;
}

bool validWindDir7DRecord(const WindDirDiskRecord &rec) {
  return rec.magic == WIND_DIR_RECORD_MAGIC &&
         rec.version == WIND_DIR_RECORD_VERSION &&
         rec.checksum == windDirRecordChecksum(rec);
}

bool validIndoor7DRecord(const IndoorDiskRecord &rec) {
  return rec.magic == INDOOR_RECORD_MAGIC &&
         rec.version == INDOOR_RECORD_VERSION &&
         rec.checksum == indoorRecordChecksum(rec);
}

bool validChart7DRecord(const ChartDiskRecord &rec) {
  return rec.magic == CHART_RECORD_MAGIC &&
         rec.version == CHART_RECORD_VERSION &&
         rec.checksum == chartRecordChecksum(rec);
}

bool loadWindDir7DFromSD() {
#if CYD_SD_ENABLE
  if (!sdReady || !SD.exists(WIND_DIR_HISTORY_FILE)) {
    windDir7dLoaded = false;
    return false;
  }

  File f = SD.open(WIND_DIR_HISTORY_FILE, FILE_READ);
  if (!f) {
    windDir7dLoaded = false;
    return false;
  }

  const uint32_t recSize = sizeof(WindDirDiskRecord);
  const uint32_t fullRecords = (uint32_t)f.size() / recSize;
  const uint32_t max7dRecords = 7UL * 24UL * 12UL;
  const uint32_t startRecord = fullRecords > max7dRecords ? fullRecords - max7dRecords : 0;
  uint32_t newest7dEpoch = newestEpochInTail<WindDirDiskRecord>(f, fullRecords, startRecord, validWindDir7DRecord);
  uint32_t cutoff7dEpoch = (newest7dEpoch > 604800UL) ? (newest7dEpoch - 604800UL) : 0;

  if (!f.seek(startRecord * recSize)) {
    f.close();
    windDir7dLoaded = false;
    return false;
  }

  windDir7dCount = 0;
  windDir7dFullHours = 0;
  windDir7dPartialHours = 0;
  windDir7dLowHours = 0;
  memset(windDir7dSamples, 0, sizeof(windDir7dSamples));
  windDir7dLoaded = true;

  float sumSin = 0.0f;
  float sumCos = 0.0f;
  float lastDir = NAN;
  uint8_t bucketCount = 0;
  uint32_t bucketHour = 0xFFFFFFFFUL;
  uint32_t bucketLastEpoch = 0;
  bool bucketTimed = false;

  auto flushDirBucket = [&]() {
    if (!bucketCount || windDir7dCount >= CHART_7D_POINTS)
      return;

    float meanDir = lastDir;
    float mag = sqrtf(sumSin * sumSin + sumCos * sumCos);
    if (mag > 0.001f) {
      meanDir = atan2f(sumSin, sumCos) * 57.2957795f;
      if (meanDir < 0.0f) meanDir += 360.0f;
    }

    if (bucketCount >= 12)
      windDir7dFullHours++;
    else if (bucketCount >= 6)
      windDir7dPartialHours++;
    else
      windDir7dLowHours++;

    uint16_t outIndex = windDir7dCount;
    windDir7dSamples[outIndex] = bucketCount;

    WindDirSample &o = windDir7d[windDir7dCount++];
    o.epoch = (bucketTimed && bucketHour != 0xFFFFFFFFUL) ?
              (bucketHour * 3600UL + 1800UL) : bucketLastEpoch;
    o.directionDeg = normalizeWindDirDeg(meanDir);

    sumSin = 0.0f;
    sumCos = 0.0f;
    lastDir = NAN;
    bucketCount = 0;
    bucketLastEpoch = 0;
    bucketTimed = false;
  };

  for (uint32_t i = startRecord; i < fullRecords; i++) {
    WindDirDiskRecord rec = {};
    if (f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) != sizeof(rec))
      break;
    if (rec.magic != WIND_DIR_RECORD_MAGIC ||
        rec.version != WIND_DIR_RECORD_VERSION ||
        rec.checksum != windDirRecordChecksum(rec))
      continue;
    if (newest7dEpoch > 100000UL &&
        (rec.epoch <= 100000UL || rec.epoch < cutoff7dEpoch))
      continue;

    float d = normalizeWindDirDeg(rec.directionDeg);
    if (!isfinite(d)) continue;

    if (rec.epoch > 100000UL) {
      uint32_t hourKey = rec.epoch / 3600UL;
      if (bucketHour == 0xFFFFFFFFUL)
        bucketHour = hourKey;
      if (hourKey != bucketHour) {
        flushDirBucket();
        bucketHour = hourKey;
      }
    } else if (bucketCount >= 12) {
      flushDirBucket();
    }

    float rad = d * 0.01745329252f;
    sumSin += sinf(rad);
    sumCos += cosf(rad);
    lastDir = d;
    bucketLastEpoch = rec.epoch;
    if (rec.epoch > 100000UL)
      bucketTimed = true;
    bucketCount++;
  }

  flushDirBucket();
  f.close();

  Serial.print("[HIST-DIR7D] records=");
  Serial.print(fullRecords - startRecord);
  Serial.print(" hourly=");
  Serial.print(windDir7dCount);
  Serial.print(" full=");
  Serial.print(windDir7dFullHours);
  Serial.print(" part=");
  Serial.print(windDir7dPartialHours);
  Serial.print(" low=");
  Serial.print(windDir7dLowHours);
  Serial.print(" axis=");
  if (windDir7dCount > 0 && windDir7d[windDir7dCount - 1].epoch > 100000UL)
    Serial.println((windDir7d[windDir7dCount - 1].epoch % 3600UL) == 1800UL ? "HH:30" : "MIXED");
  else
    Serial.println("SEQ");
  return windDir7dCount > 0;
#else
  windDir7dLoaded = true;
  windDir7dCount = 0;
  return false;
#endif
}




bool repairBinaryTailSafe(const char *path, size_t recSize, const char *tag,
                          const char *tmpPath, const char *bakPath) {
#if CYD_SD_ENABLE
  if (!sdReady || !path || !tmpPath || !bakPath || recSize == 0)
    return false;

  if (!SD.exists(path) && SD.exists(bakPath)) {
    bool restored = SD.rename(bakPath, path);
    Serial.print("[TAIL-REC] ");
    Serial.print(tag);
    Serial.print(" restore=");
    Serial.println(restored ? "OK" : "ERR");
    if (!restored)
      return false;
  }

  if (SD.exists(path) && SD.exists(tmpPath))
    SD.remove(tmpPath);

  if (SD.exists(path) && SD.exists(bakPath))
    SD.remove(bakPath);

  if (!SD.exists(path))
    return true;

  File in = SD.open(path, FILE_READ);
  if (!in) {
    Serial.print("[TAIL] ");
    Serial.print(tag);
    Serial.println(" open=ERR");
    return false;
  }

  const uint32_t fileSize = (uint32_t)in.size();
  const uint32_t alignedSize = (fileSize / recSize) * recSize;
  const uint32_t tailBytes = fileSize - alignedSize;

  if (tailBytes == 0) {
    in.close();
    return true;
  }

  Serial.print("[TAIL] ");
  Serial.print(tag);
  Serial.print(" partial=");
  Serial.print(tailBytes);
  Serial.print("B repair ");
  Serial.print(fileSize);
  Serial.print("->");
  Serial.println(alignedSize);

  if (SD.exists(tmpPath)) SD.remove(tmpPath);
  if (SD.exists(bakPath)) SD.remove(bakPath);

  File out = SD.open(tmpPath, FILE_WRITE);
  if (!out) {
    in.close();
    Serial.print("[TAIL] ");
    Serial.print(tag);
    Serial.println(" tmp=ERR");
    return false;
  }

  uint8_t buf[256];
  uint32_t copied = 0;
  bool ok = true;

  while (copied < alignedSize) {
    size_t chunk = alignedSize - copied;
    if (chunk > sizeof(buf))
      chunk = sizeof(buf);

    size_t got = in.read(buf, chunk);
    if (got != chunk) {
      ok = false;
      break;
    }

    size_t put = out.write(buf, chunk);
    if (put != chunk) {
      ok = false;
      break;
    }

    copied += chunk;
  }

  out.flush();
  out.close();
  in.close();

  if (!ok || copied != alignedSize) {
    SD.remove(tmpPath);
    Serial.print("[TAIL] ");
    Serial.print(tag);
    Serial.println(" copy=ERR");
    return false;
  }

  if (!SD.rename(path, bakPath)) {
    SD.remove(tmpPath);
    Serial.print("[TAIL] ");
    Serial.print(tag);
    Serial.println(" backup=ERR");
    return false;
  }

  if (!SD.rename(tmpPath, path)) {
    bool restored = SD.rename(bakPath, path);
    SD.remove(tmpPath);
    Serial.print("[TAIL] ");
    Serial.print(tag);
    Serial.print(" install=ERR restore=");
    Serial.println(restored ? "OK" : "ERR");
    return false;
  }

  SD.remove(bakPath);

  Serial.print("[TAIL] ");
  Serial.print(tag);
  Serial.println(" repair=OK");
  return true;
#else
  (void)path;
  (void)recSize;
  (void)tag;
  (void)tmpPath;
  (void)bakPath;
  return true;
#endif
}


bool repairCsvTailSafe(const char *path, const char *tmpPath, const char *bakPath) {
#if CYD_SD_ENABLE
  if (!sdReady || !path || !tmpPath || !bakPath)
    return false;

  if (!SD.exists(path) && SD.exists(bakPath)) {
    bool restored = SD.rename(bakPath, path);
    Serial.print("[CSV-TAIL-REC] restore=");
    Serial.println(restored ? "OK" : "ERR");
    if (!restored)
      return false;
  }

  if (SD.exists(path) && SD.exists(tmpPath))
    SD.remove(tmpPath);

  if (SD.exists(path) && SD.exists(bakPath))
    SD.remove(bakPath);

  if (!SD.exists(path))
    return true;

  File in = SD.open(path, FILE_READ);
  if (!in) {
    Serial.println("[CSV-TAIL] open=ERR");
    return false;
  }

  const uint32_t fileSize = (uint32_t)in.size();
  if (fileSize == 0) {
    in.close();
    return true;
  }

  if (!in.seek(fileSize - 1)) {
    in.close();
    Serial.println("[CSV-TAIL] seek-end=ERR");
    return false;
  }

  int lastByte = in.read();
  if (lastByte == '\n') {
    in.close();
    return true;
  }

  const size_t BLOCK = 256;
  uint8_t buf[BLOCK];
  uint32_t searchEnd = fileSize;
  uint32_t keepBytes = 0;
  bool foundNewline = false;

  while (searchEnd > 0 && !foundNewline) {
    uint32_t blockStart = (searchEnd > BLOCK) ? (searchEnd - BLOCK) : 0;
    size_t len = (size_t)(searchEnd - blockStart);

    if (!in.seek(blockStart)) {
      in.close();
      Serial.println("[CSV-TAIL] seek-scan=ERR");
      return false;
    }

    size_t got = in.read(buf, len);
    if (got != len) {
      in.close();
      Serial.println("[CSV-TAIL] read-scan=ERR");
      return false;
    }

    for (int i = (int)len - 1; i >= 0; --i) {
      if (buf[i] == '\n') {
        keepBytes = blockStart + (uint32_t)i + 1UL;
        foundNewline = true;
        break;
      }
    }

    searchEnd = blockStart;
  }

  if (!foundNewline)
    keepBytes = 0;

  Serial.print("[CSV-TAIL] partial=");
  Serial.print(fileSize - keepBytes);
  Serial.print("B repair ");
  Serial.print(fileSize);
  Serial.print("->");
  Serial.println(keepBytes);

  if (SD.exists(tmpPath)) SD.remove(tmpPath);
  if (SD.exists(bakPath)) SD.remove(bakPath);

  File out = SD.open(tmpPath, FILE_WRITE);
  if (!out) {
    in.close();
    Serial.println("[CSV-TAIL] tmp=ERR");
    return false;
  }

  if (!in.seek(0)) {
    out.close();
    in.close();
    SD.remove(tmpPath);
    Serial.println("[CSV-TAIL] seek-copy=ERR");
    return false;
  }

  uint32_t copied = 0;
  bool ok = true;

  while (copied < keepBytes) {
    size_t chunk = keepBytes - copied;
    if (chunk > sizeof(buf))
      chunk = sizeof(buf);

    size_t got = in.read(buf, chunk);
    if (got != chunk) {
      ok = false;
      break;
    }

    size_t put = out.write(buf, chunk);
    if (put != chunk) {
      ok = false;
      break;
    }

    copied += chunk;
  }

  out.flush();
  out.close();
  in.close();

  if (!ok || copied != keepBytes) {
    SD.remove(tmpPath);
    Serial.println("[CSV-TAIL] copy=ERR");
    return false;
  }

  if (!SD.rename(path, bakPath)) {
    SD.remove(tmpPath);
    Serial.println("[CSV-TAIL] backup=ERR");
    return false;
  }

  if (!SD.rename(tmpPath, path)) {
    bool restored = SD.rename(bakPath, path);
    SD.remove(tmpPath);
    Serial.print("[CSV-TAIL] install=ERR restore=");
    Serial.println(restored ? "OK" : "ERR");
    return false;
  }

  SD.remove(bakPath);
  Serial.println("[CSV-TAIL] repair=OK");
  return true;
#else
  (void)path;
  (void)tmpPath;
  (void)bakPath;
  return true;
#endif
}

void checkArchiveIntegrity() {
#if CYD_SD_ENABLE
  const uint32_t MAX_SCAN = 4096UL;

  historyCheckScanned = 0;
  historyCheckBad = 0;
  historySequenceGaps = 0;
  indoorCheckScanned = 0;
  indoorCheckBad = 0;
  historyTailAligned = true;
  indoorTailAligned = true;
  csvTailOK = true;
  archiveIntegrityOK = true;
  historyTailEpoch = 0;
  indoorTailEpoch = 0;
  archiveTailEpochSync = true;
  windDirCheckScanned = 0;
  windDirCheckBad = 0;
  windDirTailAligned = true;
  windDirTailEpoch = 0;
  windDirTailEpochSync = true;

  if (sdReady && SD.exists(CHART_HISTORY_FILE)) {
    File f = SD.open(CHART_HISTORY_FILE, FILE_READ);
    if (f) {
      uint32_t fileSize = (uint32_t)f.size();
      const uint32_t recSize = sizeof(ChartDiskRecord);
      historyTailAligned = ((fileSize % recSize) == 0);

      uint32_t fullRecords = fileSize / recSize;
      uint32_t startRecord = (fullRecords > MAX_SCAN) ? (fullRecords - MAX_SCAN) : 0;

      if (f.seek(startRecord * recSize)) {
        bool havePrevSequence = false;
        uint32_t prevSequence = 0;

        for (uint32_t i = startRecord; i < fullRecords; i++) {
          ChartDiskRecord rec = {};
          size_t got = f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec));
          if (got != sizeof(rec)) {
            historyCheckBad++;
            break;
          }

          historyCheckScanned++;

          bool validRec =
              rec.magic == CHART_RECORD_MAGIC &&
              rec.version == CHART_RECORD_VERSION &&
              rec.checksum == chartRecordChecksum(rec);

          if (!validRec) {
            historyCheckBad++;
            havePrevSequence = false;
            continue;
          }

          if (havePrevSequence) {
            if (rec.sequence > prevSequence + 1UL)
              historySequenceGaps += rec.sequence - prevSequence - 1UL;
            else if (rec.sequence <= prevSequence)
              historySequenceGaps++;
          }

          prevSequence = rec.sequence;
          havePrevSequence = true;

          // Zapamietujemy czas ostatniego poprawnego rekordu MAIN.
          if (rec.epoch > 100000UL)
            historyTailEpoch = rec.epoch;
        }
      } else {
        historyCheckBad++;
      }

      f.close();
    } else {
      historyCheckBad++;
    }
  }

  if (sdReady && SD.exists(INDOOR_HISTORY_FILE)) {
    File f = SD.open(INDOOR_HISTORY_FILE, FILE_READ);
    if (f) {
      uint32_t fileSize = (uint32_t)f.size();
      const uint32_t recSize = sizeof(IndoorDiskRecord);
      indoorTailAligned = ((fileSize % recSize) == 0);

      uint32_t fullRecords = fileSize / recSize;
      uint32_t startRecord = (fullRecords > MAX_SCAN) ? (fullRecords - MAX_SCAN) : 0;

      if (f.seek(startRecord * recSize)) {
        for (uint32_t i = startRecord; i < fullRecords; i++) {
          IndoorDiskRecord rec = {};
          size_t got = f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec));
          if (got != sizeof(rec)) {
            indoorCheckBad++;
            break;
          }

          indoorCheckScanned++;

          if (rec.magic != INDOOR_RECORD_MAGIC ||
              rec.version != INDOOR_RECORD_VERSION ||
              rec.checksum != indoorRecordChecksum(rec)) {
            indoorCheckBad++;
          } else if (rec.epoch > 100000UL) {
            // Czas ostatniego poprawnego rekordu INDOOR.
            indoorTailEpoch = rec.epoch;
          }
        }
      } else {
        indoorCheckBad++;
      }

      f.close();
    } else {
      indoorCheckBad++;
    }
  }

  if (sdReady && SD.exists(WIND_DIR_HISTORY_FILE)) {
    File f = SD.open(WIND_DIR_HISTORY_FILE, FILE_READ);
    if (f) {
      uint32_t fileSize = (uint32_t)f.size();
      const uint32_t recSize = sizeof(WindDirDiskRecord);
      windDirTailAligned = ((fileSize % recSize) == 0);

      uint32_t fullRecords = fileSize / recSize;
      uint32_t startRecord = (fullRecords > MAX_SCAN) ? (fullRecords - MAX_SCAN) : 0;

      if (f.seek(startRecord * recSize)) {
        for (uint32_t i = startRecord; i < fullRecords; ++i) {
          WindDirDiskRecord rec = {};
          size_t got = f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec));
          if (got != sizeof(rec)) {
            windDirCheckBad++;
            break;
          }

          windDirCheckScanned++;

          if (rec.magic != WIND_DIR_RECORD_MAGIC ||
              rec.version != WIND_DIR_RECORD_VERSION ||
              rec.checksum != windDirRecordChecksum(rec)) {
            windDirCheckBad++;
          } else if (rec.epoch > 100000UL) {
            windDirTailEpoch = rec.epoch;
          }
        }
      } else {
        windDirCheckBad++;
      }

      f.close();
    } else {
      windDirCheckBad++;
    }
  }

  if (sdReady && SD.exists(WEATHER_CSV_FILE)) {
    File f = SD.open(WEATHER_CSV_FILE, FILE_READ);
    if (f) {
      uint32_t sz = (uint32_t)f.size();

      if (sz > 0) {
        if (f.seek(sz - 1)) {
          int lastByte = f.read();
          csvTailOK = (lastByte == '\n');
        } else {
          csvTailOK = false;
        }
      }

      f.close();
    } else {
      csvTailOK = false;
    }
  }

  // RC15.200:
  // Historyczne pliki moga miec rozna liczbe rekordow (INDOOR powstal pozniej),
  // dlatego nie porownujemy count. Porownujemy tylko najnowszy poprawny czas.
  if (historyTailEpoch > 100000UL && indoorTailEpoch > 100000UL) {
    uint32_t diff = (historyTailEpoch >= indoorTailEpoch)
                      ? (historyTailEpoch - indoorTailEpoch)
                      : (indoorTailEpoch - historyTailEpoch);

    // Oba rekordy z jednego cyklu 5-minutowego dostaja ten sam epoch.
    // Tolerancja 2 s chroni przed starymi wersjami, ktore mogly zapisywac
    // elementy cyklu w minimalnie innym momencie.
    archiveTailEpochSync = (diff <= 2UL);
  } else {
    // Stare rekordy bez prawidlowego epoch nie sa automatycznie uznawane za blad.
    archiveTailEpochSync = true;
  }

  if (historyTailEpoch > 100000UL && windDirTailEpoch > 100000UL) {
    uint32_t diff = (historyTailEpoch >= windDirTailEpoch)
                      ? (historyTailEpoch - windDirTailEpoch)
                      : (windDirTailEpoch - historyTailEpoch);
    windDirTailEpochSync = (diff <= 2UL);
  } else {
    // Brak sidecara w bardzo starym archiwum nie uniewaznia MAIN.
    windDirTailEpochSync = true;
  }

  archiveIntegrityOK =
      archiveTailEpochSync &&
      windDirTailEpochSync &&
      windDirTailAligned &&
      windDirCheckBad == 0 &&
      historyTailAligned &&
      indoorTailAligned &&
      csvTailOK &&
      historyCheckBad == 0 &&
      indoorCheckBad == 0 &&
      historySequenceGaps == 0;

  Serial.print("[CHECK] MAIN scanned=");
  Serial.print(historyCheckScanned);
  Serial.print(" bad=");
  Serial.print(historyCheckBad);
  Serial.print(" seqGaps=");
  Serial.print(historySequenceGaps);
  Serial.print(" aligned=");
  Serial.println(historyTailAligned ? "YES" : "NO");

  Serial.print("[CHECK] INDOOR scanned=");
  Serial.print(indoorCheckScanned);
  Serial.print(" bad=");
  Serial.print(indoorCheckBad);
  Serial.print(" aligned=");
  Serial.println(indoorTailAligned ? "YES" : "NO");

  Serial.print("[CHECK] WIND-DIR scanned=");
  Serial.print(windDirCheckScanned);
  Serial.print(" bad=");
  Serial.print(windDirCheckBad);
  Serial.print(" aligned=");
  Serial.println(windDirTailAligned ? "YES" : "NO");

  Serial.print("[CHECK] TAIL DIR main=");
  Serial.print(historyTailEpoch);
  Serial.print(" dir=");
  Serial.print(windDirTailEpoch);
  Serial.print(" sync=");
  Serial.println(windDirTailEpochSync ? "YES" : "NO");

  Serial.print("[CHECK] TAIL TIME main=");
  Serial.print(historyTailEpoch);
  Serial.print(" indoor=");
  Serial.print(indoorTailEpoch);
  Serial.print(" sync=");
  Serial.println(archiveTailEpochSync ? "YES" : "NO");

  Serial.print("[CHECK] CSV tail=");
  Serial.println(csvTailOK ? "OK" : "BAD");

  Serial.print("[CHECK] ARCHIVE=");
  Serial.println(archiveIntegrityOK ? "OK" : "WARNING");
#endif
}


uint32_t historyBundleJournalChecksum(const HistoryBundleJournal &j) {
  const uint8_t *p = reinterpret_cast<const uint8_t*>(&j);
  const size_t n = sizeof(HistoryBundleJournal) - sizeof(uint32_t);
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 16777619UL;
  }
  return h;
}

bool writeHistoryBundleJournal(const WeatherPacket &d, uint32_t epoch, uint32_t sequence) {
#if CYD_SD_ENABLE
  if (!sdReady || epoch <= 100000UL)
    return false;

  if (SD.exists(HISTORY_BUNDLE_JOURNAL_FILE))
    SD.remove(HISTORY_BUNDLE_JOURNAL_FILE);

  HistoryBundleJournal j = {};
  j.magic = HISTORY_BUNDLE_JOURNAL_MAGIC;
  j.version = HISTORY_BUNDLE_JOURNAL_VERSION;
  j.sequence = sequence;
  j.epoch = epoch;
  memcpy(&j.data, &d, sizeof(WeatherPacket));

  float dir = normalizeWindDirDeg(d.kierunek_wiatru);
  if (isfinite(dir))
    j.flags |= HBJ_FLAG_DIR_REQUIRED;

  j.checksum = historyBundleJournalChecksum(j);

  File f = SD.open(HISTORY_BUNDLE_JOURNAL_FILE, FILE_WRITE);
  if (!f) {
    historyBundleJournalErrors++;
    return false;
  }

  size_t written = f.write(reinterpret_cast<const uint8_t*>(&j), sizeof(j));
  f.flush();
  f.close();

  if (written != sizeof(j)) {
    historyBundleJournalErrors++;
    SD.remove(HISTORY_BUNDLE_JOURNAL_FILE);
    return false;
  }

  return true;
#else
  (void)d; (void)epoch; (void)sequence;
  return false;
#endif
}

static bool readHistoryBundleJournal(HistoryBundleJournal &j) {
#if CYD_SD_ENABLE
  if (!sdReady || !SD.exists(HISTORY_BUNDLE_JOURNAL_FILE))
    return false;

  File f = SD.open(HISTORY_BUNDLE_JOURNAL_FILE, FILE_READ);
  if (!f)
    return false;

  if ((uint32_t)f.size() != sizeof(HistoryBundleJournal)) {
    f.close();
    return false;
  }

  size_t got = f.read(reinterpret_cast<uint8_t*>(&j), sizeof(j));
  f.close();

  if (got != sizeof(j) ||
      j.magic != HISTORY_BUNDLE_JOURNAL_MAGIC ||
      j.version != HISTORY_BUNDLE_JOURNAL_VERSION ||
      j.checksum != historyBundleJournalChecksum(j) ||
      j.epoch <= 100000UL)
    return false;

  return true;
#else
  (void)j;
  return false;
#endif
}

static bool chartTailState(uint32_t &seq, uint32_t &epoch, bool &valid) {
  seq = 0; epoch = 0; valid = false;
  if (!sdReady || !SD.exists(CHART_HISTORY_FILE))
    return true;

  File f = SD.open(CHART_HISTORY_FILE, FILE_READ);
  if (!f) return false;

  const uint32_t rs = sizeof(ChartDiskRecord);
  uint32_t n = (uint32_t)f.size() / rs;
  if (n == 0) { f.close(); return true; }

  const uint32_t scan = n > 64UL ? 64UL : n;
  for (uint32_t back=0; back<scan; ++back) {
    uint32_t idx=n-1UL-back;
    if (!f.seek(idx*rs)) break;
    ChartDiskRecord r={};
    if (f.read(reinterpret_cast<uint8_t*>(&r), sizeof(r)) != sizeof(r)) continue;
    if (r.magic==CHART_RECORD_MAGIC && r.version==CHART_RECORD_VERSION &&
        r.checksum==chartRecordChecksum(r)) {
      seq=r.sequence; epoch=r.epoch; valid=true; f.close(); return true;
    }
  }
  f.close();
  return false;
}

template <typename REC>
static bool binaryTailEpoch(const char *path,
                            uint32_t (*checksumFn)(const REC&),
                            uint32_t magic, uint16_t version,
                            uint32_t &epoch, bool &valid) {
  epoch=0; valid=false;
  if (!sdReady || !SD.exists(path))
    return true;
  File f=SD.open(path, FILE_READ);
  if (!f) return false;
  const uint32_t rs=sizeof(REC);
  uint32_t n=(uint32_t)f.size()/rs;
  if (n==0) { f.close(); return true; }
  const uint32_t scan=n>64UL?64UL:n;
  for (uint32_t back=0; back<scan; ++back) {
    uint32_t idx=n-1UL-back;
    if (!f.seek(idx*rs)) break;
    REC r={};
    if (f.read(reinterpret_cast<uint8_t*>(&r),sizeof(r))!=sizeof(r)) continue;
    if (r.magic==magic && r.version==version && r.checksum==checksumFn(r)) {
      epoch=r.epoch; valid=true; f.close(); return true;
    }
  }
  f.close();
  return false;
}

static bool csvTailSequenceEpoch(uint32_t &seq, uint32_t &epoch, bool &valid) {
  seq=0; epoch=0; valid=false;
  if (!sdReady || !SD.exists(WEATHER_CSV_FILE))
    return true;

  File f=SD.open(WEATHER_CSV_FILE, FILE_READ);
  if (!f) return false;
  uint32_t sz=(uint32_t)f.size();
  if (sz==0) { f.close(); return true; }

  const uint32_t TAKE=384;
  uint32_t start=(sz>TAKE)?(sz-TAKE):0;
  if (!f.seek(start)) { f.close(); return false; }

  String tail;
  tail.reserve((size_t)(sz-start)+1);
  while (f.available()) tail += (char)f.read();
  f.close();

  int end=tail.length()-1;
  while (end>=0 && (tail[end]=='\n' || tail[end]=='\r')) end--;
  if (end<0) return true;

  int begin=end;
  while (begin>=0 && tail[begin]!='\n') begin--;
  begin++;

  String line=tail.substring(begin,end+1);
  unsigned long s=0,e=0;
  if (sscanf(line.c_str(), "%lu;%lu;", &s, &e)==2) {
    seq=(uint32_t)s; epoch=(uint32_t)e; valid=true;
  }
  return true;
}

bool recoverHistoryBundleJournal(WeatherPacket *committedData,
                                 uint32_t *committedEpoch,
                                 bool countRecovery) {
#if CYD_SD_ENABLE
  if (!sdReady || !SD.exists(HISTORY_BUNDLE_JOURNAL_FILE))
    return true;

  HistoryBundleJournal j={};
  if (!readHistoryBundleJournal(j)) {
    // Niepelny/uszkodzony journal oznacza, ze transakcja nie zostala
    // zatwierdzona do rozpoczecia. Usuwamy go; MAIN nie byl jeszcze wywolany.
    historyBundleJournalErrors++;
    bool dropped = SD.remove(HISTORY_BUNDLE_JOURNAL_FILE);
    if (!dropped && SD.exists(HISTORY_BUNDLE_JOURNAL_FILE)) {
      Serial.println("[HIST-JRN] invalid=DROP_ERR");
      return false;
    }
    Serial.println("[HIST-JRN] invalid=DROP");
    return true;
  }

  uint32_t mSeq=0,mEpoch=0;
  bool mValid=false;
  if (!chartTailState(mSeq,mEpoch,mValid))
    return false;

  bool mainOK = mValid && mSeq==j.sequence && mEpoch==j.epoch;
  if (!mainOK) {
    if (mValid && (mSeq >= j.sequence || mEpoch >= j.epoch)) {
      Serial.println("[HIST-JRN] MAIN conflict");
      return false;
    }
    chartSequence=j.sequence;
    if (!appendChartSampleSD(j.data,j.epoch))
      return false;
    mainOK=true;
  } else {
    chartSequence=j.sequence+1UL;
  }

  uint32_t inEpoch=0;
  bool inValid=false;
  if (!binaryTailEpoch<IndoorDiskRecord>(INDOOR_HISTORY_FILE, indoorRecordChecksum,
       INDOOR_RECORD_MAGIC, INDOOR_RECORD_VERSION, inEpoch, inValid))
    return false;
  bool indoorOK=inValid && inEpoch==j.epoch;
  if (!indoorOK) {
    if (inValid && inEpoch>j.epoch) {
      Serial.println("[HIST-JRN] INDOOR conflict");
      return false;
    }
    indoorOK=appendIndoorSampleSD(j.data,j.epoch);
  }

  bool dirOK=true;
  if (j.flags & HBJ_FLAG_DIR_REQUIRED) {
    uint32_t dirEpoch=0;
    bool dirValid=false;
    if (!binaryTailEpoch<WindDirDiskRecord>(WIND_DIR_HISTORY_FILE, windDirRecordChecksum,
         WIND_DIR_RECORD_MAGIC, WIND_DIR_RECORD_VERSION, dirEpoch, dirValid))
      return false;
    dirOK=dirValid && dirEpoch==j.epoch;
    if (!dirOK) {
      if (dirValid && dirEpoch>j.epoch) {
        Serial.println("[HIST-JRN] DIR conflict");
        return false;
      }
      dirOK=appendWindDirSampleSD(j.data.kierunek_wiatru,j.epoch);
    }
  }

  uint32_t cSeq=0,cEpoch=0;
  bool cValid=false;
  if (!csvTailSequenceEpoch(cSeq,cEpoch,cValid))
    return false;
  bool csvOK=cValid && cSeq==j.sequence && cEpoch==j.epoch;
  if (!csvOK) {
    if (cValid && (cSeq>j.sequence || cEpoch>j.epoch)) {
      Serial.println("[HIST-JRN] CSV conflict");
      return false;
    }
    csvOK=appendWeatherCSV(j.data,j.epoch,j.sequence);
  }

  if (!(mainOK && indoorOK && dirOK && csvOK))
    return false;

  if (committedData) memcpy(committedData,&j.data,sizeof(WeatherPacket));
  if (committedEpoch) *committedEpoch=j.epoch;

  bool journalRemoved = SD.remove(HISTORY_BUNDLE_JOURNAL_FILE);
  if (!journalRemoved && SD.exists(HISTORY_BUNDLE_JOURNAL_FILE)) {
    historyBundleJournalErrors++;
    Serial.println("[HIST-JRN] remove=ERR pending=YES");
    return false;
  }

  if (countRecovery) historyBundleRecoveries++;

  Serial.print("[HIST-JRN] COMMIT seq=");
  Serial.print(j.sequence);
  Serial.print(" epoch=");
  Serial.println(j.epoch);
  return true;
#else
  (void)committedData; (void)committedEpoch; (void)countRecovery;
  return false;
#endif
}

bool commitHistoryBundleSD(const WeatherPacket &candidate, uint32_t epoch,
                           WeatherPacket &committedData, uint32_t &committedEpoch) {
#if CYD_SD_ENABLE
  // Najpierw zawsze konczymy stara transakcje. Dopoki istnieje journal,
  // nie wolno zaczac nowszego MAIN.
  if (SD.exists(HISTORY_BUNDLE_JOURNAL_FILE)) {
    WeatherPacket recoveredData = {};
    uint32_t recoveredEpoch = 0;
    if (!recoverHistoryBundleJournal(&recoveredData,&recoveredEpoch,true))
      return false;

    // Recovery starego bundle nie moze udawac zapisu biezacego candidate.
    // Jesli candidate nie jest jeszcze nalezny, zwracamy odzyskany rekord.
    if (recoveredEpoch > 100000UL &&
        (epoch <= recoveredEpoch ||
         (epoch - recoveredEpoch) < (CHART_SAMPLE_MS / 1000UL))) {
      committedData = recoveredData;
      committedEpoch = recoveredEpoch;
      Serial.println("[HIST-JRN] pending=RECOVERED current=NOT_DUE");
      return true;
    }

    // RC15.236: candidate jest juz nalezny, wiec caller dostanie pozniej
    // NOWY rekord. Najpierw zachowujemy odzyskany starszy punkt w RAM,
    // bo inaczej zniknalby z 1H/6H/24H az do restartu.
    if (recoveredEpoch > 100000UL) {
      bool alreadyInChartRam = false;
      if (chartCount > 0) {
        int lastIdx = chartIndexOldest(chartCount - 1);
        alreadyInChartRam = (chartHistory[lastIdx].epoch == recoveredEpoch);
      }

      if (!alreadyInChartRam) {
        pushChartSample(recoveredData, recoveredEpoch);
        pushWindDirSample(recoveredData.kierunek_wiatru, recoveredEpoch);
        historyPendingRamSyncAdded = true;
        Serial.print("[HIST-JRN] PENDING-RAM-SYNC epoch=");
        Serial.println(recoveredEpoch);
      }
    }

    // Jesli candidate jest juz nalezny, po recovery rozpoczynamy osobny
    // nowy bundle z numerem wynikajacym z fizycznego ogona MAIN.
    uint32_t tailSeq = 0, tailEpoch = 0;
    bool tailValid = false;
    if (!chartTailState(tailSeq,tailEpoch,tailValid) || !tailValid)
      return false;
    chartSequence = tailSeq + 1UL;

    Serial.print("[HIST-JRN] pending=RECOVERED nextEpoch=");
    Serial.println(epoch);
  }

  uint32_t seq=chartSequence;
  if (!writeHistoryBundleJournal(candidate,epoch,seq))
    return false;

  return recoverHistoryBundleJournal(&committedData,&committedEpoch,false);
#else
  (void)candidate; (void)epoch; (void)committedData; (void)committedEpoch;
  return false;
#endif
}

void initWeatherCSVInfo() {
#if CYD_SD_ENABLE
  weatherCsvFileBytes = 0;

  if (!sdReady || !SD.exists(WEATHER_CSV_FILE))
    return;

  File f = SD.open(WEATHER_CSV_FILE, FILE_READ);
  if (!f)
    return;

  weatherCsvFileBytes = f.size();
  f.close();

  Serial.print("[CSV] file=");
  Serial.println(formatBytesShort(weatherCsvFileBytes));
#endif
}

bool appendWeatherCSV(const WeatherPacket &d, uint32_t epoch, uint32_t sequence) {
#if CYD_SD_ENABLE
  if (!sdReady || chartSdPausedLowSpace)
    return false;

  bool newFile = !SD.exists(WEATHER_CSV_FILE);

  File f = SD.open(WEATHER_CSV_FILE, FILE_APPEND);
  if (!f) {
    weatherCsvWriteErrors++;
    return false;
  }

  if (newFile || f.size() == 0) {
    const char *header =
      "sequence;epoch;"
      "temp_out_C;hum_out_pct;pressure_hPa;"
      "wind_ms;gust_ms;wind_dir_deg;"
      "rain_total_mm;rain_mmh;uv;lux;"
      "temp_in_C;hum_in_pct\n";

    size_t hw = f.print(header);
    if (hw == 0) {
      f.close();
      weatherCsvWriteErrors++;
      return false;
    }
  }

  // Jedna linia = jedna probka 5-minutowa.
  // Separator ';' jest wygodny w polskim LibreOffice/Excel.
  String line;
  line.reserve(180);

  line += String(sequence);
  line += ";";
  line += String(epoch);
  line += ";";
  line += String(d.temperatura, 2);
  line += ";";
  line += String(d.wilgotnosc, 1);
  line += ";";
  line += String(d.cisnienie, 1);
  line += ";";
  line += String(d.predkosc_wiatru, 2);
  line += ";";
  line += String(d.poryw_wiatru, 2);
  line += ";";
  line += String(d.kierunek_wiatru, 1);
  line += ";";
  line += String(d.opady, 2);
  line += ";";
  line += String(d.opady_godzina, 2);
  line += ";";
  line += String(d.uv_index, 2);
  line += ";";
  line += String(d.swiatlo_lux, 0);
  line += ";";
  line += String(d.temp_wewnetrzna, 2);
  line += ";";
  line += String(d.wilg_wewnetrzna, 1);
  line += "\n";

  size_t written = f.print(line);
  f.flush();
  weatherCsvFileBytes = f.size();
  f.close();

  if (written != line.length()) {
    weatherCsvWriteErrors++;
    return false;
  }

  weatherCsvWritesOK++;
  return true;
#else
  (void)d;
  (void)epoch;
  (void)sequence;
  return false;
#endif
}

bool appendIndoorSampleSD(const WeatherPacket &d, uint32_t epoch) {
#if CYD_SD_ENABLE
  if (!sdReady || chartSdPausedLowSpace)
    return false;

  IndoorDiskRecord rec = {};
  rec.magic = INDOOR_RECORD_MAGIC;
  rec.version = INDOOR_RECORD_VERSION;
  rec.epoch = epoch;
  rec.tempIn = d.temp_wewnetrzna;
  rec.humIn = d.wilg_wewnetrzna;
  rec.checksum = indoorRecordChecksum(rec);

  File f = SD.open(INDOOR_HISTORY_FILE, FILE_APPEND);
  if (!f) {
    indoorSdWriteErrors++;
    return false;
  }

  size_t written = f.write(reinterpret_cast<const uint8_t*>(&rec), sizeof(rec));
  f.flush();
  f.close();

  if (written != sizeof(rec)) {
    indoorSdWriteErrors++;
    return false;
  }

  indoorSdWritesOK++;
  indoorHistoryFileBytes += sizeof(rec);
  return true;
#else
  (void)d;
  (void)epoch;
  return false;
#endif
}

void loadIndoor24HFromSD() {
#if CYD_SD_ENABLE
  if (!sdReady || !SD.exists(INDOOR_HISTORY_FILE)) {
    indoorHistoryFileBytes = 0;
    indoorBootstrapPending = sdReady;
    return;
  }

  File f = SD.open(INDOOR_HISTORY_FILE, FILE_READ);
  if (!f)
    return;

  const uint32_t recSize = sizeof(IndoorDiskRecord);
  uint32_t fileSize = (uint32_t)f.size();
  indoorHistoryFileBytes = fileSize;
  uint32_t fullRecords = fileSize / recSize;
  indoorBootstrapPending = (fullRecords == 0);

  if (chartCount == 0 || fullRecords == 0) {
    f.close();
    Serial.print("[HIST-IN] loaded=0/");
    Serial.print(fullRecords);
    Serial.print(" file=");
    Serial.println(formatBytesShort(indoorHistoryFileBytes));
    return;
  }

  // Czy MAIN ma prawidlowe znaczniki czasu?
  bool timedMain = false;
  for (uint16_t i = 0; i < chartCount; ++i) {
    int idx = chartIndexOldest(i);
    if (chartHistory[idx].epoch > 100000UL) {
      timedMain = true;
      break;
    }
  }

  if (timedMain) {
    // RC15.201:
    // czytamy wiecej niz 288 ostatnich rekordow INDOOR, aby pojedyncze
    // braki zapisu nie wyrzucily potrzebnej probki poza okno odczytu.
    const uint32_t maxRead = (uint32_t)CHART_HISTORY_POINTS * 2UL;
    uint32_t startRecord = (fullRecords > maxRead) ? (fullRecords - maxRead) : 0;

    if (!f.seek(startRecord * recSize)) {
      f.close();
      return;
    }

    uint16_t matched = 0;
    uint16_t validRead = 0;

    for (uint32_t r = startRecord; r < fullRecords; ++r) {
      IndoorDiskRecord rec = {};
      if (f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) != sizeof(rec))
        break;

      if (rec.magic != INDOOR_RECORD_MAGIC ||
          rec.version != INDOOR_RECORD_VERSION ||
          rec.checksum != indoorRecordChecksum(rec) ||
          rec.epoch <= 100000UL)
        continue;

      validRead++;

      // Szukamy MAIN o tym samym czasie. Tolerancja 2 s jest zgodna
      // z kontrola TAIL TIME z RC15.200.
      for (uint16_t i = 0; i < chartCount; ++i) {
        int idx = chartIndexOldest(i);
        uint32_t mainEpoch = chartHistory[idx].epoch;

        if (mainEpoch <= 100000UL)
          continue;

        uint32_t diff = (mainEpoch >= rec.epoch)
                          ? (mainEpoch - rec.epoch)
                          : (rec.epoch - mainEpoch);

        if (diff <= 2UL) {
          chartHistory[idx].tempIn = rec.tempIn;
          chartHistory[idx].humIn = rec.humIn;
          matched++;
          break;
        }
      }
    }

    f.close();

    Serial.print("[HIST-IN] matched=");
    Serial.print(matched);
    Serial.print("/");
    Serial.print(chartCount);
    Serial.print(" read=");
    Serial.print(validRead);
    Serial.print("/");
    Serial.print(fullRecords);
    Serial.print(" mode=EPOCH file=");
    Serial.println(formatBytesShort(indoorHistoryFileBytes));
    return;
  }

  // LEGACY FALLBACK:
  // dla bardzo starych rekordow bez epoch zachowujemy dotychczasowe
  // laczenie pozycyjne, aby nie tracic zgodnosci wstecznej.
  uint32_t wanted = fullRecords > CHART_HISTORY_POINTS ?
                    CHART_HISTORY_POINTS : fullRecords;
  uint32_t startRecord = fullRecords - wanted;

  if (!f.seek(startRecord * recSize)) {
    f.close();
    return;
  }

  uint16_t mergeCount = wanted;
  if (mergeCount > chartCount)
    mergeCount = chartCount;

  uint16_t skip = wanted - mergeCount;
  for (uint16_t i = 0; i < skip; i++) {
    IndoorDiskRecord dummy = {};
    if (f.read(reinterpret_cast<uint8_t*>(&dummy), sizeof(dummy)) != sizeof(dummy)) {
      f.close();
      return;
    }
  }

  uint16_t logicalStart = chartCount - mergeCount;
  uint16_t merged = 0;

  for (uint16_t i = 0; i < mergeCount; i++) {
    IndoorDiskRecord rec = {};
    if (f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) != sizeof(rec))
      break;

    if (rec.magic != INDOOR_RECORD_MAGIC ||
        rec.version != INDOOR_RECORD_VERSION ||
        rec.checksum != indoorRecordChecksum(rec))
      continue;

    int idx = chartIndexOldest(logicalStart + i);
    chartHistory[idx].tempIn = rec.tempIn;
    chartHistory[idx].humIn = rec.humIn;
    merged++;
  }

  f.close();

  Serial.print("[HIST-IN] matched=");
  Serial.print(merged);
  Serial.print("/");
  Serial.print(chartCount);
  Serial.print(" mode=LEGACY file=");
  Serial.println(formatBytesShort(indoorHistoryFileBytes));
#endif
}

bool loadIndoor7DFromSD() {
#if CYD_SD_ENABLE
  if (!sdReady || !SD.exists(INDOOR_HISTORY_FILE)) {
    indoor7dLoaded = false;
    return false;
  }

  File f = SD.open(INDOOR_HISTORY_FILE, FILE_READ);
  if (!f) {
    indoor7dLoaded = false;
    return false;
  }

  const uint32_t recSize = sizeof(IndoorDiskRecord);
  const uint32_t fullRecords = (uint32_t)f.size() / recSize;
  const uint32_t max7dRecords = 7UL * 24UL * 12UL;
  uint32_t startRecord = (fullRecords > max7dRecords) ? fullRecords - max7dRecords : 0;
  uint32_t newest7dEpoch = newestEpochInTail<IndoorDiskRecord>(f, fullRecords, startRecord, validIndoor7DRecord);
  uint32_t cutoff7dEpoch = (newest7dEpoch > 604800UL) ? (newest7dEpoch - 604800UL) : 0;

  if (!f.seek(startRecord * recSize)) {
    f.close();
    indoor7dLoaded = false;
    return false;
  }

  indoor7dCount = 0;
  indoor7dFullHours = 0;
  indoor7dPartialHours = 0;
  indoor7dLowHours = 0;
  memset(indoor7dSamples, 0, sizeof(indoor7dSamples));
  indoor7dLoaded = true;

  float sumT = 0.0f, sumH = 0.0f;
  uint8_t bucketCount = 0;
  uint32_t bucketHour = 0xFFFFFFFFUL;
  uint32_t bucketLastEpoch = 0;
  bool bucketTimed = false;

  auto flushIndoor = [&]() {
    if (!bucketCount || indoor7dCount >= CHART_7D_POINTS)
      return;

    if (bucketCount >= 12)
      indoor7dFullHours++;
    else if (bucketCount >= 6)
      indoor7dPartialHours++;
    else
      indoor7dLowHours++;

    uint16_t outIndex = indoor7dCount;
    indoor7dSamples[outIndex] = bucketCount;
    ChartSample &o = indoor7d[indoor7dCount++];
    o = {};
    o.epoch = (bucketTimed && bucketHour != 0xFFFFFFFFUL) ?
              (bucketHour * 3600UL + 1800UL) : bucketLastEpoch;
    o.tempIn = sumT / bucketCount;
    o.humIn = sumH / bucketCount;

    sumT = 0.0f;
    sumH = 0.0f;
    bucketLastEpoch = 0;
    bucketCount = 0;
    bucketTimed = false;
  };

  for (uint32_t i = startRecord; i < fullRecords; i++) {
    IndoorDiskRecord rec = {};
    if (f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) != sizeof(rec))
      break;

    if (rec.magic != INDOOR_RECORD_MAGIC ||
        rec.version != INDOOR_RECORD_VERSION ||
        rec.checksum != indoorRecordChecksum(rec))
      continue;
    if (newest7dEpoch > 100000UL &&
        (rec.epoch <= 100000UL || rec.epoch < cutoff7dEpoch))
      continue;

    if (rec.epoch > 100000UL) {
      uint32_t hourKey = rec.epoch / 3600UL;
      if (bucketHour == 0xFFFFFFFFUL)
        bucketHour = hourKey;

      if (hourKey != bucketHour) {
        flushIndoor();
        bucketHour = hourKey;
      }
    } else if (bucketCount >= 12) {
      flushIndoor();
    }

    sumT += rec.tempIn;
    sumH += rec.humIn;
    if (rec.epoch > 100000UL) {
      bucketLastEpoch = rec.epoch;
      bucketTimed = true;
    }
    bucketCount++;
  }

  flushIndoor();
  f.close();

  Serial.print("[HIST-IN7D] records=");
  Serial.print(fullRecords - startRecord);
  Serial.print(" hourly=");
  Serial.print(indoor7dCount);
  Serial.print(" time=");
  Serial.print((indoor7dCount > 0 && indoor7d[indoor7dCount - 1].epoch > 100000UL) ?
               "EPOCH" : "SEQ");
  Serial.print(" full=");
  Serial.print(indoor7dFullHours);
  Serial.print(" part=");
  Serial.print(indoor7dPartialHours);
  Serial.print(" low=");
  Serial.print(indoor7dLowHours);
  Serial.print(" axis=");
  if (indoor7dCount > 0 && indoor7d[indoor7dCount - 1].epoch > 100000UL)
    Serial.println((indoor7d[indoor7dCount - 1].epoch % 3600UL) == 1800UL ? "HH:30" : "MIXED");
  else
    Serial.println("SEQ");

  return indoor7dCount > 0;
#else
  indoor7dLoaded = true;
  indoor7dCount = 0;
  return false;
#endif
}

bool appendChartSampleSD(const WeatherPacket &d, uint32_t epoch) {
#if CYD_SD_ENABLE
  if (!sdReady)
    return false;

  // Sprawdzamy wolne miejsce tylko przy zapisie 5-minutowym,
  // wiec koszt SD.usedBytes() jest pomijalny.
  sdUsedBytes = SD.usedBytes();
  uint64_t freeBytes = (sdCardBytes > sdUsedBytes) ?
                       (sdCardBytes - sdUsedBytes) : 0;

  if (freeBytes < CHART_MIN_FREE_BYTES) {
    chartSdPausedLowSpace = true;
    Serial.println("[HIST] WRITE PAUSED - LOW SD SPACE");
    return false;
  }

  chartSdPausedLowSpace = false;

  ChartDiskRecord rec = {};
  rec.magic = CHART_RECORD_MAGIC;
  rec.version = CHART_RECORD_VERSION;

  // Sekwencję zatwierdzamy dopiero po udanym zapisie rekordu.
  // Przy błędzie SD nie powstaje sztuczna dziura w numeracji.
  rec.sequence = chartSequence;
  rec.epoch = epoch;
  rec.temp  = d.temperatura;
  rec.hum   = d.wilgotnosc;
  rec.press = d.cisnienie;
  rec.wind  = d.predkosc_wiatru;
  rec.gust  = d.poryw_wiatru;
  rec.uv    = d.uv_index;
  rec.rain  = d.opady_godzina;
  rec.lux   = d.swiatlo_lux;
  rec.checksum = chartRecordChecksum(rec);

  File f = SD.open(CHART_HISTORY_FILE, FILE_APPEND);
  if (!f) {
    chartSdWriteErrors++;
    return false;
  }

  size_t written = f.write(reinterpret_cast<const uint8_t*>(&rec), sizeof(rec));
  f.flush();
  f.close();

  if (written != sizeof(rec)) {
    chartSdWriteErrors++;
    return false;
  }

  chartSequence++;
  chartSdWritesOK++;
  chartHistoryFileBytes += sizeof(rec);
  return true;
#else
  (void)d;
  (void)epoch;
  return false;
#endif
}

void loadChartHistorySD() {
#if CYD_SD_ENABLE
  chartHead = 0;
  chartCount = 0;
  chartHistoryLoaded = false;
  lastCommittedHistoryEpoch = 0;

  if (!sdReady || !SD.exists(CHART_HISTORY_FILE)) {
    chartHistoryFileBytes = 0;
    Serial.println("[HIST] Brak pliku - start nowej historii");
    lastChartSampleMs = millis() - CHART_SAMPLE_MS;
    return;
  }

  File f = SD.open(CHART_HISTORY_FILE, FILE_READ);
  if (!f) {
    Serial.println("[HIST] READ OPEN FAILED");
    lastChartSampleMs = millis() - CHART_SAMPLE_MS;
    return;
  }

  const uint32_t recSize = sizeof(ChartDiskRecord);
  uint32_t fileSize = (uint32_t)f.size();
  chartHistoryFileBytes = fileSize;
  uint32_t fullRecords = fileSize / recSize;
  uint32_t startRecord = 0;

  if (fullRecords > CHART_HISTORY_POINTS)
    startRecord = fullRecords - CHART_HISTORY_POINTS;

  if (!f.seek(startRecord * recSize)) {
    Serial.println("[HIST] SEEK FAILED");
    f.close();
    lastChartSampleMs = millis() - CHART_SAMPLE_MS;
    return;
  }

  uint16_t valid = 0;
  uint32_t maxSequence = 0;
  uint32_t newestValidEpoch = 0;

  for (uint32_t i = startRecord; i < fullRecords; i++) {
    ChartDiskRecord rec = {};
    size_t got = f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec));
    if (got != sizeof(rec))
      break;

    if (rec.magic != CHART_RECORD_MAGIC ||
        rec.version != CHART_RECORD_VERSION ||
        rec.checksum != chartRecordChecksum(rec)) {
      continue;
    }

    WeatherPacket d = {};
    d.temperatura = rec.temp;
    d.wilgotnosc = rec.hum;
    d.cisnienie = rec.press;
    d.predkosc_wiatru = rec.wind;
    d.poryw_wiatru = rec.gust;
    d.uv_index = rec.uv;
    d.opady_godzina = rec.rain;
    d.swiatlo_lux = rec.lux;
    d.temp_wewnetrzna = NAN;
    d.wilg_wewnetrzna = NAN;

    pushChartSample(d, rec.epoch);
    valid++;

    if (rec.sequence >= maxSequence)
      maxSequence = rec.sequence + 1;

    if (rec.epoch > 100000UL && rec.epoch >= newestValidEpoch)
      newestValidEpoch = rec.epoch;
  }

  f.close();

  chartSequence = maxSequence;
  chartHistoryLoaded = (valid > 0);
  lastCommittedHistoryEpoch = newestValidEpoch;

  Serial.print("[HIST] loaded=");
  Serial.print(valid);
  Serial.print("/");
  Serial.print(fullRecords);
  Serial.print(" points, file=");
  if (fileSize < 1024UL) {
    Serial.print(fileSize);
    Serial.println(" B");
  } else {
    Serial.print(fileSize / 1024UL);
    Serial.println(" KB");
  }

  // RC15.204:
  // millis() pozostaje fallbackiem, ale przy prawidlowym epoch termin
  // nastepnej probki bedzie liczony od lastCommittedHistoryEpoch.
  if (valid > 0)
    lastChartSampleMs = millis();
  else
    lastChartSampleMs = millis() - CHART_SAMPLE_MS;
#else
  lastChartSampleMs = millis() - CHART_SAMPLE_MS;
#endif
}




bool loadChart7DFromSD() {
#if CYD_SD_ENABLE
  if (!sdReady || !SD.exists(CHART_HISTORY_FILE)) {
    chart7dLoaded = false;
    return false;
  }

  File f = SD.open(CHART_HISTORY_FILE, FILE_READ);
  if (!f) {
    chart7dLoaded = false;
    return false;
  }

  const uint32_t recSize = sizeof(ChartDiskRecord);
  const uint32_t fullRecords = (uint32_t)f.size() / recSize;
  const uint32_t max7dRecords = 7UL * 24UL * 12UL;
  uint32_t startRecord = (fullRecords > max7dRecords) ? fullRecords - max7dRecords : 0;
  uint32_t newest7dEpoch = newestEpochInTail<ChartDiskRecord>(f, fullRecords, startRecord, validChart7DRecord);
  uint32_t cutoff7dEpoch = (newest7dEpoch > 604800UL) ? (newest7dEpoch - 604800UL) : 0;

  if (!f.seek(startRecord * recSize)) {
    f.close();
    chart7dLoaded = false;
    return false;
  }

  chart7dCount = 0;
  chart7dFullHours = 0;
  chart7dPartialHours = 0;
  chart7dLowHours = 0;
  memset(chart7dSamples, 0, sizeof(chart7dSamples));
  chart7dLoaded = true;

  ChartSample acc = {};
  float maxGust = -INFINITY;
  float maxUv = -INFINITY;
  float maxRain = -INFINITY;
  uint8_t bucketCount = 0;
  uint32_t bucketHour = 0xFFFFFFFFUL;
  bool bucketTimed = false;
  bool haveEpoch = false;
  bool sawGap = false;
  uint32_t prevEpoch = 0;

  auto flushBucket = [&]() {
    if (!bucketCount || chart7dCount >= CHART_7D_POINTS)
      return;

    if (bucketCount >= 12)
      chart7dFullHours++;
    else if (bucketCount >= 6)
      chart7dPartialHours++;
    else
      chart7dLowHours++;

    uint16_t outIndex = chart7dCount;
    chart7dSamples[outIndex] = bucketCount;
    ChartSample &o = chart7d[chart7dCount++];
    o = acc;

    // Tylko znacznik gotowego agregatu: grupowanie pozostaje z RC15.207.
    if (bucketTimed && bucketHour != 0xFFFFFFFFUL)
      o.epoch = bucketHour * 3600UL + 1800UL;

    o.temp /= bucketCount;
    o.hum /= bucketCount;
    o.press /= bucketCount;
    o.wind /= bucketCount;

    // Parametry zagrożeń zachowują maksimum z godziny, aby 7D nie
    // wygładzało krótkiego porywu, wysokiego UV ani intensywnego opadu.
    o.gust = isfinite(maxGust) ? maxGust : NAN;
    o.uv = isfinite(maxUv) ? maxUv : NAN;
    o.rain = isfinite(maxRain) ? maxRain : NAN;

    o.lux /= bucketCount;

    acc = {};
    maxGust = -INFINITY;
    maxUv = -INFINITY;
    maxRain = -INFINITY;
    bucketCount = 0;
    bucketTimed = false;
  };

  for (uint32_t i = startRecord; i < fullRecords; i++) {
    ChartDiskRecord rec = {};
    if (f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) != sizeof(rec))
      break;

    if (rec.magic != CHART_RECORD_MAGIC ||
        rec.version != CHART_RECORD_VERSION ||
        rec.checksum != chartRecordChecksum(rec))
      continue;
    if (newest7dEpoch > 100000UL &&
        (rec.epoch <= 100000UL || rec.epoch < cutoff7dEpoch))
      continue;

    if (rec.epoch > 100000UL) {
      haveEpoch = true;
      uint32_t hourKey = rec.epoch / 3600UL;

      if (bucketHour == 0xFFFFFFFFUL)
        bucketHour = hourKey;

      if (hourKey != bucketHour) {
        flushBucket();

        if (prevEpoch > 0 && rec.epoch > prevEpoch + 900UL)
          sawGap = true;

        bucketHour = hourKey;
      }

      bucketTimed = true;
      prevEpoch = rec.epoch;
    } else {
      // Bez czasu absolutnego: 12 kolejnych rekordow ~= 1 godzina.
      if (bucketCount >= 12)
        flushBucket();
    }

    acc.epoch = rec.epoch;
    acc.temp += rec.temp;
    acc.hum += rec.hum;
    acc.press += rec.press;
    acc.wind += rec.wind;

    if (isfinite(rec.gust) && rec.gust > maxGust) maxGust = rec.gust;
    if (isfinite(rec.uv) && rec.uv > maxUv) maxUv = rec.uv;
    if (isfinite(rec.rain) && rec.rain > maxRain) maxRain = rec.rain;

    acc.lux += rec.lux;
    bucketCount++;
  }

  flushBucket();
  f.close();

  Serial.print("[HIST7D] records=");
  Serial.print(fullRecords - startRecord);
  Serial.print(" hourly=");
  Serial.print(chart7dCount);
  Serial.print(" time=");
  Serial.print(haveEpoch ? "EPOCH" : "SEQ");
  Serial.print(" gaps=");
  Serial.print(sawGap ? "YES" : "NO");
  Serial.print(" full=");
  Serial.print(chart7dFullHours);
  Serial.print(" part=");
  Serial.print(chart7dPartialHours);
  Serial.print(" low=");
  Serial.print(chart7dLowHours);
  Serial.print(" axis=");
  if (chart7dCount > 0 && chart7d[chart7dCount - 1].epoch > 100000UL)
    Serial.println((chart7d[chart7dCount - 1].epoch % 3600UL) == 1800UL ? "HH:30" : "MIXED");
  else
    Serial.println("SEQ");

  return chart7dCount > 0;
#else
  chart7dLoaded = true;
  chart7dCount = 0;
  return false;
#endif
}


const char* tr(TextId id) {
  static const char* PL[] = {
    "POGODA - SIATKA","POGODA - PANEL","POGODA - MINIMAL","KOMPAS WIATRU","WSKAŹNIKI",
    "TEMP ZEW","TEMP WEW","WILG ZEW","WILG WEW","CIŚNIENIE",
    "WIATR","PORYW","KIERUNEK","ŚWIATŁO","OPAD",
    "USTAWIENIA CYD","WIDOK","ALARMY","JASNOŚĆ","AUTO WEW/ZEW",
    "ZMIANA","WIDOK TERAZ","POWRÓT","JĘZYK","ŹRÓDŁO",
    "LIVE","CZEKAM","NIEAKT","OFFLINE",
    "OK","ZIMNO","BARDZO ZIMNO","CIEPŁO","GORĄCO","BARDZO GORĄCO",
    "SUCHO","BARDZO SUCHO","WILGOTNO","BARDZO WILGOTNO",
    "NISKIE","BARDZO NISKIE","WYSOKIE","BARDZO WYSOKIE",
    "SILNY","BARDZO SILNY","ŚREDNI","EKSTREM",
    "NOC","CIEMNO","DZIEŃ","SŁOŃCE","JASNO",
    "OPAD","DUŻO"
  };

  static const char* EN[] = {
    "WEATHER GRID","WEATHER DASH","WEATHER MINIMAL","WIND COMPASS","INSTRUMENTS",
    "TEMP OUT","TEMP IN","HUM OUT","HUM IN","PRESSURE",
    "WIND","GUST","DIRECTION","LIGHT","RAIN",
    "CYD SETTINGS","SKIN","ALERTS","BRIGHTNESS","AUTO IN/OUT",
    "CHANGE","VIEW NOW","BACK","LANGUAGE","SOURCE",
    "LIVE","WAIT","STALE","OFFLINE",
    "OK","COLD","VERY COLD","WARM","HOT","VERY HOT",
    "DRY","VERY DRY","HUMID","VERY HUMID",
    "LOW","VERY LOW","HIGH","VERY HIGH",
    "STRONG","VERY STRONG","MEDIUM","EXTREME",
    "NIGHT","DARK","DAY","SUN","BRIGHT",
    "RAIN","HEAVY"
  };

  static const char* DE[] = {
    "WETTER GRID","WETTER DASH","WETTER MINIMAL","WINDROSE","INSTRUMENTE",
    "TEMP AUSSEN","TEMP INNEN","FEUCHT AUSS","FEUCHT INN","LUFTDRUCK",
    "WIND","BOE","RICHTUNG","LICHT","REGEN",
    "CYD EINSTELL.","SKIN","ALARME","HELLIGKEIT","AUTO IN/AUSS",
    "WECHSEL","ANSICHT","ZURUECK","SPRACHE","QUELLE",
    "LIVE","WARTEN","ALT","OFFLINE",
    "OK","KALT","SEHR KALT","WARM","HEISS","SEHR HEISS",
    "TROCKEN","SEHR TROCK","FEUCHT","SEHR FEUCHT",
    "NIEDRIG","SEHR NIEDR","HOCH","SEHR HOCH",
    "STARK","SEHR STARK","MITTEL","EXTREM",
    "NACHT","DUNKEL","TAG","SONNE","HELL",
    "REGEN","STARK"
  };

  static const char* CZ[] = {
    "METEO MRIZKA","METEO PANEL","METEO MINIMAL","RUZICE VETRU","PRISTROJE",
    "TEPL VEN","TEPL UVN","VLHK VEN","VLHK UVN","TLAK",
    "VITR","NARAZ","SMER","SVETLO","DEST",
    "NASTAVENI CYD","VZHLED","ALARMY","JAS","AUTO UVN/VEN",
    "ZMENA","ZOBRAZIT","ZPET","JAZYK","ZDROJ",
    "LIVE","CEKAM","STARE","OFFLINE",
    "OK","CHLADNO","VELMI CHL","TEPLO","HORKO","VELMI HORKO",
    "SUCHO","VELMI SUCHO","VLHKO","VELMI VLHKO",
    "NIZKY","VELMI NIZKY","VYSOKY","VELMI VYS",
    "SILNY","VELMI SILNY","STREDNI","EXTREM",
    "NOC","TMA","DEN","SLUNCE","JASNO",
    "DEST","HODNE"
  };

  const char** table = PL;
  if (currentLanguage == LANG_EN) table = EN;
  else if (currentLanguage == LANG_DE) table = DE;
  else if (currentLanguage == LANG_CZ) table = CZ;

  return table[id];
}

String languageName() {
  switch (currentLanguage) {
    case LANG_EN: return "EN";
    case LANG_DE: return "DE";
    case LANG_CZ: return "CZ";
    default: return "PL";
  }
}


String languageFontCode(uint8_t lang) {
  switch (lang) {
    case LANG_EN: return "en";
    case LANG_DE: return "de";
    case LANG_CZ: return "cz";
    default:      return "pl";
  }
}

String languageFontPath(uint8_t lang, bool bold) {
  // Docelowe nazwy:
  // /CYD/fonts/ui_pl_regular.vlw
  // /CYD/fonts/ui_pl_bold.vlw
  // analogicznie en/de/cz.
  String p = "CYD/fonts/ui_";
  p += languageFontCode(lang);
  p += bold ? "_bold" : "_regular";
  return p;
}

String languageSmallFontPath(uint8_t lang) {
  String p = "CYD/fonts/ui_";
  p += languageFontCode(lang);
  p += "_small";
  return p;
}

bool languageSmallFontReady(uint8_t lang) {
  if (lang > LANG_CZ)
    lang = LANG_PL;
  return langFontSmallReady[lang];
}

bool useLanguageSmallFont() {
#if CYD_SD_SMOOTH_RUNTIME_ENABLE
#ifdef SMOOTH_FONT
  uint8_t lang = currentLanguage;
  if (lang > LANG_CZ)
    lang = LANG_PL;
  if (!languageSmallFontReady(lang))
    return false;
  return loadSmoothFontSD(languageSmallFontPath(lang));
#else
  return false;
#endif
#else
  return false;
#endif
}


bool languageFontReady(uint8_t lang, bool bold) {
  if (lang > LANG_CZ)
    lang = LANG_PL;

  return bold ? langFontBoldReady[lang] : langFontRegularReady[lang];
}

bool useLanguageUiFont(bool bold) {
  (void)bold;
#if CYD_SD_SMOOTH_RUNTIME_ENABLE
#ifdef SMOOTH_FONT
  uint8_t lang = currentLanguage;
  if (lang > LANG_CZ)
    lang = LANG_PL;
  if (!languageFontReady(lang, bold))
    return false;
  return loadSmoothFontSD(languageFontPath(lang, bold));
#else
  return false;
#endif
#else
  return false;
#endif
}

bool loadSmoothFontSD(const String &fontBasePath) {
#ifdef SMOOTH_FONT
#if CYD_SD_SMOOTH_RUNTIME_ENABLE
  if (!sdReady)
    return false;

  String fullPath = "/" + fontBasePath + ".vlw";
  if (!SD.exists(fullPath))
    return false;

  tft.unloadFont();
  tft.loadFont(fontBasePath, SD);
  smoothFontRuntimeOK = true;
  return true;
#else
  // RC15.134 QUARANTINE:
  // Backtrace'y RC15.131A/132/133 pokazaly wielokrotnie:
  // TFT_eSPI::drawGlyph -> fs::File::read/seek -> FatFS -> LoadProhibited.
  // Nie otwieramy zadnego .vlw z SD podczas pracy UI.
  (void)fontBasePath;
  smoothFontRuntimeOK = false;
  return false;
#endif
#else
  (void)fontBasePath;
  return false;
#endif
}

void unloadSmoothFontSafe() {
#ifdef SMOOTH_FONT
  tft.unloadFont();
#endif
}

bool useUiRegularFont() {
#ifdef SMOOTH_FONT
  // Najpierw maly font aktualnego jezyka.
  if (useLanguageUiFont(false))
    return true;

  // Legacy tylko jako awaryjna zgodnosc. Nie uzywamy go automatycznie
  // w naglowkach, bo jego rozmiar nie jest gwarantowany.
#endif
  return false;
}

bool useUiBoldFont() {
#ifdef SMOOTH_FONT
  if (useLanguageUiFont(true))
    return true;
#endif
  return false;
}


// ============================================================
// RC15.251 - SAFE POLISH UTF-8 RENDERER
// Nie otwiera plikow .vlw z SD. Polskie litery sa skladane z bezpiecznego
// wbudowanego fontu TFT_eSPI + prostego znaku diakrytycznego.
// Cel: zachowac stabilnosc RC15.250 i jednoczesnie pokazac PL znaki.
// ============================================================

struct SafeGlyph {
  char base;
  uint8_t mark; // 0 none, 1 acute, 2 dot, 3 ogonek, 4 stroke
};

bool decodeSafeUtf8(const String &s, int &i, SafeGlyph &g) {
  uint8_t c = (uint8_t)s[i];
  g.base = (char)c;
  g.mark = 0;

  if (c < 0x80) {
    i += 1;
    return true;
  }

  if (i + 1 >= (int)s.length()) {
    i += 1;
    g.base = '?';
    return true;
  }

  uint8_t d = (uint8_t)s[i + 1];

  // mark: 1 acute, 2 dot, 3 ogonek, 4 stroke, 5 caron,
  //       6 diaeresis, 7 ring.
  // PL ---------------------------------------------------------
  if (c == 0xC4) {
    switch (d) {
      case 0x84: g={'A',3}; break; case 0x85: g={'a',3}; break; // Ą ą
      case 0x86: g={'C',1}; break; case 0x87: g={'c',1}; break; // Ć ć
      case 0x98: g={'E',3}; break; case 0x99: g={'e',3}; break; // Ę ę

      // CZ -----------------------------------------------------
      case 0x8C: g={'C',5}; break; case 0x8D: g={'c',5}; break; // Č č
      case 0x8E: g={'D',5}; break; case 0x8F: g={'d',5}; break; // Ď ď
      case 0x9A: g={'E',5}; break; case 0x9B: g={'e',5}; break; // Ě ě
      default: g={'?',0}; break;
    }
    i += 2;
    return true;
  }

  if (c == 0xC5) {
    switch (d) {
      // PL
      case 0x81: g={'L',4}; break; case 0x82: g={'l',4}; break; // Ł ł
      case 0x83: g={'N',1}; break; case 0x84: g={'n',1}; break; // Ń ń
      case 0x9A: g={'S',1}; break; case 0x9B: g={'s',1}; break; // Ś ś
      case 0xB9: g={'Z',1}; break; case 0xBA: g={'z',1}; break; // Ź ź
      case 0xBB: g={'Z',2}; break; case 0xBC: g={'z',2}; break; // Ż ż

      // CZ
      case 0x87: g={'N',5}; break; case 0x88: g={'n',5}; break; // Ň ň
      case 0x98: g={'R',5}; break; case 0x99: g={'r',5}; break; // Ř ř
      case 0xA0: g={'S',5}; break; case 0xA1: g={'s',5}; break; // Š š
      case 0xA4: g={'T',5}; break; case 0xA5: g={'t',5}; break; // Ť ť
      case 0xAE: g={'U',7}; break; case 0xAF: g={'u',7}; break; // Ů ů
      case 0xBD: g={'Z',5}; break; case 0xBE: g={'z',5}; break; // Ž ž
      default: g={'?',0}; break;
    }
    i += 2;
    return true;
  }

  // Latin-1 supplement: DE + CZ + PL Ó.
  if (c == 0xC3) {
    switch (d) {
      // acute
      case 0x81: g={'A',1}; break; case 0xA1: g={'a',1}; break; // Á á
      case 0x89: g={'E',1}; break; case 0xA9: g={'e',1}; break; // É é
      case 0x8D: g={'I',1}; break; case 0xAD: g={'i',1}; break; // Í í
      case 0x93: g={'O',8}; break; case 0xB3: g={'o',8}; break; // Ó ó - nizszy acute
      case 0x9A: g={'U',1}; break; case 0xBA: g={'u',1}; break; // Ú ú
      case 0x9D: g={'Y',1}; break; case 0xBD: g={'y',1}; break; // Ý ý

      // German umlauts
      case 0x84: g={'A',6}; break; case 0xA4: g={'a',6}; break; // Ä ä
      case 0x96: g={'O',6}; break; case 0xB6: g={'o',6}; break; // Ö ö
      case 0x9C: g={'U',6}; break; case 0xBC: g={'u',6}; break; // Ü ü

      // ß: bezpieczny fallback do B; w tekstach menu preferujemy SS,
      // ale ten renderer nie rozszerza jednego glyphu do dwoch znakow.
      case 0x9F: g={'B',0}; break;

      default: g={'?',0}; break;
    }
    i += 2;
    return true;
  }

  i += 2;
  g={'?',0};
  return true;
}

int safeI18nTextWidth(const String &text, uint8_t font) {
  int w = 0;
  for (int i = 0; i < (int)text.length();) {
    SafeGlyph g;
    decodeSafeUtf8(text, i, g);
    char tmp[2] = {g.base, 0};
    w += tft.textWidth(tmp, font);
  }
  return w;
}

void drawSafeI18nGlyphMark(int x, int y, int cw, int ch, uint8_t mark, uint16_t fg) {
  if (!mark) return;

  const int mx = x + cw / 2;

  // RC15.254B:
  // font 1 ~8 px -> tiny
  // font 2 ~16 px -> medium
  // większe fonty -> large
  // Poprzedni podział tiny/reszta powodował, że font 2 dostawał zbyt duże
  // znaki diakrytyczne zaprojektowane dla dużo większych liter.
  const bool tiny  = (ch <= 10);
  const bool medium = (ch > 10 && ch <= 20);

  if (mark == 1) { // acute: Ć Ń Ś Ź + czeskie acute
    if (tiny) {
      // Sprawdzony wariant dla CIŚNIENIE - NIE ZMIENIAMY.
      tft.drawPixel(mx - 1, y - 2, fg);
      tft.drawPixel(mx,     y - 3, fg);
      tft.drawPixel(mx + 1, y - 4, fg);
      tft.drawPixel(mx,     y - 2, fg);
      tft.drawPixel(mx + 1, y - 3, fg);
    } else if (medium) {
      // Font 2: krótki, niski akcent, wizualnie część litery.
      tft.drawPixel(mx - 1, y,     fg);
      tft.drawPixel(mx,     y - 1, fg);
      tft.drawPixel(mx + 1, y - 2, fg);
    } else {
      // Duży font: nieco dłuższy, ale nadal proporcjonalny.
      tft.drawLine(mx - 1, y - 1, mx + 2, y - 4, fg);
    }

  } else if (mark == 2) { // dot: Ż
    if (tiny)
      tft.fillRect(mx - 1, y - 3, 2, 2, fg);
    else if (medium)
      tft.fillRect(mx, y - 2, 2, 2, fg);
    else
      tft.fillCircle(mx, y - 3, 1, fg);

  } else if (mark == 3) { // ogonek: Ą Ę
    if (tiny) {
      tft.drawLine(x + cw - 2, y + ch - 2, x + cw, y + ch, fg);
      tft.drawPixel(x + cw - 1, y + ch + 1, fg);
      tft.drawPixel(x + cw, y + ch + 1, fg);
    } else if (medium) {
      tft.drawPixel(x + cw - 2, y + ch - 1, fg);
      tft.drawPixel(x + cw - 1, y + ch, fg);
    } else {
      tft.drawLine(x + cw - 3, y + ch - 2, x + cw - 1, y + ch, fg);
    }

  } else if (mark == 4) { // stroke: Ł
    if (tiny) {
      // Duże Ł w najmniejszym foncie:
      // kreska startuje na pionowej belce mniej więcej w połowie wysokości.
      int yL = y + ch/2;
      int x1 = x + 1;
      int x2 = x + max(3, min(cw - 1, cw/2 + 1));
      tft.drawLine(x1, yL + 1, x2, yL - 1, fg);
    } else if (medium) {
      // Duże Ł w średnim foncie: ten sam naturalny punkt startu.
      int yL = y + ch/2;
      int x1 = x + max(1, cw/6);
      int x2 = x + min(cw - 1, (2*cw)/3);
      tft.drawLine(x1, yL + 1, x2, yL - 1, fg);
    } else {
      // Duży font bez zmian.
      int yL = y + ch/2 + 1;
      int x1 = x + max(2, cw/4);
      int x2 = x + min(cw - 2, (3*cw)/4);
      tft.drawLine(x1, yL + 1, x2, yL - 1, fg);
    }

  } else if (mark == 5) { // caron: Č Ř Š Ž...
    if (tiny) {
      tft.drawPixel(mx - 2, y - 4, fg);
      tft.drawPixel(mx - 1, y - 3, fg);
      tft.drawPixel(mx,     y - 2, fg);
      tft.drawPixel(mx + 1, y - 3, fg);
      tft.drawPixel(mx + 2, y - 4, fg);
    } else if (medium) {
      tft.drawPixel(mx - 2, y - 2, fg);
      tft.drawPixel(mx - 1, y - 1, fg);
      tft.drawPixel(mx,     y,     fg);
      tft.drawPixel(mx + 1, y - 1, fg);
      tft.drawPixel(mx + 2, y - 2, fg);
    } else {
      tft.drawLine(mx - 3, y - 4, mx, y - 1, fg);
      tft.drawLine(mx, y - 1, mx + 3, y - 4, fg);
    }

  } else if (mark == 6) { // diaeresis: Ä Ö Ü
    if (tiny) {
      tft.fillRect(mx - 3, y - 3, 2, 2, fg);
      tft.fillRect(mx + 2, y - 3, 2, 2, fg);
    } else if (medium) {
      tft.drawPixel(mx - 2, y - 1, fg);
      tft.drawPixel(mx + 2, y - 1, fg);
    } else {
      tft.fillCircle(mx - 3, y - 3, 1, fg);
      tft.fillCircle(mx + 3, y - 3, 1, fg);
    }

  } else if (mark == 7) { // ring: Ů
    tft.drawCircle(mx, tiny ? y - 3 : (medium ? y - 1 : y - 3),
                   tiny ? 1 : (medium ? 1 : 2), fg);

  } else if (mark == 8) { // Ó/ó - osobny, niski acute
    if (tiny) {
      tft.drawPixel(mx - 1, y - 2, fg);
      tft.drawPixel(mx,     y - 3, fg);
      tft.drawPixel(mx + 1, y - 4, fg);
      tft.drawPixel(mx,     y - 2, fg);
    } else if (medium) {
      // Font 2: ten sam subtelny styl co Ś/Ź/Ć.
      tft.drawPixel(mx - 1, y,     fg);
      tft.drawPixel(mx,     y - 1, fg);
      tft.drawPixel(mx + 1, y - 2, fg);
    } else {
      tft.drawLine(mx - 1, y, mx + 2, y - 3, fg);
    }
  }
}

void drawSafeI18nString(const String &text, int x, int y, uint8_t font,
                      uint16_t fg, uint16_t bg, uint8_t datum = TL_DATUM) {
  int totalW = safeI18nTextWidth(text, font);
  int startX = x;
  if (datum == MC_DATUM || datum == TC_DATUM || datum == BC_DATUM) startX -= totalW/2;
  else if (datum == MR_DATUM || datum == TR_DATUM || datum == BR_DATUM) startX -= totalW;

  tft.setTextColor(fg, bg);
  tft.setTextDatum(TL_DATUM);
  int px = startX;
  int ch = tft.fontHeight(font);

  for (int i = 0; i < (int)text.length();) {
    SafeGlyph g;
    decodeSafeUtf8(text, i, g);
    char tmp[2] = {g.base, 0};
    int cw = tft.textWidth(tmp, font);
    tft.drawString(tmp, px, y, font);
    drawSafeI18nGlyphMark(px, y, cw, ch, g.mark, fg);
    px += cw;
  }
}

void drawLangHeader(const String &textUtf8, const String &fallbackAscii,
                    int x, int y, uint16_t fg, uint16_t bg) {
#ifdef SMOOTH_FONT
  if (useLanguageUiFont(true)) {
    tft.setTextColor(fg, bg);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(textUtf8, x, y);
    unloadSmoothFontSafe();
    return;
  }
#endif
  if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ) {
    drawSafeI18nString(textUtf8, x, y, 2, fg, bg, TL_DATUM);
    return;
  }
  tft.setTextColor(fg, bg);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(fallbackAscii, x, y, 2);
}

void drawLangButtonCentered(const String &textUtf8, const String &fallbackAscii,
                            int cx, int cy, uint16_t fg, uint16_t bg, bool bold) {
#ifdef SMOOTH_FONT
  if (useLanguageUiFont(bold)) {
    tft.setTextColor(fg, bg);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(textUtf8, cx, cy);
    tft.setTextDatum(TL_DATUM);
    unloadSmoothFontSafe();
    return;
  }
#endif
  if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ) {
    drawSafeI18nString(textUtf8, cx, cy - 8, 2, fg, bg, MC_DATUM);
    tft.setTextDatum(TL_DATUM);
    return;
  }
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(fallbackAscii, cx, cy, 2);
  tft.setTextDatum(TL_DATUM);
}

void drawLangLabel(const String &textUtf8, const String &fallbackAscii,
                   int x, int y, uint16_t fg, uint16_t bg, bool bold) {
#ifdef SMOOTH_FONT
  if (useLanguageUiFont(bold)) {
    tft.setTextColor(fg, bg);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(textUtf8, x, y);
    unloadSmoothFontSafe();
    return;
  }
#endif
  if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ) {
    drawSafeI18nString(textUtf8, x, y, bold ? 2 : 1, fg, bg, TL_DATUM);
    return;
  }
  tft.setTextColor(fg, bg);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(fallbackAscii, x, y, bold ? 2 : 1);
}








bool useNumbersLargeFont() {
#if CYD_SD_SMOOTH_RUNTIME_ENABLE
#ifdef SMOOTH_FONT
  if (fontNumbersReady)
    return loadSmoothFontSD("CYD/fonts/numbers_large");
#endif
#endif
  return false;
}


void scanFontAssets() {
#ifdef SMOOTH_FONT
  smoothFontSupported = true;
#else
  smoothFontSupported = false;
#endif

  // Stare wspolne fonty zachowujemy tylko dla zgodnosci.
  // UI nie powinno juz zakladac, ze ich rozmiar pasuje wszedzie.
  fontRegularReady = sdReady && SD.exists("/CYD/fonts/ui_regular.vlw");
  fontBoldReady    = sdReady && SD.exists("/CYD/fonts/ui_bold.vlw");
  fontNumbersReady = sdReady && SD.exists("/CYD/fonts/numbers_large.vlw");

  for (uint8_t lang = LANG_PL; lang <= LANG_CZ; lang++) {
    String reg = "/" + languageFontPath(lang, false) + ".vlw";
    String bold = "/" + languageFontPath(lang, true) + ".vlw";
    String small = "/" + languageSmallFontPath(lang) + ".vlw";
    langFontRegularReady[lang] = sdReady && SD.exists(reg);
    langFontBoldReady[lang] = sdReady && SD.exists(bold);
    langFontSmallReady[lang] = sdReady && SD.exists(small);
  }

  Serial.print("[FONT] smooth=");
  Serial.print(smoothFontSupported ? "ON" : "OFF");

  Serial.print(" legacyReg=");
  Serial.print(fontRegularReady ? "OK" : "MISS");

  Serial.print(" legacyBold=");
  Serial.print(fontBoldReady ? "OK" : "MISS");

  Serial.print(" numbers=");
  Serial.println(fontNumbersReady ? "OK" : "MISS");

  const char *codes[] = {"PL", "EN", "DE", "CZ"};
  for (uint8_t lang = LANG_PL; lang <= LANG_CZ; lang++) {
    Serial.print("[FONT-");
    Serial.print(codes[lang]);
    Serial.print("] regular=");
    Serial.print(langFontRegularReady[lang] ? "OK" : "MISSING");
    Serial.print(" bold=");
    Serial.print(langFontBoldReady[lang] ? "OK" : "MISSING");
    Serial.print(" small=");
    Serial.println(langFontSmallReady[lang] ? "OK" : "MISSING");
  }
}


String formatBytesShort(uint64_t bytes) {
  if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
    return String((double)bytes / (1024.0 * 1024.0 * 1024.0), 1) + " GB";
  }
  if (bytes >= 1024ULL * 1024ULL) {
    return String((double)bytes / (1024.0 * 1024.0), 1) + " MB";
  }
  if (bytes >= 1024ULL) {
    return String((double)bytes / 1024.0, 1) + " KB";
  }
  return String((unsigned long)bytes) + " B";
}

bool ensureDir(const char* path) {
  if (SD.exists(path))
    return true;
  return SD.mkdir(path);
}



bool syncHistoryTailStateFromSD(const char *tag) {
#if CYD_SD_ENABLE
  if (!sdReady || !SD.exists(CHART_HISTORY_FILE))
    return false;

  File f = SD.open(CHART_HISTORY_FILE, FILE_READ);
  if (!f)
    return false;

  const uint32_t recSize = sizeof(ChartDiskRecord);
  const uint32_t fullRecords = (uint32_t)f.size() / recSize;
  if (fullRecords == 0) {
    f.close();
    chartSequence = 0;
    lastCommittedHistoryEpoch = 0;
    Serial.print("[HIST-TAIL] ");
    Serial.print(tag ? tag : "");
    Serial.println(" empty=YES seq=0 epoch=0");
    return true;
  }

  // Szukamy od konca pierwszego pelnego i poprawnego rekordu.
  // Nie zakladamy, ze ostatni fizyczny rekord jest dobry.
  const uint32_t maxScan = (fullRecords > 64UL) ? 64UL : fullRecords;

  for (uint32_t back = 0; back < maxScan; ++back) {
    uint32_t index = fullRecords - 1UL - back;
    if (!f.seek(index * recSize))
      break;

    ChartDiskRecord rec = {};
    size_t got = f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec));
    if (got != sizeof(rec))
      continue;

    if (rec.magic != CHART_RECORD_MAGIC ||
        rec.version != CHART_RECORD_VERSION ||
        rec.checksum != chartRecordChecksum(rec))
      continue;

    chartSequence = rec.sequence + 1UL;
    lastCommittedHistoryEpoch = (rec.epoch > 100000UL) ? rec.epoch : 0;

    f.close();

    Serial.print("[HIST-TAIL] ");
    Serial.print(tag ? tag : "");
    Serial.print(" seq=");
    Serial.print(chartSequence);
    Serial.print(" epoch=");
    Serial.println(lastCommittedHistoryEpoch);
    return true;
  }

  f.close();
  Serial.print("[HIST-TAIL] ");
  Serial.print(tag ? tag : "");
  Serial.println(" sync=ERR");
  return false;
#else
  (void)tag;
  return false;
#endif
}

void serviceSDHotplug() {
#if CYD_SD_ENABLE
  static unsigned long lastCheckMs = 0;
  if (millis() - lastCheckMs < 15000UL)
    return;
  lastCheckMs = millis();

  if (sdReady) {
    uint8_t type = SD.cardType();
    bool rootOK = (type != CARD_NONE) && SD.exists("/CYD");

    if (!rootOK) {
      Serial.println("[SD-HOT] LOST");
      SD.end();
      sdReady = false;
      sdCardBytes = 0;
      sdUsedBytes = 0;
      sdTypeName = "NONE";
      // Nie zerujemy buforow 24H/7D - stare dane maja pozostac widoczne.
      return;
    }

    // Lekki refresh statystyk karty.
    sdCardBytes = SD.cardSize();
    sdUsedBytes = SD.usedBytes();
    return;
  }

  // Karta byla niedostepna - probujemy remount bez restartu calego UI.
  if (!SD.begin(CYD_SD_CS_PIN, sdSPI, 4000000))
    return;

  uint8_t type = SD.cardType();
  if (type == CARD_NONE) {
    SD.end();
    return;
  }

  switch (type) {
    case CARD_MMC:  sdTypeName = "MMC"; break;
    case CARD_SD:   sdTypeName = "SDSC"; break;
    case CARD_SDHC: sdTypeName = "SDHC"; break;
    default:        sdTypeName = "UNKNOWN"; break;
  }

  sdCardBytes = SD.cardSize();
  sdUsedBytes = SD.usedBytes();
  sdReady = true;

  bool dirsOK = true;
  dirsOK &= ensureDir("/CYD");
  dirsOK &= ensureDir("/CYD/fonts");
  dirsOK &= ensureDir("/CYD/icons");
  dirsOK &= ensureDir("/CYD/skins");
  dirsOK &= ensureDir("/CYD/data");
  dirsOK &= ensureDir("/CYD/cache");

  bool tailMainOK = repairBinaryTailSafe(
      CHART_HISTORY_FILE, sizeof(ChartDiskRecord), "MAIN",
      "/CYD/.tail_main.tmp", "/CYD/.tail_main.bak");
  bool tailIndoorOK = repairBinaryTailSafe(
      INDOOR_HISTORY_FILE, sizeof(IndoorDiskRecord), "INDOOR",
      "/CYD/.tail_indoor.tmp", "/CYD/.tail_indoor.bak");
  bool tailDirOK = repairBinaryTailSafe(
      WIND_DIR_HISTORY_FILE, sizeof(WindDirDiskRecord), "WIND-DIR",
      "/CYD/.tail_dir.tmp", "/CYD/.tail_dir.bak");
  bool tailCsvOK = repairCsvTailSafe(
      WEATHER_CSV_FILE, "/CYD/.tail_csv.tmp", "/CYD/.tail_csv.bak");

  bool journalWasPending = SD.exists(HISTORY_BUNDLE_JOURNAL_FILE);
  WeatherPacket recoveredData = {};
  uint32_t recoveredEpoch = 0;
  bool journalOK = recoverHistoryBundleJournal(
      journalWasPending ? &recoveredData : nullptr,
      journalWasPending ? &recoveredEpoch : nullptr,
      true);

  // RC15.232: podczas hot-remove commit mogl nie zdazyc dopisac probki do RAM,
  // mimo ze recovery na SD zakonczylo caly bundle. Uzupelniamy RAM tylko raz.
  if (journalOK && journalWasPending && recoveredEpoch > 100000UL) {
    bool alreadyInChartRam = false;
    if (chartCount > 0) {
      int lastIdx = chartIndexOldest(chartCount - 1);
      alreadyInChartRam = (chartHistory[lastIdx].epoch == recoveredEpoch);
    }

    if (!alreadyInChartRam) {
      pushChartSample(recoveredData, recoveredEpoch);
      pushWindDirSample(recoveredData.kierunek_wiatru, recoveredEpoch);
      sdHotplugRamSyncAdded = true;
      Serial.print("[HIST-JRN] RAM-SYNC epoch=");
      Serial.println(recoveredEpoch);
    } else {
      Serial.print("[HIST-JRN] RAM-SYNC skip=EXISTS epoch=");
      Serial.println(recoveredEpoch);
    }
  }

  bool tailStateOK = syncHistoryTailStateFromSD("REMOUNT");

  initWeatherCSVInfo();
  checkArchiveIntegrity();

  // Wymuszamy tylko ponowne odczytanie godzinowych buforow 7D.
  // 24H w RAM zostaje nietkniete, wiec UI nie powinno migac.
  chart7dLoaded = false;
  indoor7dLoaded = false;
  windDir7dLoaded = false;

  Serial.print("[SD-HOT] REMOUNT ");
  Serial.print(dirsOK ? "DIRS=OK " : "DIRS=ERR ");
  Serial.print((tailMainOK && tailIndoorOK && tailDirOK && tailCsvOK) ? "TAIL=OK " : "TAIL=WARN ");
  Serial.print(journalOK ? "JRN=OK " : "JRN=WARN ");
  Serial.print(tailStateOK ? "SEQ=OK " : "SEQ=WARN ");
  Serial.print("type=");
  Serial.println(sdTypeName);
#endif
}

void initCYDSD() {
#if CYD_SD_ENABLE
  sdReady = false;
  sdCardBytes = 0;
  sdUsedBytes = 0;
  sdTypeName = "NONE";

  Serial.println("[SD] Init...");

  // SD na osobnym kontrolerze SPI (HSPI), aby nie kolidowala z dotykiem VSPI.
  sdSPI.begin(18, 19, 23, CYD_SD_CS_PIN);

  if (!SD.begin(CYD_SD_CS_PIN, sdSPI, 4000000)) {
    Serial.println("[SD] Mount FAILED");
    return;
  }

  uint8_t type = SD.cardType();
  if (type == CARD_NONE) {
    Serial.println("[SD] No card");
    SD.end();
    return;
  }

  switch (type) {
    case CARD_MMC:  sdTypeName = "MMC"; break;
    case CARD_SD:   sdTypeName = "SDSC"; break;
    case CARD_SDHC: sdTypeName = "SDHC"; break;
    default:        sdTypeName = "UNKNOWN"; break;
  }

  sdCardBytes = SD.cardSize();
  sdUsedBytes = SD.usedBytes();
  sdReady = true;

  // Struktura projektu. Tworzenie jest idempotentne.
  bool dirsOK = true;
  dirsOK &= ensureDir("/CYD");
  dirsOK &= ensureDir("/CYD/fonts");
  dirsOK &= ensureDir("/CYD/icons");
  dirsOK &= ensureDir("/CYD/skins");
  dirsOK &= ensureDir("/CYD/data");
  dirsOK &= ensureDir("/CYD/cache");

  Serial.print("[SD] OK type=");
  Serial.print(sdTypeName);
  Serial.print(" size=");
  Serial.print(formatBytesShort(sdCardBytes));
  Serial.print(" used=");
  Serial.println(formatBytesShort(sdUsedBytes));

  Serial.print("[SD] DIRS=");
  Serial.println(dirsOK ? "OK" : "ERROR");

  // Lekki test zapisu tylko przy starcie.
  File f = SD.open("/CYD/.rwtest", FILE_WRITE);
  if (f) {
    f.print("OK");
    f.flush();
    f.close();

    File r = SD.open("/CYD/.rwtest", FILE_READ);
    if (r) {
      String s = r.readString();
      r.close();
      Serial.print("[SD] RW=");
      Serial.println(s.startsWith("OK") ? "OK" : "BAD");
    } else {
      Serial.println("[SD] RW=READ_FAIL");
    }

    SD.remove("/CYD/.rwtest");
  } else {
    Serial.println("[SD] RW=WRITE_FAIL");
  }

  // RC15.224: naprawa tylko wtedy, gdy rozmiar pliku wskazuje na niepelny rekord.
  bool tailMainOK = repairBinaryTailSafe(
      CHART_HISTORY_FILE, sizeof(ChartDiskRecord), "MAIN",
      "/CYD/.tail_main.tmp", "/CYD/.tail_main.bak");
  bool tailIndoorOK = repairBinaryTailSafe(
      INDOOR_HISTORY_FILE, sizeof(IndoorDiskRecord), "INDOOR",
      "/CYD/.tail_indoor.tmp", "/CYD/.tail_indoor.bak");
  bool tailDirOK = repairBinaryTailSafe(
      WIND_DIR_HISTORY_FILE, sizeof(WindDirDiskRecord), "WIND-DIR",
      "/CYD/.tail_dir.tmp", "/CYD/.tail_dir.bak");
  bool tailCsvOK = repairCsvTailSafe(
      WEATHER_CSV_FILE, "/CYD/.tail_csv.tmp", "/CYD/.tail_csv.bak");
  bool journalBootOK = recoverHistoryBundleJournal(nullptr, nullptr, true);

  Serial.print("[TAIL] status=");
  Serial.println((tailMainOK && tailIndoorOK && tailDirOK && tailCsvOK && journalBootOK) ? "OK" : "WARNING");
  Serial.print("[HIST-JRN] boot=");
  Serial.println(journalBootOK ? "OK" : "WARNING");

  scanFontAssets();
  loadChartHistorySD();
  loadIndoor24HFromSD();
  loadWindDir24HFromSD();
  initWeatherCSVInfo();
  checkArchiveIntegrity();

#else
  Serial.println("[SD] Disabled");
#endif
}


String dataSourceModeName() {
  switch (dataSourceMode) {
    case SOURCE_LOCAL:
      return currentLanguage == LANG_PL ? "LOKALNE" : "LOCAL";
    case SOURCE_INTERNET:
      return "INTERNET";
    default:
      return "AUTO";
  }
}

String activeDataSourceName() {
  if (activeDataSource == SOURCE_INTERNET)
    return "INTERNET";

  return currentLanguage == LANG_PL ? "LOKALNE" : "LOCAL";
}


bool localSourceFresh() {
  if (!haveData)
    return false;

  return ((millis() - lastPacketMs) / 1000UL) <= LOCAL_SOURCE_TIMEOUT_S;
}

bool internetSourceFresh() {
#if CYD_INTERNET_ENABLE
  if (!internetConnected || !haveInternetData)
    return false;

  return ((millis() - lastInternetPacketMs) / 1000UL) <= INTERNET_SOURCE_TIMEOUT_S;
#else
  return false;
#endif
}

bool activeSourceAvailable() {
  if (activeDataSource == SOURCE_INTERNET)
    return haveInternetData;
  return haveData;
}

uint32_t activeSourceAgeSec() {
  if (activeDataSource == SOURCE_INTERNET) {
    if (!haveInternetData) return 999999UL;
    return (millis() - lastInternetPacketMs) / 1000UL;
  }

  if (!haveData) return 999999UL;
  return (millis() - lastPacketMs) / 1000UL;
}

uint32_t activeSourcePacketCount() {
  return (activeDataSource == SOURCE_INTERNET) ? internetPacketCount : packetCount;
}

int activeSourcePacketLen() {
  return (activeDataSource == SOURCE_INTERNET && haveInternetData)
           ? (int)sizeof(WeatherPacket)
           : lastPacketLen;
}


SourceUiState activeSourceUiState() {
  if (!activeSourceAvailable())
    return SOURCE_UI_WAIT;

  uint32_t age = activeSourceAgeSec();
  if (age <= 15UL) return SOURCE_UI_LIVE;
  if (age <= 60UL) return SOURCE_UI_STALE;
  return SOURCE_UI_OFFLINE;
}

String activeSourceUiText() {
  switch (activeSourceUiState()) {
    case SOURCE_UI_LIVE:    return String(tr(TXT_LIVE));
    case SOURCE_UI_STALE:   return String(tr(TXT_STALE));
    case SOURCE_UI_OFFLINE: return String(tr(TXT_OFFLINE));
    default:                return String(tr(TXT_WAIT));
  }
}

uint16_t activeSourceUiColor(const SkinPalette &p) {
  switch (activeSourceUiState()) {
    case SOURCE_UI_LIVE:    return toneColor(TONE_OK);
    case SOURCE_UI_STALE:   return toneColor(TONE_WARNING);
    case SOURCE_UI_OFFLINE: return toneColor(TONE_DANGER);
    default:                return p.muted;
  }
}

bool sourceUsingFallbackLocal() {
  return dataSourceMode == SOURCE_INTERNET &&
         activeDataSource == SOURCE_LOCAL &&
         localSourceFresh() &&
         !internetSourceFresh();
}

String activeSourceUiMark() {
  if (sourceUsingFallbackLocal())
    return "L*";

  return activeDataSource == SOURCE_INTERNET ? "I" : "L";
}

void updateActiveDataSource() {
  if (dataSourceMode == SOURCE_LOCAL) {
    activeDataSource = SOURCE_LOCAL;
    return;
  }

  if (dataSourceMode == SOURCE_INTERNET) {
    if (internetSourceFresh()) {
      activeDataSource = SOURCE_INTERNET;
      return;
    }

    if (localSourceFresh()) {
      activeDataSource = SOURCE_LOCAL;
      return;
    }

    // Oba zrodla nieswieze: zachowujemy ostatnio aktywne,
    // jesli jego bufor faktycznie istnieje.
    if (activeDataSource == SOURCE_LOCAL && haveData)
      return;

    if (activeDataSource == SOURCE_INTERNET && haveInternetData)
      return;

    if (haveData) {
      activeDataSource = SOURCE_LOCAL;
      return;
    }

    if (haveInternetData) {
      activeDataSource = SOURCE_INTERNET;
      return;
    }

    activeDataSource = SOURCE_LOCAL;
    return;
  }

  // AUTO: swiezy LOCAL ma pierwszenstwo.
  if (localSourceFresh()) {
    activeDataSource = SOURCE_LOCAL;
    return;
  }

  // Potem swiezy INTERNET.
  if (internetSourceFresh()) {
    activeDataSource = SOURCE_INTERNET;
    return;
  }

  // Oba nieswieze: nie zmieniamy zrodla tylko dlatego,
  // ze drugi stary bufor ma inny timestamp.
  if (activeDataSource == SOURCE_LOCAL && haveData)
    return;

  if (activeDataSource == SOURCE_INTERNET && haveInternetData)
    return;

  if (haveData) {
    activeDataSource = SOURCE_LOCAL;
    return;
  }

  if (haveInternetData) {
    activeDataSource = SOURCE_INTERNET;
    return;
  }

  activeDataSource = SOURCE_LOCAL;
}

void rgbLedWrite(bool redOn, bool greenOn, bool blueOn) {
#if CYD_LED_ENABLE
  // RC15.196: globalny OFF ma pierwszenstwo przed kazdym stanem LED,
  // lacznie z bootem, STALE/OFFLINE i alarmami pogodowymi.
  if (!rgbEnabled) {
    redOn = false;
    greenOn = false;
    blueOn = false;
  }

  digitalWrite(CYD_LED_RED_PIN,   redOn   ? LOW : HIGH);
  digitalWrite(CYD_LED_GREEN_PIN, greenOn ? LOW : HIGH);
  digitalWrite(CYD_LED_BLUE_PIN,  blueOn  ? LOW : HIGH);
#endif
}

uint8_t maxWeatherSeverity(const WeatherPacket &d) {
  uint8_t sev = 0;

  VisualState states[] = {
    stateTemperature(d.temperatura),
    stateHumidity(d.wilgotnosc),

    // RC15.185: komfort wewnetrzny jest pelnoprawna czescia globalnego statusu.
    stateTemperature(d.temp_wewnetrzna),
    stateHumidity(d.wilg_wewnetrzna),

    statePressure(d.cisnienie),
    stateWind(d.predkosc_wiatru, false),
    stateWind(d.poryw_wiatru, true),
    stateUV(d.uv_index),
    stateLux(d.swiatlo_lux),
    stateRain(d.opady_godzina)
  };

  for (auto &s : states) {
    if (s.severity > sev)
      sev = s.severity;
  }

  return sev;
}

uint8_t focusDisplayedWeatherSeverity(const WeatherPacket &d) {
  // lastDrawnFocusMetric odpowiada temu, co uzytkownik faktycznie widzi.
  // Gdy FOCUS jeszcze nic nie narysowal, korzystamy z aktualnego slotu.
  uint8_t metric = (lastDrawnFocusMetric <= 10)
                     ? lastDrawnFocusMetric
                     : currentFocusMetric();

  VisualState st = {TONE_OK, 0, "OK"};

  switch (metric) {
    case 0:  st = stateTemperature(d.temperatura); break;       // TEMP ZEW
    case 1:  st = stateHumidity(d.wilgotnosc); break;           // WILG ZEW
    case 2:  st = stateTemperature(d.temp_wewnetrzna); break;   // TEMP WEW
    case 3:  st = stateHumidity(d.wilg_wewnetrzna); break;      // WILG WEW
    case 4:  st = statePressure(d.cisnienie); break;            // CISNIENIE
    case 5:  st = stateWind(d.predkosc_wiatru, false); break;   // WIATR
    case 6:  st = stateWind(d.poryw_wiatru, true); break;       // PORYW
    case 7:  return 0;                                          // KIERUNEK
    case 8:  st = stateUV(d.uv_index); break;                   // UV
    case 9:  st = stateLux(d.swiatlo_lux); break;               // LUX
    case 10: st = stateRain(d.opady_godzina); break;            // OPAD
    default: return 0;
  }

  return st.severity;
}

LedWeatherLevel currentLedLevel() {
  // RC15.183: najpierw stan swiezosci danych, dopiero potem pogoda.
  SourceUiState srcState = activeSourceUiState();

  if (srcState == SOURCE_UI_OFFLINE)
    return LED_LEVEL_OFFLINE;

  if (srcState == SOURCE_UI_STALE)
    return LED_LEVEL_WARNING;

  if (srcState == SOURCE_UI_WAIT)
    return LED_LEVEL_INFO;

  // LIVE: na FOCUS pokazujemy stan TYLKO aktualnie wyswietlanej metryki.
  // Na pozostalych skinach zachowujemy globalny najgorszy stan pogody.
  WeatherPacket d = snapshotData();
  uint8_t sev = (currentSkin == SKIN_FOCUS)
                  ? focusDisplayedWeatherSeverity(d)
                  : maxWeatherSeverity(d);

  if (sev >= 3) return LED_LEVEL_DANGER;
  if (sev == 2) return LED_LEVEL_WARNING;
  if (sev == 1) return LED_LEVEL_INFO;
  return LED_LEVEL_OK;
}

void serviceStatusLed() {
#if CYD_LED_ENABLE
  uint32_t now = millis();
  SourceUiState srcState = activeSourceUiState();

  // RC15.183: lacznosc ma priorytet nad alarmami pogodowymi.
  if (srcState == SOURCE_UI_WAIT) {
    // Wi-Fi / transport moze juz dzialac, ale czekamy na pierwszy swiezy pakiet.
    rgbLedWrite(false, false, true);  // staly niebieski
    return;
  }

  if (srcState == SOURCE_UI_STALE) {
    // To samo znaczenie co przerywana pomaranczowa ramka.
    bool on = (now % 1000UL) < 350UL;
    rgbLedWrite(on, on, false);       // pomaranczowy puls
    return;
  }

  if (srcState == SOURCE_UI_OFFLINE) {
    // To samo znaczenie co pelna czerwona ramka.
    rgbLedWrite(true, false, false);  // staly czerwony
    return;
  }

  // LIVE: dopiero tutaj pokazujemy severity pogody.
  LedWeatherLevel level = currentLedLevel();

  switch (level) {
    case LED_LEVEL_OK:
      // Dyskretny zielony heartbeat.
      rgbLedWrite(false, (now % 5000UL) < 70UL, false);
      break;

    case LED_LEVEL_INFO:
      // Lekkie odchylenie pogodowe.
      rgbLedWrite(false, (now % 5000UL) < 120UL, false);
      break;

    case LED_LEVEL_WARNING: {
      uint32_t t = now % 2500UL;
      bool on = (t < 120UL) || (t >= 260UL && t < 380UL);
      rgbLedWrite(on, on, false);
      break;
    }

    case LED_LEVEL_DANGER:
      rgbLedWrite((now % 700UL) < 260UL, false, false);
      break;

    case LED_LEVEL_OFFLINE:
    default:
      rgbLedWrite(true, false, false);
      break;
  }
#else
  rgbLedWrite(false, false, false);
#endif
}



String currentClockText() {
  if (!ntpReady)
    return "--:--";

  struct tm tmNow;
  if (!getLocalTime(&tmNow, 10))
    return "--:--";

  char buf[6];
  strftime(buf, sizeof(buf), "%H:%M", &tmNow);
  return String(buf);
}

bool scanForConfiguredWifi(String &ssidOut,
                           const char* &passOut,
                           WifiProfile &profileOut,
                           uint8_t &channelOut) {
  if (wifiHomeRoamInProgress) {
    return false;
  }


  // RC15.101: runtime failover bez WiFi.scanNetworks().
  // Profile sa znane na sztywno, wiec probujemy od razu drugi profil.
  if (WiFi.status() != WL_CONNECTED) {
    if (activeWifiProfile == WIFI_PROFILE_HOME) {
      ssidOut = cydWifiRemoteSsid;
      passOut = cydWifiRemotePass.c_str();
      profileOut = WIFI_PROFILE_REMOTE;
      channelOut = 0;
      Serial.println("[NET] failover direct=REMOTE exactSSID (no scan)");
      return true;
    }

    if (activeWifiProfile == WIFI_PROFILE_REMOTE) {
      ssidOut = cydWifiHomeSsid;
      passOut = cydWifiHomePass.c_str();
      profileOut = WIFI_PROFILE_HOME;
      channelOut = 0;
      Serial.println("[NET] failover direct=HOME (no scan)");
      return true;
    }
  }


  // RC15.98A: HOME zostalo przed chwila znalezione przez async scan.
  // Nie uruchamiamy ponownie kosztownego WiFi.scanNetworks().
  if (activeWifiProfile == WIFI_PROFILE_HOME &&
      activeWifiSsid.length() > 0 &&
      (long)(wifiKnownHomeUntilMs - millis()) > 0) {
    ssidOut = activeWifiSsid;
    passOut = cydWifiHomePass.c_str();
    profileOut = WIFI_PROFILE_HOME;
    channelOut = 0;
    Serial.println("[NET] HOME cached - scan skipped");
    return true;
  }


  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) {
    WiFi.scanDelete();
    return false;
  }

  String homeTarget = cydWifiHomeSsid;
  String remoteTarget = cydWifiRemoteSsid;
  homeTarget.trim();
  remoteTarget.trim();

  int remoteIndex = -1;

  for (int i = 0; i < n; ++i) {
    String seen = WiFi.SSID(i);
    String norm = seen;
    norm.trim();

    if (homeTarget.length() > 0 && norm == homeTarget) {
      ssidOut = seen;
      passOut = cydWifiHomePass.c_str();
      profileOut = WIFI_PROFILE_HOME;
      channelOut = (uint8_t)WiFi.channel(i);
      WiFi.scanDelete();
      return true;
    }

    if (remoteIndex < 0 && remoteTarget.length() > 0 && norm == remoteTarget)
      remoteIndex = i;
  }

  if (remoteIndex >= 0) {
    ssidOut = WiFi.SSID(remoteIndex);
    passOut = cydWifiRemotePass.c_str();
    profileOut = WIFI_PROFILE_REMOTE;
    channelOut = (uint8_t)WiFi.channel(remoteIndex);
    WiFi.scanDelete();
    return true;
  }

  WiFi.scanDelete();
  return false;
}

const char* wifiProfileName(WifiProfile p) {
  if (p == WIFI_PROFILE_HOME) return "HOME";
  if (p == WIFI_PROFILE_REMOTE) return "REMOTE";
  return "NONE";
}

bool findWifiChannel(const char* ssid, uint8_t &channelOut, String &actualSsidOut) {
  if (!ssid || !ssid[0])
    return false;

  int n = WiFi.scanNetworks(false, true);

  if (n <= 0) {
    WiFi.scanDelete();
    return false;
  }

  String target = String(ssid);
  String targetNorm = target;
  targetNorm.trim();

  bool found = false;

  for (int i = 0; i < n; i++) {
    String seen = WiFi.SSID(i);
    String seenNorm = seen;
    seenNorm.trim();

    if (!found && seenNorm == targetNorm) {
      channelOut = (uint8_t)WiFi.channel(i);
      actualSsidOut = seen;
      found = true;
    }
  }

  WiFi.scanDelete();

#if CYD_DEBUG
  Serial.print("[NET] scan target='");
  Serial.print(targetNorm);
  Serial.print("' result=");
  Serial.println(found ? "FOUND" : "NOT_FOUND");
#endif

  return found;
}



bool validateWeatherPacket(const WeatherPacket &d) {
  if (!isfinite(d.temperatura)       || d.temperatura < -80.0f || d.temperatura > 80.0f) return false;
  if (!isfinite(d.wilgotnosc)       || d.wilgotnosc < 0.0f    || d.wilgotnosc > 100.0f) return false;
  if (!isfinite(d.cisnienie)        || d.cisnienie < 800.0f   || d.cisnienie > 1200.0f) return false;
  if (!isfinite(d.predkosc_wiatru)  || d.predkosc_wiatru < 0.0f || d.predkosc_wiatru > 100.0f) return false;
  if (!isfinite(d.poryw_wiatru)     || d.poryw_wiatru < 0.0f  || d.poryw_wiatru > 150.0f) return false;
  if (!isfinite(d.opady)            || d.opady < 0.0f         || d.opady > 100000.0f) return false;
  if (!isfinite(d.opady_godzina)    || d.opady_godzina < 0.0f || d.opady_godzina > 1000.0f) return false;
  if (!isfinite(d.uv_index)         || d.uv_index < 0.0f      || d.uv_index > 30.0f) return false;
  if (!isfinite(d.swiatlo_lux)      || d.swiatlo_lux < 0.0f   || d.swiatlo_lux > 300000.0f) return false;
  if (!isfinite(d.kierunek_wiatru)  || d.kierunek_wiatru < 0.0f || d.kierunek_wiatru > 360.0f) return false;
  if (!isfinite(d.temp_wewnetrzna)  || d.temp_wewnetrzna < -40.0f || d.temp_wewnetrzna > 80.0f) return false;
  if (!isfinite(d.wilg_wewnetrzna)  || d.wilg_wewnetrzna < 0.0f || d.wilg_wewnetrzna > 100.0f) return false;
  return true;
}

bool validateInternetPacket(const WeatherPacket &d) {
  return validateWeatherPacket(d);
}

bool extractJsonFloatField(const String &json, const char *key, float &out) {
  String token = "\"";
  token += key;
  token += "\"";

  int p = json.indexOf(token);
  if (p < 0) return false;

  p = json.indexOf(':', p + token.length());
  if (p < 0) return false;
  p++;

  while (p < (int)json.length() && isspace((unsigned char)json[p])) p++;
  if (p >= (int)json.length()) return false;

  const char *startPtr = json.c_str() + p;
  char *endPtr = nullptr;
  float v = strtof(startPtr, &endPtr);

  if (endPtr == startPtr || !isfinite(v))
    return false;

  out = v;
  return true;
}

bool extractJsonStringField(const String &json, const char *key, String &out) {
  String token = "\"";
  token += key;
  token += "\"";

  int p = json.indexOf(token);
  if (p < 0) return false;

  p = json.indexOf(':', p + token.length());
  if (p < 0) return false;
  p++;

  while (p < (int)json.length() && isspace((unsigned char)json[p])) p++;
  if (p >= (int)json.length() || json[p] != '"') return false;
  p++;

  String s;
  bool esc = false;

  for (; p < (int)json.length(); p++) {
    char c = json[p];

    if (esc) {
      if (c == '"' || c == '\\' || c == '/')
        s += c;
      else if (c == 'n')
        s += '\n';
      else if (c == 'r')
        s += '\r';
      else if (c == 't')
        s += '\t';
      else
        return false;

      esc = false;
      continue;
    }

    if (c == '\\') {
      esc = true;
      continue;
    }

    if (c == '"') {
      out = s;
      return true;
    }

    s += c;

    if (s.length() > INTERNET_MAX_PAYLOAD)
      return false;
  }

  return false;
}

bool parseInternetLiveJson(const String &payload, WeatherPacket &out) {
  String s;
  s.reserve(payload.length() + 1);
  s = payload;
  s.trim();

  if (s.length() == 0 || s.length() > INTERNET_MAX_PAYLOAD)
    return false;

  // WeatherPacket jest packed, więc jego pól nie wolno przekazywać
  // jako float&. Najpierw czytamy do zwykłych lokalnych zmiennych.
  float t, h, b, w, g, r, rh, uv, lx, dir, it, ih;

  if (!extractJsonFloatField(s, "t",  t))   return false;
  if (!extractJsonFloatField(s, "h",  h))   return false;
  if (!extractJsonFloatField(s, "b",  b))   return false;
  if (!extractJsonFloatField(s, "w",  w))   return false;
  if (!extractJsonFloatField(s, "g",  g))   return false;
  if (!extractJsonFloatField(s, "r",  r))   return false;
  if (!extractJsonFloatField(s, "rh", rh))  return false;
  if (!extractJsonFloatField(s, "uv", uv))  return false;
  if (!extractJsonFloatField(s, "lx", lx))  return false;
  if (!extractJsonFloatField(s, "d",  dir)) return false;
  if (!extractJsonFloatField(s, "it", it))  return false;
  if (!extractJsonFloatField(s, "ih", ih))  return false;

  WeatherPacket d = {};
  d.temperatura       = t;
  d.wilgotnosc       = h;
  d.cisnienie        = b;
  d.predkosc_wiatru  = w;
  d.poryw_wiatru     = g;
  d.opady             = r;
  d.opady_godzina    = rh;
  d.uv_index          = uv;
  d.swiatlo_lux      = lx;
  d.kierunek_wiatru  = dir;
  d.temp_wewnetrzna  = it;
  d.wilg_wewnetrzna  = ih;

  if (!validateInternetPacket(d))
    return false;

  out = d;
  return true;
}

bool fetchInternetPacket(WeatherPacket &out, String &recordIdOut) {
#if !CYD_INTERNET_ENABLE
  (void)out;
  (void)recordIdOut;
  return false;
#else
  if (!internetConnected || WiFi.status() != WL_CONNECTED) {
    lastInternetError = "WIFI";
    return false;
  }

  if (cydAioUsername.length() == 0 ||
      cydAioKey.length() == 0 ||
      cydAioFeed.length() == 0) {
    lastInternetError = "AIO CFG";
    return false;
  }

  String url;
  url.reserve(128);
  url = "https://io.adafruit.com/api/v2/";
  url += cydAioUsername;
  url += "/feeds/";
  url += cydAioFeed;
  url += "/data/last";

  WiFiClientSecure secure;

  // RC15.82: transport jest jeszcze domyslnie OFF.
  // Przed wlaczeniem produkcyjnym dodamy root CA dla Adafruit IO.
  secure.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(INTERNET_HTTP_TIMEOUT_MS);
  http.setTimeout(INTERNET_HTTP_TIMEOUT_MS);

  if (!http.begin(secure, url)) {
    lastInternetError = "BEGIN";
    return false;
  }

  http.addHeader("X-AIO-Key", cydAioKey);

  int code = http.GET();
  lastInternetHttpCode = code;

  if (code != HTTP_CODE_OK) {
    internetHttpErrors++;
    lastInternetError = "HTTP " + String(code);
    http.end();
    return false;
  }

  String outer = http.getString();
  http.end();

  String rawValue;
  String recordId;

  if (!extractJsonStringField(outer, "value", rawValue) ||
      !extractJsonStringField(outer, "id", recordId)) {
    internetHttpErrors++;
    lastInternetError = "AIO JSON";
    return false;
  }

  if (!parseInternetLiveJson(rawValue, out)) {
    internetHttpErrors++;
    lastInternetError = "PAYLOAD";
    return false;
  }

  recordIdOut = recordId;
  internetHttpOK++;
  lastInternetError = "OK";
  return true;
#endif
}
void internetHttpTask(void *parameter) {
#if CYD_INTERNET_ENABLE
  for (;;) {
    bool doRequest = false;

    portENTER_CRITICAL(&internetHttpMux);
    if (internetHttpRequest && !internetHttpBusy) {
      internetHttpRequest = false;
      internetHttpBusy = true;
      doRequest = true;
    }
    portEXIT_CRITICAL(&internetHttpMux);

    if (!doRequest) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    WeatherPacket d = {};
    String recordId;
    recordId.reserve(32);

    bool ok = false;

    if (WiFi.status() == WL_CONNECTED)
      ok = fetchInternetPacket(d, recordId);

    portENTER_CRITICAL(&internetHttpMux);

    if (ok) {
      memcpy(&internetHttpData, &d, sizeof(d));

      size_t n = recordId.length();
      if (n >= sizeof(internetHttpRecordId))
        n = sizeof(internetHttpRecordId) - 1;

      memcpy(internetHttpRecordId, recordId.c_str(), n);
      internetHttpRecordId[n] = '\0';
    } else {
      internetHttpRecordId[0] = '\0';
    }

    internetHttpOk = ok;
    internetHttpDone = true;
    internetHttpBusy = false;

    portEXIT_CRITICAL(&internetHttpMux);
  }
#else
  vTaskDelete(nullptr);
#endif
}




void onNtpTimeSync(struct timeval *tv) {
  if (!tv) return;
  lastNtpSyncEpoch = (uint32_t)tv->tv_sec;
  lastNtpSyncMs = millis();
  ntpSyncSeen = true;
}

void serviceNtpRetry() {
#if CYD_INTERNET_ENABLE
  if (!internetConnected || WiFi.status() != WL_CONNECTED)
    return;

  if (ntpReady)
    return;

  if (millis() - lastNtpRetryMs < NTP_RETRY_MS)
    return;

  lastNtpRetryMs = millis();

  // SNTP dziala w tle. Tu tylko sprawdzamy, czy czas jest juz poprawny.
  time_t now = time(nullptr);
  if (now > 1700000000LL) {
    ntpReady = true;
    Serial.println("[NET] NTP retry=OK async");
    printHistoryTimeDiagnostics("NTP_RETRY_OK");
  }
#endif
}


void serviceHomeRoamAsync() {
#if CYD_INTERNET_ENABLE
  if (activeWifiProfile != WIFI_PROFILE_REMOTE)
    return;

  bool httpBusyNow;
  portENTER_CRITICAL(&internetHttpMux);
  httpBusyNow = internetHttpBusy || internetHttpRequest;
  portEXIT_CRITICAL(&internetHttpMux);

  if (httpBusyNow)
    return;

  if (!wifiRoamScanPending) {
    if (millis() - lastWifiRoamCheckMs < WIFI_ROAM_CHECK_MS)
      return;

    lastWifiRoamCheckMs = millis();

    int r = WiFi.scanNetworks(true, true);

    if (r == WIFI_SCAN_RUNNING || r >= 0) {
      wifiRoamScanPending = true;
      wifiRoamScanStartedMs = millis();
#if CYD_DEBUG
      Serial.println("[NET] roam scan async=start");
#endif
    }
    return;
  }

  int n = WiFi.scanComplete();

  if (n == WIFI_SCAN_RUNNING) {
    if (millis() - wifiRoamScanStartedMs >= WIFI_ROAM_SCAN_TIMEOUT_MS) {
      WiFi.scanDelete();
      wifiRoamScanPending = false;
#if CYD_DEBUG
      Serial.println("[NET] roam scan async=timeout");
#endif
    }
    return;
  }

  wifiRoamScanPending = false;

  if (n <= 0) {
    WiFi.scanDelete();
    return;
  }

  String homeTarget = cydWifiHomeSsid;
  homeTarget.trim();

  String homeActual;
  uint8_t homeChannel = 0;
  bool homeFound = false;

  for (int i = 0; i < n; ++i) {
    String seen = WiFi.SSID(i);
    String norm = seen;
    norm.trim();

    if (homeTarget.length() > 0 && norm == homeTarget) {
      homeActual = seen;
      homeChannel = (uint8_t)WiFi.channel(i);
      homeFound = true;
      break;
    }
  }

  WiFi.scanDelete();

  if (!homeFound)
    return;

  Serial.print("[NET] HOME wykryte async -> roaming channel=");
  Serial.println(homeChannel);

  activeWifiSsid = homeActual;
  activeWifiProfile = WIFI_PROFILE_HOME;
  internetConnected = false;

  wifiHomeRoamInProgress = true;
  wifiHomeRoamStartedMs = millis();
  Serial.println("[NET] HOME roam state=CONNECTING");
  wifiKnownHomeUntilMs = millis() + WIFI_KNOWN_HOME_MS;

  portENTER_CRITICAL(&internetHttpMux);
  httpBusyNow = internetHttpBusy || internetHttpRequest;
  portEXIT_CRITICAL(&internetHttpMux);

  if (httpBusyNow)
    return;

  WiFi.disconnect(false, false);
  delay(20);

  wifiReconnectPending = true;
  wifiReconnectStartedMs = millis();
  WiFi.begin(activeWifiSsid.c_str(), cydWifiHomePass.c_str());
#endif
}

void serviceInternetTransport() {
#if CYD_INTERNET_ENABLE
  // RC15.246: captive portal ma pierwszenstwo. Nie skanujemy i nie
  // przelaczamy HOME/REMOTE, bo rozjechaloby to AP i kanal ESP-NOW.
  if (cydSetupApMode)
    return;

  wl_status_t st = WiFi.status();

  if (st != WL_CONNECTED) {
    internetConnected = false;

    // RC15.250: runtime HOME+REMOTE outage watchdog.
    // Dotychczas manager mogl krazyc HOME -> REMOTE -> HOME bez konca,
    // przez co startCydSetupAp() byl osiagalny tylko podczas bootu.
    if (wifiOfflineSinceMs == 0) {
      wifiOfflineSinceMs = millis();
      Serial.println("[NET-CFG] runtime offline timer START");
    } else if (millis() - wifiOfflineSinceMs >= WIFI_RUNTIME_SETUP_AP_TIMEOUT_MS) {
      Serial.println("[NET-CFG] runtime offline timeout -> CYD-Setup");
      wifiReconnectPending = false;
      wifiHomeRoamInProgress = false;
      wifiRoamScanPending = false;
      activeWifiProfile = WIFI_PROFILE_NONE;
      activeWifiSsid = "";
      WiFi.disconnect(false, false);
      delay(20);
      startCydSetupAp();
      return;
    }

    if (wifiHomeRoamInProgress) {
      if (millis() - wifiHomeRoamStartedMs < WIFI_HOME_ROAM_TIMEOUT_MS) {
        return;
      }

      wifiHomeRoamInProgress = false;
      wifiReconnectPending = false;
      Serial.println("[NET] HOME roam state=TIMEOUT -> manager retry");
    }

    bool httpBusyNow;
    portENTER_CRITICAL(&internetHttpMux);
    httpBusyNow = internetHttpBusy || internetHttpRequest;
    portEXIT_CRITICAL(&internetHttpMux);

    if (httpBusyNow)
      return;

    if (wifiReconnectPending) {
      if (millis() - wifiReconnectStartedMs < WIFI_RECONNECT_ATTEMPT_TIMEOUT_MS)
        return;

      wifiReconnectPending = false;
      WiFi.disconnect(false, false);
      delay(20);

      String candidateSsid;
      const char* candidatePass = nullptr;
      WifiProfile candidateProfile = WIFI_PROFILE_NONE;
      uint8_t candidateChannel = 0;

      if (scanForConfiguredWifi(candidateSsid, candidatePass, candidateProfile, candidateChannel)) {
        activeWifiSsid = candidateSsid;
        activeWifiProfile = candidateProfile;
        Serial.print("[NET] failover profile=");
        Serial.print(wifiProfileName(activeWifiProfile));
        Serial.print(" targetChannel=");
        if (candidateChannel == 0)
          Serial.println("AUTO");
        else
          Serial.println(candidateChannel);
      } else {
        activeWifiProfile = WIFI_PROFILE_NONE;
      }
    }

    if (millis() - lastInternetConnectTryMs >= INTERNET_CONNECT_RETRY_MS) {
      lastInternetConnectTryMs = millis();

      if (activeWifiProfile == WIFI_PROFILE_NONE || activeWifiSsid.length() == 0) {
        String candidateSsid;
        const char* candidatePass = nullptr;
        WifiProfile candidateProfile = WIFI_PROFILE_NONE;
        uint8_t candidateChannel = 0;

        if (scanForConfiguredWifi(candidateSsid, candidatePass, candidateProfile, candidateChannel)) {
          activeWifiSsid = candidateSsid;
          activeWifiProfile = candidateProfile;
        } else {
          return;
        }
      }

      const char* reconnectPass =
        (activeWifiProfile == WIFI_PROFILE_HOME) ? cydWifiHomePass.c_str() : cydWifiRemotePass.c_str();

      Serial.print("[NET] reconnect profile=");
      Serial.println(wifiProfileName(activeWifiProfile));

      wifiReconnectPending = true;
      wifiReconnectStartedMs = millis();
      WiFi.disconnect(false, false);
      delay(20);
      WiFi.begin(activeWifiSsid.c_str(), reconnectPass);
    }

    return;
  }

  wifiReconnectPending = false;
  wifiOfflineSinceMs = 0;

  if (!internetConnected) {
    internetConnected = true;
    internetChannel = (uint8_t)WiFi.channel();

    if (wifiHomeRoamInProgress && activeWifiProfile == WIFI_PROFILE_HOME) {
      wifiHomeRoamInProgress = false;
      Serial.println("[NET] HOME roam state=CONNECTED");
    }
    activeRadioChannel = internetChannel;
    ntpReady = false;
    lastNtpRetryMs = millis() - NTP_RETRY_MS;

    Serial.print("[NET] reconnected profile=");
    Serial.print(wifiProfileName(activeWifiProfile));
    Serial.print(" IP=");
    Serial.println(WiFi.localIP());
    Serial.print("[NET] reconnected channel=");
    Serial.println(activeRadioChannel);

    Serial.print("[NET] mode after reconnect=");
    if (activeWifiProfile == WIFI_PROFILE_HOME && activeRadioChannel == WIFI_CHANNEL)
      Serial.println("HOME LOCAL+INTERNET");
    else
      Serial.println("REMOTE INTERNET");
  }

    // REMOTE -> HOME: skan asynchroniczny, bez blokowania UI.
  serviceHomeRoamAsync();
  if (wifiReconnectPending || WiFi.status() != WL_CONNECTED)
    return;

  // ----------------------------------------------------------
  // POLLING ADAFRUIT IO - NIEBLOKUJACY
  // ----------------------------------------------------------
  bool workerDone = false;
  bool workerOk = false;
  WeatherPacket workerData = {};
  char workerRecordId[48] = {0};

  portENTER_CRITICAL(&internetHttpMux);

  if (internetHttpDone) {
    workerDone = true;
    workerOk = internetHttpOk;

    if (workerOk) {
      memcpy(&workerData, &internetHttpData, sizeof(workerData));
      memcpy(workerRecordId, internetHttpRecordId, sizeof(workerRecordId));
      workerRecordId[sizeof(workerRecordId) - 1] = '\0';
    }

    internetHttpDone = false;
  }

  bool workerBusyNow = internetHttpBusy;
  portEXIT_CRITICAL(&internetHttpMux);

  if (workerDone && workerOk) {
    String recordId = String(workerRecordId);

    if (recordId != lastInternetRecordId) {
      lastInternetRecordId = recordId;

      portENTER_CRITICAL(&dataMux);
      memcpy(&internetData, &workerData, sizeof(workerData));
      portEXIT_CRITICAL(&dataMux);

      haveInternetData = true;
      lastInternetPacketMs = millis();
      internetPacketCount++;
      newInternetData = true;

#if CYD_DEBUG
      Serial.print("[NET-DATA] NEW ASYNC #");
      Serial.print(internetPacketCount);
      Serial.print(" id=");
      Serial.print(recordId);
      Serial.print(" T=");
      Serial.print(workerData.temperatura, 1);
      Serial.print(" H=");
      Serial.print(workerData.wilgotnosc, 0);
      Serial.print(" P=");
      Serial.println(workerData.cisnienie, 1);
#endif
    }
  }

  if (!workerBusyNow &&
      !wifiReconnectPending &&
      !wifiRoamScanPending &&
      WiFi.status() == WL_CONNECTED &&
      millis() - lastInternetPollMs >= INTERNET_POLL_MS) {

    lastInternetPollMs = millis();

    portENTER_CRITICAL(&internetHttpMux);
    if (!internetHttpBusy && !internetHttpRequest)
      internetHttpRequest = true;
    portEXIT_CRITICAL(&internetHttpMux);
  }

#endif
}

void setupInternetSafe() {
#if CYD_INTERNET_ENABLE
  Serial.println("[NET] Wi-Fi dual profile start...");

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  WiFi.disconnect(false, false);
  delay(300);

  String selectedSsid;
  const char* selectedPass = nullptr;
  WifiProfile selectedProfile = WIFI_PROFILE_NONE;
  uint8_t selectedChannel = 0;

  if (!scanForConfiguredWifi(selectedSsid, selectedPass, selectedProfile, selectedChannel)) {
    Serial.println("[NET] HOME/REMOTE niewidoczne - fallback LOCAL ch=8");
    internetConnected = false;
    internetChannel = 0;
    activeRadioChannel = WIFI_CHANNEL;
    activeWifiProfile = WIFI_PROFILE_NONE;
    activeWifiSsid = "";
    return;
  }

  activeWifiSsid = selectedSsid;
  activeWifiProfile = selectedProfile;

  Serial.print("[NET] wybrano=");
  Serial.print(wifiProfileName(activeWifiProfile));
  Serial.print(" channel=");
  Serial.println(selectedChannel);

  WiFi.begin(activeWifiSsid.c_str(), selectedPass);

  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000UL)
    delay(250);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("[NET] connect FAIL profile=");
    Serial.println(wifiProfileName(activeWifiProfile));
    WiFi.disconnect(false, false);
    internetConnected = false;
    internetChannel = 0;
    activeRadioChannel = WIFI_CHANNEL;
    activeWifiProfile = WIFI_PROFILE_NONE;
    return;
  }

  internetConnected = true;
  internetChannel = (uint8_t)WiFi.channel();
  activeRadioChannel = internetChannel;

  Serial.print("[NET] hostname=");
  Serial.println(WiFi.getHostname());

  Serial.print("[NET] Wi-Fi OK profile=");
  Serial.print(wifiProfileName(activeWifiProfile));
  Serial.print(" IP=");
  Serial.println(WiFi.localIP());
  Serial.print("[NET] channel=");
  Serial.println(internetChannel);

  if (activeWifiProfile == WIFI_PROFILE_HOME && internetChannel == WIFI_CHANNEL)
    Serial.println("[NET] mode=HOME LOCAL+INTERNET");
  else
    Serial.println("[NET] mode=REMOTE INTERNET");

  // RC15.237: callback tylko obserwuje rzeczywiste synchronizacje.
  // Domyslnego interwalu SNTP nie zmieniamy.
  sntp_set_time_sync_notification_cb(onNtpTimeSync);
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", NTP_SERVER_1, NTP_SERVER_2);
  struct tm tmNow;
  ntpReady = getLocalTime(&tmNow, 3000);
  lastNtpRetryMs = millis();
  Serial.print("[NET] NTP=");
  Serial.println(ntpReady ? "OK" : "BRAK");
  printHistoryTimeDiagnostics("BOOT");
#else
  internetConnected = false;
  ntpReady = false;
  activeRadioChannel = WIFI_CHANNEL;
  activeWifiProfile = WIFI_PROFILE_NONE;
#endif
}




uint8_t currentSkin = SKIN_GRID;
uint8_t alertStyle = ALERT_BORDER;
uint8_t brightnessLevel = 100;
bool autoIndoorOutdoor = true;
uint8_t rotateSeconds = 8;

// RC15.196: nowe ustawienia wizualne, domyslnie wlaczone.
bool rgbEnabled = true;
bool freshnessFrameEnabled = true;
uint8_t settingsMenuPage = 0;  // 0 = glowne, 1 = RGB/RAMKA

bool showIndoor = false;
unsigned long lastIndoorToggle = 0;

bool menuOpen = false;
bool diagOpen = false;
bool diagForceValueRefresh = true;
bool settingsDirty = false;
bool chartSettingsSavePending = false;
unsigned long touchActionBlockUntilMs = 0;
bool waitTouchRelease = false;
bool redrawAfterTouchRelease = false;
bool closeMenuAfterTouchRelease = false;
// RC15.130A: blokada podwojnego redraw po przejsciu overlay -> skin.
bool uiTransitionRedrawnThisLoop = false;

bool focusProgressSyncPending = false;

// RC15.131 - diagnostyka resetow
RTC_DATA_ATTR uint32_t rtcBootCount = 0;
RTC_DATA_ATTR uint32_t rtcLastMark = 0;
RTC_DATA_ATTR int16_t rtcLastTouchX = -1;
RTC_DATA_ATTR int16_t rtcLastTouchY = -1;
RTC_DATA_ATTR uint8_t rtcLastSkin = 255;
RTC_DATA_ATTR uint8_t rtcLastMenu = 0;
RTC_DATA_ATTR uint8_t rtcLastFocusCfg = 0;

extern bool focusConfigOpen;

static inline void markResetDiag(uint32_t code, int x = -1, int y = -1) {
  rtcLastMark = code;
  rtcLastTouchX = x;
  rtcLastTouchY = y;
  rtcLastSkin = currentSkin;
  rtcLastMenu = menuOpen ? 1 : 0;
  rtcLastFocusCfg = focusConfigOpen ? 1 : 0;
}

const char* resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

// Pelne czyszczenie tylko przy zmianie widoku.
bool forceScreenClear = true;
bool deferTileLabels = false;
bool deferChartLabels = false;
bool preserveChartsFooterOnDraw = false;

bool calibrationOpen = false;
uint8_t calibrationStep = 0;

// Domyslna transformacja afiniczna z dzialajacej kalibracji referencyjnego CYD.
// x = ax*rawX + bx*rawY + cx
// y = ay*rawX + by*rawY + cy
float touchAx = 0.134773222f;
float touchBx = 0.039948619f;
float touchCx = -168.656903f;

float touchAy = 0.036135540f;
float touchBy = 0.103220020f;
float touchCy = -154.562385f;

bool touchCalibrationSaved = false;

struct TouchCalibrationPoint {
  int screenX;
  int screenY;
  int rawX;
  int rawY;
};

TouchCalibrationPoint calPoints[5];

bool touchReady = false;

// ============================================================
// COLORS / SKIN
// ============================================================


uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return tft.color565(r, g, b);
}

SkinPalette paletteFor(uint8_t skin) {
  SkinPalette p;

  if (skin == SKIN_DASH) {
    p.bg     = rgb(7, 13, 20);
    p.panel  = rgb(14, 25, 36);
    p.panel2 = rgb(20, 34, 48);
    p.text   = TFT_WHITE;
    p.muted  = rgb(125, 150, 170);
    p.accent = rgb(0, 205, 220);
    p.border = rgb(45, 65, 82);
    p.footer = rgb(13, 22, 32);
  }
  else if (skin == SKIN_COMPASS) {
    p.bg     = rgb(4, 10, 14);
    p.panel  = rgb(10, 18, 24);
    p.panel2 = rgb(8, 28, 34);
    p.text   = TFT_WHITE;
    p.muted  = rgb(120, 155, 165);
    p.accent = rgb(0, 220, 190);
    p.border = rgb(35, 75, 80);
    p.footer = rgb(7, 18, 22);
  }
  else if (skin == SKIN_INSTRUMENT) {
    p.bg     = rgb(10, 8, 5);
    p.panel  = rgb(24, 19, 12);
    p.panel2 = rgb(34, 27, 15);
    p.text   = TFT_WHITE;
    p.muted  = rgb(180, 160, 120);
    p.accent = rgb(255, 195, 60);
    p.border = rgb(90, 70, 35);
    p.footer = rgb(18, 14, 9);
  }
  else if (skin == SKIN_MINIMAL) {
    p.bg     = TFT_BLACK;
    p.panel  = rgb(14, 14, 14);
    p.panel2 = rgb(24, 24, 24);
    p.text   = TFT_WHITE;
    p.muted  = rgb(145, 145, 145);
    p.accent = TFT_WHITE;
    p.border = rgb(60, 60, 60);
    p.footer = rgb(18, 18, 18);
  }
  else {
    p.bg     = TFT_BLACK;
    p.panel  = rgb(8, 12, 18);
    p.panel2 = rgb(14, 20, 28);
    p.text   = TFT_WHITE;
    p.muted  = rgb(135, 150, 165);
    p.accent = TFT_CYAN;
    p.border = rgb(50, 60, 72);
    p.footer = rgb(25, 25, 35);
  }

  return p;
}

uint16_t toneColor(Tone tone) {
  switch (tone) {
    case TONE_OK:        return rgb(60, 210, 115);
    case TONE_INFO:      return rgb(70, 170, 235);
    case TONE_COLD:      return rgb(55, 155, 255);
    case TONE_VERY_COLD: return rgb(100, 210, 255);
    case TONE_WARM:      return rgb(255, 175, 50);
    case TONE_HOT:       return rgb(255, 75, 55);
    case TONE_WARNING:   return rgb(255, 205, 55);
    case TONE_DANGER:    return rgb(255, 55, 70);
    case TONE_DIM:       return rgb(110, 125, 175);
    case TONE_BRIGHT:    return rgb(255, 210, 80);
    case TONE_RAIN:      return rgb(70, 150, 255);
    default:             return rgb(180, 190, 200);
  }
}

uint16_t softFillForTone(Tone tone, uint16_t fallback) {
  switch (tone) {
    case TONE_VERY_COLD: return rgb(0, 25, 55);
    case TONE_COLD:      return rgb(0, 20, 42);
    case TONE_WARM:      return rgb(55, 32, 0);
    case TONE_HOT:       return rgb(65, 8, 5);
    case TONE_WARNING:   return rgb(55, 44, 0);
    case TONE_DANGER:    return rgb(65, 5, 12);
    case TONE_RAIN:      return rgb(5, 25, 55);
    case TONE_BRIGHT:    return rgb(50, 42, 5);
    default:             return fallback;
  }
}

// ============================================================
// CLASSIFICATION RULES
//
// These are visual/weather comfort rules, not medical alarms.
// ============================================================

VisualState stateTemperature(float v) {
  if (!isfinite(v)) return {TONE_NEUTRAL, 0, "--"};
  if (v <= 0.0f)    return {TONE_VERY_COLD, 3, tr(TXT_VERY_COLD)};
  if (v < 5.0f)     return {TONE_COLD, 2, tr(TXT_COLD)};
  if (v <= 26.0f)   return {TONE_OK, 0, tr(TXT_OK)};
  if (v < 30.0f)    return {TONE_WARM, 1, tr(TXT_WARM)};
  if (v < 35.0f)    return {TONE_HOT, 2, tr(TXT_HOT)};
  return                  {TONE_DANGER, 3, tr(TXT_VERY_HOT)};
}

VisualState stateHumidity(float v) {
  if (!isfinite(v)) return {TONE_NEUTRAL, 0, "--"};
  if (v < 20.0f)    return {TONE_DANGER, 3, tr(TXT_VERY_DRY)};
  if (v < 30.0f)    return {TONE_WARNING, 1, tr(TXT_DRY)};
  if (v <= 60.0f)   return {TONE_OK, 0, tr(TXT_OK)};
  if (v <= 75.0f)   return {TONE_INFO, 1, tr(TXT_HUMID)};
  return                  {TONE_WARNING, 2, tr(TXT_VERY_HUMID)};
}

VisualState statePressure(float v) {
  if (!isfinite(v) || v < 100.0f) return {TONE_NEUTRAL, 0, "--"};
  if (v < 950.0f)   return {TONE_DANGER, 3, tr(TXT_VERY_LOW)};
  if (v < 970.0f)   return {TONE_COLD, 1, tr(TXT_LOW)};
  if (v <= 1035.0f) return {TONE_OK, 0, tr(TXT_OK)};
  if (v <= 1045.0f) return {TONE_WARM, 1, tr(TXT_HIGH)};
  return                  {TONE_WARNING, 2, tr(TXT_VERY_HIGH)};
}

VisualState stateWind(float v, bool gust) {
  if (!isfinite(v)) return {TONE_NEUTRAL, 0, "--"};

  float a = gust ? 8.0f : 5.0f;
  float b = gust ? 14.0f : 10.0f;
  float c = gust ? 20.0f : 15.0f;

  if (v < a) return {TONE_OK, 0, tr(TXT_OK)};
  if (v < b) return {TONE_INFO, 1, tr(TXT_MEDIUM)};
  if (v < c) return {TONE_WARNING, 2, tr(TXT_STRONG)};
  return          {TONE_DANGER, 3, tr(TXT_VERY_STRONG)};
}

VisualState stateUV(float v) {
  if (!isfinite(v)) return {TONE_NEUTRAL, 0, "--"};
  if (v < 3.0f)     return {TONE_OK, 0, tr(TXT_LOW)};
  if (v < 6.0f)     return {TONE_WARNING, 1, tr(TXT_MEDIUM)};
  if (v < 8.0f)     return {TONE_WARM, 2, tr(TXT_HIGH)};
  if (v < 11.0f)    return {TONE_HOT, 3, tr(TXT_VERY_HIGH)};
  return                  {TONE_DANGER, 3, tr(TXT_EXTREME)};
}

VisualState stateLux(float v) {
  if (!isfinite(v) || v < 0) return {TONE_NEUTRAL, 0, "--"};
  if (v < 50.0f)     return {TONE_DIM, 0, tr(TXT_NIGHT)};
  if (v < 1000.0f)   return {TONE_INFO, 0, tr(TXT_DARK)};
  if (v < 50000.0f)  return {TONE_OK, 0, tr(TXT_DAY)};
  if (v < 100000.0f) return {TONE_BRIGHT, 1, tr(TXT_SUN)};
  return                   {TONE_WARNING, 1, tr(TXT_BRIGHT)};
}

VisualState stateRain(float v) {
  if (!isfinite(v)) return {TONE_NEUTRAL, 0, "--"};
  if (v <= 0.05f)   return {TONE_OK, 0, tr(TXT_DRY)};
  if (v < 2.0f)     return {TONE_RAIN, 0, tr(TXT_RAINING)};
  if (v < 10.0f)    return {TONE_INFO, 1, tr(TXT_RAIN)};
  return                  {TONE_WARNING, 2, tr(TXT_HEAVY_RAIN)};
}

// ============================================================
// SETTINGS STORAGE
// ============================================================

extern uint16_t focusEnabledMask;  // RC15.124A forward declaration

void loadSettings() {
  prefs.begin("cyd_ui", true);

  currentSkin = prefs.getUChar("skin", SKIN_GRID);
  alertStyle = prefs.getUChar("alert", ALERT_BORDER);
  brightnessLevel = prefs.getUChar("bright", 100);
  autoIndoorOutdoor = prefs.getBool("autoio", true);
  rotateSeconds = prefs.getUChar("rotate", 8);
  currentLanguage = prefs.getUChar("lang", LANG_PL);
  windDirNotationMode = prefs.getUChar("wind_dir", WIND_DIR_GLOBAL);
  dataSourceMode = prefs.getUChar("source", SOURCE_AUTO);
  chartRange = prefs.getUChar("chart_rng", CHART_RANGE_24H);
  chartPage = prefs.getUChar("chart_pg", CHART_PAGE_BASIC);
  rgbEnabled = prefs.getBool("rgb_on", true);
  freshnessFrameEnabled = prefs.getBool("frame_on", true);

  touchCalibrationSaved = prefs.getBool("tcal_ok", false);
  touchAx = prefs.getFloat("tax", 0.134773222f);
  touchBx = prefs.getFloat("tbx", 0.039948619f);
  touchCx = prefs.getFloat("tcx", -168.656903f);
  touchAy = prefs.getFloat("tay", 0.036135540f);
  touchBy = prefs.getFloat("tby", 0.103220020f);
  touchCy = prefs.getFloat("tcy", -154.562385f);

  focusEnabledMask = prefs.getUShort("focus_mask", 0x07FF) & 0x07FF;
if (focusEnabledMask == 0) focusEnabledMask = 0x07FF;

prefs.end();

  if (currentSkin > SKIN_CHARTS) currentSkin = SKIN_GRID;
  if (alertStyle > ALERT_PANEL) alertStyle = ALERT_BORDER;
  if (brightnessLevel != 20 && brightnessLevel != 40 &&
      brightnessLevel != 70 && brightnessLevel != 100) {
    brightnessLevel = 100;
  }
  if (rotateSeconds != 8 && rotateSeconds != 12 &&
      rotateSeconds != 20) {
    rotateSeconds = 8;
  }
  if (currentLanguage > LANG_CZ)
    currentLanguage = LANG_PL;
  if (windDirNotationMode > WIND_DIR_GLOBAL)
    windDirNotationMode = WIND_DIR_GLOBAL;
  if (dataSourceMode > SOURCE_INTERNET)
    dataSourceMode = SOURCE_AUTO;
  if (chartRange > CHART_RANGE_7D)
    chartRange = CHART_RANGE_24H;
  if (chartPage > CHART_PAGE_ALERTS)
    chartPage = CHART_PAGE_BASIC;
}

void saveSettings() {
  markResetDiag(300);
  prefs.begin("cyd_ui", false);

  prefs.putUChar("skin", currentSkin);
  prefs.putUChar("alert", alertStyle);
  prefs.putUChar("bright", brightnessLevel);
  prefs.putBool("autoio", autoIndoorOutdoor);
  prefs.putUChar("rotate", rotateSeconds);
  prefs.putUChar("lang", currentLanguage);
  prefs.putUChar("wind_dir", windDirNotationMode);
  prefs.putUChar("source", dataSourceMode);
  prefs.putUChar("chart_rng", chartRange);
  prefs.putUChar("chart_pg", chartPage);
  prefs.putBool("rgb_on", rgbEnabled);
  prefs.putBool("frame_on", freshnessFrameEnabled);

  prefs.putUShort("focus_mask", focusEnabledMask & 0x07FF);

prefs.end();
}

void saveTouchCalibration() {
  Preferences p;
  p.begin("cyd_ui", false);
  p.putBool("tcal_ok", true);
  p.putFloat("tax", touchAx);
  p.putFloat("tbx", touchBx);
  p.putFloat("tcx", touchCx);
  p.putFloat("tay", touchAy);
  p.putFloat("tby", touchBy);
  p.putFloat("tcy", touchCy);
  p.end();

  touchCalibrationSaved = true;
}

void resetTouchCalibrationToDefault() {
  touchAx = 0.134773222f;
  touchBx = 0.039948619f;
  touchCx = -168.656903f;

  touchAy = 0.036135540f;
  touchBy = 0.103220020f;
  touchCy = -154.562385f;

  Preferences p;
  p.begin("cyd_ui", false);
  p.putBool("tcal_ok", false);
  p.putFloat("tax", touchAx);
  p.putFloat("tbx", touchBx);
  p.putFloat("tcx", touchCx);
  p.putFloat("tay", touchAy);
  p.putFloat("tby", touchBy);
  p.putFloat("tcy", touchCy);
  p.end();

  touchCalibrationSaved = false;
}


// ============================================================
// BACKLIGHT
// ============================================================

void applyBrightness() {
  int pwm = map(brightnessLevel, 0, 100, 0, 255);
  ledcWrite(0, pwm);
}

// ============================================================
// ESP-NOW
// ============================================================

void onReceive(const uint8_t *mac, const uint8_t *data, int len) {
  // RC15.241: tylko fizyczny nadajnik LilyGO moze odswiezac LOCAL.
  if (mac == nullptr || memcmp(mac, LILYGO_ESPNOW_MAC, 6) != 0) {
    foreignEspNowPacketCount++;
    return;
  }

  lastPacketLen = len;

  if (len != (int)sizeof(WeatherPacket))
    return;

  portENTER_CRITICAL_ISR(&dataMux);
  memcpy(&pendingLocalData, data, sizeof(pendingLocalData));
  // RC15.240: callback tylko staginguje pakiet. Dopiero loop() po walidacji
  // promuje go do liveData i odswieza znacznik swiezosci LOCAL.
  newData = true;
  portEXIT_CRITICAL_ISR(&dataMux);
}

WeatherPacket snapshotLocalData() {
  WeatherPacket d = {};

  portENTER_CRITICAL(&dataMux);
  memcpy(&d, &liveData, sizeof(d));
  portEXIT_CRITICAL(&dataMux);

  return d;
}

WeatherPacket snapshotInternetData() {
  WeatherPacket d = {};

  portENTER_CRITICAL(&dataMux);
  memcpy(&d, &internetData, sizeof(d));
  portEXIT_CRITICAL(&dataMux);

  return d;
}

WeatherPacket snapshotData() {
  // UI i historia pobieraja dane z rzeczywiscie aktywnego zrodla.
  if (activeDataSource == SOURCE_INTERNET && haveInternetData)
    return snapshotInternetData();

  return snapshotLocalData();
}

// ============================================================
// FORMAT
// ============================================================

String windDir(float deg) {
  if (!isfinite(deg))
    return "--";

  while (deg < 0.0f) deg += 360.0f;
  while (deg >= 360.0f) deg -= 360.0f;

  // GLOBAL: klasyczna międzynarodowa róża 16-kierunkowa.
  // Zawsze N/E/S/W niezależnie od języka interfejsu.
  if (windDirNotationMode == WIND_DIR_GLOBAL) {
    static const char* dirsGlobal[16] = {
      "N", "NNE", "NE", "ENE",
      "E", "ESE", "SE", "SSE",
      "S", "SSW", "SW", "WSW",
      "W", "WNW", "NW", "NNW"
    };
    int idx16 = (int)((deg + 11.25f) / 22.5f) & 15;
    return String(dirsGlobal[idx16]);
  }

  // LOKALNY: oznaczenia zależne od języka UI.
  if (currentLanguage == LANG_PL) {
    static const char* dirsPL[8] = {
      "PN", "PNW", "W", "PDW",
      "PD", "PDZ", "Z", "PNZ"
    };
    int idx8 = (int)((deg + 22.5f) / 45.0f) & 7;
    return String(dirsPL[idx8]);
  }

  if (currentLanguage == LANG_EN) {
    static const char* dirsEN[16] = {
      "N", "NNE", "NE", "ENE",
      "E", "ESE", "SE", "SSE",
      "S", "SSW", "SW", "WSW",
      "W", "WNW", "NW", "NNW"
    };
    int idx16 = (int)((deg + 11.25f) / 22.5f) & 15;
    return String(dirsEN[idx16]);
  }

  if (currentLanguage == LANG_DE) {
    static const char* dirsDE[16] = {
      "N", "NNO", "NO", "ONO",
      "O", "OSO", "SO", "SSO",
      "S", "SSW", "SW", "WSW",
      "W", "WNW", "NW", "NNW"
    };
    int idx16 = (int)((deg + 11.25f) / 22.5f) & 15;
    return String(dirsDE[idx16]);
  }

  static const char* dirsCZ[16] = {
    "S", "SSV", "SV", "VSV",
    "V", "VJV", "JV", "JJV",
    "J", "JJZ", "JZ", "ZJZ",
    "Z", "ZSZ", "SZ", "SSZ"
  };
  int idx16 = (int)((deg + 11.25f) / 22.5f) & 15;
  return String(dirsCZ[idx16]);
}

String fmt(float v, uint8_t dec) {
  if (!isfinite(v))
    return "--";
  return String(v, (unsigned int)dec);
}

String fmtSigned(float v, uint8_t dec) {
  if (!isfinite(v))
    return "--";

  String s;
  if (v > 0.0f)
    s += "+";

  s += String(v, (unsigned int)dec);
  return s;
}

String fmtPressure(float v) {
  if (!isfinite(v) || v < 100.0f)
    return "--";
  return String(v, 0);
}

String fmtLux(float v, String &unit) {
  if (!isfinite(v) || v < 0) {
    unit = "";
    return "--";
  }

  if (v >= 1000.0f) {
    unit = "kLx";

    if (v >= 100000.0f)
      return String(v / 1000.0f, 0);

    return String(v / 1000.0f, 1);
  }

  unit = "Lx";
  return String(v, 0);
}

// ============================================================
// DRAW HELPERS
// ============================================================

void drawHeader(const String &title) {
  SkinPalette p = paletteFor(currentSkin);

  static String lastHeaderTitle = "";
  static uint8_t lastHeaderSkin = 255;
  static uint8_t lastHeaderLanguage = 255;

  bool titleChanged =
      forceScreenClear ||
      lastHeaderTitle != title ||
      lastHeaderSkin != currentSkin ||
      lastHeaderLanguage != currentLanguage;

  // Lewa czesc naglowka z tytulem jest rysowana TYLKO po zmianie ekranu,
  // jezyka lub wymuszonym pelnym redraw. Nowy pakiet danych jej nie dotyka.
  if (titleChanged) {
    tft.fillRect(0, 0, 225, 27, p.panel2);

    if (currentLanguage == LANG_PL) {
      String plTitle = title;

      if (title == "POGODA - SIATKA")
        plTitle = "POGODA - SIATKA";
      else if (title == "POGODA - PANEL")
        plTitle = "POGODA - PANEL";
      else if (title == "POGODA - MINIMAL")
        plTitle = "POGODA - MINIMAL";
      else if (title == "KOMPAS WIATRU")
        plTitle = "KOMPAS WIATRU";
      else if (title == "WSKAZNIKI")
        plTitle = "WSKAŹNIKI";
      else if (title == "POGODA - WYKRESY")
        plTitle = "POGODA - WYKRESY";

      drawLangHeader(plTitle, title, 8, 4, p.accent, p.panel2);
    } else {
      drawLangHeader(title, title, 8, 4, p.accent, p.panel2);
    }

    lastHeaderTitle = title;
    lastHeaderSkin = currentSkin;
    lastHeaderLanguage = currentLanguage;
  }

  refreshHeaderStatusOnly();
}


void drawChartsPageIndicator() {
  if (currentSkin != SKIN_CHARTS)
    return;

  SkinPalette p = paletteFor(currentSkin);

  // DWA niezalezne wskazniki:
  // 1) PAGE: 7 segmentow szerokich dokladnie jak przycisk PAGE (80 px).
  // 2) RANGE: 4 segmenty szerokie dokladnie jak przycisk RANGE (62 px).
  //
  // Wskazniki maja tylko 2 px wysokosci i sa przesuniete nizej,
  // aby nie zahaczaly o RX / sekundy w wierszu info.

  // ---------- PAGE ----------
  {
    const int count = 7;
    const int x0 = 2;          // identyczny poczatek jak przycisk PAGE
    const int totalW = 80;     // identyczna szerokosc jak przycisk PAGE
    const int gap = 2;
    const int segW = (totalW - (count - 1) * gap) / count;
    const int y = 214;

    // Osobna linia y=214..215, poza dynamicznym info-row 202..213.
    tft.fillRect(x0, y, totalW, 2, p.bg);

    for (int i = 0; i < count; i++) {
      int x = x0 + i * (segW + gap);
      int w = segW;

      // Ostatni segment dopelnia ewentualna reszte z dzielenia calkowitego,
      // dzieki czemu caly pasek ma DOKLADNIE 80 px.
      if (i == count - 1)
        w = totalW - (x - x0);

      uint16_t c = (i == chartPage) ? p.accent : p.border;
      tft.fillRect(x, y, w, 2, c);
    }
  }

  // ---------- RANGE ----------
  {
    const int count = 4;
    const int x0 = 112;        // identyczny poczatek jak przycisk RANGE
    const int totalW = 62;     // identyczna szerokosc jak przycisk RANGE
    const int gap = 3;
    const int segW = (totalW - (count - 1) * gap) / count;
    const int y = 214;

    // Osobna linia y=214..215, poza dynamicznym info-row 202..213.
    tft.fillRect(x0, y, totalW, 2, p.bg);

    for (int i = 0; i < count; i++) {
      int x = x0 + i * (segW + gap);
      int w = segW;

      if (i == count - 1)
        w = totalW - (x - x0);

      uint16_t c = (i == chartRange) ? p.accent : p.border;
      tft.fillRect(x, y, w, 2, c);
    }
  }


  // ---------- ZEW / WEW ----------
  if (chartPage == CHART_PAGE_TREND || chartPage == CHART_PAGE_STATS) {
    const int count = 2;
    const int x0 = 182;
    const int totalW = 50;
    const int gap = 4;
    const int segW = (totalW - gap) / count;
    const int y = 214;

    tft.fillRect(x0, y, totalW, 2, p.footer);

    for (int i = 0; i < count; i++) {
      int x = x0 + i * (segW + gap);
      int w = (i == count - 1) ? (totalW - (x - x0)) : segW;
      uint16_t c = ((i == 0 && !showIndoor) || (i == 1 && showIndoor))
                     ? p.accent : p.border;
      tft.fillRect(x, y, w, 2, c);
    }
  }
}


void refreshChartsRxOnly() {
  if (currentSkin != SKIN_CHARTS || menuOpen || diagOpen || calibrationOpen)
    return;

  SkinPalette p = paletteFor(currentSkin);

  // Co sekunde zmienia sie tylko RX/age.
  // Nie dotykamy PAGE/RANGE indicatorow, ZEW/WEW, licznika historii
  // ani paska postepu - dzieki temu nic nie pulsuje.
  // RC15.184: nie kasujemy x=0..1, gdzie biegnie lewy bok ramki.
  tft.fillRect(2, 202, 106, 12, p.bg);

  uint16_t statusColor = p.muted;
  uint32_t age = 0;

  if (activeSourceAvailable()) {
    age = activeSourceAgeSec();

    if (age <= 15)
      statusColor = toneColor(TONE_OK);
    else if (age <= 60)
      statusColor = toneColor(TONE_WARNING);
    else
      statusColor = toneColor(TONE_DANGER);
  }

  String rxInfo = "RX:";
  rxInfo += String(activeSourcePacketCount());

  if (activeSourceAvailable()) {
    rxInfo += " ";
    rxInfo += String(age);
    rxInfo += "s";
  }

  tft.setTextFont(1);
  tft.setTextColor(statusColor, p.bg);
  tft.setTextDatum(ML_DATUM);

  if (tft.textWidth(rxInfo) > 104 && activeSourceAvailable())
    rxInfo = "RX " + String(age) + "s";

  tft.drawString(rxInfo, 4, 208);
  tft.setTextDatum(TL_DATUM);
}

void refreshChartsFooterDynamic() {
  if (currentSkin != SKIN_CHARTS || menuOpen || diagOpen || calibrationOpen)
    return;

  SkinPalette p = paletteFor(currentSkin);

  // Pełny info-row zajmuje y=201..213. Nawigacja PAGE/RANGE jest
  // całkowicie osobno na y=214..215, a przyciski od y=216.
  // Dzięki temu ZEW/WEW (y=201..211) jest zawsze odtwarzane w całości.
  // RC15.184: dynamiczny info-row nie dotyka bokow ramki x=0..1 / 318..319.
  tft.fillRect(2, 201, SCREEN_W - 4, 13, p.bg);

  uint16_t statusColor = p.muted;
  uint32_t age = 0;

  if (activeSourceAvailable()) {
    age = activeSourceAgeSec();
    if (age <= 15)
      statusColor = toneColor(TONE_OK);
    else if (age <= 60)
      statusColor = toneColor(TONE_WARNING);
    else
      statusColor = toneColor(TONE_DANGER);
  }

  String rxInfo = "RX:";
  rxInfo += String(activeSourcePacketCount());
  if (activeSourceAvailable()) {
    rxInfo += " ";
    rxInfo += String(age);
    rxInfo += "s";
  }

  tft.setTextFont(1);
  tft.setTextColor(statusColor, p.bg);
  tft.setTextDatum(ML_DATUM);

  if (tft.textWidth(rxInfo) > 104 && activeSourceAvailable())
    rxInfo = "RX " + String(age) + "s";

  tft.drawString(rxInfo, 4, 208);

  // Prawa część zależy od strony.
  if (chartPage == CHART_PAGE_INOUT) {
    WeatherPacket d = snapshotData();
    float tOut = d.temperatura;
    float tIn = d.temp_wewnetrzna;
    float hOut = d.wilgotnosc;
    float hIn = d.wilg_wewnetrzna;

    if (!activeSourceAvailable() && chartCount > 0) {
      int lastIdx = chartIndexOldest(chartCount - 1);
      const ChartSample &last = chartHistory[lastIdx];
      tOut = last.temp;
      tIn = last.tempIn;
      hOut = last.hum;
      hIn = last.humIn;
    }

    float dTemp = (isfinite(tIn) && isfinite(tOut)) ? (tIn - tOut) : NAN;
    float dHum  = (isfinite(hIn) && isfinite(hOut)) ? (hIn - hOut) : NAN;

    uint16_t wewWanted = chartRangeSamples();
    uint16_t wewActual = 0;

    if (chartRange == CHART_RANGE_7D) {
      wewActual = chart7dReliableHourCount(true);
    } else {
      uint32_t indoorRecords = (uint32_t)(indoorHistoryFileBytes / sizeof(IndoorDiskRecord));
      if (indoorRecords > chartCount)
        indoorRecords = chartCount;
      wewActual = (uint16_t)indoorRecords;
    }

    if (wewActual > wewWanted)
      wewActual = wewWanted;

    String left = "H:" + String(wewActual) + "/" + String(wewWanted);
    String cmp = "DT ";
    cmp += fmtSigned(dTemp, 1);
    cmp += "C  DH ";
    cmp += fmtSigned(dHum, 0);
    cmp += "%";

    tft.setTextColor(p.muted, p.bg);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(left, 116, 208);

    // DT/DH ma prawa strefe 160..316. Przy skrajnych roznicach
    // skracamy separator, zamiast wchodzic na H:x/y.
    if (tft.textWidth(cmp, 1) > 154) {
      cmp = "DT";
      cmp += fmtSigned(dTemp, 1);
      cmp += " DH";
      cmp += fmtSigned(dHum, 0);
    }

    tft.setTextDatum(MR_DATUM);
    tft.drawString(cmp, 316, 208);

  } else if (chartPage == CHART_PAGE_COMFORT) {
    WeatherPacket d = snapshotData();
    float tOut = d.temperatura;
    float hOut = d.wilgotnosc;
    float tIn = d.temp_wewnetrzna;
    float hIn = d.wilg_wewnetrzna;

    if (!activeSourceAvailable() && chartCount > 0) {
      int lastIdx = chartIndexOldest(chartCount - 1);
      const ChartSample &last = chartHistory[lastIdx];
      tOut = last.temp;
      hOut = last.hum;
      tIn = last.tempIn;
      hIn = last.humIn;
    }

    float ahOut = absoluteHumidityGM3(tOut, hOut);
    float ahIn = absoluteHumidityGM3(tIn, hIn);
    float dAh = (isfinite(ahOut) && isfinite(ahIn)) ? (ahIn - ahOut) : NAN;

    String info = ventilationAdvice(ahOut, ahIn);
    info += " dAH ";
    info += fmtSigned(dAh, 1);
    info += "g";

    if (tft.textWidth(info, 1) > 200) {
      info = "dAH ";
      info += fmtSigned(dAh, 1);
      info += "g";
    }

    tft.setTextColor(ventilationAdviceColor(ahOut, ahIn), p.bg);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(info, 316, 208);

  } else if (chartPage == CHART_PAGE_TREND && !showIndoor) {
    float p3h = NAN;
    uint16_t p3hN = 0;
    WeatherPacket d = snapshotData();

    if (pressureTendency3H(p3h, p3hN)) {
      String left = pressureTendency3HLabel(p3h);
      String right = weatherChangeSignal(d, p3h);

      // Lewa czesc: tendencja barometryczna.
      tft.setTextColor(pressureTendency3HColor(p3h, p), p.bg);
      tft.setTextDatum(ML_DATUM);

      int leftW = (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ)
                    ? safeI18nTextWidth(left, 1) : tft.textWidth(left, 1);
      if (leftW > 132) {
        left = "3H ";
        left += fmtSigned(p3h, 1);
        left += "hPa";
      }
      if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ)
        drawSafeI18nString(left, 116, 208, 1, pressureTendency3HColor(p3h, p), p.bg, ML_DATUM);
      else
        tft.drawString(left, 116, 208);

      // Prawa czesc: ostrozny sygnal zmiany warunkow.
      tft.setTextColor(weatherChangeSignalColor(d, p3h, p), p.bg);
      tft.setTextDatum(MR_DATUM);

      int rightW = (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ)
                     ? safeI18nTextWidth(right, 1) : tft.textWidth(right, 1);
      if (rightW > 160) {
        if (d.opady_godzina >= 0.2f)
          right = "OPAD";
        else if (p3h <= -1.0f && d.wilgotnosc >= 75.0f)
          right = currentLanguage == LANG_PL ? "POGORSZENIE?" : "WORSE?";
        else if (p3h >= 1.0f && d.wilgotnosc <= 85.0f)
          right = currentLanguage == LANG_PL ? "POPRAWA?" : "BETTER?";
        else
          right = currentLanguage == LANG_PL ? "STABILNIE" : "STABLE";
      }

      if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ)
        drawSafeI18nString(right, 316, 208, 1,
                           weatherChangeSignalColor(d, p3h, p), p.bg, MR_DATUM);
      else
        tft.drawString(right, 316, 208);
    } else {
      tft.setTextColor(p.muted, p.bg);
      tft.setTextDatum(MR_DATUM);
      tft.drawString("3H --", 316, 208);
    }

  } else if (chartPage != CHART_PAGE_ALERTS) {
    uint16_t actualPoints;

    // STAT WEW korzysta z osobnej historii wewnętrznej także dla 7D.
    if (chartRange == CHART_RANGE_7D) {
      bool indoorCoverage =
          (chartPage == CHART_PAGE_STATS || chartPage == CHART_PAGE_TREND) &&
          showIndoor;
      actualPoints = chart7dReliableHourCount(indoorCoverage);
    } else {
      actualPoints = chartCount;
    }
    uint16_t wantedPoints = chartRangeSamples();

    if (actualPoints > wantedPoints)
      actualPoints = wantedPoints;

    String prog = String(actualPoints) + "/" + String(wantedPoints);
    tft.setTextColor(p.muted, p.bg);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(prog, 316, 206);

    const int pbX = 246;
    const int pbY = 211;
    const int pbW = 70;
    const int pbH = 3;
    uint8_t fillPct = chartRangeFillPercentCurrentView(showIndoor);

    tft.drawRect(pbX, pbY, pbW, pbH, p.border);
    int innerW = pbW - 2;
    int fillW = ((int)fillPct * innerW) / 100;
    if (fillW > 0)
      tft.fillRect(pbX + 1, pbY + 1, fillW, 1, p.accent);


  }

  tft.setTextDatum(TL_DATUM);

  // Wskazniki PAGE/RANGE sa statyczne i sa rysowane tylko przy
  // pelnym odswiezeniu footera / zmianie strony lub zakresu.
}


void refreshDataFreshnessFrame() {
  // RC15.196: uzytkownik moze wylaczyc tylko wizualna obwodke.
  // Stan LIVE/STALE/OFFLINE i RGB pozostaja niezalezne.
  if (!freshnessFrameEnabled)
    return;

  // RC15.182C: podczas RETURN FREEZE nic nie moze pojawic sie pod palcem
  // pomiedzy overlayem a odtworzeniem docelowego skina.
  if (menuOpen || diagOpen || calibrationOpen || waitTouchRelease)
    return;

  const uint8_t state = (uint8_t)activeSourceUiState();

  // LIVE/WAIT nie maja ramki.
  if (state != SOURCE_UI_STALE && state != SOURCE_UI_OFFLINE)
    return;

  // RC15.182: geometria bez kolizji:
  // FOCUS progress = y 29..31
  // ramka startuje dopiero od y=32
  // stopka/przyciski zaczynaja sie nizej, wiec dol konczy sie na y=209
  const int x0 = 1;
  const int y0 = 32;
  const int x1 = 318;
  const int y1 = 239;

  if (state == SOURCE_UI_STALE) {
    const uint16_t c = toneColor(TONE_WARNING);

    // Przerywana ramka.
    for (int x = x0; x <= x1; x += 12) {
      int len = 6;
      if (x + len - 1 > x1)
        len = x1 - x + 1;
      if (len > 0) {
        tft.drawFastHLine(x, y0, len, c);
        tft.drawFastHLine(x, y1, len, c);
      }
    }

    for (int y = y0; y <= y1; y += 12) {
      int len = 6;
      if (y + len - 1 > y1)
        len = y1 - y + 1;
      if (len > 0) {
        tft.drawFastVLine(x0, y, len, c);
        tft.drawFastVLine(x1, y, len, c);
      }
    }
  } else {
    const uint16_t c = toneColor(TONE_DANGER);

    // OFFLINE = pelna cienka ramka.
    tft.drawFastHLine(0, y0, 320, c);
    tft.drawFastHLine(0, y1, 320, c);
    tft.drawFastVLine(x0, y0, y1 - y0 + 1, c);
    tft.drawFastVLine(x1, y0, y1 - y0 + 1, c);
  }
}

void refreshHeaderStatusOnly() {
  if (menuOpen || diagOpen || calibrationOpen)
    return;

  SkinPalette p = paletteFor(currentSkin);

  static String lastRight = "";
  static uint16_t lastRightColor = 0xFFFF;
  static String lastSourceMark = "";
  static uint8_t lastSkin = 255;

  // RC15.177: jedna logika statusu dla naglowka i stopki.
  // Status opisuje faktycznie uzywane dane, a L* oznacza fallback LOCAL
  // przy wymuszonym trybie INTERNET.
  String right = activeSourceUiText();
  uint16_t rightColor = activeSourceUiColor(p);
  String sourceMark = activeSourceUiMark();

  bool skinChanged = lastSkin != currentSkin;
  bool fullRefresh = forceScreenClear;

  // Po MENU -> POWROT ekran zostaje wyczyszczony, więc prawa część
  // nagłówka musi być narysowana ponownie niezależnie od cache.
  if (fullRefresh || skinChanged || right != lastRight || rightColor != lastRightColor) {
    // Czyścimy tylko pole statusu, nie tytuł nagłówka.
    tft.fillRect(260, 0, 60, 27, p.panel2);
    tft.setTextColor(rightColor, p.panel2);
    tft.drawRightString(right, 312, 6, 2);

    lastRight = right;
    lastRightColor = rightColor;
  }

  if (fullRefresh || skinChanged || sourceMark != lastSourceMark) {
    tft.fillRect(232, 0, 26, 27, p.panel2);
    tft.setTextColor(sourceUsingFallbackLocal() ? toneColor(TONE_WARNING) : p.muted,
                     p.panel2);
    tft.drawCentreString(sourceMark, 245, 7, 1);
    lastSourceMark = sourceMark;
  }

  lastSkin = currentSkin;
}


void refreshChartsControlsOnly() {
  if (currentSkin != SKIN_CHARTS)
    return;

  SkinPalette p = paletteFor(currentSkin);

  // Wiersz dynamiczny byl czyszczony przez drawCharts() (do y=215),
  // wiec odtwarzamy go osobno.
  refreshChartsFooterDynamic();

  // PAGE
  tft.fillRoundRect(2, 216, 80, 22, 5, p.panel2);
  tft.drawRoundRect(2, 216, 80, 22, 5, p.border);
  drawLangButtonCentered(chartPageName(), chartPageName(),
                         42, 227, p.accent, p.panel2, false);

  // RANGE
  tft.fillRoundRect(112, 216, 62, 22, 5, p.panel2);
  tft.drawRoundRect(112, 216, 62, 22, 5, p.border);
  drawLangButtonCentered(chartRangeName(), chartRangeName(),
                         143, 227, p.accent, p.panel2, false);

  // ZEW/WEW jest kontekstowy.
  bool showInOutButton = (chartPage == CHART_PAGE_TREND ||
                          chartPage == CHART_PAGE_STATS);

  if (showInOutButton) {
    tft.fillRoundRect(182, 216, 50, 22, 5, p.panel2);
    tft.drawRoundRect(182, 216, 50, 22, 5, p.border);
    drawLangButtonCentered(showIndoor ? "WEW" : "ZEW",
                           showIndoor ? "WEW" : "ZEW",
                           207, 227, p.accent, p.panel2, false);
  } else {
    // Usuwamy pozostawiony przycisk po wyjsciu z TREND/STAT.
    tft.fillRect(180, 216, 54, 24, p.footer);
  }

#if HAS_TOUCH
  // MENU nie zmienia tresci, ale po przejsciu strony moze wymagac
  // odtworzenia tylko swojej ramki - bez czyszczenia calego footera.
  tft.fillRoundRect(258, 216, 60, 22, 5, p.panel2);
  tft.drawRoundRect(258, 216, 60, 22, 5, p.border);
  drawLangButtonCentered("MENU", "MENU",
                         288, 227, p.accent, p.panel2, false);
#endif

  drawChartsPageIndicator();
}

void refreshChartsInOutControlOnly() {
  if (currentSkin != SKIN_CHARTS ||
      (chartPage != CHART_PAGE_TREND && chartPage != CHART_PAGE_STATS))
    return;

  SkinPalette p = paletteFor(currentSkin);

  // Tylko przycisk ZEW/WEW + jego 2-segmentowy wskaznik.
  tft.fillRoundRect(182, 216, 50, 22, 5, p.panel2);
  tft.drawRoundRect(182, 216, 50, 22, 5, p.border);

  drawLangButtonCentered(showIndoor ? "WEW" : "ZEW",
                         showIndoor ? "WEW" : "ZEW",
                         207, 227, p.accent, p.panel2, false);

  const int x0 = 182;
  const int totalW = 50;
  const int gap = 4;
  const int segW = (totalW - gap) / 2;
  const int y = 214;

  tft.fillRect(x0, y, totalW, 2, p.footer);

  for (int i = 0; i < 2; i++) {
    int x = x0 + i * (segW + gap);
    int w = (i == 1) ? (totalW - (x - x0)) : segW;
    uint16_t c = ((i == 0 && !showIndoor) || (i == 1 && showIndoor))
                   ? p.accent : p.border;
    tft.fillRect(x, y, w, 2, c);
  }
}


extern bool focusConfigOpen;  // RC15.124F forward declaration
extern bool focusChartOpen;   // RC15.140

void drawFocusSelectFooterButtonVisible() {
#if HAS_TOUCH
  if (currentSkin != SKIN_FOCUS || focusConfigOpen || focusChartOpen)
    return;

  SkinPalette p = paletteFor(currentSkin);

  // RC15.140: trzy osobne przyciski, jeden styl i jeden font.
  // RX ma obszar 0..105.
  const int y = 216;
  const int h = 22;

  tft.fillRoundRect(112, y, 62, h, 5, p.panel2);
  tft.drawRoundRect(112, y, 62, h, 5, p.border);

  tft.fillRoundRect(178, y, 68, h, 5, p.panel2);
  tft.drawRoundRect(178, y, 68, h, 5, p.border);

  // RC15.254B: szerszy przycisk ustawień.
  tft.fillRoundRect(250, y, 68, h, 5, p.panel2);
  tft.drawRoundRect(250, y, 68, h, 5, p.border);

  // RC15.255: wszystkie 3 przyciski FOCUS są spójne z bieżącym językiem.
  tft.setTextColor(p.accent, p.panel2);

  String selTxt = focusSelectButtonLabel();
  String chartTxt = focusChartButtonLabel();
  String setTxt = focusSettingsButtonLabel();

  if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ) {
    drawSafeI18nString(selTxt,   143, 219, 2, p.accent, p.panel2, MC_DATUM);
    drawSafeI18nString(chartTxt, 212, 219, 2, p.accent, p.panel2, MC_DATUM);
    drawSafeI18nString(setTxt,   284, 219, 2, p.accent, p.panel2, MC_DATUM);
  } else {
    tft.setTextDatum(MC_DATUM);
    tft.drawString(selTxt,   143, 227, 2);
    tft.drawString(chartTxt, 212, 227, 2);
    tft.drawString(setTxt,   284, 227, 2);
  }
  tft.setTextDatum(TL_DATUM);
#endif
}

void drawFooter() {
  SkinPalette p = paletteFor(currentSkin);

  // Ekran WYKRESY ma osobny, wyzszy footer:
  // info/status y=202..213, przyciski y=214..239.
  if (currentSkin == SKIN_CHARTS) {
    uint16_t statusColor = p.muted;

    uint32_t age = 0;
    if (activeSourceAvailable()) {
      age = activeSourceAgeSec();
      if (age <= 15)
        statusColor = toneColor(TONE_OK);
      else if (age <= 60)
        statusColor = toneColor(TONE_WARNING);
      else
        statusColor = toneColor(TONE_DANGER);
    }

    // Lewa czesc info-row: pelny status RX + wiek.
    tft.fillRect(0, 202, 112, 12, p.bg);
    String rxInfo = "RX:";
    rxInfo += String(activeSourcePacketCount());
    if (haveData) {
      rxInfo += " ";
      rxInfo += String(age);
      rxInfo += "s";
    }

    tft.setTextFont(1);
    tft.setTextColor(statusColor, p.bg);
    tft.setTextDatum(ML_DATUM);

    // Jesli kiedys packetCount zrobi sie bardzo dlugi, skracamy tylko licznik,
    // ale wiek pakietu nadal pozostaje widoczny.
    if (tft.textWidth(rxInfo) > 108 && haveData)
      rxInfo = "RX " + String(age) + "s";

    tft.drawString(rxInfo, 4, 208);
    tft.setTextDatum(TL_DATUM);

    // Na METEO/EXTRA pokazujemy ile danych faktycznie jest w aktywnym zakresie.
    // Na WEW prawa część info-row jest zarezerwowana dla DT/DH.
    if (chartPage != CHART_PAGE_INOUT &&
        chartPage != CHART_PAGE_COMFORT &&
        chartPage != CHART_PAGE_ALERTS) {
      uint16_t actualPoints;
      if (chartRange == CHART_RANGE_7D) {
        bool indoorCoverage =
            (chartPage == CHART_PAGE_STATS || chartPage == CHART_PAGE_TREND) &&
            showIndoor;
        actualPoints = chart7dReliableHourCount(indoorCoverage);
      } else {
        actualPoints = chartCount;
      }

      uint16_t wantedPoints = chartRangeSamples();

      if (actualPoints > wantedPoints)
        actualPoints = wantedPoints;

      String prog = String(actualPoints) + "/" + String(wantedPoints);
      tft.setTextColor(p.muted, p.bg);
      tft.setTextDatum(MR_DATUM);
      tft.setTextFont(1);
      tft.drawString(prog, 316, 206);

      // Cienki pasek pokazuje wizualnie zapelnienie wybranego zakresu.
      // Np. 11/12 = prawie pelna godzina historii.
      const int pbX = 246;
      const int pbY = 211;
      const int pbW = 70;
      const int pbH = 3;
      uint8_t fillPct = chartRangeFillPercentCurrentView(showIndoor);

      tft.drawRect(pbX, pbY, pbW, pbH, p.border);
      int innerW = pbW - 2;
      int fillW = ((int)fillPct * innerW) / 100;
      if (fillW > 0)
        tft.fillRect(pbX + 1, pbY + 1, fillW, 1, p.accent);

      tft.setTextDatum(TL_DATUM);
    }

    // Sam footer przyciskow ma 26 px wysokosci.
    tft.fillRect(0, 214, SCREEN_W, 26, p.footer);

    // Przyciski maja duzo wiecej wysokosci niz font 2.
    // PAGE - przesuniety w lewo zgodnie z RC15.9
    tft.fillRoundRect(2, 216, 80, 22, 5, p.panel2);
    tft.drawRoundRect(2, 216, 80, 22, 5, p.border);

    // RANGE
    tft.fillRoundRect(112, 216, 62, 22, 5, p.panel2);
    tft.drawRoundRect(112, 216, 62, 22, 5, p.border);

    bool showInOutButton = (chartPage == CHART_PAGE_TREND ||
                            chartPage == CHART_PAGE_STATS);

#if HAS_TOUCH
    if (showInOutButton) {
      tft.fillRoundRect(182, 216, 50, 22, 5, p.panel2);
      tft.drawRoundRect(182, 216, 50, 22, 5, p.border);
    }

    // MENU
    tft.fillRoundRect(258, 216, 60, 22, 5, p.panel2);
    tft.drawRoundRect(258, 216, 60, 22, 5, p.border);
#endif

    // Przyciski footera: nowy maly font regular.
    drawLangButtonCentered(chartPageName(), chartPageName(),
                           42, 227, p.accent, p.panel2, false);
    drawLangButtonCentered(chartRangeName(), chartRangeName(),
                           143, 227, p.accent, p.panel2, false);
#if HAS_TOUCH
    if (showInOutButton)
      drawLangButtonCentered(showIndoor ? "WEW" : "ZEW",
                             showIndoor ? "WEW" : "ZEW",
                             207, 227, p.accent, p.panel2, false);

    drawLangButtonCentered("MENU", "MENU",
                           288, 227, p.accent, p.panel2, false);
#endif
    drawChartsPageIndicator();

    // Po każdym pełnym rysowaniu footera odtwarzamy cały dynamiczny info-row.
    // To gwarantuje, że STAT ZEW/WEW jest widoczne od razu po wejściu na stronę,
    // a nie dopiero po pierwszym dotknięciu.
    refreshChartsFooterDynamic();
    return;
  }

  // Pozostale skiny: footer jest statyczny.
  // Po pierwszym/full redraw nie rysujemy ponownie przycisku MENU przy
  // każdym pakiecie danych. Aktualizowany jest tylko RX po lewej.
  static uint8_t lastStaticFooterSkin = 255;
  static uint8_t lastStaticFooterLanguage = 255;

  bool needFullFooter =
      forceScreenClear ||
      lastStaticFooterSkin != currentSkin ||
      lastStaticFooterLanguage != currentLanguage;

  if (!needFullFooter) {
    refreshSimpleFooterRxOnly();
    return;
  }

  tft.fillRect(0, 216, SCREEN_W, 24, p.footer);

#if HAS_TOUCH
  if (currentSkin != SKIN_FOCUS) {
    tft.fillRoundRect(258, 216, 60, 22, 5, p.panel2);
    tft.drawRoundRect(258, 216, 60, 22, 5, p.border);
  }
#endif

  uint16_t statusColor = p.muted;

  String s = "RX:";
  s += String(activeSourcePacketCount());

  if (activeSourceAvailable()) {
    uint32_t age = activeSourceAgeSec();

    s += " ";
    s += String(age);
    s += "s";

    if (age <= 15)
      statusColor = toneColor(TONE_OK);
    else if (age <= 60)
      statusColor = toneColor(TONE_WARNING);
    else
      statusColor = toneColor(TONE_DANGER);
  }

  tft.setTextColor(statusColor, p.footer);
  tft.drawString(s, 6, 222, 2);

#if HAS_TOUCH
  if (currentSkin != SKIN_FOCUS)
    drawLangButtonCentered("MENU", "MENU",
                           288, 227, p.accent, p.panel2, false);
#else
  tft.setTextColor(p.accent, p.footer);
  tft.drawRightString("NO TOUCH", 314, 222, 1);
#endif

  lastStaticFooterSkin = currentSkin;
  lastStaticFooterLanguage = currentLanguage;
  // RC15.124E: WYBOR jako ostatnia warstwa footera.
  drawFocusSelectFooterButtonVisible();

}

void refreshSimpleFooterRxOnly() {
  if (currentSkin == SKIN_CHARTS ||
      menuOpen || diagOpen || calibrationOpen || focusChartOpen)
    return;

  SkinPalette p = paletteFor(currentSkin);

  static String lastText = "";
  static uint16_t lastColor = 0xFFFF;
  static uint8_t lastSkin = 255;

  uint16_t statusColor = activeSourceUiColor(p);

  String s = "RX:";
  s += String(activeSourcePacketCount());

  if (activeSourceAvailable()) {
    uint32_t age = activeSourceAgeSec();
    s += " ";
    s += String(age);
    s += "s";
  }

  if (!forceScreenClear &&
      lastSkin == currentSkin &&
      s == lastText &&
      statusColor == lastColor)
    return;

  // RC15.140: na FOCUS RX ma osobny obszar 0..105.
  // Nie wolno mu naruszyc WYBOR/WYKRES/MENU.
  const int rxClearW = (currentSkin == SKIN_FOCUS) ? 106 : 250;
  // RC15.184: zostawiamy x=0..1 dla lewej krawedzi ramki
  // oraz y=239 dla dolnej krawedzi. Tekst RX zaczyna sie dopiero od x=6.
  tft.fillRect(2, 216, rxClearW - 2, 23, p.footer);
  tft.setTextColor(statusColor, p.footer);
  tft.drawString(s, 6, 222, 2);

  lastText = s;
  lastColor = statusColor;
  lastSkin = currentSkin;
}


uint8_t bestFont(const String &txt, int width, uint8_t preferred = 4) {
  const uint8_t fonts[] = {4, 2, 1};

  for (uint8_t i = 0; i < 3; i++) {
    uint8_t f = fonts[i];

    if (f > preferred)
      continue;

    if (tft.textWidth(txt, f) <= width)
      return f;
  }

  return 1;
}

void drawStateDot(int x, int y, VisualState st) {
  uint16_t c = toneColor(st.tone);
  tft.fillCircle(x, y, 3, c);
}

void drawTile(
  int x, int y, int w, int h,
  const String &label,
  const String &value,
  const String &unit,
  VisualState st,
  bool large = false
) {
  SkinPalette p = paletteFor(currentSkin);

  uint16_t stateC = toneColor(st.tone);
  uint16_t bg = p.panel;

  if (alertStyle == ALERT_PANEL && st.severity >= 1)
    bg = softFillForTone(st.tone, p.panel);

  tft.fillRoundRect(x, y, w, h, 6, bg);

  uint16_t borderC = p.border;
  if (alertStyle >= ALERT_BORDER && st.severity >= 1)
    borderC = stateC;

  tft.drawRoundRect(x, y, w, h, 6, borderC);

  if (!deferTileLabels) {
    tft.setTextColor(p.muted, bg);
    if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ)
      drawSafeI18nString(label, x + 7, y + 5, 1, p.muted, bg, TL_DATUM);
    else
      drawLocalizedLabelAuto(label, x + 7, y + 5, 1);
  }

  drawStateDot(x + w - 9, y + 9, st);

  String full = value;

  if (unit.length() && !large) {
    full += " ";
    full += unit;
  }

  uint8_t font;

  if (large) {
    font = bestFont(value, w - 12, 4);
  } else {
    // Niskie kafelki (np. TEMP/UV w KOMPASIE, h=45) nie mogą używać
    // fontu 4, bo jego dolna część nachodzi na dolną ramkę.
    uint8_t preferred = (h < 50) ? 2 : 4;
    font = bestFont(full, w - 12, preferred);
  }

  uint16_t valueC = (st.tone == TONE_NEUTRAL) ? p.text : stateC;
  tft.setTextColor(valueC, bg);

  if (large) {
    int yy = (font == 4) ? y + 19 : y + 24;
    tft.drawCentreString(value, x + w / 2, yy, font);

    if (unit.length()) {
      tft.setTextColor(p.muted, bg);
      tft.drawCentreString(unit, x + w / 2, y + h - 11, 1);
    }
  }
  else {
    int yy = (font == 4) ? y + 20 : y + 26;
    tft.drawCentreString(full, x + w / 2, yy, font);
  }

  // Critical state gets a small "!" marker, not blinking.
  if (st.severity >= 3) {
    tft.setTextColor(stateC, bg);
    tft.drawString("!", x + 4, y + h - 11, 1);
  }
}

void drawSmallInfo(
  int x, int y, int w, int h,
  const String &label,
  const String &value,
  VisualState st
) {
  SkinPalette p = paletteFor(currentSkin);
  uint16_t c = toneColor(st.tone);

  tft.fillRoundRect(x, y, w, h, 5, p.panel);
  tft.drawRoundRect(x, y, w, h, 5,
                    (alertStyle >= ALERT_BORDER && st.severity >= 1) ? c : p.border);

  tft.setTextColor(p.muted, p.panel);
  tft.setTextDatum(TL_DATUM);
  if (useLanguageSmallFont()) {
    tft.drawString(label, x + 6, y + 8);
    unloadSmoothFontSafe();
  } else if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ) {
    drawSafeI18nString(label, x + 6, y + 4, 1, p.muted, p.panel, TL_DATUM);
  } else {
    tft.drawString(label, x + 6, y + 4, 1);
  }

  // Miejsce na wartość liczymy od końca etykiety do prawej krawędzi.
  int labelW = (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ)
             ? safeI18nTextWidth(label, 1)
             : tft.textWidth(label, 1);
  int valueLeft = x + 6 + labelW + 5;
  int valueRight = x + w - 6;
  int availableW = valueRight - valueLeft;

  if (availableW < 12)
    availableW = 12;

  uint8_t valueFont = bestFont(value, availableW, 2);

  tft.setTextColor(c, p.panel);
  tft.drawRightString(value, valueRight, y + 4, valueFont);
}



// ------------------------------------------------------------
// KIERUNEK WIATRU - staly, czytelny rozmiar
// Skrot kierunku jest zawsze duzy; stopnie sa osobno mala czcionka.
// ------------------------------------------------------------

void drawDirectionTile(
  int x, int y, int w, int h,
  float deg,
  const String &label
) {
  SkinPalette p = paletteFor(currentSkin);
  uint16_t c = toneColor(TONE_INFO);

  tft.fillRoundRect(x, y, w, h, 6, p.panel);
  tft.drawRoundRect(x, y, w, h, 6, p.border);

  if (!deferTileLabels) {
    tft.setTextColor(p.muted, p.panel);
    tft.drawString(label, x + 7, y + 5, 1);
  }

  drawStateDot(x + w - 9, y + 9, {TONE_INFO, 0, "DIR"});

  String dir = windDir(deg);

  tft.setTextColor(c, p.panel);
  // RC15.253A: polskie 2/3-literowe skroty nie moga byc przycinane.
  uint8_t dirFont = bestFont(dir, w - 18, 4);
  int dirY = (dirFont == 4) ? (y + 20) : (y + 24);
  tft.drawCentreString(dir, x + w / 2, dirY, dirFont);

  String degrees = "--";
  if (isfinite(deg)) {
    degrees = String(deg, 0);
    degrees += " deg";
  }

  tft.setTextColor(p.muted, p.panel);
  tft.drawCentreString(degrees, x + w / 2, y + h - 10, 1);
}

// ============================================================
// GRAFIKA - ROZA WIATROW / WSKAZNIKI
// ============================================================

void drawCompassRose(int cx, int cy, int r, float deg, uint16_t accent) {
  SkinPalette p = paletteFor(currentSkin);

  tft.drawCircle(cx, cy, r, p.border);
  tft.drawCircle(cx, cy, r - 1, p.border);

  // osie glowne
  tft.drawLine(cx, cy - r, cx, cy + r, p.border);
  tft.drawLine(cx - r, cy, cx + r, cy, p.border);

  // skosy
  int d = (int)(r * 0.70f);
  tft.drawLine(cx - d, cy - d, cx + d, cy + d, p.border);
  tft.drawLine(cx + d, cy - d, cx - d, cy + d, p.border);

  tft.setTextColor(p.muted, p.bg);
  String n = "N", s = "S", w = "W", e = "E";

  // RC15.256:
  // GLOBAL = międzynarodowy standard N/E/S/W także na osi kompasu.
  // LOKALNY = oznaczenia zależne od języka interfejsu.
  if (windDirNotationMode == WIND_DIR_LOCAL) {
    if (currentLanguage == LANG_PL) {
      // PN=północ, PD=południe, Z=zachód, W=wschód.
      n = "PN"; s = "PD"; w = "Z"; e = "W";
    }
    else if (currentLanguage == LANG_DE) {
      n = "N"; s = "S"; w = "W"; e = "O";
    }
    else if (currentLanguage == LANG_CZ) {
      n = "S"; s = "J"; w = "Z"; e = "V";
    }
  }

  // Wszystkie cztery osie tym samym fontem -> spójny wygląd.
  // RC15.254D: symetryczny odstęp górnej i dolnej etykiety od okręgu.
  // Dolna etykieta zaczyna się 2 px pod kołem; górna kończy się 2 px nad kołem.
  const int compassCardinalFont = 2;
  const int compassCardinalH = tft.fontHeight(compassCardinalFont);
  const int compassCardinalGap = 2;

  tft.drawCentreString(n, cx,
                       cy - r - compassCardinalGap - compassCardinalH,
                       compassCardinalFont);
  tft.drawCentreString(s, cx,
                       cy + r + compassCardinalGap,
                       compassCardinalFont);
  tft.drawCentreString(w, cx - r - 12, cy - 8, compassCardinalFont);
  tft.drawCentreString(e, cx + r + 12, cy - 8, compassCardinalFont);

  if (!isfinite(deg))
    return;

  float a = (deg - 90.0f) * DEG_TO_RAD;
  int x2 = cx + (int)(cosf(a) * (r - 8));
  int y2 = cy + (int)(sinf(a) * (r - 8));

  // wskazowka
  tft.drawLine(cx, cy, x2, y2, accent);
  tft.drawLine(cx + 1, cy, x2 + 1, y2, accent);
  tft.fillCircle(cx, cy, 4, accent);
  tft.fillCircle(x2, y2, 3, accent);
}

void drawGaugeBar(
  int x, int y, int w, int h,
  float value, float minV, float maxV,
  VisualState st,
  const String &label,
  const String &valueText
) {
  SkinPalette p = paletteFor(currentSkin);
  uint16_t c = toneColor(st.tone);

  tft.setTextColor(p.muted, p.bg);
  tft.drawString(label, x, y, 1);

  int labelW = tft.textWidth(label, 1);
  int availableW = w - labelW - 10;
  if (availableW < 20)
    availableW = 20;

  uint8_t valueFont = bestFont(valueText, availableW, 2);

  tft.setTextColor(c, p.bg);
  int valueY = (valueFont == 1) ? (y + 2) : y;
  tft.drawRightString(valueText, x + w, valueY, valueFont);

  int by = y + 18;

  // Usuń stare wypełnienie lokalnie zamiast czyścić cały ekran.
  if (w > 4 && h > 4)
    tft.fillRect(x + 2, by + 2, w - 4, h - 4, p.bg);

  tft.drawRoundRect(x, by, w, h, 4, p.border);

  float f = 0.0f;
  if (isfinite(value) && maxV > minV) {
    f = (value - minV) / (maxV - minV);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
  }

  int fillW = (int)((w - 4) * f);
  if (fillW > 0)
    tft.fillRoundRect(x + 2, by + 2, fillW, h - 4, 3, c);
}

// ============================================================
// SKIN 1 - GRID
// ============================================================


void drawGridLabelsBatch(bool indoor) {
  SkinPalette p = paletteFor(currentSkin);

  bool smooth = useLanguageSmallFont();

  struct GridLabel {
    int x;
    int y;
    int maxW;
    String text;
    String fallback;
  };

  // 74 px zostawia bezpieczny margines przed kropką stanu
  // w prawym górnym rogu kafelka 98 px.
  GridLabel labels[] = {
    {15,  40, 74, indoor ? String(tr(TXT_TEMP_IN)) : String(tr(TXT_TEMP_OUT)),
                  indoor ? String("TEMP WEW") : String("TEMP ZEW")},
    {118, 40, 74, indoor ? String(tr(TXT_HUM_IN)) : String(tr(TXT_HUM_OUT)),
                  indoor ? String("WILG WEW") : String("WILG ZEW")},
    {221, 40, 74, String(tr(TXT_PRESSURE)), String("PRESSURE")},
    {15, 101, 74, String(tr(TXT_WIND)), String("WIATR")},
    {118,101, 74, String(tr(TXT_GUST)), String("PORYW")},
    {221,101, 74, String(tr(TXT_DIRECTION)), String("KIERUNEK")},
    {15, 162, 74, String("UV"), String("UV")},
    {118,162, 74, String("LUX"), String("LUX")},
    {221,162, 74, String(tr(TXT_RAIN)), String("OPAD")}
  };

  tft.setTextDatum(TL_DATUM);

  for (auto &item : labels) {
    tft.setTextColor(p.accent, p.panel);

    if (smooth) {
      String s = item.text;

      while (s.length() > 1 && tft.textWidth(s) > item.maxW) {
        s.remove(s.length() - 1);
      }

      if (s != item.text && s.length() > 1) {
        while (s.length() > 1 && tft.textWidth(s + ".") > item.maxW) {
          s.remove(s.length() - 1);
        }
        s += ".";
      }

      tft.drawString(s, item.x, item.y);
    } else {
      if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ)
        drawSafeI18nString(item.text, item.x, item.y, 1, p.muted, p.panel, TL_DATUM);
      else
        tft.drawString(item.fallback, item.x, item.y, 1);
    }
  }

  if (smooth)
    unloadSmoothFontSafe();
}




// ============================================================
// RC15.117 - TRUE PARTIAL REFRESH
// Tylko wartosci/stan; etykiety i ramki statyczne nie sa malowane ponownie.
// ============================================================
void refreshTileValueOnly(
  int x, int y, int w, int h,
  const String &value, const String &unit,
  VisualState st, bool large = false
) {
  SkinPalette p = paletteFor(currentSkin);
  uint16_t stateC = toneColor(st.tone);
  uint16_t bg = p.panel;

  if (alertStyle == ALERT_PANEL && st.severity >= 1)
    bg = softFillForTone(st.tone, p.panel);

  // Zostawiamy pas etykiety y..y+16 nietkniety.
  tft.fillRect(x + 2, y + 17, w - 4, h - 19, bg);

  uint16_t borderC = p.border;
  if (alertStyle >= ALERT_BORDER && st.severity >= 1)
    borderC = stateC;
  tft.drawRoundRect(x, y, w, h, 6, borderC);
  drawStateDot(x + w - 9, y + 9, st);

  String full = value;
  if (unit.length() && !large) {
    full += " ";
    full += unit;
  }

  uint8_t font;
  if (large)
    font = bestFont(value, w - 12, 4);
  else {
    uint8_t preferred = (h < 50) ? 2 : 4;
    font = bestFont(full, w - 12, preferred);
  }

  uint16_t valueC = (st.tone == TONE_NEUTRAL) ? p.text : stateC;
  tft.setTextColor(valueC, bg);

  if (large) {
    int yy = (font == 4) ? y + 19 : y + 24;
    tft.drawCentreString(value, x + w / 2, yy, font);
    if (unit.length()) {
      tft.setTextColor(p.muted, bg);
      tft.drawCentreString(unit, x + w / 2, y + h - 11, 1);
    }
  } else {
    int yy = (font == 4) ? y + 20 : y + 26;
    tft.drawCentreString(full, x + w / 2, yy, font);
  }

  if (st.severity >= 3) {
    tft.setTextColor(stateC, bg);
    tft.drawString("!", x + 4, y + h - 11, 1);
  }
}


void drawRainTileUnified(int x, int y, int w, int h,
                         const String &label,
                         float rainValue,
                         bool drawLabel) {
  SkinPalette p = paletteFor(currentSkin);
  VisualState st = stateRain(rainValue);
  uint16_t c = toneColor(st.tone);
  uint16_t bg = p.panel;

  if (alertStyle == ALERT_PANEL && st.severity >= 1)
    bg = softFillForTone(st.tone, p.panel);

  tft.fillRoundRect(x, y, w, h, 6, bg);

  uint16_t borderC = p.border;
  if (alertStyle >= ALERT_BORDER && st.severity >= 1)
    borderC = c;
  tft.drawRoundRect(x, y, w, h, 6, borderC);
  drawStateDot(x + w - 9, y + 9, st);

  if (drawLabel) {
    tft.setTextColor(p.muted, bg);
    tft.drawString(label, x + 7, y + 5, 1);
  }

  String num = fmt(rainValue, 1);
  String unit = "mm/h";

  // Duża liczba + mniejsza jednostka w jednej linii.
  uint8_t numFont = bestFont(num, 47, 4);
  uint8_t unitFont = 1;
  int gap = 4;

  int numW = tft.textWidth(num, numFont);
  int unitW = tft.textWidth(unit, unitFont);
  int totalW = numW + gap + unitW;
  int startX = x + (w - totalW) / 2;

  tft.setTextColor(c, bg);
  int valueY = (numFont == 4) ? y + 20 : y + 25;
  tft.drawString(num, startX, valueY, numFont);

  tft.setTextColor(p.muted, bg);
  int unitY = (numFont == 4) ? y + 29 : y + 30;
  tft.drawString(unit, startX + numW + gap, unitY, unitFont);
}

void refreshRainTileUnified(int x, int y, int w, int h, float rainValue) {
  SkinPalette p = paletteFor(currentSkin);
  VisualState st = stateRain(rainValue);
  uint16_t c = toneColor(st.tone);
  uint16_t bg = p.panel;

  if (alertStyle == ALERT_PANEL && st.severity >= 1)
    bg = softFillForTone(st.tone, p.panel);

  // Nie ruszamy etykiety u góry.
  tft.fillRect(x + 2, y + 17, w - 4, h - 19, bg);

  uint16_t borderC = p.border;
  if (alertStyle >= ALERT_BORDER && st.severity >= 1)
    borderC = c;
  tft.drawRoundRect(x, y, w, h, 6, borderC);
  drawStateDot(x + w - 9, y + 9, st);

  String num = fmt(rainValue, 1);
  String unit = "mm/h";
  uint8_t numFont = bestFont(num, 47, 4);
  uint8_t unitFont = 1;
  int gap = 4;

  int numW = tft.textWidth(num, numFont);
  int unitW = tft.textWidth(unit, unitFont);
  int totalW = numW + gap + unitW;
  int startX = x + (w - totalW) / 2;

  tft.setTextColor(c, bg);
  int valueY = (numFont == 4) ? y + 20 : y + 25;
  tft.drawString(num, startX, valueY, numFont);

  tft.setTextColor(p.muted, bg);
  int unitY = (numFont == 4) ? y + 29 : y + 30;
  tft.drawString(unit, startX + numW + gap, unitY, unitFont);
}

void refreshDirectionValueOnly(int x, int y, int w, int h, float deg) {
  SkinPalette p = paletteFor(currentSkin);
  uint16_t c = toneColor(TONE_INFO);

  tft.fillRect(x + 2, y + 17, w - 4, h - 19, p.panel);
  tft.drawRoundRect(x, y, w, h, 6, p.border);

  String dir = windDir(deg);
  tft.setTextColor(c, p.panel);
  uint8_t dirFont = bestFont(dir, w - 18, 4);
  int dirY = (dirFont == 4) ? (y + 20) : (y + 24);
  tft.drawCentreString(dir, x + w / 2, dirY, dirFont);

  String degrees = "--";
  if (isfinite(deg))
    degrees = String(deg, 0) + " st";

  tft.setTextColor(p.muted, p.panel);
  tft.drawCentreString(degrees, x + w / 2, y + h - 10, 1);
}

void refreshGaugeValueOnly(
  int x, int y, int w, int h,
  float value, float minV, float maxV,
  VisualState st, const String &valueText
) {
  SkinPalette p = paletteFor(currentSkin);
  uint16_t c = toneColor(st.tone);

  // Tylko prawa czesc wiersza z wartoscia; etykieta po lewej zostaje.
  int clearX = x + w / 2;
  tft.fillRect(clearX, y, x + w - clearX + 1, 17, p.bg);

  uint8_t valueFont = bestFont(valueText, w / 2 - 4, 2);
  tft.setTextColor(c, p.bg);
  int valueY = (valueFont == 1) ? (y + 2) : y;
  tft.drawRightString(valueText, x + w, valueY, valueFont);

  int by = y + 18;
  if (w > 4 && h > 4)
    tft.fillRect(x + 2, by + 2, w - 4, h - 4, p.bg);
  tft.drawRoundRect(x, by, w, h, 4, p.border);

  float f = 0.0f;
  if (isfinite(value) && maxV > minV) {
    f = (value - minV) / (maxV - minV);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
  }
  int fillW = (int)((w - 4) * f);
  if (fillW > 0)
    tft.fillRoundRect(x + 2, by + 2, fillW, h - 4, 3, c);
}

void refreshGridLiveValues(const WeatherPacket &d) {
  SkinPalette p = paletteFor(currentSkin);
  float tempNow = showIndoor ? d.temp_wewnetrzna : d.temperatura;
  float humNow  = showIndoor ? d.wilg_wewnetrzna : d.wilgotnosc;

  refreshTileValueOnly(8, 34, 98, 54,
      fmt(tempNow, 1), "C", stateTemperature(tempNow), false);

  refreshTileValueOnly(111, 34, 98, 54,
      fmt(humNow, 0), "%", stateHumidity(humNow), false);

  refreshTileValueOnly(214, 34, 98, 54,
      fmtPressure(d.cisnienie), "hPa", statePressure(d.cisnienie), true);

  refreshTileValueOnly(8, 95, 98, 54,
      fmt(d.predkosc_wiatru, 1), "m/s",
      stateWind(d.predkosc_wiatru, false), true);

  refreshTileValueOnly(111, 95, 98, 54,
      fmt(d.poryw_wiatru, 1), "m/s",
      stateWind(d.poryw_wiatru, true), false);

  refreshDirectionValueOnly(214, 95, 98, 54, d.kierunek_wiatru);

  refreshTileValueOnly(8, 156, 98, 54,
      fmt(d.uv_index, 1), "", stateUV(d.uv_index), false);

  String luxUnit;
  String luxValue = fmtLux(d.swiatlo_lux, luxUnit);
  refreshTileValueOnly(111, 156, 98, 54,
      luxValue, luxUnit, stateLux(d.swiatlo_lux), false);

  refreshRainTileUnified(214, 156, 98, 54, d.opady_godzina);

  refreshHeaderStatusOnly();
  refreshSimpleFooterRxOnly();
}

void refreshGridInOutTiles(const WeatherPacket &d) {
  if (currentSkin != SKIN_GRID ||
      menuOpen || diagOpen || calibrationOpen)
    return;

  SkinPalette p = paletteFor(currentSkin);

  float tempNow = showIndoor ? d.temp_wewnetrzna : d.temperatura;
  float humNow  = showIndoor ? d.wilg_wewnetrzna : d.wilgotnosc;

  // Rysujemy tylko dwa kafelki zależne od WEW/ZEW.
  deferTileLabels = true;

  drawTile(
    8, 34, 98, 54,
    showIndoor ? tr(TXT_TEMP_IN) : tr(TXT_TEMP_OUT),
    fmt(tempNow, 1), "C",
    stateTemperature(tempNow)
  );

  drawTile(
    111, 34, 98, 54,
    showIndoor ? tr(TXT_HUM_IN) : tr(TXT_HUM_OUT),
    fmt(humNow, 0), "%",
    stateHumidity(humNow)
  );

  deferTileLabels = false;

  // Etykiety tylko dwóch kafelków, jednym załadowaniem fontu.
  bool smooth = useLanguageSmallFont();

  String tempLabel = showIndoor ? String(tr(TXT_TEMP_IN)) : String(tr(TXT_TEMP_OUT));
  String humLabel  = showIndoor ? String(tr(TXT_HUM_IN))  : String(tr(TXT_HUM_OUT));

  auto drawSafe = [&](String s, const String &fallback, int x) {
    const int maxW = 74;

    tft.setTextColor(p.accent, p.panel);
    tft.setTextDatum(TL_DATUM);

    if (smooth) {
      while (s.length() > 1 && tft.textWidth(s) > maxW)
        s.remove(s.length() - 1);

      if (s != (x == 15 ? tempLabel : humLabel) && s.length() > 1) {
        while (s.length() > 1 && tft.textWidth(s + ".") > maxW)
          s.remove(s.length() - 1);
        s += ".";
      }

      tft.drawString(s, x, 40);
    } else {
      tft.drawString(fallback, x, 39, 1);
    }
  };

  drawSafe(tempLabel, showIndoor ? "TEMP WEW" : "TEMP ZEW", 15);
  drawSafe(humLabel,  showIndoor ? "WILG WEW" : "WILG ZEW", 118);

  if (smooth)
    unloadSmoothFontSafe();
}

void drawGrid(const WeatherPacket &d) {
  SkinPalette p = paletteFor(currentSkin);
  if (forceScreenClear)
    tft.fillScreen(p.bg);

  drawHeader(tr(TXT_WEATHER_GRID));

  // Kafelki i wartosci najpierw bez etykiet. Wszystkie etykiety
  // nakladamy potem jednym ladowaniem fontu z SD.
  deferTileLabels = true;

  float tempNow = showIndoor ? d.temp_wewnetrzna : d.temperatura;
  float humNow  = showIndoor ? d.wilg_wewnetrzna : d.wilgotnosc;

  drawTile(
    8, 34, 98, 54,
    showIndoor ? tr(TXT_TEMP_IN) : tr(TXT_TEMP_OUT),
    fmt(tempNow, 1), "C",
    stateTemperature(tempNow)
  );

  drawTile(
    111, 34, 98, 54,
    showIndoor ? tr(TXT_HUM_IN) : tr(TXT_HUM_OUT),
    fmt(humNow, 0), "%",
    stateHumidity(humNow)
  );

  drawTile(
    214, 34, 98, 54,
    tr(TXT_PRESSURE),
    fmtPressure(d.cisnienie), "hPa",
    statePressure(d.cisnienie),
    true
  );

  drawTile(
    8, 95, 98, 54,
    tr(TXT_WIND),
    fmt(d.predkosc_wiatru, 1), "m/s",
    stateWind(d.predkosc_wiatru, false),
    true
  );

  drawTile(
    111, 95, 98, 54,
    tr(TXT_GUST),
    fmt(d.poryw_wiatru, 1), "m/s",
    stateWind(d.poryw_wiatru, true)
  );

  drawDirectionTile(214, 95, 98, 54, d.kierunek_wiatru, tr(TXT_DIRECTION));

  drawTile(
    8, 156, 98, 54,
    "UV",
    fmt(d.uv_index, 1), "",
    stateUV(d.uv_index)
  );

  String luxUnit;
  String luxValue = fmtLux(d.swiatlo_lux, luxUnit);

  drawTile(
    111, 156, 98, 54,
    "LUX",
    luxValue, luxUnit,
    stateLux(d.swiatlo_lux)
  );

  drawRainTileUnified(214, 156, 98, 54,
                      tr(TXT_RAIN),
                      d.opady_godzina,
                      false);

  deferTileLabels = false;
  drawGridLabelsBatch(showIndoor);

  drawFooter();
}

// ============================================================
// SKIN 2 - DASHBOARD
// ============================================================

void drawDashboard(const WeatherPacket &d) {
  deferTileLabels = false;
  SkinPalette p = paletteFor(currentSkin);
  if (forceScreenClear)
    tft.fillScreen(p.bg);

  drawHeader(tr(TXT_WEATHER_DASH));

  // Large external temperature
  drawTile(
    8, 34, 146, 78,
    "TEMP ZEW",
    fmt(d.temperatura, 1), "C",
    stateTemperature(d.temperatura),
    true
  );

  // Large wind
  drawTile(
    162, 34, 150, 78,
    tr(TXT_WIND),
    fmt(d.predkosc_wiatru, 1), "m/s",
    stateWind(d.predkosc_wiatru, false),
    true
  );

  drawSmallInfo(
    8, 120, 98, 34,
    "WILG",
    fmt(d.wilgotnosc, 0) + "%",
    stateHumidity(d.wilgotnosc)
  );

  drawSmallInfo(
    111, 120, 98, 34,
    tr(TXT_PRESSURE),
    fmtPressure(d.cisnienie),
    statePressure(d.cisnienie)
  );

  drawSmallInfo(
    214, 120, 98, 34,
    tr(TXT_GUST),
    fmt(d.poryw_wiatru, 1),
    stateWind(d.poryw_wiatru, true)
  );

  drawSmallInfo(
    8, 160, 98, 34,
    "UV",
    fmt(d.uv_index, 1),
    stateUV(d.uv_index)
  );

  String luxUnit;
  String lv = fmtLux(d.swiatlo_lux, luxUnit);

  drawSmallInfo(
    111, 160, 98, 34,
    "LUX",
    lv + " " + luxUnit,
    stateLux(d.swiatlo_lux)
  );

  drawSmallInfo(
    214, 160, 98, 34,
    tr(TXT_RAIN),
    fmt(d.opady_godzina, 1) + "mm/h",
    stateRain(d.opady_godzina)
  );

  // Dolny pasek PANEL: TEMP WEW + KIERUNEK.
  // RC15.113A PANEL: spokojne etykiety, wyróżnione wartości.
  tft.fillRoundRect(8, 198, 304, 17, 4, p.panel2);
  tft.drawFastVLine(160, 200, 13, p.border);

  String panelTempValue = fmt(d.temp_wewnetrzna, 1) + " C";
  String panelDirValue = windDir(d.kierunek_wiatru);

  uint16_t panelTempColor =
      toneColor(stateTemperature(d.temp_wewnetrzna).tone);
  uint16_t panelDirColor = p.accent;

  drawMenuLocalizedSmallCentered(tr(TXT_TEMP_IN), "TEMP WEW",
                                 48, 206, p.muted, p.panel2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(panelTempColor, p.panel2);
  tft.drawString(panelTempValue, 118, 206, 2);

  drawMenuLocalizedSmallCentered(tr(TXT_DIRECTION), "KIERUNEK",
                                 204, 206, p.muted, p.panel2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(panelDirColor, p.panel2);
  tft.drawString(panelDirValue, 278, 206, 2);

  tft.setTextDatum(TL_DATUM);

drawFooter();
}

// ============================================================
// SKIN 3 - MINIMAL
// ============================================================

void drawMinimal(const WeatherPacket &d) {
  deferTileLabels = false;
  SkinPalette p = paletteFor(currentSkin);
  if (forceScreenClear)
    tft.fillScreen(p.bg);

  drawHeader(tr(TXT_WEATHER_MINIMAL));

  float tempNow = showIndoor ? d.temp_wewnetrzna : d.temperatura;
  float humNow  = showIndoor ? d.wilg_wewnetrzna : d.wilgotnosc;

  drawTile(
    8, 36, 146, 86,
    showIndoor ? tr(TXT_TEMP_IN) : tr(TXT_TEMP_OUT),
    fmt(tempNow, 1), "C",
    stateTemperature(tempNow),
    true
  );

  drawTile(
    162, 36, 150, 86,
    tr(TXT_WIND),
    fmt(d.predkosc_wiatru, 1), "m/s",
    stateWind(d.predkosc_wiatru, false),
    true
  );

  drawTile(
    8, 130, 98, 78,
    showIndoor ? tr(TXT_HUM_IN) : tr(TXT_HUM_OUT),
    fmt(humNow, 0), "%",
    stateHumidity(humNow),
    true
  );

  drawTile(
    111, 130, 98, 78,
    tr(TXT_PRESSURE),
    fmtPressure(d.cisnienie), "hPa",
    statePressure(d.cisnienie),
    true
  );

  drawDirectionTile(
    214, 130, 98, 78,
    d.kierunek_wiatru,
    tr(TXT_DIRECTION)
  );

  // UV jako mala, kolorowa informacja w dolnej czesci kafelka.
  VisualState uvst = stateUV(d.uv_index);
  uint16_t uvc = toneColor(uvst.tone);
  tft.setTextColor(uvc, paletteFor(currentSkin).panel);
  tft.drawRightString("UV " + fmt(d.uv_index, 1), 305, 190, 1);

  drawFooter();
}


// ============================================================
// SKIN 4 - COMPASS / ROZA WIATROW
// ============================================================

void drawCompassSkin(const WeatherPacket &d) {
  SkinPalette p = paletteFor(currentSkin);
  if (forceScreenClear)
    tft.fillScreen(p.bg);

  drawHeader(tr(TXT_WIND_COMPASS));

  // Czyścimy tylko pole róży, aby stara wskazówka nie zostawała.
  tft.fillRect(0, 34, 146, 180, p.bg);

  // duza roza wiatrow po lewej
  VisualState wst = stateWind(d.predkosc_wiatru, false);
  uint16_t wc = toneColor(wst.tone);
  drawCompassRose(76, 118, 58, d.kierunek_wiatru, wc);

  tft.setTextColor(wc, p.bg);
  String dtext = windDir(d.kierunek_wiatru);
  uint8_t compassDirFont;
  if (currentLanguage == LANG_PL)
    compassDirFont = 2;  // PD/PN/PNW nie dominuja nad tarcza
  else
    compassDirFont = bestFont(dtext, 92, 4);
  int compassDirY = (compassDirFont == 4) ? 105 : 109;
  tft.drawCentreString(dtext, 76, compassDirY, compassDirFont);

  tft.setTextColor(p.muted, p.bg);
  String degText = fmt(d.kierunek_wiatru, 0) + " st";
  uint8_t degFont = bestFont(degText, 118, 2);
  tft.drawCentreString(degText, 76, 139, degFont);

  // prawa kolumna
  drawTile(
    154, 36, 158, 58,
    tr(TXT_WIND),
    fmt(d.predkosc_wiatru, 1), "m/s",
    stateWind(d.predkosc_wiatru, false),
    true
  );

  drawTile(
    154, 101, 158, 58,
    tr(TXT_GUST),
    fmt(d.poryw_wiatru, 1), "m/s",
    stateWind(d.poryw_wiatru, true),
    true
  );

  drawTile(
    154, 166, 76, 45,
    "TEMP",
    fmt(d.temperatura, 1), "C",
    stateTemperature(d.temperatura)
  );

  drawTile(
    236, 166, 76, 45,
    "UV",
    fmt(d.uv_index, 1), "",
    stateUV(d.uv_index)
  );

  drawFooter();
}

// ============================================================
// SKIN 5 - INSTRUMENT / PASKI
// ============================================================

void drawInstrumentSkin(const WeatherPacket &d) {
  SkinPalette p = paletteFor(currentSkin);
  if (forceScreenClear)
    tft.fillScreen(p.bg);

  drawHeader(tr(TXT_WEATHER_INSTRUMENT));

  // duze cyfry na gorze
  drawTile(
    8, 35, 148, 72,
    tr(TXT_TEMP_OUT),
    fmt(d.temperatura, 1), "C",
    stateTemperature(d.temperatura),
    true
  );

  drawTile(
    164, 35, 148, 72,
    tr(TXT_PRESSURE),
    fmtPressure(d.cisnienie), "hPa",
    statePressure(d.cisnienie),
    true
  );

  drawGaugeBar(
    12, 118, 296, 12,
    d.wilgotnosc, 0, 100,
    stateHumidity(d.wilgotnosc),
    "WILGOTNOŚĆ",
    fmt(d.wilgotnosc, 0) + "%"
  );

  drawGaugeBar(
    12, 151, 296, 12,
    d.predkosc_wiatru, 0, 20,
    stateWind(d.predkosc_wiatru, false),
    tr(TXT_WIND),
    fmt(d.predkosc_wiatru, 1) + " m/s"
  );

  drawGaugeBar(
    12, 184, 296, 12,
    d.uv_index, 0, 11,
    stateUV(d.uv_index),
    "UV",
    fmt(d.uv_index, 1)
  );

  drawFooter();
}


// ============================================================
// SKIN 6 - FOCUS / PELNY EKRAN
// Co kilka sekund pokazuje jeden parametr bardzo duza czcionka.
// ============================================================

enum FocusMetric : uint8_t {
  FOCUS_TEMP = 0,
  FOCUS_HUM,
  FOCUS_TEMP_IN,
  FOCUS_HUM_IN,
  FOCUS_PRESS,
  FOCUS_WIND,
  FOCUS_GUST,
  FOCUS_DIR,
  FOCUS_UV,
  FOCUS_LUX,
  FOCUS_RAIN,
  FOCUS_COUNT
};

bool focusMetricEnabled(uint8_t metric) {
  if (metric >= FOCUS_COUNT)
    return false;
  return (focusEnabledMask & (1U << metric)) != 0;
}

uint8_t focusActiveCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < FOCUS_COUNT; ++i)
    if (focusMetricEnabled(i))
      ++n;
  return n;
}

uint8_t currentFocusMetric() {
  uint8_t count = focusActiveCount();
  if (count == 0)
    return FOCUS_TEMP;

  uint8_t slot = (uint8_t)((millis() / 4500UL) % count);

  for (uint8_t i = 0; i < FOCUS_COUNT; ++i) {
    if (!focusMetricEnabled(i))
      continue;
    if (slot == 0)
      return i;
    --slot;
  }

  return FOCUS_TEMP;
}

uint8_t lastDrawnFocusMetric = 255;
bool focusMetricInternalRedraw = false;
uint16_t focusEnabledMask = 0x07FF;  // RC15.124: 11 aktywnych metryk
bool focusConfigOpen = false;  // RC15.123A: bez redraw header/footer

// RC15.140 - duzy wykres aktualnej planszy FOCUS.
bool focusChartOpen = false;
uint8_t focusChartMetric = FOCUS_TEMP;

// RC15.145: niezalezny zakres duzego wykresu FOCUS.
// 0=1H, 1=6H, 2=24H, 3=7D
uint8_t focusChartRange = 1;


void drawFocusCompassLetter(char ch, int x, int y, int w, int h, uint16_t c) {
  int t = max(4, w / 8);  // grubosc proporcjonalna do szerokosci litery

  if (ch == 'N') {
    tft.fillRect(x, y, t, h, c);
    tft.fillRect(x + w - t, y, t, h, c);
    for (int i = 0; i < h; i += 3) {
      int xx = x + (i * (w - t)) / h;
      tft.fillRect(xx, y + i, t, 4, c);
    }
  }
  else if (ch == 'E') {
    tft.fillRect(x, y, t, h, c);
    tft.fillRect(x, y, w, t, c);
    tft.fillRect(x, y + h/2 - t/2, w - 4, t, c);
    tft.fillRect(x, y + h - t, w, t, c);
  }
  else if (ch == 'S') {
    tft.fillRect(x + t, y, w - t, t, c);
    tft.fillRect(x, y + t, t, h/2 - t, c);
    tft.fillRect(x + t, y + h/2 - t/2, w - 2*t, t, c);
    tft.fillRect(x + w - t, y + h/2, t, h/2 - t, c);
    tft.fillRect(x, y + h - t, w - t, t, c);
  }
  else if (ch == 'W') {
    tft.fillRect(x, y, t, h - 8, c);
    tft.fillRect(x + w - t, y, t, h - 8, c);
    for (int i = 0; i < 12; i += 2) {
      int yy = y + h - 12 + i;
      int xl = x + (i * (w/2 - t)) / 12;
      int xr = x + w - t - (i * (w/2 - t)) / 12;
      tft.fillRect(xl, yy, t, 4, c);
      tft.fillRect(xr, yy, t, 4, c);
    }
  }
  else if (ch == 'P') {
    tft.fillRect(x, y, t, h, c);
    tft.fillRect(x, y, w - t, t, c);
    tft.fillRect(x + w - t, y + t, t, h/2 - 2*t, c);
    tft.fillRect(x, y + h/2 - t, w - t, t, c);
  }
  else if (ch == 'D') {
    tft.fillRect(x, y, t, h, c);
    tft.fillRect(x, y, w - t, t, c);
    tft.fillRect(x, y + h - t, w - t, t, c);
    tft.fillRect(x + w - t, y + t, t, h - 2*t, c);
  }
  else if (ch == 'Z') {
    tft.fillRect(x, y, w, t, c);
    tft.fillRect(x, y + h - t, w, t, c);
    for (int i = 0; i < h; i += 3) {
      int xx = x + w - t - (i * (w - t)) / h;
      tft.fillRect(xx, y + i, t, 4, c);
    }
  }
  else if (ch == 'O') {
    tft.fillRect(x + t, y, w - 2*t, t, c);
    tft.fillRect(x + t, y + h - t, w - 2*t, t, c);
    tft.fillRect(x, y + t, t, h - 2*t, c);
    tft.fillRect(x + w - t, y + t, t, h - 2*t, c);
  }
  else if (ch == 'V') {
    tft.fillRect(x, y, t, h - 12, c);
    tft.fillRect(x + w - t, y, t, h - 12, c);
    for (int i = 0; i < 12; i += 2) {
      int yy = y + h - 12 + i;
      int xl = x + (i * (w/2 - t)) / 12;
      int xr = x + w - t - (i * (w/2 - t)) / 12;
      tft.fillRect(xl, yy, t, 4, c);
      tft.fillRect(xr, yy, t, 4, c);
    }
  }
  else if (ch == 'J') {
    tft.fillRect(x + w - t, y, t, h - t, c);
    tft.fillRect(x + t, y + h - t, w - 2*t, t, c);
    tft.fillRect(x, y + h/2, t, h/2 - t, c);
  }
}

void drawFocusCompassXL(const String &dir, uint16_t c) {
  int len = dir.length();
  if (len <= 0) return;
  if (len > 3) len = 3;

  // Zachowujemy stary charakter FOCUS, ale dopasowujemy 1/2/3 litery.
  int h = 60;
  int w;
  int gap;

  if (len == 1) {
    w = 48;
    gap = 0;
  } else if (len == 2) {
    w = 44;
    gap = 12;
  } else {
    w = 34;
    gap = 8;
  }

  int totalW = len * w + (len - 1) * gap;
  int startX = 160 - totalW / 2;
  int y = 88;

  for (int i = 0; i < len; ++i) {
    drawFocusCompassLetter(dir[i], startX + i * (w + gap), y, w, h, c);
  }
}

String focusMetricLabel(uint8_t metric) {
  switch (metric) {
    case FOCUS_TEMP:    return tr(TXT_TEMP_OUT);
    case FOCUS_HUM:     return tr(TXT_HUM_OUT);
    case FOCUS_TEMP_IN: return tr(TXT_TEMP_IN);
    case FOCUS_HUM_IN:  return tr(TXT_HUM_IN);
    case FOCUS_PRESS:   return tr(TXT_PRESSURE);
    case FOCUS_WIND:    return tr(TXT_WIND);
    case FOCUS_GUST:    return tr(TXT_GUST);
    case FOCUS_DIR:     return tr(TXT_DIRECTION);
    case FOCUS_UV:      return "UV";
    case FOCUS_LUX:     return "LUX";
    case FOCUS_RAIN:    return tr(TXT_RAIN);
    default:            return "?";
  }
}

String focusSelectTitle() {
  if (currentLanguage == LANG_PL) return "FOCUS - WYBÓR";
  if (currentLanguage == LANG_DE) return "FOCUS - AUSWAHL";
  if (currentLanguage == LANG_CZ) return "FOCUS - VÝBĚR";
  return "FOCUS - SELECT";
}

String focusSelectButtonText() {
  if (currentLanguage == LANG_PL) return "WYBÓR";
  if (currentLanguage == LANG_DE) return "WAHL";
  if (currentLanguage == LANG_CZ) return "VÝBĚR";
  return "SELECT";
}


void drawFocusConfigItem(uint8_t i, bool smoothLoaded) {
  if (i >= FOCUS_COUNT) return;

  // RC15.132: interaktywny kafelek nie uzywa smooth fontu z SD.
  // Backtrace RC15.131A: drawFocusConfigItem -> drawGlyph -> File::read -> FatFS.
  (void)smoothLoaded;

  SkinPalette p = paletteFor(currentSkin);
  const int x0 = 5;
  const int x1 = 162;
  const int y0 = 34;
  const int w = 153;
  const int h = 25;
  const int dy = 27;

  int col = i & 1;
  int row = i >> 1;
  int x = col ? x1 : x0;
  int y = y0 + row * dy;

  bool on = focusMetricEnabled(i);
  uint16_t bg = on ? p.panel2 : p.panel;
  uint16_t border = on ? p.accent : p.border;
  uint16_t fg = on ? p.text : p.muted;

  tft.fillRoundRect(x, y, w, h, 5, bg);
  tft.drawRoundRect(x, y, w, h, 5, border);

  String label = focusMetricLabel(i);
  String value = on ? "ON" : "OFF";

  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(fg, bg);
  if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ)
    drawSafeI18nString(label, x + 7, y + 8, 1, fg, bg, TL_DATUM);
  else
    tft.drawString(label, x + 7, y + 8, 1);

  tft.setTextDatum(TR_DATUM);
  tft.drawString(value, x + w - 7, y + 8, 1);
  tft.setTextDatum(TL_DATUM);
}

void drawFocusConfig() {
  markResetDiag(220);
  focusConfigOpen = true;
  menuOpen = true;

  SkinPalette p = paletteFor(currentSkin);
  tft.fillScreen(p.bg);
  tft.fillRect(0, 0, 320, 30, p.panel2);
  drawLangHeader(focusSelectTitle(), "FOCUS - SELECT",
                 8, 6, p.accent, p.panel2);

  // RC15.132: kafelki SELECT bez pliku .vlw na SD.
  unloadSmoothFontSafe();
  tft.setTextFont(1);

  for (uint8_t i = 0; i < FOCUS_COUNT; ++i)
    drawFocusConfigItem(i, false);

  tft.fillRoundRect(8, 202, 304, 34, 7, p.accent);
  drawLangButtonCentered(tr(TXT_BACK), "BACK",
                         160, 219, TFT_BLACK, p.accent, false);

  unloadSmoothFontSafe();
  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
}

void handleFocusConfigTouch(int x, int y) {
  const int x0 = 5;
  const int x1 = 162;
  const int y0 = 34;
  const int w = 153;
  const int h = 25;
  const int dy = 27;

  for (uint8_t i = 0; i < FOCUS_COUNT; ++i) {
    int col = i & 1;
    int row = i >> 1;
    int bx = col ? x1 : x0;
    int by = y0 + row * dy;

    if (inside(x, y, bx, by, w, h)) {
      uint16_t bit = (uint16_t)(1U << i);
      uint16_t next = focusEnabledMask ^ bit;

      if ((next & 0x07FF) != 0) {
        focusEnabledMask = next & 0x07FF;
        settingsDirty = true;
        lastDrawnFocusMetric = 255;
      }

      unloadSmoothFontSafe();
      tft.setTextFont(1);
      drawFocusConfigItem(i, false);
      unloadSmoothFontSafe();
      tft.setTextFont(1);
      tft.setTextDatum(TL_DATUM);

      waitTouchRelease = true;
      touchActionBlockUntilMs = millis() + 80UL;
      return;
    }
  }

  if (inside(x, y, 0, 198, 320, 42)) {
    if (settingsDirty) {
      saveSettings();
      settingsDirty = false;
    }

    focusConfigOpen = false;
    menuOpen = false;
    waitTouchRelease = true;
    redrawAfterTouchRelease = true;
    forceScreenClear = true;
    return;
  }
}

void drawFocusValue(const String &label,
                    const String &value,
                    const String &unit,
                    VisualState st,
                    const String &sub = "") {
  SkinPalette p = paletteFor(currentSkin);
  uint16_t c = toneColor(st.tone);

  // Header/footer pozostaja nieruchome. Czyścimy tylko środek.
  // RC15.182A: przy wymuszonym pelnym redraw FOCUS trzeba wyczyscic tez
  // martwa szczeline y=27..33. Wlasnie tam lezy gorna krawedz ramki.
  if (!focusMetricInternalRedraw && forceScreenClear)
    tft.fillRect(0, 27, 320, 7, p.bg);

  tft.fillRect(0, 34, 320, 182, p.bg);  // RC15.124E: do y=215

  // RC15.123A: przy automatycznej zmianie metryki nie dotykamy belki.
  if (!focusMetricInternalRedraw && forceScreenClear)
    drawHeader("FOCUS");

  // RC15.123A: bold UTF-8 - polskie znaki i rozmiar jak w innych belkach.
  unloadSmoothFontSafe();
  bool focusLabelSmooth = useLanguageUiFont(true);
  tft.setTextColor(p.text, p.bg);
  if (focusLabelSmooth) {
    tft.setTextDatum(MC_DATUM);
    tft.drawString(label, 160, 54);
    tft.setTextDatum(TL_DATUM);
    unloadSmoothFontSafe();
  } else {
    uint8_t labelFont = bestFont(label, 296, 4);
    int labelY = (labelFont == 4) ? 40 : 46;
    if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ)
      drawSafeI18nString(label, 160, labelY, labelFont, p.text, p.bg, MC_DATUM);
    else
      tft.drawCentreString(label, 160, labelY, labelFont);
  }

  bool isDirection = (sub.length() > 0 && unit.length() == 0);

  if (isDirection) {
    // RC15.253D: jeden duzy renderer wektorowy dla PL/EN/DE/CZ.
    drawFocusCompassXL(value, c);

    // Stopnie maja byc tylko dodatkiem.
    tft.setTextColor(p.accent, p.bg);
    tft.drawCentreString(sub, 160, 158, 2);
  } else {
    // Brak danych: czytelny stan, ale bez udawania wartosci liczbowej.
    if (value == "--" || value.length() == 0) {
      tft.setTextColor(c, p.bg);
      tft.drawCentreString("--", 160, 96, 7);
    } else {
      // RC15.121: WSZYSTKIE wyniki liczbowe maja ten sam font 7.
      // Zero nie moze byc mniejsze od 65, 1013 czy 2.0.
      const uint8_t valueFont = 7;
      const uint8_t unitFont = 4;
      const int gap = unit.length() ? 8 : 0;

      int valueW = tft.textWidth(value, valueFont);
      int unitW = unit.length() ? tft.textWidth(unit, unitFont) : 0;
      int totalW = valueW + gap + unitW;

      // Awaryjnie dla bardzo szerokiej kombinacji przechodzimy na font 4,
      // ale dotyczy to calej wartosci, nie konkretnej cyfry.
      if (totalW <= 306) {
        int startX = 160 - totalW / 2;

        tft.setTextColor(c, p.bg);
        tft.drawString(value, startX, 88, valueFont);

        if (unit.length()) {
          tft.setTextColor(p.text, p.bg);
          tft.drawString(unit, startX + valueW + gap, 108, unitFont);
        }
      } else {
        String full = value;
        if (unit.length()) {
          full += " ";
          full += unit;
        }
        tft.setTextColor(c, p.bg);
        tft.drawCentreString(full, 160, 100, 4);
      }
    }
  }

  // Status zawsze nad footerem.
  tft.setTextColor(c, p.bg);
  String statusTag = String(st.tag);
  if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ)
    drawSafeI18nString(statusTag, 160, 176, 2, c, p.bg, MC_DATUM);
  else
    tft.drawCentreString(statusTag, 160, 184, 2);
}

void drawFocusPressureContext(const WeatherPacket &d) {
  SkinPalette p = paletteFor(currentSkin);

  float p3h = NAN;
  uint16_t p3hN = 0;

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);

  if (!pressureTendency3H(p3h, p3hN)) {
    tft.setTextColor(p.muted, p.bg);
    tft.drawString("3H --", 160, 166);
    tft.setTextDatum(TL_DATUM);
    return;
  }

  String line = pressureTendency3HText(p3h);
  String signal = weatherChangeSignal(d, p3h);

  if (signal.length() && signal != "--") {
    line += "  ";
    line += signal;
  }

  // Jedna linia musi miescic sie miedzy duza wartoscia a status tag.
  if (tft.textWidth(line, 1) > 296) {
    line = pressureTendency3HText(p3h);
  }

  uint16_t c = weatherChangeSignalColor(d, p3h, p);
  if (signal == "WARUNKI STABILNE" ||
      signal == "CONDITIONS STABLE" ||
      signal == "LAGE STABIL" ||
      signal == "PODMINKY STABIL")
    c = pressureTendency3HColor(p3h, p);

  tft.setTextColor(c, p.bg);
  tft.drawString(line, 160, 166);
  tft.setTextDatum(TL_DATUM);
}


void drawFocusWindContext(const WeatherPacket &d, bool gustView) {
  SkinPalette p = paletteFor(currentSkin);

  float other = gustView ? d.predkosc_wiatru : d.poryw_wiatru;
  if (!isfinite(other))
    return;

  String line;

  if (currentLanguage == LANG_EN)
    line = gustView ? "WIND " : "GUST ";
  else if (currentLanguage == LANG_DE)
    line = gustView ? "WIND " : "BOE ";
  else if (currentLanguage == LANG_CZ)
    line = gustView ? "VITR " : "NARAZ ";
  else
    line = gustView ? "WIATR " : "PORYW ";

  line += fmt(other, 1);
  line += " m/s";

  // Dla PORYWU dodatkowo pokazujemy relacje do wiatru bazowego,
  // ale tylko gdy ma sens liczbowy.
  if (gustView &&
      isfinite(d.poryw_wiatru) &&
      isfinite(d.predkosc_wiatru) &&
      d.predkosc_wiatru > 0.2f) {
    float ratio = d.poryw_wiatru / d.predkosc_wiatru;
    String extra = "  x";
    extra += String(ratio, 1);

    if (tft.textWidth(line + extra, 1) <= 296)
      line += extra;
  }

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(p.muted, p.bg);
  tft.drawString(line, 160, 166);
  tft.setTextDatum(TL_DATUM);
}


void drawFocusHumidityContext(const WeatherPacket &d, bool indoorView) {
  SkinPalette p = paletteFor(currentSkin);

  float temp = indoorView ? d.temp_wewnetrzna : d.temperatura;
  float hum  = indoorView ? d.wilg_wewnetrzna : d.wilgotnosc;
  float dew  = dewPointC(temp, hum);

  if (!isfinite(dew))
    return;

  String line;

  if (currentLanguage == LANG_EN)
    line = "DEW ";
  else if (currentLanguage == LANG_DE)
    line = "TAU ";
  else if (currentLanguage == LANG_CZ)
    line = "ROSA ";
  else
    line = "ROSA ";

  line += fmt(dew, 1);
  line += " C";

  // Dodatkowo pokazujemy roznice temperatura - punkt rosy,
  // bo mala roznica oznacza powietrze bliskie nasyceniu.
  if (isfinite(temp)) {
    float spread = temp - dew;
    if (isfinite(spread)) {
      String extra = "  DT ";
      extra += fmt(spread, 1);
      extra += " C";

      if (tft.textWidth(line + extra, 1) <= 296)
        line += extra;
    }
  }

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);

  uint16_t c = p.muted;
  if ((temp - dew) <= 2.0f)
    c = toneColor(TONE_WARNING);
  else if ((temp - dew) <= 4.0f)
    c = toneColor(TONE_INFO);

  tft.setTextColor(c, p.bg);
  tft.drawString(line, 160, 166);
  tft.setTextDatum(TL_DATUM);
}


void drawFocusTemperatureContext(const WeatherPacket &d, bool indoorView) {
  SkinPalette p = paletteFor(currentSkin);

  float outT = d.temperatura;
  float inT  = d.temp_wewnetrzna;

  if (!isfinite(outT) || !isfinite(inT))
    return;

  float delta = inT - outT;

  String line;
  if (currentLanguage == LANG_EN)
    line = indoorView ? "OUT " : "IN ";
  else if (currentLanguage == LANG_DE)
    line = indoorView ? "AUS " : "IN ";
  else if (currentLanguage == LANG_CZ)
    line = indoorView ? "VEN " : "UVN ";
  else
    line = indoorView ? "ZEW " : "WEW ";

  line += fmt(indoorView ? outT : inT, 1);
  line += " C";

  String extra = "  DT ";
  extra += fmtSigned(delta, 1);
  extra += " C";

  if (tft.textWidth(line + extra, 1) <= 296)
    line += extra;

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);

  uint16_t c = p.muted;
  if (fabsf(delta) >= 10.0f)
    c = toneColor(TONE_INFO);

  tft.setTextColor(c, p.bg);
  tft.drawString(line, 160, 166);
  tft.setTextDatum(TL_DATUM);
}


void drawFocusRainContext(const WeatherPacket &d) {
  if (!isfinite(d.opady))
    return;

  SkinPalette p = paletteFor(currentSkin);

  String line;
  if (currentLanguage == LANG_EN)
    line = "TOTAL ";
  else if (currentLanguage == LANG_DE)
    line = "SUMME ";
  else if (currentLanguage == LANG_CZ)
    line = "CELKEM ";
  else
    line = "SUMA ";

  line += fmt(d.opady, 1);
  line += " mm";

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(p.muted, p.bg);
  tft.drawString(line, 160, 166);
  tft.setTextDatum(TL_DATUM);
}


void drawFocusSkin(const WeatherPacket &d) {
  markResetDiag(210);
  uint8_t metric = currentFocusMetric();
  lastDrawnFocusMetric = metric;

  switch (metric) {
    case FOCUS_TEMP:
      drawFocusValue(tr(TXT_TEMP_OUT), fmt(d.temperatura, 1), "C",
                     stateTemperature(d.temperatura));
      drawFocusTemperatureContext(d, false);
      break;

    case FOCUS_HUM:
      drawFocusValue(tr(TXT_HUM_OUT), fmt(d.wilgotnosc, 0), "%",
                     stateHumidity(d.wilgotnosc));
      drawFocusHumidityContext(d, false);
      break;

    case FOCUS_TEMP_IN:
      drawFocusValue(tr(TXT_TEMP_IN), fmt(d.temp_wewnetrzna, 1), "C",
                     stateTemperature(d.temp_wewnetrzna));
      drawFocusTemperatureContext(d, true);
      break;

    case FOCUS_HUM_IN:
      drawFocusValue(tr(TXT_HUM_IN), fmt(d.wilg_wewnetrzna, 0), "%",
                     stateHumidity(d.wilg_wewnetrzna));
      drawFocusHumidityContext(d, true);
      break;

    case FOCUS_PRESS:
      drawFocusValue(tr(TXT_PRESSURE), fmtPressure(d.cisnienie), "hPa",
                     statePressure(d.cisnienie));
      drawFocusPressureContext(d);
      break;

    case FOCUS_WIND:
      drawFocusValue(tr(TXT_WIND), fmt(d.predkosc_wiatru, 1), "m/s",
                     stateWind(d.predkosc_wiatru, false));
      drawFocusWindContext(d, false);
      break;

    case FOCUS_GUST:
      drawFocusValue(tr(TXT_GUST), fmt(d.poryw_wiatru, 1), "m/s",
                     stateWind(d.poryw_wiatru, true));
      drawFocusWindContext(d, true);
      break;

    case FOCUS_DIR: {
      String sub = isfinite(d.kierunek_wiatru) ?
                   String(d.kierunek_wiatru, 0) + " st" : "--";
      drawFocusValue(tr(TXT_DIRECTION), windDir(d.kierunek_wiatru), "",
                     {TONE_INFO, 0, "DIR"}, sub);
      break;
    }

    case FOCUS_UV:
      drawFocusValue("UV", fmt(d.uv_index, 1), "",
                     stateUV(d.uv_index));
      break;

    case FOCUS_LUX: {
      String unit;
      String val = fmtLux(d.swiatlo_lux, unit);
      drawFocusValue("LUX", val, unit,
                     stateLux(d.swiatlo_lux));
      break;
    }

    case FOCUS_RAIN:
    default:
      drawFocusValue(tr(TXT_RAIN), fmt(d.opady_godzina, 1), "mm/h",
                     stateRain(d.opady_godzina));
      drawFocusRainContext(d);
      break;
  }

  // Segmenty postepu sa rysowane w naglowku przez serviceFocusProgress().

  // drawFocusValue() robi fillScreen(), więc cache footera nie może
  // uznać, że MENU nadal istnieje. Wymuszamy pełne odtworzenie footera.
  // RC15.123A: przy wewnetrznej zmianie metryki footer jest nietykany.
  if (!focusMetricInternalRedraw)
    drawFooter();
}

void serviceFocusProgress() {
  if (currentSkin != SKIN_FOCUS || menuOpen || diagOpen || calibrationOpen)
    return;

  static uint32_t lastUpdate = 0;
  static uint8_t lastMetricSeen = 255;

  uint32_t now = millis();
  if (now - lastUpdate < 150UL)
    return;
  lastUpdate = now;

  uint8_t metric = currentFocusMetric();

  // RC15.130A: po powrocie z overlay ekran FOCUS jest juz narysowany.
  // Synchronizujemy cache i nie wywolujemy drawCurrentSkin drugi raz.
  if (focusProgressSyncPending) {
    lastMetricSeen = metric;
    focusProgressSyncPending = false;
    return;
  }

  // Pelny ekran tylko przy zmianie parametru.
  if (metric != lastMetricSeen || metric != lastDrawnFocusMetric) {
    lastMetricSeen = metric;
    WeatherPacket d = snapshotData();
    focusMetricInternalRedraw = true;
    drawFocusSkin(d);
    focusMetricInternalRedraw = false;
    return;
  }

  SkinPalette p = paletteFor(currentSkin);

  // Zamiast dolnego paska: 11 malych segmentow w dolnej czesci naglowka.
  // Niczego nie zakrywaja i od razu widac pozycje w cyklu FOCUS.
  const int startX = 74;
  const int y = 29;
  const int segW = 13;
  const int segH = 3;
  const int gap = 3;

  uint32_t phase = now % 4500UL;
  float progress = (float)phase / 4500.0f;

  for (int i = 0; i < FOCUS_COUNT; i++) {
    int x = startX + i * (segW + gap);
    uint16_t c = focusMetricEnabled(i) ? p.border : p.panel2;

    if (i == metric)
      c = p.accent;

    tft.fillRect(x, y, segW, segH, c);
  }

  // W aktywnym segmencie widac subtelny postep do kolejnej wartosci.
  int activeX = startX + metric * (segW + gap);
  int fillW = (int)(progress * segW);
  if (fillW < 1) fillW = 1;
  if (fillW > segW) fillW = segW;
  tft.fillRect(activeX, y, fillW, segH, p.text);
}



// ============================================================
// RC15.140 - FOCUS BIG CHART OVERLAY
// RC15.141: zakres 1H/6H/24H wybierany z dolnego paska.
// RC15.149 FOCUS CHART STATS:
// RC15.149A COMPILE FIX:
// RC15.150 FOCUS CHART COVERAGE:
// RC15.151 FOCUS CHART TREND:
// RC15.152 FOCUS TREND DIRECTION:
// RC15.153 FOCUS MINMAX TIME:
// RC15.154 FOCUS CHART SPACED HEADER:
// RC15.155 FOCUS MINMAX TIME ROBUST:
// RC15.156 HISTORY TIME ROBUST:
// RC15.157 FOCUS MINMAX MARKERS:
// RC15.158 FOCUS MINMAX MARKERS SAFE:
// RC15.159 FOCUS MINMAX MARKERS VISIBILITY:
// RC15.160 FOCUS CHART NO INNER MINMAX LABELS:
// RC15.161 FOCUS MINMAX DATE 7D:
// RC15.162 FOCUS TREND PERCENT:
// RC15.162A WIDTH SAFE:
// RC15.163 FOCUS TREND STRENGTH:
// RC15.164 FOCUS HISTORY GAP SAFE:
// RC15.166 FOCUS TIME AXIS ROBUST:
// RC15.167 FOCUS TIME AXIS STRICT:
// RC15.168 TIME AXIS CONSISTENT ALL CHARTS:
// RC15.169 TIME AUDIT CLEANUP:
// RC15.170 ALERT TIME CONSISTENCY:
// RC15.170A COMPILE FIX:
// RC15.171 ALERT WORST TIME CONSISTENCY:
// RC15.172 FOCUS CURRENT RANGE MARKER:
// RC15.173 FOCUS OK ZONE GUIDES:
// RC15.174 FOCUS OK ZONE CURRENT STATE:
// RC15.175 FOCUS TREND FROM RANGE START:
// RC15.175A COMPILE FIX:
// RC15.176 FOCUS LOGIC AUDIT FIX:
// RC15.176A FINAL AUDIT:
// RC15.177 SOURCE STATUS CONSISTENCY:
// RC15.177A COMPILE FIX:
// RC15.179 DIAGNOSTICS SOURCE CHAIN:
// RC15.180 DATA FRESHNESS FRAME:
// RC15.180A BOTTOM FRAME FIX:
// RC15.180B REDRAW SAFE:
// RC15.180C FINAL LAYER:
// RC15.180D STATE REDRAW FIX:
// RC15.181 DATA FRESHNESS STATUS STRIP:
// RC15.181A NO OVERLAP:
// RC15.182 FULL FRAME FINAL:
// RC15.182A FOCUS TOP GAP CLEAR FIX:
// RC15.182B EDGE SAFE:
// RC15.182C RETURN FREEZE:
// RC15.183 RGB SOURCE PRIORITY:
// RC15.183A RX FLICKER FIX:
// RC15.184 FOCUS RGB + FRAME RX SAFE:
// RC15.184A COMPILE FIX:
// RC15.185 RGB GLOBAL INDOOR COVERAGE:
// RC15.186 PRESSURE TENDENCY 3H:
// RC15.186A LINK FIX:
// RC15.187 BAROMETRIC TREND ARROWS:
// RC15.188 BAROMETRIC CHANGE SIGNAL:
// RC15.189 PRESSURE TREND LABEL CONSISTENCY:
// RC15.190 WEATHER CHANGE SIGNAL:
// RC15.191 FOCUS PRESSURE CONTEXT:
// RC15.192 FOCUS WIND CONTEXT:
// RC15.193 FOCUS HUMIDITY DEWPOINT CONTEXT:
// RC15.194 FOCUS TEMPERATURE INOUT CONTEXT:
// RC15.195 FOCUS RAIN TOTAL CONTEXT:
// RC15.196 SETTINGS PAGE 2 RGB/FRAME:
// RC15.197 SETTINGS FRAME OFF CLEANUP:
// RC15.198 SOURCE STALE FALLBACK FIX:
// RC15.198A LAST ACTIVE SOURCE HOLD:
// RC15.199 LOCAL RX TIMESTAMP ORDER:
// RC15.200 ARCHIVE TAIL EPOCH SYNC CHECK:
// RC15.201 INDOOR EPOCH ALIGNMENT:
// RC15.202 WINDDIR ARCHIVE INTEGRITY:
// RC15.203 MAIN COMMIT GUARD:
// RC15.204 RESTART CADENCE FROM EPOCH:
// RC15.205 NO EPOCH NO HISTORY WRITE:
// RC15.206 HISTORY TIME DIAGNOSTICS:
// RC15.207 7D HOURLY COVERAGE AUDIT:
// RC15.208A 7D HOUR CENTER NO GROUPING CHANGE:
// RC15.209 7D STATS QUALITY WEIGHTING:
// RC15.210 7D PEAK STATS LABELS:
// RC15.211 FOCUS MINMAX EPOCH MARKERS:
// RC15.212 7D MINICHART QUALITY STATS:
// RC15.213 7D ALERT QUALITY GUARD:
// RC15.214 ALERT SUMMARY METRIC STREAK SYNC:
// RC15.215 ALERT SUMMARY LABEL CLARITY:
// RC15.216 7D TRUE TIME WINDOW:
// RC15.216A COMPILE FIX:
// RC15.217 7D QUALITY COVERAGE:
// RC15.218 7D COUNTER COVERAGE SYNC:
// RC15.219 FOCUS TIME LABEL LOCALIZATION:
// RC15.222 FOCUS TREND GAP GUARD:
// RC15.223A 7D LOAD FAILSAFE:
// RC15.224 POWERLOSS TAIL REPAIR SAFE:
// RC15.224A 7D BUFFER FIX:
// RC15.225 TAIL REPAIR RECOVERY:
// RC15.226 CSV TAIL RECOVERY:
// przy starcie usuwa tylko niedokonczona ostatnia linie CSV po zaniku zasilania.
// Uzywa osobnych TMP/BAK i odzyskuje backup, jesli reset nastapi podczas naprawy CSV.
// osobne TMP/BAK dla MAIN/INDOOR/WIND-DIR i odzyskanie backupu po zaniku zasilania w trakcie naprawy.
// Chroni przed utrata pliku, gdy reset nastapi miedzy rename(path->bak) i rename(tmp->path).
// naprawia zamiane buforow MAIN <-> WIND-DIR w failsafe 7D.
// loadChart7DFromSD() resetuje tylko chart7d*, loadWindDir7DFromSD() tylko windDir7d*.
// zachowuje RC15.223A i naprawia tylko niepelny ogon binarnych plikow po zaniku zasilania.
// Zdrowy plik nie jest przepisywany. Uszkodzony ogon jest usuwany przez kopie pelnych rekordow.
// transientny blad SD.open/seek nie zeruje istniejacego bufora 7D.
// Przy bledzie loader ustawia Loaded=false i zachowuje ostatnie dobre dane do kolejnej proby.
// duzy FOCUS liczy trend tylko w ostatnim ciaglym segmencie po luce czasu.
// MIN/MAX/SR oraz sama krzywa pozostaja bez zmian.
// RC15.220 WINDDIR 7D QUALITY SYNC:
// kierunek 7D zapisuje liczbe probek dla kazdej godziny.
// Pokrycie oraz DOM/STAB pomijaja LOW (<6), ale sama krzywa nadal je pokazuje.
// duzy wykres FOCUS uzywa tego samego TERAZ/NOW/JETZT/NYNI co zwykle wykresy.
// Oś czasu, dane i statystyki pozostaja bez zmian.
// liczniki x/168 i H:x/168 w 7D uzywaja tych samych wiarygodnych godzin q>=6 co procent i pasek.
// LOW pozostaje widoczne na wykresie, ale nie jest liczone jako kompletna godzina.
// pokrycie 7D liczy tylko wiarygodne godziny q>=6 wzgledem pelnych 168 godzin.
// LOW pozostaje na wykresie, ale nie podnosi procentu kompletnej historii.
// filtr cutoff 7D dziala tylko w loaderach 7D; 24H pozostaje bez zmian.
// agregacja 7D odrzuca rekordy starsze niz 7 dni wzgledem najnowszego poprawnego epoch.
// Chroni to 7D po dluzszych przerwach: 168 slotow nie moze zostac zajete przez stare godziny.
// dolne podsumowanie uzywa etykiety ALERT zamiast mylacego MAX.
// MAX oznaczalo 'najgorszy alert', a nie najwyzsza wartosc pomiaru.
// dolne podsumowanie MAX/L/T trzyma wszystkie pola przy tym samym parametrze.
// L i T nie sa juz brane jako maksimum z innych metryk.
// w zakresie 7D godziny LOW (1..5 probek) nie biora udzialu w alarmach.
// Dotyczy A/N, biezacej serii T, epizodow E, najdluzszej serii i MAX alertu.
// LOW pozostaje widoczne na wykresie, ale nie jest traktowane jak pelna godzina alarmowa.
// miniwykres 7D nadal rysuje wszystkie punkty, lacznie z LOW,
// ale jego znak trendu oraz L/H sa liczone tylko z godzin q>=6.
// Dzięki temu miniwykres i karty TREND/STAT uzywaja tej samej jakosci danych.
// graficzne znaczniki MIN/MAX sa kotwiczone do epoch faktycznego ekstremum.
// Eliminuje mozliwy rozjazd, gdy godzina LOW ma podobna wartosc jak statystyczny MIN/MAX.
// Sama krzywa nadal pokazuje wszystkie punkty, lacznie z LOW.
// dla PORYW/UV/OPAD w 7D punkty sa godzinowymi maksimami,
// dlatego AVG/SR jest jawnie opisane jako AVG MAX / SR MAX.
// Matematyka i dane wykresu pozostaja bez zmian.
// punkty LOW (1..5 probek/h) pozostaja na wykresie, ale nie wchodza do statystyk 7D.
// srednie dla metryk srednich sa wazone rzeczywista liczba probek w godzinie.
// MIN/MAX/TR korzystaja z godzin o pokryciu co najmniej 6/12.
// gotowe punkty 7D z prawidlowym epoch dostaja HH:30,
// ale logika tworzenia koszykow pozostaje identyczna jak w RC15.207.
// dodaje diagnostyke kompletności godzin 7D dla MAIN, INDOOR i WIND-DIR.
// Nie zmienia agregacji ani rysowania; tylko raportuje FULL/PART/LOW.
// dodaje jednoznaczny log stanu zegara historii: epoch, valid i READY/PAUSED_TIME.
// Nie zmienia logiki zapisu RC15.205.
// historia MAIN/INDOOR/DIR/CSV nie zapisuje nowych rekordow bez prawidlowego czasu epoch.
// Przy NTP=BRAK archiwum czeka; po odzyskaniu czasu zapis wraca automatycznie.
// Chroni tez przed niechronologicznym zapisem, gdy zegar chwilowo cofnie sie za ostatni MAIN.
// po restarcie termin nastepnej probki wynika z epoch ostatniego MAIN,
// a nie z pelnych 5 minut liczonych od uruchomienia CYD.
// Gdy prawdziwy czas jest niedostepny, pozostaje bezpieczny fallback millis().
// 5-minutowy cykl jest zatwierdzany zegarem dopiero po udanym zapisie MAIN.
// Gdy MAIN nie zapisze sie, nastepny swiezy pakiet moze sprobowac ponownie bez duplikatu MAIN.
// Po udanym MAIN zegar jest zamykany od razu, wiec bledy sidecar/CSV nie tworza duplikatu MAIN.
// rozszerza audyt archiwum o sidecar kierunku wiatru: alignment, checksum i tail epoch.
// Sidecar 24H/7D jest samodzielny czasowo, wiec nie wymaga laczenia pozycyjnego z MAIN.
// 24H MAIN<->INDOOR laczone po epoch zamiast po pozycji rekordu.
// Pojedynczy brak zapisu INDOOR nie przesuwa juz wszystkich pozniejszych danych WEW.
// Dla starych rekordow bez epoch pozostaje zgodny fallback pozycyjny.
// kontrola archiwum porownuje epoch ostatniego poprawnego rekordu MAIN i INDOOR.
// Rozna liczba rekordow historycznych jest dozwolona; wazna jest wspolna koncowka czasu.
// Diagnostyka tylko wykrywa problem - niczego automatycznie nie kasuje ani nie skraca.
// haveData/lastPacketMs sa zatwierdzane razem z pakietem w callbacku ESP-NOW.
// Selektor zrodla widzi swiezy LOCAL juz w pierwszej petli po odbiorze.
// gdy oba zrodla sa nieswieze, utrzymujemy ostatnio faktycznie aktywne zrodlo,
// o ile jego bufor istnieje. Zapobiega L* -> I OFFLINE po utracie LilyGO.
// gdy zaden kanal nie jest swiezy, zachowujemy najnowszy faktycznie posiadany bufor.
// Zapobiega falszywemu WAIT po utracie INTERNETU, gdy LOCAL nigdy nie mial danych.
// FRAME OFF natychmiast usuwa istniejaca ramke po wyjsciu z MENU przez pelny redraw.
// Audyt: hitbox 2/2 i POWROT sa rozdzielone; CAL/DIAG zachowuja aktywna strone MENU.
// druga strona MENU bez sciskania obecnych kontrolek.
// RGB ON/OFF i RAMKA STALE/OFFLINE ON/OFF sa zapisywane w Preferences.
// RGB OFF dziala takze od startu; FRAME OFF nie zmienia logiki LIVE/STALE/OFFLINE.
// FOCUS->OPAD pokazuje pod tempem mm/h takze laczny licznik opadu z pakietu.
// Minimalna zmiana; bez zmian historii, RGB, ramki, dotyku i geometrii.
// FOCUS->TEMP ZEW/WEW pokazuje jedna mala linie z temperatura po drugiej stronie
// oraz roznica DT = TEMP WEW - TEMP ZEW. Bez zmian geometrii, RGB i historii.
// FOCUS->WILG ZEW/WEW pokazuje jedna mala linie z punktem rosy.
// Bez zmian geometrii, RGB, historii, dotyku i progow alarmowych.
// FOCUS->WIATR i PORYW pokazuja jedna mala linie porownawcza.
// WIATR: PORYW x.x m/s; PORYW: WIATR x.x m/s. Bez zmian geometrii i RGB.
// FOCUS->CISNIENIE pokazuje pod duza wartoscia trend 3H + sygnal warunkow.
// Tylko jedna mala linia; bez zmian geometrii FOCUS, dotyku, RGB i historii.
// lekki sygnal zmiany warunkow na TREND ZEW: cisnienie 3H + wilgotnosc + opad.
// To wskazowka pomocnicza, nie prognoza pogody.
// opis w footerze uzywa tych samych progow co symbol 3H:
// >=+1.5 szybki wzrost, +0.5..+1.5 wzrost, -0.5..+0.5 stabilnie,
// -1.5..-0.5 spadek, <=-1.5 szybki spadek.
// na WYKRESY->TREND->ZEW footer pokazuje opis tendencji cisnienia 3H.
// To opis pomiaru (ROSNIE/STABILNE/SPADA), nie prognoza pogody.
// tendencja 3H cisnienia uzywa intuicyjnych symboli kierunku zamiast ++/--.
// Bez zmian obliczen, historii SD, RGB, ramki i nawigacji.
// dodane rzeczywiste definicje helperow pressureTendency3H*; w RC15.186 byly tylko prototypy.
// kafelek CISN na stronie TREND pokazuje dodatkowo zmiane cisnienia z ostatnich 3h.
// Trend 3H jest niezalezny od wybranego zakresu 1H/6H/24H/7D.
// Nie zmienia historii SD, nawigacji ani formatu danych.
// poza FOCUS globalna dioda RGB uwzglednia takze TEMP WEW i WILG WEW.
// FOCUS nadal reaguje tylko na aktualnie wyswietlana metryke.
// jawna deklaracja extern currentSkin przed currentLedLevel().
// 1) FOCUS: RGB reaguje tylko na aktualnie wyswietlana metryke.
// 2) Poza FOCUS: RGB nadal reaguje na najgorszy stan calej pogody.
// 3) Dynamiczne odswiezanie RX omija piksele ramki, usuwajac pulsowanie rogu.
// boczne krawedzie ramki sa x=1/318, aby odswiezanie RX od x=0
// nie kasowalo lewego boku. Gora/dol nadal obejmuja pelna szerokosc.
// RGB ma priorytet lacznosci nad pogoda: START/WIFI=niebieski,
// STALE=pomaranczowe miganie, OFFLINE=staly czerwony.
// Tylko przy LIVE wraca dotychczasowa sygnalizacja severity pogody.
// ramka nie jest rysowana podczas waitTouchRelease.
// Usuwa chwilowy blysk ramki przy POWROT z MENU / WYBOR / duzego wykresu.
// pelna ramka korzysta z fizycznych krawedzi ekranu x=0/319 i dolu y=239.
// Gora pozostaje y=32, aby nie kolidowac z FOCUS progress y=29..31.
// Eliminuje nakladanie dolnej krawedzi na kafelki/CHARTS/footer.
// FOCUS przy forceScreenClear czysci teraz takze szczeline y=27..33.
// To usuwa pozostala gorna krawedz ramki po OFFLINE/STALE -> LIVE.
// wracamy do pelnej ramki, ale bez konfliktu z FOCUS progress.
// Gorna krawedz y=32, boki x=1/318, dol y=209.
// Ramka jest ostatnia warstwa loop(); LIVE usuwa ja przez jednorazowy pelny redraw.
// pasek swiezosci przeniesiony z y=28..30 na y=32..33.
// FOCUS progress zajmuje y=29..31, wiec oba elementy nie dziela juz pikseli.
// rezygnujemy z obwodki calego ekranu; stan danych ma wlasny pas y=28..30.
// Pasek jest zawsze czyszczony przed narysowaniem nowego stanu, wiec kolory
// nie moga sie nakladac. LIVE=brak paska, STALE=przerywany, OFFLINE=pelny czerwony.
// forceScreenClear po zmianie LIVE/STALE/OFFLINE ma pierwszenstwo przed
// optymalizacjami FOCUS/CHARTS/GRID, wiec stara ramka jest zawsze fizycznie usuwana.
// ramka jest ostatnia warstwa calego loop(), po wszystkich redraw skinow i stopki.
// Nie czysci swoich krawedzi kolorem tla. Zmiana stanu wymusza jeden pelny redraw.
// ramka swiezosci jest odtwarzana po kazdym odswiezeniu widoku/statusu,
// dzieki czemu kafelki, stopka ani inne dynamiczne redraw nie moga jej trwale skasowac.
// dolna krawedz ramki konczy sie nad dynamiczna stopka/przyciskami,
// aby ich odswiezanie nie nadpisywalo fragmentu ramki.
// wszystkie skiny dostaja wspolny subtelny sygnal swiezosci danych:
// LIVE=brak ramki, STALE=przerywana ramka ostrzegawcza, OFFLINE=ramka alarmowa.
// Nie zaslania kafelkow, FOCUS ani dolnych przyciskow.
// diagnostyka rozdziela lancuch danych na LOCAL / WIFI / AIO / NTP-CZAS.
// Bez zmian failover, FOCUS, historii, dotyku i formatu pakietu.
// SourceUiState przeniesiony przed automatyczne prototypy Arduino IDE 1.8.x
// i dodane jawne prototypy helperow statusu zrodla.
// naglowek i stopka korzystaja z jednej logiki swiezosci aktywnego zrodla.
// Przy wymuszonym INTERNET i awarii backendu, swiezy fallback LOCAL pokazuje LIVE,
// a znacznik zrodla L* informuje, ze jest to fallback zamiast mylacego OFFLINE.
// poprawione currentAlertStreak oraz jawne prototypy helperow alarmowych
// dla generatora prototypow Arduino IDE 1.8.x.
// 1) prawa strzalka uzywa biezacego odczytu, nie ostatniej probki 5-min,
// 2) granice OK sa zgodne z klasyfikacja (w tym progi wykluczajace),
// 3) worst alert porownuje severity, nie numer enum Tone,
// 4) UV 6..11 nie wypada juz z licznikow alarmowych przez kolor WARM/HOT.
// poprawiona nazwa zmiennej lastV -> lastVStats w obliczeniu TR.
// TR jest jawnie liczony od pierwszej poprawnej widocznej probki
// wybranego zakresu do ostatniej poprawnej widocznej probki.
// prawy znacznik aktualnej wartosci korzysta z koloru stanu wzgledem strefy OK.
// Zielony = w strefie, niebieski = ponizej, czerwony = powyzej.
// Nie zmienia progow, historii, osi czasu ani geometrii wykresu.
// duzy wykres pokazuje subtelne przerywane granice strefy OK,
// korzystajac z tych samych progow co klasyfikacja kafelkow/alarmow.
// Bez dodatkowego tekstu i bez zmian historii/dotyku.
// po prawej stronie duzego wykresu pojawia sie maly znacznik pozycji
// aktualnej wartosci pomiedzy rzeczywistym MIN i MAX wybranego zakresu.
// Bez dodatkowego tekstu i bez zmian historii/dotyku.
// podsumowanie MAX/worstAlertToneForRange korzysta z tego samego okna czasu
// co wykresy i pozostale liczniki alarmow.
// AlertTimeWindow przeniesiony przed prototypy Arduino IDE 1.8.x.
// Dodane brakujace filtry czasu w currentAlertStreak i longestAlertStreak.
// alarmy korzystaja z tej samej osi czasu co wykresy.
// Punkty epoch=0 i punkty poza aktualnym oknem sa pomijane,
// jesli w zakresie istnieje wiarygodny czas.
// procent pokrycia FOCUS oraz karty STAT/TREND licza tylko punkty
// faktycznie nalezace do wybranego okna czasu i widoczne na osi.
// Rysowanie RC15.168 pozostaje bez zmian.
// duze i male wykresy uzywaja tej samej osi czasu.
// Punkty epoch=0 sa pomijane, jesli istnieje wiarygodna os czasu.
// Punkty starsze niz wybrany zakres sa pomijane zamiast przyklejania do lewej krawedzi.
// Statystyki liczone sa tylko z punktow faktycznie widocznych na osi.
// przy obecnosci poprawnego epoch nie zgadujemy pozycji rekordow epoch=0.
// Takie punkty sa pomijane w rysowaniu i przerywaja linie.
// Gdy caly widoczny zakres nie ma czasu, dopiero wtedy uzywany jest fallback indeksowy.
// poprawna stala os czasu -1H/-6H/-24H/-7D -> TERAZ.
// Rekordy z epoch=0 dostaja tylko pozycje szacowana z kroku 5 min / 1 h,
// ale nigdy nie mieszamy dwoch roznych ukladow X w jednym wykresie.
// Prawdziwe luki z poprawnym epoch zachowuja proporcjonalna szerokosc.
// duzy wykres nie laczy sztuczna linia probek rozdzielonych luka czasowa.
// 1H/6H/24H: >15 min, 7D: >90 min = przerwa w krzywej.
// trend historyczny dostaje klase sily: stabilny / lagodny / szybki.
// Klasa jest kodowana kompaktowym symbolem w TR, bez nowego wiersza.
// pasek statystyk mierzy rzeczywista szerokosc tekstu fontem 1.
// Gdy pelny TR z procentem sie nie miesci, procent jest automatycznie pomijany;
// w kolejnym kroku zmniejszane sa odstepy. Brak wyjscia tekstu poza bezpieczny obszar.
// TR pozostaje zmiana bezwzgledna, a dla metryk gdzie procent ma sens
// dopisujemy zmiane wzgledna w nawiasie. Bez dodatkowego wiersza.
// dla 1H/6H/24H czas ekstremum pozostaje HH:MM;
// dla 7D pokazujemy DD.MM HH:MM, aby bylo wiadomo ktorego dnia bylo MIN/MAX.
// usuniete powielone liczby skali MIN/MAX z lewego wnetrza wykresu.
// MIN/MAX sa juz w pasku statystyk, a markery pozostaja w pelni widoczne.
// znaczniki MIN/MAX maja wiekszy wewnetrzny margines od ramki,
// aby nie zlewaly sie z lewym/dolnym/gornym obramowaniem wykresu.
// znaczniki MIN/MAX sa ograniczone do wnetrza ramki; dla praktycznie plaskiego
// przebiegu nie rysujemy dwoch nakladajacych sie trojkatow.
// na duzym wykresie FOCUS punkty MIN i MAX sa oznaczone graficznie.
// Bez dodatkowych napisow w naglowku i bez zmian w historii/dotyku.
// zapis historii pobiera epoch z time(nullptr) zawsze, gdy zegar systemowy jest poprawny,
// niezaleznie od chwilowego stanu flagi ntpReady. Ogranicza nowe rekordy epoch=0.
// MIN@ i MAX@ sa wyswietlane niezaleznie; brak epoch jednego ekstremum
// nie ukrywa juz calego wiersza. Brak czasu = --:--.
// uporzadkowany pionowy uklad: metryka/wartosc, statystyki, czas MIN/MAX, wykres.
// Wykres zaczyna sie nizej, aby teksty nie byly scisniete przy ramce.
// dla poprawnego epoch pokazuje godzine wystapienia MIN i MAX w aktualnym zakresie.
// Bez poprawnego czasu statystyki pozostaja jak w RC15.152.
// TR dostaje jednoznaczny znak kierunku: ^ wzrost, v spadek, = stabilnie.
// Prog stabilnosci zalezy od metryki, aby szum pomiarowy nie udawal trendu.
// duzy wykres pokazuje zmiane pierwsza->ostatnia probka dla aktualnego zakresu.
// To trend historyczny, nie prognoza.
// naglowek duzego wykresu pokazuje faktyczne wypelnienie zakresu 0..100%.
// Pozwala odroznic pelne 7D od dopiero zbieranej historii.
// jawne (unsigned int) dla String(float, decimalPlaces) na ESP32 core 2.0.14.
// duzy wykres pokazuje MIN / MAX / SR dla aktualnego zakresu.
// Kierunek zachowuje osobne DOM / STAB z RC15.147.
// Korzysta z istniejacego chartHistory - bez nowego duzego bufora RAM.
// ============================================================

float focusChartSampleValue(const ChartSample &s, uint8_t metric) {
  switch (metric) {
    case FOCUS_TEMP:    return s.temp;
    case FOCUS_HUM:     return s.hum;
    case FOCUS_TEMP_IN: return s.tempIn;
    case FOCUS_HUM_IN:  return s.humIn;
    case FOCUS_PRESS:   return s.press;
    case FOCUS_WIND:    return s.wind;
    case FOCUS_GUST:    return s.gust;
    case FOCUS_UV:      return s.uv;
    case FOCUS_LUX:     return s.lux;
    case FOCUS_RAIN:    return s.rain;
    default:            return NAN;  // kierunek nie jest zapisany w ChartSample
  }
}

String focusChartUnit(uint8_t metric) {
  switch (metric) {
    case FOCUS_TEMP:
    case FOCUS_TEMP_IN: return "C";
    case FOCUS_HUM:
    case FOCUS_HUM_IN:  return "%";
    case FOCUS_PRESS:   return "hPa";
    case FOCUS_WIND:
    case FOCUS_GUST:    return "m/s";
    case FOCUS_UV:      return "";
    case FOCUS_LUX:     return "lx";
    case FOCUS_RAIN:    return "mm/h";
    default:            return "";
  }
}

float focusChartCurrentValue(uint8_t metric, const WeatherPacket &d) {
  switch (metric) {
    case FOCUS_TEMP:    return d.temperatura;
    case FOCUS_HUM:     return d.wilgotnosc;
    case FOCUS_TEMP_IN: return d.temp_wewnetrzna;
    case FOCUS_HUM_IN:  return d.wilg_wewnetrzna;
    case FOCUS_PRESS:   return d.cisnienie;
    case FOCUS_WIND:    return d.predkosc_wiatru;
    case FOCUS_GUST:    return d.poryw_wiatru;
    case FOCUS_DIR:     return d.kierunek_wiatru;
    case FOCUS_UV:      return d.uv_index;
    case FOCUS_LUX:     return d.swiatlo_lux;
    case FOCUS_RAIN:    return d.opady_godzina;
    default:            return NAN;
  }
}

String focusChartCurrentText(uint8_t metric, const WeatherPacket &d) {
  switch (metric) {
    case FOCUS_TEMP:    return fmt(d.temperatura, 1);
    case FOCUS_HUM:     return fmt(d.wilgotnosc, 0);
    case FOCUS_TEMP_IN: return fmt(d.temp_wewnetrzna, 1);
    case FOCUS_HUM_IN:  return fmt(d.wilg_wewnetrzna, 0);
    case FOCUS_PRESS:   return fmtPressure(d.cisnienie);
    case FOCUS_WIND:    return fmt(d.predkosc_wiatru, 1);
    case FOCUS_GUST:    return fmt(d.poryw_wiatru, 1);
    case FOCUS_DIR: {
      if (!isfinite(d.kierunek_wiatru)) return "--";
      float dir = normalizeWindDirDeg(d.kierunek_wiatru);
      return windDir(dir) + " " + String(dir, 0) + " st";
    }
    case FOCUS_UV:      return fmt(d.uv_index, 1);
    case FOCUS_LUX: {
      String u;
      return fmtLux(d.swiatlo_lux, u);
    }
    case FOCUS_RAIN:    return fmt(d.opady_godzina, 1);
    default:            return "--";
  }
}

String focusChartAsciiLabel(uint8_t metric) {
  switch (metric) {
    case FOCUS_TEMP:    return tr(TXT_TEMP_OUT);
    case FOCUS_HUM:     return tr(TXT_HUM_OUT);
    case FOCUS_TEMP_IN: return tr(TXT_TEMP_IN);
    case FOCUS_HUM_IN:  return tr(TXT_HUM_IN);
    case FOCUS_PRESS:   return tr(TXT_PRESSURE);
    case FOCUS_WIND:    return tr(TXT_WIND);
    case FOCUS_GUST:    return tr(TXT_GUST);
    case FOCUS_DIR:     return tr(TXT_DIRECTION);
    case FOCUS_UV:      return "UV";
    case FOCUS_LUX:     return tr(TXT_LIGHT);
    case FOCUS_RAIN:    return tr(TXT_RAIN);
    default:            return "FOCUS";
  }
}

uint16_t focusChartWantedSamples() {
  if (focusChartRange == 0) return 12;              // 1H, probki 5 min
  if (focusChartRange == 2) return 288;             // 24H
  if (focusChartRange == 3) return CHART_7D_POINTS; // 7D, probki godzinowe
  return 72;                                        // 6H
}

String focusChartRangeName() {
  if (focusChartRange == 0) return "1H";
  if (focusChartRange == 2) return "24H";
  if (focusChartRange == 3) return "7D";
  return "6H";
}

String focusChartLeftLabel() {
  if (focusChartRange == 0) return "-1H";
  if (focusChartRange == 2) return "-24H";
  if (focusChartRange == 3) return "-7D";
  return "-6H";
}

bool focusChartMetricUsesIndoor(uint8_t metric) {
  return metric == FOCUS_TEMP_IN || metric == FOCUS_HUM_IN;
}

String focusChartTimeShort(uint32_t epoch) {
  if (epoch <= 100000UL)
    return "";

  time_t tt = (time_t)epoch;
  struct tm tmv;
  if (!localtime_r(&tt, &tmv))
    return "";

  // RC15.161:
  // Na 7D sama godzina jest niejednoznaczna, wiec pokazujemy dzien i godzine.
  if (focusChartRange == 3) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d.%02d %02d:%02d",
             tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_hour, tmv.tm_min);
    return String(buf);
  }

  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  return String(buf);
}


uint32_t historyEpochNow() {
  // RC15.156: nie uzalezniamy archiwum od samej flagi ntpReady.
  // Po poprawnej synchronizacji zegar ESP32 dalej chodzi nawet przy utracie Wi-Fi.
  time_t nowEpoch = time(nullptr);
  if (nowEpoch > 100000)
    return (uint32_t)nowEpoch;
  return 0;
}



bool historyEpochTrajectoryOK(uint32_t nowMs, uint32_t nowEpoch) {
  if (nowEpoch <= 100000UL)
    return false;

  // Pierwszy poprawny czas w tym uruchomieniu ustanawia kotwice.
  // To celowo pozwala na dowolnie dluga prawdziwa przerwe miedzy restartami.
  if (historyClockAnchorEpoch <= 100000UL) {
    historyClockAnchorEpoch = nowEpoch;
    historyClockAnchorMs = nowMs;
    return true;
  }

  uint32_t elapsedSec = (uint32_t)(nowMs - historyClockAnchorMs) / 1000UL;
  uint64_t maxExpected = (uint64_t)historyClockAnchorEpoch +
                         (uint64_t)elapsedSec +
                         (uint64_t)HISTORY_MAX_FORWARD_JUMP_SEC;

  if ((uint64_t)nowEpoch > maxExpected) {
    historyClockJumpBlocks++;
    static unsigned long lastJumpLogMs = 0;
    if (nowMs - lastJumpLogMs >= 10000UL) {
      lastJumpLogMs = nowMs;
      Serial.print("[HIST-TIME] FORWARD_JUMP_BLOCK now=");
      Serial.print(nowEpoch);
      Serial.print(" anchor=");
      Serial.print(historyClockAnchorEpoch);
      Serial.print(" elapsed=");
      Serial.print(elapsedSec);
      Serial.print(" limit=");
      Serial.println((uint32_t)maxExpected);
    }
    return false;
  }

  // Cofniecie czasu nie aktualizuje kotwicy wstecz.
  // Sama funkcja historySampleDue() i tak blokuje zapis <= lastCommittedHistoryEpoch.
  if (nowEpoch >= historyClockAnchorEpoch) {
    historyClockAnchorEpoch = nowEpoch;
    historyClockAnchorMs = nowMs;
  }

  return true;
}

bool historySampleDue(uint32_t nowMs, uint32_t nowEpoch) {
  // RC15.205: bez wiarygodnego czasu NIE zapisujemy historii.
  // Rekord epoch=0 psuje os czasu 24H/7D i synchronizacje sidecarow.
  if (nowEpoch <= 100000UL)
    return false;

  // RC15.229: nie pozwalamy jednemu falszywemu skokowi NTP do przodu
  // zatruc osi czasu i pozniej zablokowac historii na godziny/dni/lata.
  if (!historyEpochTrajectoryOK(nowMs, nowEpoch))
    return false;

  // Jesli znamy ostatni zatwierdzony MAIN, rytm liczony jest wyłącznie
  // po prawdziwym czasie. Cofniecie zegara nie moze stworzyc rekordu
  // starszego od juz zapisanego.
  if (lastCommittedHistoryEpoch > 100000UL) {
    if (nowEpoch <= lastCommittedHistoryEpoch)
      return false;

    return (nowEpoch - lastCommittedHistoryEpoch) >=
           (CHART_SAMPLE_MS / 1000UL);
  }

  // Nowe/puste archiwum: epoch jest juz prawidlowy, a millis decyduje
  // tylko kiedy wykonac pierwsza probe.
  return (uint32_t)(nowMs - lastChartSampleMs) >= CHART_SAMPLE_MS;
}


void printHistoryTimeDiagnostics(const char *tag) {
  uint32_t e = historyEpochNow();
  bool valid = e > 100000UL;

  Serial.print("[HIST-TIME] ");
  if (tag && *tag) {
    Serial.print(tag);
    Serial.print(" ");
  }

  Serial.print("epoch=");
  Serial.print(e);
  Serial.print(" valid=");
  Serial.print(valid ? "YES" : "NO");
  Serial.print(" state=");
  Serial.print(valid ? "READY" : "PAUSED_TIME");
  Serial.print(" last=");
  Serial.println(lastCommittedHistoryEpoch);
}

uint16_t focusChartVisibleTimedCount(uint8_t metric,
                                      bool use7d,
                                      uint16_t sourceCount) {
  if (sourceCount == 0)
    return 0;

  uint16_t wanted = focusChartWantedSamples();
  uint16_t count = sourceCount > wanted ? wanted : sourceCount;
  uint16_t logicalStart = sourceCount - count;

  uint32_t newestEpoch = 0;

  for (uint16_t i = 0; i < count; ++i) {
    uint32_t epoch = 0;

    if (metric == FOCUS_DIR) {
      WindDirSample s = use7d
                        ? windDir7d[logicalStart + i]
                        : windDirHistory[windDirIndexOldest(logicalStart + i)];
      epoch = s.epoch;
    } else {
      bool indoorMetric = focusChartMetricUsesIndoor(metric);
      const ChartSample *sp = nullptr;

      if (use7d) {
        sp = indoorMetric
               ? &indoor7d[logicalStart + i]
               : &chart7d[logicalStart + i];
      } else {
        int idx = chartIndexOldest(logicalStart + i);
        sp = &chartHistory[idx];
      }

      epoch = sp->epoch;
    }

    if (epoch > newestEpoch)
      newestEpoch = epoch;
  }

  // Brak jakiegokolwiek poprawnego czasu = zachowujemy zgodny fallback indeksowy.
  if (newestEpoch <= 100000UL)
    return count;

  uint32_t axisEnd = historyEpochNow();
  if (axisEnd <= 100000UL || axisEnd < newestEpoch)
    axisEnd = newestEpoch;

  uint32_t windowSec = 21600UL;
  if (focusChartRange == 0) windowSec = 3600UL;
  else if (focusChartRange == 2) windowSec = 86400UL;
  else if (focusChartRange == 3) windowSec = 604800UL;

  uint32_t axisStart = axisEnd > windowSec ? axisEnd - windowSec : 0;
  uint16_t validVisible = 0;

  for (uint16_t i = 0; i < count; ++i) {
    uint32_t epoch = 0;
    bool valueOK = false;

    if (metric == FOCUS_DIR) {
      WindDirSample s = use7d
                        ? windDir7d[logicalStart + i]
                        : windDirHistory[windDirIndexOldest(logicalStart + i)];
      epoch = s.epoch;
      valueOK = isfinite(normalizeWindDirDeg(s.directionDeg));
    } else {
      bool indoorMetric = focusChartMetricUsesIndoor(metric);
      const ChartSample *sp = nullptr;

      if (use7d) {
        sp = indoorMetric
               ? &indoor7d[logicalStart + i]
               : &chart7d[logicalStart + i];
      } else {
        int idx = chartIndexOldest(logicalStart + i);
        sp = &chartHistory[idx];
      }

      epoch = sp->epoch;
      valueOK = isfinite(focusChartSampleValue(*sp, metric));
    }

    if (valueOK &&
        epoch > 100000UL &&
        epoch >= axisStart &&
        epoch <= axisEnd) {
      bool qualityOK = true;
      if (use7d) {
        if (metric == FOCUS_DIR) {
          qualityOK = windDir7dSamples[logicalStart + i] >= 6;
        } else {
          bool indoorMetric = focusChartMetricUsesIndoor(metric);
          qualityOK = chart7dQualitySamples(indoorMetric, logicalStart + i) >= 6;
        }
      }
      if (qualityOK)
        validVisible++;
    }
  }

  return validVisible;
}

// RC15.150: procent rzeczywiscie zebranego zakresu dla duzego FOCUS.
// sourceCount jest liczba probek dostepnych dla aktualnej metryki.
void drawFocusChartCoverage(uint16_t sourceCount) {
  SkinPalette p = paletteFor(SKIN_FOCUS);
  uint16_t wanted = focusChartWantedSamples();

  uint16_t actual = sourceCount;
  if (actual > wanted) actual = wanted;

  uint8_t pct = 0;
  if (wanted > 0) {
    uint32_t p32 = ((uint32_t)actual * 100UL) / wanted;
    if (p32 > 100UL) p32 = 100UL;
    pct = (uint8_t)p32;
  }

  String s = focusChartRangeName();
  s += " ";
  s += String((unsigned int)pct);
  s += "%";

  // Czyscimy tylko prawa czesc naglowka, bez naruszania tytulu.
  tft.fillRect(220, 0, 100, 30, p.panel2);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(p.muted, p.panel2);
  tft.drawString(s, 312, 7, 2);
  tft.setTextDatum(TL_DATUM);
}

bool focusChartHistoryGap(uint32_t prevEpoch, uint32_t currEpoch, bool use7d) {
  if (prevEpoch <= 100000UL || currEpoch <= 100000UL)
    return false;

  if (currEpoch <= prevEpoch)
    return true;

  const uint32_t maxGap = use7d ? 5400UL : 900UL;
  return (currEpoch - prevEpoch) > maxGap;
}

uint32_t focusChartWindowSeconds() {
  if (focusChartRange == 0) return 3600UL;       // 1H
  if (focusChartRange == 2) return 86400UL;      // 24H
  if (focusChartRange == 3) return 604800UL;     // 7D
  return 21600UL;                                // 6H
}

int chartTimeXStrictRange(uint32_t epoch,
                          uint16_t i,
                          uint16_t count,
                          bool timeAxisAvailable,
                          uint32_t axisEndEpoch,
                          uint32_t windowSec,
                          int gx,
                          int gw) {
  if (timeAxisAvailable) {
    if (epoch <= 100000UL || axisEndEpoch <= 100000UL || windowSec == 0)
      return -32768;

    uint32_t axisStartEpoch =
        (axisEndEpoch > windowSec) ? (axisEndEpoch - windowSec) : 0;

    // RC15.168: punkt poza zakresem nie jest przyklejany do krawedzi.
    if (epoch < axisStartEpoch || epoch > axisEndEpoch)
      return -32768;

    uint64_t pos = (uint64_t)(epoch - axisStartEpoch);
    int x = gx + 2 +
            (int)((pos * (uint64_t)(gw - 5)) / (uint64_t)windowSec);

    if (x < gx + 2) x = gx + 2;
    if (x > gx + gw - 3) x = gx + gw - 3;
    return x;
  }

  // Caly widoczny zakres bez poprawnego czasu: bezpieczny fallback indeksowy.
  if (count <= 1)
    return gx + 2;

  return gx + 2 + ((int)i * (gw - 5)) / (count - 1);
}

int focusChartTimeXStrict(uint32_t epoch,
                          uint16_t i,
                          uint16_t count,
                          bool timeAxisAvailable,
                          uint32_t axisEndEpoch,
                          int gx,
                          int gw) {
  return chartTimeXStrictRange(epoch, i, count, timeAxisAvailable,
                               axisEndEpoch, focusChartWindowSeconds(), gx, gw);
}

uint32_t miniChartWindowSeconds() {
  switch (chartRange) {
    case CHART_RANGE_1H:  return 3600UL;
    case CHART_RANGE_6H:  return 21600UL;
    case CHART_RANGE_24H: return 86400UL;
    default:              return 604800UL;
  }
}

String miniChartMidFixedLabel() {
  switch (chartRange) {
    case CHART_RANGE_1H:  return "-30M";
    case CHART_RANGE_6H:  return "-3H";
    case CHART_RANGE_24H: return "-12H";
    default:              return "-3.5D";
  }
}

bool focusChartOkRange(uint8_t metric, float &low, float &high) {
  switch (metric) {
    case FOCUS_TEMP:
    case FOCUS_TEMP_IN:
      low = 5.0f; high = 26.0f; return true;

    case FOCUS_HUM:
    case FOCUS_HUM_IN:
      low = 30.0f; high = 60.0f; return true;

    case FOCUS_PRESS:
      low = 970.0f; high = 1035.0f; return true;

    case FOCUS_WIND:
      low = 0.0f; high = 5.0f; return true;

    case FOCUS_GUST:
      low = 0.0f; high = 8.0f; return true;

    case FOCUS_UV:
      low = 0.0f; high = 3.0f; return true;

    case FOCUS_LUX:
      low = 1000.0f; high = 50000.0f; return true;

    case FOCUS_RAIN:
      low = 0.0f; high = 0.05f; return true;

    default:
      return false;
  }
}

uint16_t focusChartCurrentStateColor(uint8_t metric,
                                     float value,
                                     uint16_t fallbackColor) {
  float okLow = 0.0f, okHigh = 0.0f;
  if (!isfinite(value) || !focusChartOkRange(metric, okLow, okHigh))
    return fallbackColor;

  if (value < okLow)
    return toneColor(TONE_COLD);

  // Część klasyfikacji ma górną granicę włączoną (<=),
  // a część wyłączoną (<). Zachowujemy dokładnie tę samą semantykę.
  bool above = false;
  switch (metric) {
    case FOCUS_WIND:
    case FOCUS_GUST:
    case FOCUS_UV:
    case FOCUS_LUX:
      above = (value >= okHigh);
      break;
    default:
      above = (value > okHigh);
      break;
  }

  if (above)
    return toneColor(TONE_HOT);

  return toneColor(TONE_OK);
}

void drawFocusOkZoneGuides(uint8_t metric,
                           float vMin,
                           float vMax,
                           int gx, int gy, int gw, int gh,
                           uint16_t bgColor) {
  float okLow = 0.0f, okHigh = 0.0f;
  if (!focusChartOkRange(metric, okLow, okHigh))
    return;

  if (!isfinite(vMin) || !isfinite(vMax) || vMax <= vMin)
    return;

  const float span = vMax - vMin;
  const uint16_t c = toneColor(TONE_OK);

  auto valueToY = [&](float v) -> int {
    float norm = (v - vMin) / span;
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    return gy + gh - 3 - (int)(norm * (gh - 6));
  };

  // Rysujemy tylko granice, ktore rzeczywiscie wpadaja w aktualna skale.
  // Linie sa przerywane i subtelne, zeby nie konkurowaly z krzywa.
  if (okLow > vMin && okLow < vMax) {
    int y = valueToY(okLow);
    for (int x = gx + 3; x < gx + gw - 3; x += 10)
      tft.drawFastHLine(x, y, 4, c);
  }

  if (okHigh > vMin && okHigh < vMax) {
    int y = valueToY(okHigh);
    for (int x = gx + 3; x < gx + gw - 3; x += 10)
      tft.drawFastHLine(x, y, 4, c);
  }

  (void)bgColor;
}

bool focusRangeStartDelta(float firstVisible, float lastVisible, float &delta) {
  if (!isfinite(firstVisible) || !isfinite(lastVisible))
    return false;
  delta = lastVisible - firstVisible;
  return isfinite(delta);
}

void drawFocusBigChart(uint8_t metric) {
  SkinPalette p = paletteFor(SKIN_FOCUS);
  WeatherPacket d = snapshotData();

  // RC15.230: prefetch 7D przed fillScreen/header/ramka.
  if (focusChartRange == 3) {
    if (metric == FOCUS_DIR) {
      if (!windDir7dLoaded)
        loadWindDir7DFromSD();
    } else if (focusChartMetricUsesIndoor(metric)) {
      if (!indoor7dLoaded)
        loadIndoor7DFromSD();
    } else {
      if (!chart7dLoaded)
        loadChart7DFromSD();
    }
  }

  // Overlay jest samodzielnym ekranem - zadnych smooth fontow.
  unloadSmoothFontSafe();
  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
  tft.fillScreen(p.bg);

  // Header
  tft.fillRect(0, 0, 320, 30, p.panel2);
  tft.setTextColor(p.accent, p.panel2);
  tft.drawString("FOCUS / WYKRES", 8, 7, 2);
  tft.setTextColor(p.muted, p.panel2);
  tft.drawRightString(focusChartRangeName(), 312, 7, 2);

  // Nazwa + aktualna wartosc
  String label = focusChartAsciiLabel(metric);
  String current = focusChartCurrentText(metric, d);
  String unit = focusChartUnit(metric);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(p.text, p.bg);
  if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ)
    drawSafeI18nString(label, 12, 34, 2, p.text, p.bg, TL_DATUM);
  else
    tft.drawString(label, 12, 34, 2);

  String nowText = current;
  if (unit.length()) nowText += " " + unit;
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(p.accent, p.bg);
  tft.drawString(nowText, 308, 34, 2);
  tft.setTextDatum(TL_DATUM);

  // Obszar wykresu.
  const int gx = 16;
  const int gy = 78;
  const int gw = 288;
  const int gh = 102;

  tft.drawRect(gx, gy, gw, gh, p.border);

  // RC15.142: lekka siatka - 25/50/75% w pionie i 1/4, 1/2, 3/4 czasu.
  for (uint8_t gi = 1; gi < 4; ++gi) {
    int yy = gy + (gh * gi) / 4;
    int xx = gx + (gw * gi) / 4;
    for (int sx = gx + 2; sx < gx + gw - 2; sx += 6)
      tft.drawFastHLine(sx, yy, 2, p.panel2);
    for (int sy = gy + 2; sy < gy + gh - 2; sy += 6)
      tft.drawFastVLine(xx, sy, 2, p.panel2);
  }

  if (metric == FOCUS_DIR) {
    const bool use7d = (focusChartRange == 3);

    uint16_t sourceCount = use7d ? windDir7dCount : windDirCount;
    drawFocusChartCoverage(focusChartVisibleTimedCount(metric, use7d, sourceCount));

    if (sourceCount < 2) {
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(p.muted, p.bg);
      tft.drawString("BRAK DANYCH KIERUNKU", 160, 120, 2);
      tft.setTextDatum(TL_DATUM);
    } else {
      const uint16_t wanted = focusChartWantedSamples();
      uint16_t count = sourceCount > wanted ? wanted : sourceCount;
      uint16_t logicalStart = sourceCount - count;

      // Stala skala kolowa: N/E/S/W/N. N wystepuje na obu krawedziach,
      // dlatego przejscie 359 -> 0 nie tworzy falszywej przekatnej.
      const int plotLeft = gx + 24;
      const int plotRight = gx + gw - 4;
      const int plotTop = gy + 3;
      const int plotBottom = gy + gh - 4;
      const int plotH = plotBottom - plotTop;

      const char* cardinal[5] = {"N", "E", "S", "W", "N"};
      for (int ci = 0; ci < 5; ++ci) {
        int yy = plotTop + (plotH * ci) / 4;
        tft.setTextDatum(MR_DATUM);
        tft.setTextColor((ci == 0 || ci == 4) ? p.accent : p.muted, p.bg);
        tft.drawString(cardinal[ci], gx + 19, yy, 1);
        if (ci > 0 && ci < 4)
          tft.drawFastHLine(plotLeft, yy, plotRight - plotLeft + 1, p.panel2);
      }
      tft.setTextDatum(TL_DATUM);

      // RC15.147: statystyka kolowa dla aktualnie widocznego zakresu.
      // meanDir = dominujacy/sredni kierunek, stability = skupienie 0..100%.
      // 100% oznacza praktycznie staly kierunek, wartosc bliska 0% - duza zmiennosc.
      double sumSin = 0.0;
      double sumCos = 0.0;
      uint16_t statCount = 0;

      uint32_t newestDirEpoch = 0;
      for (uint16_t ti = 0; ti < count; ++ti) {
        WindDirSample ts = use7d
                           ? windDir7d[logicalStart + ti]
                           : windDirHistory[windDirIndexOldest(logicalStart + ti)];
        if (ts.epoch > newestDirEpoch)
          newestDirEpoch = ts.epoch;
      }

      uint32_t dirAxisEnd = historyEpochNow();
      if (dirAxisEnd <= 100000UL || dirAxisEnd < newestDirEpoch)
        dirAxisEnd = newestDirEpoch;

      bool havePrev = false;
      int prevX = 0, prevY = 0;
      uint32_t prevDirEpoch = 0;
      float prevDir = NAN;
      int lastX = -1, lastY = -1;
      float lastDir = NAN;

      for (uint16_t i = 0; i < count; ++i) {
        WindDirSample sample;
        if (use7d) {
          sample = windDir7d[logicalStart + i];
        } else {
          int idx = windDirIndexOldest(logicalStart + i);
          sample = windDirHistory[idx];
        }

        float dir = normalizeWindDirDeg(sample.directionDeg);
        if (!isfinite(dir)) {
          havePrev = false;
          continue;
        }

        int dirGw = (plotRight - plotLeft) + 1;
        const bool dirTimeAxisAvailable = newestDirEpoch > 100000UL;
        int x = focusChartTimeXStrict(sample.epoch,
                                     i,
                                     count,
                                     dirTimeAxisAvailable,
                                     dirAxisEnd,
                                     plotLeft - 2,
                                     dirGw + 4);

        if (x == -32768) {
          havePrev = false;
          prevDirEpoch = 0;
          continue;
        }

        // Statystyka kierunku tylko z punktow faktycznie widocznych na osi.
        // W 7D godzina LOW pozostaje na krzywej, ale nie steruje DOM/STAB.
        bool statQualityOK = !use7d || windDir7dSamples[logicalStart + i] >= 6;
        if (statQualityOK) {
          const double rad = (double)dir * 0.017453292519943295;
          sumSin += sin(rad);
          sumCos += cos(rad);
          statCount++;
        }

        int y = plotTop + (int)((dir / 360.0f) * plotH);
        if (y < plotTop) y = plotTop;
        if (y > plotBottom) y = plotBottom;

        uint32_t currDirEpoch = sample.epoch;

        if (havePrev && !focusChartHistoryGap(prevDirEpoch, currDirEpoch, use7d)) {
          float rawDiff = dir - prevDir;
          if (fabsf(rawDiff) <= 180.0f) {
            tft.drawLine(prevX, prevY, x, y, p.accent);
          } else {
            // RC15.148: przejscie przez N (0/360) jest przerwa w linii.
            // Poprzednio kreska dochodzila do dolnej krawedzi i byla
            // kontynuowana od gornej, co wizualnie wygladalo jak wykres
            // wychodzacy poza ramke. N jest opisane na obu krawedziach,
            // wiec dwa punkty po obu stronach jednoznacznie pokazuja
            // rzeczywiste przejscie np. 359 -> 1 bez sztucznej kreski.
            tft.fillCircle(prevX, prevY, 2, p.accent);
            tft.fillCircle(x, y, 2, p.accent);
          }
        }

        prevX = x;
        prevY = y;
        prevDirEpoch = currDirEpoch;
        prevDir = dir;
        havePrev = true;
        lastX = x;
        lastY = y;
        lastDir = dir;
      }

      if (isfinite(lastDir) && lastX >= 0 && lastY >= 0) {
        tft.fillCircle(lastX, lastY, 3, p.accent);
        tft.drawCircle(lastX, lastY, 5, p.text);
      }

      // RC15.147: kompaktowe podsumowanie nad wykresem.
      if (statCount > 0) {
        double meanRad = atan2(sumSin, sumCos);
        double meanDeg = meanRad * 57.29577951308232;
        if (meanDeg < 0.0) meanDeg += 360.0;

        double resultant = sqrt(sumSin * sumSin + sumCos * sumCos) / (double)statCount;
        if (resultant < 0.0) resultant = 0.0;
        if (resultant > 1.0) resultant = 1.0;
        int stability = (int)(resultant * 100.0 + 0.5);

        String statText = "DOM: ";
        statText += windDir((float)meanDeg);
        statText += " ";
        statText += String((float)meanDeg, 0);
        statText += "st   STAB: ";
        statText += String(stability);
        statText += "%";

        // Pole pomiedzy aktualna wartoscia a wykresem; nie nachodzi na przebieg.
        tft.fillRect(12, 50, 296, 12, p.bg);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(p.muted, p.bg);
        tft.drawString(statText, 160, 56, 1);
        tft.setTextDatum(TL_DATUM);
      }
    }
  } else {
    const bool use7d = (focusChartRange == 3);
    const bool indoorMetric = focusChartMetricUsesIndoor(metric);

    // 7D jest juz przygotowane przed rozpoczeciem rysowania ekranu.
    uint16_t sourceCount = use7d
                             ? (indoorMetric ? indoor7dCount : chart7dCount)
                             : chartCount;

    drawFocusChartCoverage(focusChartVisibleTimedCount(metric, use7d, sourceCount));

    if (sourceCount < 2) {
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(p.muted, p.bg);
      tft.drawString("BRAK DANYCH HISTORII", 160, 120, 2);
      tft.setTextDatum(TL_DATUM);
    } else {
      const uint16_t wanted = focusChartWantedSamples();
      uint16_t count = sourceCount;
      if (count > wanted) count = wanted;
      uint16_t logicalStart = sourceCount - count;

      // RC15.168: ustalamy wiarygodna os czasu PRZED statystykami,
      // aby statystyki i rysunek korzystaly z identycznego zbioru probek.
      uint32_t newestEpochPlot = 0;
      for (uint16_t ti = 0; ti < count; ++ti) {
        const ChartSample *tp = nullptr;
        if (use7d) {
          tp = indoorMetric
                 ? &indoor7d[logicalStart + ti]
                 : &chart7d[logicalStart + ti];
        } else {
          int tidx = chartIndexOldest(logicalStart + ti);
          tp = &chartHistory[tidx];
        }
        if (tp->epoch > newestEpochPlot)
          newestEpochPlot = tp->epoch;
      }

      uint32_t axisEndPlot = historyEpochNow();
      if (axisEndPlot <= 100000UL || axisEndPlot < newestEpochPlot)
        axisEndPlot = newestEpochPlot;

      const bool timeAxisAvailable = newestEpochPlot > 100000UL;
      const uint32_t axisWindowSec = focusChartWindowSeconds();
      const uint32_t axisStartPlot =
          (axisEndPlot > axisWindowSec) ? (axisEndPlot - axisWindowSec) : 0;

      float vMin = INFINITY;
      float vMax = -INFINITY;
      uint32_t minEpoch = 0;
      uint32_t maxEpoch = 0;
      double vSum = 0.0;
      uint32_t vWeight = 0;
      float firstV = NAN;
      float lastVStats = NAN;
      // RC15.222: FOCUS TREND GAP GUARD.
      // firstV ma wskazywac poczatek ostatniego ciaglego segmentu,
      // natomiast MIN/MAX/SR nadal obejmuja cale wiarygodne okno.
      uint32_t prevFocusTrendEpoch = 0;
      uint16_t valid = 0;

      for (uint16_t i = 0; i < count; ++i) {
        const ChartSample *sp = nullptr;
        if (use7d) {
          sp = indoorMetric
                 ? &indoor7d[logicalStart + i]
                 : &chart7d[logicalStart + i];
        } else {
          int idx = chartIndexOldest(logicalStart + i);
          sp = &chartHistory[idx];
        }

        float v = focusChartSampleValue(*sp, metric);
        if (!isfinite(v)) continue;

        if (use7d) {
          uint16_t sourceIndex = logicalStart + i;
          uint8_t q = chart7dQualitySamples(indoorMetric, sourceIndex);
          if (q < 6)
            continue;
        }

        uint32_t sampleEpoch = sp->epoch;

        if (timeAxisAvailable) {
          if (sampleEpoch <= 100000UL ||
              sampleEpoch < axisStartPlot ||
              sampleEpoch > axisEndPlot)
            continue;
        }

        // TREND w FOCUS nie moze porownywac wartosci po przeciwnych
        // stronach rzeczywistej luki historii. Progi pozostaja identyczne
        // jak przy rysowaniu FOCUS: 7D=90 min, pozostale zakresy=15 min.
        if (prevFocusTrendEpoch > 100000UL &&
            sampleEpoch > 100000UL &&
            focusChartHistoryGap(prevFocusTrendEpoch, sampleEpoch, use7d)) {
          firstV = v;
        } else if (!isfinite(firstV)) {
          firstV = v;
        }

        if (sampleEpoch > 100000UL)
          prevFocusTrendEpoch = sampleEpoch;

        lastVStats = v;

        if (v < vMin) {
          vMin = v;
          minEpoch = sampleEpoch;
        }
        if (v > vMax) {
          vMax = v;
          maxEpoch = sampleEpoch;
        }

        if (use7d) {
          uint16_t sourceIndex = logicalStart + i;
          uint8_t q = chart7dQualitySamples(indoorMetric, sourceIndex);

          // FOCUS: metryki bazowe srednie wazymy pokryciem.
          bool weightedMetric =
              metric == FOCUS_TEMP || metric == FOCUS_HUM ||
              metric == FOCUS_PRESS || metric == FOCUS_WIND ||
              metric == FOCUS_LUX ||
              metric == FOCUS_TEMP_IN || metric == FOCUS_HUM_IN;

          if (weightedMetric) {
            vSum += (double)v * (double)q;
            vWeight += q;
          } else {
            vSum += v;
            vWeight++;
          }
        } else {
          vSum += v;
          vWeight++;
        }
        valid++;
      }

      if (valid < 2 || !isfinite(vMin) || !isfinite(vMax)) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(p.muted, p.bg);
        drawSafeI18nString("ZA MAŁO DANYCH", 160, 112, 2, p.muted, p.bg, MC_DATUM);
        tft.setTextDatum(TL_DATUM);
      } else {
        // RC15.149: zapamietujemy rzeczywiste statystyki zanim dodamy
        // margines wizualny do skali wykresu.
        const float statMin = vMin;
        const float statMax = vMax;
        const float statAvg = (vWeight > 0) ?
                              (float)(vSum / (double)vWeight) : NAN;

        // Kompaktowy pasek statystyk w bezpiecznej strefie nad wykresem.
        uint8_t statDecimals = 1;
        if (metric == FOCUS_HUM || metric == FOCUS_HUM_IN ||
            metric == FOCUS_PRESS || metric == FOCUS_LUX)
          statDecimals = 0;

        String statsText = "MIN:";
        statsText += String(statMin, (unsigned int)statDecimals);
        statsText += "  MAX:";
        statsText += String(statMax, (unsigned int)statDecimals);
        bool focusPeakHourly =
            use7d && (metric == FOCUS_GUST ||
                      metric == FOCUS_UV ||
                      metric == FOCUS_RAIN);

        statsText += focusPeakHourly ? "  SR MAX:" : "  SR:";
        statsText += String(statAvg, (unsigned int)statDecimals);

        String statsBase = statsText;
        float trendV = NAN;
      // RC15.175: zmiana od poczatku faktycznie widocznego zakresu.
      focusRangeStartDelta(firstV, lastVStats, trendV);

        // RC15.152: martwa strefa zależna od metryki.
        // Dzięki temu drobny szum czujnika nie jest pokazywany jako realny wzrost/spadek.
        float trendDeadband = 0.05f;
        switch (metric) {
          case FOCUS_TEMP:
          case FOCUS_TEMP_IN: trendDeadband = 0.2f; break;
          case FOCUS_HUM:
          case FOCUS_HUM_IN:  trendDeadband = 1.0f; break;
          case FOCUS_PRESS:   trendDeadband = 0.5f; break;
          case FOCUS_WIND:
          case FOCUS_GUST:    trendDeadband = 0.2f; break;
          case FOCUS_UV:      trendDeadband = 0.1f; break;
          case FOCUS_RAIN:    trendDeadband = 0.1f; break;
          case FOCUS_LUX:     trendDeadband = 50.0f; break;
          default: break;
        }

        String trendText = "  TR:";
        String trendNoPct = trendText;
        if (isfinite(trendV)) {
          // RC15.163: sila trendu liczona wzgledem martwej strefy danej metryki.
          // <= deadband      : stabilnie
          // > deadband       : lagodny trend
          // > 3 * deadband   : szybki trend
          float absTrend = fabsf(trendV);
          bool fastTrend = absTrend > (trendDeadband * 3.0f);

          if (trendV > trendDeadband) {
            trendText += fastTrend ? "^^+" : "^+";
          } else if (trendV < -trendDeadband) {
            trendText += fastTrend ? "vv" : "v";
          } else {
            trendText += "=";
          }

          trendText += String(trendV, (unsigned int)statDecimals);
          trendNoPct = trendText;

          // RC15.162: dodatkowy procent zmiany tylko tam, gdzie jest czytelny
          // i matematycznie sensowny. Dla temperatury, kierunku, UV i opadu
          // zostawiamy sama zmiane bezwzgledna.
          bool showTrendPct =
              metric == FOCUS_PRESS ||
              metric == FOCUS_WIND ||
              metric == FOCUS_GUST ||
              metric == FOCUS_LUX;

          if (showTrendPct && isfinite(firstV) && fabsf(firstV) > 0.001f) {
            float trendPct = (trendV / fabsf(firstV)) * 100.0f;

            // Chronimy pasek przed absurdalnie dlugim tekstem przy bardzo malej bazie.
            if (isfinite(trendPct) && fabsf(trendPct) < 1000.0f) {
              trendText += "(";
              if (trendPct > 0.0f) trendText += "+";
              trendText += String(trendPct, 0);
              trendText += "%)";
            }
          }
        } else {
          trendText += "--";
          trendNoPct = trendText;
        }

        // RC15.162A: dobieramy zapis do rzeczywistej szerokosci 296 px.
        statsText = statsBase + trendText;

        if (tft.textWidth(statsText, 1) > 292) {
          statsText = statsBase + trendNoPct;
        }

        if (tft.textWidth(statsText, 1) > 292) {
          statsText.replace("  ", " ");
        }

        // Ostateczny awaryjny wariant: przy ekstremalnie dlugich liczbach
        // zachowujemy MIN/MAX/TR, pomijajac SR zamiast pozwolic tekstowi wyjsc poza pole.
        if (tft.textWidth(statsText, 1) > 292) {
          String safeText = "MIN:";
          safeText += String(statMin, (unsigned int)statDecimals);
          safeText += " MAX:";
          safeText += String(statMax, (unsigned int)statDecimals);
          safeText += trendNoPct;
          safeText.replace("  ", " ");
          statsText = safeText;
        }

        tft.fillRect(12, 50, 296, 12, p.bg);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(p.muted, p.bg);
        tft.drawString(statsText, 160, 56, 1);
        tft.setTextDatum(TL_DATUM);

        // RC15.153: czas MIN/MAX tylko gdy epoch jest poprawny.
        String minTime = focusChartTimeShort(minEpoch);
        String maxTime = focusChartTimeShort(maxEpoch);

        // RC15.155: czasy ekstremow sa niezalezne.
        // W starszej historii czesc rekordow mogla powstac przed NTP i miec epoch=0.
        // Nie chowamy wtedy calego wiersza - pokazujemy --:-- tylko dla brakujacego czasu.
        if (!minTime.length()) minTime = "--:--";
        if (!maxTime.length()) maxTime = "--:--";

        String timeText = "MIN@";
        timeText += minTime;
        timeText += "  MAX@";
        timeText += maxTime;

        // 7D ma dluzszy format DD.MM HH:MM, dlatego korzystamy z calej szerokosci.
        tft.fillRect(12, 63, 296, 12, p.bg);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(p.muted, p.bg);
        tft.drawString(timeText, 160, 69, 1);
        tft.setTextDatum(TL_DATUM);

        float span = vMax - vMin;
        if (span < 0.001f) {
          float pad = fabsf(vMin) * 0.03f;
          if (pad < 0.5f) pad = 0.5f;
          vMin -= pad;
          vMax += pad;
          span = vMax - vMin;
        } else {
          float pad = span * 0.08f;
          vMin -= pad;
          vMax += pad;
          span = vMax - vMin;
        }

        // RC15.173: granice strefy OK sa warstwa odniesienia pod krzywa.
        drawFocusOkZoneGuides(metric, vMin, vMax, gx, gy, gw, gh, p.bg);

        bool havePrev = false;
        int px = 0, py = 0;
        uint32_t prevEpochPlot = 0;
        float lastV = NAN;
        int lastX = -1, lastY = -1;

        // RC15.157: pozycje ekstremow na wykresie.
        int minX = -1, minY = -1;
        int maxX = -1, maxY = -1;
        float bestMinDist = INFINITY;
        float bestMaxDist = INFINITY;

        for (uint16_t i = 0; i < count; ++i) {
          const ChartSample *sp = nullptr;
          if (use7d) {
            sp = indoorMetric
                   ? &indoor7d[logicalStart + i]
                   : &chart7d[logicalStart + i];
          } else {
            int idx = chartIndexOldest(logicalStart + i);
            sp = &chartHistory[idx];
          }

          float v = focusChartSampleValue(*sp, metric);

          if (!isfinite(v)) {
            havePrev = false;
            continue;
          }

          int x = focusChartTimeXStrict(sp->epoch,
                                       i,
                                       count,
                                       timeAxisAvailable,
                                       axisEndPlot,
                                       gx,
                                       gw);

          if (x == -32768) {
            havePrev = false;
            prevEpochPlot = 0;
            continue;
          }

          float norm = (v - vMin) / span;
          if (norm < 0.0f) norm = 0.0f;
          if (norm > 1.0f) norm = 1.0f;

          int y = gy + gh - 3 - (int)(norm * (gh - 6));

          // RC15.211:
          // Gdy czas ekstremum jest znany, marker musi trafic dokladnie
          // w ten sam rekord co MIN@ / MAX@. Nie wybieramy juz punktu LOW
          // tylko dlatego, ze ma podobna wartosc.
          uint32_t currEpochPlot = sp->epoch;

          if (minEpoch > 100000UL) {
            if (currEpochPlot == minEpoch) {
              minX = x;
              minY = y;
              bestMinDist = 0.0f;
            }
          } else {
            float dMin = fabsf(v - statMin);
            if (dMin < bestMinDist) {
              bestMinDist = dMin;
              minX = x;
              minY = y;
            }
          }

          if (maxEpoch > 100000UL) {
            if (currEpochPlot == maxEpoch) {
              maxX = x;
              maxY = y;
              bestMaxDist = 0.0f;
            }
          } else {
            float dMax = fabsf(v - statMax);
            if (dMax < bestMaxDist) {
              bestMaxDist = dMax;
              maxX = x;
              maxY = y;
            }
          }

          if (havePrev && !focusChartHistoryGap(prevEpochPlot, currEpochPlot, use7d))
            tft.drawLine(px, py, x, y, p.accent);

          px = x;
          py = y;
          prevEpochPlot = currEpochPlot;
          havePrev = true;
          lastV = v;
          lastX = x;
          lastY = y;
        }

      // RC15.158: graficzne znaczniki MIN/MAX z bezpiecznym marginesem.
      // MIN = trojkat skierowany w dol, MAX = trojkat skierowany w gore.
      // Przy praktycznie plaskim przebiegu MIN i MAX sa tym samym zjawiskiem,
      // wiec nie nakladamy dwoch znacznikow na siebie.
      // Jesli poprawny epoch ekstremum nie znalazl sie w rysowanym oknie,
      // lepiej pominac znacznik niz pokazac go przy innym punkcie.
      if (minEpoch > 100000UL && bestMinDist != 0.0f) {
        minX = -1;
        minY = -1;
      }
      if (maxEpoch > 100000UL && bestMaxDist != 0.0f) {
        maxX = -1;
        maxY = -1;
      }

      const float flatEps = 0.0001f;
      const bool distinctExtremes =
          isfinite(statMin) && isfinite(statMax) &&
          fabsf(statMax - statMin) > flatEps;

      if (distinctExtremes && minX >= 0 && minY >= 0) {
        int xx = minX;
        int yy = minY;

        // Wiekszy margines, bo przy 6 px trojkat nadal optycznie
        // zlewal sie z ramka, szczegolnie w lewym dolnym rogu.
        if (xx < gx + 10) xx = gx + 10;
        if (xx > gx + gw - 11) xx = gx + gw - 11;
        if (yy < gy + 10) yy = gy + 10;
        if (yy > gy + gh - 11) yy = gy + gh - 11;

        // Cienka obwodka w kolorze tla/tekstu poprawia czytelnosc na linii wykresu.
        tft.fillTriangle(xx - 5, yy - 4, xx + 5, yy - 4, xx, yy + 5, p.bg);
        tft.fillTriangle(xx - 4, yy - 3, xx + 4, yy - 3, xx, yy + 4, p.text);
      }

      if (distinctExtremes && maxX >= 0 && maxY >= 0) {
        int xx = maxX;
        int yy = maxY;

        if (xx < gx + 10) xx = gx + 10;
        if (xx > gx + gw - 11) xx = gx + gw - 11;
        if (yy < gy + 10) yy = gy + 10;
        if (yy > gy + gh - 11) yy = gy + gh - 11;

        tft.fillTriangle(xx - 5, yy + 4, xx + 5, yy + 4, xx, yy - 5, p.bg);
        tft.fillTriangle(xx - 4, yy + 3, xx + 4, yy + 3, xx, yy - 4, p.accent);
      }

      // RC15.142: aktualny punkt jest jednoznacznie zaznaczony.
      if (isfinite(lastV) && lastX >= 0 && lastY >= 0) {
        tft.fillCircle(lastX, lastY, 3, p.accent);
        tft.drawCircle(lastX, lastY, 5, p.text);
      }

      // RC15.176: prawa strzalka reprezentuje BIEZACY odczyt z pakietu.
      // Kropka/pierścień na krzywej nadal oznacza ostatnia probke historii.
      float liveV = focusChartCurrentValue(metric, d);
      if (isfinite(liveV) &&
          isfinite(statMin) &&
          isfinite(statMax) &&
          statMax > statMin + 0.0001f) {
        float pos = (liveV - statMin) / (statMax - statMin);
        if (pos < 0.0f) pos = 0.0f;
        if (pos > 1.0f) pos = 1.0f;

        int markerY = gy + gh - 6 - (int)(pos * (gh - 12));
        if (markerY < gy + 6) markerY = gy + 6;
        if (markerY > gy + gh - 6) markerY = gy + gh - 6;

        const int markerX = gx + gw + 4;
        const uint16_t markerColor =
            focusChartCurrentStateColor(metric, liveV, p.accent);

        // Mala strzalka skierowana w strone wykresu.
        // RC15.174: kolor od razu pokazuje stan wzgledem strefy OK.
        tft.fillTriangle(markerX, markerY,
                         markerX + 5, markerY - 4,
                         markerX + 5, markerY + 4,
                         markerColor);
      }

      // RC15.160:
      // Nie rysujemy juz liczbowych etykiet vMin/vMax wewnatrz pola wykresu.
      // Te same informacje sa czytelniej pokazane w pasku MIN/MAX nad wykresem,
      // a wewnetrzne etykiety mogly zaslaniac markery ekstremow przy krawedziach.

        // RC15.144:
        // Nie rysujemy drugiej, malej kopii aktualnej wartosci.
        // Biezacy wynik jest juz czytelnie pokazany nad wykresem,
        // a ostatni punkt pozostaje oznaczony kropka/pierścieniem.
      }
    }
  }

  // Oznaczenia czasu.
  tft.setTextColor(p.muted, p.bg);
  tft.drawString(focusChartLeftLabel(), gx, 184, 1);

  String midLabel = "-3H";
  if (focusChartRange == 0) midLabel = "-30M";
  else if (focusChartRange == 2) midLabel = "-12H";
  else if (focusChartRange == 3) midLabel = "-3.5D";
  tft.drawCentreString(midLabel, gx + gw / 2, 184, 1);

  tft.drawRightString(chartNowText(), gx + gw, 184, 1);

  // RC15.145: 1H / 6H / 24H / 7D / POWROT.
  // Wszystko builtin font, brak smooth .vlw.
  tft.fillRoundRect(8,   202, 48, 34, 7, p.panel2);
  tft.drawRoundRect(8,   202, 48, 34, 7, p.border);

  tft.fillRoundRect(60,  202, 48, 34, 7, p.panel2);
  tft.drawRoundRect(60,  202, 48, 34, 7, p.border);

  tft.fillRoundRect(112, 202, 52, 34, 7, p.panel2);
  tft.drawRoundRect(112, 202, 52, 34, 7, p.border);

  tft.fillRoundRect(168, 202, 48, 34, 7, p.panel2);
  tft.drawRoundRect(168, 202, 48, 34, 7, p.border);

  tft.fillRoundRect(220, 202, 92, 34, 7, p.accent);

  if (focusChartRange == 0) tft.fillRect(14, 205, 36, 3, p.accent);
  if (focusChartRange == 1) tft.fillRect(66, 205, 36, 3, p.accent);
  if (focusChartRange == 2) tft.fillRect(118, 205, 40, 3, p.accent);
  if (focusChartRange == 3) tft.fillRect(174, 205, 36, 3, p.accent);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(p.accent, p.panel2);
  tft.drawString("1H",  32, 219, 2);
  tft.drawString("6H",  84, 219, 2);
  tft.drawString("24H", 138, 219, 2);
  tft.drawString("7D",  192, 219, 2);

  tft.setTextColor(TFT_BLACK, p.accent);
  if (currentLanguage == LANG_PL)
    drawSafeI18nString("POWRÓT", 266, 211, 2, TFT_BLACK, p.accent, MC_DATUM);
  else
    drawLangButtonCentered(tr(TXT_BACK), tr(TXT_BACK),
                           266, 219, TFT_BLACK, p.accent, false);
  tft.setTextDatum(TL_DATUM);
}

void openFocusBigChart() {
  focusChartMetric = (lastDrawnFocusMetric < FOCUS_COUNT)
                       ? lastDrawnFocusMetric
                       : currentFocusMetric();

  focusChartOpen = true;
  menuOpen = true;  // blokuje wszystkie istniejace background redrawy
  waitTouchRelease = true;
  redrawAfterTouchRelease = false;
  drawFocusBigChart(focusChartMetric);
  touchActionBlockUntilMs = millis() + 80UL;
}

void handleFocusBigChartTouch(int x, int y) {
  // RC15.145: geometria odpowiada 5 widocznym przyciskom.
  if (inside(x, y, 4, 198, 56, 42)) {
    focusChartRange = 0;
    drawFocusBigChart(focusChartMetric);
    waitTouchRelease = true;
    touchActionBlockUntilMs = millis() + 80UL;
    return;
  }

  if (inside(x, y, 56, 198, 56, 42)) {
    focusChartRange = 1;
    drawFocusBigChart(focusChartMetric);
    waitTouchRelease = true;
    touchActionBlockUntilMs = millis() + 80UL;
    return;
  }

  if (inside(x, y, 108, 198, 60, 42)) {
    focusChartRange = 2;
    drawFocusBigChart(focusChartMetric);
    waitTouchRelease = true;
    touchActionBlockUntilMs = millis() + 80UL;
    return;
  }

  if (inside(x, y, 164, 198, 56, 42)) {
    focusChartRange = 3;
    drawFocusBigChart(focusChartMetric);
    waitTouchRelease = true;
    touchActionBlockUntilMs = millis() + 80UL;
    return;
  }

  if (inside(x, y, 216, 198, 100, 42)) {
    focusChartOpen = false;
    menuOpen = false;
    waitTouchRelease = true;
    redrawAfterTouchRelease = true;
    forceScreenClear = true;
    focusProgressSyncPending = true;
    touchActionBlockUntilMs = millis() + 80UL;
    return;
  }
}

// ============================================================
// SKIN 7 - CHARTS / WYKRESY
// ============================================================

String chartsTitle() {
  if (currentLanguage == LANG_EN) return "WEATHER CHARTS";
  if (currentLanguage == LANG_DE) return "WETTERKURVEN";
  if (currentLanguage == LANG_CZ) return "METEO GRAFY";
  return "POGODA - WYKRESY";
}

float chartSampleValue(const ChartSample &s, uint8_t metric) {
  switch (metric) {
    case 0: return s.temp;
    case 1: return s.hum;
    case 2: return s.press;
    case 3: return s.wind;
    case 4: return s.gust;
    case 5: return s.uv;
    case 6: return s.rain;
    case 7: return s.lux;
    case 8: return s.tempIn;
    case 9: return s.humIn;
    case 10: return dewPointC(s.temp, s.hum);
    case 11: return dewPointC(s.tempIn, s.humIn);
    case 12: return absoluteHumidityGM3(s.temp, s.hum);
    default: return absoluteHumidityGM3(s.tempIn, s.humIn);
  }
}

void drawMiniChart(int x, int y, int w, int h,
                   uint8_t metric,
                   const String &label,
                   const String &unit,
                   uint16_t lineColor) {
  SkinPalette p = paletteFor(currentSkin);

  tft.fillRoundRect(x, y, w, h, 5, p.panel);
  tft.drawRoundRect(x, y, w, h, 5, p.border);

  if (!deferChartLabels) {
    tft.setTextColor(p.muted, p.panel);
    tft.drawString(label, x + 5, y + 3, 1);
  }

  bool use7d = (chartRange == CHART_RANGE_7D);
  bool indoorMetric = chartMetricUsesIndoor(metric);

  if (use7d && indoorMetric && !indoor7dLoaded)
    loadIndoor7DFromSD();
  else if (use7d && !indoorMetric && !chart7dLoaded)
    loadChart7DFromSD();

  uint16_t sourceCount;
  if (use7d)
    sourceCount = indoorMetric ? indoor7dCount : chart7dCount;
  else
    sourceCount = chartCount;

  if (sourceCount == 0) {
    tft.setTextColor(p.muted, p.panel);
    tft.drawCentreString("--", x + w / 2, y + h / 2 - 4, 2);
    return;
  }

  uint16_t visibleCount = sourceCount;
  uint16_t wanted = chartRangeSamples();
  if (visibleCount > wanted)
    visibleCount = wanted;

  uint16_t logicalStart = sourceCount - visibleCount;

  // RC15.168: miniwykresy korzystaja z tej samej prawdziwej osi czasu.
  uint32_t newestMiniEpoch = 0;
  for (uint16_t ti = 0; ti < visibleCount; ++ti) {
    const ChartSample *tp = nullptr;
    if (use7d) {
      tp = indoorMetric
             ? &indoor7d[logicalStart + ti]
             : &chart7d[logicalStart + ti];
    } else {
      int tidx = chartIndexOldest(logicalStart + ti);
      tp = &chartHistory[tidx];
    }
    if (tp->epoch > newestMiniEpoch)
      newestMiniEpoch = tp->epoch;
  }

  uint32_t miniAxisEnd = historyEpochNow();
  if (miniAxisEnd <= 100000UL || miniAxisEnd < newestMiniEpoch)
    miniAxisEnd = newestMiniEpoch;

  const bool miniTimeAxisAvailable = newestMiniEpoch > 100000UL;
  const uint32_t miniWindowSec = miniChartWindowSeconds();
  const uint32_t miniAxisStart =
      (miniAxisEnd > miniWindowSec) ? (miniAxisEnd - miniWindowSec) : 0;

  float minV = INFINITY;
  float maxV = -INFINITY;
  float firstV = NAN;
  float lastV = NAN;

  for (uint16_t i = 0; i < visibleCount; i++) {
    const ChartSample *sp = nullptr;
    if (use7d) {
      sp = indoorMetric
             ? &indoor7d[logicalStart + i]
             : &chart7d[logicalStart + i];
    } else {
      int idx = chartIndexOldest(logicalStart + i);
      sp = &chartHistory[idx];
    }

    if (miniTimeAxisAvailable) {
      if (sp->epoch <= 100000UL ||
          sp->epoch < miniAxisStart ||
          sp->epoch > miniAxisEnd)
        continue;
    }

    float v = chartSampleValue(*sp, metric);
    if (!isfinite(v)) continue;

    // RC15.212: krzywa 7D moze nadal pokazywac LOW, ale statystyki
    // naglowka miniwykresu musza byc zgodne z TREND/STAT.
    if (use7d) {
      uint16_t sourceIndex = logicalStart + i;
      uint8_t q = chart7dQualitySamples(indoorMetric, sourceIndex);
      if (q < 6)
        continue;
    }

    if (!isfinite(firstV)) firstV = v;
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
    lastV = v;
  }

  if (!isfinite(minV) || !isfinite(maxV)) {
    tft.setTextColor(p.muted, p.panel);
    tft.drawCentreString("--", x + w / 2, y + h / 2 - 4, 2);
    return;
  }

  // Minimalny zakres, aby linia nie "skakala" przy prawie stalych danych.
  float span = maxV - minV;
  float minSpan = 1.0f;
  if (metric == 1 || metric == 9) minSpan = 5.0f; // humidity out/in
  else if (metric == 2) minSpan = 4.0f;            // pressure
  else if (metric == 3) minSpan = 1.5f;            // wind

  if (span < minSpan) {
    float mid = (minV + maxV) * 0.5f;
    minV = mid - minSpan * 0.5f;
    maxV = mid + minSpan * 0.5f;
    span = minSpan;
  }

  int gx = x + 5;
  int gy = y + 14;
  int gw = w - 10;

  // Zostawiamy dolny pasek 9 px na opisy czasu.
  int gh = h - 28;

  // Subtelne poziome linie odniesienia.
  tft.drawFastHLine(gx, gy + gh / 2, gw, p.border);

  int prevX = -1, prevY = -1;
  uint32_t prevMiniEpoch = 0;
  int lastPixelX = -1;

  for (uint16_t i = 0; i < visibleCount; i++) {
    const ChartSample *sp = nullptr;
    if (use7d) {
      sp = indoorMetric
             ? &indoor7d[logicalStart + i]
             : &chart7d[logicalStart + i];
    } else {
      int idx = chartIndexOldest(logicalStart + i);
      sp = &chartHistory[idx];
    }

    float v = chartSampleValue(*sp, metric);
    if (!isfinite(v)) {
      prevX = -1;
      prevMiniEpoch = 0;
      continue;
    }

    int px = chartTimeXStrictRange(sp->epoch,
                                   i,
                                   visibleCount,
                                   miniTimeAxisAvailable,
                                   miniAxisEnd,
                                   miniWindowSec,
                                   gx - 2,
                                   gw + 4);

    if (px == -32768) {
      prevX = -1;
      prevMiniEpoch = 0;
      continue;
    }

    // Przy 24H mozemy miec wiecej probek niz pikseli w poziomie.
    // Dla jednego x zachowujemy najnowsza probke.
    if (px == lastPixelX && i < visibleCount - 1)
      continue;

    float norm = (v - minV) / (maxV - minV);
    if (norm < 0) norm = 0;
    if (norm > 1) norm = 1;
    int py = gy + gh - 1 - (int)(norm * (gh - 1));

    if (prevX >= 0 &&
        !focusChartHistoryGap(prevMiniEpoch, sp->epoch, use7d))
      tft.drawLine(prevX, prevY, px, py, lineColor);

    prevX = px;
    prevY = py;
    prevMiniEpoch = sp->epoch;
    lastPixelX = px;
  }

  // Przy jednej próbce nie ma odcinka do narysowania.
  // Pokazujemy więc punkt, żeby nowa historia (np. TEMP WEW/WILG WEW)
  // była widoczna od pierwszego zapisu.
  if (visibleCount == 1 && prevX >= 0)
    tft.fillCircle(prevX, prevY, 2, lineColor);

  // Ostatnia wartosc po prawej.
  String lastTxt = isfinite(lastV) ?
                   String(lastV, (metric == 1 || metric == 9 || metric == 2) ? 0 : 1) :
                   "--";
  if (unit.length()) lastTxt += unit;

  tft.setTextColor(lineColor, p.panel);
  tft.setTextFont(1);

  int lastW = tft.textWidth(lastTxt, 1);
  tft.drawRightString(lastTxt, x + w - 5, y + 3, 1);

  // Trend od pierwszej do ostatniej widocznej probki.
  // Gdy gorny wiersz jest ciasny, znak trendu pomijamy zamiast
  // nakladac go na etykiete albo ostatnia wartosc.
  String trend = chartTrendSymbol(firstV, lastV, metric);
  if (trend.length()) {
    const int labelReserveW = 86; // tyle maksymalnie rezerwuje batch etykiet
    int trendW = tft.textWidth(trend, 1);
    int trendX = x + 5 + labelReserveW + 3;
    int lastLeft = x + w - 5 - lastW;

    if (trendX + trendW + 3 < lastLeft) {
      tft.setTextColor(lineColor, p.panel);
      tft.drawString(trend, trendX, y + 3, 1);
    }
  }

  // MIN/MAX dla aktualnie widocznego zakresu.
  // Celowo bardzo mala czcionka, aby nie zabierac miejsca wykresowi.
  String minTxt = "L:";
  String maxTxt = "H:";

  uint8_t mmDec = (metric == 1 || metric == 9 ||
                   metric == 2 || metric == 5) ? 0 : 1;
  minTxt += String(minV, (unsigned int)mmDec);
  maxTxt += String(maxV, (unsigned int)mmDec);

  tft.setTextColor(p.muted, p.panel);
  tft.drawString(minTxt, x + 5, y + h - 10, 1);
  tft.drawRightString(maxTxt, x + w - 5, y + h - 10, 1);

  // RC15.168: etykiety odpowiadaja stalej osi czasu, nie liczbie probek.
  String leftTime = chartRangeLeftLabel();
  String midTime = miniChartMidFixedLabel();

  tft.drawString(leftTime, gx, y + h - 19, 1);

  if (midTime.length()) {
    tft.setTextDatum(MC_DATUM);
    tft.drawString(midTime, gx + gw / 2, y + h - 16, 1);
    tft.setTextDatum(TL_DATUM);
  }

  tft.drawRightString(chartNowText(), gx + gw, y + h - 19, 1);
}



static inline uint8_t chart7dQualitySamples(bool indoorMetric, uint16_t index) {
  if (index >= CHART_7D_POINTS)
    return 0;
  return indoorMetric ? indoor7dSamples[index] : chart7dSamples[index];
}

static inline bool chart7dMetricIsHourlyPeak(uint8_t metric) {
  // W 7D te pola sa maksimum z godzinnego koszyka, nie srednia probek.
  return metric == 4 || metric == 5 || metric == 6; // gust, UV, rain
}

static inline bool chart7dMetricUsesSampleWeightedAverage(uint8_t metric) {
  // Temp, wilgotnosc, cisnienie, sredni wiatr, lux i odpowiedniki WEW.
  // Poryw/UV/opad sa godzinowymi maksimami, wiec ich AVG pozostaje srednia
  // z wiarygodnych godzin, bez mnozenia maksimum przez liczbe probek.
  return metric == 0 || metric == 1 || metric == 2 ||
         metric == 3 || metric == 7 ||
         metric == 8 || metric == 9 ||
         metric == 10 || metric == 11 ||
         metric == 12 || metric == 13;
}

bool chartMetricStats(uint8_t metric,
                      float &firstV,
                      float &lastV,
                      float &avgV,
                      float &minV,
                      float &maxV,
                      uint16_t &countV) {
  firstV = NAN;
  lastV = NAN;
  avgV = NAN;
  minV = INFINITY;
  maxV = -INFINITY;
  countV = 0;

  bool use7d = (chartRange == CHART_RANGE_7D);
  bool indoorMetric = chartMetricUsesIndoor(metric);

  if (use7d) {
    if (indoorMetric && !indoor7dLoaded)
      loadIndoor7DFromSD();
    else if (!indoorMetric && !chart7dLoaded)
      loadChart7DFromSD();
  }

  uint16_t sourceCount = use7d ?
                         (indoorMetric ? indoor7dCount : chart7dCount) :
                         chartCount;
  if (sourceCount == 0)
    return false;

  uint16_t wanted = chartRangeSamples();
  uint16_t visibleCount = sourceCount;
  if (visibleCount > wanted)
    visibleCount = wanted;

  uint16_t logicalStart = sourceCount - visibleCount;

  // RC15.169: STAT/TREND korzystaja z tego samego okna czasu co miniwykres.
  uint32_t newestStatsEpoch = 0;
  for (uint16_t ti = 0; ti < visibleCount; ++ti) {
    const ChartSample *sp = nullptr;

    if (use7d) {
      sp = indoorMetric
             ? &indoor7d[logicalStart + ti]
             : &chart7d[logicalStart + ti];
    } else {
      int idx = chartIndexOldest(logicalStart + ti);
      sp = &chartHistory[idx];
    }

    if (sp->epoch > newestStatsEpoch)
      newestStatsEpoch = sp->epoch;
  }

  uint32_t statsAxisEnd = historyEpochNow();
  if (statsAxisEnd <= 100000UL || statsAxisEnd < newestStatsEpoch)
    statsAxisEnd = newestStatsEpoch;

  const bool statsTimeAxisAvailable = newestStatsEpoch > 100000UL;
  const uint32_t statsWindowSec = miniChartWindowSeconds();
  const uint32_t statsAxisStart =
      (statsAxisEnd > statsWindowSec) ? (statsAxisEnd - statsWindowSec) : 0;

  double sum = 0.0;
  uint32_t avgWeight = 0;

  // RC15.221: TREND GAP GUARD.
  // MIN/MAX/AVG nadal obejmuja cale wiarygodne okno, ale firstV dla TREND
  // zaczyna sie od nowa po rzeczywistej luce czasu. Dzieki temu delta TREND
  // nie porownuje wartosci znajdujacych sie po przeciwnych stronach dziury.
  uint32_t prevTrendEpoch = 0;

  for (uint16_t i = 0; i < visibleCount; i++) {
    const ChartSample *sp = nullptr;

    if (use7d) {
      sp = indoorMetric
             ? &indoor7d[logicalStart + i]
             : &chart7d[logicalStart + i];
    } else {
      int idx = chartIndexOldest(logicalStart + i);
      sp = &chartHistory[idx];
    }

    if (use7d) {
      uint16_t sourceIndex = logicalStart + i;
      uint8_t q = chart7dQualitySamples(indoorMetric, sourceIndex);

      // LOW nadal istnieje na wykresie, ale nie moze sterowac
      // MIN/MAX/SR/TR jak pelna godzina.
      if (q < 6)
        continue;
    }

    if (statsTimeAxisAvailable) {
      if (sp->epoch <= 100000UL ||
          sp->epoch < statsAxisStart ||
          sp->epoch > statsAxisEnd)
        continue;
    }

    float v = chartSampleValue(*sp, metric);
    if (!isfinite(v))
      continue;

    // Dla TREND zachowujemy tylko poczatek ostatniego ciaglego segmentu.
    // alertSampleGap() ma juz progi zgodne z rozdzielczoscia: 7D=90 min,
    // pozostale zakresy=15 min. Brak prawidlowego EPOCH nie tworzy sztucznej luki.
    if (prevTrendEpoch > 100000UL &&
        sp->epoch > 100000UL &&
        alertSampleGap(prevTrendEpoch, sp->epoch)) {
      firstV = v;
    } else if (!isfinite(firstV)) {
      firstV = v;
    }

    if (sp->epoch > 100000UL)
      prevTrendEpoch = sp->epoch;

    lastV = v;
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
    if (use7d && chart7dMetricUsesSampleWeightedAverage(metric)) {
      uint16_t sourceIndex = logicalStart + i;
      uint8_t q = chart7dQualitySamples(indoorMetric, sourceIndex);
      sum += (double)v * (double)q;
      avgWeight += q;
    } else {
      sum += v;
      avgWeight++;
    }
    countV++;
  }

  if (countV == 0)
    return false;

  if (avgWeight == 0)
    return false;

  avgV = (float)(sum / (double)avgWeight);
  return true;
}

void drawTrendCard(int x, int y, int w, int h,
                   const String &label,
                   uint8_t metric,
                   const String &unit) {
  SkinPalette p = paletteFor(currentSkin);

  tft.fillRoundRect(x, y, w, h, 5, p.panel);
  tft.drawRoundRect(x, y, w, h, 5, p.border);

  if (!deferChartLabels) {
    tft.setTextColor(p.muted, p.panel);
    if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ)
      drawSafeI18nString(label, x + 6, y + 5, 1, p.muted, p.panel, TL_DATUM);
    else
      tft.drawString(label, x + 6, y + 5, 1);
  }

  float firstV, lastV, avgV, minV, maxV;
  uint16_t countV;

  if (!chartMetricStats(metric, firstV, lastV, avgV, minV, maxV, countV)) {
    tft.setTextColor(p.muted, p.panel);
    tft.drawCentreString("--", x + w / 2, y + 29, 2);
    return;
  }

  float delta = lastV - firstV;
  uint8_t dec = (metric == 1 || metric == 2) ? 0 : 1;

  String d = fmtSigned(delta, dec);
  if (unit.length()) d += unit;

  String avg = (chartRange == CHART_RANGE_7D &&
                chart7dMetricIsHourlyPeak(metric)) ? "AVG MAX " : "AVG ";
  avg += String(avgV, (unsigned int)dec);
  if (unit.length()) avg += unit;

  String lohi = "L:";
  lohi += String(minV, (unsigned int)dec);
  lohi += " H:";
  lohi += String(maxV, (unsigned int)dec);

  String tr = chartTrendSymbol(firstV, lastV, metric);

  uint16_t c = p.accent;
  if (tr == "+") c = toneColor(TONE_OK);
  else if (tr == "-") c = toneColor(TONE_WARNING);
  else c = p.muted;

  // Pokazujemy tylko podpisana delte. Kierunek pozostaje zakodowany kolorem,
  // wiec nie ma juz mylacego "++" albo "--".
  tft.setTextColor(c, p.panel);
  tft.setTextDatum(MC_DATUM);
  uint8_t deltaFont = bestFont(d, w - 14, 2);
  tft.drawString(d, x + w / 2, y + 29, deltaFont);

  tft.setTextColor(p.muted, p.panel);
  tft.setTextFont(1);

  if (tft.textWidth(avg, 1) > w - 12) {
    avg = (chartRange == CHART_RANGE_7D &&
           chart7dMetricIsHourlyPeak(metric)) ? "AM:" : "A:";
    avg += String(avgV, (unsigned int)dec);
    if (unit.length()) avg += unit;
  }

  if (tft.textWidth(lohi, 1) > w - 12) {
    lohi = String(minV, (unsigned int)dec);
    lohi += "/";
    lohi += String(maxV, (unsigned int)dec);
  }

  tft.drawString(avg, x + w / 2, y + 48);
  tft.drawString(lohi, x + w / 2, y + 59);

  if (metric == 2) {
    // RC15.186: CISN dostaje staly trend 3H niezalezny od RANGE.
    float p3h = NAN;
    uint16_t p3hN = 0;

    if (pressureTendency3H(p3h, p3hN)) {
      String pTxt = pressureTendency3HText(p3h);
      tft.setTextColor(pressureTendency3HColor(p3h, p), p.panel);

      if (tft.textWidth(pTxt, 1) > w - 12) {
        pTxt = "3H ";
        pTxt += fmtSigned(p3h, 1);
      }

      tft.drawString(pTxt, x + w / 2, y + 69);
    } else {
      tft.setTextColor(p.muted, p.panel);
      tft.drawString("3H --", x + w / 2, y + 69);
    }
  } else {
    String n = "N=" + String(countV);
    tft.drawString(n, x + w / 2, y + 69);
  }

  tft.setTextDatum(TL_DATUM);
}

void drawStatsCard(int x, int y, int w, int h,
                   const String &label,
                   uint8_t metric,
                   const String &unit) {
  SkinPalette p = paletteFor(currentSkin);

  tft.fillRoundRect(x, y, w, h, 5, p.panel);
  tft.drawRoundRect(x, y, w, h, 5, p.border);

  if (!deferChartLabels) {
    tft.setTextColor(p.accent, p.panel);
    if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ)
      drawSafeI18nString(label, x + 6, y + 5, 1, p.accent, p.panel, TL_DATUM);
    else
      tft.drawString(label, x + 6, y + 5, 1);
  }

  float firstV, lastV, avgV, minV, maxV;
  uint16_t countV;

  if (!chartMetricStats(metric, firstV, lastV, avgV, minV, maxV, countV)) {
    tft.setTextColor(p.muted, p.panel);
    tft.drawCentreString("--", x + w / 2, y + 28, 2);
    return;
  }

  uint8_t dec = (metric == 1 || metric == 2) ? 0 : 1;

  String avg = (chartRange == CHART_RANGE_7D &&
                chart7dMetricIsHourlyPeak(metric)) ? "AVG MAX " : "AVG ";
  avg += String(avgV, (unsigned int)dec);
  if (unit.length()) avg += unit;

  String lo = "MIN ";
  lo += String(minV, (unsigned int)dec);
  if (unit.length()) lo += unit;

  String hi = "MAX ";
  hi += String(maxV, (unsigned int)dec);
  if (unit.length()) hi += unit;

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(p.text, p.panel);
  uint8_t avgFont = bestFont(avg, w - 14, 2);
  tft.setTextFont(avgFont);
  tft.drawString(avg, x + w / 2, y + 27);

  tft.setTextColor(p.muted, p.panel);
  tft.setTextFont(1);
  tft.drawString(lo, x + w / 2, y + 47);
  tft.drawString(hi, x + w / 2, y + 59);

  uint16_t wantedV = chartRangeSamples();
  uint8_t cov = wantedV ? (uint8_t)min(100UL, ((uint32_t)countV * 100UL) / wantedV) : 0;

  String n = "N=" + String(countV);
  n += "  ";
  n += String(cov);
  n += "%";
  tft.drawString(n, x + w / 2, y + 70);
  tft.setTextDatum(TL_DATUM);
}

void drawWindStatsCard(int x, int y, int w, int h) {
  SkinPalette p = paletteFor(currentSkin);

  tft.fillRoundRect(x, y, w, h, 5, p.panel);
  tft.drawRoundRect(x, y, w, h, 5, p.border);

  if (!deferChartLabels) {
    tft.setTextColor(p.accent, p.panel);
    tft.drawString("WIATR", x + 6, y + 5, 1);
  }

  float fW, lW, avgW, minW, maxW;
  float fG, lG, avgG, minG, maxG;
  uint16_t nW, nG;

  bool okW = chartMetricStats(3, fW, lW, avgW, minW, maxW, nW);
  bool okG = chartMetricStats(4, fG, lG, avgG, minG, maxG, nG);

  if (!okW) {
    tft.setTextColor(p.muted, p.panel);
    tft.drawCentreString("--", x + w / 2, y + 28, 2);
    return;
  }

  String avg = "AVG ";
  avg += String(avgW, 1);
  avg += "m/s";

  String maxTxt = "MAX ";
  maxTxt += String(maxW, 1);
  maxTxt += "m/s";

  String gustTxt = "PORYW ";
  gustTxt += okG ? String(maxG, 1) : String("--");
  gustTxt += "m/s";

  if (tft.textWidth(gustTxt, 1) > w - 12) {
    gustTxt = "P:";
    gustTxt += okG ? String(maxG, 1) : String("--");
    gustTxt += "m/s";
  }

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(p.text, p.panel);
  uint8_t avgFont = bestFont(avg, w - 14, 2);
  tft.setTextFont(avgFont);
  tft.drawString(avg, x + w / 2, y + 27);

  tft.setTextColor(p.muted, p.panel);
  tft.setTextFont(1);
  tft.drawString(maxTxt, x + w / 2, y + 47);
  tft.drawString(gustTxt, x + w / 2, y + 59);

  uint16_t wantedW = chartRangeSamples();
  uint8_t covW = wantedW ? (uint8_t)min(100UL, ((uint32_t)nW * 100UL) / wantedW) : 0;

  String n = "N=" + String(nW);
  n += "  ";
  n += String(covW);
  n += "%";
  tft.drawString(n, x + w / 2, y + 70);
  tft.setTextDatum(TL_DATUM);
}




AlertTimeWindow alertTimeWindowForMetric(uint8_t metric) {
  AlertTimeWindow w = {false, 0, 0};

  bool indoorMetric = chartMetricUsesIndoor(metric);
  bool use7d = (chartRange == CHART_RANGE_7D);

  if (use7d) {
    if (indoorMetric && !indoor7dLoaded)
      loadIndoor7DFromSD();
    else if (!indoorMetric && !chart7dLoaded)
      loadChart7DFromSD();
  }

  uint16_t sourceCount = use7d
                           ? (indoorMetric ? indoor7dCount : chart7dCount)
                           : chartCount;

  if (sourceCount == 0)
    return w;

  uint16_t wanted = chartRangeSamples();
  uint16_t visibleCount = sourceCount > wanted ? wanted : sourceCount;
  uint16_t logicalStart = sourceCount - visibleCount;

  uint32_t newestEpoch = 0;

  for (uint16_t i = 0; i < visibleCount; ++i) {
    const ChartSample *sp = nullptr;

    if (use7d) {
      sp = indoorMetric
             ? &indoor7d[logicalStart + i]
             : &chart7d[logicalStart + i];
    } else {
      int idx = chartIndexOldest(logicalStart + i);
      sp = &chartHistory[idx];
    }

    if (sp->epoch > newestEpoch)
      newestEpoch = sp->epoch;
  }

  if (newestEpoch <= 100000UL)
    return w;

  uint32_t axisEnd = historyEpochNow();
  if (axisEnd <= 100000UL || axisEnd < newestEpoch)
    axisEnd = newestEpoch;

  uint32_t windowSec = miniChartWindowSeconds();
  uint32_t axisStart = axisEnd > windowSec ? axisEnd - windowSec : 0;

  w.timeAxisAvailable = true;
  w.axisStart = axisStart;
  w.axisEnd = axisEnd;
  return w;
}

bool alertSampleVisibleInTime(uint32_t epoch, const AlertTimeWindow &w) {
  if (!w.timeAxisAvailable)
    return true;

  return epoch > 100000UL &&
         epoch >= w.axisStart &&
         epoch <= w.axisEnd;
}

bool alert7dSampleQualityOK(uint8_t metric, uint16_t sourceIndex) {
  if (chartRange != CHART_RANGE_7D)
    return true;

  bool indoorMetric = chartMetricUsesIndoor(metric);
  return chart7dQualitySamples(indoorMetric, sourceIndex) >= 6;
}

uint16_t visibleAlertSampleCount(uint8_t metric) {
  bool indoorMetric = chartMetricUsesIndoor(metric);
  bool use7d = (chartRange == CHART_RANGE_7D);

  if (use7d) {
    if (indoorMetric && !indoor7dLoaded)
      loadIndoor7DFromSD();
    else if (!indoorMetric && !chart7dLoaded)
      loadChart7DFromSD();
  }

  uint16_t sourceCount = use7d
                           ? (indoorMetric ? indoor7dCount : chart7dCount)
                           : chartCount;

  if (sourceCount == 0)
    return 0;

  uint16_t wanted = chartRangeSamples();
  uint16_t visibleCount = sourceCount > wanted ? wanted : sourceCount;
  uint16_t logicalStart = sourceCount - visibleCount;
  AlertTimeWindow tw = alertTimeWindowForMetric(metric);

  uint16_t n = 0;

  for (uint16_t i = 0; i < visibleCount; ++i) {
    const ChartSample *sp = nullptr;

    if (use7d) {
      sp = indoorMetric
             ? &indoor7d[logicalStart + i]
             : &chart7d[logicalStart + i];
    } else {
      int idx = chartIndexOldest(logicalStart + i);
      sp = &chartHistory[idx];
    }

    if (!alertSampleVisibleInTime(sp->epoch, tw))
      continue;

    if (use7d && !alert7dSampleQualityOK(metric, logicalStart + i))
      continue;

    float v = chartSampleValue(*sp, metric);
    if (isfinite(v))
      n++;
  }

  return n;
}

VisualState alertVisualState(uint8_t metric, float v, bool gustMode) {
  if (metric == 3 || metric == 4) return stateWind(v, gustMode);
  if (metric == 5) return stateUV(v);
  if (metric == 6) return stateRain(v);
  if (metric == 1 || metric == 9) return stateHumidity(v);
  return {TONE_OK, 0, "OK"};
}

bool alertStateIsActive(uint8_t metric, const VisualState &st) {
  // UV używa WARM/HOT dla wysokich poziomów, więc nie wolno
  // rozpoznawać alarmu wyłącznie po nazwie Tone.
  if (metric == 5)
    return st.severity >= 1;

  return st.tone == TONE_WARNING || st.tone == TONE_DANGER;
}

uint16_t countAlertSamples(uint8_t metric, bool gustMode) {
  bool indoorMetric = chartMetricUsesIndoor(metric);
  bool use7d = (chartRange == CHART_RANGE_7D);

  if (use7d) {
    if (indoorMetric && !indoor7dLoaded)
      loadIndoor7DFromSD();
    else if (!indoorMetric && !chart7dLoaded)
      loadChart7DFromSD();
  }

  uint16_t sourceCount;
  if (use7d)
    sourceCount = indoorMetric ? indoor7dCount : chart7dCount;
  else
    sourceCount = chartCount;

  if (sourceCount == 0)
    return 0;

  uint16_t wanted = chartRangeSamples();
  uint16_t visibleCount = sourceCount;
  if (visibleCount > wanted)
    visibleCount = wanted;

  uint16_t logicalStart = sourceCount - visibleCount;
  AlertTimeWindow tw = alertTimeWindowForMetric(metric);
  uint16_t alerts = 0;

  for (uint16_t i = 0; i < visibleCount; i++) {
    float v;

    if (use7d) {
      if (indoorMetric)
        v = chartSampleValue(indoor7d[logicalStart + i], metric);
      else
        v = chartSampleValue(chart7d[logicalStart + i], metric);
    } else {
      int idx = chartIndexOldest(logicalStart + i);
      v = chartSampleValue(chartHistory[idx], metric);
    }

    uint32_t epoch = 0;
    if (use7d) {
      const ChartSample &s = indoorMetric ? indoor7d[logicalStart + i] : chart7d[logicalStart + i];
      epoch = s.epoch;
    } else {
      int idxEpoch = chartIndexOldest(logicalStart + i);
      epoch = chartHistory[idxEpoch].epoch;
    }

    if (!alertSampleVisibleInTime(epoch, tw) || !isfinite(v))
      continue;

    if (use7d && !alert7dSampleQualityOK(metric, logicalStart + i))
      continue;

    VisualState ast = alertVisualState(metric, v, gustMode);

    if (alertStateIsActive(metric, ast))
      alerts++;
  }

  return alerts;
}




bool alertSampleGap(uint32_t prevEpoch, uint32_t currEpoch) {
  if (prevEpoch <= 100000UL || currEpoch <= 100000UL)
    return false;

  if (currEpoch <= prevEpoch)
    return true;

  uint32_t maxGap = (chartRange == CHART_RANGE_7D) ? 5400UL : 900UL;
  return (currEpoch - prevEpoch) > maxGap;
}

uint16_t countAlertEpisodes(uint8_t metric, bool gustMode) {
  bool indoorMetric = chartMetricUsesIndoor(metric);
  bool use7d = (chartRange == CHART_RANGE_7D);

  if (use7d) {
    if (indoorMetric && !indoor7dLoaded)
      loadIndoor7DFromSD();
    else if (!indoorMetric && !chart7dLoaded)
      loadChart7DFromSD();
  }

  uint16_t sourceCount = use7d ?
                         (indoorMetric ? indoor7dCount : chart7dCount) :
                         chartCount;

  if (sourceCount == 0)
    return 0;

  uint16_t wanted = chartRangeSamples();
  uint16_t visibleCount = sourceCount > wanted ? wanted : sourceCount;
  uint16_t logicalStart = sourceCount - visibleCount;
  AlertTimeWindow tw = alertTimeWindowForMetric(metric);

  uint16_t episodes = 0;
  bool wasAlert = false;
  uint32_t prevEpoch = 0;

  for (uint16_t i = 0; i < visibleCount; i++) {
    float v;
    uint32_t epoch = 0;

    if (use7d) {
      const ChartSample &s = indoorMetric ?
                             indoor7d[logicalStart + i] :
                             chart7d[logicalStart + i];
      v = chartSampleValue(s, metric);
      epoch = s.epoch;
    } else {
      int idx = chartIndexOldest(logicalStart + i);
      const ChartSample &s = chartHistory[idx];
      v = chartSampleValue(s, metric);
      epoch = s.epoch;
    }

    if (!alertSampleVisibleInTime(epoch, tw)) {
      wasAlert = false;
      prevEpoch = 0;
      continue;
    }

    if (use7d && !alert7dSampleQualityOK(metric, logicalStart + i)) {
      wasAlert = false;
      prevEpoch = 0;
      continue;
    }

    if (alertSampleGap(prevEpoch, epoch))
      wasAlert = false;

    if (epoch > 100000UL)
      prevEpoch = epoch;

    if (!isfinite(v)) {
      wasAlert = false;
      continue;
    }

    VisualState ast = alertVisualState(metric, v, gustMode);

    bool isAlert = alertStateIsActive(metric, ast);

    if (isAlert && !wasAlert)
      episodes++;

    wasAlert = isAlert;
  }

  return episodes;
}

uint16_t longestAlertStreak(uint8_t metric, bool gustMode) {
  bool indoorMetric = chartMetricUsesIndoor(metric);
  bool use7d = (chartRange == CHART_RANGE_7D);

  if (use7d) {
    if (indoorMetric && !indoor7dLoaded)
      loadIndoor7DFromSD();
    else if (!indoorMetric && !chart7dLoaded)
      loadChart7DFromSD();
  }

  uint16_t sourceCount = use7d ?
                         (indoorMetric ? indoor7dCount : chart7dCount) :
                         chartCount;

  if (sourceCount == 0)
    return 0;

  uint16_t wanted = chartRangeSamples();
  uint16_t visibleCount = sourceCount > wanted ? wanted : sourceCount;
  uint16_t logicalStart = sourceCount - visibleCount;
  AlertTimeWindow tw = alertTimeWindowForMetric(metric);

  uint16_t current = 0;
  uint16_t longest = 0;
  uint32_t prevEpoch = 0;

  for (uint16_t i = 0; i < visibleCount; i++) {
    float v;
    uint32_t epoch = 0;

    if (use7d) {
      const ChartSample &s = indoorMetric ?
                             indoor7d[logicalStart + i] :
                             chart7d[logicalStart + i];
      v = chartSampleValue(s, metric);
      epoch = s.epoch;
    } else {
      int idx = chartIndexOldest(logicalStart + i);
      const ChartSample &s = chartHistory[idx];
      v = chartSampleValue(s, metric);
      epoch = s.epoch;
    }

    if (!alertSampleVisibleInTime(epoch, tw)) {
      current = 0;
      prevEpoch = 0;
      continue;
    }

    if (use7d && !alert7dSampleQualityOK(metric, logicalStart + i)) {
      current = 0;
      prevEpoch = 0;
      continue;
    }

    if (alertSampleGap(prevEpoch, epoch))
      current = 0;

    if (epoch > 100000UL)
      prevEpoch = epoch;

    if (!isfinite(v)) {
      current = 0;
      continue;
    }

    VisualState ast = alertVisualState(metric, v, gustMode);

    bool isAlert = alertStateIsActive(metric, ast);

    if (isAlert) {
      current++;
      if (current > longest)
        longest = current;
    } else {
      current = 0;
    }
  }

  return longest;
}

Tone worstAlertToneForRange(String &labelOut, String &valueOut) {
  labelOut = "BRAK";
  valueOut = "--";

  Tone worst = TONE_OK;
  uint8_t worstSeverity = 0;
  float worstValue = NAN;

  struct Candidate {
    const char *label;
    uint8_t metric;
    bool gustMode;
  };

  const Candidate cands[] = {
    {"WIATR", 3, false},
    {"PORYW", 4, true},
    {"UV", 5, false},
    {"OPAD", 6, false},
    {"WILG", 1, false}
  };

  for (const auto &c : cands) {
    bool use7d = (chartRange == CHART_RANGE_7D);
    bool indoorMetric = chartMetricUsesIndoor(c.metric);

    if (use7d) {
      if (indoorMetric && !indoor7dLoaded)
        loadIndoor7DFromSD();
      else if (!indoorMetric && !chart7dLoaded)
        loadChart7DFromSD();
    }

    uint16_t sourceCount;
    if (use7d)
      sourceCount = indoorMetric ? indoor7dCount : chart7dCount;
    else
      sourceCount = chartCount;

    if (sourceCount == 0)
      continue;

    uint16_t wanted = chartRangeSamples();
    uint16_t visibleCount = sourceCount;
    if (visibleCount > wanted)
      visibleCount = wanted;

    uint16_t logicalStart = sourceCount - visibleCount;
    AlertTimeWindow tw = alertTimeWindowForMetric(c.metric);

    for (uint16_t i = 0; i < visibleCount; i++) {
      const ChartSample *sp = nullptr;

      if (use7d) {
        sp = indoorMetric
               ? &indoor7d[logicalStart + i]
               : &chart7d[logicalStart + i];
      } else {
        int idx = chartIndexOldest(logicalStart + i);
        sp = &chartHistory[idx];
      }

      if (!alertSampleVisibleInTime(sp->epoch, tw))
        continue;

      if (use7d && !alert7dSampleQualityOK(c.metric, logicalStart + i))
        continue;

      float v = chartSampleValue(*sp, c.metric);
      if (!isfinite(v))
        continue;

      VisualState st = alertVisualState(c.metric, v, c.gustMode);

      // RC15.176: enum Tone jest paleta/rodzajem koloru, NIE skala severity.
      // Wybieramy najgorszy stan wyłącznie po VisualState.severity.
      if (st.severity > worstSeverity) {
        worstSeverity = st.severity;
        worst = st.tone;
        labelOut = String(c.label);
        worstValue = v;
      } else if (st.severity == worstSeverity &&
                 st.severity > 0 &&
                 isfinite(v)) {
        if (!isfinite(worstValue) || v > worstValue) {
          worst = st.tone;
          labelOut = String(c.label);
          worstValue = v;
        }
      }
    }
  }

  if (isfinite(worstValue)) {
    if (labelOut == "UV") {
      valueOut = String(worstValue, 1);
    } else if (labelOut == "OPAD") {
      valueOut = String(worstValue, 1) + "mm/h";
    } else if (labelOut == "WILG") {
      valueOut = String(worstValue, 0) + "%";
    } else {
      valueOut = String(worstValue, 1) + "m/s";
    }
  }

  return worst;
}

void drawAlertRow(int y, const String &label, const String &value, Tone tone) {
  SkinPalette p = paletteFor(currentSkin);
  uint16_t c = toneColor(tone);

  tft.fillRoundRect(8, y, 304, 25, 5, p.panel);
  tft.drawRoundRect(8, y, 304, 25, 5, c);

  tft.setTextColor(p.muted, p.panel);
  tft.drawString(label, 15, y + 6, 1);

  int labelW = tft.textWidth(label, 1);
  int valueRight = 305;
  int valueLeft = 15 + labelW + 8;
  int availableW = valueRight - valueLeft;

  if (availableW < 20)
    availableW = 20;

  uint8_t valueFont = bestFont(value, availableW, 2);

  tft.setTextColor(c, p.panel);

  // Font 1 jest niższy, więc przesuwamy go minimalnie w dół,
  // aby optycznie pozostał wycentrowany w 25 px wierszu.
  int valueY = (valueFont == 1) ? (y + 8) : (y + 4);
  tft.drawRightString(value, valueRight, valueY, valueFont);
}


String compactAlertValueIfNeeded(const String &full,
                                 const String &weatherPart,
                                 uint16_t alerts,
                                 uint16_t samples,
                                 uint16_t episodes,
                                 int availableW) {
  if (tft.textWidth(full, 1) <= availableW)
    return full;

  // Najpierw skracamy opis statystyk, nie samą wartość pogodową.
  String compact = weatherPart;
  compact += " A";
  compact += String(alerts);
  compact += "/";
  compact += String(samples);
  compact += " E";
  compact += String(episodes);

  if (tft.textWidth(compact, 1) <= availableW)
    return compact;

  compact = weatherPart;
  compact += " ";
  compact += String(alerts);
  compact += "/";
  compact += String(samples);

  return compact;
}


String alertWindGustLabel() {
  switch (currentLanguage) {
    case LANG_EN: return "WIND / GUST";
    case LANG_DE: return "WIND / BOE";
    case LANG_CZ: return "VITR / NARAZ";
    default:      return "WIATR / PORYW";
  }
}

String alertRainLabel() {
  switch (currentLanguage) {
    case LANG_EN: return "RAIN";
    case LANG_DE: return "REGEN";
    case LANG_CZ: return "DEST";
    default:      return "OPAD";
  }
}

String alertHumidityLabel() {
  switch (currentLanguage) {
    case LANG_EN: return "HUMIDITY";
    case LANG_DE: return "FEUCHTE";
    case LANG_CZ: return "VLHKOST";
    default:      return "WILGOTNOŚĆ";
  }
}

String alertRxLabelBase() {
  switch (currentLanguage) {
    case LANG_EN: return "LINK RX#";
    case LANG_DE: return "LINK RX#";
    case LANG_CZ: return "SPOJ RX#";
    default:      return "ŁĄCZNOŚĆ RX#";
  }
}

String alertWaitLiveText() {
  switch (currentLanguage) {
    case LANG_EN: return "WAIT LIVE";
    case LANG_DE: return "WARTE LIVE";
    case LANG_CZ: return "CEKAM LIVE";
    default:      return "CZEKAM NA LIVE";
  }
}

String alertNoDataText() {
  switch (currentLanguage) {
    case LANG_EN: return "NO DATA";
    case LANG_DE: return "KEINE DATEN";
    case LANG_CZ: return "BEZ DAT";
    default:      return "BRAK DANYCH";
  }
}

String alertNoWarningsText() {
  switch (currentLanguage) {
    case LANG_EN: return "NO WARNINGS";
    case LANG_DE: return "KEINE WARNUNG";
    case LANG_CZ: return "BEZ VAROVANI";
    default:      return "BRAK OSTRZEŻEŃ";
  }
}

String alertLocalizedMetricLabel(const String &internalLabel) {
  if (internalLabel == "WIATR") {
    if (currentLanguage == LANG_EN) return "WIND";
    if (currentLanguage == LANG_DE) return "WIND";
    if (currentLanguage == LANG_CZ) return "VITR";
    return "WIATR";
  }

  if (internalLabel == "PORYW") {
    if (currentLanguage == LANG_EN) return "GUST";
    if (currentLanguage == LANG_DE) return "BOE";
    if (currentLanguage == LANG_CZ) return "NARAZ";
    return "PORYW";
  }

  if (internalLabel == "OPAD") return alertRainLabel();

  if (internalLabel == "WILG") {
    if (currentLanguage == LANG_EN) return "HUM";
    if (currentLanguage == LANG_DE) return "FEUCHT";
    if (currentLanguage == LANG_CZ) return "VLHK";
    return "WILG";
  }

  return internalLabel; // UV
}

void drawAlertsPage(const WeatherPacket &d) {
  SkinPalette p = paletteFor(currentSkin);

  // Pobieramy ostatnie poprawne dane z historii, jesli po restarcie
  // jeszcze nie przyszla nowa ramka ESP-NOW.
  float wind = d.predkosc_wiatru;
  float gust = d.poryw_wiatru;
  float uv = d.uv_index;
  float rain = d.opady_godzina;
  float hum = d.wilgotnosc;

  if (!activeSourceAvailable() && chartCount > 0) {
    int lastIdx = chartIndexOldest(chartCount - 1);
    const ChartSample &last = chartHistory[lastIdx];
    wind = last.wind;
    gust = last.gust;
    uv = last.uv;
    rain = last.rain;
    hum = last.hum;
  }

  uint32_t age = activeSourceAvailable() ? activeSourceAgeSec() : 999999UL;

  Tone windTone = stateWind(wind, false).tone;
  Tone gustTone = stateWind(gust, true).tone;
  Tone uvTone = stateUV(uv).tone;
  Tone rainTone = stateRain(rain).tone;
  Tone humTone = stateHumidity(hum).tone;

  Tone rxTone = TONE_OK;
  String rxText;

  if (!activeSourceAvailable()) {
    // Po restarcie historia SD jest juz dostepna, ale pierwszy live pakiet
    // ESP-NOW moze jeszcze nie nadejsc. Nie oznaczamy tego jako awarii.
    if (chartCount > 0) {
      rxTone = TONE_WARNING;
      rxText = alertWaitLiveText();
    } else {
      rxTone = TONE_DANGER;
      rxText = alertNoDataText();
    }
  } else if (age > 60) {
    rxTone = TONE_DANGER;
    rxText = "STALE " + String(age) + "s";
  } else if (age > 15) {
    rxTone = TONE_WARNING;
    rxText = "STALE " + String(age) + "s";
  } else {
    rxTone = TONE_OK;
    rxText = "LIVE " + String(age) + "s";
  }

  uint16_t nWind = visibleAlertSampleCount(3);
  uint16_t nUV = visibleAlertSampleCount(5);
  uint16_t nRain = visibleAlertSampleCount(6);
  uint16_t nHum = visibleAlertSampleCount(1);

  uint16_t aWind = countAlertSamples(3, false);
  uint16_t aGust = countAlertSamples(4, true);
  uint16_t aUV = countAlertSamples(5, false);
  uint16_t aRain = countAlertSamples(6, false);
  uint16_t aHum = countAlertSamples(1, false);

  uint16_t sWind = currentAlertStreak(3, false);
  uint16_t sGust = currentAlertStreak(4, true);
  uint16_t sUV = currentAlertStreak(5, false);
  uint16_t sRain = currentAlertStreak(6, false);
  uint16_t sHum = currentAlertStreak(1, false);

  uint16_t eWind = countAlertEpisodes(3, false);
  uint16_t eGust = countAlertEpisodes(4, true);
  uint16_t eUV = countAlertEpisodes(5, false);
  uint16_t eRain = countAlertEpisodes(6, false);
  uint16_t eHum = countAlertEpisodes(1, false);

  uint16_t lWind = longestAlertStreak(3, false);
  uint16_t lGust = longestAlertStreak(4, true);
  uint16_t lUV = longestAlertStreak(5, false);
  uint16_t lRain = longestAlertStreak(6, false);
  uint16_t lHum = longestAlertStreak(1, false);

  String windWeather = String(wind, 1) + "/" + String(gust, 1) + "m/s";
  String windTxt = windWeather;
  windTxt += " A:";
  windTxt += String((aGust > aWind) ? aGust : aWind);
  windTxt += "/";
  windTxt += String(nWind);
  windTxt += " E:";
  windTxt += String((eGust > eWind) ? eGust : eWind);

  String uvWeather = String(uv, 1);
  String uvTxt = uvWeather;
  uvTxt += " A:";
  uvTxt += String(aUV);
  uvTxt += "/";
  uvTxt += String(nUV);
  uvTxt += " E:";
  uvTxt += String(eUV);

  String rainWeather = String(rain, 1) + "mm/h";
  String rainTxt = rainWeather;
  rainTxt += " A:";
  rainTxt += String(aRain);
  rainTxt += "/";
  rainTxt += String(nRain);
  rainTxt += " E:";
  rainTxt += String(eRain);

  String humWeather = String(hum, 0) + "%";
  String humTxt = humWeather;
  humTxt += " A:";
  humTxt += String(aHum);
  humTxt += "/";
  humTxt += String(nHum);
  humTxt += " E:";
  humTxt += String(eHum);

  String windRowLabel = alertWindGustLabel();
  String rainRowLabel = alertRainLabel();
  String humRowLabel = alertHumidityLabel();

  // Dostępne szerokości wynikają z długości etykiet w font 1.
  // Jeśli nawet font 1 byłby za szeroki, skracamy tylko część statystyczną.
  windTxt = compactAlertValueIfNeeded(
      windTxt, windWeather,
      (aGust > aWind) ? aGust : aWind,
      nWind,
      (eGust > eWind) ? eGust : eWind,
      305 - (15 + tft.textWidth(windRowLabel, 1) + 8));

  uvTxt = compactAlertValueIfNeeded(
      uvTxt, uvWeather, aUV, nUV, eUV,
      305 - (15 + tft.textWidth("UV", 1) + 8));

  rainTxt = compactAlertValueIfNeeded(
      rainTxt, rainWeather, aRain, nRain, eRain,
      305 - (15 + tft.textWidth(rainRowLabel, 1) + 8));

  humTxt = compactAlertValueIfNeeded(
      humTxt, humWeather, aHum, nHum, eHum,
      305 - (15 + tft.textWidth(humRowLabel, 1) + 8));

  drawAlertRow(35,  windRowLabel, windTxt,
               (gustTone > windTone) ? gustTone : windTone);
  drawAlertRow(64,  "UV", uvTxt, uvTone);
  drawAlertRow(93,  rainRowLabel, rainTxt, rainTone);
  drawAlertRow(122, humRowLabel, humTxt, humTone);
  String rxLabel = alertRxLabelBase() + String(activeSourcePacketCount());

  // Po bardzo długiej pracy licznik RX może mieć wiele cyfr.
  // Nie pozwalamy, aby etykieta zjadła miejsce na LIVE/STALE.
  if (tft.textWidth(rxLabel, 1) > 118)
    rxLabel = "RX#" + String(activeSourcePacketCount());

  drawAlertRow(151, rxLabel, rxText, rxTone);

  // Podsumowanie wszystkich alertow.
  int warnings = 0;
  if (windTone == TONE_WARNING || windTone == TONE_DANGER) warnings++;
  if (gustTone == TONE_WARNING || gustTone == TONE_DANGER) warnings++;
  if (stateUV(uv).severity >= 1) warnings++;
  if (rainTone == TONE_WARNING || rainTone == TONE_DANGER) warnings++;
  if (humTone == TONE_WARNING || humTone == TONE_DANGER) warnings++;
  if (rxTone == TONE_WARNING || rxTone == TONE_DANGER) warnings++;

  String summary;
  Tone sumTone;

  String worstLabel;
  String worstValue;
  Tone worstTone = worstAlertToneForRange(worstLabel, worstValue);

  // RC15.214: serie L/T musza nalezec do tego samego parametru,
  // ktory zostal wybrany jako MAX w podsumowaniu.
  uint16_t worstLongest = 0;
  uint16_t worstCurrent = 0;

  if (worstLabel == "WIATR") {
    worstLongest = lWind;
    worstCurrent = sWind;
  } else if (worstLabel == "PORYW") {
    worstLongest = lGust;
    worstCurrent = sGust;
  } else if (worstLabel == "UV") {
    worstLongest = lUV;
    worstCurrent = sUV;
  } else if (worstLabel == "OPAD") {
    worstLongest = lRain;
    worstCurrent = sRain;
  } else if (worstLabel == "WILG") {
    worstLongest = lHum;
    worstCurrent = sHum;
  }

  if (warnings == 0 && worstTone == TONE_OK) {
    summary = alertNoWarningsText();
    sumTone = TONE_OK;
  } else {
    summary = "ALERT ";
    summary += alertLocalizedMetricLabel(worstLabel);
    summary += " ";
    summary += worstValue;
    summary += " L:";
    summary += alertStreakText(worstLongest).substring(2);
    if (worstCurrent > 0) {
      summary += " T:";
      summary += alertStreakText(worstCurrent).substring(2);
    }

    sumTone = worstTone;
    if (sumTone == TONE_OK)
      sumTone = (warnings >= 3) ? TONE_DANGER : TONE_WARNING;
  }

  tft.fillRoundRect(8, 182, 304, 17, 5, p.panel2);
  tft.setTextColor(toneColor(sumTone), p.panel2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  tft.drawString(summary, 160, 190);
  tft.setTextDatum(TL_DATUM);
}



void redrawTrendPageOnly() {
  if (currentSkin != SKIN_CHARTS || chartPage != CHART_PAGE_TREND)
    return;

  SkinPalette p = paletteFor(currentSkin);
  tft.fillRect(0, 30, SCREEN_W, 172, p.bg);

  deferChartLabels = true;

  if (!showIndoor) {
    drawTrendCard(6,   34, 151, 81, "TEMP ZEW", 0, "C");
    drawTrendCard(163, 34, 151, 81, chartCompactLabel(2, false), 2, "hPa");
    drawTrendCard(6,  119, 151, 81, "WILG ZEW", 1, "%");
    drawTrendCard(163,119, 151, 81, "WIATR", 3, "m/s");
  } else {
    drawTrendCard(6,   34, 151, 81, "TEMP WEW", 8, "C");
    drawTrendCard(163, 34, 151, 81, "WILG WEW", 9, "%");
    drawTrendCard(6,  119, 151, 81, "ROSA WEW", 11, "C");
    drawTrendCard(163,119, 151, 81, "AH WEW", 13, "g/m3");
  }

  deferChartLabels = false;
  drawChartPageLabelsBatch();
  refreshChartsFooterDynamic();
}

void redrawStatsPageOnly() {
  if (currentSkin != SKIN_CHARTS || chartPage != CHART_PAGE_STATS)
    return;

  SkinPalette p = paletteFor(currentSkin);

  // Czyścimy tylko obszar kart STAT, nie cały ekran i nie footer.
  tft.fillRect(0, 30, SCREEN_W, 172, p.bg);

  deferChartLabels = true;

  if (!showIndoor) {
    drawStatsCard(6,   34, 151, 81, "TEMP ZEW", 0, "C");
    drawStatsCard(163, 34, 151, 81, "WILG ZEW", 1, "%");
    drawStatsCard(6,  119, 151, 81, chartCompactLabel(2, false), 2, "hPa");
    drawWindStatsCard(163,119, 151, 81);
  } else {
    drawStatsCard(6,   34, 151, 81, "TEMP WEW", 8, "C");
    drawStatsCard(163, 34, 151, 81, "WILG WEW", 9, "%");
    drawStatsCard(6,  119, 151, 81, "ROSA WEW", 11, "C");
    drawStatsCard(163,119, 151, 81, "AH WEW", 13, "g/m3");
  }

  deferChartLabels = false;
  drawChartPageLabelsBatch();

  // Odtwarzamy dynamiczny info-row. Paski PAGE/RANGE pozostają na swojej
  // osobnej linii i nie są czyszczone.
  refreshChartsFooterDynamic();
}


String chartCompactLabel(uint8_t metric, bool indoorVariant) {
  switch (currentLanguage) {
    case LANG_EN:
      switch (metric) {
        case 0:  return indoorVariant ? "TEMP IN" : "TEMP OUT";
        case 1:  return indoorVariant ? "HUM IN" : "HUM OUT";
        case 2:  return "PRESS";
        case 3:  return "WIND";
        case 4:  return "GUST";
        case 5:  return "UV";
        case 6:  return "RAIN";
        case 7:  return "LIGHT";
        case 10: return indoorVariant ? "DEW IN" : "DEW OUT";
        case 12: return indoorVariant ? "AH IN" : "AH OUT";
      }
      break;

    case LANG_DE:
      switch (metric) {
        case 0:  return indoorVariant ? "TEMP IN" : "TEMP AUS";
        case 1:  return indoorVariant ? "FEUCHT IN" : "FEUCHT A";
        case 2:  return "DRUCK";
        case 3:  return "WIND";
        case 4:  return "BOE";
        case 5:  return "UV";
        case 6:  return "REGEN";
        case 7:  return "LICHT";
        case 10: return indoorVariant ? "TAU IN" : "TAU AUS";
        case 12: return indoorVariant ? "AH IN" : "AH AUS";
      }
      break;

    case LANG_CZ:
      switch (metric) {
        case 0:  return indoorVariant ? "TEPL UVN" : "TEPL VEN";
        case 1:  return indoorVariant ? "VLH UVN" : "VLH VEN";
        case 2:  return "TLAK";
        case 3:  return "VÍTR";
        case 4:  return "NÁRAZ";
        case 5:  return "UV";
        case 6:  return "DÉŠŤ";
        case 7:  return "SVĚTLO";
        case 10: return indoorVariant ? "ROSA UVN" : "ROSA VEN";
        case 12: return indoorVariant ? "AH UVN" : "AH VEN";
      }
      break;

    default:
      switch (metric) {
        case 0:  return indoorVariant ? "TEMP WEW" : "TEMP ZEW";
        case 1:  return indoorVariant ? "WILG WEW" : "WILG ZEW";
        case 2:  return "CIŚNIENIE";
        case 3:  return "WIATR";
        case 4:  return "PORYW";
        case 5:  return "UV";
        case 6:  return "OPAD";
        case 7:  return "ŚWIATŁO";
        case 10: return indoorVariant ? "ROSA WEW" : "ROSA ZEW";
        case 12: return indoorVariant ? "AH WEW" : "AH ZEW";
      }
      break;
  }

  return "--";
}

String chartNowText() {
  switch (currentLanguage) {
    case LANG_EN: return "NOW";
    case LANG_DE: return "JETZT";
    case LANG_CZ: return "NYNI";
    default:      return "TERAZ";
  }
}

void drawChartPageLabelsBatch() {
  if (chartPage == CHART_PAGE_ALERTS)
    return;

  SkinPalette p = paletteFor(currentSkin);

  // WYKRESY preferują osobny font 12 px.
  // Gdy go nie ma na SD, bezpieczny fallback to dotychczasowy regular 14 px.
  bool smoothSmall = useLanguageSmallFont();
  bool smoothRegular = false;

  if (!smoothSmall)
    smoothRegular = useLanguageUiFont(false);

  bool smooth = smoothSmall || smoothRegular;

  struct LabelItem {
    int x;
    int y;
    int maxW;
    String text;
    String fallback;
  };

  LabelItem items[4];
  bool haveItems = true;

  if (chartPage == CHART_PAGE_BASIC) {
    items[0] = {11,  37, 86, chartCompactLabel(0, false), "TEMP"};
    items[1] = {168, 37, 86, chartCompactLabel(1, false), "WILG"};
    items[2] = {11, 126, 86, chartCompactLabel(2, false), "CISN"};
    items[3] = {168,126, 86, chartCompactLabel(3, false), "WIATR"};
  } else if (chartPage == CHART_PAGE_EXTRA) {
    items[0] = {11,  37, 86, chartCompactLabel(4, false), "PORYW"};
    items[1] = {168, 37, 86, chartCompactLabel(5, false), "UV"};
    items[2] = {11, 126, 86, chartCompactLabel(6, false), "OPAD"};
    items[3] = {168,126, 86, chartCompactLabel(7, false), "LUX"};
  } else if (chartPage == CHART_PAGE_INOUT) {
    items[0] = {11,  37, 86, chartCompactLabel(0, false), "TEMP ZEW"};
    items[1] = {168, 37, 86, chartCompactLabel(0, true),  "TEMP WEW"};
    items[2] = {11, 126, 86, chartCompactLabel(1, false), "WILG ZEW"};
    items[3] = {168,126, 86, chartCompactLabel(1, true),  "WILG WEW"};
  } else if (chartPage == CHART_PAGE_COMFORT) {
    items[0] = {11,  37, 86, chartCompactLabel(10, false), "ROSA ZEW"};
    items[1] = {168, 37, 86, chartCompactLabel(10, true),  "ROSA WEW"};
    items[2] = {11, 126, 86, chartCompactLabel(12, false), "AH ZEW"};
    items[3] = {168,126, 86, chartCompactLabel(12, true),  "AH WEW"};
  } else if (chartPage == CHART_PAGE_TREND || chartPage == CHART_PAGE_STATS) {
    if (!showIndoor) {
      items[0] = {12,  39, 100, chartCompactLabel(0, false), "TEMP ZEW"};
      items[1] = {169, 39, 100,
                  chartPage == CHART_PAGE_TREND ? chartCompactLabel(2, false)
                                                : chartCompactLabel(1, false),
                  chartPage == CHART_PAGE_TREND ? "CISN" : "WILG ZEW"};
      items[2] = {12, 124, 100,
                  chartPage == CHART_PAGE_TREND ? chartCompactLabel(1, false)
                                                : chartCompactLabel(2, false),
                  chartPage == CHART_PAGE_TREND ? "WILG ZEW" : "CISN"};
      items[3] = {169,124, 100, chartCompactLabel(3, false), "WIATR"};
    } else {
      items[0] = {12,  39, 100, chartCompactLabel(0, true), "TEMP WEW"};
      items[1] = {169, 39, 100, chartCompactLabel(1, true), "WILG WEW"};
      items[2] = {12, 124, 100, chartCompactLabel(10, true), "ROSA WEW"};
      items[3] = {169,124, 100, chartCompactLabel(12, true), "AH WEW"};
    }
  } else {
    haveItems = false;
  }

  if (!haveItems) {
    if (smooth)
      unloadSmoothFontSafe();
    return;
  }

  tft.setTextDatum(TL_DATUM);

  for (auto &item : items) {
    tft.setTextColor(p.muted, p.panel);

    if (smooth) {
      String s = item.text;
      while (s.length() > 1 && tft.textWidth(s) > item.maxW)
        s.remove(s.length() - 1);

      if (s != item.text && s.length() > 1) {
        while (s.length() > 1 && tft.textWidth(s + ".") > item.maxW)
          s.remove(s.length() - 1);
        s += ".";
      }

      int yy = item.y + (smoothSmall ? 1 : 0);
      tft.drawString(s, item.x, yy);
    } else {
      if (currentLanguage == LANG_PL || currentLanguage == LANG_DE || currentLanguage == LANG_CZ)
        drawSafeI18nString(item.text, item.x, item.y, 1, p.muted, p.panel, TL_DATUM);
      else
        tft.drawString(item.fallback, item.x, item.y, 1);
    }
  }

  if (smooth)
    unloadSmoothFontSafe();
}

void drawCharts(const WeatherPacket &d) {
  SkinPalette p = paletteFor(currentSkin);

  // RC15.230: prefetch 7D ZANIM narysujemy choc jeden piksel nowej planszy.
  if (chartRange == CHART_RANGE_7D) {
    if (!chart7dLoaded)
      loadChart7DFromSD();
    if ((chartPage == CHART_PAGE_INOUT ||
         chartPage == CHART_PAGE_COMFORT ||
         ((chartPage == CHART_PAGE_TREND || chartPage == CHART_PAGE_STATS) && showIndoor)) &&
        !indoor7dLoaded)
      loadIndoor7DFromSD();
  }

  if (forceScreenClear)
    tft.fillScreen(p.bg);

  drawHeader(chartsTitle());

  // Kolory wykresow musza odpowiadac wartosciom, ktore faktycznie
  // widzimy na wykresie. Zaraz po restarcie liveData jest jeszcze puste,
  // ale historia z SD jest juz zaladowana. Wtedy bierzemy ostatnia
  // probke historii zamiast zerowego liveData.
  float colorTemp  = d.temperatura;
  float colorHum   = d.wilgotnosc;
  float colorPress = d.cisnienie;
  float colorWind  = d.predkosc_wiatru;

  if (!activeSourceAvailable() && chartCount > 0) {
    int lastIdx = chartIndexOldest(chartCount - 1);
    const ChartSample &last = chartHistory[lastIdx];

    colorTemp  = last.temp;
    colorHum   = last.hum;
    colorPress = last.press;
    colorWind  = last.wind;
  }

  // Rysujemy tylko obszar wykresow, aby odswiezanie nie mrugalo.
  tft.fillRect(0, 28, 320, 188, p.bg);

  deferChartLabels = (chartPage != CHART_PAGE_ALERTS);

  if (chartPage == CHART_PAGE_BASIC) {
    drawMiniChart(6,   34, 151, 83, 0, "TEMP", "C",
                  toneColor(stateTemperature(colorTemp).tone));
    drawMiniChart(163, 34, 151, 83, 1, "WILG", "%",
                  toneColor(stateHumidity(colorHum).tone));
    drawMiniChart(6,  123, 151, 79, 2, "CISN", "hPa",
                  toneColor(statePressure(colorPress).tone));
    drawMiniChart(163,123, 151, 79, 3, "WIATR","m/s",
                  toneColor(stateWind(colorWind, false).tone));
  } else if (chartPage == CHART_PAGE_EXTRA) {
    float colorGust = d.poryw_wiatru;
    float colorUv = d.uv_index;
    float colorRain = d.opady_godzina;
    float colorLux = d.swiatlo_lux;

    if (!activeSourceAvailable() && chartCount > 0) {
      int lastIdx = chartIndexOldest(chartCount - 1);
      const ChartSample &last = chartHistory[lastIdx];
      colorGust = last.gust;
      colorUv = last.uv;
      colorRain = last.rain;
      colorLux = last.lux;
    }

    drawMiniChart(6,   34, 151, 83, 4, "PORYW", "m/s",
                  toneColor(stateWind(colorGust, true).tone));
    drawMiniChart(163, 34, 151, 83, 5, "UV", "",
                  toneColor(stateUV(colorUv).tone));
    drawMiniChart(6,  123, 151, 79, 6, "OPAD", "mm/h",
                  toneColor(stateRain(colorRain).tone));
    drawMiniChart(163,123, 151, 79, 7, "LUX", "lx",
                  toneColor(stateLux(colorLux).tone));
  } else if (chartPage == CHART_PAGE_INOUT) {
    float tOut = d.temperatura;
    float tIn = d.temp_wewnetrzna;
    float hOut = d.wilgotnosc;
    float hIn = d.wilg_wewnetrzna;

    if (!activeSourceAvailable() && chartCount > 0) {
      int lastIdx = chartIndexOldest(chartCount - 1);
      const ChartSample &last = chartHistory[lastIdx];
      tOut = last.temp;
      tIn = last.tempIn;
      hOut = last.hum;
      hIn = last.humIn;
    }

    drawMiniChart(6,   34, 151, 83, 0, "TEMP ZEW", "C",
                  toneColor(stateTemperature(tOut).tone));
    drawMiniChart(163, 34, 151, 83, 8, "TEMP WEW", "C",
                  toneColor(stateTemperature(tIn).tone));
    drawMiniChart(6,  123, 151, 79, 1, "WILG ZEW", "%",
                  toneColor(stateHumidity(hOut).tone));
    drawMiniChart(163,123, 151, 79, 9, "WILG WEW", "%",
                  toneColor(stateHumidity(hIn).tone));

    float dTemp = (isfinite(tIn) && isfinite(tOut)) ? (tIn - tOut) : NAN;
    float dHum  = (isfinite(hIn) && isfinite(hOut)) ? (hIn - hOut) : NAN;

    String cmp = "DT ";
    cmp += fmtSigned(dTemp, 1);
    cmp += "C  DH ";
    cmp += fmtSigned(dHum, 0);
    cmp += "%";

    tft.fillRect(112, 202, 208, 12, p.bg);

    uint16_t wewWanted = chartRangeSamples();
    uint16_t wewActual = 0;

    if (chartRange == CHART_RANGE_7D) {
      wewActual = chart7dReliableHourCount(true);
    } else {
      uint32_t indoorRecords = (uint32_t)(indoorHistoryFileBytes / sizeof(IndoorDiskRecord));
      if (indoorRecords > chartCount)
        indoorRecords = chartCount;
      wewActual = (uint16_t)indoorRecords;
    }

    if (wewActual > wewWanted)
      wewActual = wewWanted;

    String wewProg = "H:" + String(wewActual) + "/" + String(wewWanted);
    tft.setTextColor(p.muted, p.bg);
    tft.setTextDatum(ML_DATUM);
    tft.setTextFont(1);
    tft.drawString(wewProg, 116, 208);

    tft.setTextDatum(MR_DATUM);
    tft.drawString(cmp, 316, 208);
    tft.setTextDatum(TL_DATUM);

  } else if (chartPage == CHART_PAGE_COMFORT) {
    // KOMF: wartości pochodne, liczone z już istniejącej historii.
    float tOut = d.temperatura;
    float hOut = d.wilgotnosc;
    float tIn = d.temp_wewnetrzna;
    float hIn = d.wilg_wewnetrzna;

    if (!activeSourceAvailable() && chartCount > 0) {
      int lastIdx = chartIndexOldest(chartCount - 1);
      const ChartSample &last = chartHistory[lastIdx];
      tOut = last.temp;
      hOut = last.hum;
      tIn = last.tempIn;
      hIn = last.humIn;
    }

    float dewOut = dewPointC(tOut, hOut);
    float dewIn = dewPointC(tIn, hIn);

    drawMiniChart(6,   34, 151, 83, 10, "ROSA ZEW", "C",
                  toneColor(stateTemperature(dewOut).tone));
    drawMiniChart(163, 34, 151, 83, 11, "ROSA WEW", "C",
                  toneColor(stateTemperature(dewIn).tone));
    drawMiniChart(6,  123, 151, 79, 12, "AH ZEW", "g/m3",
                  toneColor(stateHumidity(hOut).tone));
    drawMiniChart(163,123, 151, 79, 13, "AH WEW", "g/m3",
                  toneColor(stateHumidity(hIn).tone));

    float ahOut = absoluteHumidityGM3(tOut, hOut);
    float ahIn = absoluteHumidityGM3(tIn, hIn);
    float dAh = (isfinite(ahOut) && isfinite(ahIn)) ? (ahIn - ahOut) : NAN;

    String comfortInfo = ventilationAdvice(ahOut, ahIn);
    comfortInfo += "  dAH ";
    comfortInfo += fmtSigned(dAh, 1);
    comfortInfo += "g";

    tft.fillRect(112, 202, 208, 12, p.bg);
    tft.setTextColor(ventilationAdviceColor(ahOut, ahIn), p.bg);
    tft.setTextDatum(MR_DATUM);
    tft.setTextFont(1);
    tft.drawString(comfortInfo, 316, 208);
    tft.setTextDatum(TL_DATUM);

  } else if (chartPage == CHART_PAGE_TREND) {
    if (!showIndoor) {
      drawTrendCard(6,   34, 151, 81, "TEMP ZEW", 0, "C");
      drawTrendCard(163, 34, 151, 81, chartCompactLabel(2, false), 2, "hPa");
      drawTrendCard(6,  119, 151, 81, "WILG ZEW", 1, "%");
      drawTrendCard(163,119, 151, 81, "WIATR", 3, "m/s");
    } else {
      drawTrendCard(6,   34, 151, 81, "TEMP WEW", 8, "C");
      drawTrendCard(163, 34, 151, 81, "WILG WEW", 9, "%");
      drawTrendCard(6,  119, 151, 81, "ROSA WEW", 11, "C");
      drawTrendCard(163,119, 151, 81, "AH WEW", 13, "g/m3");
    }

  } else if (chartPage == CHART_PAGE_STATS) {
    // STAT ma dwa zestawy danych, przełączane istniejącym showIndoor.
    // Dotknięcie górnego nagłówka już zmienia showIndoor, więc nie dodajemy
    // kolejnego przycisku ani kolejnej pozycji do nawigacji PAGE.
    if (!showIndoor) {
      drawStatsCard(6,   34, 151, 81, "TEMP ZEW", 0, "C");
      drawStatsCard(163, 34, 151, 81, "WILG ZEW", 1, "%");
      drawStatsCard(6,  119, 151, 81, chartCompactLabel(2, false), 2, "hPa");
      drawWindStatsCard(163,119, 151, 81);
    } else {
      drawStatsCard(6,   34, 151, 81, "TEMP WEW", 8, "C");
      drawStatsCard(163, 34, 151, 81, "WILG WEW", 9, "%");
      drawStatsCard(6,  119, 151, 81, "ROSA WEW", 11, "C");
      drawStatsCard(163,119, 151, 81, "AH WEW", 13, "g/m3");
    }

  } else {
    drawAlertsPage(d);
  }

  if (deferChartLabels) {
    deferChartLabels = false;
    drawChartPageLabelsBatch();
  }

  if (preserveChartsFooterOnDraw) {
    preserveChartsFooterOnDraw = false;
    refreshChartsControlsOnly();
  } else {
    drawFooter();
  }
}


// ============================================================
// MAIN DRAW
// ============================================================


// ============================================================
// RC15.116 - lekki refresh skinow bez czyszczenia calego TFT
// ============================================================

void refreshMinimalLiveValues(const WeatherPacket &d) {
  float tempNow = showIndoor ? d.temp_wewnetrzna : d.temperatura;
  float humNow  = showIndoor ? d.wilg_wewnetrzna : d.wilgotnosc;

  refreshTileValueOnly(8, 36, 146, 86,
      fmt(tempNow, 1), "C", stateTemperature(tempNow), true);

  refreshTileValueOnly(162, 36, 150, 86,
      fmt(d.predkosc_wiatru, 1), "m/s",
      stateWind(d.predkosc_wiatru, false), true);

  refreshTileValueOnly(8, 130, 98, 78,
      fmt(humNow, 0), "%", stateHumidity(humNow), true);

  refreshTileValueOnly(111, 130, 98, 78,
      fmtPressure(d.cisnienie), "hPa", statePressure(d.cisnienie), true);

  refreshDirectionValueOnly(214, 130, 98, 78, d.kierunek_wiatru);

  VisualState uvst = stateUV(d.uv_index);
  uint16_t uvc = toneColor(uvst.tone);
  SkinPalette p = paletteFor(currentSkin);
  tft.fillRect(245, 187, 62, 13, p.panel);
  tft.setTextColor(uvc, p.panel);
  tft.drawRightString("UV " + fmt(d.uv_index, 1), 305, 190, 1);

  refreshHeaderStatusOnly();
  refreshSimpleFooterRxOnly();
}

void refreshInstrumentLiveValues(const WeatherPacket &d) {
  refreshTileValueOnly(8, 35, 148, 72,
      fmt(d.temperatura, 1), "C", stateTemperature(d.temperatura), true);

  refreshTileValueOnly(164, 35, 148, 72,
      fmtPressure(d.cisnienie), "hPa", statePressure(d.cisnienie), true);

  refreshGaugeValueOnly(12, 118, 296, 12,
      d.wilgotnosc, 0, 100, stateHumidity(d.wilgotnosc),
      fmt(d.wilgotnosc, 0) + "%");

  refreshGaugeValueOnly(12, 151, 296, 12,
      d.predkosc_wiatru, 0, 20, stateWind(d.predkosc_wiatru, false),
      fmt(d.predkosc_wiatru, 1) + " m/s");

  refreshGaugeValueOnly(12, 184, 296, 12,
      d.uv_index, 0, 11, stateUV(d.uv_index),
      fmt(d.uv_index, 1));

  refreshHeaderStatusOnly();
  refreshSimpleFooterRxOnly();
}

void refreshCurrentSkinLiveValues() {
  if (menuOpen || diagOpen || calibrationOpen || forceScreenClear)
    return;

  WeatherPacket d = snapshotData();

  switch (currentSkin) {
    case SKIN_GRID:
      refreshGridLiveValues(d);
      break;
    case SKIN_MINIMAL:
      refreshMinimalLiveValues(d);
      break;
    case SKIN_INSTRUMENT:
      refreshInstrumentLiveValues(d);
      break;
    case SKIN_FOCUS:
    case SKIN_CHARTS:
    default:
      break;
  }
}

void drawCurrentSkin() {
  markResetDiag(200);
  if (menuOpen)
    return;

  WeatherPacket d = snapshotData();

  switch (currentSkin) {
    case SKIN_DASH:
      drawDashboard(d);
      break;

    case SKIN_MINIMAL:
      drawMinimal(d);
      break;

    case SKIN_COMPASS:
      drawCompassSkin(d);
      break;

    case SKIN_INSTRUMENT:
      drawInstrumentSkin(d);
      break;

    case SKIN_FOCUS:
      drawFocusSkin(d);
      break;

    case SKIN_CHARTS:
      drawCharts(d);
      break;

    default:
      drawGrid(d);
      break;
  }
  forceScreenClear = false;
}



// ============================================================
// KALIBRACJA DOTYKU
// 5 punktow: LG, PG, S, LD, PD.
// Wynik zapisuje prostokat kalibracyjny do Preferences.
// ============================================================

void drawCalibrationTarget(int x, int y, const String &label) {
  SkinPalette p = paletteFor(currentSkin);
  tft.fillScreen(p.bg);

  tft.setTextColor(p.text, p.bg);
  tft.drawCentreString("KALIBRACJA DOTYKU", 160, 12, 2);
  tft.setTextColor(p.muted, p.bg);
  if (currentLanguage == LANG_PL)
    drawSafeI18nString("Dotknij krzyżyka", 160, 30, 2, p.muted, p.bg, MC_DATUM);
  else
    tft.drawCentreString("Touch the cross", 160, 38, 2);

  tft.drawLine(x - 12, y, x + 12, y, p.accent);
  tft.drawLine(x, y - 12, x, y + 12, p.accent);
  tft.drawCircle(x, y, 8, p.accent);

  tft.setTextColor(p.accent, p.bg);
  tft.drawCentreString(label, 160, 210, 2);
}

void startTouchCalibration() {
  calibrationOpen = true;
  menuOpen = false;
  diagOpen = false;
  calibrationStep = 0;

  calPoints[0] = {20, 20, 0, 0};
  calPoints[1] = {300, 20, 0, 0};
  calPoints[2] = {160, 120, 0, 0};
  calPoints[3] = {20, 220, 0, 0};
  calPoints[4] = {300, 220, 0, 0};

  drawCalibrationTarget(20, 20, "1/5");
}


bool solve3x3(float a[3][4], float out[3]) {
  for (int col = 0; col < 3; col++) {
    int pivot = col;
    float best = fabsf(a[col][col]);

    for (int row = col + 1; row < 3; row++) {
      float v = fabsf(a[row][col]);
      if (v > best) {
        best = v;
        pivot = row;
      }
    }

    if (best < 0.000001f)
      return false;

    if (pivot != col) {
      for (int k = col; k < 4; k++) {
        float tmp = a[col][k];
        a[col][k] = a[pivot][k];
        a[pivot][k] = tmp;
      }
    }

    float div = a[col][col];
    for (int k = col; k < 4; k++)
      a[col][k] /= div;

    for (int row = 0; row < 3; row++) {
      if (row == col)
        continue;

      float factor = a[row][col];
      for (int k = col; k < 4; k++)
        a[row][k] -= factor * a[col][k];
    }
  }

  out[0] = a[0][3];
  out[1] = a[1][3];
  out[2] = a[2][3];
  return true;
}

bool fitTouchAffine() {
  // Najmniejsze kwadraty dla 5 punktow:
  // screen = a*rawX + b*rawY + c
  float sXX = 0, sXY = 0, sYY = 0;
  float sX = 0, sY = 0;
  float sxX = 0, sxY = 0, sx1 = 0;
  float syX = 0, syY = 0, sy1 = 0;

  for (int i = 0; i < 5; i++) {
    float rx = (float)calPoints[i].rawX;
    float ry = (float)calPoints[i].rawY;
    float sx = (float)calPoints[i].screenX;
    float sy = (float)calPoints[i].screenY;

    sXX += rx * rx;
    sXY += rx * ry;
    sYY += ry * ry;
    sX  += rx;
    sY  += ry;

    sxX += rx * sx;
    sxY += ry * sx;
    sx1 += sx;

    syX += rx * sy;
    syY += ry * sy;
    sy1 += sy;
  }

  float mx[3][4] = {
    {sXX, sXY, sX, sxX},
    {sXY, sYY, sY, sxY},
    {sX,  sY,  5.0f, sx1}
  };

  float my[3][4] = {
    {sXX, sXY, sX, syX},
    {sXY, sYY, sY, syY},
    {sX,  sY,  5.0f, sy1}
  };

  float ox[3], oy[3];

  if (!solve3x3(mx, ox) || !solve3x3(my, oy))
    return false;

  touchAx = ox[0];
  touchBx = ox[1];
  touchCx = ox[2];

  touchAy = oy[0];
  touchBy = oy[1];
  touchCy = oy[2];

  return true;
}

void finishTouchCalibration() {
  bool ok = fitTouchAffine();

  if (ok) {
    // Sprawdzamy blad sredni na 5 punktach, aby nie zapisac przypadkowych dotkniec.
    float errSum = 0.0f;

    for (int i = 0; i < 5; i++) {
      float px = touchAx * calPoints[i].rawX +
                 touchBx * calPoints[i].rawY +
                 touchCx;

      float py = touchAy * calPoints[i].rawX +
                 touchBy * calPoints[i].rawY +
                 touchCy;

      float dx = px - calPoints[i].screenX;
      float dy = py - calPoints[i].screenY;
      errSum += sqrtf(dx * dx + dy * dy);
    }

    float avgErr = errSum / 5.0f;

    if (avgErr <= 35.0f) {
      saveTouchCalibration();
    } else {
      resetTouchCalibrationToDefault();
    }
  } else {
    resetTouchCalibrationToDefault();
  }

  calibrationOpen = false;
  menuOpen = true;
  drawMenu();
}

void serviceCalibrationTouch() {
#if HAS_TOUCH
  if (!calibrationOpen)
    return;

  static bool pressed = false;

  if (!touch.touched()) {
    pressed = false;
    return;
  }

  if (pressed)
    return;

  TS_Point p = touch.getPoint();
  pressed = true;

  if (calibrationStep < 5) {
    calPoints[calibrationStep].rawX = p.x;
    calPoints[calibrationStep].rawY = p.y;
    calibrationStep++;

    delay(120);

    if (calibrationStep >= 5) {
      finishTouchCalibration();
      return;
    }

    int x = calPoints[calibrationStep].screenX;
    int y = calPoints[calibrationStep].screenY;
    drawCalibrationTarget(x, y, String(calibrationStep + 1) + "/5");
  }
#endif
}

// ============================================================
// DIAGNOSTYKA
// Dostepna z menu przez przycisk DIAG w prawym gornym rogu.
// ============================================================

void drawDiagnosticsStatic() {
  diagOpen = true;
  menuOpen = false;
  diagForceValueRefresh = true;

  SkinPalette p = paletteFor(currentSkin);
  tft.fillScreen(p.bg);

  tft.fillRect(0, 0, 320, 30, p.panel2);
  tft.setTextColor(p.accent, p.panel2);
  drawLangButtonCentered("DIAGNOSTYKA", "DIAGNOSTYKA",
                         160, 15, p.accent, p.panel2, true);

  // Gorna sekcja - najwazniejsze informacje systemowe.
  tft.setTextColor(p.text, p.bg);
  drawLangLabel("RX / BAJTY:", "RX / BAJTY:", 10, 34, p.text, p.bg, false);
  drawLangLabel("WIEK:", "WIEK:", 10, 54, p.text, p.bg, false);
  drawLangLabel(currentLanguage == LANG_PL ? "ŹRÓDŁO:" : "SOURCE:",
                "ZRODLO:", 10, 74, p.text, p.bg, false);
  drawLangLabel("LOCAL:", "LOCAL:", 10, 94, p.text, p.bg, false);
  drawLangLabel("WI-FI:", "WI-FI:", 10, 114, p.text, p.bg, false);
  drawLangLabel("AIO:", "AIO:", 10, 134, p.text, p.bg, false);
  drawLangLabel("CZAS/NTP:", "CZAS/NTP:", 10, 154, p.text, p.bg, false);

  // Sekcja archiwum - celowo mniejszy font, ale osobne czytelne wiersze.
  tft.setTextFont(1);
  tft.setTextColor(p.muted, p.bg);
  tft.drawString("SD:",         10, 176);
  tft.drawString("ARCH:",       10, 187);
  if (currentLanguage == LANG_PL)
    drawSafeI18nString("GŁÓWNA:", 10, 198, 1, p.muted, p.bg, TL_DATUM);
  else
    tft.drawString("MAIN:",      10, 198);
  tft.drawString(currentLanguage == LANG_PL ? "WEW:" : "IN:", 10, 209);

  // CSV pokazujemy razem z WEW w prawej czesci ostatniego wiersza,
  // zeby zachowac duzy przycisk POWROT.
  tft.drawString("CSV:",        168, 209);

  // Sam fakt istnienia .vlw nie mówi, czy zawiera komplet polskich glifów.
  // Nie renderujemy tu dużej próbki. Status P-FONT informuje tylko o assetach.
  tft.setTextColor(p.muted, p.bg);
  tft.drawString("FONT UI:",     205, 198);

  // Staly przycisk powrotu.
  tft.fillRoundRect(8, 220, 304, 18, 6, p.accent);
  tft.setTextColor(TFT_BLACK, p.accent);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  if (currentLanguage == LANG_PL)
    drawLangButtonCentered("POWRÓT", "POWROT",
                           160, 229, TFT_BLACK, p.accent, false);
  else
    drawLangButtonCentered(tr(TXT_BACK), tr(TXT_BACK),
                           160, 229, TFT_BLACK, p.accent, false);
  tft.setTextDatum(TL_DATUM);
}

void refreshDiagnosticsValues() {
  if (!diagOpen)
    return;

  if (sdReady)
    sdUsedBytes = SD.usedBytes();

  SkinPalette p = paletteFor(currentSkin);
  uint32_t age = activeSourceAvailable() ? activeSourceAgeSec() : 0;

  // Cache tekstów: pole jest czyszczone i rysowane TYLKO wtedy,
  // gdy jego treść faktycznie się zmieniła.
  static String lastRx;
  static String lastAge;
  static String lastSource;
  static String lastLocal;
  static String lastWifi;
  static String lastAio;
  static String lastTime;
  static String lastSd;
  static String lastArch;
  static String lastHeap;
  static String lastHttpStack;
  static String lastMain;
  static String lastPFont;
  static String lastIndoor;
  static String lastCsv;

  if (diagForceValueRefresh) {
    lastRx = "";
    lastAge = "";
    lastSource = "";
    lastLocal = "";
    lastWifi = "";
    lastAio = "";
    lastTime = "";
    lastSd = "";
    lastArch = "";
    lastHeap = "";
    lastHttpStack = "";
    lastMain = "";
    lastPFont = "";
    lastIndoor = "";
    lastCsv = "";
    diagForceValueRefresh = false;
  }

  String rxText = String(activeSourcePacketCount()) + " / " + String(activeSourcePacketLen()) + "B";
  String ageText = activeSourceAvailable() ? String(age) + " s" : "--";
  String sourceText = dataSourceModeName() + " / " + activeDataSourceName();

  // RC15.179: cztery niezalezne etapy lancucha danych.
  String localText;
  if (haveData) {
    uint32_t localAge = (millis() - lastPacketMs) / 1000UL;
    localText = localSourceFresh() ? "OK " : "STALE ";
    localText += String(localAge);
    localText += "s CH";
    localText += String(activeRadioChannel);
  } else {
    localText = "BRAK";
  }

#if CYD_INTERNET_ENABLE
  String wifiText;
  if (internetConnected && WiFi.status() == WL_CONNECTED) {
    wifiText = (activeWifiProfile == WIFI_PROFILE_HOME) ? "HOME" :
               (activeWifiProfile == WIFI_PROFILE_REMOTE) ? "REMOTE" : "OK";
    wifiText += " CH";
    wifiText += String(internetChannel);
  } else {
    wifiText = "BRAK";
  }

  String aioText;
  if (!internetConnected || WiFi.status() != WL_CONNECTED) {
    aioText = "NO WIFI";
  } else if (haveInternetData) {
    uint32_t netAge = (millis() - lastInternetPacketMs) / 1000UL;
    aioText = "HTTP ";
    aioText += String(lastInternetHttpCode);
    aioText += " D:";
    aioText += String(netAge);
    aioText += "s";
  } else if (lastInternetHttpCode != 0) {
    aioText = "HTTP ";
    aioText += String(lastInternetHttpCode);
    if (lastInternetError.length()) {
      aioText += " ";
      aioText += lastInternetError;
    }
  } else {
    aioText = lastInternetError.length() ? lastInternetError : "WAIT";
  }
#else
  String wifiText = "OFF";
  String aioText = "OFF";
#endif

  String timeText = currentClockText();
  uint16_t ntpDiagColor = toneColor(TONE_WARNING);

  if (ntpReady && ntpSyncSeen) {
    uint32_t syncAgeSec = (uint32_t)(millis() - lastNtpSyncMs) / 1000UL;
    timeText += " NTP ";

    if (syncAgeSec < 60UL) {
      timeText += String(syncAgeSec);
      timeText += "s";
    } else if (syncAgeSec < 3600UL) {
      timeText += String(syncAgeSec / 60UL);
      timeText += "m";
    } else {
      timeText += String(syncAgeSec / 3600UL);
      timeText += "h";
    }

    // Zielony: synchronizacja nie starsza niz 2 domyslne okresy SNTP.
    // Starsza nadal oznacza prawidlowy zegar, ale DIAG ostrzega o wieku sync.
    ntpDiagColor = (syncAgeSec <= 7200UL) ?
                   toneColor(TONE_OK) : toneColor(TONE_WARNING);
  } else if (ntpReady) {
    timeText += " NTP WAIT";
  } else {
    timeText += " NTP --";
  }

  String sdText;
  String archText;
  String mainText;
  String pFontText;
  String indoorText;
  String csvText;

  uint16_t sdColor = p.muted;
  uint16_t archColor = p.muted;

  if (sdReady) {
    uint64_t freeBytes = (sdCardBytes > sdUsedBytes) ?
                         (sdCardBytes - sdUsedBytes) : 0;

    sdColor = chartSdPausedLowSpace ?
              toneColor(TONE_WARNING) : toneColor(TONE_OK);

    sdText = sdTypeName + "  FREE " + formatBytesShort(freeBytes);

    archColor = archiveIntegrityOK ?
                toneColor(TONE_OK) : toneColor(TONE_WARNING);

    archText = archiveIntegrityOK ? "OK" : "WARN";
    archText += " M:";
    archText += String(historyCheckBad);
    archText += " Q:";
    archText += String(historySequenceGaps);
    archText += " I:";
    archText += String(indoorCheckBad);
    archText += " C:";
    archText += csvTailOK ? "OK" : "BAD";
    archText += " S:";
    archText += (historyBundleWriteErrors == 0) ? "OK" : "ERR";

    mainText = String(historyCheckScanned) + " rec  ";
    mainText += formatBytesShort(chartHistoryFileBytes);

    pFontText = languageFontReady(currentLanguage, false) &&
                languageFontReady(currentLanguage, true) ? "OK" : "BRAK";

    uint32_t mainRecordsTotal =
        (uint32_t)(chartHistoryFileBytes / sizeof(ChartDiskRecord));
    uint32_t indoorRecordsTotal =
        (uint32_t)(indoorHistoryFileBytes / sizeof(IndoorDiskRecord));

    indoorText = String(indoorCheckScanned) + " rec ";
    indoorText += formatBytesShort(indoorHistoryFileBytes);

    if (mainRecordsTotal >= indoorRecordsTotal) {
      indoorText += " O-";
      indoorText += String(mainRecordsTotal - indoorRecordsTotal);
    } else {
      indoorText += " O+";
      indoorText += String(indoorRecordsTotal - mainRecordsTotal);
    }

    csvText = formatBytesShort(weatherCsvFileBytes);
  } else {
    sdColor = toneColor(TONE_DANGER);
    archColor = toneColor(TONE_DANGER);
    sdText = "BRAK / ERROR";
    archText = "WARN";
    mainText = "--";
    pFontText = languageFontReady(currentLanguage, false) &&
                languageFontReady(currentLanguage, true) ? "OK" : "BRAK";
    indoorText = "--";
    csvText = "--";
  }

  auto drawTopIfChanged = [&](String &last,
                              const String &now,
                              int y,
                              uint16_t color) {
    if (last == now)
      return;

    tft.fillRect(132, y, 180, 18, p.bg);
    tft.setTextColor(color, p.bg);
    tft.setTextFont(2);
    tft.drawRightString(now, 310, y, 2);
    last = now;
  };

  auto drawSmallRightIfChanged = [&](String &last,
                                     const String &now,
                                     int xClear,
                                     int y,
                                     int wClear,
                                     int xRight,
                                     uint16_t color) {
    if (last == now)
      return;

    tft.fillRect(xClear, y - 2, wClear, 12, p.bg);
    tft.setTextColor(color, p.bg);
    tft.setTextFont(1);
    tft.drawRightString(now, xRight, y, 1);
    last = now;
  };

  // RC15.238: heap watermark do dlugoterminowej diagnostyki fragmentacji RAM.
  // ESP.getMinFreeHeap() jest minimum zarejestrowanym przez allocator od startu.
  String heapText = "HEAP ";
  heapText += String(ESP.getFreeHeap() / 1024UL);
  heapText += "K / MIN ";
  heapText += String(ESP.getMinFreeHeap() / 1024UL);
  heapText += "K";

  String httpStackText = "HTTP STK --";
  uint32_t httpStackFreeBytes = 0;
#if CYD_INTERNET_ENABLE
  if (internetHttpTaskHandle != nullptr) {
    UBaseType_t hwmWords = uxTaskGetStackHighWaterMark(internetHttpTaskHandle);
    httpStackFreeBytes = (uint32_t)hwmWords * (uint32_t)sizeof(StackType_t);
    httpStackText = "HTTP STK ";
    if (httpStackFreeBytes >= 1024UL) {
      httpStackText += String(httpStackFreeBytes / 1024.0f, 1);
      httpStackText += "K";
    } else {
      httpStackText += String(httpStackFreeBytes);
      httpStackText += "B";
    }
  }
#endif

  drawTopIfChanged(lastRx, rxText, 34, p.text);
  drawTopIfChanged(lastAge, ageText, 54, p.text);
  drawTopIfChanged(lastSource, sourceText, 74, p.text);

  uint16_t localColor = !haveData ? toneColor(TONE_WARNING) :
                        localSourceFresh() ? toneColor(TONE_OK) :
                        toneColor(TONE_WARNING);
  drawTopIfChanged(lastLocal, localText, 94, localColor);

#if CYD_INTERNET_ENABLE
  uint16_t wifiColor =
      (internetConnected && WiFi.status() == WL_CONNECTED) ?
      toneColor(TONE_OK) : toneColor(TONE_WARNING);

  uint16_t aioColor =
      internetSourceFresh() ? toneColor(TONE_OK) :
      (internetConnected ? toneColor(TONE_WARNING) : p.muted);

  drawTopIfChanged(lastWifi, wifiText, 114, wifiColor);
  drawTopIfChanged(lastAio, aioText, 134, aioColor);
#else
  drawTopIfChanged(lastWifi, wifiText, 114, p.muted);
  drawTopIfChanged(lastAio, aioText, 134, p.muted);
#endif

  drawTopIfChanged(lastTime, timeText, 154, ntpDiagColor);

  drawSmallRightIfChanged(lastSd, sdText, 48, 176, 264, 310, sdColor);
  drawSmallRightIfChanged(lastArch, archText, 48, 187, 132, 180, archColor);
  drawSmallRightIfChanged(lastHttpStack, httpStackText, 205, 187, 107, 310,
                          (internetHttpTaskHandle == nullptr) ? p.muted :
                          (httpStackFreeBytes >= 2048UL) ? toneColor(TONE_OK) :
                          (httpStackFreeBytes >= 1024UL) ? toneColor(TONE_WARNING) :
                                                          toneColor(TONE_DANGER));
  // MAIN zostaje po lewej, P-FONT po prawej tego samego wiersza.
  drawSmallRightIfChanged(lastMain, mainText, 48, 198, 132, 180, p.text);
  drawSmallRightIfChanged(lastPFont, pFontText, 250, 198, 62, 310,
                          (languageFontReady(currentLanguage, false) &&
                           languageFontReady(currentLanguage, true)) ?
                          toneColor(TONE_OK) : toneColor(TONE_WARNING));
  drawSmallRightIfChanged(lastIndoor, indoorText, 48, 209, 112, 160, p.text);
  drawSmallRightIfChanged(lastHeap, heapText, 205, 197, 107, 310,
                          (ESP.getMinFreeHeap() >= 80000UL) ? toneColor(TONE_OK) :
                          (ESP.getMinFreeHeap() >= 50000UL) ? toneColor(TONE_WARNING) :
                                                             toneColor(TONE_DANGER));
  drawSmallRightIfChanged(lastCsv, csvText, 205, 209, 107, 310, p.text);
}

void drawDiagnostics() {
  drawDiagnosticsStatic();
  refreshDiagnosticsValues();
}

// ============================================================
// MENU
// ============================================================



String focusSelectButtonLabel() {
  if (currentLanguage == LANG_EN) return "SELECT";
  if (currentLanguage == LANG_DE) return "WAHL";
  if (currentLanguage == LANG_CZ) return "VÝBĚR";
  return "WYBÓR";
}

String focusChartButtonLabel() {
  if (currentLanguage == LANG_EN) return "CHART";
  if (currentLanguage == LANG_DE) return "DIAGR.";
  if (currentLanguage == LANG_CZ) return "GRAF";
  return "WYKRES";
}

String focusSettingsButtonLabel() {
  if (currentLanguage == LANG_EN) return "SETUP";
  if (currentLanguage == LANG_DE) return "EINST";
  if (currentLanguage == LANG_CZ) return "NAST";
  return "USTAW";
}

String windDirModeLabel() {
  if (currentLanguage == LANG_EN) return "WIND DIR";
  if (currentLanguage == LANG_DE) return "WINDRICHT.";
  if (currentLanguage == LANG_CZ) return "SMĚR VĚTRU";
  return "KIER. WIATRU";
}

String windDirModeValue() {
  if (windDirNotationMode == WIND_DIR_GLOBAL)
    return "GLOBAL";

  if (currentLanguage == LANG_EN) return "LOCAL";
  if (currentLanguage == LANG_DE) return "LOKAL";
  if (currentLanguage == LANG_CZ) return "LOKAL";
  return "LOKALNY";
}

String skinNameMenu() {
  if (currentLanguage == LANG_EN) {
    if (currentSkin == SKIN_DASH) return "DASH";
    if (currentSkin == SKIN_MINIMAL) return "MINIMAL";
    if (currentSkin == SKIN_COMPASS) return "COMPASS";
    if (currentSkin == SKIN_INSTRUMENT) return "GAUGES";
    if (currentSkin == SKIN_FOCUS) return "FOCUS";
    if (currentSkin == SKIN_CHARTS) return "CHARTS";
    return "GRID";
  }
  if (currentLanguage == LANG_DE) {
    if (currentSkin == SKIN_DASH) return "PANEL";
    if (currentSkin == SKIN_MINIMAL) return "MINIMAL";
    if (currentSkin == SKIN_COMPASS) return "WINDROSE";
    if (currentSkin == SKIN_INSTRUMENT) return "INSTR.";
    if (currentSkin == SKIN_FOCUS) return "FOKUS";
    if (currentSkin == SKIN_CHARTS) return "DIAGRAMM";
    return "RASTER";
  }
  if (currentLanguage == LANG_CZ) {
    if (currentSkin == SKIN_DASH) return "PANEL";
    if (currentSkin == SKIN_MINIMAL) return "MINIMAL";
    if (currentSkin == SKIN_COMPASS) return "RUZICE";
    if (currentSkin == SKIN_INSTRUMENT) return "PRISTROJE";
    if (currentSkin == SKIN_FOCUS) return "FOKUS";
    if (currentSkin == SKIN_CHARTS) return "GRAFY";
    return "MRIZKA";
  }
  if (currentSkin == SKIN_DASH) return "PANEL";
  if (currentSkin == SKIN_MINIMAL) return "MINIMAL";
  if (currentSkin == SKIN_COMPASS) return "KOMPAS";
  if (currentSkin == SKIN_INSTRUMENT) return "WSKAŹNIKI";
  if (currentSkin == SKIN_FOCUS) return "FOCUS";
  if (currentSkin == SKIN_CHARTS) return "WYKRESY";
  return "SIATKA";
}

String menuRotateLabel() {
  switch (currentLanguage) {
    case LANG_EN: return "TIME";
    case LANG_DE: return "ZEIT";
    case LANG_CZ: return "CAS";
    default:      return "CZAS";
  }
}

String menuViewNowLabel() {
  switch (currentLanguage) {
    case LANG_EN: return "VIEW";
    case LANG_DE: return "JETZT";
    case LANG_CZ: return "NYNI";
    default:      return "TERAZ";
  }
}

String menuLanguageLabel() {
  switch (currentLanguage) {
    case LANG_EN: return "LANG";
    case LANG_DE: return "SPR.";
    case LANG_CZ: return "JAZYK";
    default:      return "JĘZYK";
  }
}

String menuSourceLabel() {
  switch (currentLanguage) {
    case LANG_EN: return "SRC";
    case LANG_DE: return "QUELLE";
    case LANG_CZ: return "ZDROJ";
    default:      return "ŹRÓDŁO";
  }
}

String menuSourceValue() {
  if (dataSourceMode == SOURCE_AUTO)
    return "AUTO";

  if (dataSourceMode == SOURCE_INTERNET)
    return "NET";

  if (currentLanguage == LANG_PL)
    return "LOK";
  if (currentLanguage == LANG_DE || currentLanguage == LANG_CZ)
    return "LOK";

  return "LOCAL";
}


String menuOnOff(bool on) {
  if (currentLanguage == LANG_PL)
    return on ? "WŁ" : "WYŁ";

  if (currentLanguage == LANG_CZ)
    return on ? "ZAP" : "VYP";

  if (currentLanguage == LANG_DE)
    return on ? "EIN" : "AUS";

  return on ? "ON" : "OFF";
}

String menuViewNowValue() {
  if (currentLanguage == LANG_PL)
    return showIndoor ? "WEW" : "ZEW";

  if (currentLanguage == LANG_CZ)
    return showIndoor ? "UVN" : "VEN";

  if (currentLanguage == LANG_DE)
    return showIndoor ? "IN" : "AUS";

  return showIndoor ? "IN" : "OUT";
}


String skinName() {
  // Nazwy ASCII-safe. Polskie znaki dodamy dopiero po ujednoliceniu
  // smooth-fontow, aby nie wprowadzac kwadratow/brakujacych glifow.
  if (currentSkin == SKIN_DASH) return "PANEL";
  if (currentSkin == SKIN_MINIMAL) return "MINIMAL";
  if (currentSkin == SKIN_COMPASS) return "KOMPAS";
  if (currentSkin == SKIN_INSTRUMENT) return "WSKAŹNIKI";
  if (currentSkin == SKIN_FOCUS) return "FOCUS";
  if (currentSkin == SKIN_CHARTS) return "WYKRESY";
  return "SIATKA";
}

String alertName() {
  if (currentLanguage == LANG_EN) {
    if (alertStyle == ALERT_VALUE) return "VALUE";
    if (alertStyle == ALERT_PANEL) return "PANEL";
    return "BORDER";
  }

  if (currentLanguage == LANG_DE) {
    if (alertStyle == ALERT_VALUE) return "WERT";
    if (alertStyle == ALERT_PANEL) return "PANEL";
    return "RAHMEN";
  }

  if (currentLanguage == LANG_CZ) {
    if (alertStyle == ALERT_VALUE) return "HODNOTA";
    if (alertStyle == ALERT_PANEL) return "PANEL";
    return "RÁMEČEK";
  }

  if (alertStyle == ALERT_VALUE) return "KOLOR";
  if (alertStyle == ALERT_PANEL) return "PANEL";
  return "RAMKA";
}


void drawMenuButtonLoadedFont(int x, int y, int w, int h,
                              const String &label, const String &value,
                              bool smoothLoaded) {
  // RC15.133: interaktywny kafelek MENU nie korzysta z pozyczonego
  // smooth fontu. Backtrace RC15.132 pokazal drawGlyph -> File::read.
  (void)smoothLoaded;

  SkinPalette p = paletteFor(currentSkin);
  tft.fillRoundRect(x, y, w, h, 7, p.panel);
  tft.drawRoundRect(x, y, w, h, 7, p.border);

  String a = label;
  String b = value;

  a.replace("Ą","A"); a.replace("ą","a");
  a.replace("Ć","C"); a.replace("ć","c");
  a.replace("Ę","E"); a.replace("ę","e");
  a.replace("Ł","L"); a.replace("ł","l");
  a.replace("Ń","N"); a.replace("ń","n");
  a.replace("Ó","O"); a.replace("ó","o");
  a.replace("Ś","S"); a.replace("ś","s");
  a.replace("Ź","Z"); a.replace("ź","z");
  a.replace("Ż","Z"); a.replace("ż","z");

  b.replace("Ą","A"); b.replace("ą","a");
  b.replace("Ć","C"); b.replace("ć","c");
  b.replace("Ę","E"); b.replace("ę","e");
  b.replace("Ł","L"); b.replace("ł","l");
  b.replace("Ń","N"); b.replace("ń","n");
  b.replace("Ó","O"); b.replace("ó","o");
  b.replace("Ś","S"); b.replace("ś","s");
  b.replace("Ź","Z"); b.replace("ź","z");
  b.replace("Ż","Z"); b.replace("ż","z");

  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(p.muted, p.panel);
  tft.drawString(a, x + 7, y + 5, 1);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(p.accent, p.panel);
  tft.drawString(b, x + w / 2, y + 30, 1);
  tft.setTextDatum(TL_DATUM);
}

void drawMenuButton(
  int x, int y, int w, int h,
  const String &label,
  const String &value
) {
  SkinPalette p = paletteFor(currentSkin);

  tft.fillRoundRect(x, y, w, h, 7, p.panel);
  tft.drawRoundRect(x, y, w, h, 7, p.border);

  // Małe etykiety i wartości menu korzystają z fontu aktualnego języka.
  drawLangLabel(label, label, x + 8, y + 5, p.muted, p.panel, false);

  drawLangButtonCentered(value, value,
                         x + w / 2, y + 30,
                         p.accent, p.panel, false);
}


void drawMenuLocalizedSmallCenteredLoaded(const String &utf8,
                                          const String &fallbackAscii,
                                          int cx, int cy,
                                          uint16_t fg, uint16_t bg,
                                          bool smoothLoaded) {
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);

  if (smoothLoaded)
    tft.drawString(utf8, cx, cy);
  else
    tft.drawCentreString(fallbackAscii, cx, cy - 4, 1);

  tft.setTextDatum(TL_DATUM);
}

void drawMenu() {
  menuOpen = true;
  closeMenuAfterTouchRelease = false;

  SkinPalette p = paletteFor(currentSkin);
  tft.fillScreen(p.bg);

  // Naglowek
  tft.fillRect(0, 0, 320, 30, p.panel2);

  if (settingsMenuPage == 0)
    drawLangHeader(tr(TXT_SETTINGS), "USTAWIENIA CYD",
                   6, 6, p.accent, p.panel2);
  else
    drawLangHeader(currentLanguage == LANG_EN ? "SETTINGS 2/2" :
                   currentLanguage == LANG_DE ? "EINSTELL. 2/2" :
                   currentLanguage == LANG_CZ ? "NASTAVENI 2/2" :
                                                "USTAWIENIA 2/2",
                   "USTAWIENIA 2/2",
                   6, 6, p.accent, p.panel2);

#if HAS_TOUCH
  tft.fillRoundRect(220, 3, 44, 23, 5, p.panel);
  tft.drawRoundRect(220, 3, 44, 23, 5, p.border);
  tft.fillRoundRect(272, 3, 44, 23, 5, p.panel);
  tft.drawRoundRect(272, 3, 44, 23, 5, p.border);

  tft.setTextFont(1);
  tft.setTextColor(p.accent, p.panel);
  tft.drawCentreString("CAL", 242, 9, 1);
  tft.drawCentreString("DIAG", 294, 9, 1);
#else
  tft.setTextColor(TFT_ORANGE, p.panel2);
  tft.drawRightString("NO TOUCH", 313, 7, 1);
#endif

  if (settingsMenuPage == 0) {
    // Strona 1/2 - dotychczasowe MENU bez zmiany geometrii.
    drawMenuButton(10, 40, 145, 48, tr(TXT_SKIN), skinNameMenu());
    drawMenuButton(165, 40, 145, 48, tr(TXT_ALERTS), alertName());
    drawMenuButton(10, 96, 145, 48, tr(TXT_BRIGHTNESS),
                   String(brightnessLevel) + "%");
    drawMenuButton(165, 96, 145, 48, tr(TXT_AUTO_INOUT),
                   menuOnOff(autoIndoorOutdoor));
    drawMenuButton(8, 152, 72, 44, menuRotateLabel(),
                   String(rotateSeconds) + "s");
    drawMenuButton(84, 152, 72, 44, menuViewNowLabel(),
                   menuViewNowValue());
    drawMenuButton(160, 152, 72, 44, menuLanguageLabel(),
                   languageName());
    drawMenuButton(236, 152, 76, 44, menuSourceLabel(),
                   menuSourceValue());
  } else {
    // RC15.255: Strona 2/2 - trzy jednoznaczne ustawienia.
    drawMenuButton(10, 38, 300, 44,
                   currentLanguage == LANG_EN ? "RGB STATUS" :
                   currentLanguage == LANG_DE ? "RGB STATUS" :
                   currentLanguage == LANG_CZ ? "STAV RGB" : "STATUS RGB",
                   menuOnOff(rgbEnabled));

    drawMenuButton(10, 88, 300, 44,
                   currentLanguage == LANG_EN ? "STALE / OFFLINE FRAME" :
                   currentLanguage == LANG_DE ? "ALT / OFFLINE RAHMEN" :
                   currentLanguage == LANG_CZ ? "RÁMEČEK STAVU" :
                                                "RAMKA NIEAKT / OFFLINE",
                   menuOnOff(freshnessFrameEnabled));

    drawMenuButton(10, 138, 300, 44,
                   windDirModeLabel(),
                   windDirModeValue());

    tft.setTextFont(1);
    tft.setTextColor(p.muted, p.bg);
    tft.setTextDatum(MC_DATUM);
    String info2 = currentLanguage == LANG_EN ? "GLOBAL = N/E/S/W standard" :
                   currentLanguage == LANG_DE ? "GLOBAL = N/E/S/W Standard" :
                   currentLanguage == LANG_CZ ? "GLOBAL = standard N/E/S/W" :
                                                "GLOBAL = standard N/E/S/W";
    tft.drawString(info2, 160, 190);
    tft.setTextDatum(TL_DATUM);
  }

  // Dolny pasek: POWROT + przelacznik stron.
  tft.fillRoundRect(8, 202, 228, 34, 7, p.accent);
  drawLangButtonCentered(tr(TXT_BACK), "BACK",
                         122, 219, TFT_BLACK, p.accent, true);

  tft.fillRoundRect(244, 202, 68, 34, 7, p.panel);
  tft.drawRoundRect(244, 202, 68, 34, 7, p.border);
  tft.setTextColor(p.accent, p.panel);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  tft.drawString(settingsMenuPage == 0 ? "2/2 >" : "< 1/2", 278, 219);
  tft.setTextDatum(TL_DATUM);

  unloadSmoothFontSafe();
  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
}

void nextBrightness() {
  if (brightnessLevel == 100) brightnessLevel = 70;
  else if (brightnessLevel == 70) brightnessLevel = 40;
  else if (brightnessLevel == 40) brightnessLevel = 20;
  else brightnessLevel = 100;

  applyBrightness();
}

void nextRotate() {
  if (rotateSeconds == 8) rotateSeconds = 12;
  else if (rotateSeconds == 12) rotateSeconds = 20;
  else rotateSeconds = 8;
}

// ============================================================
// TOUCH
// ============================================================

#if HAS_TOUCH
bool readTouch(int &x, int &y) {
  // RC15.127: wieloprobkowy filtr medianowy XPT2046.
  // Jedno dotkniecie -> do 5 szybkich probek RAW.
  // Nie czekamy na puszczenie palca i nie dodajemy dlugich delay().
  if (!touch.touched())
    return false;

  static const uint8_t NMAX = 7;
  int16_t xs[NMAX];
  int16_t ys[NMAX];
  int16_t zs[NMAX];
  uint8_t n = 0;

  for (uint8_t i = 0; i < NMAX; ++i) {
    if (!touch.touched())
      break;

    TS_Point p = touch.getPoint();

    if (p.z >= 120) {
      xs[n] = p.x;
      ys[n] = p.y;
      zs[n] = p.z;
      ++n;
    }

    // RC15.129: 2 ms pomiedzy probkami; do ok. 12 ms na tap.
    // Nadal wymagamy minimum 3 poprawnych probek i uzywamy mediany.
    if (i + 1 < NMAX)
      delay(2);
  }

  // Minimum 3 prawidlowe probki, inaczej dotkniecie bylo zbyt krotkie/slabe.
  if (n < 3)
    return false;

  // Mala tablica -> prosty insertion sort, bez alokacji dynamicznej.
  for (uint8_t i = 1; i < n; ++i) {
    int16_t vx = xs[i];
    int j = i - 1;
    while (j >= 0 && xs[j] > vx) {
      xs[j + 1] = xs[j];
      --j;
    }
    xs[j + 1] = vx;
  }

  for (uint8_t i = 1; i < n; ++i) {
    int16_t vy = ys[i];
    int j = i - 1;
    while (j >= 0 && ys[j] > vy) {
      ys[j + 1] = ys[j];
      --j;
    }
    ys[j + 1] = vy;
  }

  for (uint8_t i = 1; i < n; ++i) {
    int16_t vz = zs[i];
    int j = i - 1;
    while (j >= 0 && zs[j] > vz) {
      zs[j + 1] = zs[j];
      --j;
    }
    zs[j + 1] = vz;
  }

  const int16_t rawX = xs[n / 2];
  const int16_t rawY = ys[n / 2];
  const int16_t rawZ = zs[n / 2];
  const int16_t spreadX = xs[n - 1] - xs[0];
  const int16_t spreadY = ys[n - 1] - ys[0];

  // RC15.137 TOUCH MATRIX35 AFFINE:
  // model policzony z pelnej macierzy 35 punktow tego konkretnego CYD.
  // Stara kalibracja 5-punktowa mocno przesuwala X w prawo przy dolnej
  // krawedzi (np. ekran ~206 px byl liczony jako ~280 px), przez co
  // dotkniecie WYBOR trafialo do strefy MENU.
  //
  // AFFINE z testu 35 pkt:
  // X = 0.108954292993*rawX + 0.016543370981*rawY - 86.669129334
  // Y = 0.015956279825*rawX + 0.081252662921*rawY - 80.925092672
  const float sx = 0.108954292993f * (float)rawX
                 + 0.016543370981f * (float)rawY
                 - 86.669129334f;

  const float sy = 0.015956279825f * (float)rawX
                 + 0.081252662921f * (float)rawY
                 - 80.925092672f;

  x = constrain((int)lroundf(sx), 0, SCREEN_W - 1);
  y = constrain((int)lroundf(sy), 0, SCREEN_H - 1);

#if CYD_DEBUG
  Serial.print("[TOUCH MED] n=");
  Serial.print(n);
  Serial.print(" rawX=");
  Serial.print(rawX);
  Serial.print(" rawY=");
  Serial.print(rawY);
  Serial.print(" z=");
  Serial.print(rawZ);
  Serial.print(" spreadX=");
  Serial.print(spreadX);
  Serial.print(" spreadY=");
  Serial.print(spreadY);
  Serial.print(" -> x=");
  Serial.print(x);
  Serial.print(" y=");
  Serial.println(y);
#endif

  return true;
}
#endif

bool inside(int x, int y, int bx, int by, int bw, int bh) {
  return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

void handleMenuTouch(int x, int y) {
  bool changed = false;

  // CAL / DIAG dostepne na obu stronach.
  if (inside(x, y, 220, 3, 44, 23)) {
    startTouchCalibration();
    waitTouchRelease = true;
    touchActionBlockUntilMs = millis() + 40UL;
    return;
  }

  if (inside(x, y, 272, 3, 44, 23)) {
    drawDiagnostics();
    waitTouchRelease = true;
    touchActionBlockUntilMs = millis() + 40UL;
    return;
  }

  // Przelacznik strony MENU ma wlasny hitbox i jest sprawdzany przed POWROT.
  if (inside(x, y, 240, 198, 76, 42)) {
    settingsMenuPage = settingsMenuPage ? 0 : 1;
    drawMenu();
    waitTouchRelease = true;
    touchActionBlockUntilMs = millis() + 40UL;
    return;
  }

  if (settingsMenuPage == 1) {
    if (inside(x, y, 10, 38, 300, 44)) {
      rgbEnabled = !rgbEnabled;
      settingsDirty = true;

      // Natychmiastowy efekt OFF/ON bez czekania na wyjscie z MENU.
      if (!rgbEnabled)
        rgbLedWrite(false, false, false);
      else
        serviceStatusLed();

      drawMenu();
      waitTouchRelease = true;
      touchActionBlockUntilMs = millis() + 40UL;
      return;
    }

    if (inside(x, y, 10, 88, 300, 44)) {
      freshnessFrameEnabled = !freshnessFrameEnabled;
      settingsDirty = true;
      drawMenu();
      waitTouchRelease = true;
      touchActionBlockUntilMs = millis() + 40UL;
      return;
    }

    if (inside(x, y, 10, 138, 300, 44)) {
      windDirNotationMode = (windDirNotationMode == WIND_DIR_GLOBAL)
                              ? WIND_DIR_LOCAL
                              : WIND_DIR_GLOBAL;
      settingsDirty = true;
      forceScreenClear = true;
      drawMenu();
      waitTouchRelease = true;
      touchActionBlockUntilMs = millis() + 40UL;
      return;
    }

    if (inside(x, y, 0, 198, 240, 42)) {
      if (settingsDirty) {
        saveSettings();
        settingsDirty = false;
      }

      settingsMenuPage = 0;
      closeMenuAfterTouchRelease = true;
      waitTouchRelease = true;
      redrawAfterTouchRelease = true;
      touchActionBlockUntilMs = millis() + 40UL;
      return;
    }

    return;
  }

  // ===== STRONA 1/2 =====
  if (inside(x, y, 10, 40, 145, 48)) {
    currentSkin++;
    if (currentSkin > SKIN_CHARTS)
      currentSkin = SKIN_GRID;

    forceScreenClear = true;
    settingsDirty = true;
    lastIndoorToggle = millis();

    drawMenu();
    waitTouchRelease = true;
    touchActionBlockUntilMs = millis() + 40UL;
    return;
  }
  else if (inside(x, y, 165, 40, 145, 48)) {
    alertStyle++;
    if (alertStyle > ALERT_PANEL)
      alertStyle = ALERT_VALUE;
    changed = true;
  }
  else if (inside(x, y, 10, 96, 145, 48)) {
    nextBrightness();
    changed = true;
  }
  else if (inside(x, y, 165, 96, 145, 48)) {
    autoIndoorOutdoor = !autoIndoorOutdoor;
    changed = true;
  }
  else if (inside(x, y, 8, 152, 72, 44)) {
    nextRotate();
    changed = true;
  }
  else if (inside(x, y, 84, 152, 72, 44)) {
    showIndoor = !showIndoor;
    changed = true;
  }
  else if (inside(x, y, 160, 152, 72, 44)) {
    currentLanguage++;
    if (currentLanguage > LANG_CZ)
      currentLanguage = LANG_PL;

    settingsDirty = true;
    drawMenu();
    waitTouchRelease = true;
    touchActionBlockUntilMs = millis() + 40UL;
    return;
  }
  else if (inside(x, y, 236, 152, 76, 44)) {
    dataSourceMode++;
    if (dataSourceMode > SOURCE_INTERNET)
      dataSourceMode = SOURCE_AUTO;
    updateActiveDataSource();
    changed = true;
  }
  else if (inside(x, y, 0, 198, 240, 42)) {
    if (settingsDirty) {
      saveSettings();
      settingsDirty = false;
    }

    settingsMenuPage = 0;
    closeMenuAfterTouchRelease = true;
    waitTouchRelease = true;
    redrawAfterTouchRelease = true;
    touchActionBlockUntilMs = millis() + 40UL;
    return;
  }

  if (changed) {
    settingsDirty = true;
    drawMenu();
    touchActionBlockUntilMs = millis() + 180UL;
  }
}




void serviceTouch() {
#if HAS_TOUCH
  static unsigned long lastTouchMs = 0;
  static unsigned long lastTouchDiagMs = 0;

#if CYD_DEBUG
  if (millis() - lastTouchDiagMs >= 1000UL) {
    lastTouchDiagMs = millis();
    Serial.print("[TOUCH IRQ] ");
    Serial.println(digitalRead(TOUCH_IRQ));
  }
#endif

  if (millis() - lastTouchMs < 180)
    return;

  if ((long)(touchActionBlockUntilMs - millis()) > 0)
    return;

  int x, y;

  if (waitTouchRelease) {
    if (!touch.touched()) {
      waitTouchRelease = false;
      touchActionBlockUntilMs = millis() + 40UL;

      if (chartSettingsSavePending) {
        chartSettingsSavePending = false;
        prefs.begin("cyd_ui", false);
        prefs.putUChar("chart_pg", chartPage);
        prefs.putUChar("chart_rng", chartRange);
        prefs.end();
      }

      if (closeMenuAfterTouchRelease) {
        closeMenuAfterTouchRelease = false;
        menuOpen = false;
      }

      if (redrawAfterTouchRelease) {
        redrawAfterTouchRelease = false;

        // RC15.126: overlay (MENU/FOCUS SELECT) znika jednym ruchem.
        // Nie zostawiamy dolnej czesci starego POWROT do konca redraw.
        SkinPalette exitP = paletteFor(currentSkin);
        tft.fillScreen(exitP.bg);

        forceScreenClear = true;
        drawCurrentSkin();

        // RC15.130A: ekran docelowy zostal juz narysowany w tej iteracji.
        uiTransitionRedrawnThisLoop = true;

        // serviceFocusProgress ma lokalny cache numeru metryki.
        // Po powrocie tylko go zsynchronizujemy, bez drugiego redraw.
        if (currentSkin == SKIN_FOCUS)
          focusProgressSyncPending = true;
      }
    }
    return;
  }

  if (!readTouch(x, y))
    return;

  lastTouchMs = millis();
  markResetDiag(100, x, y);

#if CYD_DEBUG
  Serial.print("[TOUCH] x=");
  Serial.print(x);
  Serial.print(" y=");
  Serial.println(y);
#endif

  if (focusChartOpen) {
    markResetDiag(150, x, y);
    handleFocusBigChartTouch(x, y);
    return;
  }

  if (focusConfigOpen) {
    markResetDiag(110, x, y);
    handleFocusConfigTouch(x, y);
    return;
  }

  // RC15.140: FOCUS footer - WYBOR / WYKRES / MENU.
  if (currentSkin == SKIN_FOCUS &&
      !menuOpen && !diagOpen && !calibrationOpen && !focusConfigOpen && !focusChartOpen &&
      x >= 108 && x <= 177 &&
      y >= 212 && y <= 239) {
    markResetDiag(120, x, y);
    drawFocusConfig();
    waitTouchRelease = true;
    touchActionBlockUntilMs = millis() + 80UL;
    return;
  }

  if (currentSkin == SKIN_FOCUS &&
      !menuOpen && !diagOpen && !calibrationOpen && !focusConfigOpen && !focusChartOpen &&
      // Widoczny WYKRES: x=178..245. Dajemy tylko mały margines dotyku,
      // bez wchodzenia w lewą część przycisku USTAW.
      x >= 176 && x <= 247 &&
      y >= 212 && y <= 239) {
    markResetDiag(150, x, y);
    openFocusBigChart();
    return;
  }

if (diagOpen) {
    markResetDiag(140, x, y);
    // RC15.138: POWROT DIAG zgodny z widoczna ramka (8,220,304,18)
    // z niewielkim marginesem dotyku.
    if (inside(x, y, 4, 216, 312, 24)) {
      diagOpen = false;
      menuOpen = true;
      waitTouchRelease = true;
      redrawAfterTouchRelease = false;
      drawMenu();
      touchActionBlockUntilMs = millis() + 40UL;
    }
    return;
  }

  if (menuOpen) {
    markResetDiag(130, x, y);
    handleMenuTouch(x, y);
    return;
  }

  // RC15.138: WYKRESY po kalibracji MATRIX35.
  // Hitboxy odpowiadaja widocznym ramkom + niewielki margines:
  // PAGE 0..86 | RANGE 108..177 | INOUT 178..237 | MENU 252..319.
  // Martwe strefy zapobiegaja przypadkowemu przechodzeniu miedzy kontrolkami.
  // Y=212..239 pozostaje tolerancyjne dla dolnej krawedzi panelu.

  // WYKRESY: lewy przycisk PAGE.
  if (currentSkin == SKIN_CHARTS &&
      x >= 0 && x <= 86 &&
      y >= 212 && y <= 239) {
    nextChartPage();

    chartSettingsSavePending = true;

    preserveChartsFooterOnDraw = true;
    drawCurrentSkin();
    waitTouchRelease = true;
    touchActionBlockUntilMs = millis() + 40UL;
    return;
  }

  // WYKRESY: precyzyjna strefa tylko wokol napisu 1H/6H/24H/7D.
  // Celowo zostawiamy martwa przestrzen miedzy zakresem i MENU.
  if (currentSkin == SKIN_CHARTS &&
      x >= 108 && x <= 177 &&
      y >= 212 && y <= 239) {
    nextChartRange();

    chartSettingsSavePending = true;

    preserveChartsFooterOnDraw = true;
    drawCurrentSkin();
    waitTouchRelease = true;
    touchActionBlockUntilMs = millis() + 40UL;
    return;
  }

  // TREND/STAT: kontekstowy przycisk ZEW/WEW w dolnym footerze.
  if (currentSkin == SKIN_CHARTS &&
      (chartPage == CHART_PAGE_TREND || chartPage == CHART_PAGE_STATS) &&
      x >= 178 && x <= 237 &&
      y >= 212 && y <= 239) {

    showIndoor = !showIndoor;
    lastIndoorToggle = millis();

    if (chartPage == CHART_PAGE_TREND)
      redrawTrendPageOnly();
    else
      redrawStatsPageOnly();

    refreshChartsInOutControlOnly();
    waitTouchRelease = true;
    touchActionBlockUntilMs = millis() + 40UL;
    return;
  }

  // MENU na WYKRESACH ma hitbox zgodny z nowa ramka.
  if (currentSkin == SKIN_CHARTS &&
      x >= 252 && x <= 319 &&
      y >= 212 && y <= 239) {
    drawMenu();
    touchActionBlockUntilMs = millis() + 250UL;
    return;
  }

  // FOCUS: USTAW ma osobną prawą strefę zgodną z widoczną ramką x=250..317.
  // x=248..249 pełni rolę bezpiecznego marginesu pomiędzy WYKRES i USTAW.
  if (currentSkin == SKIN_FOCUS &&
      x >= 248 && x <= 319 &&
      y >= 212 && y <= 239) {
    drawMenu();
    touchActionBlockUntilMs = millis() + 250UL;
    return;
  }

  // Pozostale skiny: precyzyjna strefa MENU.
  if (currentSkin != SKIN_CHARTS && currentSkin != SKIN_FOCUS &&
      x >= 252 && x <= 319 &&
      y >= 212 && y <= 239) {
    drawMenu();
    touchActionBlockUntilMs = millis() + 250UL;
    return;
  }



  // Szybki toggle nagłówkiem tylko na ekranach, gdzie WEW/ZEW
  // faktycznie zmienia prezentowane dane.
  if ((currentSkin == SKIN_GRID || currentSkin == SKIN_MINIMAL) && y < 32) {
    showIndoor = !showIndoor;
    lastIndoorToggle = millis();

    if (currentSkin == SKIN_GRID) {
      WeatherPacket d = snapshotData();
      refreshGridInOutTiles(d);
    } else {
      drawCurrentSkin();
    }

    touchActionBlockUntilMs = millis() + 250UL;
  }
#endif
}

// ============================================================
// SETUP
// ============================================================


// ============================================================
// RC15.116 - ekran startowy bez zaleznosci od SD
// ============================================================
void drawBootScreen(uint8_t pct, const char *stage) {
  SkinPalette p = paletteFor(currentSkin);

  // RC15.118: kazdy etap odtwarza komplet ekranu startowego.
  tft.fillScreen(p.bg);

  tft.drawRoundRect(129, 34, 62, 54, 8, p.border);
  tft.fillCircle(160, 54, 8, p.accent);
  tft.drawLine(160, 62, 160, 104, p.accent);
  tft.drawLine(145, 104, 175, 104, p.accent);
  tft.drawLine(160, 74, 184, 66, p.muted);
  tft.drawLine(184, 66, 178, 62, p.muted);
  tft.drawLine(184, 66, 179, 72, p.muted);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(p.accent, p.bg);
  tft.drawString("CYD WEATHER UI PRO", 160, 124, 2);

  tft.setTextColor(p.muted, p.bg);
  tft.drawString(stage ? stage : "", 160, 158, 1);

  tft.drawRoundRect(28, 184, 264, 14, 5, p.border);
  int fillW = ((int)pct * 258) / 100;
  if (fillW < 0) fillW = 0;
  if (fillW > 258) fillW = 258;
  if (fillW > 0)
    tft.fillRoundRect(31, 187, fillW, 8, 3, p.accent);

  tft.setTextColor(p.text, p.bg);
  tft.drawString(String((int)pct) + "%", 160, 216, 1);
  tft.setTextDatum(TL_DATUM);
}

void setup() {
  Serial.begin(115200);
  delay(150);
  ++rtcBootCount;
  esp_reset_reason_t rr131 = esp_reset_reason();
  Serial.println();
  Serial.println("===== RC15.131 RESET DIAG =====");
  Serial.print("RESET=");
  Serial.print(resetReasonText(rr131));
  Serial.print(" code=");
  Serial.println((int)rr131);
  Serial.print("BOOT=");
  Serial.println(rtcBootCount);
  Serial.print("LAST_MARK=");
  Serial.print(rtcLastMark);
  Serial.print(" TOUCH=");
  Serial.print(rtcLastTouchX);
  Serial.print(",");
  Serial.print(rtcLastTouchY);
  Serial.print(" SKIN=");
  Serial.print(rtcLastSkin);
  Serial.print(" MENU=");
  Serial.print(rtcLastMenu);
  Serial.print(" FOCUSCFG=");
  Serial.println(rtcLastFocusCfg);
  Serial.print("HEAP=");
  Serial.println(ESP.getFreeHeap());
  Serial.println("===============================");

  // RC15.196: ustawienia musza byc znane PRZED inicjalizacja RGB,
  // aby RGB=OFF nie dawalo nawet krotkiego niebieskiego blysku po restarcie.
  loadSettings();

#if CYD_LED_ENABLE
  pinMode(CYD_LED_RED_PIN, OUTPUT);
  pinMode(CYD_LED_GREEN_PIN, OUTPUT);
  pinMode(CYD_LED_BLUE_PIN, OUTPUT);
  // RC15.183: start / inicjalizacja = niebieski.
  rgbLedWrite(false, false, true);
#endif
  delay(100);

  Serial.println();
  Serial.println("======================================");
  Serial.println(" CYD WEATHER UI PRO - RC15.256 FINAL GLOBAL COMPASS");
  Serial.println("======================================");
  Serial.println("[BUILD] XPT2046 compile-time = ON");

  // Preferences zaladowano juz przed inicjalizacja RGB.

  // TFT uruchamiamy PRZED cięższym skanowaniem SD/historii,
  // żeby po flash/reset użytkownik od razu widział, że CYD startuje.
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, 0);

  tft.init();
  tft.setRotation(1);

  SkinPalette startupPalette = paletteFor(currentSkin);
  tft.fillScreen(startupPalette.bg);
  applyBrightness();
  drawBootScreen(3, "START");

  // RC15.116: splash zamiast START... i pustego ekranu.

  // Teraz dopiero SD, fonty, historia i kontrola archiwum.
  drawBootScreen(8, "KARTA SD");
  initCYDSD();
  drawBootScreen(30, "SD / FONTY / HISTORIA");

#ifdef SMOOTH_FONT
  // Test ladowania fontu wykonujemy dopiero po inicjalizacji TFT.
  if (fontRegularReady) {
    if (loadSmoothFontSD("CYD/fonts/ui_regular")) {
      Serial.println("[FONT] ui_regular LOAD=OK");
      unloadSmoothFontSafe();
    } else {
      Serial.println("[FONT] ui_regular runtime=QUARANTINE");
    }
  }
#endif

  SkinPalette p = paletteFor(currentSkin);
  drawBootScreen(38, "FONTY GOTOWE");

#if !HAS_TOUCH
  Serial.println("[TOUCH] Library XPT2046_Touchscreen not installed");
  Serial.println("[TOUCH] Display will work, menu touch disabled");
#endif

  // RC15.243/247: konfiguracja Wi-Fi i AIO jest ladowana z NVS przed startem sieci.
  loadWifiConfigNvs();
  loadAioConfigNvs();

  // RC15.241A: w Arduino-ESP32 2.x hostname musi byc ustawiony
  // zanim interfejs STA zostanie uruchomiony.
  bool hostnameOK = WiFi.setHostname("CYD-Weather");
  Serial.print("[NET] hostname set=");
  Serial.print(hostnameOK ? "OK " : "FAIL ");
  Serial.println(WiFi.getHostname());

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  // Najpierw Internet / skan Wi-Fi. To pozwala hotspotowi pracować
  // na dowolnym kanale 2.4 GHz.
  drawBootScreen(45, "WI-FI");
#if CYD_LED_ENABLE
  // Szukanie / laczenie sieci: niebieski sygnal startowy.
  rgbLedWrite(false, false, true);
#endif
  setupInternetSafe();

  // RC15.246: normalnie konfigurator dziala w LAN.
  // Gdy oba profile sa niedostepne, uruchamiamy CYD-Setup na kanale LOCAL=8.
  if (WiFi.status() == WL_CONNECTED) {
    startWifiConfigWeb();
  } else {
    startCydSetupAp();
    startWifiConfigWeb();
  }
#if CYD_LED_ENABLE
  // Po Wi-Fi nadal czekamy na pierwszy swiezy pakiet danych.
  rgbLedWrite(false, false, true);
#endif
  drawBootScreen(62, "SIEC GOTOWA");

  // Jeżeli nie połączyliśmy się z Wi-Fi, wracamy na stały kanał LOCAL=8.
  // Gdy Wi-Fi jest połączone, radio już pracuje na kanale AP i nie wolno
  // go przestawiać, bo zerwalibyśmy Internet.
  if (!internetConnected) {
    if (!cydSetupApMode) {
      esp_wifi_set_promiscuous(true);
      esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
      esp_wifi_set_promiscuous(false);
    }
    // W setup AP kanal zostal juz ustawiony przez WiFi.softAP(..., WIFI_CHANNEL).
    activeRadioChannel = WIFI_CHANNEL;
  } else {
    activeRadioChannel = (uint8_t)WiFi.channel();
  }

  esp_err_t r = esp_now_init();

  if (r != ESP_OK) {
    Serial.print("[ESP-NOW] init error=");
    Serial.println((int)r);

    tft.setTextColor(TFT_RED, p.bg);
    tft.drawCentreString("ESP-NOW ERROR", 160, 105, 4);
    return;
  }

  esp_now_register_recv_cb(onReceive);
  drawBootScreen(74, "ESP-NOW");

#if HAS_TOUCH
  // IDENTYCZNA inicjalizacja jak w CYD_TOUCH_TEST, ktory u Ciebie dzialal.
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT);

  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  bool touchOk = touch.begin(touchSPI);
  touch.setRotation(1);
  touchReady = touchOk;
  drawBootScreen(84, "DOTYK");

  Serial.print("[TOUCH] begin = ");
  Serial.println(touchOk ? "OK" : "BLAD");
  Serial.println("[TOUCH] CLK=25 MISO=39 MOSI=32 CS=33 IRQ=36");
  Serial.println("[TOUCH] rotation=1; tryb zgodny z dzialajacym testem");
#endif

  Serial.print("[ESP-NOW] OK channel ");
  Serial.println(activeRadioChannel);
  Serial.print("[ESP-NOW] packet size ");

#if CYD_INTERNET_ENABLE
  drawBootScreen(90, "HTTP / FINALIZACJA");
  BaseType_t httpTaskOk = xTaskCreatePinnedToCore(
    internetHttpTask,
    "CYD_HTTP",
    8192,
    nullptr,
    1,
    &internetHttpTaskHandle,
    0
  );

  Serial.print("[NET] HTTP worker=");
  Serial.println(httpTaskOk == pdPASS ? "OK" : "ERR");
  drawBootScreen(96, "FINALIZACJA");
#endif
  Serial.println(sizeof(WeatherPacket));

  Serial.print("[HIST] RAM points=");
  Serial.print(chartCount);
  Serial.print("/");
  Serial.print(CHART_HISTORY_POINTS);
  Serial.print(" interval=");
  Serial.print(CHART_SAMPLE_MS / 60000UL);
  Serial.print(" min range=");
  Serial.print(chartRangeName());
  Serial.print(" page=");
  Serial.println(chartPageName());

  Serial.print("[HIST] file=");
  Serial.print(formatBytesShort(chartHistoryFileBytes));
  Serial.print(" sessionWrites=");
  Serial.print(chartSdWritesOK);
  Serial.print(" sessionErrors=");
  Serial.println(chartSdWriteErrors);

  Serial.print("[HIST-IN] file=");
  Serial.print(formatBytesShort(indoorHistoryFileBytes));
  Serial.print(" sessionWrites=");
  Serial.print(indoorSdWritesOK);
  Serial.print(" sessionErrors=");
  Serial.print(indoorSdWriteErrors);
  Serial.print(" bootstrap=");
  Serial.println(indoorBootstrapPending ? "WAIT" : "OK");

  Serial.print("[CSV] file=");
  Serial.print(formatBytesShort(weatherCsvFileBytes));
  Serial.print(" sessionWrites=");
  Serial.print(weatherCsvWritesOK);
  Serial.print(" sessionErrors=");
  Serial.println(weatherCsvWriteErrors);

  Serial.print("[HIST-SYNC] writes=");
  Serial.print(historyBundleWritesOK);
  Serial.print(" errors=");
  Serial.print(historyBundleWriteErrors);
  Serial.print(" recoveries=");
  Serial.print(historyBundleRecoveries);
  Serial.print(" jrnErr=");
  Serial.println(historyBundleJournalErrors);

  lastIndoorToggle = millis();

  delay(120);
  drawBootScreen(100, "GOTOWE");
  // RC15.122: usun pozostalosci finalnego ekranu BOOT przed UI.
  tft.fillScreen(TFT_BLACK);
  forceScreenClear = true;
  delay(150);
  drawCurrentSkin();
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  bool redraw = false;
  bool chartSampleAdded = false;
  bool indoorOutdoorChanged = false;
  static unsigned long lastFullRedrawMs = 0;
  static uint8_t lastActiveSourceSeen = 255;
  static uint8_t lastFreshnessStateSeen = 255;
  static unsigned long lastFreshnessFrameMs = 0;
  static unsigned long lastHistoryTimeDiagMs = 0;

  serviceNtpRetry();
  serviceInternetTransport();
  serviceWifiConfigWeb();
  serviceSDHotplug();

  // RC15.233: recovery po hotplug moglo dodac nowy punkt do RAM poza
  // normalna sciezka pakietu. Traktujemy go jak zwykly nowy sample UI.
  if (sdHotplugRamSyncAdded) {
    sdHotplugRamSyncAdded = false;
    chartSampleAdded = true;
    redraw = true;
  }

  updateActiveDataSource();

  if (millis() - lastHistoryTimeDiagMs >= 60000UL) {
    lastHistoryTimeDiagMs = millis();
    printHistoryTimeDiagnostics("PERIODIC");
  }

  // RC15.182: zmiana LIVE/STALE/OFFLINE wymusza jeden pelny redraw.
  // Dzieki temu po powrocie LIVE poprzednia ramka znika fizycznie,
  // bez malowania jej kolorem tla po elementach skina.
  uint8_t freshnessStateNow = (uint8_t)activeSourceUiState();
  if (lastFreshnessStateSeen != freshnessStateNow) {
    lastFreshnessStateSeen = freshnessStateNow;

    if (!menuOpen && !diagOpen && !calibrationOpen) {
      forceScreenClear = true;
      redraw = true;
    }
  }

  if (lastActiveSourceSeen != activeDataSource) {
    lastActiveSourceSeen = activeDataSource;
    forceScreenClear = true;
    redraw = true;

#if CYD_DEBUG
    Serial.print("[SOURCE] active=");
    Serial.println(activeDataSource == SOURCE_INTERNET ? "INTERNET" : "LOCAL");
#endif
  }

  if (newData) {
    WeatherPacket d = {};
    portENTER_CRITICAL(&dataMux);
    memcpy(&d, &pendingLocalData, sizeof(d));
    newData = false;
    portEXIT_CRITICAL(&dataMux);

    if (!validateWeatherPacket(d)) {
      localBadPacketCount++;
#if CYD_DEBUG
      Serial.print("[ESP-NOW] LOCAL BAD=");
      Serial.println(localBadPacketCount);
#endif
    } else {
      portENTER_CRITICAL(&dataMux);
      memcpy(&liveData, &d, sizeof(liveData));
      haveData = true;
      lastPacketMs = millis();
      packetCount++;
      portEXIT_CRITICAL(&dataMux);

      if (activeDataSource == SOURCE_LOCAL)
        redraw = true;

    // Nowy sidecar WEW dostaje pierwsza probke natychmiast.
    // Nie dopisujemy przy tym rekordu do glownego weather_history.bin,
    // wiec nie zageszczamy starej historii podczas testowych restartow.
    if (activeDataSource == SOURCE_LOCAL && indoorBootstrapPending && sdReady &&
        !SD.exists(HISTORY_BUNDLE_JOURNAL_FILE)) {
      uint32_t bootstrapEpoch = historyEpochNow();

      // RC15.205: sidecar WEW rowniez nie moze dostac epoch=0.
      if (bootstrapEpoch > 100000UL &&
          appendIndoorSampleSD(d, bootstrapEpoch)) {
        indoorBootstrapPending = false;

        // Zeby strona WEW od razu pokazala pierwszy punkt,
        // dopinamy wartosci WEW do najnowszej probki RAM.
        if (chartCount > 0) {
          int lastIdx = chartIndexOldest(chartCount - 1);
          chartHistory[lastIdx].tempIn = d.temp_wewnetrzna;
          chartHistory[lastIdx].humIn = d.wilg_wewnetrzna;
        }

        indoor7dLoaded = false;
        Serial.println("[HIST-IN] bootstrap=OK");
      } else {
        Serial.println(bootstrapEpoch > 100000UL ?
                       "[HIST-IN] bootstrap=WAIT" :
                       "[HIST-IN] bootstrap=WAIT_TIME");
      }
    }

    // Historia: pierwsza swieza probka po starcie od razu,
    // kolejne co 5 minut. RAM sluzy do szybkiego rysowania,
    // SD tylko do trwalego archiwum.
    uint32_t historyNowMs = millis();
    uint32_t historyNowEpoch = historyEpochNow();

    if (historyNowEpoch <= 100000UL &&
        (uint32_t)(historyNowMs - lastChartSampleMs) >= CHART_SAMPLE_MS) {
      static unsigned long lastLocalTimePauseLogMs = 0;
      if (historyNowMs - lastLocalTimePauseLogMs >= 60000UL) {
        lastLocalTimePauseLogMs = historyNowMs;
        Serial.println("[HIST-TIME] LOCAL write=PAUSED_TIME");
      }
    }

    if (activeDataSource == SOURCE_LOCAL &&
        historySampleDue(historyNowMs, historyNowEpoch)) {
      uint32_t sampleMs = historyNowMs;
      uint32_t epoch = historyNowEpoch;

      // RC15.203: MAIN jest rekordem nadrzednym i "commit markerem" cyklu.
      // Jesli MAIN nie zapisze sie, NIE przesuwamy lastChartSampleMs.
      // Kolejny swiezy pakiet LOCAL moze wtedy sprobowac ponownie.
      WeatherPacket committedD = {};
      uint32_t committedEpoch = 0;
      bool bundleSaved = commitHistoryBundleSD(d, epoch, committedD, committedEpoch);

      if (historyPendingRamSyncAdded) {
        historyPendingRamSyncAdded = false;
        chart7dLoaded = false;
        indoor7dLoaded = false;
        windDir7dLoaded = false;
        chartSampleAdded = true;
        redraw = true;
      }

      if (bundleSaved) {
        lastChartSampleMs = sampleMs;
        lastCommittedHistoryEpoch = committedEpoch;

        pushChartSample(committedD, committedEpoch);
        pushWindDirSample(committedD.kierunek_wiatru, committedEpoch);

        historyBundleWritesOK++;

        chart7dLoaded = false;
        indoor7dLoaded = false;
        windDir7dLoaded = false;
        chartSampleAdded = true;
      } else {
        historyBundleWriteErrors++;
        Serial.println("[HIST-JRN] WARNING pending retry=NEXT_PACKET");
      }
    }

#if CYD_DEBUG
    Serial.print("[RX] #");
    Serial.print(packetCount);
    Serial.print(" len=");
    Serial.print(lastPacketLen);
    Serial.print(" T=");
    Serial.print(d.temperatura, 1);
    Serial.print(" H=");
    Serial.print(d.wilgotnosc, 0);
    Serial.print(" P=");
    Serial.print(d.cisnienie, 1);
    Serial.print(" W=");
    Serial.print(d.predkosc_wiatru, 1);
    Serial.print(" UV=");
    Serial.print(d.uv_index, 1);
    Serial.print(" LUX=");
    Serial.println(d.swiatlo_lux, 0);
#endif
    }
  }


  // Nowy rekord INTERNET odswieza UI i historie tylko wtedy,
  // gdy INTERNET jest faktycznie aktywnym zrodlem.
  if (newInternetData) {
    newInternetData = false;

    if (activeDataSource == SOURCE_INTERNET) {
      redraw = true;
      WeatherPacket d = snapshotInternetData();

      uint32_t historyNowMs = millis();
      uint32_t historyNowEpoch = historyEpochNow();

      if (historyNowEpoch <= 100000UL &&
          (uint32_t)(historyNowMs - lastChartSampleMs) >= CHART_SAMPLE_MS) {
        static unsigned long lastNetTimePauseLogMs = 0;
        if (historyNowMs - lastNetTimePauseLogMs >= 60000UL) {
          lastNetTimePauseLogMs = historyNowMs;
          Serial.println("[HIST-TIME] INTERNET write=PAUSED_TIME");
        }
      }

      if (historySampleDue(historyNowMs, historyNowEpoch)) {
        uint32_t sampleMs = historyNowMs;
        uint32_t epoch = historyNowEpoch;

        WeatherPacket committedD = {};
        uint32_t committedEpoch = 0;
        bool bundleSaved = commitHistoryBundleSD(d, epoch, committedD, committedEpoch);

        if (historyPendingRamSyncAdded) {
          historyPendingRamSyncAdded = false;
          chart7dLoaded = false;
          indoor7dLoaded = false;
          windDir7dLoaded = false;
          chartSampleAdded = true;
          redraw = true;
        }

        if (bundleSaved) {
          lastChartSampleMs = sampleMs;
          lastCommittedHistoryEpoch = committedEpoch;

          pushChartSample(committedD, committedEpoch);
          pushWindDirSample(committedD.kierunek_wiatru, committedEpoch);

          historyBundleWritesOK++;

          chart7dLoaded = false;
          indoor7dLoaded = false;
          windDir7dLoaded = false;
          chartSampleAdded = true;
        } else {
          historyBundleWriteErrors++;
          Serial.println("[HIST-JRN] NET WARNING pending retry=NEXT_RECORD");
        }
      }
    }
  }

  if (autoIndoorOutdoor &&
      activeSourceAvailable() &&
      !waitTouchRelease &&
      !menuOpen && !diagOpen && !calibrationOpen &&
      (currentSkin == SKIN_GRID || currentSkin == SKIN_MINIMAL) &&
      millis() - lastIndoorToggle >= (unsigned long)rotateSeconds * 1000UL) {
    // Automatyczne WEW/ZEW ma sens na klasycznych ekranach pogodowych.
    // Na WYKRESACH, szczególnie STAT, stan wybiera użytkownik ręcznie.
    lastIndoorToggle = millis();
    showIndoor = !showIndoor;
    indoorOutdoorChanged = true;

    // GRID: tylko dwa kafelki zależne od WEW/ZEW.
    // Pozostałe skiny zachowują dotychczasowy pełny redraw.
    if (currentSkin == SKIN_GRID) {
      WeatherPacket d = snapshotData();
      refreshGridInOutTiles(d);
      redraw = false;
    } else if (currentSkin == SKIN_MINIMAL) {
      refreshCurrentSkinLiveValues();
      redraw = false;
    } else {
      redraw = true;
    }

#if CYD_DEBUG
    Serial.print("[INOUT] auto -> ");
    Serial.println(showIndoor ? "WEW" : "ZEW");
#endif
  }

  uiTransitionRedrawnThisLoop = false;

  if (calibrationOpen) {
    serviceCalibrationTouch();
  } else {
    serviceTouch();
  }

  // RC15.135 RETURN FREEZE:
  // Gdy akcja overlay czeka jeszcze na faktyczne puszczenie palca,
  // ekran pozostaje całkowicie zamrożony. Dane, sieć, historia i LED
  // nadal pracują, ale nic nie może rysować pod MENU / FOCUS SELECT.
  if (waitTouchRelease)
    redraw = false;

  // Jesli serviceTouch odtworzyl ekran po MENU/FOCUS SELECT,
  // nie wykonuj ponownie redraw z pakietu RX z tej samej iteracji.
  if (uiTransitionRedrawnThisLoop)
    redraw = false;

  serviceStatusLed();

  // FOCUS nie przerysowuje juz calego ekranu co 500 ms.
  // RC15.135: pasek/progres również stoi podczas RETURN FREEZE.
  if (!waitTouchRelease)
    serviceFocusProgress();

  static unsigned long lastDiagRefresh = 0;
  if (diagOpen && millis() - lastDiagRefresh >= 1000UL) {
    lastDiagRefresh = millis();
    refreshDiagnosticsValues();
  }

  if (redraw && !waitTouchRelease &&
      !menuOpen && !diagOpen && !calibrationOpen &&
      !forceScreenClear && !chartSampleAdded &&
      (currentSkin == SKIN_GRID ||
       currentSkin == SKIN_MINIMAL ||
       currentSkin == SKIN_INSTRUMENT)) {
    refreshCurrentSkinLiveValues();
    redraw = false;
  }

  if (redraw && !waitTouchRelease &&
      !menuOpen && !diagOpen && !calibrationOpen) {
    // Zwykly pakiet nie czyści FOCUS, ale forceScreenClear po zmianie
    // stanu swiezosci ma pierwszenstwo.
    if ((forceScreenClear || currentSkin != SKIN_FOCUS) &&
        millis() - lastFullRedrawMs >= 250UL) {

      // CHARTS i GRID nie wymagaja pelnego redraw przy kazdym pakiecie.
      // CHARTS: tylko nowa probka historii.
      // GRID: pelny redraw tylko przy zmianie ekranu/WEW-ZEW albo
      // przy nowej probce historii.
      bool allowFullRedraw = true;

      if (currentSkin == SKIN_CHARTS &&
          !chartSampleAdded &&
          !forceScreenClear)
        allowFullRedraw = false;

      if (currentSkin == SKIN_GRID &&
          !chartSampleAdded &&
          !forceScreenClear)
        allowFullRedraw = false;

      if (allowFullRedraw) {
        lastFullRedrawMs = millis();
        drawCurrentSkin();
      }
    }
  }

  // Footer age refresh every second
  static unsigned long lastFooter = 0;

  if (!waitTouchRelease &&
      !uiTransitionRedrawnThisLoop &&
      !menuOpen && !diagOpen && !calibrationOpen &&
      millis() - lastFooter >= 1000UL) {
    lastFooter = millis();

    // RC15.59: odswiezamy tylko pola dynamiczne.
    // Tytul naglowka oraz przycisk MENU nie sa juz dotykane co sekunde.
    refreshHeaderStatusOnly();

    if (currentSkin == SKIN_CHARTS)
      refreshChartsRxOnly();
    else
      refreshSimpleFooterRxOnly();
  }

  // RC15.180C: ostatnia warstwa calego ekranu.
  if (!waitTouchRelease &&
      !menuOpen && !diagOpen && !calibrationOpen &&
      millis() - lastFreshnessFrameMs >= 100UL) {
    lastFreshnessFrameMs = millis();
    refreshDataFreshnessFrame();
  }

  delay(10);
}