#include <SPI.h>
#include <LoRa.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ---------------- PIN DEFINITIONS (Using your WORKING pins) ----------------
#define SOIL_PIN       34      
#define ONE_WIRE_BUS   4       

#define LORA_SCK       18
#define LORA_MISO      19
#define LORA_MOSI      23
#define LORA_SS        15       // Update if you moved this to 15
#define LORA_RST       27      // Update if you moved this to 27 or 2
#define LORA_DIO0      26

#define LORA_FREQ      433E6
#define SYNC_WORD      0xF3

// ---------------- SENSOR OBJECTS ----------------
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);

// ---------------- CALIBRATION ----------------
float dryValue = 3030.0;   // Based on your recent "Air" reading
float wetValue = 1200.0;   // Estimated water reading

unsigned long lastSendTime = 0;
unsigned long interval = 10000; 

void setup() {
  Serial.begin(115200);
  
  // Give the hardware a second to breathe
  delay(1000);

  analogReadResolution(12);
  ds18b20.begin();

  // Initialize SPI & LoRa
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("Critical Error: LoRa lost after sensor boot!");
    while (1);
  }

  LoRa.setSyncWord(SYNC_WORD);
  Serial.println("System Online: Sending Sensor Data...");
}

void loop() {
  if (millis() - lastSendTime > interval) {
    lastSendTime = millis();

    // 1. Read Soil
    float raw = 0;
    for(int i=0; i<10; i++) raw += analogRead(SOIL_PIN);
    raw /= 10;
    float soilPct = (dryValue - raw) * 100.0 / (dryValue - wetValue);
    soilPct = constrain(soilPct, 0, 100);

    // 2. Read Temp
    ds18b20.requestTemperatures();
    float tempC = ds18b20.getTempCByIndex(0);

    // 3. Send LoRa Packet
    Serial.print("Sending: ");
    Serial.print(soilPct); Serial.print("% | ");
    Serial.println(tempC);

    LoRa.beginPacket();
    LoRa.print("soil="); LoRa.print(soilPct, 1);
    LoRa.print(",temp="); LoRa.print(tempC, 1);
    LoRa.endPacket();
  }
}