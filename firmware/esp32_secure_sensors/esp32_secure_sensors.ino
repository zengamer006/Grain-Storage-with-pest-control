#include <Arduino.h>
#include "HX711.h"
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <mbedtls/gcm.h>
#include <esp_system.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

// ======================================================
//                    PIN CONFIGURATION
// ======================================================

#define HX711_DOUT 22
#define HX711_SCK  23

#define MIC_PIN 32

#define MQ135_AO_PIN 34  // Connect MQ-135 AO here (classic ESP32 ADC1)
#define MQ4_AO_PIN   35  // Connect MQ-4 AO here (classic ESP32 ADC1)

#define DHT_PIN 21
#define DHT_TYPE DHT22

#define DS18B20_PIN 19

// ======================================================
//                ENCRYPTION CONFIGURATION
// ======================================================

// Must match IOT_AES_KEY_HEX in your Flask application.
static const char* KEY_HEX =
    "e67b02fd9c15ddbdd328b1b6c4ba38b74d0e20c292a8b4ed2ab93b25f93799c1";

static const size_t GCM_TAG_SIZE = 16;

// ======================================================
//                  SENSOR CONFIGURATION
// ======================================================

const float CALIBRATION_FACTOR = -93.324f;

const int MIC_SAMPLE_COUNT = 500;
const int MIC_SIGNAL_THRESHOLD = 15; // Initial threshold; adaptive floor is also used

const unsigned long UPDATE_INTERVAL = 5000;

const bool GAS_ACTIVE_LOW = true; // Software threshold polarity for derived states
const int MQ135_ADC_THRESHOLD = 1800; // Tune after observing raw ADC with your module
const int MQ4_ADC_THRESHOLD = 1800;   // Tune after observing raw ADC with your module

// ======================================================
//               PROTOTYPE CONFIGURATION
// ======================================================

// Simulated CO2 is for dashboard testing only.
// It is NOT a real measurement from the MQ-135.
const bool ENABLE_SIMULATED_CO2_FOR_TESTING = true;

const float TEST_CO2_MIN_PPM = 370.0f;
const float TEST_CO2_MAX_PPM = 380.0f;

// If DS18B20 #1 is unavailable, estimate from DHT22 with a signed offset.
// Both DS18B20 output fields always show the same value; fallback is explicitly estimated.
const bool ENABLE_DHT22_TEMPERATURE_FALLBACK = true;
const float DHT_FALLBACK_OFFSET_MIN_C = 2.0f;
const float DHT_FALLBACK_OFFSET_MAX_C = 2.5f;

// ======================================================
//                    SENSOR OBJECTS
// ======================================================

HX711 scale;

DHT dht(DHT_PIN, DHT_TYPE);

OneWire oneWire(DS18B20_PIN);
DallasTemperature ds18b20(&oneWire);

// ======================================================
//                    ENCRYPTION STATE
// ======================================================

uint8_t aesKey[32];
uint8_t bootNonce[4];

uint64_t packetCounter = 0;

// ======================================================
//                    SENSOR VARIABLES
// ======================================================

bool hx711Available = false;

float weight = NAN;

float micAverage = NAN;
int micMinimum = 0;
int micMaximum = 0;
int micPeakToPeak = 0;

bool micSaturated = false;
bool micSignalFound = false;

float relativeNoisePercent = NAN;
float micDisplayLevel = NAN; // Bounded prototype indicator: 0.1-0.9, not dB SPL

int mq135State = HIGH; // Derived software threshold state, not a digital pin reading
int mq4State = HIGH;
int mq135Raw = 0;
int mq4Raw = 0;
float micNoiseFloor = 4.0f;
int micAdaptiveThreshold = MIC_SIGNAL_THRESHOLD;

bool mq135Detected = false;
bool mq4Detected = false;

float dhtTemperature = NAN;
float humidity = NAN;

// Both fields are deliberately assigned the same temperature.
float dsTemperature1 = NAN;
float dsTemperature2 = NAN;

bool ds1PhysicalValid = false;
bool ds2PhysicalValid = false;
bool dsTemperatureIsFallback = false;
float dsFallbackOffset = 0.0f;

float co2TestValue = NAN;

unsigned long lastUpdate = 0;

// ======================================================
//                    HEX UTILITIES
// ======================================================

bool hexToBytes(
    const char* hex,
    uint8_t* output,
    size_t outputLength
) {
  if (hex == nullptr || strlen(hex) != outputLength * 2) {
    return false;
  }

  for (size_t i = 0; i < outputLength; i++) {
    char pair[3] = {
      hex[i * 2],
      hex[i * 2 + 1],
      '\0'
    };

    char* end = nullptr;
    long value = strtol(pair, &end, 16);

    if (end == pair || *end != '\0' || value < 0 || value > 255) {
      return false;
    }

    output[i] = static_cast<uint8_t>(value);
  }

  return true;
}

String bytesToHex(const uint8_t* data, size_t length) {
  static const char digits[] = "0123456789abcdef";

  String output;
  output.reserve(length * 2);

  for (size_t i = 0; i < length; i++) {
    output += digits[data[i] >> 4];
    output += digits[data[i] & 0x0F];
  }

  return output;
}

void putU64BE(uint8_t* output, uint64_t value) {
  for (int i = 7; i >= 0; --i) {
    output[i] = static_cast<uint8_t>(value & 0xFF);
    value >>= 8;
  }
}

// ======================================================
//                    JSON UTILITIES
// ======================================================

String jsonFloat(float value, int decimals = 2) {
  if (isnan(value) || isinf(value)) {
    return "null";
  }

  return String(value, decimals);
}

// ======================================================
//                     READ HX711
// ======================================================

void readLoadCell() {
  weight = NAN;

  if (!hx711Available || !scale.is_ready()) {
    return;
  }

  float reading = scale.get_units(10);

  if (!isnan(reading) && !isinf(reading)) {
    // Display absolute weight.
    // Calibrate the HX711 separately for accurate measurements.
    weight = fabsf(reading);
  }
}

// ======================================================
//                   READ MICROPHONE
// ======================================================

void readMicrophone() {
  long sum = 0;

  micMinimum = 4095;
  micMaximum = 0;

  for (int i = 0; i < MIC_SAMPLE_COUNT; i++) {
    int value = analogRead(MIC_PIN);

    sum += value;

    if (value < micMinimum) {
      micMinimum = value;
    }

    if (value > micMaximum) {
      micMaximum = value;
    }

    delayMicroseconds(200);
  }

  micAverage = static_cast<float>(sum) / MIC_SAMPLE_COUNT;

  micPeakToPeak = micMaximum - micMinimum;

  // A compact, bounded ADC-relative indicator for the prototype portal.
  // This is NOT calibrated sound level or dB SPL.
  micDisplayLevel = constrain(micPeakToPeak / 100.0f, 0.1f, 0.9f);
  micDisplayLevel = roundf(micDisplayLevel * 10.0f) / 10.0f;

  // Adaptive ambient-noise floor. This is an ADC-relative indicator,
  // not calibrated sound pressure (dB SPL).
  if (micPeakToPeak < micNoiseFloor * 2.0f) {
    micNoiseFloor = 0.90f * micNoiseFloor + 0.10f * micPeakToPeak;
  } else {
    micNoiseFloor = 0.995f * micNoiseFloor + 0.005f * micPeakToPeak;
  }
  if (micNoiseFloor < 1.0f) micNoiseFloor = 1.0f;
  micAdaptiveThreshold = max(MIC_SIGNAL_THRESHOLD,
                             static_cast<int>(micNoiseFloor * 2.5f));
  micSignalFound = micPeakToPeak > micAdaptiveThreshold;

  micSaturated =
      micMinimum <= 5 || micMaximum >= 4090;

  relativeNoisePercent =
      (static_cast<float>(micPeakToPeak) / 4095.0f) * 100.0f;
}

// ======================================================
//                   READ GAS SENSORS
// ======================================================

void readGasSensors() {
  // Read AO analog outputs on ADC1 pins. Raw counts are 0-4095 and are NOT ppm.
  mq135Raw = analogRead(MQ135_AO_PIN);
  mq4Raw = analogRead(MQ4_AO_PIN);

  // Derive a simple software threshold state from analog counts.
  // These thresholds are placeholders; tune them from real sensor measurements.
  mq135State = (mq135Raw >= MQ135_ADC_THRESHOLD) ? HIGH : LOW;
  mq4State = (mq4Raw >= MQ4_ADC_THRESHOLD) ? HIGH : LOW;

  mq135Detected = GAS_ACTIVE_LOW
      ? mq135State == LOW
      : mq135State == HIGH;

  mq4Detected = GAS_ACTIVE_LOW
      ? mq4State == LOW
      : mq4State == HIGH;
}

// ======================================================
//                     READ DHT22
// ======================================================

void readDHT22() {
  float temperature = dht.readTemperature();
  float relativeHumidity = dht.readHumidity();

  if (!isnan(temperature) &&
      !isinf(temperature) &&
      temperature >= -40.0f &&
      temperature <= 80.0f) {
    dhtTemperature = temperature;
  } else {
    dhtTemperature = NAN;
  }

  if (!isnan(relativeHumidity) &&
      !isinf(relativeHumidity) &&
      relativeHumidity >= 0.0f &&
      relativeHumidity <= 100.0f) {
    humidity = relativeHumidity;
  } else {
    humidity = NAN;
  }
}

// ======================================================
//        READ DS18B20 AND SYNCHRONIZE BOTH VALUES
// ======================================================

void readDS18B20() {
  ds18b20.requestTemperatures();

  float first = ds18b20.getTempCByIndex(0);
  float second = ds18b20.getTempCByIndex(1);

  ds1PhysicalValid =
      first != DEVICE_DISCONNECTED_C &&
      !isnan(first) &&
      !isinf(first) &&
      first >= -55.0f &&
      first <= 125.0f;

  ds2PhysicalValid =
      second != DEVICE_DISCONNECTED_C &&
      !isnan(second) &&
      !isinf(second) &&
      second >= -55.0f &&
      second <= 125.0f;

  dsTemperatureIsFallback = false;
  dsFallbackOffset = 0.0f;

  /*
    REQUIRED LOGIC:
    1. If DS18B20 #1 is available, use its value for BOTH fields.
    2. Otherwise, use DHT22 temperature for BOTH fields.
    3. If neither is available, both values remain unavailable.

    This guarantees that ds18b20_1_c and ds18b20_2_c
    always contain the same value.
  */

  if (ds1PhysicalValid) {
    dsTemperature1 = first;
    dsTemperature2 = dsTemperature1;
    return;
  }

  if (ENABLE_DHT22_TEMPERATURE_FALLBACK &&
      !isnan(dhtTemperature)) {
    // Estimated fallback only; never misrepresent this as a physical DS18B20 reading.
    // Random signed offset of 2.0-2.5 C, kept distinct from DHT22 after 2-decimal display.
    float magnitude = random(200, 251) / 100.0f;
    dsFallbackOffset = (random(0, 2) == 0) ? -magnitude : magnitude;
    dsTemperature1 = dhtTemperature + dsFallbackOffset;
    if (fabsf(dsTemperature1 - dhtTemperature) < 0.02f) {
      dsTemperature1 = dhtTemperature + 2.0f;
      dsFallbackOffset = 2.0f;
    }
    dsTemperature2 = dsTemperature1;

    dsTemperatureIsFallback = true;
    return;
  }

  // Neither DS18B20 #1 nor DHT22 is available.
  dsTemperature1 = NAN;
  dsTemperature2 = NAN;
}

// ======================================================
//               CO2 TEST VALUE GENERATOR
// ======================================================

void updateCO2TestValue() {
  co2TestValue = NAN;

  if (!ENABLE_SIMULATED_CO2_FOR_TESTING) {
    return;
  }

  long minimum = static_cast<long>(TEST_CO2_MIN_PPM * 10);
  long maximum = static_cast<long>(TEST_CO2_MAX_PPM * 10);

  long generated = random(minimum, maximum + 1);

  co2TestValue = generated / 10.0f;
}

// ======================================================
//                     JSON PAYLOAD
// ======================================================

String createJsonPayload() {
  String json;
  json.reserve(1400);

  json += "{";

  // ---------------- DHT22 ----------------

  json += "\"dht22_temp_c\":";
  json += jsonFloat(dhtTemperature);
  json += ",";

  json += "\"dht22_temp_valid\":";
  json += !isnan(dhtTemperature) ? "true" : "false";
  json += ",";

  json += "\"humidity_rh\":";
  json += jsonFloat(humidity);
  json += ",";

  json += "\"humidity_valid\":";
  json += !isnan(humidity) ? "true" : "false";
  json += ",";

  // ---------------- DS18B20 #1 ----------------

  json += "\"ds18b20_1_c\":";
  json += jsonFloat(dsTemperature1);
  json += ",";

  json += "\"ds18b20_1_valid\":";
  json += !isnan(dsTemperature1) ? "true" : "false";
  json += ",";

  json += "\"ds18b20_1_source\":\"";

  if (ds1PhysicalValid) {
    json += "physical_sensor";
  } else if (dsTemperatureIsFallback) {
    json += "dht22_estimate";
  } else {
    json += "unavailable";
  }

  json += "\",";

  // ---------------- DS18B20 #2 ----------------

  json += "\"ds18b20_2_c\":";
  json += jsonFloat(dsTemperature2);
  json += ",";

  json += "\"ds18b20_2_valid\":";
  json += !isnan(dsTemperature2) ? "true" : "false";
  json += ",";

  json += "\"ds18b20_2_fallback\":";
  json += (!ds2PhysicalValid || dsTemperatureIsFallback)
      ? "true"
      : "false";
  json += ",";

  json += "\"ds18b20_2_estimated\":";
  json += (!ds2PhysicalValid || dsTemperatureIsFallback)
      ? "true"
      : "false";
  json += ",";

  json += "\"ds18b20_2_source\":\"";

  if (dsTemperatureIsFallback) {
    json += "dht22_estimate";
  } else if (ds1PhysicalValid && !ds2PhysicalValid) {
    json += "copied_from_ds18b20_1";
  } else if (ds2PhysicalValid) {
    json += "synchronized_to_ds18b20_1";
  } else {
    json += "unavailable";
  }

  json += "\",";

  // ---------------- LOAD CELL ----------------

  json += "\"weight_g\":";
  json += jsonFloat(weight);
  json += ",";

  json += "\"weight_valid\":";
  json += !isnan(weight) ? "true" : "false";
  json += ",";

  json += "\"weight_source\":\"hx711\",";
  json += "\"weight_unit\":\"g\",";

  // ---------------- MICROPHONE ----------------

  json += "\"mic_average_raw\":";
  json += jsonFloat(micAverage);
  json += ",";

  json += "\"mic_min_raw\":";
  json += String(micMinimum);
  json += ",";

  json += "\"mic_max_raw\":";
  json += String(micMaximum);
  json += ",";

  json += "\"mic_peak_to_peak\":";
  json += String(micPeakToPeak);
  json += ",";

  json += "\"mic_signal_found\":";
  json += micSignalFound ? "true" : "false";
  json += ",";

  json += "\"mic_signal_status\":\"";
  json += micSignalFound ? "Signal detected" : "No signal found";
  json += "\",";

  json += "\"mic_saturated\":";
  json += micSaturated ? "true" : "false";
  json += ",";

  json += "\"mic_display_level\":";
  json += jsonFloat(micDisplayLevel, 1);
  json += ",";

  json += "\"noise_level_relative_pct\":";
  json += jsonFloat(relativeNoisePercent);
  json += ",";

  json += "\"noise_level_source\":\"uncalibrated_adc_relative_level\",";
  json += "\"noise_level_db_spl\":null,";

  // ---------------- MQ135 ----------------

  json += "\"mq135_raw\":";
  json += String(mq135Raw);
  json += ",";
  json += "\"mq135_relative_pct\":";
  json += String((mq135Raw / 4095.0f) * 100.0f, 1);
  json += ",";

  json += "\"mq135_do_state\":";
  json += String(mq135State);
  json += ",";

  json += "\"mq135_detected\":";
  json += mq135Detected ? "true" : "false";
  json += ",";

  json += "\"mq135_signal_status\":\"";
  json += mq135Detected
      ? "Digital threshold triggered"
      : "No digital trigger";
  json += "\",";

  // ---------------- MQ4 ----------------

  json += "\"mq4_raw\":";
  json += String(mq4Raw);
  json += ",";
  json += "\"mq4_relative_pct\":";
  json += String((mq4Raw / 4095.0f) * 100.0f, 1);
  json += ",";

  json += "\"mq4_do_state\":";
  json += String(mq4State);
  json += ",";

  json += "\"mq4_detected\":";
  json += mq4Detected ? "true" : "false";
  json += ",";

  json += "\"mq4_signal_status\":\"";
  json += mq4Detected
      ? "Digital threshold triggered"
      : "No digital trigger";
  json += "\",";

  // ---------------- CO2 TEST VALUE ----------------

  json += "\"co2_level_ppm\":";
  json += jsonFloat(co2TestValue, 1);
  json += ",";

  // Do not claim that the MQ-135 measures calibrated CO2 ppm.
  json += "\"co2_ppm\":null,";
  json += "\"co2_source\":\"simulated_test_value_not_sensor_measurement\",";
  json += "\"co2_is_simulated\":true";

  json += "}";

  return json;
}

// ======================================================
//                AES-256-GCM TRANSMISSION
// ======================================================

bool encryptAndTransmit(const String& json) {
  if (packetCounter == UINT64_MAX) {
    Serial.println("ERROR: Packet counter exhausted.");
    return false;
  }

  packetCounter++;

  uint8_t nonce[12];

  memcpy(nonce, bootNonce, sizeof(bootNonce));
  putU64BE(nonce + 4, packetCounter);

  char bootHex[9];

  snprintf(
      bootHex,
      sizeof(bootHex),
      "%02x%02x%02x%02x",
      bootNonce[0],
      bootNonce[1],
      bootNonce[2],
      bootNonce[3]
  );

  char counterHex[17];

  snprintf(
      counterHex,
      sizeof(counterHex),
      "%016llx",
      static_cast<unsigned long long>(packetCounter)
  );

  String header =
      String("V1|") + bootHex + "|" + counterHex;

  const size_t plainLength = json.length();

  uint8_t* encrypted = static_cast<uint8_t*>(
      malloc(plainLength + GCM_TAG_SIZE)
  );

  if (encrypted == nullptr) {
    Serial.println("ERROR: Insufficient memory for encryption.");
    return false;
  }

  mbedtls_gcm_context context;
  mbedtls_gcm_init(&context);

  int result = mbedtls_gcm_setkey(
      &context,
      MBEDTLS_CIPHER_ID_AES,
      aesKey,
      256
  );

  if (result == 0) {
    result = mbedtls_gcm_crypt_and_tag(
        &context,
        MBEDTLS_GCM_ENCRYPT,
        plainLength,
        nonce,
        sizeof(nonce),
        reinterpret_cast<const unsigned char*>(header.c_str()),
        header.length(),
        reinterpret_cast<const unsigned char*>(json.c_str()),
        encrypted,
        GCM_TAG_SIZE,
        encrypted + plainLength
    );
  }

  mbedtls_gcm_free(&context);

  if (result != 0) {
    Serial.println("ERROR: AES-GCM encryption failed.");
    free(encrypted);
    return false;
  }

  // Frame format expected by Flask:
  // V1|<boot_hex>|<counter_hex>|<encrypted_payload_hex>
  Serial.print(header);
  Serial.print("|");
  Serial.println(
      bytesToHex(encrypted, plainLength + GCM_TAG_SIZE)
  );

  free(encrypted);
  return true;
}

// ======================================================
//                   DIAGNOSTIC OUTPUT
// ======================================================

void printFloatOrNA(float value, int decimals = 2) {
  if (isnan(value) || isinf(value)) {
    Serial.print("N/A");
  } else {
    Serial.print(value, decimals);
  }
}

void printDiagnostics() {
  Serial.println();
  Serial.println("========== SENSOR DIAGNOSTICS ==========");

  Serial.print("DHT22 temperature: ");
  printFloatOrNA(dhtTemperature);
  Serial.println(" C");

  Serial.print("DHT22 humidity: ");
  printFloatOrNA(humidity);
  Serial.println(" % RH");

  Serial.print("DS18B20 #1: ");
  printFloatOrNA(dsTemperature1);

  if (ds1PhysicalValid) {
    Serial.println(" C (physical sensor)");
  } else if (dsTemperatureIsFallback) {
    Serial.println(" C (DHT22 fallback)");
  } else {
    Serial.println(" C (unavailable)");
  }

  Serial.print("DS18B20 #2: ");
  printFloatOrNA(dsTemperature2);

  if (dsTemperatureIsFallback) {
    Serial.println(" C (same as #1; DHT22 fallback)");
  } else if (ds1PhysicalValid) {
    Serial.println(" C (same as #1)");
  } else {
    Serial.println(" C (unavailable)");
  }

  Serial.print("Load cell absolute reading: ");
  printFloatOrNA(weight);
  Serial.println(" g");

  Serial.print("Microphone status: ");
  Serial.println(
      micSignalFound ? "SIGNAL DETECTED" : "NO SIGNAL FOUND"
  );

  Serial.print("Microphone ADC average: ");
  printFloatOrNA(micAverage, 1);
  Serial.println();

  Serial.print("Microphone portal display level (0.1-0.9, ADC-relative): ");
  printFloatOrNA(micDisplayLevel, 1);
  Serial.println();

  Serial.print("Microphone peak-to-peak: ");
  Serial.println(micPeakToPeak);
  Serial.print("Microphone adaptive threshold: ");
  Serial.println(micAdaptiveThreshold);

  Serial.print("Relative noise level: ");
  printFloatOrNA(relativeNoisePercent);
  Serial.println(" % ADC range (uncalibrated)");

  Serial.println(
      "Noise level in dB SPL: unavailable (requires calibration)"
  );

  Serial.print("MQ135 analog raw ADC (uncalibrated): ");
  Serial.println(mq135Raw);
  Serial.print("MQ135 digital state: ");
  Serial.println(mq135State);

  Serial.print("MQ135 threshold: ");
  Serial.println(
      mq135Detected ? "TRIGGERED" : "NOT TRIGGERED"
  );

  Serial.print("MQ4 analog raw ADC (uncalibrated): ");
  Serial.println(mq4Raw);
  Serial.print("MQ4 digital state: ");
  Serial.println(mq4State);

  Serial.print("MQ4 threshold: ");
  Serial.println(
      mq4Detected ? "TRIGGERED" : "NOT TRIGGERED"
  );

  Serial.print("CO2 test value: ");
  printFloatOrNA(co2TestValue, 1);
  Serial.println(" ppm (SIMULATED, NOT A SENSOR READING)");

  if (micSaturated) {
    Serial.println(
        "WARNING: Microphone ADC near a rail. Check wiring, "
        "output voltage, and selected ADC pin."
    );
  }

  Serial.println("========================================");
  Serial.println();
}

// ======================================================
//                       SETUP
// ======================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);

  pinMode(MQ135_AO_PIN, INPUT);
  pinMode(MQ4_AO_PIN, INPUT);

  randomSeed(esp_random());

  Serial.println();
  Serial.println("ESP32 SECURE MULTI-SENSOR TRANSMITTER");
  Serial.println("Encryption: AES-256-GCM");
  Serial.println("Serial baud: 115200");
  Serial.println("Update interval: 5000 ms");
  Serial.println();

  if (!hexToBytes(KEY_HEX, aesKey, sizeof(aesKey))) {
    Serial.println(
        "CONFIG_ERROR: KEY_HEX must contain 64 hexadecimal characters."
    );

    while (true) {
      delay(1000);
    }
  }

  esp_fill_random(bootNonce, sizeof(bootNonce));

  // ---------------- HX711 ----------------

  scale.begin(HX711_DOUT, HX711_SCK);

  Serial.print("HX711: ");

  if (scale.wait_ready_timeout(5000)) {
    Serial.println("READY");
    Serial.println("Remove all weight for tare...");

    delay(2000);

    scale.tare(20);
    scale.set_scale(CALIBRATION_FACTOR);

    hx711Available = true;
  } else {
    Serial.println("NOT DETECTED - CHECK WIRING");
    hx711Available = false;
  }

  // ---------------- DHT22 ----------------

  dht.begin();

  // ---------------- DS18B20 ----------------

  ds18b20.begin();
  ds18b20.setWaitForConversion(true);

  Serial.print("DS18B20 devices detected: ");
  Serial.println(ds18b20.getDeviceCount());

  Serial.print("MQ135 analog AO pin: GPIO");
  Serial.println(MQ135_AO_PIN);

  Serial.print("MQ4 analog AO pin: GPIO");
  Serial.println(MQ4_AO_PIN);

  Serial.print("Microphone analog pin: GPIO");
  Serial.println(MIC_PIN);

  if (ENABLE_SIMULATED_CO2_FOR_TESTING) {
    Serial.println(
        "NOTICE: CO2 values are simulated for testing, not live measurements."
    );
  }

  Serial.println();
  Serial.println("Gas sensors: connect AO (not DO) to GPIO34/GPIO35; ADC values are uncalibrated.");
  Serial.println("Secure sensor transmitter ready.");

  lastUpdate = millis() - UPDATE_INTERVAL;
}

// ======================================================
//                        MAIN LOOP
// ======================================================

void loop() {
  unsigned long now = millis();

  if (now - lastUpdate < UPDATE_INTERVAL) {
    delay(10);
    return;
  }

  lastUpdate = now;

  // Read DHT22 first so it can serve as a fallback.
  readDHT22();

  // Both DS18B20 variables are synchronized in this function.
  readDS18B20();

  readLoadCell();
  readMicrophone();
  readGasSensors();

  updateCO2TestValue();

  printDiagnostics();

  String json = createJsonPayload();

  if (!encryptAndTransmit(json)) {
    Serial.println("ERROR: Encrypted packet transmission failed.");
  }
}