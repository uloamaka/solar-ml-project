import requests
import pandas as pd

from features import LAT, LON, TZ

BASE_URL = 'https://api.open-meteo.com/v1/forecast'

RENAME = {
    'shortwave_radiation': 'ghi',
    'direct_normal_irradiance': 'dni',
    'diffuse_radiation': 'dhi',
    'temperature_2m': 'temp_air',
    'relative_humidity_2m': 'relative_humidity',
    'wind_speed_10m': 'wind_speed',
    'cloud_cover': 'cloud_cover',
}

HOURLY_VARS = list(RENAME.keys())


def fetch_forecast(latitude, longitude, tz, forecast_days=2, start_date=None, end_date=None):
    params = {
        'latitude': latitude,
        'longitude': longitude,
        'hourly': ','.join(HOURLY_VARS),
        'wind_speed_unit': 'ms',
        'timezone': tz,
    }
    if start_date and end_date:
        params['start_date'] = start_date
        params['end_date'] = end_date
    else:
        params['forecast_days'] = forecast_days

    resp = requests.get(BASE_URL, params=params, timeout=15)
    resp.raise_for_status()
    hourly = resp.json()['hourly']

    df = pd.DataFrame(hourly)
    df['time'] = pd.to_datetime(df['time'])
    df = df.set_index('time')
    df.index = df.index.tz_localize(tz)
    df = df.rename(columns=RENAME)
    return df[list(RENAME.values())]


def fetch_site_forecast(forecast_days=2, start_date=None, end_date=None):
    return fetch_forecast(LAT, LON, TZ, forecast_days=forecast_days,
                           start_date=start_date, end_date=end_date)