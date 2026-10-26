"""Local LSTM inference service for the ESP32 sensor dashboard.

Loads the trained Keras model, fitted sklearn preprocessor and label encoder.

CO2 handling:
- Uses calibrated CO2 ppm from ESP32 telemetry when available.
- Otherwise, accepts a manually supplied co2_level_ppm value.
- Never treats raw MQ-135 ADC readings as actual CO2 ppm.
- A test value such as 400 ppm must be explicitly supplied for testing.
"""

from pathlib import Path
import threading

import joblib
import numpy as np
import pandas as pd
import tensorflow as tf


# ============================================================
# MODEL FILE PATHS
# ============================================================

MODEL_DIR = Path(__file__).resolve().parent / "models"

MODEL_PATH = MODEL_DIR / "pest_lstm.keras"
PREPROCESSOR_PATH = MODEL_DIR / "preprocessor.joblib"
LABEL_ENCODER_PATH = MODEL_DIR / "label_encoder.joblib"


# ============================================================
# MODEL FEATURES
# Keep these names and ordering compatible with model training.
# ============================================================

NUM_COLS = [
    "Temperature_C",
    "Humidity_Percentage",
    "Soil_Moisture_Percentage",
    "Light_Intensity_Lux",
    "CO2_Level_ppm",
    "Wind_Speed_kmph",
    "Leaf_Damage_Percentage",
    "Pest_Density",
    "CNN_Feature_Score",
    "Detection_Confidence",
    "Latency_ms",
    "Pesticide_Usage_ml",
    "Battery_Level_Percentage",
    "Network_Signal_Strength",
]

CAT_COLS = [
    "Crop_Type",
    "Pest_Type",
    "Weather_Condition",
    "Soil_Type",
    "Control_Action",
]

FEATURE_ORDER = NUM_COLS + CAT_COLS


# ============================================================
# DEFAULT VALUES
# These are prototype assumptions, not live sensor readings.
# ============================================================

DEFAULTS = {
    "crop_type": "Rice",
    "soil_type": "Clay",
    "pest_type": "Beetle",
    "light_intensity_lux": 550.0,
    "wind_speed_kmph": 12.0,
    "leaf_damage_percentage": 15.0,
    "pest_density": 40.0,
    "cnn_feature_score": 0.90,
    "detection_confidence": 0.90,
    "latency_ms": 180.0,
    "pesticide_usage_ml": 45.0,
    "battery_level_percentage": 90.0,
    "network_signal_strength": 4.0,
    "control_action": "No_Action",
}


# ============================================================
# MODEL STATE
# ============================================================

_model = None
_preprocessor = None
_label_encoder = None
_load_error = None
_load_lock = threading.Lock()


# ============================================================
# LOAD TRAINED MODEL AND PREPROCESSOR
# ============================================================

def _load_artifacts():
    """Load model artifacts once and validate compatibility."""

    global _model, _preprocessor, _label_encoder, _load_error

    if _model is not None:
        return

    with _load_lock:

        if _model is not None:
            return

        missing = [
            str(path)
            for path in (
                MODEL_PATH,
                PREPROCESSOR_PATH,
                LABEL_ENCODER_PATH,
            )
            if not path.is_file()
        ]

        if missing:
            raise FileNotFoundError(
                "Missing trained model artifact(s): "
                + ", ".join(missing)
            )

        try:
            _model = tf.keras.models.load_model(
                MODEL_PATH,
                compile=False,
            )

            _preprocessor = joblib.load(PREPROCESSOR_PATH)
            _label_encoder = joblib.load(LABEL_ENCODER_PATH)

            if len(_model.input_shape) != 3:
                raise ValueError(
                    "Expected a 3D LSTM input, but received "
                    f"{_model.input_shape}."
                )

            if _model.input_shape[1] != 1:
                raise ValueError(
                    "This integration expects one timestep, but the "
                    f"model expects {_model.input_shape[1]}."
                )

            if (
                len(_label_encoder.classes_)
                != _model.output_shape[-1]
            ):
                raise ValueError(
                    "Label encoder class count does not match "
                    "the model output dimension."
                )

            _load_error = None

        except Exception as exc:
            _model = None
            _preprocessor = None
            _label_encoder = None

            _load_error = f"{type(exc).__name__}: {exc}"
            raise


# ============================================================
# DATA VALIDATION HELPERS
# ============================================================

def _valid_number(value):
    """Return a finite float or None."""

    try:
        number = float(value)

        if np.isfinite(number):
            return number

    except (TypeError, ValueError):
        pass

    return None


def _first_valid(record, keys):
    """Return the first valid numeric value found in a record."""

    for key in keys:

        value = _valid_number(record.get(key))

        if value is not None:
            return value

    return None


def _weather_from_humidity(humidity):
    """Simple prototype classification based on relative humidity."""

    if humidity >= 70:
        return "Humid"

    if humidity <= 40:
        return "Dry"

    return "Cloudy"


# ============================================================
# BUILD FEATURES FOR THE LSTM MODEL
# ============================================================

def _build_features(record, user_inputs=None):
    """
    Combine ESP32 telemetry, manual inputs and prototype defaults.

    Sensor values take priority over manual values when valid.
    CO2 must come from a calibrated CO2 field or an explicitly
    supplied manual CO2 value. MQ-135 ADC readings are not ppm.
    """

    record = record or {}
    user_inputs = user_inputs or {}

    assumptions = []

    # --------------------------------------------------------
    # TEMPERATURE
    # --------------------------------------------------------

    temperature = _first_valid(
        record,
        [
            "ds18b20_1_c",
            "ds18b20_2_c",
            "dht22_temp_c",
        ],
    )

    # Reject invalid sensor values and common error sentinels.
    if temperature is not None:
        if not -55 <= temperature <= 125:
            temperature = None

    if temperature is None:
        temperature = _valid_number(
            user_inputs.get("temperature_c")
        )

    if temperature is None:
        raise ValueError(
            "No valid temperature reading is available. "
            "Check the ESP32 telemetry or supply temperature_c."
        )

    # --------------------------------------------------------
    # HUMIDITY
    # --------------------------------------------------------

    humidity = _first_valid(
        record,
        [
            "humidity_rh",
            "dht22_humidity",
            "humidity_percentage",
        ],
    )

    if humidity is not None:
        if not 0 <= humidity <= 100:
            humidity = None

    if humidity is None:
        humidity = _valid_number(
            user_inputs.get("humidity_percentage")
        )

    if humidity is None or not 0 <= humidity <= 100:
        raise ValueError(
            "No valid humidity reading is available. "
            "Check the ESP32 telemetry or supply "
            "humidity_percentage between 0 and 100."
        )

    # --------------------------------------------------------
    # CO2 / GAS ANALYSIS
    # --------------------------------------------------------
    #
    # Preferred telemetry field:
    #     co2_ppm
    #
    # This field must contain a calibrated CO2 reading from a
    # suitable CO2 sensor, such as an NDIR sensor.
    #
    # For a pipeline test, pass:
    #     {"co2_level_ppm": 400}
    #
    # Do not interpret mq135_raw or mq135_relative_pct as ppm.
    # --------------------------------------------------------

    co2_ppm = _first_valid(
        record,
        [
            "co2_ppm",
            "co2_level_ppm",
        ],
    )

    co2_source = "calibrated_sensor"

    # Reject physically implausible values for ordinary
    # ambient CO2 monitoring.
    if co2_ppm is not None:
        if not 0 < co2_ppm <= 10000:
            co2_ppm = None

    # If there is no valid calibrated sensor reading,
    # accept an explicitly supplied manual/test value.
    if co2_ppm is None:

        co2_ppm = _valid_number(
            user_inputs.get("co2_level_ppm")
        )

        co2_source = "manual_or_test_input"

    if co2_ppm is None or not 0 < co2_ppm <= 10000:
        raise ValueError(
            "A valid CO2 value is required. Provide calibrated "
            "ESP32 telemetry using the 'co2_ppm' field, or supply "
            "a manual/test value using 'co2_level_ppm'. "
            "MQ-135 raw ADC values cannot be used directly as ppm."
        )

    if co2_source == "manual_or_test_input":
        assumptions.append(
            f"CO2 value of {co2_ppm:g} ppm was supplied manually "
            "or for testing; it is not verified as a live sensor reading."
        )

    # --------------------------------------------------------
    # MOISTURE PROXY
    # --------------------------------------------------------
    #
    # DHT22 measures air humidity, not soil moisture.
    # Retained as a proxy to preserve the trained feature schema.
    # --------------------------------------------------------

    moisture_proxy = humidity

    assumptions.append(
        "Soil_Moisture_Percentage uses air humidity as a proxy; "
        "a dedicated soil-moisture sensor is needed for a true "
        "soil-moisture measurement."
    )

    # --------------------------------------------------------
    # OPTIONAL MANUAL VALUES / DEFAULTS
    # --------------------------------------------------------

    values = dict(DEFAULTS)

    for key in values:

        if key in user_inputs:
            supplied_value = user_inputs[key]

            if supplied_value is not None and supplied_value != "":
                values[key] = supplied_value

    weather = _weather_from_humidity(humidity)

    if user_inputs.get("weather_condition"):
        weather = str(
            user_inputs["weather_condition"]
        ).strip()

    # --------------------------------------------------------
    # MODEL INPUT FEATURES
    # --------------------------------------------------------

    features = {
        "Temperature_C": temperature,
        "Humidity_Percentage": humidity,
        "Soil_Moisture_Percentage": moisture_proxy,
        "Light_Intensity_Lux": values["light_intensity_lux"],
        "CO2_Level_ppm": co2_ppm,
        "Wind_Speed_kmph": values["wind_speed_kmph"],
        "Leaf_Damage_Percentage": values[
            "leaf_damage_percentage"
        ],
        "Pest_Density": values["pest_density"],
        "CNN_Feature_Score": values["cnn_feature_score"],
        "Detection_Confidence": values[
            "detection_confidence"
        ],
        "Latency_ms": values["latency_ms"],
        "Pesticide_Usage_ml": values["pesticide_usage_ml"],
        "Battery_Level_Percentage": values[
            "battery_level_percentage"
        ],
        "Network_Signal_Strength": values[
            "network_signal_strength"
        ],
        "Crop_Type": values["crop_type"],
        "Pest_Type": values["pest_type"],
        "Weather_Condition": weather,
        "Soil_Type": values["soil_type"],
        "Control_Action": values["control_action"],
    }

    # --------------------------------------------------------
    # VALIDATE NUMERIC FEATURES
    # --------------------------------------------------------

    for column in NUM_COLS:

        number = _valid_number(features[column])

        if number is None:
            raise ValueError(
                f"Feature '{column}' must contain a valid number."
            )

        features[column] = number

    # --------------------------------------------------------
    # BUILD DATAFRAME IN THE EXACT TRAINING ORDER
    # --------------------------------------------------------

    input_df = pd.DataFrame(
        [
            [
                features[column]
                for column in FEATURE_ORDER
            ]
        ],
        columns=FEATURE_ORDER,
    )

    # Add explicit notes about prototype defaults.
    assumptions.append(
        "Wind speed, leaf damage, pest density, CNN score, "
        "detection confidence, latency, pesticide usage, battery "
        "level and network strength use configured defaults unless "
        "manually overridden."
    )

    assumptions.append(
        "The model's prediction is based on the supplied features "
        "and should not be treated as a verified diagnosis without "
        "appropriate model validation."
    )

    return input_df, assumptions


# ============================================================
# RUN LSTM PREDICTION
# ============================================================

def predict_if_available(record, user_inputs=None):
    """
    Run the trained LSTM model and return a Flask-compatible
    dictionary containing the predicted label, probabilities,
    assumptions and actual input features.
    """

    try:
        _load_artifacts()

        input_df, assumptions = _build_features(
            record,
            user_inputs,
        )

        # Apply the same fitted preprocessor used during training.
        processed = _preprocessor.transform(input_df)

        processed = np.asarray(
            processed,
            dtype=np.float32,
        )

        if processed.ndim != 2 or processed.shape[0] != 1:
            raise ValueError(
                "Unexpected preprocessor output shape: "
                f"{processed.shape}"
            )

        expected_features = _model.input_shape[-1]

        if processed.shape[1] != expected_features:
            raise ValueError(
                "Preprocessor output/model input mismatch: "
                f"{processed.shape[1]} vs "
                f"{expected_features} features."
            )

        # LSTM input shape: samples, timesteps, features.
        model_input = processed.reshape(
            (1, 1, processed.shape[1])
        )

        probabilities = _model.predict(
            model_input,
            verbose=0,
        )[0]

        if not np.all(np.isfinite(probabilities)):
            raise ValueError(
                "The model returned invalid prediction probabilities."
            )

        predicted_index = int(
            np.argmax(probabilities)
        )

        predicted_label = str(
            _label_encoder.classes_[predicted_index]
        )

        input_features = {
            key: (
                value.item()
                if hasattr(value, "item")
                else value
            )
            for key, value in input_df.iloc[0].to_dict().items()
        }

        return {
            "available": True,
            "predicted_label": predicted_label,
            "confidence_percent": round(
                float(probabilities[predicted_index]) * 100,
                2,
            ),
            "probabilities": {
                str(label): round(
                    float(probability) * 100,
                    2,
                )
                for label, probability in zip(
                    _label_encoder.classes_,
                    probabilities,
                )
            },
            "assumptions": assumptions,
            "input_features": input_features,
            "co2_input": {
                "value_ppm": input_features["CO2_Level_ppm"],
                "source": (
                    "calibrated_sensor"
                    if _first_valid(
                        record or {},
                        ["co2_ppm", "co2_level_ppm"],
                    ) is not None
                    and 0 < _first_valid(
                        record or {},
                        ["co2_ppm", "co2_level_ppm"],
                    ) <= 10000
                    else "manual_or_test_input"
                ),
            },
        }

    except Exception as exc:

        return {
            "available": False,
            "reason": f"{type(exc).__name__}: {exc}",
            "load_error": _load_error,
        }