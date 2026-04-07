/* eslint-disable no-console */
/*
本文件结构（Web sidecar，负责“监控大屏 + 网页发包/收包”）：
- 环境变量与端口：
  - WEB_HOST/WEB_PORT：sidecar HTTP 监听
  - FWD_UP_* / FWD_DOWN_* / FWD_ADMIN_*：转发器 upstream/downstream/admin 目标地址
- upstream（调试发包）：
  - ensureUpstreamConnected()：维护一条到 forwarder upstream 的长期 TCP 连接（会占用“唯一上游槽位”）
  - /api/upstream/send：将网页输入打包为协议帧并写入 upstream
- downstream（收包展示）：
  - connectDownstream()/downstreamPool：sidecar 主动创建若干条下游 TCP 连接用于接收广播数据
  - parseFramesFromSocket()：按 Header(40,v2)+Body 解析帧，body 以 base64 形式给前端
  - /api/downstream/stream：SSE 推送帧到浏览器（可按 conn_id 过滤）
- admin 代理：
  - /api/stats、/api/events：从 forwarder 的 admin 端口拉取并转发给前端页面
*/
const express = require('express');
const fetch = require('node-fetch');
const net = require('net');
const path = require('path');
const protobuf = require('protobufjs');

const MAGIC = 0x44574641;
const VERSION = 2;
const HEADER_LEN = 40;

function parseU64Json(v, def = '0') {
  if (v == null || v === '') return def;
  try {
    return BigInt(String(v)).toString();
  } catch {
    return def;
  }
}

function envInt(name, def) {
  const v = process.env[name];
  if (!v) return def;
  const n = parseInt(v, 10);
  return Number.isFinite(n) ? n : def;
}

const HTTP_HOST = process.env.WEB_HOST || '0.0.0.0';
const HTTP_PORT = envInt('WEB_PORT', 8080);

const FWD_ADMIN_HOST = process.env.FWD_ADMIN_HOST || '127.0.0.1';
const FWD_ADMIN_PORT = envInt('FWD_ADMIN_PORT', 19003);
const FWD_UP_HOST = process.env.FWD_UP_HOST || '127.0.0.1';
const FWD_UP_PORT = envInt('FWD_UP_PORT', 19001);
const FWD_DOWN_HOST = process.env.FWD_DOWN_HOST || '127.0.0.1';
const FWD_DOWN_PORT = envInt('FWD_DOWN_PORT', 19002);

const ADMIN_BASE = `http://${FWD_ADMIN_HOST}:${FWD_ADMIN_PORT}`;

const app = express();
app.use(express.json({ limit: '2mb' }));
app.use(express.static(path.join(__dirname, 'public')));

// ---- sidecar state ----
// Protobuf 载荷：用 protobufjs 运行时加载同一份 proto/market.proto
// 目的：在 Web 页面直接发送/解码“结构体 -> bytes”的效果。
let marketProtoRoot = null;
let StockQuoteType = null;
let HeartbeatType = null;
let MarketPayloadType = null;
let marketProtoLoadError = '';

function safeLoadMarketProto() {
  try {
    const protoPath = path.join(__dirname, '..', 'proto', 'market.proto');
    marketProtoRoot = protobuf.loadSync(protoPath);
    StockQuoteType = marketProtoRoot.lookupType('fwd.market.StockQuote');
    HeartbeatType = marketProtoRoot.lookupType('fwd.market.Heartbeat');
    MarketPayloadType = marketProtoRoot.lookupType('fwd.market.MarketPayload');
    marketProtoLoadError = '';
  } catch (e) {
    marketProtoLoadError = String(e && e.message ? e.message : e);
  }
}

safeLoadMarketProto();

const WireMsgType = {
  STOCK_QUOTE: 100,
  HEARTBEAT: 101,
  MARKET_PAYLOAD: 102,
};

function toProtoPreview(obj) {
  try {
    const s = JSON.stringify(obj);
    // 限制长度，避免 UI / SSE 被超大消息拖慢
    return s.length > 800 ? s.slice(0, 800) + '…' : s;
  } catch (e) {
    return null;
  }
}

function decodeBodyByMsgType(msgType, bodyBuf) {
  if (!marketProtoRoot) return null;
  try {
    const v2s = (v) => {
      if (v == null) return '';
      if (typeof v === 'object' && typeof v.toString === 'function') return v.toString();
      return String(v);
    };

    if (msgType === WireMsgType.STOCK_QUOTE) {
      const m = StockQuoteType.decode(bodyBuf);
      const out = {
        symbol: m.symbol,
        last_price: m.lastPrice,
        volume: v2s(m.volume),
        ts_ms: v2s(m.tsMs),
        exchange: m.exchange,
      };
      return { type: 'StockQuote', preview: toProtoPreview(out) };
    }
    if (msgType === WireMsgType.HEARTBEAT) {
      const m = HeartbeatType.decode(bodyBuf);
      const out = { ts_ms: v2s(m.tsMs), node_id: m.nodeId };
      return { type: 'Heartbeat', preview: toProtoPreview(out) };
    }
    if (msgType === WireMsgType.MARKET_PAYLOAD) {
      const mp = MarketPayloadType.decode(bodyBuf);
      // protobufjs oneof：我们用字段存在与否来判断。
      const out = { inner: null };
      if (mp.stockQuote) {
        out.inner = 'stock_quote';
        out.stock_quote = {
          symbol: mp.stockQuote.symbol,
          last_price: mp.stockQuote.lastPrice,
          volume: v2s(mp.stockQuote.volume),
          ts_ms: v2s(mp.stockQuote.tsMs),
          exchange: mp.stockQuote.exchange,
        };
      }
      if (mp.heartbeat) {
        out.inner = 'heartbeat';
        out.heartbeat = { ts_ms: v2s(mp.heartbeat.tsMs), node_id: mp.heartbeat.nodeId };
      }
      return { type: 'MarketPayload', preview: toProtoPreview(out) };
    }
  } catch (e) {
    return { type: 'unknown', preview: null, error: String(e && e.message ? e.message : e) };
  }
  return null;
}

let upstreamSock = null;
let upstreamConnecting = false;
let upstreamConnectedAtMs = 0;
let upstreamLastError = '';

const sseClients = new Set(); // SSE 响应对象（res）
const frameRing = [];
const FRAME_RING_MAX = 200;
let framesReceived = 0;
let framesStreamed = 0;
let downstreamLastError = '';
let downstreamConnectedAtMs = 0;

// 下游连接池（用于模拟/增加 sidecar 的下游客户端连接数）
let initialDownstreamConnections = envInt('DOWNSTREAM_CONNECTIONS', 1);
if (!Number.isFinite(initialDownstreamConnections) || initialDownstreamConnections < 0) initialDownstreamConnections = 1;
if (initialDownstreamConnections > 200) initialDownstreamConnections = 200;
let nextDownstreamId = 1;
const downstreamPool = new Map(); // id -> { id, sock, connectedAtMs, lastError, reconnectTimer }
const downstreamFrameRings = new Map(); // id -> frame[]
const DOWNSTREAM_FRAME_RING_MAX = 300;

function nowMs() {
  return Date.now();
}

function pushRing(item) {
  frameRing.push(item);
  while (frameRing.length > FRAME_RING_MAX) frameRing.shift();
}

function pushDownstreamRing(connId, frame) {
  if (!downstreamFrameRings.has(connId)) downstreamFrameRings.set(connId, []);
  const ring = downstreamFrameRings.get(connId);
  ring.push(frame);
  while (ring.length > DOWNSTREAM_FRAME_RING_MAX) ring.shift();
}

function sseSend(res, event, dataObj) {
  try {
    res.write(`event: ${event}\n`);
    res.write(`data: ${JSON.stringify(dataObj)}\n\n`);
    return true;
  } catch (e) {
    return false;
  }
}

function broadcastFrame(frame) {
  for (const res of Array.from(sseClients)) {
    const ok = sseSend(res, 'frame', frame);
    if (!ok) sseClients.delete(res);
    else framesStreamed += 1;
  }
}

function broadcastFrameToFiltered(frame) {
  for (const res of Array.from(sseClients)) {
    const want = res.__conn_id;
    if (want != null && String(want) !== String(frame.conn_id)) continue;
    const ok = sseSend(res, 'frame', frame);
    if (!ok) sseClients.delete(res);
    else framesStreamed += 1;
  }
}

function ensureUpstreamConnected(cb) {
  if (upstreamSock && !upstreamSock.destroyed) return cb(null, upstreamSock);
  if (upstreamConnecting) return cb(new Error('upstream_connecting'));
  upstreamConnecting = true;
  upstreamLastError = '';

  const sock = net.createConnection({ host: FWD_UP_HOST, port: FWD_UP_PORT }, () => {
    upstreamSock = sock;
    upstreamConnecting = false;
    upstreamConnectedAtMs = nowMs();
    cb(null, sock);
  });

  sock.setNoDelay(true);
  sock.on('error', (err) => {
    upstreamLastError = String(err && err.message ? err.message : err);
  });
  sock.on('close', () => {
    if (upstreamSock === sock) upstreamSock = null;
  });

  sock.setTimeout(15000, () => {
    upstreamLastError = 'timeout';
    sock.destroy();
  });

  // 防止 connect 阶段直接失败时 upstreamConnecting 卡死
  sock.once('error', (err) => {
    upstreamConnecting = false;
    cb(err);
  });
}

function packHeader(bodyLen, msgType, flags, seq, srcUserIdStr = '0', dstUserIdStr = '0') {
  const b = Buffer.alloc(HEADER_LEN);
  b.writeUInt32LE(MAGIC >>> 0, 0);
  b.writeUInt16LE(VERSION, 4);
  b.writeUInt16LE(HEADER_LEN, 6);
  b.writeUInt32LE(bodyLen >>> 0, 8);
  b.writeUInt32LE((msgType >>> 0), 12);
  b.writeUInt32LE((flags >>> 0), 16);
  b.writeUInt32LE((seq >>> 0), 20);
  let src = 0n;
  let dst = 0n;
  try {
    src = BigInt(String(srcUserIdStr));
    dst = BigInt(String(dstUserIdStr));
  } catch {
    src = 0n;
    dst = 0n;
  }
  b.writeBigUInt64LE(src, 24);
  b.writeBigUInt64LE(dst, 32);
  return b;
}

async function fetchJson(pathname) {
  const r = await fetch(`${ADMIN_BASE}${pathname}`, { method: 'GET', timeout: 2000 });
  if (!r.ok) throw new Error(`forwarder_admin_http_${r.status}`);
  return await r.json();
}

// ---- downstream receiver ----
function parseFramesFromSocket(sock, connId, onFrame, onBadHeader) {
  let buf = Buffer.alloc(0);
  let want = HEADER_LEN;
  let currentHeader = null;

  function pump() {
    while (buf.length >= want) {
      const chunk = buf.slice(0, want);
      buf = buf.slice(want);

      if (!currentHeader) {
        const magic = chunk.readUInt32LE(0);
        const ver = chunk.readUInt16LE(4);
        const hlen = chunk.readUInt16LE(6);
        const blen = chunk.readUInt32LE(8);
        const msg_type = chunk.readUInt32LE(12);
        const flags = chunk.readUInt32LE(16);
        const seq = chunk.readUInt32LE(20);
        const src_user_id = chunk.readBigUInt64LE(24).toString();
        const dst_user_id = chunk.readBigUInt64LE(32).toString();
        if (magic !== MAGIC || ver !== VERSION || hlen !== HEADER_LEN) {
          onBadHeader(`invalid_header magic=${magic} ver=${ver} hlen=${hlen}`);
          sock.destroy();
          return;
        }
        currentHeader = { magic, ver, hlen, blen, msg_type, flags, seq, src_user_id, dst_user_id };
        want = blen;
        if (want === 0) {
          onFrame({ ts_ms: nowMs(), conn_id: connId, header: currentHeader, body_b64: '', body_len: 0 });
          currentHeader = null;
          want = HEADER_LEN;
        }
      } else {
        const body = chunk;
        const decoded = decodeBodyByMsgType(currentHeader.msg_type, body);
        onFrame({
          ts_ms: nowMs(),
          conn_id: connId,
          header: currentHeader,
          body_len: body.length,
          body_b64: body.toString('base64'),
          body_proto_type: decoded ? decoded.type : null,
          body_proto_preview: decoded ? decoded.preview : null,
        });
        currentHeader = null;
        want = HEADER_LEN;
      }
    }
  }

  sock.on('data', (data) => {
    buf = Buffer.concat([buf, data]);
    pump();
  });
}

function connectDownstream(id) {
  const slot = downstreamPool.get(id);
  if (!slot) return;
  if (slot.sock && !slot.sock.destroyed) return;

  const sock = net.createConnection({ host: FWD_DOWN_HOST, port: FWD_DOWN_PORT }, () => {
    slot.connectedAtMs = nowMs();
    slot.lastError = '';
    downstreamLastError = '';
    downstreamConnectedAtMs = slot.connectedAtMs;
    console.log(`downstream#${id} connected to ${FWD_DOWN_HOST}:${FWD_DOWN_PORT}`);
  });
  slot.sock = sock;
  sock.setNoDelay(true);

  parseFramesFromSocket(
    sock,
    id,
    (frame) => {
      framesReceived += 1;
      pushRing(frame);
      pushDownstreamRing(id, frame);
      broadcastFrameToFiltered(frame);
    },
    (errText) => {
      slot.lastError = errText;
      downstreamLastError = errText;
    }
  );

  sock.on('error', (err) => {
    const msg = String(err && err.message ? err.message : err);
    slot.lastError = msg;
    downstreamLastError = msg;
  });
  sock.on('close', () => {
    slot.sock = null;
    if (!downstreamPool.has(id)) return;
    slot.reconnectTimer = setTimeout(() => connectDownstream(id), 1000);
  });
}

function addDownstreamConnection() {
  if (downstreamPool.size >= 200) return null;
  const id = nextDownstreamId++;
  downstreamPool.set(id, { id, sock: null, connectedAtMs: 0, lastError: '', reconnectTimer: null });
  connectDownstream(id);
  return id;
}

function removeDownstreamConnection(id) {
  const slot = downstreamPool.get(id);
  if (!slot) return false;
  if (slot.reconnectTimer) clearTimeout(slot.reconnectTimer);
  if (slot.sock && !slot.sock.destroyed) slot.sock.destroy();
  downstreamPool.delete(id);
  downstreamFrameRings.delete(id);
  return true;
}

function reconcileDownstreamPool() {
  for (const [id, slot] of downstreamPool.entries()) {
    if (!slot.sock || slot.sock.destroyed) connectDownstream(id);
  }
}

// ---- API ----
app.get('/api/health', async (req, res) => {
  res.json({ ok: true });
});

app.get('/api/stats', async (req, res) => {
  let fwd = null;
  let fwd_err = '';
  try {
    fwd = await fetchJson('/api/stats');
  } catch (e) {
    fwd_err = String(e && e.message ? e.message : e);
  }
  res.json({
    ts_ms: nowMs(),
    forwarder: fwd,
    forwarder_error: fwd_err,
    sidecar: {
      upstream: {
        host: FWD_UP_HOST,
        port: FWD_UP_PORT,
        connected: !!(upstreamSock && !upstreamSock.destroyed),
        connected_at_ms: upstreamConnectedAtMs,
        last_error: upstreamLastError,
      },
      downstream: {
        host: FWD_DOWN_HOST,
        port: FWD_DOWN_PORT,
        connected_at_ms: downstreamConnectedAtMs,
        last_error: downstreamLastError,
      },
      downstream_pool: {
        desired: downstreamPool.size,
        active: Array.from(downstreamPool.values()).filter(s => s.sock && !s.sock.destroyed).length,
      },
      sse_clients: sseClients.size,
      frames_received: framesReceived,
      frames_streamed: framesStreamed,
      frame_ring_size: frameRing.length,
    },
  });
});

app.get('/api/events', async (req, res) => {
  try {
    const ev = await fetchJson('/api/events');
    res.json(ev);
  } catch (e) {
    res.status(502).json({ error: 'forwarder_admin_unreachable', detail: String(e && e.message ? e.message : e) });
  }
});

app.get('/api/downstream/status', async (req, res) => {
  const slots = Array.from(downstreamPool.values()).sort((a, b) => a.id - b.id).map(s => ({
    id: s.id,
    connected: !!(s.sock && !s.sock.destroyed),
    connected_at_ms: s.connectedAtMs,
    last_error: s.lastError,
    frames_ring: (downstreamFrameRings.get(s.id) || []).length,
  }));
  res.json({
    host: FWD_DOWN_HOST,
    port: FWD_DOWN_PORT,
    desired: downstreamPool.size,
    active: slots.filter(s => s.connected).length,
    slots,
  });
});

app.post('/api/downstream/set_connections', async (req, res) => {
  const body = req.body || {};
  let n = parseInt(body.count, 10);
  if (!Number.isFinite(n) || n < 0) n = 0;
  if (n > 200) n = 200;
  while (downstreamPool.size < n) addDownstreamConnection();
  while (downstreamPool.size > n) {
    const ids = Array.from(downstreamPool.keys()).sort((a, b) => b - a);
    if (!ids.length) break;
    removeDownstreamConnection(ids[0]);
  }
  res.json({ ok: true, desired: downstreamPool.size });
});

app.post('/api/downstream/add', async (req, res) => {
  const id = addDownstreamConnection();
  if (id == null) return res.status(400).json({ error: 'limit_reached' });
  res.json({ ok: true, id });
});

app.post('/api/downstream/remove', async (req, res) => {
  const body = req.body || {};
  const id = parseInt(body.id, 10);
  if (!Number.isFinite(id)) return res.status(400).json({ error: 'invalid_id' });
  const ok = removeDownstreamConnection(id);
  if (!ok) return res.status(404).json({ error: 'not_found' });
  res.json({ ok: true });
});

app.get('/api/upstream/status', async (req, res) => {
  res.json({
    host: FWD_UP_HOST,
    port: FWD_UP_PORT,
    connected: !!(upstreamSock && !upstreamSock.destroyed),
    connected_at_ms: upstreamConnectedAtMs,
    last_error: upstreamLastError,
    note: '此连接会占用转发器的“唯一上游连接”槽位（调试模式）。',
  });
});

app.post('/api/upstream/disconnect', async (req, res) => {
  if (upstreamSock && !upstreamSock.destroyed) upstreamSock.destroy();
  upstreamSock = null;
  res.json({ ok: true });
});

app.post('/api/upstream/send', async (req, res) => {
  const body = req.body || {};
  const msg_type = (body.msg_type >>> 0) || 100;
  const flags = (body.flags >>> 0) || 0;
  const seq = (body.seq >>> 0) || 1;
  const src_user_id = parseU64Json(body.src_user_id, '0');
  const dst_user_id = parseU64Json(body.dst_user_id, '0');
  const encoding = body.body_encoding || 'utf8';

  let payload = Buffer.alloc(0);
  try {
    if (body.body == null) payload = Buffer.alloc(0);
    else if (encoding === 'hex') payload = Buffer.from(String(body.body).replace(/\s+/g, ''), 'hex');
    else payload = Buffer.from(String(body.body), 'utf8');
  } catch (e) {
    return res.status(400).json({ error: 'invalid_body', detail: String(e && e.message ? e.message : e) });
  }

  ensureUpstreamConnected((err, sock) => {
    if (err) return res.status(502).json({ error: 'upstream_connect_failed', detail: String(err.message || err) });
    const hdr = packHeader(payload.length, msg_type, flags, seq, src_user_id, dst_user_id);
    const out = Buffer.concat([hdr, payload]);
    sock.write(out, (e) => {
      if (e) return res.status(502).json({ error: 'upstream_write_failed', detail: String(e.message || e) });
      res.json({
        ok: true,
        sent: { msg_type, flags, seq, src_user_id, dst_user_id, body_len: payload.length, body_encoding: encoding },
        warning: '注意：sidecar 的上游连接会踢掉其他上游连接（调试模式）。',
      });
    });
  });
});

// Protobuf 载荷发送：浏览器填“结构体字段(JSON)”，sidecar 编码为 Protobuf bytes 写入 upstream。
app.post('/api/upstream/send_protobuf', async (req, res) => {
  const body = req.body || {};
  const flags = (body.flags >>> 0) || 0;
  const seq = (body.seq >>> 0) || 1;
  const src_user_id = parseU64Json(body.src_user_id, '0');
  const dst_user_id = parseU64Json(body.dst_user_id, '0');
  const proto_kind = String(body.proto_kind || '');
  const data = body.data || {};

  if (!marketProtoRoot) {
    return res.status(500).json({ error: 'protobuf_unavailable', detail: marketProtoLoadError || 'failed_to_load_proto' });
  }

  let msg_type = (body.msg_type >>> 0) || 0;
  let type = null;
  let payload = {};

  const toInt64 = (v) => {
    const Long = protobuf.util.Long;
    if (v == null) return Long.fromNumber(0);
    if (Long.isLong(v)) return v;
    if (typeof v === 'number') return Long.fromNumber(v);
    // protobufjs int64（Long）可由十进制字符串构造
    return Long.fromString(String(v));
  };

  try {
    if (proto_kind === 'stock_quote') {
      msg_type = msg_type || WireMsgType.STOCK_QUOTE;
      type = StockQuoteType;
      payload = {
        symbol: String(data.symbol || ''),
        lastPrice: Number(data.last_price != null ? data.last_price : 0),
        volume: toInt64(data.volume != null ? data.volume : 0),
        tsMs: toInt64(data.ts_ms != null ? data.ts_ms : 0),
        exchange: String(data.exchange || ''),
      };
    } else if (proto_kind === 'heartbeat') {
      msg_type = msg_type || WireMsgType.HEARTBEAT;
      type = HeartbeatType;
      payload = {
        tsMs: toInt64(data.ts_ms != null ? data.ts_ms : 0),
        nodeId: String(data.node_id || ''),
      };
    } else if (proto_kind === 'market_payload') {
      msg_type = msg_type || WireMsgType.MARKET_PAYLOAD;
      type = MarketPayloadType;
      const inner = String(data.inner || 'stock_quote');
      if (inner === 'stock_quote') {
        payload = {
          stockQuote: {
            symbol: String(data.stock_quote && data.stock_quote.symbol ? data.stock_quote.symbol : ''),
            lastPrice: Number(data.stock_quote && data.stock_quote.last_price != null ? data.stock_quote.last_price : 0),
            volume: toInt64(data.stock_quote && data.stock_quote.volume != null ? data.stock_quote.volume : 0),
            tsMs: toInt64(data.stock_quote && data.stock_quote.ts_ms != null ? data.stock_quote.ts_ms : 0),
            exchange: String(data.stock_quote && data.stock_quote.exchange ? data.stock_quote.exchange : ''),
          },
        };
      } else {
        payload = {
          heartbeat: {
            tsMs: toInt64(data.heartbeat && data.heartbeat.ts_ms != null ? data.heartbeat.ts_ms : 0),
            nodeId: String(data.heartbeat && data.heartbeat.node_id ? data.heartbeat.node_id : ''),
          },
        };
      }
    } else {
      return res.status(400).json({ error: 'invalid_proto_kind', detail: 'use stock_quote|heartbeat|market_payload' });
    }
  } catch (e) {
    return res.status(400).json({ error: 'invalid_fields', detail: String(e && e.message ? e.message : e) });
  }

  const verifyErr = type.verify(payload);
  if (verifyErr) return res.status(400).json({ error: 'proto_verify_failed', detail: verifyErr });

  const buf = Buffer.from(type.encode(payload).finish());

  ensureUpstreamConnected((err, sock) => {
    if (err) return res.status(502).json({ error: 'upstream_connect_failed', detail: String(err.message || err) });
    const hdr = packHeader(buf.length, msg_type, flags, seq, src_user_id, dst_user_id);
    const out = Buffer.concat([hdr, buf]);
    sock.write(out, (e) => {
      if (e) return res.status(502).json({ error: 'upstream_write_failed', detail: String(e.message || e) });
      res.json({
        ok: true,
        sent: { msg_type, flags, seq, src_user_id, dst_user_id, body_len: buf.length, proto_kind },
      });
    });
  });
});

app.get('/api/downstream/stream', (req, res) => {
  const connId = req.query && req.query.conn_id != null ? String(req.query.conn_id) : null;
  res.status(200);
  res.setHeader('Content-Type', 'text/event-stream; charset=utf-8');
  res.setHeader('Cache-Control', 'no-cache, no-transform');
  res.setHeader('Connection', 'keep-alive');

  res.write('event: hello\n');
  res.write(`data: ${JSON.stringify({ ts_ms: nowMs(), ring_size: frameRing.length, conn_id: connId })}\n\n`);

  res.__conn_id = connId;
  // 新 SSE 订阅建立后，先补发最近缓存的帧（全局或按 conn 过滤）
  if (connId == null) {
    for (const f of frameRing) sseSend(res, 'frame', f);
  } else {
    const ring = downstreamFrameRings.get(parseInt(connId, 10)) || [];
    for (const f of ring) sseSend(res, 'frame', f);
  }

  sseClients.add(res);
  const timer = setInterval(() => {
    try {
      res.write(`event: ping\ndata: ${nowMs()}\n\n`);
    } catch (e) {
      // 忽略：通常是客户端断开导致 write 抛错
    }
  }, 15000);

  req.on('close', () => {
    clearInterval(timer);
    sseClients.delete(res);
  });
});

app.get('/api/downstream/frames', async (req, res) => {
  const connId = req.query && req.query.conn_id != null ? parseInt(String(req.query.conn_id), 10) : NaN;
  if (!Number.isFinite(connId)) return res.status(400).json({ error: 'invalid_conn_id' });
  const ring = downstreamFrameRings.get(connId) || [];
  res.json({ conn_id: connId, frames: ring });
});

while (downstreamPool.size < initialDownstreamConnections) addDownstreamConnection();
reconcileDownstreamPool();

app.listen(HTTP_PORT, HTTP_HOST, () => {
  console.log(`web sidecar listening on http://${HTTP_HOST}:${HTTP_PORT}`);
  console.log(`forwarder: upstream=${FWD_UP_HOST}:${FWD_UP_PORT} downstream=${FWD_DOWN_HOST}:${FWD_DOWN_PORT} admin=${ADMIN_BASE}`);
});

