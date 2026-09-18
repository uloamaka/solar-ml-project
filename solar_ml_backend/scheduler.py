import pandas as pd

import db
from features import TZ
from open_meteo import fetch_site_forecast
from predictor import predict_batch

NIGHTLY_TRIGGER_HOUR = 22


def backfill_today():
    now = pd.Timestamp.now(tz=TZ)
    midnight = now.normalize()
    cutoff = now.floor('h')
    today_str = now.strftime('%Y-%m-%d')

    weather = fetch_site_forecast(start_date=today_str, end_date=today_str)
    predictions = predict_batch(weather)

    past = predictions.loc[predictions.index < cutoff]
    live = predictions.loc[predictions.index >= cutoff]

    db.insert_predictions(past, source='catchup', mode='ignore')
    db.insert_predictions(live, source='scheduled', mode='replace')


def ensure_tomorrow(force=False):
    now = pd.Timestamp.now(tz=TZ)
    if not force and now.hour < NIGHTLY_TRIGGER_HOUR:
        return

    tomorrow = (now + pd.Timedelta(days=1)).normalize()
    day_str = tomorrow.strftime('%Y-%m-%d')
    weather = fetch_site_forecast(start_date=day_str, end_date=day_str)

    predictions = predict_batch(weather)
    db.insert_predictions(predictions, source='scheduled', mode='replace')


def run_once(force_tomorrow=False):
    db.init_db()
    backfill_today()
    ensure_tomorrow(force=force_tomorrow)


if __name__ == '__main__':
    import sys
    force = '--force-tomorrow' in sys.argv
    run_once(force_tomorrow=force)
