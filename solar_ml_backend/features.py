import pvlib
from pvlib.location import Location

LAT, LON, ALT = 5.572378, 5.843264, 50
TZ = 'Africa/Lagos'

FEATURES = [
    'year', 'ghi', 'dni', 'dhi', 'temp_air', 'relative_humidity',
    'wind_speed', 'cloud_cover', 'clearness_index',
    'solar_zenith_angle', 'solar_azimuth',
]
TARGET = 'performance_ratio'

ZENITH_NIGHT_THRESHOLD = 90.0


def get_location():
    return Location(LAT, LON, tz=TZ, altitude=ALT, name='Warri, Nigeria')


def add_solar_geometry(df, location):
    solpos = location.get_solarposition(df.index)
    df['solar_zenith_angle'] = solpos['apparent_zenith']
    df['solar_azimuth'] = solpos['azimuth']
    df['clearness_index'] = pvlib.irradiance.clearness_index(
        ghi=df['ghi'],
        solar_zenith=df['solar_zenith_angle'],
        extra_radiation=pvlib.irradiance.get_extra_radiation(df.index),
        min_cos_zenith=0.065,
        max_clearness_index=2.0)
    return df


def prepare_features(weather_df, location):
    df = weather_df.copy()
    df['year'] = df.index.year
    df = add_solar_geometry(df, location)
    return df


def is_daylight(df):
    return df['solar_zenith_angle'] < ZENITH_NIGHT_THRESHOLD


def to_model_input(df):
    return df[FEATURES].to_numpy()