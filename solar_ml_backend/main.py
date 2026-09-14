import pandas as pd
import uvicorn
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from features import TZ
from open_meteo import fetch_site_forecast
from predictor import predict_batch

app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=False,
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.get("/")
def home():
    return {"status": "FastAPI is running"}


@app.get("/predict")
def predict():
    now = pd.Timestamp.now(tz=TZ)
    today = now.strftime('%Y-%m-%d')
    tomorrow = (now + pd.Timedelta(days=1)).strftime('%Y-%m-%d')

    weather = fetch_site_forecast(start_date=today, end_date=tomorrow)

    current_hour = now.floor('h')
    next_hour = current_hour + pd.Timedelta(hours=1)
    window = weather.loc[weather.index.isin([current_hour, next_hour])]

    predictions = predict_batch(window)

    result = []
    for ts, row in predictions.iterrows():
        pr = row['predicted_pr']
        result.append({
            "timestamp": ts.isoformat(),
            "predicted_pr": None if pd.isna(pr) else float(pr),
            "predicted_power_w": float(row['predicted_power_w']),
        })

    return {"success": True, "predictions": result}


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)