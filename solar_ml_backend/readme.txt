# Solar Power Forecasting Backend

Forecasts hourly power output (current-hour through day-ahead) for a 30W solar
panel in Warri, Nigeria, using Open-Meteo weather forecasts, pvlib solar
geometry, and a pre-trained XGBoost model. Predictions are compared against
real ESP32 telemetry on a local Grafana dashboard, with ThingSpeak used
separately for remote monitoring.

## Architecture

- **Open-Meteo** — hourly weather forecast (ghi, dni, dhi, temp_air,
  relative_humidity, wind_speed, cloud_cover)
- **pvlib** — solar zenith/azimuth and clearness index, computed identically
  at training time and inference time
- **XGBoost** — pre-trained model, predicts performance ratio (PR), converted
  to power using `pdc0` and GHI as a POA proxy
- **SQLite** (`predictions.db`) — stores both model predictions and real
  ESP32 readings, on the same hourly timestamp grid
- **Scheduler** (cron) — idempotent: backfills today from midnight, ensures
  tomorrow's day-ahead forecast exists, self-heals after power/network loss
- **FastAPI** — on-demand `/predict` for current + next hour, independent of
  the scheduled batch predictions
- **Mosquitto (MQTT)** — local broker for ESP32 telemetry, bridges a subset
  of messages onward to ThingSpeak for remote viewing
- **Grafana** — reads `predictions.db` directly via the SQLite plugin,
  overlays predicted vs. actual power on one chart, including future values
- **ThingSpeak** — remote monitoring only; not used for forecast display

## Repository contents

| File | Purpose |
|---|---|
| `features.py` | Site config, shared pvlib feature engineering, day/night gate |
| `open_meteo.py` | Forecast client, renamed/unit-aligned to match training data |
| `db.py` | SQLite schema, insert-if-absent writes, gap detection |
| `predictor.py` | Loads the model, applies the night gate, PR → power |
| `scheduler.py` | Cron entry point: backfill + day-ahead job |
| `main.py` | FastAPI service, on-demand `/predict` |
| `mqtt_subscriber.py` | Persistent daemon: ESP32 telemetry → SQLite |
| `mosquitto.conf` | Local broker + bridge to ThingSpeak |
| `requirements.txt` | Python dependencies |
| `solar-mqtt-subscriber.service` | systemd unit for the subscriber |
| `solar-api.service` | systemd unit for FastAPI |
| `xgb_model.json` | Trained model (not included — copy in separately) |

## Prerequisites

- Python 3.10+
- The trained model file, `xgb_model.json`, placed in the project root
- A ThingSpeak account with a channel created (public or private, doesn't
  matter) and an **MQTT Device** created under Devices > MQTT, with that
  channel added under Authorized Channels with **Publish** permission enabled
- `mosquitto` and `mosquitto-clients`
- Grafana with the `frser-sqlite-datasource` plugin

## Local installation (dev machine / laptop)

1. **Clone or copy the project**, then set up the Python environment:
   ```bash
   python3 -m venv venv
   source venv/bin/activate
   pip install -r requirements.txt
   ```

2. **Place the trained model** at `xgb_model.json` in the project root
   (path is set by `MODEL_PATH` in `predictor.py`).

3. **Install Mosquitto**:
   ```bash
   sudo apt install mosquitto mosquitto-clients
   ```
   If it auto-starts as a system service, stop it before testing a custom
   config manually: `sudo systemctl stop mosquitto`.

4. **Edit `mosquitto.conf`** and fill in the placeholders from your
   ThingSpeak MQTT Device:
   - `MQTT_USERNAME_FROM_THINGSPEAK`
   - `MQTT_PASSWORD_FROM_THINGSPEAK`
   - `MQTT_CLIENT_ID_FROM_THINGSPEAK`
   - `YOUR_CHANNEL_ID` (the numeric channel ID, appears twice)

5. **Run Mosquitto against this config** and confirm the bridge connects:
   ```bash
   mosquitto -c mosquitto.conf -v
   ```
   Look for `Received CONNACK on connection local...` — this confirms the
   bridge reached ThingSpeak successfully.

6. **Test the pipeline manually**, each in its own terminal:
   ```bash
   python3 mqtt_subscriber.py
   ```
   ```bash
   mosquitto_pub -h localhost -t solar/telemetry -m '{"power_w": 12.3}'
   mosquitto_pub -h localhost -t "channels/YOUR_CHANNEL_ID/publish" -m "field1=12.3"
   ```
   Confirm a row appears in the `actuals` table, and the value appears on the
   ThingSpeak channel (via its feed URL if the dashboard doesn't refresh
   immediately: `https://api.thingspeak.com/channels/YOUR_CHANNEL_ID/feeds.json?api_key=YOUR_READ_KEY&results=5`).

7. **Seed some predictions** for testing, regardless of time of day:
   ```bash
   python3 scheduler.py --force-tomorrow
   ```

8. **Run the API**:
   ```bash
   uvicorn main:app --host 0.0.0.0 --port 8000
   ```
   Test with `curl http://localhost:8000/predict`. To predict for current hour and the hour ahead.

9. **Set up Grafana**:
   ```bash
   sudo apt-get install grafana
   sudo systemctl enable --now grafana-server
   sudo grafana cli plugins install frser-sqlite-datasource
   sudo systemctl restart grafana-server
   ```
   Move the database to a location Grafana can read without touching your
   home directory's permissions:
   ```bash
   sudo mkdir -p /var/lib/solar_backend
   sudo chown $(whoami):grafana /var/lib/solar_backend
   sudo chmod 750 /var/lib/solar_backend
   mv predictions.db /var/lib/solar_backend/predictions.db
   sudo chown $(whoami):grafana /var/lib/solar_backend/predictions.db
   sudo chmod 640 /var/lib/solar_backend/predictions.db
   ```
   In Grafana: add a SQLite data source pointing at that path, create a Time
   series panel, and add two queries:
   ```sql
   SELECT timestamp AS time, predicted_power_w AS "Predicted power (W)" FROM predictions ORDER BY timestamp ASC
   ```
   ```sql
   SELECT timestamp AS time, measured_power_w AS "Actual power (W)" FROM actuals ORDER BY timestamp ASC
   ```
   Set the dashboard's time range to something like `now-6h` to `now+24h` to
   see the day-ahead forecast extend past the current moment.

## Raspberry Pi deployment

The steps are the same as above, with these differences:

- **Do not copy the `venv` folder over** — it contains compiled binaries
  specific to your laptop's CPU architecture (x86), which won't run on the
  Pi's ARM processor. Rebuild it fresh on the Pi using the same
  `requirements.txt`.
- **Update the username and paths** in `solar-mqtt-subscriber.service`,
  `solar-api.service`, and the crontab entry — they're currently hardcoded
  to a specific laptop user/path and need to match wherever the project
  lives on the Pi (commonly `/home/pi/...`).
- Raspberry Pi OS has native `systemd`, so none of the WSL workarounds
  (manual `grafana-server` invocation, `systemctl` not being available)
  should be needed there.
- Grafana and the SQLite plugin both have confirmed ARM builds; installation
  commands are identical to the ones above.

## Running as background services

Install the persistent services:
```bash
sudo cp solar-mqtt-subscriber.service /etc/systemd/system/
sudo cp solar-api.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now solar-mqtt-subscriber
sudo systemctl enable --now solar-api
```

Add the scheduler to cron (runs every 15 minutes, self-heals after outages):
```bash
crontab -e
```
```
*/15 * * * * /path/to/venv/bin/python3 /path/to/scheduler.py >> /path/to/scheduler.log 2>&1
```

## Configuration reference

Site-specific constants live in `features.py`:
- `LAT, LON, ALT = 5.572378, 5.843264, 50`
- `TZ = 'Africa/Lagos'`
- `ZENITH_NIGHT_THRESHOLD = 90.0` (degrees; gates model calls at night)

Model/site constants live in `predictor.py`:
- `PDC0 = 30.0` (panel rated power, watts)
- `MODEL_PATH = 'xgb_model.json'`

Scheduler behavior in `scheduler.py`:
- `NIGHTLY_TRIGGER_HOUR = 22` — hour after which tomorrow's forecast is
  generated; `--force-tomorrow` bypasses this for manual testing

## Known open items

- Security hardening before long-term unattended operation: Mosquitto's
  `allow_anonymous true` and FastAPI's wildcard CORS (`allow_origins=["*"]`)
  are fine for local testing, not for production.
- ESP32 firmware doesn't exist yet — everything above has been validated
  using `mosquitto_pub` to simulate its messages.
- No accuracy-evaluation view yet (MAE/RMSE between predicted and actual
  power) — a natural next addition once real telemetry accumulates.

## Troubleshooting notes

- **Mosquitto "Address already in use" on port 1883** — a systemd-managed
  Mosquitto instance is likely already running; `sudo systemctl stop
  mosquitto` before running a custom config manually, or drop your config
  into `/etc/mosquitto/conf.d/` and restart the service instead.
- **Grafana "permission denied" reading the SQLite file** — almost always a
  parent-directory permission issue, not the file itself; Linux requires
  execute (traversal) permission on every directory in the path, and home
  directories are usually locked down by default. Solved here by moving the
  database to `/var/lib/solar_backend/`.
- **`ImportError: sklearn needs to be installed`** — `xgboost`'s
  `XGBRegressor` wrapper needs `scikit-learn` as a separate dependency; it's
  not pulled in automatically.
- **WSL and `systemctl`** — if `systemctl` reports the system hasn't been
  booted with systemd, either enable systemd support in `/etc/wsl.conf`
  and restart WSL, or run services manually in a dedicated terminal instead.
  