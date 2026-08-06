#!/usr/bin/env node
import { createRequire } from 'node:module';

const require = createRequire(new URL('../frontend/package.json', import.meta.url));
const WebSocket = require('ws');

const url = process.env.ESPAGENT_WS_URL || 'ws://127.0.0.1:4175/ws';
const chatId = 'webskill-' + Date.now();
const messages = (process.env.ESPAGENT_WS_MESSAGES || 'skill blue,skill red,skill light off')
  .split(',')
  .map((item) => item.trim())
  .filter(Boolean);

const replies = [];
const sent = [];
let sendIndex = 0;
let closed = false;
let ws;

function finish(code, error = null) {
  if (closed) return;
  closed = true;
  const checks = {
    sent: sent.length,
    replies: replies.length,
    blue: replies.some((line) => /blue|蓝|0,0,255/.test(line)),
    red: replies.some((line) => /red|红|255,0,0/.test(line)),
    off: replies.some((line) => /off|关闭|0,0,0/.test(line)),
    skillRule: replies.some((line) => /skill 规则|Runtime Skill|tool-status-light/.test(line)),
    meshResult: replies.some((line) => /status light on GPIO 48|远程control_agent已执行|mesh/i.test(line)),
  };
  const passed = checks.sent === messages.length &&
    checks.replies >= messages.length &&
    checks.blue && checks.red && checks.off &&
    checks.skillRule && checks.meshResult;
  console.log(JSON.stringify({ passed, url, chatId, messages, sent, checks, replies, error }, null, 2));
  try { ws?.close(); } catch {}
  process.exit(code ?? (passed ? 0 : 1));
}

ws = new WebSocket(url);

ws.on('open', () => {
  const tick = () => {
    if (sendIndex >= messages.length) {
      setTimeout(() => finish(undefined), 18000);
      return;
    }
    const content = messages[sendIndex++];
    sent.push(content);
    ws.send(JSON.stringify({ type: 'message', chat_id: chatId, content }));
    setTimeout(tick, 7000);
  };
  setTimeout(tick, 500);
});

ws.on('message', (raw) => {
  const text = raw.toString();
  replies.push(text);
  console.log('WS_REPLY ' + text);
});

ws.on('error', (err) => finish(2, String(err?.message || err)));
setTimeout(() => finish(3, 'timeout'), 60000);
