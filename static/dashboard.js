"use strict";

const $ = (id) => document.getElementById(id);

let lastLatest = null;
let requestInProgress = false;


// ============================================================
// GENERAL HELPERS
// ============================================================

function setText(id, value) {
  const element = $(id);
  if (element) element.textContent = value;
}

function formatNumber(value, digits = 2, suffix = "") {
  if (value === null || value === undefined || value === "") {
    return "—";
  }

  const number = Number(value);

  return Number.isFinite(number)
    ? `${number.toFixed(digits)}${suffix}`
    : "—";
}

function firstAvailable(record, keys) {
  for (const key of keys) {
    const value = record?.[key];

    if (value !== null && value !== undefined && value !== "") {
      const number = Number(value);

      if (Number.isFinite(number)) {
        return number;
      }
    }
  }

  return null;
}


// ============================================================
// SENSOR CARD UPDATES
// ============================================================

function updateSensorCards(record) {
  if (!record) return;
    // HX711 Load Cell Weight
  const weight = firstAvailable(record, [
    "weight_g",
    "load_grams",
    "weight_grams",
    "weight"
  ]);

  setText(
    "weight",
    weight !== null ? formatNumber(weight, 2, " g") : "—"
  );

  setText(
    "weightnote",
    weight !== null ? "Live HX711 reading" : "Waiting for weight data"
  );

  // Temperature sensors
  const ds1 = firstAvailable(record, [
    "ds18b20_1_c",
    "ds1_c"
  ]);

  const ds2 = firstAvailable(record, [
    "ds18b20_2_c",
    "ds2_c"
  ]);

  const dhtTemp = firstAvailable(record, [
    "dht22_temp_c",
    "temperature_c"
  ]);

  // Humidity
  const humidity = firstAvailable(record, [
    "humidity_rh",
    "dht22_humidity",
    "humidity_percentage"
  ]);

  // MQ-135
  const mq135Relative = firstAvailable(record, [
    "mq135_relative_pct"
  ]);

  const mq135Raw = firstAvailable(record, [
    "mq135_raw"
  ]);

  // MQ-4
  const mq4Relative = firstAvailable(record, [
    "mq4_relative_pct"
  ]);

  const mq4Raw = firstAvailable(record, [
    "mq4_raw"
  ]);

  // Calibrated CO2 reading, if provided by a suitable sensor.
  const co2Ppm = firstAvailable(record, [
    "co2_ppm",
    "co2_level_ppm"
  ]);

  // Temperature cards
  setText("ds1", formatNumber(ds1, 2, " °C"));
  setText("ds2", formatNumber(ds2, 2, " °C"));
  setText("temp", formatNumber(dhtTemp, 2, " °C"));
  setText("humidity", formatNumber(humidity, 2, " %"));

  setText(
    "ds1note",
    ds1 === null ? "Unavailable" : "Temperature reading"
  );

  setText(
    "ds2note",
    ds2 === null ? "Unavailable" : "Temperature reading"
  );

  // MQ-135: display relative signal or raw ADC.
  setText(
    "mq135",
    mq135Relative !== null
      ? formatNumber(mq135Relative, 1, " %")
      : mq135Raw !== null
        ? `${mq135Raw.toFixed(0)} ADC`
        : "—"
  );

  // MQ-4: display relative signal or raw ADC.
  setText(
    "mq4",
    mq4Relative !== null
      ? formatNumber(mq4Relative, 1, " %")
      : mq4Raw !== null
        ? `${mq4Raw.toFixed(0)} ADC`
        : "—"
  );

  // Optional CO2 card.
  // This updates only if your HTML contains an element with id="co2".
  if (co2Ppm !== null && co2Ppm > 0 && co2Ppm <= 10000) {
    setText("co2", formatNumber(co2Ppm, 0, " ppm"));
    setText("co2note", "CO₂ sensor reading");
  } else {
    setText("co2", "—");
    setText(
      "co2note",
      "Calibrated CO₂ sensor reading unavailable"
    );
  }
}


// ============================================================
// CONNECTION STATUS
// ============================================================

function updateConnectionStatus(status, latest) {
  const connected = Boolean(status?.serial_connected);
  const connection = $("connection");

  if (connection) {
    connection.textContent = connected
      ? "ESP32 Connected"
      : "Disconnected";

    connection.classList.toggle("connected", connected);
    connection.classList.toggle("disconnected", !connected);
  }

  // Use the configured backend serial port if exposed by the API.
  setText(
    "port",
    status?.serial_port || "Configured serial port"
  );

  setText("accepted", status?.accepted_packets ?? 0);
  setText("rejected", status?.rejected_packets ?? 0);

  setText(
    "lastPacket",
    latest?.timestamp
      ? new Date(latest.timestamp).toLocaleString()
      : "Waiting for packet"
  );

  setText("error", status?.last_error || "None");
}


// ============================================================
// REFRESH DASHBOARD
// ============================================================

async function refreshDashboard() {
  try {
    const response = await fetch("/api/latest", {
      cache: "no-store"
    });

    if (!response.ok) {
      throw new Error(
        `Dashboard API returned ${response.status}`
      );
    }

    const data = await response.json();

    lastLatest = data.latest || null;

    updateConnectionStatus(
      data.status,
      lastLatest
    );

    if (lastLatest) {
      updateSensorCards(lastLatest);
    } else {
      setText("ds1", "—");
      setText("ds2", "—");
      setText("temp", "—");
      setText("humidity", "—");
      setText("mq135", "—");
      setText("mq4", "—");
      setText("co2", "—");
    }

  } catch (error) {
    const connection = $("connection");

    if (connection) {
      connection.textContent = "Dashboard API Error";
      connection.classList.add("disconnected");
      connection.classList.remove("connected");
    }

    setText(
      "error",
      error.message || "Unable to read dashboard data"
    );
  }
}


// ============================================================
// READ DIAGNOSIS FORM
// ============================================================

function readFormValues() {
  const form = $("diagnosisForm");

  if (!form) {
    throw new Error("Diagnosis form was not found.");
  }

  const formData = new FormData(form);
  const values = {};

  for (const [key, value] of formData.entries()) {
    const element = form.elements.namedItem(key);

    if (element && element.type === "number") {
      if (String(value).trim() === "") {
        throw new Error(
          `Please enter a value for ${key}.`
        );
      }

      const number = Number(value);

      if (!Number.isFinite(number)) {
        throw new Error(
          `Please enter a valid number for ${key}.`
        );
      }

      values[key] = number;

    } else {
      values[key] = String(value).trim();
    }
  }

  return values;
}


// ============================================================
// RENDER PREDICTION RESULTS
// ============================================================

function renderPrediction(result) {
  const prediction = result.prediction;

  if (!prediction) {
    throw new Error("The server returned no prediction data.");
  }

  const label = prediction.predicted_label ?? "Unknown";
  const confidence = Number(prediction.confidence_percent);

  // Diagnosis result
  const diagnosis = $("diagnosisResult");

  if (diagnosis) {
    diagnosis.replaceChildren();

    const heading = document.createElement("h3");
    heading.textContent = label.replaceAll("_", " ");
    diagnosis.appendChild(heading);

    const confidenceLine = document.createElement("p");

    confidenceLine.textContent = Number.isFinite(confidence)
      ? `Model confidence: ${confidence.toFixed(2)}%`
      : "Model confidence unavailable";

    diagnosis.appendChild(confidenceLine);
  }

  // Display CO2 value and source used by the model.
  const inputFeatures = prediction.input_features || {};
  const co2 = Number(inputFeatures.CO2_Level_ppm);

  const co2Info = prediction.co2_input || {};

  if (Number.isFinite(co2)) {
    setText("modelCo2", `${co2.toFixed(0)} ppm`);

    setText(
      "modelCo2Source",
      co2Info.source === "calibrated_sensor"
        ? "Calibrated sensor input"
        : "Manual/test input — not a verified live reading"
    );
  } else {
    setText("modelCo2", "—");
    setText("modelCo2Source", "CO₂ input unavailable");
  }

  // Class probabilities
  const probabilityContainer = $("probabilityBreakdown");

  if (probabilityContainer) {
    probabilityContainer.replaceChildren();

    const probabilityHeading = document.createElement("h3");
    probabilityHeading.textContent =
      "Class probability breakdown";

    probabilityContainer.appendChild(probabilityHeading);

    for (const [className, probability] of Object.entries(
      prediction.probabilities || {}
    )) {
      const line = document.createElement("p");
      const number = Number(probability);

      line.textContent =
        `${className.replaceAll("_", " ")}: ${
          Number.isFinite(number)
            ? number.toFixed(2) + "%"
            : "—"
        }`;

      probabilityContainer.appendChild(line);
    }
  }

  // Assumptions
  const assumptionsContainer = $("assumptionsList");

  if (assumptionsContainer) {
    assumptionsContainer.replaceChildren();

    const assumptionsHeading = document.createElement("h3");
    assumptionsHeading.textContent = "Input assumptions";

    assumptionsContainer.appendChild(assumptionsHeading);

    const assumptions = prediction.assumptions || [];

    if (assumptions.length === 0) {
      const none = document.createElement("p");
      none.textContent = "No assumptions were reported.";

      assumptionsContainer.appendChild(none);

    } else {
      const list = document.createElement("ul");

      for (const assumption of assumptions) {
        const item = document.createElement("li");
        item.textContent = assumption;
        list.appendChild(item);
      }

      assumptionsContainer.appendChild(list);
    }
  }

  setText(
    "prediction",
    `Prediction available: ${label.replaceAll("_", " ")}`
  );
}


// ============================================================
// SUBMIT DIAGNOSIS
// ============================================================

async function submitDiagnosis(event) {
  event.preventDefault();

  if (requestInProgress) return;

  const button = $("predictButton");
  const message = $("formMessage");

  try {
    if (!lastLatest) {
      throw new Error(
        "No authenticated ESP32 packet is available. " +
        "Connect the ESP32 and wait for a valid encrypted reading."
      );
    }

    const values = readFormValues();

    requestInProgress = true;

    if (button) {
      button.disabled = true;
    }

    if (message) {
      message.textContent = "Running LSTM prediction…";
    }

    setText(
      "prediction",
      "Prediction in progress…"
    );

    const diagnosis = $("diagnosisResult");

    if (diagnosis) {
      diagnosis.textContent =
        "Waiting for prediction response…";
    }

    const response = await fetch("/api/predict", {
      method: "POST",
      headers: {
        "Content-Type": "application/json"
      },
      body: JSON.stringify(values)
    });

    const result = await response.json();

    if (!response.ok || !result.available) {
      throw new Error(
        result.reason ||
        `Prediction request failed (${response.status}).`
      );
    }

    renderPrediction(result);

    if (message) {
      message.textContent =
        "Prediction completed. Results are experimental " +
        "until validated against real-world data.";
    }

  } catch (error) {
    const diagnosis = $("diagnosisResult");

    if (diagnosis) {
      diagnosis.textContent =
        `Prediction failed: ${error.message}`;
    }

    setText(
      "prediction",
      "Prediction unavailable"
    );

    const probabilities = $("probabilityBreakdown");

    if (probabilities) {
      probabilities.replaceChildren();
    }

    const assumptions = $("assumptionsList");

    if (assumptions) {
      assumptions.replaceChildren();
    }

    if (message) {
      message.textContent = error.message;
    }

  } finally {
    requestInProgress = false;

    if (button) {
      button.disabled = false;
    }
  }
}


// ============================================================
// INITIALIZE DASHBOARD
// ============================================================

document.addEventListener("DOMContentLoaded", () => {
  const form = $("diagnosisForm");

  if (form) {
    form.addEventListener(
      "submit",
      submitDiagnosis
    );
  }

  refreshDashboard();

  // Refresh live sensor readings every two seconds.
  window.setInterval(
    refreshDashboard,
    2000
  );
});