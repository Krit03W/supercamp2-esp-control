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
const char* RELAY_HOST = "espcontroll.wijak.org";   // โดเมนที่ deploy (ไม่ต้องใส่ https://)
const int   RELAY_PORT = 443;                 // 443 = WSS
const bool  RELAY_TLS  = true;                // true = wss (ผ่าน Dokploy/HTTPS)
const char* ROOM       = "car1";              // ⚠️ คันที่ N ตั้งเป็น "carN" — แต่ละคันไม่ซ้ำกัน (car1..car7)
const char* TOKEN      = "AISuperCamp2";                  // ใส่ให้ตรงกับ AUTH_TOKEN ถ้าตั้งไว้

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

/* ตำแหน่ง servo สำหรับ macro "เก็บหิน" (ปรับองศาตามแขนจริง) */
const int ARM_DOWN  = 30,  ARM_UP    = 150;   // แขนลง / แขนยก
const int GRIP_OPEN = 180, GRIP_CLOSE = 0;    // มือเปิด / มือหุบ

/* ===================== ระบบ ===================== */
WebSocketsClient webSocket;
Servo sBase, sArm, sWrist, sGrip;
int aBase = 90, aArm = 90, aWrist = 90;
unsigned long lastCmd = 0;                     // กัน "หุ่นวิ่งหนี" ถ้าขาดการติดต่อ
const unsigned long CMD_TIMEOUT = 800;         // ms: ไม่มีคำสั่งนานเกินนี้ = หยุดทุกอย่าง (เว็บส่ง keepalive ทุก 250ms)
bool moving = false;

/* ===================== button state array (index ตรงกับเว็บ) ===================== */
enum {
  B_FWD, B_BACK, B_LEFT, B_RIGHT,        // 0-3 เคลื่อนที่
  B_BASE_M, B_BASE_P,                    // 4,5  ฐานหมุน
  B_ARM_M,  B_ARM_P,                     // 6,7  แขน
  B_WRIST_M, B_WRIST_P,                  // 8,9  ข้อมือ
  B_GRIP_O, B_GRIP_C,                    // 10,11 เปิด/หุบมือ (edge)
  B_LIGHT,                               // 12   ไฟ (level)
  B_COLLECT,                             // 13   เก็บหิน (edge)
  N_BTN
};
uint8_t btn[N_BTN]     = {0};            // สถานะปัจจุบัน
uint8_t prevBtn[N_BTN] = {0};            // สถานะก่อนหน้า (ใช้จับ edge)
unsigned long lastStep = 0;              // จับเวลาเดิน servo ทีละ step

/* ===================== มอเตอร์ ===================== */
void driveMotors(int l, int r) {
  digitalWrite(IN1, l > 0); digitalWrite(IN2, l < 0);
  digitalWrite(IN3, r > 0); digitalWrite(IN4, r < 0);
  analogWrite(ENA, l != 0 ? motorSpeed : 0);
  analogWrite(ENB, r != 0 ? motorSpeed : 0);
  moving = (l != 0 || r != 0);
}

// ขับมอเตอร์ฝั่งเดียวด้วยค่า -1..1 (ทิศจากเครื่องหมาย, ความเร็วจากขนาด)
void setSide(int pinA, int pinB, int pinEN, float v) {
  digitalWrite(pinA, v > 0);
  digitalWrite(pinB, v < 0);
  analogWrite(pinEN, (int)(fabs(v) * motorSpeed));
}

// ขับแบบรวม throttle (เดินหน้า/ถอย) + steer (ซ้าย/ขวา) -> ผสมเป็นล้อซ้าย/ขวา
// กดหน้า+ซ้ายพร้อมกัน = เลี้ยวขณะเดินได้ realtime
void handleDrive(float t, float s) {
  lastCmd = millis();
  float l = t + s;     // ล้อซ้าย
  float r = t - s;     // ล้อขวา
  float m = max(fabs(l), fabs(r));
  if (m > 1.0f) { l /= m; r /= m; }   // normalize ไม่ให้เกิน 1
  setSide(IN1, IN2, ENA, l);
  setSide(IN3, IN4, ENB, r);
  moving = (t != 0 || s != 0);
}

// macro เก็บหิน: เปิดมือ -> ลงแขน -> หุบมือ(จับ) -> ยกแขน
void collectRock() {
  Serial.println("COLLECT rock");
  driveMotors(0, 0);                                    // หยุดล้อก่อน
  sGrip.write(GRIP_OPEN);            delay(400);
  aArm = ARM_DOWN;  sArm.write(aArm); delay(700);
  sGrip.write(GRIP_CLOSE);           delay(600);
  aArm = ARM_UP;    sArm.write(aArm); delay(700);
}

void stepServo(Servo& sv, int& ang, int delta) { ang = constrain(ang + delta, 0, 180); sv.write(ang); }

// หยุดทุกอย่าง (เรียกตอน timeout / หลุดการเชื่อมต่อ)
void stopAll() {
  for (int i = 0; i < N_BTN; i++) { btn[i] = 0; prevBtn[i] = 0; }
  driveMotors(0, 0);
  digitalWrite(LIGHT_PIN, LOW);
  moving = false;
}

// print สถานะปุ่มเป็น list 14 ค่า เช่น {0,0,0,1,0,1,1,0,1,1,1,0,1,0}
void printButtons() {
  Serial.print("BTN {");
  for (int i = 0; i < N_BTN; i++) { Serial.print(btn[i]); if (i < N_BTN - 1) Serial.print(","); }
  Serial.println("}");
}

// อ่าน button array แล้วสั่งงานทั้งหมดในครั้งเดียว (realtime)
void applyButtons() {
  lastCmd = millis();
  if (memcmp(btn, prevBtn, sizeof(btn)) != 0) printButtons();   // print เฉพาะตอนเปลี่ยน
  // เคลื่อนที่ (รวมทิศ) — กดหน้า+ซ้ายพร้อมกัน = เลี้ยวขณะเดิน
  float t = (btn[B_FWD]   ? 1.0f : 0) - (btn[B_BACK] ? 1.0f : 0);
  float s = (btn[B_RIGHT] ? 1.0f : 0) - (btn[B_LEFT] ? 1.0f : 0);
  handleDrive(t, s);
  // ไฟ (level: 1=เปิด)
  digitalWrite(LIGHT_PIN, btn[B_LIGHT] ? HIGH : LOW);
  // edge actions (ทำครั้งเดียวตอนเพิ่งกด)
  if (btn[B_GRIP_O]   && !prevBtn[B_GRIP_O])   sGrip.write(GRIP_OPEN);
  if (btn[B_GRIP_C]   && !prevBtn[B_GRIP_C])   sGrip.write(GRIP_CLOSE);
  if (btn[B_COLLECT]  && !prevBtn[B_COLLECT])  collectRock();
  memcpy(prevBtn, btn, sizeof(btn));
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
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, msg)) { Serial.println("JSON err: " + msg); return; }

  // ── รูปแบบใหม่: button-state array เดียว {"b":[0,1,0,...]} ──
  if (doc["b"].is<JsonArray>()) {
    JsonArray b = doc["b"];
    for (int i = 0; i < N_BTN; i++) btn[i] = (i < (int)b.size()) ? (uint8_t)(int)b[i] : 0;
    applyButtons();
    return;
  }

  // ── รูปแบบเดิม (เผื่อ backward-compat) ──
  const char* cmd = doc["cmd"]; if (!cmd) return;
  String value = doc["value"] | "";
  if      (strcmp(cmd, "drive") == 0)       handleDrive(doc["t"] | 0.0f, doc["s"] | 0.0f);
  else if (strcmp(cmd, "move") == 0)       handleMove(value);
  else if (strcmp(cmd, "collect") == 0)    collectRock();
  else if (strcmp(cmd, "stop") == 0)       driveMotors(0, 0);
  else if (strcmp(cmd, "light") == 0)    { digitalWrite(LIGHT_PIN, value == "on"); }
  else if (strcmp(cmd, "grip_open") == 0)  sGrip.write(180);
  else if (strcmp(cmd, "grip_close") == 0) sGrip.write(0);
}

/* ===================== WebSocket event ===================== */
void onWsEvent(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:    Serial.println("[relay] connected"); break;
    case WStype_DISCONNECTED: Serial.println("[relay] disconnected -> หยุดทุกอย่าง"); stopAll(); break;
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

  // เดิน servo ทีละ step ขณะกดค้าง (ไม่ขึ้นกับอัตราที่เว็บส่งมา)
  if (millis() - lastStep > 60) {
    lastStep = millis();
    if (btn[B_BASE_P])  stepServo(sBase,  aBase,  +SERVO_STEP);
    if (btn[B_BASE_M])  stepServo(sBase,  aBase,  -SERVO_STEP);
    if (btn[B_ARM_P])   stepServo(sArm,   aArm,   +SERVO_STEP);
    if (btn[B_ARM_M])   stepServo(sArm,   aArm,   -SERVO_STEP);
    if (btn[B_WRIST_P]) stepServo(sWrist, aWrist, +SERVO_STEP);
    if (btn[B_WRIST_M]) stepServo(sWrist, aWrist, -SERVO_STEP);
  }

  // safety: ขาดคำสั่งนานเกินไป (สัญญาณหาย) -> หยุดทุกอย่าง
  bool anyActive = false;
  for (int i = 0; i < N_BTN; i++) if (btn[i]) { anyActive = true; break; }
  if (anyActive && millis() - lastCmd > CMD_TIMEOUT) { Serial.println("timeout -> หยุด"); stopAll(); }
}
