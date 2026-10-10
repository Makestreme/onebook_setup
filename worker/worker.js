// OneBook tunnel: relays the web app's HTTPS requests to the OneBook device over its WebSocket.
import { DurableObject } from 'cloudflare:workers';

const CHUNK = 8192;                      // must match the firmware's CHUNK
const MAX_BODY_BYTES = 25 * 1024 * 1024;
const IDLE_TIMEOUT_MS = 45000;
const MAX_QUEUE = 32;
const TUNNEL_NAME = 'onebook';

const PREFLIGHT = {
  'Access-Control-Allow-Methods': 'GET, PUT, DELETE, OPTIONS',
  'Access-Control-Allow-Headers': 'Authorization, Content-Type',
  'Access-Control-Max-Age': '86400',
};

function corsHeaders(request, env) {
  const origin = request.headers.get('Origin');
  const allowed = String(env.ALLOWED_ORIGIN || '').split(',').map(s => s.trim().replace(/\/+$/, '')).filter(Boolean);
  const headers = { Vary: 'Origin' };
  if (origin && (allowed.includes('*') || allowed.includes(origin))) headers['Access-Control-Allow-Origin'] = origin;
  return headers;
}

function json(status, error, headers = {}) {
  return new Response(JSON.stringify({ error }), {
    status,
    headers: { ...headers, 'Content-Type': 'application/json', 'Cache-Control': 'no-store' },
  });
}

function text(status, body) {
  return new Response(body, { status, headers: { 'Content-Type': 'text/plain' } });
}

async function tokenMatches(request, expected) {
  const auth = request.headers.get('Authorization') || '';
  const given = auth.startsWith('Bearer ') ? auth.slice(7).trim() : '';
  if (!given || !expected) return false;
  const enc = new TextEncoder();
  const [a, b] = await Promise.all([
    crypto.subtle.digest('SHA-256', enc.encode(given)),
    crypto.subtle.digest('SHA-256', enc.encode(expected)),
  ]);
  return crypto.subtle.timingSafeEqual(a, b);
}

function tunnel(env) {
  return env.TUNNEL.get(env.TUNNEL.idFromName(TUNNEL_NAME));
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (url.pathname === '/device') {
      if ((request.headers.get('Upgrade') || '').toLowerCase() !== 'websocket') return text(426, 'Expected a WebSocket upgrade');
      if (!env.DEVICE_SECRET) return text(500, 'DEVICE_SECRET is not set on the Worker');
      if (!(await tokenMatches(request, env.DEVICE_SECRET))) return text(401, 'Unauthorized');
      return tunnel(env).fetch(request);
    }

    if (url.pathname.startsWith('/api/')) {
      const cors = corsHeaders(request, env);
      if (request.method === 'OPTIONS') return new Response(null, { status: 204, headers: { ...cors, ...PREFLIGHT } });
      if (!env.APP_TOKEN) return json(500, 'APP_TOKEN is not set on the Worker', cors);
      if (!(await tokenMatches(request, env.APP_TOKEN))) return json(401, 'Invalid access token', cors);
      if (!['GET', 'PUT', 'DELETE'].includes(request.method)) return json(405, 'Method not allowed', cors);
      return tunnel(env).fetch(request);
    }

    if (url.pathname === '/') return text(200, 'OneBook tunnel is running.');
    return text(404, 'Not found');
  },
};

// One instance holds the device socket and runs requests one at a time, because the device can only serve one.
export class Tunnel extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.queue = [];
    this.busy = false;
    this.current = null;
    this.nextId = 0;
  }

  async fetch(request) {
    if (new URL(request.url).pathname === '/device') return this.acceptDevice();
    return this.enqueue(request);
  }

  acceptDevice() {
    const pair = new WebSocketPair();
    const [client, server] = [pair[0], pair[1]];
    for (const old of this.ctx.getWebSockets('device')) {
      try { old.close(1012, 'Replaced by a new connection'); } catch { /* already closed */ }
    }
    if (this.current) this.current.fail(503, 'OneBook reconnected, please retry', false);
    this.ctx.acceptWebSocket(server, ['device']);
    return new Response(null, { status: 101, webSocket: client });
  }

  device() {
    const sockets = this.ctx.getWebSockets('device');
    for (let i = sockets.length - 1; i >= 0; i--) {
      if (sockets[i].readyState === 1) return sockets[i];
    }
    return null;
  }

  enqueue(request) {
    const cors = corsHeaders(request, this.env);
    if (this.queue.length >= MAX_QUEUE) return json(503, 'OneBook is busy, try again shortly', cors);
    return new Promise(respond => {
      this.queue.push({ request, respond, cors });
      this.pump();
    });
  }

  async pump() {
    if (this.busy) return;
    const item = this.queue.shift();
    if (!item) return;
    this.busy = true;
    try {
      await this.exchange(item);
    } catch (e) {
      item.respond(json(502, 'Tunnel error', item.cors));
    } finally {
      this.busy = false;
      this.pump();
    }
  }

  exchange({ request, respond, cors }) {
    return new Promise(finish => {
      const ws = this.device();
      if (!ws) {
        respond(json(503, 'OneBook is offline', cors));
        return finish();
      }
      const hasLength = request.headers.has('Content-Length');
      const len = Number(request.headers.get('Content-Length') || 0);
      if (request.method === 'PUT' && !hasLength) {
        respond(json(411, 'Content-Length is required', cors));
        return finish();
      }
      if (!Number.isFinite(len) || len < 0 || len > MAX_BODY_BYTES) {
        respond(json(413, 'Request body is too large', cors));
        return finish();
      }

      const url = new URL(request.url);
      const id = this.nextId = (this.nextId % 0x7fffffff) + 1;
      let responded = false;
      let finished = false;
      let writer = null;
      let expected = 0;
      let received = 0;
      let timer = null;

      const send = obj => {
        try { ws.send(JSON.stringify(obj)); } catch { /* socket gone; close handler fails the exchange */ }
      };
      const done = () => {
        if (finished) return;
        finished = true;
        clearTimeout(timer);
        if (this.current === ex) this.current = null;
        finish();
      };
      const fail = (status, msg, tellDevice = true) => {
        if (finished) return;
        if (tellDevice) send({ t: 'abort', id });
        if (!responded) {
          responded = true;
          respond(json(status, msg, cors));
        } else if (writer) {
          writer.abort(new Error(msg)).catch(() => {});
        }
        done();
      };
      const touch = () => {
        if (finished) return;
        clearTimeout(timer);
        timer = setTimeout(() => fail(504, 'OneBook did not respond in time'), IDLE_TIMEOUT_MS);
      };

      const ex = {
        id,
        touch,
        fail,
        accepting: () => !finished && !responded,
        head: m => {
          if (responded || finished) return;
          responded = true;
          touch();
          expected = Number(m.len) || 0;
          const status = Number(m.s) || 502;
          const headers = { ...cors, 'Content-Type': m.ct || 'application/octet-stream', 'Cache-Control': 'no-store' };
          if (!expected) {
            respond(new Response(null, { status, headers }));
            return done();
          }
          const { readable, writable } = new FixedLengthStream(expected);
          writer = writable.getWriter();
          respond(new Response(readable, { status, headers }));
        },
        data: bytes => {
          if (!writer || finished) return;
          if (received + bytes.byteLength > expected) return fail(502, 'OneBook sent more data than announced');
          received += bytes.byteLength;
          touch();
          writer.write(bytes).catch(() => fail(499, 'Client went away'));
          if (received === expected) {
            writer.close().catch(() => {});
            done();
          }
        },
      };

      this.current = ex;
      touch();
      send({ t: 'req', id, m: request.method, p: url.pathname + url.search, len });
      if (len > 0) this.pipeBody(request, ws, ex, len);
    });
  }

  async pipeBody(request, ws, ex, len) {
    const reader = request.body.getReader();
    const frame = new Uint8Array(4 + CHUNK);
    new DataView(frame.buffer).setUint32(0, ex.id);
    let fill = 0;
    let sent = 0;
    const flush = () => {
      if (!fill) return;
      ws.send(frame.slice(0, 4 + fill));
      sent += fill;
      fill = 0;
      ex.touch();
    };
    try {
      for (;;) {
        if (!ex.accepting()) {
          await reader.cancel().catch(() => {});
          return;
        }
        const { done, value } = await reader.read();
        if (done) break;
        for (let off = 0; off < value.length;) {
          const n = Math.min(CHUNK - fill, value.length - off);
          frame.set(value.subarray(off, off + n), 4 + fill);
          fill += n;
          off += n;
          if (fill === CHUNK) flush();
        }
      }
      flush();
      if (sent !== len) ex.fail(400, 'Upload size did not match Content-Length');
    } catch {
      ex.fail(400, 'Upload was interrupted');
    }
  }

  webSocketMessage(ws, message) {
    const cur = this.current;
    if (typeof message === 'string') {
      let m;
      try { m = JSON.parse(message); } catch { return; }
      if (m.t === 'hello') {
        this.firmware = String(m.fw || '');
        return;
      }
      if (!cur || m.id !== cur.id) return;
      if (m.t === 'res') cur.head(m);
      else if (m.t === 'fail') cur.fail(502, 'OneBook could not finish the transfer', false);
      return;
    }
    if (!cur || message.byteLength < 4) return;
    if (new DataView(message).getUint32(0) !== cur.id) return;
    cur.data(new Uint8Array(message, 4));
  }

  webSocketClose(ws) {
    try { ws.close(1000, 'Closing'); } catch { /* already closed */ }
    if (this.current && !this.device()) this.current.fail(503, 'OneBook disconnected', false);
  }

  webSocketError(ws) {
    this.webSocketClose(ws);
  }
}
