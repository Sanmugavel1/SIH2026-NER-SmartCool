"""
train_model.py
------------------------------------------------------------------------
Trains a regression model that predicts the SAFE HOLDOVER TIME (in hours)
remaining for a vaccine, given:
    - core_temp     : temperature measured at the product (DS18B20), C
    - ambient_temp   : box/ambient temperature (DHT22), C
    - ambient_hum     : box/ambient relative humidity (DHT22), %

WHY SYNTHETIC DATA
------------------------------------------------------------------------
There is no public per-vial holdover dataset available for a student
project, so we build a *physics-informed* synthetic dataset: the target
follows the real cold-chain engineering rule of thumb that holdover time
degrades roughly exponentially the further the product drifts from the
WHO-recommended 2-8C envelope (this mirrors how vaccine cold-box/carrier
datasheets specify holdover: e.g. "list price: 48h holdover at 43C
ambient"). Gaussian noise is added so the model learns a *robust* mapping
rather than memorising a formula, which is what you want for a report
that talks about a trained regression model rather than a lookup table.

You can swap this out later for a real datasheet-derived table or lab
measurements — just replace generate_synthetic_dataset() and retrain.
------------------------------------------------------------------------
"""

import os
import numpy as np
import pandas as pd
from sklearn.ensemble import RandomForestRegressor
from sklearn.model_selection import train_test_split
from sklearn.metrics import mean_absolute_error, r2_score
import joblib

RNG = np.random.default_rng(42)

MODEL_DIR = os.path.dirname(__file__)
MODEL_PATH = os.path.join(MODEL_DIR, "holdover_model.pkl")
DATASET_PATH = os.path.join(MODEL_DIR, "synthetic_training_data.csv")

SAFE_MIN, SAFE_MAX = 2.0, 8.0
BASE_HOLDOVER_HOURS = 48.0   # typical WHO PQS cold-box holdover spec at design ambient


def true_holdover_hours(core_temp, ambient_temp, ambient_hum):
    """
    Physics-informed formula used to LABEL the synthetic dataset.
    Not used at inference time -- the trained model replaces this.
    """
    if core_temp < SAFE_MIN:
        deviation = SAFE_MIN - core_temp          # freezing risk
        decay_rate = 0.22
    elif core_temp > SAFE_MAX:
        deviation = core_temp - SAFE_MAX           # heat risk
        decay_rate = 0.16
    else:
        deviation = 0.0
        decay_rate = 0.0

    holdover = BASE_HOLDOVER_HOURS * np.exp(-decay_rate * deviation)

    # Higher ambient temperature accelerates internal warming -> shortens holdover
    ambient_penalty = 1.0 - 0.006 * max(ambient_temp - 25.0, 0)
    # Higher humidity slightly degrades insulation/ice-pack performance
    humidity_penalty = 1.0 - 0.0015 * max(ambient_hum - 50.0, 0)

    holdover = holdover * max(ambient_penalty, 0.4) * max(humidity_penalty, 0.6)
    return max(holdover, 0.0)


def generate_synthetic_dataset(n_samples=8000):
    core_temp = RNG.uniform(-8, 35, n_samples)
    ambient_temp = RNG.uniform(0, 45, n_samples)
    ambient_hum = RNG.uniform(10, 95, n_samples)

    labels = np.array([
        true_holdover_hours(ct, at, ah)
        for ct, at, ah in zip(core_temp, ambient_temp, ambient_hum)
    ])
    noise = RNG.normal(0, 1.5, n_samples)          # sensor + real-world variability
    labels = np.clip(labels + noise, 0, BASE_HOLDOVER_HOURS)

    df = pd.DataFrame({
        "core_temp": core_temp,
        "ambient_temp": ambient_temp,
        "ambient_hum": ambient_hum,
        "holdover_hours": labels,
    })
    return df


def train():
    df = generate_synthetic_dataset()
    df.to_csv(DATASET_PATH, index=False)
    print(f"[DATA] Synthetic dataset saved -> {DATASET_PATH} ({len(df)} rows)")

    X = df[["core_temp", "ambient_temp", "ambient_hum"]]
    y = df["holdover_hours"]
    X_train, X_test, y_train, y_test = train_test_split(X, y, test_size=0.2, random_state=42)

    model = RandomForestRegressor(
        n_estimators=200,
        max_depth=12,
        min_samples_leaf=3,
        random_state=42,
        n_jobs=-1,
    )
    model.fit(X_train, y_train)

    preds = model.predict(X_test)
    mae = mean_absolute_error(y_test, preds)
    r2 = r2_score(y_test, preds)
    print(f"[TRAIN] Test MAE: {mae:.2f} hours  |  R^2: {r2:.4f}")

    joblib.dump(model, MODEL_PATH)
    print(f"[TRAIN] Model saved -> {MODEL_PATH}")

    importances = dict(zip(X.columns, model.feature_importances_))
    print("[TRAIN] Feature importances:", {k: round(v, 3) for k, v in importances.items()})

    return model, mae, r2


if __name__ == "__main__":
    train()
