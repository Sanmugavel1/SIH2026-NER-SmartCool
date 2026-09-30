"""
app.py
------------------------------------------------------------------------
Flask backend for the Vaccine Cold Chain Management System.

Responsibilities
------------------------------------------------------------------------
1. Receive sensor data from the ESP32 over HTTP POST (/api/data), both
   "live" readings and "backlog" (offline-buffered, SD-synced) readings.
2. Run every incoming reading through the trained ML model to predict
   safe holdover time remaining.
3. Persist everything to SQLite (database.py).
4. Serve two role-based dashboards (admin / user) plus a small JSON API
   the dashboards poll for live charts.
5. Basic session-based authentication.

Run:
    python app.py
Then browse to http://<this-machine-ip>:5000/login
------------------------------------------------------------------------
"""

import os
import sys
import csv
import io
from functools import wraps
from flask import (
    Flask, request, jsonify, render_template, redirect, url_for,
    session, flash, Response
)
from werkzeug.security import check_password_hash

sys.path.append(os.path.join(os.path.dirname(__file__), "ml_model"))

import database as db
import predict as ml

app = Flask(__name__)
app.secret_key = os.environ.get("COLDCHAIN_SECRET_KEY", "dev-secret-change-me")

API_KEY = "esp32-coldchain-key-01"   # must match the ESP32 firmware's API_KEY
DEFAULT_DEVICE = "COLD_BOX_01"


# ---------------------------------------------------------------------------
# AUTH HELPERS
# ---------------------------------------------------------------------------
def login_required(role=None):
    def decorator(fn):
        @wraps(fn)
        def wrapper(*args, **kwargs):
            if "username" not in session:
                return redirect(url_for("login", next=request.path))
            if role and session.get("role") != role:
                flash("You don't have access to that page.", "error")
                return redirect(url_for("user_dashboard"))
            return fn(*args, **kwargs)
        return wrapper
    return decorator


# ---------------------------------------------------------------------------
# DEVICE API — called by the ESP32 firmware
# ---------------------------------------------------------------------------
@app.route("/api/data", methods=["POST"])
def api_data():
    if request.headers.get("X-API-Key") != API_KEY:
        return jsonify({"error": "unauthorized"}), 401

    payload = request.get_json(silent=True)
    if not payload:
        return jsonify({"error": "invalid or missing JSON body"}), 400

    required = ["device_id", "timestamp", "core_temp", "ambient_temp", "ambient_hum"]
    missing = [k for k in required if k not in payload]
    if missing:
        return jsonify({"error": f"missing fields: {missing}"}), 400

    device_id = payload["device_id"]
    timestamp = payload["timestamp"]
    core_temp = float(payload["core_temp"])
    ambient_temp = float(payload["ambient_temp"])
    ambient_hum = float(payload["ambient_hum"])
    excursion = bool(payload.get("excursion", False))
    source = payload.get("source", "live")

    holdover_hours = ml.predict_holdover(core_temp, ambient_temp, ambient_hum)

    reading_id = db.insert_reading(
        device_id=device_id,
        timestamp=timestamp,
        core_temp=core_temp,
        ambient_temp=ambient_temp,
        ambient_hum=ambient_hum,
        excursion=excursion,
        source=source,
        predicted_holdover_hours=holdover_hours,
    )

    return jsonify({
        "status": "ok",
        "reading_id": reading_id,
        "predicted_holdover_hours": holdover_hours,
    }), 201


@app.route("/api/predict", methods=["GET"])
def api_predict():
    """On-demand 'what-if' prediction, used by the dashboard's simulator widget."""
    try:
        core_temp = float(request.args.get("core_temp", 5))
        ambient_temp = float(request.args.get("ambient_temp", 22))
        ambient_hum = float(request.args.get("ambient_hum", 45))
    except ValueError:
        return jsonify({"error": "core_temp, ambient_temp, ambient_hum must be numbers"}), 400

    hours = ml.predict_holdover(core_temp, ambient_temp, ambient_hum)
    return jsonify({"predicted_holdover_hours": hours})


@app.route("/api/latest")
def api_latest():
    device_id = request.args.get("device_id", DEFAULT_DEVICE)
    return jsonify(db.get_latest_reading(device_id) or {})


@app.route("/api/history")
def api_history():
    device_id = request.args.get("device_id", DEFAULT_DEVICE)
    limit = int(request.args.get("limit", 200))
    return jsonify(db.get_history(device_id, limit))


@app.route("/api/alerts")
def api_alerts():
    device_id = request.args.get("device_id", DEFAULT_DEVICE)
    return jsonify(db.get_alerts(device_id))


@app.route("/api/stats")
def api_stats():
    device_id = request.args.get("device_id", DEFAULT_DEVICE)
    return jsonify(db.get_stats(device_id))


# ---------------------------------------------------------------------------
# ADMIN-ONLY API
# ---------------------------------------------------------------------------
@app.route("/api/export")
@login_required(role="admin")
def api_export():
    """CSV export of full reading history, for the admin dashboard's Export button."""
    device_id = request.args.get("device_id", DEFAULT_DEVICE)
    rows = db.get_history(device_id, limit=100000)

    buf = io.StringIO()
    writer = csv.writer(buf)
    writer.writerow(["timestamp", "core_temp", "ambient_temp", "ambient_hum",
                      "excursion", "predicted_holdover_hours", "source", "received_at"])
    for r in rows:
        writer.writerow([r["timestamp"], r["core_temp"], r["ambient_temp"], r["ambient_hum"],
                          r["excursion"], r["predicted_holdover_hours"], r["source"], r["received_at"]])

    return Response(
        buf.getvalue(),
        mimetype="text/csv",
        headers={"Content-Disposition": f"attachment; filename={device_id}_history.csv"},
    )


@app.route("/api/users", methods=["GET", "POST"])
@login_required(role="admin")
def api_users():
    if request.method == "POST":
        payload = request.get_json(silent=True) or {}
        username = payload.get("username", "").strip()
        password = payload.get("password", "")
        role = payload.get("role", "user")
        if not username or not password or role not in ("admin", "user"):
            return jsonify({"error": "username, password and a valid role are required"}), 400
        if db.get_user(username):
            return jsonify({"error": "username already exists"}), 409
        db.create_user(username, password, role)
        return jsonify({"status": "created"}), 201
    return jsonify(db.list_users())


@app.route("/api/devices")
@login_required(role="admin")
def api_devices():
    return jsonify(db.get_devices())


@app.route("/api/alerts/<int:alert_id>/ack", methods=["POST"])
@login_required(role="admin")
def api_ack_alert(alert_id):
    db.acknowledge_alert(alert_id)
    return jsonify({"status": "acknowledged"})


# ---------------------------------------------------------------------------
# AUTH ROUTES
# ---------------------------------------------------------------------------
@app.route("/login", methods=["GET", "POST"])
def login():
    if request.method == "POST":
        username = request.form.get("username", "").strip()
        password = request.form.get("password", "")
        user = db.get_user(username)
        if user and check_password_hash(user["password_hash"], password):
            session["username"] = user["username"]
            session["role"] = user["role"]
            return redirect(url_for("admin_dashboard") if user["role"] == "admin" else url_for("user_dashboard"))
        flash("Invalid username or password.", "error")
    return render_template("login.html")


@app.route("/logout")
def logout():
    session.clear()
    return redirect(url_for("login"))


@app.route("/")
def index():
    return redirect(url_for("login"))


# ---------------------------------------------------------------------------
# DASHBOARD PAGES
# ---------------------------------------------------------------------------
@app.route("/admin")
@login_required(role="admin")
def admin_dashboard():
    return render_template("admin_dashboard.html", username=session["username"], device_id=DEFAULT_DEVICE)


@app.route("/user")
@login_required()
def user_dashboard():
    return render_template("user_dashboard.html", username=session["username"], device_id=DEFAULT_DEVICE)


# ---------------------------------------------------------------------------
if __name__ == "__main__":
    db.init_db()
    app.run(host="0.0.0.0", port=5000, debug=True)
