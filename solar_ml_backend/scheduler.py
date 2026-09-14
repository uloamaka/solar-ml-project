import pandas as pd

import db
from features import TZ
from open_meteo import fetch_site_forecast
from predictor import predict_batch

NIGHTLY_TRIGGER_HOUR = 22


def backfill_today():
    now = pd.Timestamp.now(tz=TZ)
    midnight = now.normalize()
    missing = db.get_missing_hours(midnight, now.floor('h'))
    if not missing:
        return

    start = missing[0].strftime('%Y-%m-%d')
    end = missing[-1].strftime('%Y-%m-%d')
    weather = fetch_site_forecast(start_date=start, end_date=end)
    weather = weather.loc[weather.index.isin(missing)]

    predictions = predict_batch(weather)
    db.insert_predictions(predictions, source='catchup')


def ensure_tomorrow(force=False):
    now = pd.Timestamp.now(tz=TZ)
    if not force and now.hour < NIGHTLY_TRIGGER_HOUR:
        return

    tomorrow = (now + pd.Timedelta(days=1)).normalize()
    tomorrow_end = tomorrow + pd.Timedelta(hours=23)
    missing = db.get_missing_hours(tomorrow, tomorrow_end)
    if not missing:
        return

    day_str = tomorrow.strftime('%Y-%m-%d')
    weather = fetch_site_forecast(start_date=day_str, end_date=day_str)

    predictions = predict_batch(weather)
    db.insert_predictions(predictions, source='scheduled')


def run_once(force_tomorrow=False):
    db.init_db()
    backfill_today()
    ensure_tomorrow(force=force_tomorrow)


if __name__ == '__main__':
    import sys
    force = '--force-tomorrow' in sys.argv
    run_once(force_tomorrow=force)
