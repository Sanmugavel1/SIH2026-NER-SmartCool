"""
database.py
------------------------------------------------------------------------
SQLite data layer for the Cold Chain Management backend.

Tables
------
users     : login accounts (admin / user roles)
devices   : registered cold-chain units
readings  : every sensor reading (both live and synced-offline rows)
alerts    : excursion events derived from readings, for quick lookup

Run this file directly to (re)initialise the database with schema +
seed accounts:
    python database.py
------------------------------------------------------------------------
"""

import sqlite3
import os
from datetime import datetime
from werkzeug.security import generate_password_hash

DB_PATH = os.path.join(os.path.dirname(__file__), "data", "coldchain.db")


def get_connection():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    conn = sqlite3.connect(DB_PATH, timeout=10)
    conn.execute("PRAGMA journal_mode=WAL;")   # better for concurrent web + POST writes
    conn.row_factory = sqlite3.Row
    return conn


SCHEMA = """
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    role TEXT NOT NULL CHECK(role IN ('admin', 'user')),
    created_at TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS devices (
    device_id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    location TEXT,
    safe_temp_min REAL NOT NULL DEFAULT 2.0,
    safe_temp_max REAL NOT NULL DEFAULT 8.0,
    registered_at TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS readings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    timestamp TEXT NOT NULL,           -- device RTC timestamp (ISO8601)
    received_at TEXT NOT NULL DEFAULT (datetime('now')),  -- server receive time
    core_temp REAL NOT NULL,
    ambient_temp REAL NOT NULL,
    ambient_hum REAL NOT NULL,
    excursion INTEGER NOT NULL DEFAULT 0,
    predicted_holdover_hours REAL,
    source TEXT NOT NULL DEFAULT 'live',   -- 'live' or 'backlog'
    FOREIGN KEY(device_id) REFERENCES devices(device_id)
);
CREATE INDEX IF NOT EXISTS idx_readings_device_time ON readings(device_id, timestamp);

CREATE TABLE IF NOT EXISTS alerts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    reading_id INTEGER NOT NULL,
    device_id TEXT NOT NULL,
    timestamp TEXT NOT NULL,
    message TEXT NOT NULL,
    acknowledged INTEGER NOT NULL DEFAULT 0,
    FOREIGN KEY(reading_id) REFERENCES readings(id)
);
"""


def init_db():
    conn = get_connection()
    conn.executescript(SCHEMA)

    # Seed default accounts if the users table is empty
    cur = conn.execute("SELECT COUNT(*) AS c FROM users")
    if cur.fetchone()["c"] == 0:
        conn.execute(
            "INSERT INTO users (username, password_hash, role) VALUES (?, ?, ?)",
            ("admin", generate_password_hash("admin123"), "admin"),
        )
        conn.execute(
            "INSERT INTO users (username, password_hash, role) VALUES (?, ?, ?)",
            ("viewer", generate_password_hash("viewer123"), "user"),
        )
        print("[DB] Seeded default accounts: admin/admin123 (admin), viewer/viewer123 (user)")

    # Seed the default device referenced by the firmware's DEVICE_ID
    conn.execute(
        """INSERT OR IGNORE INTO devices (device_id, name, location, safe_temp_min, safe_temp_max)
           VALUES (?, ?, ?, ?, ?)""",
        ("COLD_BOX_01", "Cold Chain Unit 01", "Main Lab", 2.0, 8.0),
    )
    conn.commit()
    conn.close()
    print(f"[DB] Database ready at {DB_PATH}")


def insert_reading(device_id, timestamp, core_temp, ambient_temp, ambient_hum,
                    excursion, source, predicted_holdover_hours=None):
    conn = get_connection()
    cur = conn.execute(
        """INSERT INTO readings
           (device_id, timestamp, core_temp, ambient_temp, ambient_hum,
            excursion, source, predicted_holdover_hours)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?)""",
        (device_id, timestamp, core_temp, ambient_temp, ambient_hum,
         int(bool(excursion)), source, predicted_holdover_hours),
    )
    reading_id = cur.lastrowid

    if excursion:
        conn.execute(
            """INSERT INTO alerts (reading_id, device_id, timestamp, message)
               VALUES (?, ?, ?, ?)""",
            (reading_id, device_id, timestamp,
             f"Core temperature {core_temp:.1f}C out of safe range on {device_id}"),
        )
    conn.commit()
    conn.close()
    return reading_id


def get_latest_reading(device_id="COLD_BOX_01"):
    conn = get_connection()
    row = conn.execute(
        "SELECT * FROM readings WHERE device_id=? ORDER BY id DESC LIMIT 1", (device_id,)
    ).fetchone()
    conn.close()
    return dict(row) if row else None


def get_history(device_id="COLD_BOX_01", limit=500):
    conn = get_connection()
    rows = conn.execute(
        "SELECT * FROM readings WHERE device_id=? ORDER BY id DESC LIMIT ?",
        (device_id, limit),
    ).fetchall()
    conn.close()
    return [dict(r) for r in reversed(rows)]   # chronological order for charts


def get_alerts(device_id="COLD_BOX_01", limit=100, only_unacknowledged=False):
    conn = get_connection()
    q = "SELECT * FROM alerts WHERE device_id=?"
    if only_unacknowledged:
        q += " AND acknowledged=0"
    q += " ORDER BY id DESC LIMIT ?"
    rows = conn.execute(q, (device_id, limit)).fetchall()
    conn.close()
    return [dict(r) for r in rows]


def acknowledge_alert(alert_id):
    conn = get_connection()
    conn.execute("UPDATE alerts SET acknowledged=1 WHERE id=?", (alert_id,))
    conn.commit()
    conn.close()


def get_user(username):
    conn = get_connection()
    row = conn.execute("SELECT * FROM users WHERE username=?", (username,)).fetchone()
    conn.close()
    return dict(row) if row else None


def create_user(username, password, role):
    conn = get_connection()
    conn.execute(
        "INSERT INTO users (username, password_hash, role) VALUES (?, ?, ?)",
        (username, generate_password_hash(password), role),
    )
    conn.commit()
    conn.close()


def list_users():
    conn = get_connection()
    rows = conn.execute("SELECT id, username, role, created_at FROM users").fetchall()
    conn.close()
    return [dict(r) for r in rows]


def get_devices():
    conn = get_connection()
    rows = conn.execute("SELECT * FROM devices").fetchall()
    conn.close()
    return [dict(r) for r in rows]


def get_stats(device_id="COLD_BOX_01"):
    """Summary numbers for the admin dashboard header cards."""
    conn = get_connection()
    total = conn.execute("SELECT COUNT(*) c FROM readings WHERE device_id=?", (device_id,)).fetchone()["c"]
    excursions = conn.execute(
        "SELECT COUNT(*) c FROM readings WHERE device_id=? AND excursion=1", (device_id,)
    ).fetchone()["c"]
    backlog_synced = conn.execute(
        "SELECT COUNT(*) c FROM readings WHERE device_id=? AND source='backlog'", (device_id,)
    ).fetchone()["c"]
    conn.close()
    return {"total_readings": total, "excursions": excursions, "backlog_synced": backlog_synced}


if __name__ == "__main__":
    init_db()
