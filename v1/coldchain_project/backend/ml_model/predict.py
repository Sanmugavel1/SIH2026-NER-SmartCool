"""
predict.py
------------------------------------------------------------------------
Loads the trained holdover-time model once and exposes a simple
predict_holdover() function for the Flask backend to call on every
incoming reading (and on-demand from the dashboard's "what-if" tool).
------------------------------------------------------------------------
"""

import os
import joblib
import numpy as np

MODEL_PATH = os.path.join(os.path.dirname(__file__), "holdover_model.pkl")

_model = None


def _load_model():
    global _model
    if _model is None:
        if not os.path.exists(MODEL_PATH):
            raise FileNotFoundError(
                f"Model not found at {MODEL_PATH}. Run `python ml_model/train_model.py` first."
            )
        _model = joblib.load(MODEL_PATH)
    return _model


def predict_holdover(core_temp: float, ambient_temp: float, ambient_hum: float) -> float:
    """Returns predicted safe holdover time remaining, in hours (>= 0)."""
    model = _load_model()
    X = np.array([[core_temp, ambient_temp, ambient_hum]])
    hours = float(model.predict(X)[0])
    return round(max(hours, 0.0), 2)


if __name__ == "__main__":
    # Quick manual sanity check
    print("In-range (5C, 22C, 45%%):", predict_holdover(5.0, 22.0, 45.0), "h")
    print("Mild excursion (11C, 30C, 60%%):", predict_holdover(11.0, 30.0, 60.0), "h")
    print("Severe excursion (25C, 38C, 70%%):", predict_holdover(25.0, 38.0, 70.0), "h")
    print("Freezing risk (-3C, 10C, 40%%):", predict_holdover(-3.0, 10.0, 40.0), "h")
