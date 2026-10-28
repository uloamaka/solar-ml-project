/*
 * 09_local_broker_realtime.ino   (reliability revision)
 *
 * Sensors: BH1750, INA219, DS18B20, AM2302, DS3231, SD, local MQTT (Mosquitto).
 * Deep sleep, DS3231-driven wake every SAMPLE_INTERVAL_MIN, day/night gated.
 */

#include <Wire.h>
#include <RTClib.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <driver/rtc_io.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <BH1750.h>
#include <Adafruit_INA219.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>

// Pins 
#define SDA_PIN      21
#define SCL_PIN      22
#define SD_CS_PIN    5
#define RTC_INT_PIN  GPIO_NUM_27
#define ONE_WIRE_PIN 4
#define DHT_PIN      15

// Schedule
#define SAMPLE_INTERVAL_MIN  1
#define DAY_START_HOUR       6
#define DAY_END_HOUR         18
#define MIN_ALARM_LEAD_S     2     // alarm must be at least this far in the future
#define BACKUP_TIMER_GRACE_S 90    // backup timer wake = planned wake + this

// Sensors
#define DHT_TYPE DHT22
#define BURST_SAMPLES   5
#define BURST_DELAY_MS  300
#define LUX_TO_IRRADIANCE 0.0079
#define BH1750_CALIBRATION_FACTOR 2.33

// Files
#define RAW_LOG_FILE      "/raw_log.csv"
#define HOURLY_LOG_FILE   "/hourly_avg.csv"
#define DEVICE_LOG_FILE   "/device_log.csv"
#define PENDING_FILE      "/pending_hourly.csv"   // hourly payloads not yet ACKed
#define PENDING_TMP_FILE  "/pending_tmp.csv"

// Fallback queue in RTC memory, used only if the SD write fails.
#define RTC_QUEUE_MAX  4
#define RTC_QUEUE_LINE 128

// Network
const char* ssid     = "*****";
const char* password = "*****";

const char* mqtt_server = "192.168.0.***";
const int   mqtt_port   = 1883;
const char* MQTT_CLIENT_ID = "esp32_solar_logger";

// Hourly power reading (Grafana forecast-vs-actual). Retried until ACKed.
const char* MQTT_TOPIC_TELEMETRY = "solar/telemetry";
// The Python subscriber publishes the hour string it stored here.
const char* MQTT_TOPIC_ACK       = "solar/ack";

#define THINGSPEAK_CHANNEL_ID "3493100"
const char* MQTT_TOPIC_THINGSPEAK = "channels/" THINGSPEAK_CHANNEL_ID "/publish";

const char* MQTT_TOPIC_LOGS   = "solar/logs";
const char* MQTT_TOPIC_STATUS = "solar/status";

#define WIFI_ATTEMPTS            3
#define WIFI_ATTEMPT_TIMEOUT_MS  8000
#define MQTT_ATTEMPTS            2
#define MQTT_SOCKET_TIMEOUT_S    5
#define MQTT_KEEPALIVE_S         30
#define MQTT_BUFFER_SIZE         512
#define ACK_TIMEOUT_MS           2000


WiFiClient espClient;
PubSubClient mqttClient(espClient);
RTC_DS3231 rtc;
BH1750 lightMeter(0x23);
Adafruit_INA219 ina219(0x40);
OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature ds18b20(&oneWire);
DeviceAddress ds18b20Addr;
DHT dht(DHT_PIN, DHT_TYPE);

// RAM state (reset every wake)
bool inMqttSession = false;
bool rtcOk = false;
bool sdOk  = false;
bool ds18Ok = false;
int  wakeQueued = 0;                 // hourly payloads queued during this wake

volatile bool ackReceived = false;
char ackExpected[24] = "";

struct Reading {
  float irradiance, panelTempC, ambientTempC, humidity, voltage, current, power;
};
Reading reading = {0, 0, 0, 0, 0, 0, 0};

// State kept across deep sleep 
RTC_DATA_ATTR uint32_t bucketStartUnix  = 0;   // unix time of the hour being accumulated
RTC_DATA_ATTR float    sumBucketPower   = 0;
RTC_DATA_ATTR int      bucketSampleCount = 0;

RTC_DATA_ATTR int   bootCount = 0;
RTC_DATA_ATTR float lastGoodPanelTemp   = 25.0;
RTC_DATA_ATTR float lastGoodAmbientTemp = 25.0;
RTC_DATA_ATTR float lastGoodHumidity    = 50.0;

RTC_DATA_ATTR int  logErrorCount = 0;
RTC_DATA_ATTR int  logWarnCount  = 0;
RTC_DATA_ATTR char lastLogLevel[8]    = "";
RTC_DATA_ATTR char lastLogMessage[80] = "";

RTC_DATA_ATTR char rtcQueue[RTC_QUEUE_MAX][RTC_QUEUE_LINE] = {};
RTC_DATA_ATTR int  rtcQueueCount = 0;

bool     isDaytime(const DateTime &now);
uint32_t hourStartUnix(const DateTime &d);
DateTime computeNextWake(const DateTime &t);
DateTime scheduleNextWake();
void     goToDeepSleep(const DateTime &nextWake);
void     initSensors();
void     takeSample(const DateTime &now);
void     foldIntoBucket(const DateTime &now);
void     queueCurrentBucket();
bool     queueHourly(const char *payload);
bool     hasPending();
bool     drainPending();
bool     publishHourlyWithAck(const char *line);
void     onMqttMessage(char *topic, byte *payload, unsigned int length);
bool     connectWiFi();
bool     connectMQTT();
bool     openMqttSession();
bool     openMqttSessionRetry(int attempts);
void     closeMqttSession();
void     publishThingSpeakReading();
bool     publishPendingLogs();
void     publishStatus();
void     logImportant(const char *level, const char *message);
const char *resetReasonName(esp_reset_reason_t r);

void setup() {
  Serial.begin(115200);
  delay(300);
  bootCount++;
  Serial.println("=================================");
  Serial.print("Boot #"); Serial.println(bootCount);

  Wire.begin(SDA_PIN, SCL_PIN);

  rtcOk = rtc.begin();
  if (!rtcOk) {
    Serial.println("ERROR: DS3231 not found - retrying in 60 s (timer wake).");
    Serial.flush();
    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
    esp_deep_sleep_start();
  }

  // Alarm pin setup. Clear stale flags so INT is released before we sleep again.
  rtc.writeSqwPinMode(DS3231_OFF);
  rtc.clearAlarm(1);
  rtc.clearAlarm(2);
  rtc.disableAlarm(1);

  // SD first, so everything below can be logged to it.
  for (int a = 0; a < 3 && !sdOk; a++) {
    sdOk = SD.begin(SD_CS_PIN);
    if (!sdOk) delay(100);
  }
  if (sdOk) Serial.println("SD card initialized.");
  else      logImportant("ERROR", "SD card init failed");

  if (rtc.lostPower()) {
    logImportant("WARN", "DS3231 lost power - time may be invalid");
    // rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); // uncomment once, reflash, comment back out
  }

  esp_reset_reason_t rr = esp_reset_reason();
  if (rr != ESP_RST_DEEPSLEEP) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Reset reason: %s (boot %d)", resetReasonName(rr), bootCount);
    bool bad = (rr == ESP_RST_BROWNOUT || rr == ESP_RST_PANIC ||
                rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT);
    logImportant(bad ? "ERROR" : "INFO", msg);
  }

  initSensors();

  DateTime now = rtc.now();
  Serial.printf("Current time: %04d-%02d-%02d %02d:%02d:%02d\n",
                now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());

  // Sample + local logging (no network involved)
  bool sampled = false;
  if (isDaytime(now)) {
    takeSample(now);
    foldIntoBucket(now);      // may queue the previous hour if a wake was skipped
    sampled = true;
  } else {
    Serial.println("Outside daytime window - skipping sample.");
  }

  // Set the next wake NOW, before any slow network work
  DateTime nextWake = scheduleNextWake();

  // If the next wake falls in a different hour (or tomorrow), this was the
  // last sample of the bucket: queue it now instead of waiting.
  if (sampled && bucketSampleCount > 0 && hourStartUnix(nextWake) != bucketStartUnix) {
    queueCurrentBucket();
  }

  // ONE network session: hourly queue, ThingSpeak, logs, status
  if (sampled || hasPending()) {
    int attempts = (wakeQueued > 0) ? 2 : 1;   // try harder when a fresh hour is waiting
    if (openMqttSessionRetry(attempts)) {
      if (hasPending()) drainPending();
      if (sampled) publishThingSpeakReading();
      publishPendingLogs();
      publishStatus();
      closeMqttSession();
    } else {
      logImportant("WARN", "MQTT session failed - live sample dropped, hourly queue kept");
    }
  }

  goToDeepSleep(nextWake);
}

void loop() {}

// SCHEDULING
bool isDaytime(const DateTime &now) {
  return (now.hour() >= DAY_START_HOUR && now.hour() < DAY_END_HOUR);
}

uint32_t hourStartUnix(const DateTime &d) {
  return d.unixtime() - (uint32_t)(d.minute() * 60 + d.second());
}

// Next wake: minute-aligned, at least MIN_ALARM_LEAD_S in the future, inside the day window.
DateTime computeNextWake(const DateTime &t) {
  DateTime base(t.year(), t.month(), t.day(), t.hour(), t.minute(), 0);
  DateTime next = base + TimeSpan(0, 0, SAMPLE_INTERVAL_MIN, 0);
  while (next.unixtime() <= t.unixtime() + MIN_ALARM_LEAD_S) {
    next = next + TimeSpan(0, 0, SAMPLE_INTERVAL_MIN, 0);
  }

  if (next.hour() >= DAY_END_HOUR || next.hour() < DAY_START_HOUR) {
    DateTime morning(next.year(), next.month(), next.day(), DAY_START_HOUR, 0, 0);
    if (morning.unixtime() <= t.unixtime() + MIN_ALARM_LEAD_S) {
      morning = morning + TimeSpan(1, 0, 0, 0);
    }
    next = morning;
  }
  return next;
}

DateTime scheduleNextWake() {
  DateTime nextWake = computeNextWake(rtc.now());

  Serial.printf("Next wake: %04d-%02d-%02d %02d:%02d\n",
                nextWake.year(), nextWake.month(), nextWake.day(),
                nextWake.hour(), nextWake.minute());

  rtc.clearAlarm(2);
  rtc.setAlarm2(nextWake, DS3231_A2_Hour);   // matches hour + minute
  return nextWake;
}

void goToDeepSleep(const DateTime &nextWake) {
  // Backup timer wake in case the DS3231 alarm is ever missed.
  int32_t remaining = (int32_t)nextWake.unixtime() - (int32_t)rtc.now().unixtime();
  if (remaining < 0) remaining = 0;
  uint64_t backupS = (uint64_t)remaining + BACKUP_TIMER_GRACE_S;
  esp_sleep_enable_timer_wakeup(backupS * 1000000ULL);

  Serial.println("Entering deep sleep now.");
  Serial.println("=================================");
  Serial.flush();

  // If INT is already low (alarm fired while we were busy), this wakes immediately.
  rtc_gpio_pullup_en(RTC_INT_PIN);
  rtc_gpio_pulldown_dis(RTC_INT_PIN);
  esp_sleep_enable_ext0_wakeup(RTC_INT_PIN, 0);
  esp_deep_sleep_start();
}

// SENSORS
// Re-init every wake - deep sleep wipes RAM, sensors themselves stay powered.
void initSensors() {
  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    logImportant("ERROR", "BH1750 init failed");
  } else {
    lightMeter.setMTreg(31);
  }

  if (!ina219.begin()) {
    logImportant("ERROR", "INA219 init failed");
  } else {
    ina219.setCalibration_32V_2A();
  }

  ds18b20.begin();
  ds18Ok = (ds18b20.getDeviceCount() > 0) && ds18b20.getAddress(ds18b20Addr, 0);
  if (!ds18Ok) {
    logImportant("ERROR", "DS18B20 not detected");
  } else {
    ds18b20.setResolution(ds18b20Addr, 12);
  }

  dht.begin();
}

void takeSample(const DateTime &now) {
  // BH1750 needs one full measurement after (re)configuration; discard the first read.
  delay(200);
  lightMeter.readLightLevel();

  float sumLux = 0, sumV = 0, sumI = 0, sumP = 0;

  for (int b = 0; b < BURST_SAMPLES; b++) {
    float lux = lightMeter.readLightLevel();
    if (lux < 0) lux = 0;

    float busV    = ina219.getBusVoltage_V();
    float shuntMV = ina219.getShuntVoltage_mV();
    float panelV  = busV + (shuntMV / 1000.0);
    float current = ina219.getCurrent_mA();
    float power   = ina219.getPower_mW() / 1000.0;

    sumLux += lux;
    sumV += panelV;
    sumI += current;
    sumP += power;

    delay(BURST_DELAY_MS);
  }

  reading.irradiance = (sumLux / BURST_SAMPLES) * LUX_TO_IRRADIANCE * BH1750_CALIBRATION_FACTOR;
  reading.voltage    = sumV / BURST_SAMPLES;
  reading.current    = sumI / BURST_SAMPLES;
  reading.power      = sumP / BURST_SAMPLES;

  // ---- Panel temperature (DS18B20) ----
  float panelTempC = DEVICE_DISCONNECTED_C;
  if (ds18Ok) {
    ds18b20.requestTemperatures();
    panelTempC = ds18b20.getTempC(ds18b20Addr);
  }
  // 85.0 C is the DS18B20 power-on-reset value, not a real reading.
  if (panelTempC == DEVICE_DISCONNECTED_C || panelTempC == 85.0) {
    logImportant("WARN", "DS18B20 read failed - using last good value");
    panelTempC = lastGoodPanelTemp;
  } else {
    lastGoodPanelTemp = panelTempC;
  }
  reading.panelTempC = panelTempC;

  // Ambient temp + humidity (AM2302), one retry
  float ambientTempC = lastGoodAmbientTemp;
  float humidity     = lastGoodHumidity;
  bool dhtOk = false;
  for (int a = 0; a < 2 && !dhtOk; a++) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      ambientTempC = t;
      humidity = h;
      lastGoodAmbientTemp = t;
      lastGoodHumidity = h;
      dhtOk = true;
    } else if (a == 0) {
      delay(2000);   // AM2302 needs >= 2 s between reads
    }
  }
  if (!dhtOk) logImportant("WARN", "AM2302 read failed - using last good value");
  reading.ambientTempC = ambientTempC;
  reading.humidity     = humidity;

  // Raw log on SD
  if (sdOk) {
    File rawFile = SD.open(RAW_LOG_FILE, FILE_APPEND);
    if (rawFile) {
      rawFile.printf("%04d-%02d-%02d %02d:%02d:%02d,%.2f,%.2f,%.2f,%.1f,%.3f,%.3f,%.3f\n",
                     now.year(), now.month(), now.day(),
                     now.hour(), now.minute(), now.second(),
                     reading.irradiance, reading.panelTempC, reading.ambientTempC,
                     reading.humidity, reading.voltage, reading.current, reading.power);
      rawFile.close();
    } else {
      logImportant("ERROR", "Raw log file open failed");
    }
  }

  Serial.printf("Sample: irr=%.1f W/m2 pT=%.1f aT=%.1f RH=%.1f V=%.2f I=%.1f mA P=%.3f W\n",
                reading.irradiance, reading.panelTempC, reading.ambientTempC,
                reading.humidity, reading.voltage, reading.current, reading.power);
}

// HOURLY BUCKET + QUEUE
// Fold the current sample into the bucket for its hour. If the sample belongs to a
// new hour and the old bucket still holds data (e.g. a wake was skipped), queue it.
void foldIntoBucket(const DateTime &now) {
  uint32_t hs = hourStartUnix(now);

  if (bucketStartUnix != hs) {
    if (bucketSampleCount > 0) queueCurrentBucket();
    bucketStartUnix = hs;
    sumBucketPower = 0;
    bucketSampleCount = 0;
  }

  sumBucketPower += reading.power;
  bucketSampleCount++;

  Serial.printf("Bucket %02d:00: %d sample(s) accumulated (this sample %.3f W)\n",
                DateTime(bucketStartUnix).hour(), bucketSampleCount, reading.power);
}

// Write the finished bucket to hourly_avg.csv and put its payload in the retry queue.
void queueCurrentBucket() {
  if (bucketSampleCount == 0 || bucketStartUnix == 0) return;

  float hourlyAvgPower = sumBucketPower / bucketSampleCount;
  DateTime hourStart(bucketStartUnix);
  DateTime nowT = rtc.now();

  if (sdOk) {
    File hf = SD.open(HOURLY_LOG_FILE, FILE_APPEND);
    if (hf) {
      hf.printf("%04d-%02d-%02d %02d:%02d:%02d,bucket_hour=%d,samples=%d,power_w=%.3f\n",
                nowT.year(), nowT.month(), nowT.day(),
                nowT.hour(), nowT.minute(), nowT.second(),
                hourStart.hour(), bucketSampleCount, hourlyAvgPower);
      hf.close();
    } else {
      logImportant("ERROR", "Hourly log file open failed");
    }
  }

  char payload[RTC_QUEUE_LINE];
  snprintf(payload, sizeof(payload),
           "{\"hour\":\"%04d-%02d-%02d %02d:00:00\",\"power_w\":%.3f,\"samples\":%d}",
           hourStart.year(), hourStart.month(), hourStart.day(), hourStart.hour(),
           hourlyAvgPower, bucketSampleCount);

  queueHourly(payload);
  wakeQueued++;
  Serial.printf("Queued hourly payload: %s\n", payload);

  sumBucketPower = 0;
  bucketSampleCount = 0;
}

// Append a payload to the SD queue; fall back to RTC memory if SD fails.
bool queueHourly(const char *payload) {
  if (sdOk) {
    File f = SD.open(PENDING_FILE, FILE_APPEND);
    if (f) {
      size_t n = f.println(payload);
      f.close();
      if (n > 0) return true;
    }
  }

  if (rtcQueueCount >= RTC_QUEUE_MAX) {              // full: drop the oldest
    for (int i = 1; i < RTC_QUEUE_MAX; i++) {
      memcpy(rtcQueue[i - 1], rtcQueue[i], RTC_QUEUE_LINE);
    }
    rtcQueueCount = RTC_QUEUE_MAX - 1;
  }
  strncpy(rtcQueue[rtcQueueCount], payload, RTC_QUEUE_LINE - 1);
  rtcQueue[rtcQueueCount][RTC_QUEUE_LINE - 1] = '\0';
  rtcQueueCount++;
  logImportant("ERROR", "Pending queue SD write failed - kept in RTC memory");
  return false;
}

bool hasPending() {
  if (rtcQueueCount > 0) return true;
  if (!sdOk) return false;
  return SD.exists(PENDING_FILE) || SD.exists(PENDING_TMP_FILE);
}

// Called from the MQTT client when a message arrives on solar/ack.
void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  if (length == strlen(ackExpected) && memcmp(payload, ackExpected, length) == 0) {
    ackReceived = true;
  }
}

// Publish one queued payload and wait for the subscriber to ACK its hour string.
// Returns true when ACKed (or when the line is malformed and should be dropped).
bool publishHourlyWithAck(const char *line) {
  const char *p = strstr(line, "\"hour\":\"");
  if (!p) return true;                 // malformed line: drop it
  p += 8;                              // length of "hour":"
  if (strlen(p) < 19) return true;     // malformed line: drop it

  memcpy(ackExpected, p, 19);
  ackExpected[19] = '\0';
  ackReceived = false;

  if (!mqttClient.publish(MQTT_TOPIC_TELEMETRY, line)) return false;

  unsigned long start = millis();
  while (!ackReceived && mqttClient.connected() && millis() - start < ACK_TIMEOUT_MS) {
    mqttClient.loop();
    delay(10);
  }
  Serial.printf("Hourly %s -> %s\n", ackExpected, ackReceived ? "ACKED" : "no ACK (kept for retry)");
  return ackReceived;
}

// Send everything that is queued. Stops at the first failure and keeps the rest.
bool drainPending() {
  bool stop = false;

  // 1) RTC-memory fallback queue
  int kept = 0;
  for (int i = 0; i < rtcQueueCount; i++) {
    if (!stop && publishHourlyWithAck(rtcQueue[i])) continue;
    stop = true;
    if (kept != i) memcpy(rtcQueue[kept], rtcQueue[i], RTC_QUEUE_LINE);
    kept++;
  }
  rtcQueueCount = kept;
  if (stop) return false;

  // 2) SD queue
  if (!sdOk) return true;

  // Recover from a power cut between remove() and rename() of a previous drain.
  if (!SD.exists(PENDING_FILE) && SD.exists(PENDING_TMP_FILE)) {
    SD.rename(PENDING_TMP_FILE, PENDING_FILE);
  }
  if (!SD.exists(PENDING_FILE)) return true;

  File in = SD.open(PENDING_FILE, FILE_READ);
  if (!in) return false;
  File keep = SD.open(PENDING_TMP_FILE, FILE_WRITE);   // FILE_WRITE truncates
  if (!keep) { in.close(); return false; }

  int sent = 0, retained = 0;
  while (in.available()) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    if (!stop && publishHourlyWithAck(line.c_str())) { sent++; continue; }
    stop = true;
    keep.println(line);
    retained++;
  }
  in.close();
  keep.close();

  SD.remove(PENDING_FILE);
  if (retained == 0) SD.remove(PENDING_TMP_FILE);
  else               SD.rename(PENDING_TMP_FILE, PENDING_FILE);

  Serial.printf("Queue drain: %d delivered, %d kept\n", sent, retained);
  return !stop;
}

// NETWORK
bool connectWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  for (int a = 0; a < WIFI_ATTEMPTS; a++) {
    WiFi.begin(ssid, password);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_ATTEMPT_TIMEOUT_MS) {
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) return true;

    Serial.printf("WiFi attempt %d failed (status %d)\n", a + 1, (int)WiFi.status());
    WiFi.disconnect(true);
    delay(200);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
  }

  logImportant("WARN", "WiFi failed after all attempts");
  return false;
}

bool connectMQTT() {
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
  mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
  mqttClient.setKeepAlive(MQTT_KEEPALIVE_S);
  mqttClient.setCallback(onMqttMessage);

  for (int a = 0; a < MQTT_ATTEMPTS; a++) {
    // Last-will "offline" (retained) only fires if we drop WITHOUT a clean disconnect.
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_TOPIC_STATUS, 1, true, "offline")) {
      mqttClient.subscribe(MQTT_TOPIC_ACK, 1);
      return true;
    }
    Serial.printf("MQTT connect attempt %d failed, rc=%d\n", a + 1, mqttClient.state());
    delay(500);
  }

  char msg[48];
  snprintf(msg, sizeof(msg), "MQTT connect failed rc=%d", mqttClient.state());
  logImportant("WARN", msg);
  return false;
}

bool openMqttSession() {
  if (inMqttSession) return true;
  if (!connectWiFi()) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }
  if (!connectMQTT()) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }
  inMqttSession = true;
  return true;
}

bool openMqttSessionRetry(int attempts) {
  for (int a = 0; a < attempts; a++) {
    if (openMqttSession()) return true;
    if (a + 1 < attempts) delay(3000);
  }
  return false;
}

void closeMqttSession() {
  if (!inMqttSession) return;

  // Let the client process anything pending and let lwIP flush its TCP send buffer
  // BEFORE the radio is switched off. publish() returning true is not delivery.
  for (int i = 0; i < 10; i++) { mqttClient.loop(); delay(20); }
  delay(300);
  mqttClient.disconnect();
  delay(100);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  inMqttSession = false;
}

void publishThingSpeakReading() {
  char payload[220];
  snprintf(payload, sizeof(payload),
           "field1=%.2f&field2=%.2f&field3=%.2f&field4=%.1f&field5=%.3f&field6=%.3f&field7=%.3f",
           reading.irradiance, reading.panelTempC, reading.ambientTempC, reading.humidity,
           reading.voltage, reading.current, reading.power);

  bool ok = mqttClient.publish(MQTT_TOPIC_THINGSPEAK, payload);
  mqttClient.loop();
  Serial.print("ThingSpeak publish "); Serial.println(ok ? "SUCCESS" : "FAILED");
  if (!ok) logImportant("WARN", "ThingSpeak publish failed (not retried - monitoring only)");
}

bool publishPendingLogs() {
  if (logErrorCount == 0 && logWarnCount == 0) return true;

  DateTime t = rtc.now();
  char payload[220];
  snprintf(payload, sizeof(payload),
           "{\"ts\":\"%04d-%02d-%02d %02d:%02d:%02d\",\"errors\":%d,\"warnings\":%d,\"last\":\"%s: %s\"}",
           t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second(),
           logErrorCount, logWarnCount, lastLogLevel, lastLogMessage);

  bool ok = mqttClient.publish(MQTT_TOPIC_LOGS, payload);
  if (ok) {
    logErrorCount = 0;
    logWarnCount = 0;
    lastLogLevel[0] = '\0';
    lastLogMessage[0] = '\0';
  }
  return ok;
}

// Retained "last successful session" heartbeat. Check the ts to see if the node is alive.
void publishStatus() {
  DateTime t = rtc.now();
  char payload[160];
  snprintf(payload, sizeof(payload),
           "{\"ts\":\"%04d-%02d-%02d %02d:%02d:%02d\",\"boot\":%d,\"rssi\":%d,\"rtc_queue\":%d}",
           t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second(),
           bootCount, (int)WiFi.RSSI(), rtcQueueCount);
  mqttClient.publish(MQTT_TOPIC_STATUS, payload, true);
}

// LOGGING
void logImportant(const char *level, const char *message) {
  Serial.printf("[%s] %s\n", level, message);

  if (sdOk) {
    File logFile = SD.open(DEVICE_LOG_FILE, FILE_APPEND);
    if (logFile) {
      if (rtcOk) {
        DateTime t = rtc.now();
        logFile.printf("%04d-%02d-%02d %02d:%02d:%02d,%s,%s\n",
                       t.year(), t.month(), t.day(),
                       t.hour(), t.minute(), t.second(), level, message);
      } else {
        logFile.printf("(no rtc),%s,%s\n", level, message);
      }
      logFile.close();
    }
  }

  if (strcmp(level, "ERROR") == 0)      logErrorCount++;
  else if (strcmp(level, "WARN") == 0)  logWarnCount++;
  else return;                           // INFO: local log only, not counted/published

  strncpy(lastLogLevel, level, sizeof(lastLogLevel) - 1);
  lastLogLevel[sizeof(lastLogLevel) - 1] = '\0';
  strncpy(lastLogMessage, message, sizeof(lastLogMessage) - 1);
  lastLogMessage[sizeof(lastLogMessage) - 1] = '\0';
}

const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic/crash";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
  }
}
