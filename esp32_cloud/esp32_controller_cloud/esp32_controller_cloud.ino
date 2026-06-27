/* ============================================================
   ESP32 Robot Controller — โหมด CLOUD (WebSocket CLIENT)
   วิ่งออกไปต่อ relay บน Dokploy เอง → คุมจากที่ไหนก็ได้

   ── ไลบรารี ──
     1) WebSockets   by Markus Sattler
     2) ArduinoJson  by Benoit Blanchon (v6+)
     3) ESP32Servo   by Kevin Harrington
   ── บอร์ด ── ESP32 Dev Module
   ============================================================ */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

/* ===================== 1) WiFi ===================== */
const char* WIFI_SSID = "iPhone krit";
const char* WIFI_PASS = "0954312751";

/* ===================== 2) Relay (Dokploy) ===================== */
const char* RELAY_HOST = "your-domain.com";   // โดเมนที่ deploy (ไม่ต้องใส่ https://)
const int   RELAY_PORT = 443;                 // 443 = WSS
const bool  RELAY_TLS  = true;                // true = wss (ผ่าน Dokploy/HTTPS)
const char* ROOM       = "robot1";
const char* TOKEN      = "";                  // ใส่ให้ตรงกับ AUTH_TOKEN ถ้าตั้งไว้

/* ===================== 3) ขา (แก้ตามการต่อจริง) ===================== */
#define ENA 13
#define IN1 12
#define IN2 14
#define ENB 27
#define IN3 26
#define IN4 25
#define LIGHT_PIN 2
#define SERVO_BASE  18
#define SERVO_ARM   19
#define SERVO_WRIST 21
#define SERVO_GRIP  22

int motorSpeed = 200;
const int SERVO_STEP = 5;

/* ===================== ระบบ ===================== */
WebSocketsClient webSocket;
Servo sBase, sArm, sWrist, sGrip;
int aBase = 90, aArm = 90, aWrist = 90;
unsigned long lastCmd = 0;                     // กัน "หุ่นวิ่งหนี" ถ้าขาดการติดต่อ
const unsigned long CMD_TIMEOUT = 1500;        // ms: ไม่มีคำสั่งนานเกินนี้ขณะเดิน = หยุด
bool moving = false;

/* ===================== มอเตอร์ ===================== */
void driveMotors(int l, int r) {
  digitalWrite(IN1, l > 0); digitalWrite(IN2, l < 0);
  digitalWrite(IN3, r > 0); digitalWrite(IN4, r < 0);
  analogWrite(ENA, l != 0 ? motorSpeed : 0);
  analogWrite(ENB, r != 0 ? motorSpeed : 0);
  moving = (l != 0 || r != 0);
}

void handleMove(const String& v) {
  lastCmd = millis();
  Serial.println("MOVE -> " + v);
  if      (v == "forward")  driveMotors( 1,  1);
  else if (v == "backward") driveMotors(-1, -1);
  else if (v == "left")     driveMotors(-1,  1);
  else if (v == "right")    driveMotors( 1, -1);
  else if (v == "stop")     driveMotors( 0,  0);
  else if (v == "base_plus")   { aBase  = constrain(aBase  + SERVO_STEP, 0, 180); sBase.write(aBase); }
  else if (v == "base_minus")  { aBase  = constrain(aBase  - SERVO_STEP, 0, 180); sBase.write(aBase); }
  else if (v == "arm_plus")    { aArm   = constrain(aArm   + SERVO_STEP, 0, 180); sArm.write(aArm); }
  else if (v == "arm_minus")   { aArm   = constrain(aArm   - SERVO_STEP, 0, 180); sArm.write(aArm); }
  else if (v == "wrist_plus")  { aWrist = constrain(aWrist + SERVO_STEP, 0, 180); sWrist.write(aWrist); }
  else if (v == "wrist_minus") { aWrist = constrain(aWrist - SERVO_STEP, 0, 180); sWrist.write(aWrist); }
}

void handleCommand(const String& msg) {
  StaticJsonDocument<200> doc;
  if (deserializeJson(doc, msg)) { Serial.println("JSON err: " + msg); return; }
  const char* cmd = doc["cmd"]; if (!cmd) return;
  String value = doc["value"] | "";

  if      (strcmp(cmd, "move") == 0)       handleMove(value);
  else if (strcmp(cmd, "stop") == 0)       driveMotors(0, 0);
  else if (strcmp(cmd, "light") == 0)    { digitalWrite(LIGHT_PIN, value == "on"); Serial.println("LIGHT -> " + value); }
  else if (strcmp(cmd, "grip_open") == 0)  { sGrip.write(180); Serial.println("GRIP -> open"); }
  else if (strcmp(cmd, "grip_close") == 0) { sGrip.write(0);   Serial.println("GRIP -> close"); }
}

/* ===================== WebSocket event ===================== */
void onWsEvent(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:    Serial.println("[relay] connected"); break;
    case WStype_DISCONNECTED: Serial.println("[relay] disconnected -> หยุดมอเตอร์"); driveMotors(0, 0); break;
    case WStype_TEXT:         handleCommand(String((char*)payload).substring(0, len)); break;
    default: break;
  }
}

/* ===================== setup / loop ===================== */
void setup() {
  Serial.begin(115200); delay(300);

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT); pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  pinMode(ENA, OUTPUT); pinMode(ENB, OUTPUT); pinMode(LIGHT_PIN, OUTPUT);
  driveMotors(0, 0);
  sBase.attach(SERVO_BASE); sBase.write(aBase);
  sArm.attach(SERVO_ARM);   sArm.write(aArm);
  sWrist.attach(SERVO_WRIST); sWrist.write(aWrist);
  sGrip.attach(SERVO_GRIP); sGrip.write(90);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  // path: /ws?room=...&role=robot&token=...
  String path = String("/ws?room=") + ROOM + "&role=robot";
  if (strlen(TOKEN)) path += String("&token=") + TOKEN;

  if (RELAY_TLS) webSocket.beginSSL(RELAY_HOST, RELAY_PORT, path.c_str());
  else           webSocket.begin(RELAY_HOST, RELAY_PORT, path.c_str());

  webSocket.onEvent([](WStype_t t, uint8_t* p, size_t l) { onWsEvent(t, p, l); });
  webSocket.setReconnectInterval(3000);              // ต่อใหม่อัตโนมัติถ้าหลุด
  webSocket.enableHeartbeat(15000, 3000, 2);         // ping กันโดน proxy ตัด
  Serial.println("กำลังต่อ relay: " + String(RELAY_HOST) + path);
}

void loop() {
  webSocket.loop();
  // ตัดมอเตอร์อัตโนมัติถ้าขาดคำสั่งนานเกินไปขณะกำลังเดิน (กันสัญญาณหาย)
  if (moving && millis() - lastCmd > CMD_TIMEOUT) { Serial.println("timeout -> หยุด"); driveMotors(0, 0); }
}
