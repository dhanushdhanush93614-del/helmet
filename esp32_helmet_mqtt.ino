#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <MPU6050.h>
#include <HardwareSerial.h>
#include <TinyGPS++.h>
#include <DHT.h>

// ---------------- WIFI ----------------
const char* WIFI_SSID = "iPhone";
const char* WIFI_PASSWORD = "12345678";

// ---------------- MQTT ----------------
const char* MQTT_HOST = "u660b616.ala.eu-central-1.emqxsl.com";
const int MQTT_PORT = 8883;
const char* MQTT_USERNAME = "helmet123";
const char* MQTT_PASSWORD = "helmet123";
const char* MQTT_CLIENT_ID = "esp3";

const char* TOPIC_LIVE = "hel/live";
const char* TOPIC_ALERTS = "hel/alerts";
const char* TOPIC_STATUS = "hel/status";
const char* TOPIC_COMMAND = "hel/cmd";

// ---------------- PINS ----------------
#define FLAME_PIN 33
#define DHT_PIN 32
#define BUZZER 14

// ---------------- SIM800L ----------------
#define SIM800_TX 17
#define SIM800_RX 16
#define SIM800_BAUD 9600

const char* EMERGENCY_NUMBER = "8072907854";
const unsigned long CALL_DURATION = 20000;
const unsigned long CALL_COOLDOWN = 30000;

// ---------------- FAKE VALUES (STABLE) ----------------
const int ALCOHOL_VALUE = 400;
const float LAT = 13.055146;
const float LNG = 80.226799;

// ---------------- THRESHOLDS ----------------
const float SHAKE_THRESHOLD = 15.0;
const unsigned long SHAKE_TIME = 2000;
const float TEMP_THRESHOLD = 40.0;
const int FLAME_THRESHOLD = 2000;

// ---------------- OBJECTS ----------------
MPU6050 mpu;
DHT dht(DHT_PIN, DHT11);
WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);
HardwareSerial sim800(2);

// ---------------- VARIABLES ----------------
unsigned long shakeStart = 0;
bool accidentSent = false;
unsigned long lastLive = 0;
unsigned long lastStatus = 0;
unsigned long lastAlert = 0;
unsigned long lastReconnect = 0;
unsigned long lastCallTime = 0;
float lastTemp = 0;

// ---------------- FUNCTION DECLARATIONS ----------------
void connectWiFi();
void connectMQTT();
void publishLive(float lat, float lng, int alcohol, int flame, float temp, float accel, float tilt, const char* alert);
void sendAlert(const char* type, float lat, float lng, int alcohol, int flame, float temp, float accel, float tilt);
void publishStatus(const char* state);
void initSIM800();
void makeEmergencyCall();
void sendSIM800Command(const char* command, unsigned long waitMs);

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(9600);

  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  Wire.begin(21, 22);
  mpu.initialize();

  dht.begin();
  initSIM800();

  connectWiFi();

  secureClient.setInsecure();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  Serial.println("SYSTEM READY");
}

// ---------------- LOOP ----------------
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!mqtt.connected()) {
    connectMQTT();
  }

  mqtt.loop();

  // -------- SENSOR --------
  float lat = LAT;
  float lng = LNG;
  int alcohol = ALCOHOL_VALUE;
  int flame = analogRead(FLAME_PIN);

  float temp = dht.readTemperature();
  if (!isnan(temp)) {
    lastTemp = temp;
  } else {
    temp = lastTemp;
  }

  int16_t ax, ay, az;
  mpu.getAcceleration(&ax, &ay, &az);

  float accel = sqrt(ax * ax + ay * ay + az * az) / 16384.0;

  float pitch = atan2(ay, sqrt(ax * ax + az * az)) * 57.2958;
  float roll = atan2(-ax, az) * 57.2958;
  float tilt = max(abs(pitch), abs(roll));

  // -------- DEBUG --------
  Serial.println("\n===== LIVE DATA =====");
  Serial.println("Accel: " + String(accel));
  Serial.println("Tilt : " + String(tilt));
  Serial.println("Alcohol: " + String(alcohol));
  Serial.println("Flame: " + String(flame));
  Serial.println("Temp: " + String(temp));
  Serial.println("GPS: " + String(lat, 6) + "," + String(lng, 6));

  const char* alert = "none";

  // -------- ACCIDENT --------
  if (tilt >= SHAKE_THRESHOLD) {
    if (shakeStart == 0) {
      shakeStart = millis();
    }

    if (millis() - shakeStart >= SHAKE_TIME && !accidentSent) {
      Serial.println("ACCIDENT DETECTED");
      sendAlert("accident", lat, lng, alcohol, flame, temp, accel, tilt);
      accidentSent = true;
      alert = "accident";
    }
  } else {
    shakeStart = 0;
    accidentSent = false;
  }

  // -------- FIRE --------
  if (flame < FLAME_THRESHOLD && millis() - lastAlert > 10000) {
    sendAlert("fire", lat, lng, alcohol, flame, temp, accel, tilt);
    lastAlert = millis();
    alert = "fire";
  }

  // -------- TEMP --------
  if (temp > TEMP_THRESHOLD && millis() - lastAlert > 10000) {
    sendAlert("temperature", lat, lng, alcohol, flame, temp, accel, tilt);
    lastAlert = millis();
    alert = "temperature";
  }

  // -------- LIVE DATA --------
  if (millis() - lastLive > 2000) {
    publishLive(lat, lng, alcohol, flame, temp, accel, tilt, alert);
    lastLive = millis();
  }

  // -------- STATUS --------
  if (millis() - lastStatus > 5000) {
    publishStatus(accidentSent ? "Emergency" : "Monitoring");
    lastStatus = millis();
  }

  delay(500);
}

// ---------------- WIFI ----------------
void connectWiFi() {
  Serial.print("Connecting WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");
}

// ---------------- MQTT ----------------
void connectMQTT() {
  if (millis() - lastReconnect < 2000) {
    return;
  }

  lastReconnect = millis();

  Serial.print("Connecting MQTT...");
  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD)) {
    Serial.println("Connected");
    publishStatus("Monitoring");
  } else {
    Serial.println("Failed");
  }
}

// ---------------- LIVE ----------------
void publishLive(float lat, float lng, int alcohol, int flame, float temp, float accel, float tilt, const char* alert) {
  StaticJsonDocument<256> doc;
  char payload[256];

  doc["temperature"] = temp;
  doc["alcohol"] = alcohol;
  doc["flame"] = flame;
  doc["accel"] = accel;
  doc["tilt"] = tilt;
  doc["lat"] = lat;
  doc["lng"] = lng;
  doc["alert"] = alert;

  serializeJson(doc, payload);
  mqtt.publish(TOPIC_LIVE, payload, true);
}

// ---------------- ALERT ----------------
void sendAlert(const char* type, float lat, float lng, int alcohol, int flame, float temp, float accel, float tilt) {
  Serial.println("ALERT: " + String(type));
  digitalWrite(BUZZER, HIGH);

  StaticJsonDocument<256> doc;
  char payload[256];

  doc["alert"] = type;
  doc["lat"] = lat;
  doc["lng"] = lng;
  doc["temp"] = temp;

  serializeJson(doc, payload);
  mqtt.publish(TOPIC_ALERTS, payload, true);

  makeEmergencyCall();

  delay(3000);
  digitalWrite(BUZZER, LOW);
}

// ---------------- STATUS ----------------
void publishStatus(const char* state) {
  StaticJsonDocument<128> doc;
  char payload[128];

  doc["state"] = state;

  serializeJson(doc, payload);
  mqtt.publish(TOPIC_STATUS, payload, true);
}

// ---------------- SIM800L ----------------
void initSIM800() {
  sim800.begin(SIM800_BAUD, SERIAL_8N1, SIM800_RX, SIM800_TX);
  delay(1000);

  sendSIM800Command("AT", 1000);
  sendSIM800Command("ATE0", 1000);
  sendSIM800Command("AT+CLVL=100", 1000);

  Serial.println("SIM800L READY");
}

void makeEmergencyCall() {
  if (millis() - lastCallTime < CALL_COOLDOWN) {
    Serial.println("Call cooldown active");
    return;
  }

  lastCallTime = millis();

  Serial.println("CALLING: " + String(EMERGENCY_NUMBER));
  sim800.print("ATD");
  sim800.print(EMERGENCY_NUMBER);
  sim800.println(";");

  unsigned long startCall = millis();
  while (millis() - startCall < CALL_DURATION) {
    while (sim800.available()) {
      Serial.write(sim800.read());
    }
    delay(10);
  }

  sendSIM800Command("ATH", 1000);
  Serial.println("CALL ENDED");
}

void sendSIM800Command(const char* command, unsigned long waitMs) {
  sim800.println(command);

  unsigned long start = millis();
  while (millis() - start < waitMs) {
    while (sim800.available()) {
      Serial.write(sim800.read());
    }
    delay(10);
  }
}
