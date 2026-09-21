/*
 * ASCIS - Arduino-based Sensor Controlled Irrigation System
 * Target: Arduino Uno R3 (ATmega328P)
 *
 * Reads soil moisture, air temperature/humidity and (optionally) barometric
 * pressure, decides when to run the pump, switches it through a relay and shows
 * the live values on a 16x2 I2C LCD.
 *
 * Libraries (Arduino IDE -> Library Manager):
 *   - DHT sensor library            (Adafruit)  + Adafruit Unified Sensor
 *   - LiquidCrystal I2C             (Frank de Brabander)
 *   - Adafruit BMP085 Library       (works for the BMP180) + Adafruit BusIO
 *
 * Serial commands (9600 baud):
 *   1 = pump ON (manual)   0 = pump OFF (manual)   a = back to AUTO / clear lockout
 *   s = print status
 */

#include <Arduino.h>
#include <Wire.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include "config.h"
#if USE_BMP180
#include <Adafruit_BMP085.h>
#endif

static_assert(SOIL_RAW_DRY != SOIL_RAW_WET, "SOIL_RAW_DRY and SOIL_RAW_WET must differ");
static_assert(MOISTURE_ON_PCT < MOISTURE_OFF_PCT, "MOISTURE_ON_PCT must be below MOISTURE_OFF_PCT");

#define PUMP_MAX_RUN_MS   (PUMP_MAX_RUN_S * 1000UL)
#define PUMP_MIN_REST_MS  (PUMP_MIN_REST_S * 1000UL)
#define RELAY_ON_LEVEL    (RELAY_ACTIVE_LOW ? LOW : HIGH)
#define RELAY_OFF_LEVEL   (RELAY_ACTIVE_LOW ? HIGH : LOW)

/* ------------------------------ devices ------------------------------ */
DHT dht(PIN_DHT, DHT_TYPE);
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 16, 2);
#if USE_BMP180
Adafruit_BMP085 bmp;
static bool bmpOk = false;
#endif

/* ------------------------------- state -------------------------------- */
enum Mode : uint8_t { MODE_AUTO, MODE_FORCE_ON, MODE_FORCE_OFF };

static Mode     mode          = MODE_AUTO;
static bool     pumpOn        = false;
static bool     pumpHasRun    = false;   // false until the first cycle finishes
static bool     lockout       = false;   // set by repeated failed cycles or current protection
static uint8_t  failedCycles  = 0;
static uint32_t pumpStartMs   = 0;
static uint32_t pumpStopMs    = 0;
static uint32_t lastSampleMs  = 0;

static int      soilRaw       = 0;
static int      soilPct       = 0;
static bool     soilFault     = false;
static float    tempC         = NAN;
static float    humidity      = NAN;
static float    pressureHpa   = NAN;
static bool     rainLikely    = false;

#if USE_BMP180
static float    pHist[PRESSURE_HIST_LEN];
static uint8_t  pIdx          = 0;
static uint8_t  pCount        = 0;
static uint32_t lastPressureMs = 0;
#endif

#if USE_CURRENT_SENSOR
static float    currentA      = 0.0f;
static float    currentZeroRaw = 512.0f;
#endif

static inline uint32_t since(uint32_t t) { return millis() - t; }

/* ------------------------------- pump --------------------------------- */
static void pumpWrite(bool on) {
  digitalWrite(PIN_RELAY, on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
#if USE_MOSFET_DRIVER
  digitalWrite(PIN_MOSFET_GATE, on ? HIGH : LOW);
#endif
}

static void pumpStart(const __FlashStringHelper *why) {
  pumpWrite(true);
  pumpOn = true;
  pumpStartMs = millis();
  Serial.print(F("[pump] ON  - "));
  Serial.println(why);
}

static void pumpStop(const __FlashStringHelper *why) {
  pumpWrite(false);
  pumpOn = false;
  pumpHasRun = true;
  pumpStopMs = millis();
  Serial.print(F("[pump] OFF - "));
  Serial.println(why);
}

static void enterLockout(const __FlashStringHelper *why) {
  lockout = true;
  Serial.print(F("[!] LOCKOUT: "));
  Serial.println(why);
  Serial.println(F("    Check tank, pipes and pump, then send 'a' to resume."));
}

/* ------------------------------ sensors ------------------------------- */
static void readSoil() {
  (void)analogRead(PIN_SOIL);                       // discard first sample after mux switch
  uint32_t sum = 0;
  for (uint8_t i = 0; i < SOIL_SAMPLES; i++) {
    sum += analogRead(PIN_SOIL);
    delay(5);
  }
  soilRaw = (int)(sum / SOIL_SAMPLES);
  soilFault = SOIL_FAULT_CHECK && (soilRaw < SOIL_RAW_MIN_VALID || soilRaw > SOIL_RAW_MAX_VALID);
  long pct = map(soilRaw, SOIL_RAW_DRY, SOIL_RAW_WET, 0, 100);   // map() handles reversed ranges
  soilPct = (int)constrain(pct, 0L, 100L);
}

static void readAir() {
  tempC    = dht.readTemperature();                 // NaN when the read fails
  humidity = dht.readHumidity();
}

#if USE_BMP180
// Keep a 3 h history (one sample per PRESSURE_SAMPLE_MIN) and flag a falling trend.
static void updatePressure() {
  if (!bmpOk) return;
  pressureHpa = bmp.readPressure() / 100.0f;
  if (pCount == 0 || since(lastPressureMs) >= PRESSURE_SAMPLE_MIN * 60000UL) {
    lastPressureMs = millis();
    pHist[pIdx] = pressureHpa;
    pIdx = (pIdx + 1) % PRESSURE_HIST_LEN;
    if (pCount < PRESSURE_HIST_LEN) pCount++;
    if (pCount == PRESSURE_HIST_LEN) {              // window full: pHist[pIdx] is the oldest
      rainLikely = (pHist[pIdx] - pressureHpa) >= PRESSURE_DROP_HPA;
    }
  }
}
#endif

#if USE_CURRENT_SENSOR
static float readCurrentRaw() {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < CURRENT_SAMPLES; i++) sum += analogRead(PIN_CURRENT);
  return sum / (float)CURRENT_SAMPLES;
}

static void readCurrent() {
  currentA = fabsf((readCurrentRaw() - currentZeroRaw) * (5.0f / 1023.0f) / CURRENT_SENS_V_PER_A);
}
#endif

/* ---------------------------- control logic --------------------------- */
// Hot weather dries the soil faster -> start earlier; cool weather -> start later.
static int startThreshold() {
  int t = MOISTURE_ON_PCT;
  if (!isnan(tempC)) {
    if (tempC >= HOT_TEMP_C)       t += HOT_BONUS_PCT;
    else if (tempC <= COOL_TEMP_C) t -= COOL_PENALTY_PCT;
  }
  return constrain(t, 5, MOISTURE_OFF_PCT - 5);
}

static void controlPump() {
#if USE_CURRENT_SENSOR
  if (pumpOn && since(pumpStartMs) >= CURRENT_GRACE_MS) {
    if (currentA > OVERCURRENT_A) {
      pumpStop(F("overcurrent"));
      enterLockout(F("pump current too high (jam?)"));
    } else if (currentA < DRYRUN_MIN_A) {
      pumpStop(F("no load current"));
      enterLockout(F("pump draws no current (dry run / wiring?)"));
    }
  }
#endif

  if (lockout) {
    if (pumpOn) pumpStop(F("lockout"));
    return;
  }

  if (mode == MODE_FORCE_OFF) {
    if (pumpOn) pumpStop(F("manual off"));
    return;
  }

  if (mode == MODE_FORCE_ON) {
    if (!pumpOn) {
      pumpStart(F("manual on"));
    } else if (since(pumpStartMs) >= PUMP_MAX_RUN_MS) {
      pumpStop(F("manual run limit"));
      mode = MODE_AUTO;
    }
    return;
  }

  /* ---- AUTO ---- */
  if (soilFault) {
    if (pumpOn) pumpStop(F("soil sensor fault"));
    return;
  }

  if (pumpOn) {
    if (soilPct >= MOISTURE_OFF_PCT) {
      pumpStop(F("target moisture reached"));
      failedCycles = 0;
    } else if (since(pumpStartMs) >= PUMP_MAX_RUN_MS) {
      pumpStop(F("max run time, soil still dry"));
      if (++failedCycles >= MAX_FAILED_CYCLES) {
        enterLockout(F("soil never reached target (empty tank / blocked pipe?)"));
      }
    }
    return;
  }

  const bool rested   = !pumpHasRun || since(pumpStopMs) >= PUMP_MIN_REST_MS;
  const bool dry      = soilPct < startThreshold();
  const bool critical = soilPct <= MOISTURE_CRITICAL_PCT;
  if (dry && rested && (!rainLikely || critical)) {
    pumpStart(F("soil dry"));
  }
}

/* ------------------------------ user output --------------------------- */
static void showLcd() {
  char l1[17], l2[17], tmp[32], ts[8], hs[8], ps[10];

  snprintf(l1, sizeof(l1), "S:%3d%% Pump:%-3s ", soilPct, pumpOn ? "ON" : "OFF");

  if (lockout) {
    snprintf(l2, sizeof(l2), "%-16s", "CHECK WATER/PUMP");
  } else if (soilFault) {
    snprintf(l2, sizeof(l2), "%-16s", "SOIL SENSOR ERR");
  } else {
    if (isnan(tempC))    strcpy(ts, "--C");   else snprintf(ts, sizeof(ts), "%dC", (int)(tempC + 0.5f));
    if (isnan(humidity)) strcpy(hs, "--%");   else snprintf(hs, sizeof(hs), "%d%%", (int)(humidity + 0.5f));
    ps[0] = '\0';
#if USE_BMP180
    if (!isnan(pressureHpa)) snprintf(ps, sizeof(ps), "%dhPa", (int)(pressureHpa + 0.5f));
#endif
    snprintf(tmp, sizeof(tmp), "%s %s %s", ts, hs, ps);
    snprintf(l2, sizeof(l2), "%-16.16s", tmp);
  }

  lcd.setCursor(0, 0);
  lcd.print(l1);
  lcd.setCursor(0, 1);
  lcd.print(l2);
}

static const __FlashStringHelper *modeName() {
  switch (mode) {
    case MODE_FORCE_ON:  return F("MANUAL-ON");
    case MODE_FORCE_OFF: return F("MANUAL-OFF");
    default:             return F("AUTO");
  }
}

static void logStatus() {
  Serial.print(F("t="));            Serial.print(millis() / 1000UL);
  Serial.print(F("s soil_raw="));   Serial.print(soilRaw);
  Serial.print(F(" soil="));        Serial.print(soilPct);
  Serial.print(F("% temp="));       Serial.print(tempC, 1);
  Serial.print(F("C rh="));         Serial.print(humidity, 0);
#if USE_BMP180
  Serial.print(F("% p="));          Serial.print(pressureHpa, 1);
  Serial.print(F("hPa rain="));     Serial.print(rainLikely ? F("likely") : F("no"));
#endif
#if USE_CURRENT_SENSOR
  Serial.print(F(" I="));           Serial.print(currentA, 2);
  Serial.print(F("A"));
#endif
  Serial.print(F(" start<"));       Serial.print(startThreshold());
  Serial.print(F("% pump="));       Serial.print(pumpOn ? F("ON") : F("OFF"));
  Serial.print(F(" mode="));        Serial.print(modeName());
  if (soilFault) Serial.print(F(" [SOIL FAULT]"));
  if (lockout)   Serial.print(F(" [LOCKOUT]"));
  Serial.println();
}

/* ----------------------------- serial commands ------------------------ */
static void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      case '1':
        if (lockout) Serial.println(F("Locked out - send 'a' first."));
        else { mode = MODE_FORCE_ON;  Serial.println(F("Manual: pump ON (limited to max run time)")); }
        break;
      case '0':
        mode = MODE_FORCE_OFF;
        Serial.println(F("Manual: pump OFF"));
        break;
      case 'a':
      case 'A':
        mode = MODE_AUTO;
        lockout = false;
        failedCycles = 0;
        Serial.println(F("AUTO mode, lockout cleared"));
        break;
      case 's':
      case 'S':
        logStatus();
        break;
      default:
        break;   // ignore CR/LF and anything else
    }
  }
}

/* ------------------------------- Arduino ------------------------------- */
void setup() {
  // Set the output level BEFORE switching to OUTPUT so the pump never glitches on at boot.
  digitalWrite(PIN_RELAY, RELAY_OFF_LEVEL);
  pinMode(PIN_RELAY, OUTPUT);
#if USE_MOSFET_DRIVER
  digitalWrite(PIN_MOSFET_GATE, LOW);
  pinMode(PIN_MOSFET_GATE, OUTPUT);
#endif

  Serial.begin(SERIAL_BAUD);
  Serial.println(F("ASCIS starting..."));

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print(F("ASCIS irrigation"));
  lcd.setCursor(0, 1);
  lcd.print(F("starting..."));

  dht.begin();

#if USE_BMP180
  bmpOk = bmp.begin();
  if (!bmpOk) Serial.println(F("BMP180 not found - rain heuristic disabled"));
#endif

#if USE_CURRENT_SENSOR
  currentZeroRaw = readCurrentRaw();               // pump is off here -> this is the zero-current level
#endif

  delay(2000);                                     // splash + let the DHT22 settle
  lcd.clear();
  lastSampleMs = millis() - SAMPLE_INTERVAL_MS;    // first measurement right away
}

void loop() {
  handleSerial();

  if (since(lastSampleMs) < SAMPLE_INTERVAL_MS) return;
  lastSampleMs = millis();

  readSoil();

#if CALIBRATION_MODE
  Serial.print(F("soil_raw=")); Serial.println(soilRaw);
  lcd.setCursor(0, 0); lcd.print(F("CALIBRATION     "));
  lcd.setCursor(0, 1); lcd.print(F("raw: "));
  lcd.print(soilRaw);  lcd.print(F("     "));
  return;                                          // pump stays off
#endif

  readAir();
#if USE_BMP180
  updatePressure();
#endif
#if USE_CURRENT_SENSOR
  readCurrent();
#endif

  controlPump();
  showLcd();
  logStatus();
}
