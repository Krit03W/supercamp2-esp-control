/* ============================================================
   ESP32 Robot — WebSocket Relay + Static Host
   รันเป็น container เดียวบน Dokploy: host หน้าเว็บ + ส่งต่อข้อความ

   หลักการ:
     - ทุกฝ่าย "วิ่งออกมาต่อ" relay นี้เอง (ไม่ต้อง port forward)
     - แบ่งเป็น room (เช่น "robot1") และ role:
         controller = หน้าเว็บที่คุม
         robot      = ESP32 ตัวคุมมอเตอร์/servo
         camera     = ESP32-CAM
     - เส้นทางส่งต่อ:
         controller --(JSON คำสั่ง)--> robot
         robot      --(JSON telemetry)--> controller
         camera     --(binary JPEG)--> controller

   ENV:
     PORT        (default 8080)
     AUTH_TOKEN  (ถ้าตั้ง ทุกฝ่ายต้องส่ง ?token=... ให้ตรง)
   ============================================================ */

const http = require('http');
const fs = require('fs');
const path = require('path');
const url = require('url');
const os = require('os');
const { spawn } = require('child_process');
const { WebSocketServer } = require('ws');

function parseEnvLine(line) {
  const trimmed = line.trim();
  if (!trimmed || trimmed.startsWith('#') || !trimmed.includes('=')) return null;
  const i = trimmed.indexOf('=');
  const key = trimmed.slice(0, i).trim();
  let value = trimmed.slice(i + 1).trim();
  value = value.replace(/^["']|["']$/g, '');
  return key ? [key, value] : null;
}

function loadEnvFile(file) {
  if (!fs.existsSync(file)) return;
  const text = fs.readFileSync(file, 'utf8');
  for (const line of text.split(/\r?\n/)) {
    const parsed = parseEnvLine(line);
    if (!parsed) continue;
    const [key, value] = parsed;
    if (process.env[key] === undefined) process.env[key] = value;
  }
}

loadEnvFile(path.join(__dirname, '..', '.env'));
loadEnvFile(path.join(__dirname, '.env'));

const PORT = process.env.PORT || 8080;
const TOKEN = process.env.AUTH_TOKEN || '';
const PUBLIC = path.join(__dirname, 'public');
const PYTHON_BIN = process.env.PYTHON_BIN || (process.platform === 'win32' ? 'python' : 'python3');
const SCAN_RUNE = path.join(__dirname, 'scan_rune.py');
const KRIT_BODY_LIMIT = 8 * 1024 * 1024;

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript',
  '.css': 'text/css',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
};

function sendJson(res, code, obj) {
  res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8' });
  res.end(JSON.stringify(obj));
}

function readJsonBody(req, limitBytes) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let total = 0;
    let done = false;
    req.on('data', chunk => {
      if (done) return;
      total += chunk.length;
      if (total > limitBytes) {
        done = true;
        const err = new Error('Request body is too large.');
        err.statusCode = 413;
        reject(err);
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => {
      if (done) return;
      try {
        resolve(JSON.parse(Buffer.concat(chunks).toString('utf8') || '{}'));
      } catch (e) {
        e.statusCode = 400;
        reject(e);
      }
    });
    req.on('error', err => {
      if (!done) reject(err);
    });
  });
}

function parseImageData(imageData) {
  if (typeof imageData !== 'string' || !imageData.trim()) {
    const err = new Error('No image was provided.');
    err.statusCode = 400;
    throw err;
  }

  const m = imageData.match(/^data:(image\/(?:jpeg|jpg|png|webp));base64,([A-Za-z0-9+/=]+)$/i);
  const mime = m ? m[1].toLowerCase() : 'image/jpeg';
  const raw = m ? m[2] : imageData.trim();
  const ext = mime.includes('png') ? '.png' : mime.includes('webp') ? '.webp' : '.jpg';
  const buffer = Buffer.from(raw, 'base64');
  if (!buffer.length) {
    const err = new Error('Image data was empty.');
    err.statusCode = 400;
    throw err;
  }
  return { buffer, ext };
}

function runRuneScanner(imagePath, question) {
  return new Promise((resolve, reject) => {
    const args = [SCAN_RUNE, imagePath, '--json'];
    if (question) args.push('--question', question);

    const child = spawn(PYTHON_BIN, args, {
      env: process.env,
      windowsHide: true,
    });
    let stdout = '';
    let stderr = '';
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error('Krit AI timed out while reading the rune.'));
    }, 30000);

    child.stdout.on('data', data => { stdout += data.toString(); });
    child.stderr.on('data', data => { stderr += data.toString(); });
    child.on('error', err => {
      clearTimeout(timer);
      reject(err);
    });
    child.on('close', code => {
      clearTimeout(timer);
      const text = stdout.trim().split(/\r?\n/).pop() || '';
      try {
        const parsed = JSON.parse(text);
        if (code !== 0 && parsed.ok === undefined) parsed.ok = false;
        resolve(parsed);
      } catch (e) {
        reject(new Error(stderr.trim() || stdout.trim() || `Scanner exited with code ${code}`));
      }
    });
  });
}

async function handleKritAi(req, res) {
  let tempDir = null;
  try {
    const body = await readJsonBody(req, KRIT_BODY_LIMIT);
    const { buffer, ext } = parseImageData(body.imageData);
    const question = typeof body.question === 'string' ? body.question.slice(0, 500) : '';

    tempDir = await fs.promises.mkdtemp(path.join(os.tmpdir(), 'krit-ai-'));
    const imagePath = path.join(tempDir, `rune${ext}`);
    await fs.promises.writeFile(imagePath, buffer);

    const result = await runRuneScanner(imagePath, question);
    sendJson(res, result.ok ? 200 : 422, result);
  } catch (err) {
    sendJson(res, err.statusCode || 500, {
      ok: false,
      method: 'server',
      status: err.message || 'Krit AI failed.',
    });
  } finally {
    if (tempDir) fs.promises.rm(tempDir, { recursive: true, force: true }).catch(() => {});
  }
}

/* ---------- static host ---------- */
const server = http.createServer(async (req, res) => {
  let p = url.parse(req.url).pathname;
  if (p === '/health') { res.writeHead(200); return res.end('ok'); }
  if (req.method === 'POST' && p === '/api/krit-ai') return handleKritAi(req, res);
  if (p === '/') p = '/index.html';

  const file = path.join(PUBLIC, path.normalize(p));
  if (!file.startsWith(PUBLIC)) { res.writeHead(403); return res.end('forbidden'); }

  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404); return res.end('not found'); }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
    res.end(data);
  });
});

/* ---------- WebSocket relay ที่ /ws ---------- */
const wss = new WebSocketServer({ server, path: '/ws' });

// rooms: Map<roomName, Set<ws>>
const rooms = new Map();

function peers(room) { return rooms.get(room) || new Set(); }

// ส่งต่อ data ไปยังทุก connection ใน room ที่มี role = toRole (ยกเว้นผู้ส่งเอง)
function forward(room, toRole, data, isBinary, from) {
  for (const c of peers(room)) {
    if (c !== from && c.role === toRole && c.readyState === 1) {
      c.send(data, { binary: isBinary });
    }
  }
}
// ส่งข้อความระบบ (text) ให้ role ที่ระบุ
function notify(room, toRole, obj) {
  const text = JSON.stringify(obj);
  for (const c of peers(room)) {
    if (c.role === toRole && c.readyState === 1) c.send(text);
  }
}

// ห้องที่มี "คนขับ" (controller ที่เป็น driver) อยู่ = คันที่ถูกล็อก
function occupiedRooms() {
  const out = [];
  for (const [room, set] of rooms) {
    if ([...set].some(c => c.role === 'controller' && c.isDriver)) out.push(room);
  }
  return out;
}

// ส่งรายการคันที่ถูกล็อกให้ controller ทุกคน (ทุก room) เพื่ออัปเดตแถบเลือกคัน
function broadcastCars() {
  const msg = JSON.stringify({ sys: 'cars', occupied: occupiedRooms() });
  for (const set of rooms.values())
    for (const c of set)
      if (c.role === 'controller' && c.readyState === 1) c.send(msg);
}

wss.on('connection', (ws, req) => {
  const q = url.parse(req.url, true).query;
  const room = String(q.room || 'default');
  const role = String(q.role || 'controller');

  if (TOKEN && q.token !== TOKEN) { ws.close(4001, 'bad token'); return; }
  if (!['controller', 'robot', 'camera'].includes(role)) { ws.close(4002, 'bad role'); return; }

  ws.room = room;
  ws.role = role;
  ws.isAlive = true;
  if (!rooms.has(room)) rooms.set(room, new Set());
  rooms.get(room).add(ws);
  console.log(`+ ${role} -> room "${room}" (${peers(room).size} peers)`);

  if (role === 'controller') {
    // ล็อกตัวแรก: ถ้ายังไม่มีคนขับใน room นี้ -> เป็นคนขับ, ไม่งั้นเป็นผู้ชม
    const hasDriver = [...peers(room)].some(c => c !== ws && c.role === 'controller' && c.isDriver);
    ws.isDriver = !hasDriver;
    ws.send(JSON.stringify({ sys: 'role', room, driver: ws.isDriver }));
    ws.send(JSON.stringify({ sys: 'cars', occupied: occupiedRooms() }));
    if (ws.isDriver) broadcastCars();      // มีคนขับใหม่ -> แจ้งทุกคน
  } else {
    // บอก controller ว่ามี robot/camera ออนไลน์แล้ว
    notify(room, 'controller', { sys: 'status', role, online: true });
  }

  ws.on('pong', () => { ws.isAlive = true; });

  ws.on('message', (data, isBinary) => {
    switch (role) {
      case 'controller': if (ws.isDriver) forward(room, 'robot', data, isBinary, ws); break; // เฉพาะคนขับ -> หุ่น
      case 'robot':      forward(room, 'controller', data, isBinary, ws); break; // telemetry -> เว็บ
      case 'camera':     forward(room, 'controller', data, isBinary, ws); break; // เฟรม -> เว็บ (ผู้ชมก็เห็น)
    }
  });

  ws.on('close', () => {
    const wasDriver = role === 'controller' && ws.isDriver;
    peers(room).delete(ws);
    if (peers(room).size === 0) rooms.delete(room);
    if (role !== 'controller') notify(room, 'controller', { sys: 'status', role, online: false });

    if (wasDriver) {
      // คนขับออก -> เลื่อนผู้ชมคนถัดไปขึ้นเป็นคนขับ
      const next = [...peers(room)].find(c => c.role === 'controller');
      if (next) { next.isDriver = true; next.send(JSON.stringify({ sys: 'role', room, driver: true })); }
      broadcastCars();
    }
    console.log(`- ${role} left room "${room}"`);
  });

  ws.on('error', () => {});
});

/* ---------- heartbeat กัน proxy ตัดการเชื่อมต่อตอน idle ---------- */
setInterval(() => {
  for (const set of rooms.values()) {
    for (const ws of set) {
      if (ws.isAlive === false) { ws.terminate(); continue; }
      ws.isAlive = false;
      try { ws.ping(); } catch (e) {}
    }
  }
}, 25000);

server.listen(PORT, () => console.log(`ESP32 relay listening on :${PORT} (token ${TOKEN ? 'ON' : 'OFF'})`));
