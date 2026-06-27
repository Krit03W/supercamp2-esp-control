# ESP32 Robot — Cloud Relay

Relay + เว็บคุมหุ่น รันเป็น container เดียว: host หน้าเว็บ + ส่งต่อข้อความ/เฟรมกล้องระหว่างเว็บกับ ESP32

```
[เว็บ controller] ──WSS──┐
                         ├──> [Relay (ตัวนี้)] <──วิ่งออกมาต่อ── [ESP32 robot]
[ESP32-CAM camera] ──────┘                                     [ESP32-CAM]
```

ทุกฝ่ายต่อมาที่ `/ws?room=<ห้อง>&role=<controller|robot|camera>` (ถ้าตั้ง token ก็ใส่ `&token=...`)

## รันในเครื่อง (ทดสอบ)
```bash
cd server
npm install
PORT=8080 node server.js
# เปิด http://localhost:8080
```

## Deploy บน Dokploy
1. push โค้ดนี้ขึ้น Git repo
2. Dokploy → Create **Application** → ชี้มาที่ repo, **Build Path / Context = `/server`**
3. Build type: **Dockerfile** (มี Dockerfile ในโฟลเดอร์นี้แล้ว)
4. **Environment** (ถ้าต้องการ token กันคนอื่นแอบคุม):
   - `AUTH_TOKEN=ตั้งรหัสลับ`
   - `PORT=8080` (Dokploy map ให้เอง)
5. **Domains**: ผูกโดเมน + เปิด **HTTPS (Let's Encrypt)** → Traefik จะให้ `wss://` อัตโนมัติ
6. Deploy เสร็จ เปิด `https://<โดเมน>` ได้หน้าเว็บคุมเลย

## ตั้งค่าฝั่ง ESP32
ในไฟล์ `.ino` ทั้งสอง แก้:
```cpp
const char* RELAY_HOST = "<โดเมนของคุณ>";   // ไม่ต้องใส่ https://
const int   RELAY_PORT = 443;
const bool  RELAY_TLS  = true;
const char* ROOM       = "robot1";          // ต้องตรงกับ room ในหน้าเว็บ
const char* TOKEN      = "<ถ้าตั้ง AUTH_TOKEN>";
```

## หมายเหตุ
- **กล้องผ่าน cloud**: ESP32-CAM push เฟรม JPEG (QVGA ~8fps) ปรับได้ที่ `FRAME_SIZE`/`JPEG_QUALITY`/`FPS_LIMIT`
- `room` ใช้แยกหุ่นแต่ละตัว — ตั้งให้ตรงกันทั้ง 3 ฝ่าย (เว็บ/robot/camera)
- ความปลอดภัย: ตั้ง `AUTH_TOKEN` เสมอเมื่อ deploy public ไม่งั้นใครเดา URL ได้ก็คุมหุ่นได้
