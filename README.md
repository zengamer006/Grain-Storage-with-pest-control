# ESP32 Secure Telemetry + Flask + LSTM Starter

This is a starter project for the agreed architecture:

ESP32 sensors -> encryption on ESP32 -> encrypted USB serial (COM6) -> Python authentication/decryption -> Flask dashboard -> optional LSTM inference.

## Important implementation choice

This starter uses **AES-256-GCM** on both the ESP32 (ESP-IDF mbedTLS bundled with Arduino-ESP32) and Python (`cryptography`). This is a practical, authenticated-encryption path with a directly compatible ESP32 API. It is **not ASCON**. Your uploaded crypto notebook contains an ASCON design, but that is a different protocol and cannot be mixed with AES-GCM. If you require ASCON specifically, replace both ends together using the same NIST SP 800-232 implementation and protocol; do not mix variants.

The starter is an integration baseline, not a production-certified security product. Test on your exact board and Arduino-ESP32 core version before relying on it.

## Sensors / pins

- DS18B20 bus: GPIO 4 (both probes share the bus; 4.7 kΩ pull-up to 3.3 V)
- DHT22 data: GPIO 16
- MQ-135 analog out: GPIO 34
- MQ-4 analog out: GPIO 35
- MQ modules may be powered from 5 V, but **never feed more than 3.3 V to an ESP32 ADC pin**. Use a suitable divider/conditioning circuit.
- Failed DS18B20 readings fall back to DHT22 temperature and are explicitly marked as fallback.

## Install (Windows)

1. Install Arduino IDE and the ESP32 board package.
2. Install Arduino libraries: `OneWire`, `DallasTemperature`, `DHT sensor library` (Adafruit), `Adafruit Unified Sensor`.
3. Open `firmware/esp32_secure_sensors.ino`.
4. Replace `REPLACE_WITH_64_HEX_CHARACTERS` with a randomly generated 32-byte key represented as 64 hex characters. Use the exact same key in `app.py` as environment variable `IOT_AES_KEY_HEX`. Do not share the key or commit it to public repositories.
5. Select your exact ESP32 board and COM port, upload.
6. Close Arduino Serial Monitor before running Flask; only one process can own COM6.
7. In a terminal at this folder:
   ```powershell
   python -m venv .venv
   .venv\Scripts\activate
   pip install -r requirements.txt
   $env:SERIAL_PORT="COM6"
   $env:IOT_AES_KEY_HEX="YOUR_SAME_64_HEX_CHARACTER_KEY"
   python app.py
   ```
8. Open http://127.0.0.1:5000

## Generate a development key

Run once in Python:
```powershell
python -c "import secrets; print(secrets.token_hex(32))"
```
Copy the resulting 64 hex characters to the firmware and environment variable. For a real deployment, use secure provisioning and do not expose the key.

## Protocol

Each serial line is:
`V1|<8-hex-char boot nonce>|<16-hex-char counter>|<hex ciphertext and 16-byte GCM tag>`

The 12-byte AES-GCM nonce is `boot_nonce (4 bytes) || counter (8 bytes)`. A fresh random boot nonce is generated at every boot, and the counter increments for each packet. Header fields are authenticated as associated data. Python rejects authentication failures and duplicate/replayed counters for a boot nonce.

## LSTM integration status

The uploaded PestCalculationmodel notebook expects 19 input features, while these sensors supply only temperature, humidity, and relative MQ analog signals. Therefore this starter intentionally shows **prediction unavailable** until the trained model, fitted preprocessor, label encoder, and a valid complete feature mapping are supplied. It does not invent missing crop/pest/soil/vision values.

Export from the notebook:
- `models/pest_lstm.keras`
- `models/preprocessor.joblib`
- `models/label_encoder.joblib`

Then implement the mapping in `model_service.py` using valid sources for all 19 fields. The included service safely reports unavailable until that mapping is completed.

## Data persistence

Database and CSV persistence are intentionally not included in this first milestone, as agreed. First verify encrypted serial reception, authenticated decryption, sensor display, and model-artifact compatibility. Persistence can be added after that passes.

## Troubleshooting

- `PermissionError` on COM6: close Arduino Serial Monitor/Plotter and any other serial app.
- Authentication failures: keys differ, frame corruption, wrong firmware version, or key pasted incorrectly.
- MQ percentage is relative ADC signal, not gas ppm or atmospheric gas percentage.
- The fallback DS18B20 display value is copied from DHT22 and is not an independent measurement.
"# Grain-Storage-with-pest-control" 
