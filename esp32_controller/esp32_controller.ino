/* ============================================================
   ESP32 Robot Controller  —  WebSocket + JSON
   ตรงกับโปรโตคอลของหน้าเว็บ index.html

   ── ไลบรารีที่ต้องติดตั้ง (Library Manager) ──
     1) WebSockets         by Markus Sattler   (ชื่อ "WebSockets")
     2) ArduinoJson        by Benoit Blanchon   (v6 ขึ้นไป)
     3) ESP32Servo         by Kevin Harrington  (ถ้าใช้ servo gripper)

   ── บอร์ด ──  เลือก "ESP32 Dev Module"
   ── หน้าเว็บต่อมาที่ ──  ws://<ip ของ esp32>:81
   ============================================================ */

#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

/* ===================== 1) ตั้งค่า WiFi ===================== */
// --- โหมด AP: ESP32 ปล่อย WiFi เอง (default, ตรงกับ 192.168.4.1 ในหน้าเว็บ) ---
#define USE_AP_MODE   false
const char* AP_SSID = "ESP32-Robot";
const char* AP_PASS = "12345678";       // อย่างน้อย 8 ตัว

// --- โหมด STA: ต่อเข้า router บ้าน (ถ้าตั้ง USE_AP_MODE = false) ---
const char* STA_SSID = "your-wifi";
const char* STA_PASS = "your-password";

/* ===================== 2) ตั้งค่าขา (แก้ตามการต่อจริง) ===================== */
// มอเตอร์ขับเคลื่อน — ตัวอย่างใช้ L298N (2 มอเตอร์ ซ้าย/ขวา)
#define ENA  13   // PWM ความเร็วล้อซ้าย
#define IN1  12
#define IN2  14
#define ENB  27   // PWM ความเร็วล้อขวา
#define IN3  26
#define IN4  25

#define LIGHT_PIN  2    // ไฟ (LED บนบอร์ดมักเป็นขา 2)

// Servo gripper 3 แกน + มือจับ
#define SERVO_BASE   18
#define SERVO_ARM    19
#define SERVO_WRIST  21
#define SERVO_GRIP   22

int motorSpeed = 200;          // 0-255
const int SERVO_STEP = 5;      // องศาที่ขยับต่อการกด 1 ครั้ง

/* ===================== ตัวแปรระบบ ===================== */
WebSocketsServer webSocket(81);
Servo sBase, sArm, sWrist, sGrip;
int aBase = 90, aArm = 90, aWrist = 90;   // มุมเริ่มต้นของ servo

/* ===================== ฟังก์ชันมอเตอร์ ===================== */
void driveMotors(int leftDir, int rightDir) {   // -1 ถอย, 0 หยุด, 1 หน้า
  digitalWrite(IN1, leftDir > 0);
  digitalWrite(IN2, leftDir < 0);
  digitalWrite(IN3, rightDir > 0);
  digitalWrite(IN4, rightDir < 0);
  analogWrite(ENA, leftDir  != 0 ? motorSpeed : 0);
  analogWrite(ENB, rightDir != 0 ? motorSpeed : 0);
}

void handleMove(const String& v) {
  Serial.println("MOVE -> " + v);
  if      (v == "forward")  driveMotors( 1,  1);
  else if (v == "backward") driveMotors(-1, -1);
  else if (v == "left")     driveMotors(-1,  1);
  else if (v == "right")    driveMotors( 1, -1);
  else if (v == "stop")     driveMotors( 0,  0);

  // --- gripper 3 แกน (กดค้าง = ขยับทีละ step ตอนเริ่มกด) ---
  else if (v == "base_plus")   { aBase  = constrain(aBase  + SERVO_STEP, 0, 180); sBase.write(aBase);   Serial.println("base="  + String(aBase)); }
  else if (v == "base_minus")  { aBase  = constrain(aBase  - SERVO_STEP, 0, 180); sBase.write(aBase);   Serial.println("base="  + String(aBase)); }
  else if (v == "arm_plus")    { aArm   = constrain(aArm   + SERVO_STEP, 0, 180); sArm.write(aArm);     Serial.println("arm="   + String(aArm)); }
  else if (v == "arm_minus")   { aArm   = constrain(aArm   - SERVO_STEP, 0, 180); sArm.write(aArm);     Serial.println("arm="   + String(aArm)); }
  else if (v == "wrist_plus")  { aWrist = constrain(aWrist + SERVO_STEP, 0, 180); sWrist.write(aWrist); Serial.println("wrist=" + String(aWrist)); }
  else if (v == "wrist_minus") { aWrist = constrain(aWrist - SERVO_STEP, 0, 180); sWrist.write(aWrist); Serial.println("wrist=" + String(aWrist)); }
}

/* ===================== รับคำสั่งจาก WebSocket ===================== */
void handleCommand(const String& msg) {
  StaticJsonDocument<200> doc;
  if (deserializeJson(doc, msg)) { Serial.println("JSON error: " + msg); return; }

  const char* cmd = doc["cmd"];
  String value    = doc["value"] | "";

  if (strcmp(cmd, "move") == 0)            handleMove(value);
  else if (strcmp(cmd, "stop") == 0)       driveMotors(0, 0);
  else if (strcmp(cmd, "light") == 0)    { digitalWrite(LIGHT_PIN, value == "on"); Serial.println("LIGHT -> " + value); }
  else if (strcmp(cmd, "grip_open") == 0)  { sGrip.write(180); Serial.println("GRIP -> open"); }
  else if (strcmp(cmd, "grip_close") == 0) { sGrip.write(0);   Serial.println("GRIP -> close"); }
  else Serial.println("unknown cmd: " + String(cmd));
}

void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:    Serial.printf("[%u] client connected\n", num); break;
    case WStype_DISCONNECTED: Serial.printf("[%u] disconnected -> หยุดมอเตอร์\n", num); driveMotors(0, 0); break;
    case WStype_TEXT:         handleCommand(String((char*)payload).substring(0, len)); break;
    default: break;
  }
}

/* ===================== setup / loop ===================== */
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT); pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  pinMode(ENA, OUTPUT); pinMode(ENB, OUTPUT); pinMode(LIGHT_PIN, OUTPUT);
  driveMotors(0, 0);

  sBase.attach(SERVO_BASE);   sBase.write(aBase);
  sArm.attach(SERVO_ARM);     sArm.write(aArm);
  sWrist.attach(SERVO_WRIST); sWrist.write(aWrist);
  sGrip.attach(SERVO_GRIP);   sGrip.write(90);

  if (USE_AP_MODE) {
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.println("\n=== AP MODE ===");
    Serial.print("SSID: "); Serial.println(AP_SSID);
    Serial.print("เชื่อมต่อแล้วใส่ในหน้าเว็บ: ws://");
    Serial.print(WiFi.softAPIP()); Serial.println(":81");
  } else {
    WiFi.begin(STA_SSID, STA_PASS);
    Serial.print("กำลังต่อ WiFi");
    while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
    Serial.println("\n=== STA MODE ===");
    Serial.print("ใส่ในหน้าเว็บ: ws://");
    Serial.print(WiFi.localIP()); Serial.println(":81");
  }

  webSocket.begin();
  webSocket.onEvent(onWsEvent);
  Serial.println("WebSocket server เริ่มทำงานที่พอร์ต 81");
}

void loop() {
  webSocket.loop();
}
