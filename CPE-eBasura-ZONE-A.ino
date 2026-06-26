// Zone A bins

#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

// ── WiFi ─────────────────────────────────────
const char* WIFI_SSID     = "Checo";
const char* WIFI_PASSWORD = "12345678";

const char* SERVER_IP   = "10.98.96.66";
const int   SERVER_PORT = 80;

// ── XSHUT Pins ───────────────────────────────
#define XSHUT_S1  26
#define XSHUT_S2  27
#define XSHUT_S3  25
#define XSHUT_S4  33

// ── I2C Addresses ────────────────────────────
#define ADDR_S1   0x30
#define ADDR_S2   0x31
#define ADDR_S3   0x32
#define ADDR_S4   0x33

// ── Servo Pins ───────────────────────────────
#define SERVO_PIN_1  19
#define SERVO_PIN_2  18
#define SERVO_PIN_3   4
#define SERVO_PIN_4   5

// ── Servo Angles ─────────────────────────────
#define SERVO_CLOSE   60
#define SERVO_OPEN   170

// ── Waste Level Thresholds ───────────────────
#define DIST_FULL      50   // < 50mm  → 100% FULL → close servo
#define DIST_HALF     100   // < 100mm → 50%
#define DIST_LOW      170   // > 170mm → 10% (nearly empty)

// ── Close delay ──────────────────────────────
#define CLOSE_DELAY_MS  1500   // 1.5s before re-opening after bin clears

// ── Forced-open duration (pick command) ──────
#define FORCED_OPEN_DURATION_MS  5000  // 5s hold-open after a pick

// ── Config ───────────────────────────────────
#define NUM_REAL_BINS     4
#define POST_INTERVAL  2000
#define ZONE_A_ID_OFFSET  0

// ── Lookup tables ────────────────────────────
const int     XSHUT_PINS[NUM_REAL_BINS] = { XSHUT_S1,    XSHUT_S2,    XSHUT_S3,    XSHUT_S4    };
const uint8_t ADDRESSES[NUM_REAL_BINS]  = { ADDR_S1,     ADDR_S2,     ADDR_S3,     ADDR_S4     };
const int     SERVO_PINS[NUM_REAL_BINS] = { SERVO_PIN_1, SERVO_PIN_2, SERVO_PIN_3, SERVO_PIN_4 };

// ── Sensors, Servos & Web server ─────────────
Adafruit_VL53L0X sensor[NUM_REAL_BINS];
Servo            servo[NUM_REAL_BINS];
AsyncWebServer   localServer(80);

// ── State ─────────────────────────────────────
int           binDist[NUM_REAL_BINS]    = {0};
int           binLevel[NUM_REAL_BINS]   = {0};
bool          binOk[NUM_REAL_BINS]      = {false};
bool          lastState[NUM_REAL_BINS]  = {false};  // true = CLOSED, false = OPEN

// ── Re-open timer per servo (non-blocking) ───
// 0 = no pending re-open
unsigned long closeTimer[NUM_REAL_BINS] = {0};

// ── Forced-open state (pick command) ─────────
// Prevents updateServos() from re-closing immediately after a pick
bool          forcedOpen[NUM_REAL_BINS]      = {false};
unsigned long forcedOpenTimer[NUM_REAL_BINS] = {0};

unsigned long lastPost = 0;

// ── Prototypes ────────────────────────────────
void initSensors();
void readSensors();
void updateServos();
void postToServer();
int  distToPercent(int distMm);
const char* percentLabel(int pct);

// =============================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("[BOOT] Zone A Starting...");

  for (int i = 0; i < NUM_REAL_BINS; i++) {
    pinMode(XSHUT_PINS[i], OUTPUT);
    digitalWrite(XSHUT_PINS[i], LOW);
  }
  delay(200);

  Wire.begin(21, 22);
  Wire.setClock(100000);
  Serial.println("[I2C]  Wire started (SDA=21, SCL=22)");

  for (int i = 0; i < NUM_REAL_BINS; i++) {
    servo[i].attach(SERVO_PINS[i]);
    servo[i].write(SERVO_OPEN);
    Serial.printf("[SERVO] A%d attached pin %d → OPEN (%d°)\n",
                  i + 1, SERVO_PINS[i], SERVO_OPEN);
  }
  delay(500);

  initSensors();

  Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[WIFI] Zone A ONLINE  IP: %s\n",
                WiFi.localIP().toString().c_str());

  // ── POST /api/pick — force open a specific servo ──
  // Called by the server ESP32, forwarding a pick request from the dashboard.
  // Body param: bin=<globalId>  (0–3 for Zone A)
  localServer.on("/api/pick", HTTP_POST, [](AsyncWebServerRequest* req) {
    int binId = -1;

    if (req->hasParam("bin", true)) {
      binId = req->getParam("bin", true)->value().toInt();
    }

    // Zone A owns global ids 0–3; local servo index = binId
    if (binId >= 0 && binId <= 3) {
      servo[binId].write(SERVO_OPEN);
      lastState[binId]       = false;        // false = OPEN
      closeTimer[binId]      = 0;            // cancel any pending re-open timer
      forcedOpen[binId]      = true;         // block updateServos() from re-closing
      forcedOpenTimer[binId] = millis();     // start the hold-open countdown
      Serial.printf("[PICK] A%d → forced OPEN via /api/pick (held for %ds)\n",
                    binId + 1, FORCED_OPEN_DURATION_MS / 1000);
      req->send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
      Serial.printf("[PICK] ERR — invalid bin id: %d\n", binId);
      req->send(400, "application/json", "{\"error\":\"invalid bin id for Zone A\"}");
    }
  });

  localServer.begin();
  Serial.println("[HTTP] Zone A local server started on port 80");

  Serial.println("=== Boot Complete ===\n");
}

// =============================================
void loop() {
  readSensors();
  updateServos();

  if (millis() - lastPost >= POST_INTERVAL) {
    postToServer();
    lastPost = millis();
  }

  delay(100);
}

// =============================================
// DISTANCE → PERCENTAGE
// =============================================
int distToPercent(int distMm) {
  if (distMm <= DIST_FULL) return 100;
  if (distMm <= DIST_HALF) return 50;
  if (distMm >  DIST_LOW)  return 10;
  return map(distMm, DIST_HALF, DIST_LOW, 50, 10);
}

const char* percentLabel(int pct) {
  if (pct == 100) return "FULL";
  if (pct >= 50)  return "HALF";
  return "LOW";
}

// =============================================
// SENSOR INIT
// =============================================
void initSensors() {
  Serial.println("[INIT] VL53L0X Sensors A1–A4");

  for (int i = 0; i < NUM_REAL_BINS; i++) {
    digitalWrite(XSHUT_PINS[i], LOW);
  }
  delay(200);

  for (int i = 0; i < NUM_REAL_BINS; i++) {
    digitalWrite(XSHUT_PINS[i], HIGH);
    delay(200);

    if (sensor[i].begin(0x29, false)) {
      sensor[i].setAddress(ADDRESSES[i]);
      Serial.printf("[OK]  A%d → I2C 0x%02X  (XSHUT=GPIO%d)\n",
                    i + 1, ADDRESSES[i], XSHUT_PINS[i]);
    } else {
      Serial.printf("[ERR] A%d failed! Check XSHUT=GPIO%d wiring.\n",
                    i + 1, XSHUT_PINS[i]);
    }
  }
  Serial.println("[INIT] Done\n");
}

// =============================================
// READ ALL SENSORS
// =============================================
void readSensors() {
  VL53L0X_RangingMeasurementData_t m;

  for (int i = 0; i < NUM_REAL_BINS; i++) {
    sensor[i].rangingTest(&m, false);

    if (m.RangeStatus != 4) {
      binDist[i]  = m.RangeMilliMeter;
      binLevel[i] = distToPercent(binDist[i]);
      binOk[i]    = true;
    } else {
      binDist[i]  = 0;
      binLevel[i] = 0;
      binOk[i]    = false;
    }
  }

  Serial.println("┌────────┬──────────┬──────────┬──────────────┐");
  Serial.println("│  Bin   │  Dist    │  Level   │  Servo       │");
  Serial.println("├────────┼──────────┼──────────┼──────────────┤");
  for (int i = 0; i < NUM_REAL_BINS; i++) {
    if (binOk[i]) {
      const char* servoStatus;
      if (forcedOpen[i])          servoStatus = "FORCED OPEN";
      else if (lastState[i])      servoStatus = "CLOSED";
      else if (closeTimer[i] > 0) servoStatus = "OPENING...";
      else                        servoStatus = "OPEN";

      Serial.printf("│  A%-2d   │  %4dmm  │  %3d%% %-4s│  %-12s│\n",
                    i + 1, binDist[i], binLevel[i],
                    percentLabel(binLevel[i]),
                    servoStatus);
    } else {
      Serial.printf("│  A%-2d   │   ERR    │   ---    │   ---        │\n", i + 1);
    }
  }
  Serial.println("└────────┴──────────┴──────────┴──────────────┘");
}

// =============================================
// SERVO LOGIC — non-blocking 1.5s re-open delay
// lastState: true = CLOSED (bin full), false = OPEN (default)
// =============================================
void updateServos() {
  unsigned long now = millis();

  for (int i = 0; i < NUM_REAL_BINS; i++) {

    // ── Forced-open guard — skip normal logic while active ──
    if (forcedOpen[i]) {
      if (now - forcedOpenTimer[i] >= FORCED_OPEN_DURATION_MS) {
        forcedOpen[i] = false;
        Serial.printf("[PICK] A%d forced-open expired → resuming normal logic\n", i + 1);
        // Fall through to normal logic immediately
      } else {
        continue;  // Hold open; don't let sensor re-close this bin
      }
    }

    if (!binOk[i]) {
      Serial.printf("[SKIP] A%d bad reading — servo unchanged\n", i + 1);
      continue;
    }

    bool shouldClose = (binLevel[i] == 100);

    if (shouldClose) {
      // ── Bin is FULL → close immediately ───────
      if (closeTimer[i] > 0) {
        closeTimer[i] = 0;
        Serial.printf("[TIMER] A%d re-open timer cancelled — bin still FULL\n", i + 1);
      }

      if (!lastState[i]) {
        servo[i].write(SERVO_CLOSE);
        lastState[i] = true;   // true = CLOSED
        Serial.printf("[CLOSE] A%d → %d%% FULL → CLOSE (%d°)\n",
                      i + 1, binLevel[i], SERVO_CLOSE);
      }

    } else {
      // ── Bin is NOT full → re-open after delay ─
      if (lastState[i]) {
        if (closeTimer[i] == 0) {
          closeTimer[i] = now;
          Serial.printf("[TIMER] A%d bin cleared → re-opening in %.1fs...\n",
                        i + 1, CLOSE_DELAY_MS / 1000.0);
        }
      }

      // ── Check if re-open timer has elapsed ────
      if (closeTimer[i] > 0 && (now - closeTimer[i] >= CLOSE_DELAY_MS)) {
        servo[i].write(SERVO_OPEN);
        lastState[i]  = false;   // false = OPEN
        closeTimer[i] = 0;
        Serial.printf("[OPEN]  A%d → delay elapsed → OPEN (%d°)\n",
                      i + 1, SERVO_OPEN);
      }
    }
  }
}

// =============================================
// HTTP POST TO SERVER
// =============================================
void postToServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] WiFi disconnected — skipping POST");
    return;
  }

  JsonDocument doc;
  JsonArray arr = doc["bins"].to<JsonArray>();

  for (int i = 0; i < NUM_REAL_BINS; i++) {
    JsonObject b = arr.add<JsonObject>();
    b["id"]    = i + ZONE_A_ID_OFFSET;
    b["dist"]  = binDist[i];
    b["level"] = binLevel[i];
    b["ok"]    = binOk[i];
    b["open"]  = lastState[i];
  }

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  String url = String("http://") + SERVER_IP + ":" + SERVER_PORT + "/api/zone/a";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(payload);
  Serial.printf("[HTTP] POST %s → %d\n", url.c_str(), code);

  if (code < 0) {
    Serial.printf("[HTTP] Error: %s\n", HTTPClient::errorToString(code).c_str());
  }

  http.end();
}
