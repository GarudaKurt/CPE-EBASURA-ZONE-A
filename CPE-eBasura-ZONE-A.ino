
#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ── WiFi ─────────────────────────────────────
const char* WIFI_SSID     = "Checo";
const char* WIFI_PASSWORD = "12345678";

const char* SERVER_IP   = "10.37.96.167";
const int   SERVER_PORT = 80;

// ── XSHUT Pins ───────────────────────────────
#define XSHUT_S1   2
#define XSHUT_S2   4
#define XSHUT_S3   5
#define XSHUT_S4  15

// ── I2C Addresses ────────────────────────────
#define ADDR_S1    0x30
#define ADDR_S2    0x31
#define ADDR_S3    0x32
#define ADDR_S4    0x33

// ── Servo Pins ───────────────────────────────
#define SERVO_PIN_1  13
#define SERVO_PIN_2  12
#define SERVO_PIN_3  14
#define SERVO_PIN_4  27

// ── Servo Angles ─────────────────────────────
#define SERVO_OPEN   160
#define SERVO_CLOSE   55

// ── FULL condition ───────────────────────────
#define FULL_DISTANCE_MM  20

#define NUM_REAL_BINS      4
#define POST_INTERVAL   2000
#define ZONE_A_ID_OFFSET   0

// ── Lookup tables ────────────────────────────
const int     XSHUT_PINS[NUM_REAL_BINS]  = { XSHUT_S1,    XSHUT_S2,    XSHUT_S3,    XSHUT_S4    };
const uint8_t ADDRESSES[NUM_REAL_BINS]   = { ADDR_S1,     ADDR_S2,     ADDR_S3,     ADDR_S4     };
const int     SERVO_PINS[NUM_REAL_BINS]  = { SERVO_PIN_1, SERVO_PIN_2, SERVO_PIN_3, SERVO_PIN_4 };

// ── Sensors & Servos ─────────────────────────
Adafruit_VL53L0X sensor[NUM_REAL_BINS];
Servo           servo[NUM_REAL_BINS];

// ── Data ─────────────────────────────────────
int  binDist[NUM_REAL_BINS] = {0};
bool binOk[NUM_REAL_BINS]   = {false};
bool lastState[NUM_REAL_BINS] = {false};

unsigned long lastPost = 0;

// ── Function prototypes ───────────────────────
void initSensors();
void readSensors();
void updateServos();
void postToServer();

// ======================================================
void setup() {

  Serial.begin(115200);
  delay(500);
  Serial.println("[BOOT] Zone A Starting...");

  // XSHUT — all LOW to reset
  for (int i = 0; i < NUM_REAL_BINS; i++) {
    pinMode(XSHUT_PINS[i], OUTPUT);
    digitalWrite(XSHUT_PINS[i], LOW);
  }
  delay(200);

  // I2C
  Wire.begin(21, 22);
  Wire.setClock(100000);

  // Attach servos and start CLOSED
  for (int i = 0; i < NUM_REAL_BINS; i++) {
    servo[i].attach(SERVO_PINS[i]);
    servo[i].write(SERVO_CLOSE);
    Serial.printf("[SERVO] A%d attached pin %d → CLOSE (%d°)\n",
                  i + 1, SERVO_PINS[i], SERVO_CLOSE);
  }
  delay(500);

  initSensors();

  // WiFi
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.printf("\nZone A ONLINE IP: %s\n",
    WiFi.localIP().toString().c_str());
}

// ======================================================
void loop() {

  readSensors();
  updateServos();

  if (millis() - lastPost >= POST_INTERVAL) {
    postToServer();
    lastPost = millis();
  }

  delay(100);
}

// ======================================================
// SENSOR INIT
// ======================================================
void initSensors() {

  Serial.println("[INIT] VL53L0X Sensors A1–A4");

  for (int i = 0; i < NUM_REAL_BINS; i++) {
    digitalWrite(XSHUT_PINS[i], LOW);
  }
  delay(200);

  for (int i = 0; i < NUM_REAL_BINS; i++) {

    digitalWrite(XSHUT_PINS[i], HIGH);
    delay(200);

    if (sensor[i].begin()) {
      sensor[i].setAddress(ADDRESSES[i]);
      Serial.printf("[OK] A%d → I2C 0x%02X\n", i + 1, ADDRESSES[i]);
    } else {
      Serial.printf("[ERR] A%d failed to init!\n", i + 1);
    }
  }
}

// ======================================================
// READ ALL SENSORS
// ======================================================
void readSensors() {

  VL53L0X_RangingMeasurementData_t m;

  for (int i = 0; i < NUM_REAL_BINS; i++) {

    sensor[i].rangingTest(&m, false);

    if (m.RangeStatus != 4) {
      binDist[i] = m.RangeMilliMeter;
      binOk[i]   = true;
    } else {
      binDist[i] = 0;
      binOk[i]   = false;
    }
  }

  Serial.printf("A1:%4dmm  A2:%4dmm  A3:%4dmm  A4:%4dmm\n",
    binDist[0], binDist[1], binDist[2], binDist[3]);
}

// ======================================================
// SERVO LOGIC — only moves on state change
// ======================================================
void updateServos() {

  for (int i = 0; i < NUM_REAL_BINS; i++) {

    if (!binOk[i]) continue;

    bool shouldOpen = (binDist[i] <= FULL_DISTANCE_MM);

    if (shouldOpen != lastState[i]) {

      if (shouldOpen) {
        servo[i].write(SERVO_OPEN);
        Serial.printf("[ALERT] A%d FULL → OPEN (%d°)\n", i + 1, SERVO_OPEN);
      } else {
        servo[i].write(SERVO_CLOSE);
        Serial.printf("[OK] A%d NOT FULL → CLOSE (%d°)\n", i + 1, SERVO_CLOSE);
      }

      lastState[i] = shouldOpen;
    }
  }
}

// ======================================================
// SERVER POST
// ======================================================
void postToServer() {

  if (WiFi.status() != WL_CONNECTED) return;

  JsonDocument doc;
  JsonArray arr = doc["bins"].to<JsonArray>();

  for (int i = 0; i < NUM_REAL_BINS; i++) {

    JsonObject b = arr.add<JsonObject>();
    b["id"]   = i + ZONE_A_ID_OFFSET;
    b["dist"] = binDist[i];
    b["ok"]   = binOk[i];
    b["open"] = lastState[i];
  }

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;

  String url =
    String("http://") +
    SERVER_IP + ":" +
    SERVER_PORT +
    "/api/zone/a";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(payload);
  Serial.printf("[HTTP] POST → %d\n", code);

  http.end();
}