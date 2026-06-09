#include <Arduino.h>

// Sovereignty One — ESP32 CDI/MED Hybrid Water System Controller
//
// Controls a Capacitive Deionization (CDI) + Membrane Electrodialysis (MED)
// hybrid water purification system.
//
// Features:
//  - Pump control (CDI / MED switching based on PV voltage & TDS thresholds)
//  - Anti-scaling polarity reversal (automatic scheduling)
//  - Safety interlocks (over-pressure, dry-run, temp limits)
//  - Serial telemetry (JSON output at 1 Hz)
//
// Pin assignments and thresholds configurable via #define constants below.

#define PIN_PUMP_RELAY             18
#define PIN_CDI_RELAY              19
#define PIN_MED_RELAY              21
#define PIN_POLARITY_RELAY         22
#define PIN_STATUS_LED             2

#define PIN_PV_VOLTAGE_SENSE       36
#define PIN_TDS_SENSE              39
#define PIN_PRESSURE_SENSE         34
#define PIN_TEMPERATURE_SENSE      35
#define PIN_DRY_RUN_SWITCH         27

#define RELAY_ACTIVE_HIGH          true
#define DRY_RUN_ACTIVE_LOW         true

#define ADC_MAX_COUNTS             4095.0f
#define ADC_REFERENCE_VOLTAGE      3.3f

#define PV_DIVIDER_RATIO           6.20f
#define TDS_SENSOR_SCALE_PPM_PER_V 1000.0f
#define PRESSURE_SENSOR_SCALE_KPA  100.0f
#define TEMP_SENSOR_SCALE_C_PER_V   100.0f
#define TEMP_SENSOR_OFFSET_C       -50.0f

#define PV_MIN_FOR_CDI_V           13.0f
#define PV_MIN_FOR_MED_V           14.0f
#define TDS_MIN_FOR_CDI_PPM        300.0f
#define TDS_MIN_FOR_MED_PPM        1200.0f

#define MAX_PRESSURE_KPA           300.0f
#define MAX_TEMPERATURE_C           55.0f
#define MIN_TEMPERATURE_C            0.0f

#define POLARITY_REVERSE_INTERVAL_MS  1800000UL
#define POLARITY_REVERSE_DURATION_MS     3000UL
#define TELEMETRY_INTERVAL_MS           1000UL
#define CONTROL_LOOP_INTERVAL_MS          100UL

#define FILTER_ALPHA                    0.25f

enum SystemMode : uint8_t {
  MODE_OFF = 0,
  MODE_CDI,
  MODE_MED,
  MODE_FAULT
};

struct TelemetryState {
  float pvVoltage = 0.0f;
  float tdsPpm = 0.0f;
  float pressureKpa = 0.0f;
  float temperatureC = 0.0f;
  SystemMode mode = MODE_OFF;
  bool pumpEnabled = false;
  bool polarityReversing = false;
  bool faultActive = false;
  const char *faultReason = "none";
  unsigned long modeEnteredAtMs = 0;
  unsigned long lastTelemetryAtMs = 0;
  unsigned long lastControlAtMs = 0;
  unsigned long lastPolarityReverseAtMs = 0;
};

TelemetryState state;

static inline void writeRelay(uint8_t pin, bool active) {
  digitalWrite(pin, (RELAY_ACTIVE_HIGH ? active : !active) ? HIGH : LOW);
}

static float readScaledVoltage(uint8_t pin) {
  const float adc = static_cast<float>(analogRead(pin));
  return (adc / ADC_MAX_COUNTS) * ADC_REFERENCE_VOLTAGE;
}

static float smoothValue(float current, float sample) {
  if (current <= 0.0f) {
    return sample;
  }
  return current + (FILTER_ALPHA * (sample - current));
}

static float readPvVoltage() {
  return readScaledVoltage(PIN_PV_VOLTAGE_SENSE) * PV_DIVIDER_RATIO;
}

static float readTdsPpm() {
  return readScaledVoltage(PIN_TDS_SENSE) * TDS_SENSOR_SCALE_PPM_PER_V;
}

static float readPressureKpa() {
  return readScaledVoltage(PIN_PRESSURE_SENSE) * PRESSURE_SENSOR_SCALE_KPA;
}

static float readTemperatureC() {
  return (readScaledVoltage(PIN_TEMPERATURE_SENSE) * TEMP_SENSOR_SCALE_C_PER_V) +
         TEMP_SENSOR_OFFSET_C;
}

static bool dryRunDetected() {
  const int level = digitalRead(PIN_DRY_RUN_SWITCH);
  return DRY_RUN_ACTIVE_LOW ? (level == LOW) : (level == HIGH);
}

static const char *modeName(SystemMode mode) {
  switch (mode) {
    case MODE_CDI: return "CDI";
    case MODE_MED: return "MED";
    case MODE_FAULT: return "FAULT";
    case MODE_OFF:
    default: return "OFF";
  }
}

static bool evaluateSafety(float pressureKpa, float temperatureC, bool dryRun, const char **reason) {
  if (dryRun) {
    *reason = "dry_run";
    return false;
  }
  if (pressureKpa > MAX_PRESSURE_KPA) {
    *reason = "over_pressure";
    return false;
  }
  if (temperatureC > MAX_TEMPERATURE_C) {
    *reason = "temp_high";
    return false;
  }
  if (temperatureC < MIN_TEMPERATURE_C) {
    *reason = "temp_low";
    return false;
  }
  *reason = "none";
  return true;
}

static SystemMode selectMode(float pvVoltage, float tdsPpm) {
  if (pvVoltage >= PV_MIN_FOR_MED_V && tdsPpm >= TDS_MIN_FOR_MED_PPM) {
    return MODE_MED;
  }
  if (pvVoltage >= PV_MIN_FOR_CDI_V && tdsPpm >= TDS_MIN_FOR_CDI_PPM) {
    return MODE_CDI;
  }
  return MODE_OFF;
}

static void applyOutputs(SystemMode mode, bool polarityReversing) {
  const bool pumpOn = (mode == MODE_CDI || mode == MODE_MED) && !state.faultActive;

  writeRelay(PIN_PUMP_RELAY, pumpOn);
  writeRelay(PIN_CDI_RELAY, mode == MODE_CDI);
  writeRelay(PIN_MED_RELAY, mode == MODE_MED);
  writeRelay(PIN_POLARITY_RELAY, polarityReversing);
  digitalWrite(PIN_STATUS_LED, pumpOn ? HIGH : LOW);

  state.pumpEnabled = pumpOn;
  state.mode = mode;
  state.polarityReversing = polarityReversing;
}

static void updatePolaritySchedule(unsigned long nowMs) {
  if (state.mode != MODE_MED || state.faultActive) {
    state.polarityReversing = false;
    state.lastPolarityReverseAtMs = nowMs;
    return;
  }

  if (!state.polarityReversing &&
      (nowMs - state.lastPolarityReverseAtMs) >= POLARITY_REVERSE_INTERVAL_MS) {
    state.polarityReversing = true;
    state.modeEnteredAtMs = nowMs;
    return;
  }

  if (state.polarityReversing &&
      (nowMs - state.modeEnteredAtMs) >= POLARITY_REVERSE_DURATION_MS) {
    state.polarityReversing = false;
    state.lastPolarityReverseAtMs = nowMs;
  }
}

static void publishTelemetry(unsigned long nowMs, const char *faultReason) {
  Serial.print(F("{\"ts_ms\":"));
  Serial.print(nowMs);
  Serial.print(F(",\"mode\":\""));
  Serial.print(modeName(state.mode));
  Serial.print(F("\",\"pv_v\":"));
  Serial.print(state.pvVoltage, 2);
  Serial.print(F(",\"tds_ppm\":"));
  Serial.print(state.tdsPpm, 1);
  Serial.print(F(",\"pressure_kpa\":"));
  Serial.print(state.pressureKpa, 1);
  Serial.print(F(",\"temp_c\":"));
  Serial.print(state.temperatureC, 1);
  Serial.print(F(",\"pump\":"));
  Serial.print(state.pumpEnabled ? 1 : 0);
  Serial.print(F(",\"reverse\":"));
  Serial.print(state.polarityReversing ? 1 : 0);
  Serial.print(F(",\"fault\":\""));
  Serial.print(faultReason);
  Serial.println(F("\"}"));
}

static void sampleInputs() {
  state.pvVoltage = smoothValue(state.pvVoltage, readPvVoltage());
  state.tdsPpm = smoothValue(state.tdsPpm, readTdsPpm());
  state.pressureKpa = smoothValue(state.pressureKpa, readPressureKpa());
  state.temperatureC = smoothValue(state.temperatureC, readTemperatureC());
}

void setup() {
  pinMode(PIN_PUMP_RELAY, OUTPUT);
  pinMode(PIN_CDI_RELAY, OUTPUT);
  pinMode(PIN_MED_RELAY, OUTPUT);
  pinMode(PIN_POLARITY_RELAY, OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);
  pinMode(PIN_DRY_RUN_SWITCH, INPUT_PULLUP);

  analogReadResolution(12);

  writeRelay(PIN_PUMP_RELAY, false);
  writeRelay(PIN_CDI_RELAY, false);
  writeRelay(PIN_MED_RELAY, false);
  writeRelay(PIN_POLARITY_RELAY, false);
  digitalWrite(PIN_STATUS_LED, LOW);

  Serial.begin(115200);
  delay(200);

  state.lastTelemetryAtMs = millis();
  state.lastControlAtMs = millis();
  state.lastPolarityReverseAtMs = millis();
}

void loop() {
  const unsigned long nowMs = millis();

  if ((nowMs - state.lastControlAtMs) >= CONTROL_LOOP_INTERVAL_MS) {
    state.lastControlAtMs = nowMs;

    sampleInputs();

    const bool dryRun = dryRunDetected();
    const char *faultReason = "none";
    const bool safetyOk = evaluateSafety(state.pressureKpa, state.temperatureC, dryRun, &faultReason);

    state.faultActive = !safetyOk;
    state.faultReason = faultReason;

    if (state.faultActive) {
      applyOutputs(MODE_FAULT, false);
    } else {
      const SystemMode requestedMode = selectMode(state.pvVoltage, state.tdsPpm);

      if (requestedMode != state.mode) {
        state.modeEnteredAtMs = nowMs;
      }

      state.mode = requestedMode;
      updatePolaritySchedule(nowMs);
      applyOutputs(requestedMode, state.polarityReversing);
    }
  }

  if ((nowMs - state.lastTelemetryAtMs) >= TELEMETRY_INTERVAL_MS) {
    state.lastTelemetryAtMs = nowMs;
    publishTelemetry(nowMs, state.faultReason);
  }
}
