#include <LiquidCrystal.h>          // 16x2 LCD keypad shield
#include <DHT.h>                    // DHT sensor library
#include <stdlib.h>                 // dtostrf
#include <EEPROM.h>                 // EEPROM persistence
#include <math.h>                   // fabs

#ifdef __AVR__
  #include <avr/wdt.h>             // Watchdog for AVR boards
#endif

// ==================== Configuration Flags ====================
// Only numeric, label-based output for Serial Plotter (no debug chatter)
const bool ENABLE_DEBUG_LOGS = false;

// Plot preferences: use 0/1 flags for heater & humidifier on Serial Plotter
#define PLOT_BINARY_FLAGS 1  // 1: 0/1 flags; 0: simple normalized command example

// ==================== Hardware Pins ====================
#define DHT_PIN 2                   // DHT sensor pin
#define HEATER_PIN 3                // Heater output pin
#define HUMIDIFIER_PIN 11           // Humidifier (latching relay pulse) pin
#define KEYPAD_PIN A0               // LCD keypad shield buttons

#define DHT_TYPE DHT22              // Using DHT22 (AM2302)

// LCD keypad shield: rs, en, d4, d5, d6, d7
LiquidCrystal lcd(8, 9, 4, 5, 6, 7);
DHT dht(DHT_PIN, DHT_TYPE);

// ==================== Custom Characters ====================
// Degree symbol (slot 0)
byte degChar[8] = {
  B00111,
  B00101,
  B00111,
  B00000,
  B00000,
  B00000,
  B00000,
  B00000
};

// ==================== Timing ====================
const unsigned long SENSOR_INTERVAL = 2000UL;     // DHT22 recommended min interval (ms)
const unsigned long LCD_REFRESH_INTERVAL = 200UL; // LCD refresh period (ms)

unsigned long lastSensorRead = 0;
unsigned long lastLcdRefresh = 0;

const unsigned long HUMIDIFIER_ON_PULSE  = 200UL; // ms (LOW pulse to turn ON)
const unsigned long HUMIDIFIER_OFF_PULSE = 100UL; // ms (LOW pulse to turn OFF)
bool humidifierPulseActive = false;
bool humidifierPulseTargetOn = false;
unsigned long humidifierPulseStart = 0;

// ==================== Measurements & State ====================
float lastTemperature = 0.0f;
float lastHumidity = 0.0f;
bool lastReadOk = false;

bool heaterState = false;           // Directly driven via digitalWrite
bool humidifierState = false;       // Logical latch; physical change via pulse

float previousTemperature = NAN;
float previousHumidity = NAN;
float lastTemperatureSlope = 0.0f;
float lastHumiditySlope = 0.0f;
uint8_t consecutiveSensorFailures = 0;

// For Serial Plotter (0/1 flags or normalized)
float heaterCmdPlot = 0.0f;
float humidifierCmdPlot = 0.0f;

// ==================== Temperature Control Params ====================
const int TEMP_SETPOINT_DEFAULT = 40;   // °C
int targetTempSetpoint = TEMP_SETPOINT_DEFAULT;
const int TEMP_SETPOINT_MIN = 18;
const int TEMP_SETPOINT_MAX = 75;
const float TEMP_HYST_ON  = 0.1f;
const float TEMP_HYST_OFF = 0.3f;
const float TEMP_TREND_GAIN = 0.4f;

// ==================== Humidity Control Params ====================
const int HUM_SETPOINT_DEFAULT = 80;    // %RH
int targetHumSetpoint = HUM_SETPOINT_DEFAULT;
const int HUM_SETPOINT_MIN = 20;
const int HUM_SETPOINT_MAX = 100;
const float HUM_HYST_ON  = 2.5f;
const float HUM_HYST_OFF = 0.7f;
const float HUM_TREND_GAIN = 0.6f;

// ==================== Keypad & Selection ====================
enum Button {
  BUTTON_NONE,
  BUTTON_RIGHT,
  BUTTON_UP,
  BUTTON_DOWN,
  BUTTON_LEFT,
  BUTTON_SELECT
};

enum SetpointSelection {
  SELECT_TEMP,   // Adjust temperature setpoint
  SELECT_HUM     // Adjust humidity setpoint
};
SetpointSelection activeSelection = SELECT_TEMP;

// Debounce for analog keypad
const unsigned long KEY_DEBOUNCE_MS = 35UL;
static Button stableButton = BUTTON_NONE;
static Button lastRawButton = BUTTON_NONE;
static unsigned long lastBounceMs = 0;

// ==================== EEPROM Persistence ====================
// CRC-protected compact struct
struct Settings {
  uint16_t magic;
  int8_t   tempSet;
  int8_t   humSet;
  uint8_t  crc;
};
const int EE_ADDR = 0;
const uint16_t SETTINGS_MAGIC = 0xA5C3;

// Wear-friendly delayed save
bool pendingSettingsSave = false;
unsigned long lastSetpointEditMs = 0;
const unsigned long SETTINGS_SAVE_DELAY = 2000UL;

static uint8_t crc8(const uint8_t* d, size_t n) {
  uint8_t c = 0x00;
  while (n--) {
    uint8_t x = *d++ ^ c;
    for (uint8_t i=0; i<8; i++) x = (x & 0x80) ? (uint8_t)((x<<1) ^ 0x07) : (uint8_t)(x<<1);
    c = x;
  }
  return c;
}

void loadSettings() {
  Settings s;
  EEPROM.get(EE_ADDR, s);
  if (s.magic == SETTINGS_MAGIC) {
    uint8_t calc = crc8((uint8_t*)&s, sizeof(Settings)-1);
    if (calc == s.crc) {
      if (s.tempSet >= TEMP_SETPOINT_MIN && s.tempSet <= TEMP_SETPOINT_MAX)
        targetTempSetpoint = s.tempSet;
      if (s.humSet >= HUM_SETPOINT_MIN && s.humSet <= HUM_SETPOINT_MAX)
        targetHumSetpoint = s.humSet;
      return;
    }
  }
  // Else keep defaults
}

void saveSettingsNow() {
  Settings s;
  s.magic   = SETTINGS_MAGIC;
  s.tempSet = (int8_t)targetTempSetpoint;
  s.humSet  = (int8_t)targetHumSetpoint;
  s.crc     = crc8((uint8_t*)&s, sizeof(Settings)-1);
  EEPROM.put(EE_ADDR, s);
}

void scheduleSettingsSave(unsigned long now) {
  pendingSettingsSave = true;
  lastSetpointEditMs  = now;
}

void maybeFlushSettings(unsigned long now) {
  if (pendingSettingsSave && (now - lastSetpointEditMs >= SETTINGS_SAVE_DELAY)) {
    saveSettingsNow();
    pendingSettingsSave = false;
  }
}

// ==================== Forward Declarations ====================
void refreshLcd();
void controlOutputs(float temperature, float humidity, float tempSlope, float humSlope, unsigned long now);
void printPadded2(int value);
void printAlignedFloat(float value);
Button readKeypadButtonRaw();
Button readKeypadButtonDebounced(unsigned long now);
void handleKeypad(unsigned long now);
void updateHumidifierPulse(unsigned long now);
void startHumidifierPulse(unsigned long now, bool turnOn);

// ==================== Setup ====================
void setup() {
#ifdef __AVR__
  wdt_enable(WDTO_2S);   // 2s watchdog (safe for this sketch)
#endif

  Serial.begin(9600);

  pinMode(HEATER_PIN, OUTPUT);
  pinMode(HUMIDIFIER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);   // heater off
  digitalWrite(HUMIDIFIER_PIN, HIGH); // latching relay idle high

  lcd.begin(16, 2);
  lcd.createChar(0, degChar);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("DHT22 & LCD");
  lcd.setCursor(0, 1);
  lcd.print("Baslatiliyor...");

  loadSettings();       // restore setpoints if present
  dht.begin();

  lastSensorRead = millis() - SENSOR_INTERVAL; // force immediate first read
  lastLcdRefresh = millis();
}

// ==================== Main Loop ====================
void loop() {
  unsigned long now = millis();

  handleKeypad(now);  // user input

  // Periodic sensor read
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;

    float temperatureRaw = dht.readTemperature();
    float humidityRaw    = dht.readHumidity();

    bool spikeIgnored = false; // do not count as failure if just a spike
    lastReadOk = !isnan(temperatureRaw) && !isnan(humidityRaw);

    // Spike guard: ignore single unrealistic jumps (don't treat as failure)
    if (lastReadOk && !isnan(previousTemperature) && !isnan(previousHumidity)) {
      if (fabs(temperatureRaw - lastTemperature) > 10.0f ||
          fabs(humidityRaw    - lastHumidity)    > 20.0f) {
        if (ENABLE_DEBUG_LOGS) {
          Serial.println(F("Spike ignored: sensor jump too large."));
        }
        spikeIgnored = true;
        lastReadOk = false; // skip updates/printing this cycle
      }
    }

    if (lastReadOk) {
      consecutiveSensorFailures = 0;

      // Trend (slope) update
      if (!isnan(previousTemperature)) {
        lastTemperatureSlope = temperatureRaw - previousTemperature;
      } else {
        lastTemperatureSlope = 0.0f;
      }
      if (!isnan(previousHumidity)) {
        lastHumiditySlope = humidityRaw - previousHumidity;
      } else {
        lastHumiditySlope = 0.0f;
      }

      previousTemperature = temperatureRaw;
      previousHumidity    = humidityRaw;
      lastTemperature     = temperatureRaw;
      lastHumidity        = humidityRaw;

      controlOutputs(lastTemperature, lastHumidity, lastTemperatureSlope, lastHumiditySlope, now);

      // ===== Serial Plotter: label-based 4 series =====
      Serial.print(F("Temperature:"));
      Serial.print(lastTemperature, 1);
      Serial.print(F(" Humidity:"));
      Serial.print(lastHumidity, 1);
      Serial.print(F(" HeaterFlag:"));
      Serial.print(heaterCmdPlot, 3);        // 0.000 or 1.000
      Serial.print(F(" HumidifierFlag:"));
      Serial.println(humidifierCmdPlot, 3);  // 0.000 or 1.000
    } else {
      // Only increment failure counter if NOT a spike ignore
      if (!spikeIgnored) {
        consecutiveSensorFailures++;
        if (ENABLE_DEBUG_LOGS) {
          Serial.print(F("DHT22 okumasi basarisiz (#"));
          Serial.print(consecutiveSensorFailures);
          Serial.println(F(")"));
        }
        if (consecutiveSensorFailures >= 3) {
          if (ENABLE_DEBUG_LOGS) {
            Serial.println(F("3 ardısik hata -> DHT yeniden baslatiliyor."));
          }
          dht.begin();
          consecutiveSensorFailures = 0;
        }
      }
    }
  }

  updateHumidifierPulse(now);

  // LCD refresh
  if (now - lastLcdRefresh >= LCD_REFRESH_INTERVAL) {
    lastLcdRefresh = now;
    refreshLcd();
  }

  // EEPROM delayed save (wear-friendly)
  maybeFlushSettings(now);

#ifdef __AVR__
  wdt_reset();  // keep watchdog happy
#endif
}

// ==================== UI & Display ====================
void refreshLcd() {
  if (lastReadOk) {
    consecutiveSensorFailures = 0;
    lcd.setCursor(0, 0);
    lcd.print("T:");
    printAlignedFloat(lastTemperature);
    lcd.write(byte(0));
    lcd.print('C');
    lcd.print(' ');

    lcd.setCursor(9, 0);
    lcd.print("H:");
    printAlignedFloat(lastHumidity);
    lcd.print('%');

    lcd.setCursor(0, 1);
    lcd.print(activeSelection == SELECT_TEMP ? "T>" : "T ");
    printPadded2(targetTempSetpoint);
    lcd.write(byte(0));
    lcd.print('C');
    lcd.print(' ');

    lcd.print(activeSelection == SELECT_HUM ? "H>" : "H ");
    printPadded2(targetHumSetpoint);
    lcd.print('%');
    lcd.print("   ");

    lcd.setCursor(12, 1);
    lcd.print(heaterState ? "IA" : "IK");
    lcd.setCursor(14, 1);
    lcd.print(humidifierState ? "NA" : "NK");
  } else {
    lcd.setCursor(0, 0);
    lcd.print("DHT22 Hata!    ");
    lcd.setCursor(0, 1);
    lcd.print("Hata say: ");
    lcd.print(consecutiveSensorFailures);
    lcd.print("   ");
  }
}

void printPadded2(int value) {
  if (value < 10 && value >= 0) lcd.print(' ');
  lcd.print(value);
}

void printAlignedFloat(float value) {
  char buffer[8];
  dtostrf(value, 4, 1, buffer);
  lcd.print(buffer);
}

// ==================== Keypad Handling ====================
Button readKeypadButtonRaw() {
  int analogValue = analogRead(KEYPAD_PIN);
  if (analogValue > 1000) return BUTTON_NONE;
  else if (analogValue < 60)   return BUTTON_RIGHT;
  else if (analogValue < 200)  return BUTTON_UP;
  else if (analogValue < 400)  return BUTTON_DOWN;
  else if (analogValue < 600)  return BUTTON_LEFT;
  else if (analogValue < 800)  return BUTTON_SELECT;
  return BUTTON_NONE;
}

Button readKeypadButtonDebounced(unsigned long now) {
  Button raw = readKeypadButtonRaw();
  if (raw != lastRawButton) {
    lastRawButton = raw;
    lastBounceMs = now;
  }
  if (now - lastBounceMs >= KEY_DEBOUNCE_MS) {
    stableButton = lastRawButton;
  }
  return stableButton;
}

void handleKeypad(unsigned long now) {
  static Button lastButton = BUTTON_NONE;
  Button currentButton = readKeypadButtonDebounced(now);

  if (currentButton != lastButton) {
    if (currentButton != BUTTON_NONE) {
      bool updated = false;
      bool selectionChanged = false;

      switch (currentButton) {
        case BUTTON_LEFT:
          if (activeSelection != SELECT_TEMP) {
            activeSelection = SELECT_TEMP;
            selectionChanged = true;
          }
          break;
        case BUTTON_RIGHT:
          if (activeSelection != SELECT_HUM) {
            activeSelection = SELECT_HUM;
            selectionChanged = true;
          }
          break;
        case BUTTON_UP:
          if (activeSelection == SELECT_TEMP) {
            if (targetTempSetpoint < TEMP_SETPOINT_MAX) {
              targetTempSetpoint++;
              updated = true;
            }
          } else {
            if (targetHumSetpoint < HUM_SETPOINT_MAX) {
              targetHumSetpoint++;
              updated = true;
            }
          }
          break;
        case BUTTON_DOWN:
          if (activeSelection == SELECT_TEMP) {
            if (targetTempSetpoint > TEMP_SETPOINT_MIN) {
              targetTempSetpoint--;
              updated = true;
            }
          } else {
            if (targetHumSetpoint > HUM_SETPOINT_MIN) {
              targetHumSetpoint--;
              updated = true;
            }
          }
          break;
        case BUTTON_SELECT:
          if (activeSelection == SELECT_TEMP) {
            targetTempSetpoint = TEMP_SETPOINT_DEFAULT;
            updated = true;
          } else {
            targetHumSetpoint = HUM_SETPOINT_DEFAULT;
            updated = true;
          }
          break;
        default: break;
      }

      if (selectionChanged) {
        if (ENABLE_DEBUG_LOGS) {
          Serial.print(F("Ayarlanan hedef: "));
          Serial.println(activeSelection == SELECT_TEMP ? F("Sicaklik") : F("Nem"));
        }
      }

      if (updated) {
        scheduleSettingsSave(now); // EEPROM save (delayed)
        if (ENABLE_DEBUG_LOGS) {
          if (activeSelection == SELECT_TEMP) {
            Serial.print(F("Yeni sicaklik setpoint: "));
            Serial.println(targetTempSetpoint);
          } else {
            Serial.print(F("Yeni nem setpoint: "));
            Serial.println(targetHumSetpoint);
          }
        }
        if (lastReadOk) {
          controlOutputs(lastTemperature, lastHumidity, lastTemperatureSlope, lastHumiditySlope, now);
        }
      }
    }
    lastButton = currentButton;
  }
}

// ==================== Humidifier Pulse Handling ====================
void startHumidifierPulse(unsigned long now, bool turnOn) {
  humidifierPulseActive = true;
  humidifierPulseTargetOn = turnOn;
  humidifierPulseStart = now;
  digitalWrite(HUMIDIFIER_PIN, LOW); // start LOW pulse
}

void updateHumidifierPulse(unsigned long now) {
  if (humidifierPulseActive) {
    unsigned long pulseDuration = humidifierPulseTargetOn ? HUMIDIFIER_ON_PULSE : HUMIDIFIER_OFF_PULSE;
    if (now - humidifierPulseStart >= pulseDuration) {
      humidifierPulseActive = false;
      digitalWrite(HUMIDIFIER_PIN, HIGH); // release back to idle
    }
  }
}

// ==================== Control Logic ====================
void controlOutputs(float temperature, float humidity, float tempSlope, float humSlope, unsigned long now) {
  // ----- Temperature: trend-aware hysteresis
  float heatOnBand  = TEMP_HYST_ON;
  float heatOffBand = TEMP_HYST_OFF;

  if (tempSlope < -0.15f) {
    float boost = (-tempSlope) * TEMP_TREND_GAIN;
    float maxBoost = TEMP_HYST_ON - TEMP_HYST_OFF;
    if (boost > maxBoost) boost = maxBoost;
    heatOnBand = TEMP_HYST_ON - boost;
    if (heatOnBand < TEMP_HYST_OFF) heatOnBand = TEMP_HYST_OFF;
  }

  if (tempSlope > 0.15f) {
    float tighten = tempSlope * TEMP_TREND_GAIN;
    if (tighten > heatOffBand - 0.05f) tighten = heatOffBand - 0.05f;
    heatOffBand -= tighten;
    if (heatOffBand < 0.05f) heatOffBand = 0.05f;
  }

  float heatOnThreshold  = targetTempSetpoint - heatOnBand;
  float heatOffThreshold = targetTempSetpoint + heatOffBand;

  if (temperature <= heatOnThreshold) heaterState = true;
  else if (temperature >= heatOffThreshold) heaterState = false;

  digitalWrite(HEATER_PIN, heaterState ? HIGH : LOW);

  // ----- Humidity: trend-aware hysteresis with latching relay pulse
  float humOnBand  = HUM_HYST_ON;
  float humOffBand = HUM_HYST_OFF;

  if (humSlope < -0.3f) {
    float tighten = (-humSlope) * HUM_TREND_GAIN;
    float maxTighten = HUM_HYST_ON - (HUM_HYST_OFF + 0.3f);
    if (tighten > maxTighten) tighten = maxTighten;
    humOnBand = HUM_HYST_ON - tighten;
    if (humOnBand < HUM_HYST_OFF + 0.3f) humOnBand = HUM_HYST_OFF + 0.3f;
  }

  if (humSlope > 0.3f) {
    float tighten = humSlope * HUM_TREND_GAIN;
    if (tighten > humOffBand - 0.2f) tighten = humOffBand - 0.2f;
    humOffBand -= tighten;
    if (humOffBand < 0.2f) humOffBand = 0.2f;
  }

  float humOnThreshold  = targetHumSetpoint - humOnBand;
  float humOffThreshold = targetHumSetpoint + humOffBand;

  bool desiredHumidifierState = humidifierState;

  if (humidity <= humOnThreshold) desiredHumidifierState = true;
  else if (humidity >= humOffThreshold) desiredHumidifierState = false;

  if (desiredHumidifierState != humidifierState) {
    humidifierState = desiredHumidifierState;
    startHumidifierPulse(now, humidifierState);
    if (ENABLE_DEBUG_LOGS) {
      Serial.print(F("Nemlendirici durumu: "));
      Serial.println(humidifierState ? F("Acik") : F("Kapali"));
    }
  }

#if PLOT_BINARY_FLAGS
  heaterCmdPlot     = heaterState ? 1.0f : 0.0f;
  humidifierCmdPlot = humidifierState ? 1.0f : 0.0f;
#else
  // Simple normalized example (customize as needed)
  float tempErr = (float)targetTempSetpoint - temperature;
  heaterCmdPlot     = tempErr > 0 ? 1.0f : 0.0f;
  float humErr  = (float)targetHumSetpoint - humidity;
  humidifierCmdPlot = humErr > 0 ? 1.0f : 0.0f;
#endif
}
