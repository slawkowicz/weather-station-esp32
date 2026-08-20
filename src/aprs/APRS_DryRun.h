#pragma once

#include <Arduino.h>
#include <math.h>

// APRS v2.0 STEP 1 - DRY-RUN ONLY
// Ten modul tylko formatuje dane pogodowe. Nie otwiera socketow,
// nie laczy sie z APRS-IS i nie nadaje RF.

struct AprsWeatherInput {
  float temperatureC;
  float humidityPct;
  float pressureHpa;
  float windSpeedMs;
  float windGustMs;
  float windDirectionDeg;
  float rainLastHourMm;
};

static inline int aprsClampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static inline String buildAprsWxBody(const AprsWeatherInput &d) {
  // APRS weather fields used here:
  // c = wind direction [deg]
  // s = sustained wind [mph]
  // g = gust [mph]
  // t = temperature [deg F]
  // r = rain last hour [0.01 inch]
  // h = humidity [%], 00 means 100%
  // b = pressure [0.1 hPa]
  //
  // Deliberately omitted at this stage:
  // p = rain last 24 h
  // P = rain since midnight
  // because LilyGO v1.0 does not yet expose guaranteed values for
  // those exact APRS meanings.

  float dirDeg = isfinite(d.windDirectionDeg) ? d.windDirectionDeg : 0.0f;
  while (dirDeg < 0.0f) dirDeg += 360.0f;
  while (dirDeg >= 360.0f) dirDeg -= 360.0f;
  int dir = aprsClampInt((int)lroundf(dirDeg), 0, 359);

  float windMs = isfinite(d.windSpeedMs) ? d.windSpeedMs : 0.0f;
  float gustMs = isfinite(d.windGustMs) ? d.windGustMs : 0.0f;
  int windMph = aprsClampInt((int)lroundf(max(0.0f, windMs) * 2.23693629f), 0, 999);
  int gustMph = aprsClampInt((int)lroundf(max(0.0f, gustMs) * 2.23693629f), 0, 999);

  float tempC = isfinite(d.temperatureC) ? d.temperatureC : 0.0f;
  int tempF = aprsClampInt((int)lroundf(tempC * 9.0f / 5.0f + 32.0f), -99, 999);

  float rainHourMm = isfinite(d.rainLastHourMm) ? d.rainLastHourMm : 0.0f;
  int rainHundredthsInch = aprsClampInt(
      (int)lroundf(max(0.0f, rainHourMm) * 3.93700787f), 0, 999);

  float humPct = isfinite(d.humidityPct) ? d.humidityPct : 0.0f;
  int hum = aprsClampInt((int)lroundf(humPct), 0, 100);

  float pressureHpa = isfinite(d.pressureHpa) ? d.pressureHpa : 0.0f;
  int pressureTenthHpa = aprsClampInt(
      (int)lroundf(max(0.0f, pressureHpa) * 10.0f), 0, 99999);

  char tField[8];
  if (tempF < 0)
    snprintf(tField, sizeof(tField), "-%02d", abs(tempF));
  else
    snprintf(tField, sizeof(tField), "%03d", tempF);

  char hField[4];
  if (hum >= 100)
    snprintf(hField, sizeof(hField), "00");
  else
    snprintf(hField, sizeof(hField), "%02d", hum);

  char body[96];
  snprintf(body, sizeof(body),
           "c%03ds%03dg%03dt%sr%03dh%sb%05d",
           dir, windMph, gustMph, tField,
           rainHundredthsInch, hField, pressureTenthHpa);

  return String(body);
}
