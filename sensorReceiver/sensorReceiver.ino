#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ---------------- CONFIGURATION ----------------
const char* ssid = "EEnL";
const char* password = ".V@lidStudentQ";

// Supabase details
const char* supabase_url = "https://lhyxuuomjusjmkycuwuh.supabase.co/rest/v1/garden_data";
const char* supabase_config_url = "https://lhyxuuomjusjmkycuwuh.supabase.co/rest/v1/garden_config?id=eq.1&select=pump_mode,manual_override,soil_threshold";
const char* supabase_key = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImxoeXh1dW9tanVzam1reWN1d3VoIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDEwODc0NjIsImV4cCI6MjA1NjY2MzQ2Mn0.d_Ax_9uf6_yrx6lpwFzptpyL3AfB_VPo-sXEgavje54";

// ---------------- YOUR DEFINED PINS ----------------
#define LORA_SCK       18
#define LORA_MISO      19
#define LORA_MOSI      23
#define LORA_SS        15       
#define LORA_RST       27      
#define LORA_DIO0      26

#define BUZZER_PIN     14
#define RELAY_PIN      13

// Depending on your relay module (Active-High or Active-Low)
// Change this to LOW if you find it is working backwards!
#define RELAY_ON       HIGH
#define RELAY_OFF      LOW

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

  // Engage relay using defined logic
  digitalWrite(RELAY_PIN, relayActive ? RELAY_ON : RELAY_OFF);
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
  if (WiFi.status() != WL_CONNECTED) return;
  
  WiFiClientSecure client;
  client.setInsecure(); // Required for HTTPS on ESP32
  
  HTTPClient http;
  if (http.begin(client, supabase_url)) {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", supabase_key);
    http.addHeader("Authorization", "Bearer " + String(supabase_key));
    http.addHeader("Prefer", "return=minimal"); 
    
    String json = "{\"soil_moisture\": " + String(soil, 1) + 
                  ", \"temperature\": " + String(temp, 1) + 
                  ", \"relay_status\": " + (relayState ? "true" : "false") + "}";
    
    int code = http.POST(json);
    Serial.printf("[Supabase] Data POST Response: %d\n", code);
    http.end();
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

      // Extremely basic JSON string slicing suitable for this structure
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
        Serial.println("[Config] Settings updated from Dashboard!");
        applyPumpConfig();
      }
    } else {
      Serial.printf("[Supabase] Config GET failed: %d\n", httpCode);
    }
    http.end();
  }
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  // 1. WiFi Connection
  WiFi.begin(ssid, password);
  Serial.print("\n[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
  beep(2, 80, 80);

  // 2. LoRa Initialization
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("[LoRa] Init Failed! Check connections.");
    while (1);
  }
  LoRa.setSyncWord(SYNC_WORD);
  
  Serial.println("[LoRa] Garden Receiver Online. Waiting for transmitter...");
  
  // Initial config pull on boot
  getSupabaseConfig();
}

// ---------------- MAIN LOOP ----------------
void loop() {
  // 1. Check for manual config updates every 5 seconds
  if (millis() - lastConfigCheck > CONFIG_INTERVAL) {
    getSupabaseConfig();
    lastConfigCheck = millis();
  }

  // 2. Process incoming LoRa data
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  String incoming = "";
  while (LoRa.available()) {
    incoming += (char)LoRa.read();
  }

  int rssi = LoRa.packetRssi();

  // Validate format
  int soilIdx  = incoming.indexOf("soil=") + 5;
  int commaIdx = incoming.indexOf(",");
  int tempIdx  = incoming.indexOf("temp=") + 5;

  if (soilIdx < 5 || commaIdx == -1 || tempIdx < 5) {
    Serial.println("[Parse] Unrecognised packet — ignored: " + incoming);
    return;
  }

  lastSoilVal = incoming.substring(soilIdx, commaIdx).toFloat();
  lastTempVal = incoming.substring(tempIdx).toFloat();
  hasData = true;

  // Beep to confirm reception
  beep(1, 100);

  // 3. Evaluate & apply relay logic depending on mode/threshold
  applyPumpConfig();

  Serial.printf("[Data] Raw: %s | RSSI: %d | Relay: %s\n", 
                incoming.c_str(), rssi, lastRelayState ? "ON" : "OFF");

  // 4. Upload fresh data history to Supabase
  postToSupabase(lastSoilVal, lastTempVal, lastRelayState);
}
