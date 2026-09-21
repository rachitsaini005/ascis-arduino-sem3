# ASCIS firmware

Arduino Uno sketch for the irrigation controller. Open `firmware/ASCIS/ASCIS.ino` in the Arduino IDE (the `config.h` next to it opens as a second tab).

## Libraries

Install through **Sketch → Include Library → Manage Libraries**:

- **DHT sensor library** (Adafruit) — also install **Adafruit Unified Sensor** when asked
- **LiquidCrystal I2C** (Frank de Brabander)
- **Adafruit BMP085 Library** (also drives the BMP180) — also install **Adafruit BusIO** when asked

Board: **Arduino Uno**. Builds to about 15.7 KB of flash and 0.7 KB of RAM.

## Pin map

Read from `images/circuit-diagram.png`. **Check it against your real wiring before powering the pump.**

| Signal | Arduino pin | Notes |
|---|---|---|
| DHT22 data | D2 | 10 kΩ pull-up to 5 V |
| Relay IN | D3 | switches the pump (confirmed) |
| MOSFET gate | D5 | not used — the relay switches the pump; leave `USE_MOSFET_DRIVER 0` |
| Soil moisture SIG | A1 | |
| Current sensor OUT | A3 | optional, off by default (`USE_CURRENT_SENSOR`) |
| I2C SDA / SCL | A4 / A5 | 16×2 LCD backpack and BMP180 share the bus |

If your wiring differs, change the numbers at the top of `config.h` — nothing else needs editing.

## First-time setup

1. **Calibrate the soil sensor.** In `config.h` set `CALIBRATION_MODE 1`, upload, and open the Serial Monitor at 9600 baud. Note the value in dry air → `SOIL_RAW_DRY`, and in a glass of water → `SOIL_RAW_WET`. Set `CALIBRATION_MODE` back to `0` and upload again. The pump never runs in calibration mode.
2. **Check the LCD.** Blank screen? Try `LCD_I2C_ADDR 0x3F`, or turn the contrast screw on the backpack.
3. **Check the relay logic.** With no water connected, send `1` in the Serial Monitor. If the relay clicks *off* when it should click on, set `RELAY_ACTIVE_LOW 1`.
4. Tune the thresholds for your crop and soil.

## How it decides

- **Start** when soil moisture drops below `MOISTURE_ON_PCT` (35 %); **stop** at `MOISTURE_OFF_PCT` (60 %). The gap prevents rapid on/off cycling.
- **Weather adjustment:** at or above 35 °C the start threshold rises by 5 points; at or below 15 °C it drops by 5.
- **Rain heuristic (BMP180):** a pressure fall of 4 hPa or more over the last 3 hours counts as "rain likely" and watering is skipped, unless soil is critically dry (≤ 15 %). This needs a full 3 hours of readings after every power-up and is a rule of thumb, not a forecast.
- **Safety limits:** a cycle is cut off after 5 minutes; there is a 10-minute rest before the next one so water can soak in; three cycles in a row that hit the time limit without reaching the target trigger a **lockout** (empty tank or blocked pipe).
- **Sensor fault:** a soil reading stuck at the extremes (loose or shorted wire) keeps the pump off.
- The relay output is set to "off" *before* the pin becomes an output, so the pump doesn't twitch on at boot.

All numbers above are the defaults in `config.h`.

## Serial commands (9600 baud)

| Key | Action |
|---|---|
| `1` | Pump on (manual, still limited to the max run time) |
| `0` | Pump off (manual) |
| `a` | Back to automatic mode and clear a lockout |
| `s` | Print current readings and state |

## Optional: pump current protection

With `USE_CURRENT_SENSOR 1` the sketch reads the current sensor on A3 and stops the pump (with a lockout) on overcurrent (jam) or on no current while running (dry run / broken wire). This only works if the sensor's screw terminal is wired **in series with the pump** — leave it off otherwise, or the pump will be locked out on every start.
