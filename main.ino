#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <time.h>

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600; // Thailand GMT+7
const int   daylightOffset_sec = 0;

// ===== WIFI =====
const char* ssid = "CAMEL_2.4G";
const char* password = "0949356256";

// ===== THINGSBOARD =====
const char* mqtt_server = "eu.thingsboard.cloud";
const char* token = "3eoRYReQjSfuRgh7ygOg";

enum PH_State {
  IDLE,
  PUMP1_ON,
  PUMP2_ON
};

PH_State phState = IDLE;
unsigned long phTimer = 0;
float PH_LOW_THRESHOLD = 6.5; // ปรับตามจริง

// ===== MQTT =====
WiFiClient espClient;
PubSubClient client(espClient);

// ===== TEMP =====
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ===== ANALOG =====
#define PH_PIN 34
#define CL_PIN 35

// ===== FLOAT =====
#define FLOAT_LOW   27
#define FLOAT_HIGH  26

// ===== RELAY ===== (กำหนดขาจริง!!)
#define LED  14
#define Pump1 12
#define Pump2 13
#define water_filter 25

// ===== MODE =====
bool autoMode = true;

// ===== RELAY STATE =====
bool ledState = false;
bool pump1State = false;
bool pump2State = false;
bool filterState = false;

// ================= WIFI =================
void setup_wifi() {
  delay(10);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
}

// ================= CALLBACK =================
void callback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  deserializeJson(doc, payload, length);

  String method = doc["method"];

  if (method == "setMode") {
    String mode = doc["params"]["mode"];
    autoMode = (mode == "auto");
  }

  if (!autoMode) {
    if (method == "toggleLED") ledState = !ledState;
    if (method == "togglePump1") pump1State = !pump1State;
    if (method == "togglePump2") pump2State = !pump2State;
    if (method == "toggleFilter") filterState = !filterState;
  }
}

// ================= RECONNECT =================
void reconnect() {
  while (!client.connected()) {
    if (client.connect("ESP32", token, NULL)) {
      client.subscribe("v1/devices/me/rpc/request/+");
    } else {
      delay(2000);
    }
  }
}

// ================= APPLY RELAY =================
void applyRelay() {
  digitalWrite(LED, ledState);
  digitalWrite(Pump1, pump1State);
  digitalWrite(Pump2, pump2State);
  digitalWrite(water_filter, filterState);
}

// ================= AUTO LOGIC =================
void autoControl(int lowLevel, int highLevel) {

  if (!autoMode) return;

  // ตัวอย่าง logic
  if (lowLevel == HIGH && highLevel == HIGH) {
    pump1State = true;   // เติมน้ำ
  }
  else if (highLevel == LOW) {
    pump1State = false;  // เต็มแล้วหยุด
  }

  // filter ทำงานตลอดใน auto
  filterState = true;
}

// ================= SEND DATA =================
void sendData(float temp, float ph, float chlorine, int lowLevel, int highLevel) {

  String payload = "{";
  payload += "\"temp\":" + String(temp) + ",";
  payload += "\"pH\":" + String(ph) + ",";
  payload += "\"chlorine\":" + String(chlorine) + ",";
  payload += "\"lowLevel\":" + String(lowLevel) + ",";
  payload += "\"highLevel\":" + String(highLevel) + ",";

  payload += "\"autoMode\":" + String(autoMode ? "true" : "false") + ",";

  payload += "\"LED\":" + String(ledState ? "true" : "false") + ",";
  payload += "\"Pump1\":" + String(pump1State ? "true" : "false") + ",";
  payload += "\"Pump2\":" + String(pump2State ? "true" : "false") + ",";
  payload += "\"Filter\":" + String(filterState ? "true" : "false");

  payload += "}";
  Serial.println(payload);
  client.publish("v1/devices/me/telemetry", payload.c_str());
}

void checkTimeCondition() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Time read error");
    return;
  }

  int hourNow = timeinfo.tm_hour;
  int minuteNow = timeinfo.tm_min;

  // ===== 16:00 =====
  if (hourNow == 16 && minuteNow == 0) {
    Serial.print("Now time: ");
    Serial.println(&timeinfo, "%H:%M:%S");
  }

  // ===== 19:00 =====
  if (hourNow == 19 && minuteNow == 0) {
    Serial.print("Now time: ");
    Serial.println(&timeinfo, "%H:%M:%S");
  }
}

void controlPH(float pH) {

  // ===== trigger เริ่ม =====
  if (autoMode && pH < PH_LOW_THRESHOLD && phState == IDLE) {
    phState = PUMP1_ON;
    phTimer = millis();

    pump1State = true;
    pump2State = false;

    Serial.println("pH LOW → Start Pump1");
  }

  // ===== Pump1 ทำงาน =====
  if (phState == PUMP1_ON) {
    if (millis() - phTimer >= 5000) {
      pump1State = false;
      pump2State = true;

      phState = PUMP2_ON;
      phTimer = millis();

      Serial.println("Switch to Pump2");
    }
  }

  // ===== Pump2 ทำงาน =====
  if (phState == PUMP2_ON) {
    if (millis() - phTimer >= 5000) {
      pump2State = false;

      phState = IDLE;

      Serial.println("pH Control Done");
    }
  }
}

void controlWaterLevel(int lowLevel, int highLevel) {

  if (!autoMode) return;

  // ===== น้ำต่ำ → เริ่มเติม =====
  if (lowLevel == HIGH && highLevel == HIGH) {
    pump2State = true;
    Serial.println("Water LOW → Start filling");
  }

  // ===== น้ำเต็ม → หยุด =====
  if (highLevel == LOW) {
    pump2State = false;
    Serial.println("Water FULL → Stop filling");
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  sensors.begin();

  pinMode(FLOAT_LOW, INPUT_PULLUP);
  pinMode(FLOAT_HIGH, INPUT_PULLUP);

  pinMode(LED, OUTPUT);
  pinMode(Pump1, OUTPUT);
  pinMode(Pump2, OUTPUT);
  pinMode(water_filter, OUTPUT);

  setup_wifi();

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  Serial.println("Waiting for NTP time...");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to get time...");
    delay(1000);
  }
  Serial.println("Time synced!");
}

// ================= LOOP =================
void loop() {

  if (!client.connected()) reconnect();
  client.loop();

  // ===== SENSOR =====
  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);

  int phRaw = analogRead(PH_PIN);
  float phVoltage = phRaw * (3.3 / 4095.0);
  float pH = 7 + ((2.5 - phVoltage) / 0.18);

  int clRaw = analogRead(CL_PIN);
  float clVoltage = clRaw * (3.3 / 4095.0);
  float chlorine = clVoltage * 2.0;

  int lowLevel  = digitalRead(FLOAT_LOW);
  int highLevel = digitalRead(FLOAT_HIGH);

  // ===== AUTO CONTROL =====
  autoControl(lowLevel, highLevel);

  // ===== APPLY RELAY =====
  applyRelay();

  // ===== SEND DATA =====
  sendData(tempC, pH, chlorine, lowLevel, highLevel);

  // ===== Conditions ======
  if (autoMode) {
    // 1. auto with check time
    checkTimeCondition();

    // 2. check pH 
    controlPH(pH);

    // 3. check water level
    controlWaterLevel(lowLevel, highLevel);

  }

  delay(2000);
}
