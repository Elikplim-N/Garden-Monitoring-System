#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ---------------- CONFIGURATION ----------------
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Supabase details
const char* supabase_url = "https://your-project.supabase.co/rest/v1/garden_data";
const char* supabase_config_url = "https://your-project.supabase.co/rest/v1/garden_config?id=eq.1&select=pump_mode,manual_override,soil_threshold";
const char* supabase_key = "YOUR_SUPABASE_ANON_KEY";

// ---------------- PIN DEFINITIONS (ESP32) ----------------
#define LORA_SCK       18 // Hardware VSPI Clock
#define LORA_MISO      19 // Hardware VSPI MISO
#define LORA_MOSI      23 // Hardware VSPI MOSI
#define LORA_SS        15 // Hardware VSPI SS
#define LORA_RST       27 // Reset
#define LORA_DIO0      26 // DIO0

#define BUZZER_PIN     21 // Buzzer positive (GPIO21)
#define RELAY_PIN      22 // Controls pump / solenoid (GPIO22)

// ---------------- LORA SETTINGS ----------------
#define LORA_FREQ      433E6
#define SYNC_WORD      0xF3

// ---------------- APP STATE & CONFIG ----------------
unsigned long lastConfigCheck = 0;
const unsigned long CONFIG_INTERVAL = 5000; // Check config every 5s

struct AppConfig {
  String pump_mode;
  bool manual_override;
  float soil_threshold;
} currentConfig = {"AUTO", false, 30.0};

bool hasData = false;
float lastSoilVal = 0.0;
float lastTempVal = 0.0;
bool lastRelayState = false;

// ---------------- CONTROL LOGIC ----------------
void applyPumpConfig() {
  bool relayActive = false;
  if (currentConfig.pump_mode == "MANUAL") {
    relayActive = currentConfig.manual_override;
    Serial.print("[Pump] Mode: MANUAL | Override: ");
    Serial.println(relayActive ? "ON" : "OFF");
  } else {
    if (hasData) {
      relayActive = (lastSoilVal < currentConfig.soil_threshold);
      Serial.printf("[Pump] Mode: AUTO | Threshold: %.1f%% | State: %s\n", 
                    currentConfig.soil_threshold, relayActive ? "ON" : "OFF");
    } else {
      relayActive = false; // Default off until sensors read
      Serial.println("[Pump] Mode: AUTO | Waiting for sensor data...");
    }
  }

  digitalWrite(RELAY_PIN, relayActive ? HIGH : LOW);
  lastRelayState = relayActive;
}

// ---------------- HELPERS ----------------

void beep(int times = 1, int onMs = 100, int offMs = 100) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);  if (i < times - 1) delay(offMs);
  }
}

void postToSupabase(float soil, float temp, bool relayState) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Not connected — skipping upload.");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Supabase uses HTTPS, skip cert validation for simplicity

  HTTPClient http;
  if (http.begin(client, supabase_url)) {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", supabase_key);
    http.addHeader("Authorization", "Bearer " + String(supabase_key));
    http.addHeader("Prefer", "return=minimal");   // Supabase best practice
  
    String json = "{\"soil_moisture\": " + String(soil, 1) +
                  ", \"temperature\": "  + String(temp, 1) +
                  ", \"relay_status\": " + (relayState ? "true" : "false") + "}";
  
    int code = http.POST(json);
    Serial.printf("[Supabase] Response: %d\n", code);
    http.end();
  } else {
    Serial.println("[Supabase] Connection failed.");
  }
}

void getSupabaseConfig() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure(); 

  HTTPClient http;
  if (http.begin(client, supabase_config_url)) {
    http.addHeader("apikey", supabase_key);
    http.addHeader("Authorization", "Bearer " + String(supabase_key));
  
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      bool configChanged = false;

      int modeIdx = payload.indexOf("\"pump_mode\":\"") + 13;
      int modeEndIdx = payload.indexOf("\"", modeIdx);
      if (modeIdx > 12) {
        String newMode = payload.substring(modeIdx, modeEndIdx);
        if (newMode != currentConfig.pump_mode) {
          currentConfig.pump_mode = newMode;
          configChanged = true;
        }
      }
  
      int overrideIdx = payload.indexOf("\"manual_override\":") + 18;
      if (overrideIdx > 17) {
         bool newOverride = (payload.substring(overrideIdx, overrideIdx+4) == "true");
         if (newOverride != currentConfig.manual_override) {
           currentConfig.manual_override = newOverride;
           configChanged = true;
         }
      }
  
      int threshIdx = payload.indexOf("\"soil_threshold\":") + 17;
      int threshEndIdx = payload.indexOf("}", threshIdx);
      if (threshEndIdx < 0) threshEndIdx = payload.indexOf("]", threshIdx);
      if (threshIdx > 16) {
         float newThresh = payload.substring(threshIdx, threshEndIdx).toFloat();
         if (newThresh != currentConfig.soil_threshold) {
           currentConfig.soil_threshold = newThresh;
           configChanged = true;
         }
      }

      if (configChanged) {
        Serial.println("[Config] Settings updated from dashboard");
        applyPumpConfig();
      }
    }
    http.end();
  }
}

// ---------------- SETUP ----------------

void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RELAY_PIN,  OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RELAY_PIN,  LOW); // Pump OFF on boot

  // 1. WiFi
  WiFi.begin(ssid, password);
  Serial.print("[WiFi] Connecting");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
    beep(2, 80, 80); // Two short beeps = WiFi OK
  } else {
    Serial.println("\n[WiFi] Failed — continuing without cloud upload.");
    beep(1, 500);    // One long beep = WiFi failed
  }

  // 2. LoRa
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("[LoRa] Init failed! Check pins 15/27/26.");
    while (true);
  }
  LoRa.setSyncWord(SYNC_WORD);
  Serial.println("[LoRa] Garden Receiver Online.");
}

// ---------------- MAIN LOOP ----------------

void loop() {
  // 1. Check for config updates every 5 seconds
  if (millis() - lastConfigCheck > CONFIG_INTERVAL) {
    getSupabaseConfig();
    lastConfigCheck = millis();
  }

  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  // Read packet
  String incoming = "";
  while (LoRa.available()) {
    incoming += (char)LoRa.read();
  }
  int rssi = LoRa.packetRssi();
  Serial.printf("[LoRa] Raw: \"%s\" | RSSI: %d dBm\n", incoming.c_str(), rssi);

  // Validate expected format: "soil=XX.X,temp=YY.Y"
  if (incoming.indexOf("soil=") < 0 || incoming.indexOf("temp=") < 0) {
    Serial.println("[Parse] Unrecognised packet — ignored.");
    return;
  }

  // Parse values
  int soilIdx  = incoming.indexOf("soil=") + 5;
  int commaIdx = incoming.indexOf(",");
  int tempIdx  = incoming.indexOf("temp=") + 5;

  lastSoilVal = incoming.substring(soilIdx, commaIdx).toFloat();
  lastTempVal = incoming.substring(tempIdx).toFloat();
  hasData = true;

  // Confirm reception with a beep
  beep(1, 100);

  // Evaluate & apply
  applyPumpConfig();

  Serial.printf("[Data] Soil: %.1f%% | Temp: %.1f°C | Pump: %s\n",
                lastSoilVal, lastTempVal, lastRelayState ? "ON" : "OFF");

  // Upload to Supabase
  postToSupabase(lastSoilVal, lastTempVal, lastRelayState);
}
