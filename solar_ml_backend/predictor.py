import numpy as np
import xgboost as xgb

from features import get_location, prepare_features, is_daylight, to_model_input

PDC0 = 30.0
MODEL_PATH = 'xgb_model.json'

_model = xgb.XGBRegressor()
_model.load_model(MODEL_PATH)


def predict_batch(weather_df):
    location = get_location()
    df = prepare_features(weather_df, location)
    daylight = is_daylight(df)

    df['predicted_pr'] = np.nan
    df['predicted_power_w'] = 0.0

    if daylight.any():
        x = to_model_input(df.loc[daylight])
        pr = _model.predict(x)
        df.loc[daylight, 'predicted_pr'] = pr
        df.loc[daylight, 'predicted_power_w'] = pr * PDC0 * (df.loc[daylight, 'ghi'] / 1000)

    return df