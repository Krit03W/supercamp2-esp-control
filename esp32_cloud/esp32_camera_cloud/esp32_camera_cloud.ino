/* ============================================================
   ESP32-CAM — โหมด CLOUD: push เฟรม JPEG ผ่าน WebSocket relay
   (AI-Thinker ESP32-CAM)

   ── ไลบรารี ── WebSockets by Markus Sattler
   ── บอร์ด ── "AI Thinker ESP32-CAM"
   ── อัปโหลด ── ต้องต่อ USB-TTL + ต่อ GPIO0->GND ตอนแฟลช
                แล้วถอด GPIO0 ออก กด reset ก่อนรัน

   ⚠️ ส่ง video ผ่าน cloud กินแรง/เน็ตเยอะ → ตั้งภาพเล็ก + จำกัด fps
   ============================================================ */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include "esp_camera.h"

/* ===================== WiFi ===================== */
const char* WIFI_SSID = "iPhone krit";
const char* WIFI_PASS = "0954312751";

/* ===================== Relay ===================== */
const char* RELAY_HOST = "your-domain.com";
const int   RELAY_PORT = 443;
const bool  RELAY_TLS  = true;
const char* ROOM       = "robot1";
const char* TOKEN      = "";

/* ===================== คุณภาพ/อัตราเฟรม ===================== */
const framesize_t FRAME_SIZE  = FRAMESIZE_QVGA;  // 320x240 (ลด/เพิ่มได้: CIF, VGA)
const int         JPEG_QUALITY = 14;             // 10(ดี/ใหญ่) .. 30(หยาบ/เล็ก)
const int         FPS_LIMIT    = 8;              // จำกัดเฟรม/วินาที กันเน็ตตัน

/* ===================== ขากล้อง AI-Thinker ===================== */
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

WebSocketsClient webSocket;
bool connected = false;
unsigned long lastFrame = 0;
const unsigned long frameInterval = 1000 / FPS_LIMIT;

void onWsEvent(WStype_t type, uint8_t* payload, size_t len) {
  if (type == WStype_CONNECTED)    { connected = true;  Serial.println("[relay] connected"); }
  else if (type == WStype_DISCONNECTED) { connected = false; Serial.println("[relay] disconnected"); }
}

bool initCamera() {
  camera_config_t c;
  c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM; c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAME_SIZE;
  c.jpeg_quality = JPEG_QUALITY;
  c.fb_count = psramFound() ? 2 : 1;
  c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  return esp_camera_init(&c) == ESP_OK;
}

void setup() {
  Serial.begin(115200); delay(300);

  if (!initCamera()) { Serial.println("camera init FAILED — เช็คสาย/รุ่นบอร์ด"); while (true) delay(1000); }
  Serial.println("camera OK");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  String path = String("/ws?room=") + ROOM + "&role=camera";
  if (strlen(TOKEN)) path += String("&token=") + TOKEN;

  if (RELAY_TLS) webSocket.beginSSL(RELAY_HOST, RELAY_PORT, path.c_str());
  else           webSocket.begin(RELAY_HOST, RELAY_PORT, path.c_str());
  webSocket.onEvent(onWsEvent);
  webSocket.setReconnectInterval(3000);
  webSocket.enableHeartbeat(15000, 3000, 2);
  Serial.println("กำลังต่อ relay: " + String(RELAY_HOST) + path);
}

void loop() {
  webSocket.loop();
  if (!connected) return;
  if (millis() - lastFrame < frameInterval) return;
  lastFrame = millis();

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { Serial.println("capture failed"); return; }
  webSocket.sendBIN(fb->buf, fb->len);     // ส่งเฟรม JPEG เป็น binary -> relay -> เว็บ
  esp_camera_fb_return(fb);
}
