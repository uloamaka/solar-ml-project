import sqlite3
import pandas as pd

DB_PATH = '/var/lib/solar_backend/predictions.db'

SCHEMA = """
CREATE TABLE IF NOT EXISTS predictions (
    timestamp TEXT PRIMARY KEY,
    year INTEGER,
    ghi REAL,
    dni REAL,
    dhi REAL,
    temp_air REAL,
    relative_humidity REAL,
    wind_speed REAL,
    cloud_cover REAL,
    clearness_index REAL,
    solar_zenith_angle REAL,
    solar_azimuth REAL,
    predicted_pr REAL,
    predicted_power_w REAL,
    source TEXT,
    generated_at TEXT
)
"""

INSERT_SQL = """
INSERT OR IGNORE INTO predictions
(timestamp, year, ghi, dni, dhi, temp_air, relative_humidity,
 wind_speed, cloud_cover, clearness_index, solar_zenith_angle,
 solar_azimuth, predicted_pr, predicted_power_w, source, generated_at)
VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
"""


ACTUALS_SCHEMA = """
CREATE TABLE IF NOT EXISTS actuals (
    timestamp TEXT PRIMARY KEY,
    measured_power_w REAL,
    received_at TEXT
)
"""


def get_connection(db_path=DB_PATH):
    return sqlite3.connect(db_path)


def init_db(db_path=DB_PATH):
    conn = get_connection(db_path)
    conn.execute(SCHEMA)
    conn.execute(ACTUALS_SCHEMA)
    conn.commit()
    conn.close()


def insert_actual(timestamp, measured_power_w, db_path=DB_PATH):
    conn = get_connection(db_path)
    conn.execute(
        "INSERT OR IGNORE INTO actuals (timestamp, measured_power_w, received_at) VALUES (?,?,?)",
        (timestamp.isoformat(), measured_power_w, pd.Timestamp.utcnow().isoformat()))
    conn.commit()
    conn.close()


def insert_predictions(df, source, db_path=DB_PATH):
    conn = get_connection(db_path)
    generated_at = pd.Timestamp.utcnow().isoformat()
    rows = [
        (ts.isoformat(), int(r['year']), r['ghi'], r['dni'], r['dhi'],
         r['temp_air'], r['relative_humidity'], r['wind_speed'],
         r['cloud_cover'], r['clearness_index'], r['solar_zenith_angle'],
         r['solar_azimuth'], r['predicted_pr'], r['predicted_power_w'],
         source, generated_at)
        for ts, r in df.iterrows()
    ]
    conn.executemany(INSERT_SQL, rows)
    conn.commit()
    conn.close()


def get_existing_timestamps(start, end, db_path=DB_PATH):
    conn = get_connection(db_path)
    cur = conn.execute(
        "SELECT timestamp FROM predictions WHERE timestamp >= ? AND timestamp <= ?",
        (start.isoformat(), end.isoformat()))
    rows = [r[0] for r in cur.fetchall()]
    conn.close()
    return set(pd.to_datetime(rows))


def get_missing_hours(start, end, db_path=DB_PATH):
    full_range = pd.date_range(start, end, freq='h')
    existing = get_existing_timestamps(start, end, db_path)
    return [ts for ts in full_range if ts not in existing]


def get_predictions(start, end, db_path=DB_PATH):
    conn = get_connection(db_path)
    df = pd.read_sql(
        "SELECT * FROM predictions WHERE timestamp >= ? AND timestamp <= ? ORDER BY timestamp",
        conn, params=(start.isoformat(), end.isoformat()))
    conn.close()
    df['timestamp'] = pd.to_datetime(df['timestamp'])
    return df.set_index('timestamp')
    