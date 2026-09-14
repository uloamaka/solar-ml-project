import { useState } from "react";
import axios from "axios";
import { getPosition } from 'suncalc';
import {
  Container,
  TextField,
  Button,
  Typography,
  Card,
  CardContent,
  Grid,
} from "@mui/material";

function App() {
  const [city, setCity] = useState("");
  const [weather, setWeather] = useState(null);
  const [error, setError] = useState(null);
  const [predict, setPredict] = useState(null);
  const [ghi, setGhi] = useState(null);
  // --- NEW CALCULATOR STATE ---
  const [systemCapacity, setSystemCapacity] = useState("");
  const [sunHours, setSunHours] = useState("");
  const [calculatedYield, setCalculatedYield] = useState(null);

  const fetchWeather = async () => {
    try {
      setError(null);
      setWeather(null);

      // Step 1: Get coordinates from Open-Meteo’s geocoding API
      const geoRes = await axios.get(
        `https://geocoding-api.open-meteo.com/v1/search?name=${city}&count=1`
      );

      if (!geoRes.data.results || geoRes.data.results.length === 0) {
        setError("City not found");
        return;
      }

      const year = new Date().getFullYear();
      const { latitude, longitude, name, country } = geoRes.data.results[0];

      // Step 2: Fetch current weather conditions from Open-Meteo’s weather API
      const weatherRes = await axios.get(
        `https://api.open-meteo.com/v1/forecast?latitude=${latitude}&longitude=${longitude}&current=temperature_2m,relative_humidity_2m,wind_speed_10m,shortwave_radiation,diffuse_radiation,direct_normal_irradiance,cloud_cover&timezone=Africa/Lagos`
      );

      const date = new Date(weatherRes.data.current.time);

      function calcExtraterrestrialIrradiance(date, lat, long) {
        const dayOfYear = Math.floor(
          (Date.UTC(date.getUTCFullYear(), date.getUTCMonth(), date.getUTCDate()) -
            Date.UTC(date.getUTCFullYear(), 0, 0)) /
            86400000
        );

        const Gsc = 1367; // Solar constant (W/m²)
        const decl =
          (23.45 * Math.PI) / 180 *
          Math.sin((2 * Math.PI / 365) * (284 + dayOfYear));

        const B = (2 * Math.PI / 364) * (dayOfYear - 81);
        const EoT =
          9.87 * Math.sin(2 * B) -
          7.53 * Math.cos(B) -
          1.5 * Math.sin(B);

        const TC = 4 * long + EoT;
        const LST =
          date.getUTCHours() + date.getUTCMinutes() / 60 + TC / 60;

        const HRA = (15 * (LST - 12)) * (Math.PI / 180);
        const latRad = lat * (Math.PI / 180);

        const cosZenith =
          Math.sin(latRad) * Math.sin(decl) +
          Math.cos(latRad) * Math.cos(decl) * Math.cos(HRA);

        const zenith = Math.acos(cosZenith);
        const E0 = 1 + 0.033 * Math.cos((2 * Math.PI / 365) * dayOfYear);

        const G0 = Gsc * E0 * Math.cos(zenith);
        return G0 > 0 ? G0 : 0;
      }

      // Sun position & irradiance calculations
      const position = getPosition(date, latitude, longitude);
      const solarAzimuth = position.azimuth;
      const solarZenith = position.altitude;

      const G0 = calcExtraterrestrialIrradiance(date, latitude, longitude);
      const clearnessIndex = G0 > 0 ? weatherRes.data.current.shortwave_radiation / G0 : null;

      // Step 3: Send weather data to backend
      const predictionRes = await axios.post(
        "http://127.0.0.1:8000/predict", {
          year: year,
          ghi: weatherRes.data.current.shortwave_radiation,
          dni: weatherRes.data.current.direct_normal_irradiance,
          dhi: weatherRes.data.current.diffuse_radiation,
          temp_air: weatherRes.data.current.temperature_2m,
          relative_humidity: weatherRes.data.current.relative_humidity_2m,
          wind_speed: weatherRes.data.current.wind_speed_10m,
          cloud_cover: weatherRes.data.current.cloud_cover,
          clearness_index: clearnessIndex,
          solar_zenith_angle: solarZenith,
          solar_azimuth: solarAzimuth
        }
      );
      
      setPredict((JSON.parse(predictionRes.data.prediction).toFixed(3)));
      setGhi((JSON.parse(predictionRes.data.ghi).toFixed(2)));

      setWeather({
        location: `${name}, ${country}`,
        current: weatherRes.data.current,
      });

    } catch (err) {
      setError("Failed to fetch weather data");
    }
  };

  // --- NEW CALCULATION LOGIC ---
  const handleCalculate = () => {
    const capacity = parseFloat(systemCapacity);
    

    if (!isNaN(capacity)&& ghi!==0) {
      // Daily Yield (kWh) = System Power (kW) * Performance Ratio
      const dailyYield = capacity * 0.8 *predict * ghi/1000;
      setCalculatedYield(dailyYield.toFixed(2));
    }
    
  };

  const handleClearCalculator = () => {
    setSystemCapacity("");
    //setSunHours("");
    setCalculatedYield(null);
  };

  //const ghi= weather.current.shortwave_radiation;
//console.log("GHI:", ghi);
  return (
    <Container maxWidth="md" style={{ marginTop: "40px", marginBottom: "40px" }}>
      <Typography variant="h4" gutterBottom>
        📶 Solar Panel Output Prediction Dashboard
      </Typography>
      
      <Grid container spacing={2} style={{ alignItems: "center" }}>
        <Grid item xs={8}>
          <TextField
            fullWidth
            label="Enter City"
            variant="outlined"
            value={city}
            onChange={(e) => setCity(e.target.value)}
          />
        </Grid>
        <Grid item xs={4}>
          <Button
            fullWidth
            variant="contained"
            color="primary"
            onClick={fetchWeather}
          >
            Get Weather
          </Button>
        </Grid>
      </Grid>

      {error && (
        <Typography color="error" style={{ marginTop: "20px" }}>
          {error}
        </Typography>
      )}

      {weather && (
        <Card style={{ marginTop: "30px" }}>
          <CardContent>
            <Typography variant="h5">{weather.location}</Typography>
            <Typography variant="subtitle1" style={{ marginTop: "10px" }}>
              Current Hour Conditions:
            </Typography>
            
            <Card variant="outlined" style={{ marginTop: "10px" }}>
              <CardContent>
                <Typography variant="body1" gutterBottom>
                  🕒 Time: {new Date(weather.current.time).toLocaleTimeString([], {
                    hour: "2-digit",
                    minute: "2-digit",
                  })}
                </Typography>
                <Typography variant="body2">
                  🌡️ Temperature: {weather.current.temperature_2m}°C
                </Typography>
                <Typography variant="body2">
                  ☼ Shortwave Radiation: {weather.current.shortwave_radiation} W/m²
                </Typography>
                <Typography variant="body2">
                  ☀️ Direct Normal Irradiance: {weather.current.direct_normal_irradiance} W/m²
                </Typography>
                <Typography variant="body2">
                  ☀️ Diffuse Radiation: {weather.current.diffuse_radiation} W/m²
                </Typography>
                <Typography variant="body2">
                  💧 Relative Humidity: {weather.current.relative_humidity_2m}%
                </Typography>
                <Typography variant="body2">
                  🌬️ Wind Speed (10m): {weather.current.wind_speed_10m} km/h
                </Typography>
                <Typography variant="body2">
                  ⛅ Cloud Cover: {weather.current.cloud_cover}%
                </Typography>
              </CardContent>
            </Card>
          </CardContent>
        </Card>
      )}

      {/* Prediction Output Section */}
      <Grid container spacing={2} style={{ marginTop: "20px", alignItems: "center" }}>
        <Grid item xs={12}>
          <Typography variant="h6" gutterBottom>  
            Predicted Panel Performance Ratio: 
          </Typography>
        </Grid>
        <Grid item xs={8}>
          <TextField
            fullWidth
            variant="outlined"
            value={predict || ""}
            InputProps={{ readOnly: true }}
          />
        </Grid>
        <Grid item xs={4}>
          <Button 
            onClick={() => setPredict(null)}
            fullWidth
            variant="outlined"
            color="secondary"
          >
            Clear Prediction
          </Button>
        </Grid>
      </Grid>

      {/* --- NEW USER INPUT & CALCULATOR SECTION --- */}
      <Card style={{ marginTop: "40px", backgroundColor: "#f9f9f9" }}>
        <CardContent>
          <Typography variant="h5" gutterBottom>
            📶Solar Panel Energy Yield Estimator
          </Typography>
          <Typography variant="body2" color="textSecondary" style={{ marginBottom: "20px" }}>
            Estimate solar panel energy output based on predicted performance ratio.
          </Typography>
          <Typography variant="body2" color="textSecondary" style={{ marginBottom: "20px" }}>
            Enter your solar panel system's rated power (in Watts):          
            </Typography>
          <Grid container spacing={2}>
            <Grid item xs={12} sm={6}>
              
              <TextField
                fullWidth
                type="number"
                label="Solar Rated Power(W)"
                variant="outlined"
                value={systemCapacity}
                onChange={(e) => setSystemCapacity(e.target.value)}
              />
            </Grid>
            

            <Grid item xs={6}>
              <Button
                fullWidth
                variant="contained"
                color="primary"
                onClick={handleCalculate}
              >
                Calculate Expected Power Output
              </Button>
            </Grid>
            <Grid item xs={6}>
              <Button
                fullWidth
                variant="outlined"
                onClick={handleClearCalculator}
              >
                Reset
              </Button>
            </Grid>

            {calculatedYield !== null && (
              <Grid item xs={12} style={{ marginTop: "15px" }}>
                <Card variant="outlined" style={{ backgroundColor: "#e8f5e9" }}>
                  <CardContent>
                    <Typography variant="h6" color="primary">
                      ⚡ Estimated Solar Power Output: {calculatedYield} W
                    </Typography>
                    <Typography variant="caption" color="textSecondary">
                      (Formula: Capacity × {0.8} × {predict} x retreived (GHI/1000))
                    </Typography>
                  </CardContent>
                </Card>
              </Grid>
            )}
          </Grid>
        </CardContent>
      </Card>

    </Container>
  );
}

export default App;