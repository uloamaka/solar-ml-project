from typing import Optional

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import uvicorn
import xgboost as xgb

import numpy as np

app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=False,
    allow_methods=["*"],
    allow_headers=["*"],
)


# Load the trained model
#model = joblib.load("xgb_model.pkl")


model = xgb.XGBRegressor()
model.load_model("xgb_model.json")

class WeatherData(BaseModel):
    year: float
    ghi: float
    dni: float
    dhi: float
    temp_air: float
    relative_humidity: float
    wind_speed: float
    cloud_cover: float
    clearness_index: Optional[float]
    solar_zenith_angle: float
    solar_azimuth: float


@app.get("/")
def home():
    return {"status": "FastAPI is running"}


@app.post("/predict")
def predict(weather: WeatherData):
    features = np.array([[
        weather.year,
        weather.ghi,
        weather.dni,
        weather.dhi,
        weather.temp_air,
        weather.relative_humidity,
        weather.wind_speed,
        weather.cloud_cover,
        weather.clearness_index,
        weather.solar_zenith_angle,
        weather.solar_azimuth
    ]])

    print("\n==============================")
    print("DATA RECEIVED FROM REACT")
    print("==============================")

    print("Temperature:", weather.temp_air)
    print("Current Year:", weather.year)

    prediction = float(model.predict(features)[0])
    return {
        "success": True,
        "message": "FastAPI received the weather data",
        "prediction": prediction
    }


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)

