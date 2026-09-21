/*
 * ASCIS - configuration
 * All pin numbers, hardware options and tuning values live here so the
 * main sketch never needs editing for a different wiring or crop.
 */
#ifndef ASCIS_CONFIG_H
#define ASCIS_CONFIG_H

/* ------------------------------------------------------------------ */
/* Pin map (traced from docs/images/circuit-diagram.png)              */
/* ------------------------------------------------------------------ */
#define PIN_DHT           2     // DHT22 data (10 k pull-up to 5 V on the breadboard)
#define PIN_RELAY         3     // Relay module signal input (IN / S)
#define PIN_MOSFET_GATE   5     // Gate of the breadboard MOSFET - only used if USE_MOSFET_DRIVER = 1
#define PIN_SOIL          A1    // Soil moisture sensor SIG
#define PIN_CURRENT       A3    // Current sensor OUT (only used if USE_CURRENT_SENSOR = 1)
// I2C bus: A4 = SDA, A5 = SCL  (16x2 LCD backpack + BMP180 share it)

/* ------------------------------------------------------------------ */
/* Hardware options                                                   */
/* ------------------------------------------------------------------ */
#define DHT_TYPE          DHT22 // DHT22 or DHT11
#define LCD_I2C_ADDR      0x27  // Try 0x3F if the LCD stays blank
#define RELAY_ACTIVE_LOW  0     // 1 if your relay board switches ON when the pin goes LOW
#define USE_BMP180        1     // Barometric pressure -> simple rain heuristic
#define USE_MOSFET_DRIVER 0     // 1 = also drive PIN_MOSFET_GATE together with the relay
#define USE_CURRENT_SENSOR 0    // 1 = pump current protection (needs the sensor wired in series with the pump)

/* ------------------------------------------------------------------ */
/* Soil moisture calibration                                          */
/* ------------------------------------------------------------------ */
// Set CALIBRATION_MODE to 1, upload, open the Serial Monitor (9600 baud):
//   - sensor in dry air   -> note the value -> SOIL_RAW_DRY
//   - sensor in a glass of water (to the line) -> note it -> SOIL_RAW_WET
// Works for sensors that read higher when wet AND ones that read lower.
// The numbers below are PLACEHOLDERS - replace them with your own readings.
#define CALIBRATION_MODE  0
#define SOIL_RAW_DRY      520
#define SOIL_RAW_WET      260
#define SOIL_SAMPLES      10    // readings averaged per measurement

// A raw reading outside this window means a wire is loose/shorted -> treated as a fault
#define SOIL_FAULT_CHECK  1
#define SOIL_RAW_MIN_VALID 3
#define SOIL_RAW_MAX_VALID 1020

/* ------------------------------------------------------------------ */
/* Watering logic                                                     */
/* ------------------------------------------------------------------ */
#define MOISTURE_ON_PCT        35   // start watering below this (before weather adjustment)
#define MOISTURE_OFF_PCT       60   // stop watering at/above this
#define MOISTURE_CRITICAL_PCT  15   // water even if rain is expected

#define HOT_TEMP_C             35.0f  // at/above: soil dries faster -> start earlier
#define HOT_BONUS_PCT          5
#define COOL_TEMP_C            15.0f  // at/below: slower drying -> start later
#define COOL_PENALTY_PCT       5

#define PUMP_MAX_RUN_S         300UL  // hard limit for one watering cycle
#define PUMP_MIN_REST_S        600UL  // let water soak in before judging again
#define MAX_FAILED_CYCLES      3      // cycles that hit the limit without reaching the target -> lock out

#define SAMPLE_INTERVAL_MS     2000UL // DHT22 needs >= 2 s between reads

/* ------------------------------------------------------------------ */
/* Rain heuristic (BMP180)                                            */
/* ------------------------------------------------------------------ */
// Pressure is stored every PRESSURE_SAMPLE_MIN minutes; a fall of at least
// PRESSURE_DROP_HPA over the last ~3 hours is treated as "rain likely".
#define PRESSURE_SAMPLE_MIN    15UL
#define PRESSURE_HIST_LEN      13     // 13 samples x 15 min = 3 h window
#define PRESSURE_DROP_HPA      4.0f

/* ------------------------------------------------------------------ */
/* Pump current protection (ACS712 style sensor, only if enabled)     */
/* ------------------------------------------------------------------ */
#define CURRENT_SENS_V_PER_A   0.185f // 5 A module = 0.185, 20 A = 0.100, 30 A = 0.066
#define CURRENT_SAMPLES        50
#define CURRENT_GRACE_MS       2000UL // ignore the start-up surge
#define OVERCURRENT_A          2.0f   // stalled / jammed pump
#define DRYRUN_MIN_A           0.05f  // pump "on" but drawing nothing

#define SERIAL_BAUD            9600

#endif
