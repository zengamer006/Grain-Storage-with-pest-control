import os
import json
import threading
import time
from collections import deque
from datetime import datetime, timezone

import serial
from flask import Flask, jsonify, render_template, request
from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from model_service import predict_if_available

app = Flask(__name__)


# ============================================================
# CONFIGURATION
# ============================================================

SERIAL_PORT = os.getenv("SERIAL_PORT", "COM6")
BAUD_RATE = int(os.getenv("SERIAL_BAUD", "115200"))
KEY_HEX = os.getenv("IOT_AES_KEY_HEX", "").strip()

if len(KEY_HEX) != 64:
    raise RuntimeError(
        "Set IOT_AES_KEY_HEX to the same 64-hex-character "
        "AES-256 key configured in the ESP32 firmware."
    )

try:
    KEY = bytes.fromhex(KEY_HEX)
except ValueError as exc:
    raise RuntimeError(
        "IOT_AES_KEY_HEX must contain hexadecimal characters."
    ) from exc

aesgcm = AESGCM(KEY)


# ============================================================
# SHARED DATA
# ============================================================

lock = threading.Lock()

history = deque(maxlen=300)

status = {
    "serial_connected": False,
    "last_packet_at": None,
    "accepted_packets": 0,
    "rejected_packets": 0,
    "last_error": "Waiting for serial connection",
}

# Track packet counters separately for each ESP32 boot nonce.
highest_counter_by_boot = {}


# ============================================================
# SENSOR PAYLOAD VALIDATION
# ============================================================

def validate_payload(payload):
    """
    Validate the decrypted JSON payload.

    Expected sensor keys may include:
        dht22_temp_c
        humidity_rh
        ds18b20_1_c
        ds18b20_2_c
        co2_ppm
        mq135_raw
        mq135_relative_pct
        mq4_raw

    Unknown keys are preserved for forward compatibility.
    """

    if not isinstance(payload, dict):
        raise ValueError(
            "Decrypted sensor payload must be a JSON object."
        )

    # Ensure the JSON payload can be serialized and is not excessively large.
    try:
        encoded = json.dumps(payload, allow_nan=False)
    except (TypeError, ValueError) as exc:
        raise ValueError(
            "Sensor payload contains invalid JSON values."
        ) from exc

    if len(encoded) > 50000:
        raise ValueError("Sensor payload is too large.")

    # Reject non-numeric values for known numeric sensor fields.
    numeric_sensor_fields = [
        "dht22_temp_c",
        "humidity_rh",
        "dht22_humidity",
        "humidity_percentage",
        "ds18b20_1_c",
        "ds18b20_2_c",
        "co2_ppm",
        "co2_level_ppm",
        "mq135_raw",
        "mq135_relative_pct",
        "mq4_raw",
        "mq4_relative_pct",
        "temperature_c",
        "soil_moisture_percentage",
        "light_intensity_lux",
    ]

    for field in numeric_sensor_fields:
        if field not in payload or payload[field] is None:
            continue

        value = payload[field]

        if isinstance(value, bool):
            raise ValueError(
                f"Sensor field '{field}' must be numeric."
            )

        try:
            number = float(value)
        except (TypeError, ValueError) as exc:
            raise ValueError(
                f"Sensor field '{field}' must be numeric."
            ) from exc

        import math

        if not math.isfinite(number):
            raise ValueError(
                f"Sensor field '{field}' must be finite."
            )

        payload[field] = number

    return payload


# ============================================================
# AES-GCM FRAME PARSER
# ============================================================

def parse_frame(line: str):
    """
    Decode the ESP32 encrypted frame.

    Format:
        V1|<8-character boot nonce hex>|<16-character counter hex>|<ciphertext+tag hex>
    """

    parts = line.strip().split("|")

    if len(parts) != 4 or parts[0] != "V1":
        raise ValueError("Invalid encrypted frame format.")

    boot_hex, counter_hex, payload_hex = parts[1:]

    if len(boot_hex) != 8 or len(counter_hex) != 16:
        raise ValueError("Invalid encrypted frame header length.")

    try:
        boot = bytes.fromhex(boot_hex)
        counter = int(counter_hex, 16)
        ciphertext_and_tag = bytes.fromhex(payload_hex)
    except ValueError as exc:
        raise ValueError(
            "Frame contains invalid hexadecimal data."
        ) from exc

    if len(ciphertext_and_tag) < 16:
        raise ValueError("Encrypted payload is too short.")

    nonce = boot + counter.to_bytes(8, "big")

    aad = (
        b"V1|"
        + boot_hex.encode("ascii")
        + b"|"
        + counter_hex.encode("ascii")
    )

    plaintext = aesgcm.decrypt(
        nonce,
        ciphertext_and_tag,
        aad,
    )

    payload = json.loads(
        plaintext.decode("utf-8")
    )

    payload = validate_payload(payload)

    return boot_hex, counter, payload


# ============================================================
# SERIAL READER THREAD
# ============================================================

def serial_worker():
    """
    Continuously read ESP32 encrypted sensor frames.

    Non-protocol lines, such as ordinary Serial Monitor output,
    are ignored. The ESP32 must transmit valid V1 encrypted frames.
    """

    while True:
        try:
            with serial.Serial(
                SERIAL_PORT,
                BAUD_RATE,
                timeout=1,
            ) as ser:

                with lock:
                    status["serial_connected"] = True
                    status["last_error"] = None

                while True:
                    raw = ser.readline()

                    if not raw:
                        continue

                    try:
                        line = raw.decode(
                            "ascii",
                            errors="strict",
                        ).strip()

                        # Ignore boot messages and plain-text sensor tables.
                        if not line.startswith("V1|"):
                            continue

                        boot_hex, counter, payload = parse_frame(line)

                        with lock:

                            previous = highest_counter_by_boot.get(
                                boot_hex,
                                -1,
                            )

                            if counter <= previous:
                                raise ValueError(
                                    "Replay/out-of-order packet rejected."
                                )

                            highest_counter_by_boot[boot_hex] = counter

                            record = {
                                "timestamp": datetime.now(
                                    timezone.utc
                                ).isoformat(),
                                "boot_nonce": boot_hex,
                                "counter": counter,
                                "crypto_status": "authenticated",
                                **payload,
                            }

                            history.appendleft(record)

                            status["accepted_packets"] += 1
                            status["last_packet_at"] = record["timestamp"]
                            status["last_error"] = None

                    except (
                        ValueError,
                        UnicodeDecodeError,
                        InvalidTag,
                        KeyError,
                        json.JSONDecodeError,
                    ) as exc:

                        with lock:
                            status["rejected_packets"] += 1
                            status["last_error"] = (
                                f"Packet rejected: {exc}"
                            )

        except Exception as exc:

            with lock:
                status["serial_connected"] = False
                status["last_error"] = (
                    f"Serial unavailable: {exc}"
                )

            time.sleep(2)


# ============================================================
# DASHBOARD HOME
# ============================================================

@app.get("/")
def index():
    return render_template("index.html")


# ============================================================
# LATEST SENSOR DATA
# ============================================================

@app.get("/api/latest")
def api_latest():
    with lock:
        latest = dict(history[0]) if history else None
        current_status = dict(status)

    return jsonify({
        "latest": latest,
        "status": current_status,
    })


# ============================================================
# SENSOR HISTORY
# ============================================================

@app.get("/api/history")
def api_history():
    with lock:
        rows = [
            dict(row)
            for row in list(history)[:100]
        ]

    return jsonify(rows)


# ============================================================
# MODEL DIAGNOSIS
# ============================================================

@app.post("/api/predict")
def api_predict():
    """
    Run one LSTM prediction using the latest authenticated
    ESP32 telemetry and optional JSON form inputs.

    Example test request body:
    {
        "co2_level_ppm": 400,
        "crop_type": "Rice",
        "soil_type": "Clay",
        "pest_type": "Beetle"
    }

    The 400 ppm value is a manually supplied test value unless
    a valid calibrated co2_ppm sensor reading is present.
    """

    user_inputs = request.get_json(silent=True)

    if not isinstance(user_inputs, dict):
        return jsonify({
            "available": False,
            "reason": (
                "Send the diagnosis form as a JSON object."
            ),
        }), 400

    with lock:
        latest = dict(history[0]) if history else None

    if latest is None:
        return jsonify({
            "available": False,
            "reason": (
                "No authenticated ESP32 readings are available yet. "
                "Connect the ESP32 and wait for a valid encrypted packet."
            ),
        }), 503

    # The model service validates temperature, humidity and CO2.
    # MQ-135 raw ADC values are not interpreted as CO2 ppm.
    result = predict_if_available(
        latest,
        user_inputs,
    )

    if not result.get("available", False):
        return jsonify(result), 422

    return jsonify({
        "available": True,
        "prediction": result,
        "sensor_timestamp": latest.get("timestamp"),
        "sensor_packet": {
            "boot_nonce": latest.get("boot_nonce"),
            "counter": latest.get("counter"),
            "crypto_status": latest.get("crypto_status"),
        },
    })


# ============================================================
# HEALTH / CONNECTION STATUS
# ============================================================

@app.get("/api/health")
def api_health():
    with lock:
        current_status = dict(status)
        latest_timestamp = (
            history[0].get("timestamp")
            if history
            else None
        )

    return jsonify({
        "application": "running",
        "serial_connected": current_status["serial_connected"],
        "latest_sensor_timestamp": latest_timestamp,
        "accepted_packets": current_status["accepted_packets"],
        "rejected_packets": current_status["rejected_packets"],
        "last_error": current_status["last_error"],
    })


# ============================================================
# START APPLICATION
# ============================================================

if __name__ == "__main__":

    threading.Thread(
        target=serial_worker,
        daemon=True,
    ).start()

    app.run(
        host="127.0.0.1",
        port=5000,
        debug=False,
        threaded=True,
    )