/*
 * ZoneA.ino — MOCK DATA MODE
 * Skips all sensors, POSTs fake distance data to confirm
 * network connectivity with the main server.
 */

#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ── Config ───────────────────────────────────
const char* WIFI_SSID     = "CPEeBasura";
const char* WIFI_PASSWORD = "cpeebasura123";

const char* SERVER_IP  = "10.109.102.167";
const int   SERVER_PORT = 80;

#define TCA_ADDRESS    0x70
#define NUM_ZONE_BINS  8
#define BIN_DEPTH_MM   400
#define FULL_THRESHOLD 90
#define POST_INTERVAL  2000

// ── Mock distances (mm) ───────────────────────
// 400mm = 0% full, 0mm = 100% full
const int MOCK_DIST[NUM_ZONE_BINS] = {
  352,  // Bin A1 ~12%
  264,  // Bin A2 ~34%
   88,  // Bin A3 ~78%
  180,  // Bin A4 ~55%
   32,  // Bin A5 ~92% (FULL)
  308,  // Bin A6 ~23%
  132,  // Bin A7 ~67%
  220   // Bin A8 ~45%
};

// ── Function declarations ─────────────────────
void postToServer();

// ── Timing ────────────────────────────────────
unsigned long lastPost = 0;

// ======================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.printf("\nZone A online. IP: %s\n",
    WiFi.localIP().toString().c_str());
  Serial.println("*** MOCK DATA MODE — sensors skipped ***");
}

// ======================================================
void loop() {
  unsigned long now = millis();

  if (now - lastPost >= POST_INTERVAL) {
    postToServer();
    lastPost = now;
  }
}

// ======================================================
void postToServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WARN] WiFi dropped, skipping POST");
    return;
  }

  // Build JSON payload
  JsonDocument doc;
  JsonArray arr = doc["bins"].to<JsonArray>();

  for (int i = 0; i < NUM_ZONE_BINS; i++) {
    JsonObject b = arr.add<JsonObject>();
    b["id"]   = i;
    b["dist"] = MOCK_DIST[i];
    b["ok"]   = true;
  }

  String payload;
  serializeJson(doc, payload);

  // ── Print what we're about to send ───────────
  Serial.println("─────────────────────────────────");
  Serial.println("[ZONE A] Sending bin data:");
  for (int i = 0; i < NUM_ZONE_BINS; i++) {
    float fill = 100.0f * (1.0f - (float)MOCK_DIST[i] / BIN_DEPTH_MM);
    Serial.printf("  Bin A%d (id=%d) | dist=%4d mm | fill=%3.0f%%\n",
      i + 1, i, MOCK_DIST[i], fill);
  }

  // ── POST ─────────────────────────────────────
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);

  String url = String("http://") + SERVER_IP +
               ":" + SERVER_PORT + "/api/zone/a";

  if (!http.begin(url)) {
    Serial.println("[ERR] http.begin() failed");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(payload);

  if (httpCode == 200) {
    Serial.printf("[OK]  Server accepted. HTTP %d\n", httpCode);
  } else {
    Serial.printf("[ERR] HTTP %d — %s\n",
      httpCode,
      HTTPClient::errorToString(httpCode).c_str());
  }

  http.end();
}