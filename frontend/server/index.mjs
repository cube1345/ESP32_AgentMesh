import http from 'node:http';
import mqtt from 'mqtt';
import { WebSocketServer, WebSocket } from 'ws';

const PORT = Number(process.env.ESPAGENT_DASHBOARD_PORT || 4175);
const MQTT_HOST = process.env.ESPAGENT_MQTT_HOST || 'broker.emqx.io';
const MQTT_PORT = Number(process.env.ESPAGENT_MQTT_PORT || 1883);
const MQTT_PROTOCOL = process.env.ESPAGENT_MQTT_PROTOCOL || 'mqtt';
const TOPIC_PREFIX = (process.env.ESPAGENT_TOPIC_PREFIX || 'espagent/cube1345').replace(/\/+$/, '');
const MQTT_URL = `${MQTT_PROTOCOL}://${MQTT_HOST}:${MQTT_PORT}`;
const CHAT_REQUEST_TOPIC = `${TOPIC_PREFIX}/web/chat/request`;
const CHAT_REPLY_TOPIC = `${TOPIC_PREFIX}/web/chat/reply`;
const VOICE_STT_REQUEST_TOPIC = `${TOPIC_PREFIX}/voice/stt/request`;
const VOICE_STT_RESULT_TOPIC = `${TOPIC_PREFIX}/voice/stt/result`;
const SKILLS_API_BASE = (process.env.ESPAGENT_SKILLS_API_BASE || '').replace(/\/+$/, '');
const CHAT_GATEWAY_PATH = '/ws';
const runtimeSkills = new Map();
const chatSessions = new Map();
const wsSessions = new Set();

function nowIso() {
  return new Date().toISOString();
}

function normalizeNode(role, nodeId) {
  return {
    id: nodeId || role || 'unknown-node',
    role: role || 'unknown',
    transport: ['MQTT Mesh'],
    status: 'online',
    location: 'mqtt mesh',
    responsibilities: [],
    surfaces: ['dashboard']
  };
}

function inferResponsibilities(role) {
  if (role === 'sensor_agent') return ['环境采集', 'telemetry'];
  if (role === 'control_agent') return ['执行控制', 'actuator state'];
  if (role === 'guardian_agent') return ['policy check', 'audit', 'stateboard'];
  if (role === 'coordinator_agent') return ['LLM 调度', 'timeline'];
  return ['mesh node'];
}

function safeJsonParse(input) {
  try {
    return JSON.parse(input);
  } catch {
    return null;
  }
}

function skillDraftToMarkdown(skill) {
  const lines = [
    `# ${skill.name || 'Untitled Skill'}`,
    '',
    `- Scope: ${skill.scope || 'unspecified'}`,
    `- Trigger: ${skill.trigger || 'manual'}`,
    `- Policy: ${skill.policy || 'none'}`,
    `- Enabled: ${skill.enabled ? 'true' : 'false'}`,
    '',
    '## Prompt',
    '',
    skill.prompt || ''
  ];
  return lines.join('\n').trimEnd() + '\n';
}

function normalizeSkillName(name) {
  return String(name || '')
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9_-]+/g, '_')
    .replace(/^_+|_+$/g, '')
    .slice(0, 48);
}

function draftToRuntimeRecord(skill, source = 'local_mock', message = 'runtime skill installed') {
  const runtimeName = normalizeSkillName(skill.name || skill.id);
  return {
    id: skill.id || runtimeName,
    runtimeName,
    title: skill.name || runtimeName,
    path: `/spiffs/skills/${runtimeName}.md`,
    source,
    installedAt: nowIso(),
    cacheState: source === 'proxy' ? 'unknown' : 'invalidated',
    status: 'installed',
    scope: skill.scope || 'unspecified',
    enabled: Boolean(skill.enabled),
    lastMessage: message
  };
}

function upstreamSkillToRuntimeRecord(item) {
  const runtimeName = normalizeSkillName(item?.name || item?.title || 'unknown');
  return {
    id: runtimeName,
    runtimeName,
    title: item?.title || item?.name || runtimeName,
    path: item?.path || `/spiffs/skills/${runtimeName}.md`,
    source: 'proxy',
    installedAt: nowIso(),
    cacheState: 'unknown',
    status: 'installed',
    scope: 'runtime',
    enabled: true,
    lastMessage: typeof item?.size_bytes === 'number'
      ? `upstream listed, ${item.size_bytes} bytes`
      : 'upstream listed'
  };
}

function listRuntimeSkills() {
  return Array.from(runtimeSkills.values()).sort((a, b) => {
    return String(b.installedAt).localeCompare(String(a.installedAt));
  });
}

async function fetchUpstreamRuntimeSkills() {
  const response = await fetch(`${SKILLS_API_BASE}/api/skills`);
  const payload = await response.json().catch(() => ({}));
  if (!response.ok || payload?.ok === false || !Array.isArray(payload?.skills)) {
    throw new Error(payload?.error || `HTTP ${response.status}`);
  }

  runtimeSkills.clear();
  for (const item of payload.skills) {
    const record = upstreamSkillToRuntimeRecord(item);
    runtimeSkills.set(record.runtimeName, record);
  }
  return listRuntimeSkills();
}

function createStore() {
  return {
    mqtt: {
      connected: false,
      url: MQTT_URL,
      topicPrefix: TOPIC_PREFIX,
      lastEventAt: null
    },
    chatGateway: {
      enabled: true,
      path: CHAT_GATEWAY_PATH,
      upstreamUrl: `${MQTT_URL} -> ${CHAT_REQUEST_TOPIC}`,
      activeSessions: 0,
      connectedSessions: 0,
      lastEventAt: null,
      lastError: null
    },
    nodes: new Map(),
    telemetry: new Map(),
    timeline: [],
    alerts: [],
    guardian: null
  };
}

const store = createStore();

function ensureNode(nodeId, role) {
  const existing = store.nodes.get(nodeId);
  if (existing) {
    if (role && existing.role === 'unknown') {
      existing.role = role;
      existing.responsibilities = inferResponsibilities(role);
    }
    return existing;
  }

  const node = normalizeNode(role, nodeId);
  node.responsibilities = inferResponsibilities(role);
  store.nodes.set(node.id, node);
  return node;
}

function pushTimeline(entry) {
  store.timeline.unshift(entry);
  if (store.timeline.length > 60) {
    store.timeline.length = 60;
  }
}

function mapEnvironmentMetrics() {
  const sensorNode = Array.from(store.telemetry.values()).find(
    (item) => item.role === 'sensor_agent'
  );

  if (!sensorNode) {
    return [
      { label: '温度', value: 0, unit: '°C', status: 'attention', trend: 0 },
      { label: '湿度', value: 0, unit: '%', status: 'attention', trend: 0 },
      { label: 'eCO2', value: 0, unit: 'ppm', status: 'attention', trend: 0 },
      { label: 'TVOC', value: 0, unit: 'ppb', status: 'attention', trend: 0 },
      { label: '光照', value: 0, unit: 'lux', status: 'attention', trend: 0 },
      { label: 'Presence', value: 0, unit: 'detected', status: 'attention', trend: 0 }
    ];
  }

  const { payload } = sensorNode;
  const temp = Number(payload.temp || 0);
  const humidity = Number(payload.humidity || 0);
  const co2 = Number(payload.co2 || 0);
  const tvoc = Number(payload.tvoc || 0);
  const light = Number(payload.light_lux || 0);
  const presence = payload.present ? 1 : 0;

  return [
    { label: '温度', value: temp, unit: '°C', status: temp > 32 ? 'critical' : temp > 28 ? 'attention' : 'good', trend: Number((temp - Number(payload.temp_avg || temp)).toFixed(1)) },
    { label: '湿度', value: humidity, unit: '%', status: humidity > 70 ? 'critical' : humidity > 60 ? 'attention' : 'good', trend: Number((humidity - Number(payload.humidity_avg || humidity)).toFixed(1)) },
    { label: 'eCO2', value: co2, unit: 'ppm', status: co2 > 1000 ? 'critical' : co2 > 800 ? 'attention' : 'good', trend: 0 },
    { label: 'TVOC', value: tvoc, unit: 'ppb', status: tvoc > 120 ? 'critical' : tvoc > 60 ? 'attention' : 'good', trend: 0 },
    { label: '光照', value: light, unit: 'lux', status: light < 80 ? 'attention' : 'good', trend: Number((light - Number(payload.light_lux_avg || light)).toFixed(1)) },
    { label: 'Presence', value: presence, unit: 'detected', status: 'good', trend: 0 }
  ];
}

function toDashboardPayload() {
  const nodes = Array.from(store.nodes.values());
  const capabilities = [
    { name: 'read_environment', category: '感知', role: 'sensor_agent', maturity: '已验证', summary: '来自 sensor_agent 的真实环境遥测聚合。' },
    { name: 'mesh_send_command', category: '协同', role: 'coordinator_agent', maturity: '已验证', summary: '通过 MQTT Mesh 进行跨节点调度。' },
    { name: 'policy_check', category: '安全', role: 'guardian_agent', maturity: '已验证', summary: 'Guardian 对远程执行进行裁决与审计。' },
    { name: 'set_status_light', category: '控制', role: 'control_agent', maturity: '已验证', summary: '控制执行器状态并回传控制结果。' },
    { name: 'voice_request_stt', category: '语音', role: 'coordinator_agent', maturity: '进行中', summary: '语音前端通过 MQTT 参与协作。' },
    { name: 'virtual_device_control', category: '扩展', role: 'control_agent', maturity: '进行中', summary: '通过 manifest 扩展硬件控制面。' }
  ];

  const defaultSkills = [
    {
      id: 'real-skill-1',
      name: '真实环境数据展示',
      scope: 'dashboard',
      trigger: 'telemetry update',
      policy: '只读聚合',
      prompt: '订阅真实 MQTT telemetry 并汇总到 dashboard。',
      enabled: true
    }
  ];

  const preferences = {
    preferredChannel: 'Web Console',
    voiceOutput: true,
    privacyMode: 'metadata_only',
    automationAggressiveness: 60,
    summaryStyle: 'concise',
    preferredLanguage: 'zh-CN'
  };

  const flows = [
    {
      id: 'flow-1',
      step: 'MQTT 接入',
      transport: 'MQTT',
      producer: MQTT_URL,
      consumer: 'dashboard server',
      topic: `${TOPIC_PREFIX}/nodes/+/telemetry`,
      detail: '聚合服务直接订阅真实环境遥测。'
    },
    {
      id: 'flow-2',
      step: '状态聚合',
      transport: 'Node HTTP',
      producer: 'dashboard server',
      consumer: 'React frontend',
      topic: '/api/dashboard',
      detail: '将 nodes / telemetry / timeline 统一组装为一个接口。'
    }
  ];

  return {
    nodes,
    capabilities,
    timeline: store.timeline,
    environment: mapEnvironmentMetrics(),
    skills: defaultSkills,
    preferences,
    flows,
    mqtt: store.mqtt,
    chatGateway: store.chatGateway,
    guardian: store.guardian
  };
}

function noteChatGatewayEvent(error = null) {
  store.chatGateway.lastEventAt = nowIso();
  store.chatGateway.lastError = error;
}

function sendWsJson(socket, payload) {
  if (socket.readyState !== WebSocket.OPEN) {
    return;
  }
  socket.send(JSON.stringify(payload));
}

function createChatSession(downstream) {
  const session = {
    downstream,
    chatId: null
  };
  wsSessions.add(session);

  store.chatGateway.activeSessions += 1;
  store.chatGateway.connectedSessions += 1;
  noteChatGatewayEvent(null);
  sendWsJson(downstream, {
    type: 'system',
    content: `已连接到本地 MQTT Chat Gateway：${CHAT_REQUEST_TOPIC}`
  });

  downstream.on('message', (raw) => {
    const text = typeof raw === 'string' ? raw : raw.toString();
    const payload = safeJsonParse(text);
    if (!payload || typeof payload.type !== 'string') {
      sendWsJson(downstream, {
        type: 'system',
        content: '网关只接受已定义的 JSON 消息类型。'
      });
      return;
    }
    if (!store.mqtt.connected) {
      noteChatGatewayEvent('mqtt offline');
      sendWsJson(downstream, {
        type: 'system',
        content: 'MQTT 网关当前不可用，请稍后重试。'
      });
      return;
    }

    if (payload.type === 'message') {
      if (typeof payload.content !== 'string') {
        sendWsJson(downstream, {
          type: 'system',
          content: 'message 类型必须包含字符串 content。'
        });
        return;
      }

      const chatId = typeof payload.chat_id === 'string' && payload.chat_id.trim()
        ? payload.chat_id.trim()
        : 'web_console_01';
      if (session.chatId && chatSessions.get(session.chatId) === session) {
        chatSessions.delete(session.chatId);
      }
      session.chatId = chatId;
      chatSessions.set(chatId, session);

      const requestPayload = JSON.stringify({
        type: 'message',
        chat_id: chatId,
        content: payload.content
      });
      client.publish(CHAT_REQUEST_TOPIC, requestPayload, { qos: 0 }, (error) => {
        if (error) {
          noteChatGatewayEvent(error.message);
          sendWsJson(downstream, {
            type: 'system',
            content: `MQTT 聊天请求发送失败：${error.message}`
          });
          return;
        }
        noteChatGatewayEvent(null);
      });
      return;
    }

    if (payload.type === 'stt_result') {
      const transcript = typeof payload.transcript === 'string' ? payload.transcript.trim() : '';
      if (!transcript) {
        sendWsJson(downstream, {
          type: 'system',
          content: 'stt_result 缺少 transcript。'
        });
        return;
      }

      const sttPayload = JSON.stringify({
        schema: 'espagent.voice.stt_result.v1',
        event: 'stt_result',
        request_id: typeof payload.request_id === 'string' && payload.request_id.trim()
          ? payload.request_id.trim()
          : `web-stt-${Date.now()}`,
        transcript,
        status: typeof payload.status === 'string' ? payload.status : 'ok',
        provider: typeof payload.provider === 'string' ? payload.provider : 'browser_webspeech',
        reply_channel: typeof payload.reply_channel === 'string' ? payload.reply_channel : 'web',
        reply_chat_id: typeof payload.reply_chat_id === 'string' && payload.reply_chat_id.trim()
          ? payload.reply_chat_id.trim()
          : (session.chatId || 'web_console_01'),
        ts_ms: Date.now()
      });
      client.publish(VOICE_STT_RESULT_TOPIC, sttPayload, { qos: 0 }, (error) => {
        if (error) {
          noteChatGatewayEvent(error.message);
          sendWsJson(downstream, {
            type: 'system',
            content: `STT 结果回传失败：${error.message}`
          });
          return;
        }
        noteChatGatewayEvent(null);
        pushTimeline({
          time: new Date().toLocaleTimeString('zh-CN', { hour12: false }),
          stage: 'stt_result',
          source: 'dashboard_web',
          target: 'coordinator_agent',
          payload: transcript,
          status: 'ok'
        });
        sendWsJson(downstream, {
          type: 'system',
          content: `STT 转写已回传：${transcript}`
        });
      });
      return;
    }

    sendWsJson(downstream, {
      type: 'system',
      content: `未知消息类型：${payload.type}`
    });
  });

  downstream.on('close', () => {
    store.chatGateway.activeSessions = Math.max(0, store.chatGateway.activeSessions - 1);
    store.chatGateway.connectedSessions = Math.max(0, store.chatGateway.connectedSessions - 1);
    noteChatGatewayEvent(null);
    if (session.chatId && chatSessions.get(session.chatId) === session) {
      chatSessions.delete(session.chatId);
    }
    wsSessions.delete(session);
  });
}

function handleTelemetry(topic, payload) {
  const nodeId = payload.node_id || topic.split('/').at(-2) || 'unknown-node';
  const role = payload.role || 'sensor_agent';
  const node = ensureNode(nodeId, role);
  node.status = 'online';
  node.location = node.location === 'mqtt mesh' ? topic : node.location;

  store.telemetry.set(nodeId, {
    nodeId,
    role,
    payload,
    updatedAt: nowIso()
  });

  pushTimeline({
    time: new Date().toLocaleTimeString('zh-CN', { hour12: false }),
    stage: 'telemetry',
    source: nodeId,
    target: 'dashboard',
    payload: `temp=${payload.temp ?? '-'} humidity=${payload.humidity ?? '-'} co2=${payload.co2 ?? '-'} lux=${payload.light_lux ?? '-'}`,
    status: 'ok'
  });
}

function handleState(topic, payload) {
  const nodeId = payload.node_id || topic.split('/').at(-2) || 'unknown-node';
  const role = payload.role || 'unknown';
  const node = ensureNode(nodeId, role);
  node.status = payload.state === 'online' ? 'online' : payload.state === 'offline' ? 'offline' : 'degraded';
  node.transport = ['MQTT Mesh'];
  node.location = topic;
  node.responsibilities = inferResponsibilities(role);

  pushTimeline({
    time: new Date().toLocaleTimeString('zh-CN', { hour12: false }),
    stage: 'node_state',
    source: nodeId,
    target: 'dashboard',
    payload: `state=${payload.state || 'unknown'} role=${role}`,
    status: payload.state === 'online' ? 'ok' : 'warn'
  });
}

function handleTimeline(payload) {
  const stage = payload.event || payload.type || payload.phase || 'timeline';
  pushTimeline({
    time: new Date(Number(payload.ts_ms || Date.now())).toLocaleTimeString('zh-CN', { hour12: false }),
    stage,
    source: payload.node_id || payload.sender || payload.source || 'mesh',
    target: payload.recipient || payload.target_role || payload.target_node || 'dashboard',
    payload: payload.summary || payload.text || payload.detail || JSON.stringify(payload).slice(0, 160),
    status: payload.status === 'ok' || payload.status === 'queued' ? payload.status : payload.status === 'error' ? 'warn' : 'ok'
  });
}

function handleGuardian(payload) {
  store.guardian = {
    updatedAt: nowIso(),
    payload
  };
}

const client = mqtt.connect(MQTT_URL, {
  reconnectPeriod: 3000,
  connectTimeout: 5000,
  clientId: `espagent-dashboard-${Math.random().toString(16).slice(2, 10)}`
});

client.on('connect', () => {
  store.mqtt.connected = true;
  const topics = [
    `${TOPIC_PREFIX}/nodes/+/telemetry`,
    `${TOPIC_PREFIX}/nodes/+/state`,
    `${TOPIC_PREFIX}/agent/timeline`,
    `${TOPIC_PREFIX}/guardian/stateboard`,
    CHAT_REPLY_TOPIC,
    VOICE_STT_REQUEST_TOPIC,
    VOICE_STT_RESULT_TOPIC
  ];
  client.subscribe(topics, (error) => {
    if (error) {
      console.error('[dashboard] subscribe failed', error.message);
      return;
    }
    console.log('[dashboard] subscribed', topics);
  });
});

client.on('reconnect', () => {
  store.mqtt.connected = false;
});

client.on('close', () => {
  store.mqtt.connected = false;
});

client.on('error', (error) => {
  store.mqtt.connected = false;
  console.error('[dashboard] mqtt error', error.message);
});

client.on('message', (topic, buffer) => {
  const text = buffer.toString();
  const payload = safeJsonParse(text);
  store.mqtt.lastEventAt = nowIso();

  if (!payload) {
    return;
  }

  if (topic.endsWith('/telemetry')) {
    handleTelemetry(topic, payload);
    return;
  }
  if (topic.endsWith('/state')) {
    handleState(topic, payload);
    return;
  }
  if (topic.endsWith('/timeline')) {
    handleTimeline(payload);
    return;
  }
  if (topic === CHAT_REPLY_TOPIC) {
    const chatId = typeof payload.chat_id === 'string' ? payload.chat_id : '';
    const session = chatId ? chatSessions.get(chatId) : null;
    noteChatGatewayEvent(null);
    if (session?.downstream?.readyState === WebSocket.OPEN) {
      session.downstream.send(JSON.stringify(payload));
    }
    return;
  }
  if (topic === VOICE_STT_REQUEST_TOPIC) {
    const request = {
      type: 'stt_request',
      request_id: typeof payload.request_id === 'string' ? payload.request_id : `stt-${Date.now()}`,
      reply_channel: typeof payload.reply_channel === 'string' ? payload.reply_channel : 'web',
      reply_chat_id: typeof payload.reply_chat_id === 'string' ? payload.reply_chat_id : 'web_console_01',
      hint_text: typeof payload.hint_text === 'string' ? payload.hint_text : '',
      auto_route_reply: payload.auto_route_reply !== false
    };
    for (const session of wsSessions) {
      sendWsJson(session.downstream, request);
    }
    pushTimeline({
      time: new Date().toLocaleTimeString('zh-CN', { hour12: false }),
      stage: 'stt_request',
      source: payload.node_id || payload.role || 'coordinator_agent',
      target: 'dashboard_web',
      payload: request.hint_text || '语音转写请求',
      status: 'queued'
    });
    return;
  }
  if (topic === VOICE_STT_RESULT_TOPIC) {
    pushTimeline({
      time: new Date(Number(payload.ts_ms || Date.now())).toLocaleTimeString('zh-CN', { hour12: false }),
      stage: 'stt_result',
      source: payload.provider || 'voice_frontend',
      target: payload.reply_chat_id || 'agent_loop',
      payload: payload.transcript || '(empty transcript)',
      status: payload.status === 'error' ? 'warn' : 'ok'
    });
    return;
  }
  if (topic.endsWith('/stateboard')) {
    handleGuardian(payload);
  }
});

function sendJson(res, statusCode, payload) {
  res.writeHead(statusCode, {
    'Content-Type': 'application/json; charset=utf-8',
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET,POST,OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type'
  });
  res.end(JSON.stringify(payload));
}

const server = http.createServer((req, res) => {
  if (!req.url) {
    sendJson(res, 404, { error: 'missing url' });
    return;
  }

  if (req.method === 'OPTIONS') {
    sendJson(res, 204, {});
    return;
  }

  const url = new URL(req.url, `http://${req.headers.host || '127.0.0.1'}`);

  if (req.method === 'GET' && url.pathname === '/api/health') {
    sendJson(res, 200, {
      ok: true,
      mqtt: store.mqtt,
      nodes: store.nodes.size,
      telemetry: store.telemetry.size
    });
    return;
  }

  if (req.method === 'GET' && url.pathname === '/api/dashboard') {
    sendJson(res, 200, toDashboardPayload());
    return;
  }

  if (req.method === 'GET' && url.pathname === '/api/skills/runtime') {
    if (SKILLS_API_BASE) {
      fetchUpstreamRuntimeSkills()
        .then((skills) => {
          sendJson(res, 200, {
            skills,
            source: 'proxy'
          });
        })
        .catch((error) => {
          sendJson(res, 502, {
            skills: listRuntimeSkills(),
            source: 'proxy',
            error: error instanceof Error ? error.message : 'upstream runtime list failed'
          });
        });
      return;
    }

    sendJson(res, 200, {
      skills: listRuntimeSkills(),
      source: 'local_mock'
    });
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/skills') {
    let body = '';
    req.on('data', (chunk) => {
      body += chunk;
    });
    req.on('end', () => {
      const payload = safeJsonParse(body);
      const skills = Array.isArray(payload?.skills) ? payload.skills : [];
      sendJson(res, 200, { skills });
    });
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/skills/install') {
    let body = '';
    req.on('data', (chunk) => {
      body += chunk;
    });
    req.on('end', async () => {
      const payload = safeJsonParse(body);
      const skill = payload?.skill;
      const confirmed = Boolean(payload?.confirmed);
      const source = SKILLS_API_BASE ? 'proxy' : 'local_mock';

      if (!skill || typeof skill !== 'object') {
        sendJson(res, 400, {
          ok: false,
          source,
          message: 'missing skill payload',
          error: 'missing skill payload'
        });
        return;
      }
      if (!confirmed) {
        sendJson(res, 400, {
          ok: false,
          source,
          message: 'skill install requires confirmed=true',
          error: 'skill install requires confirmed=true'
        });
        return;
      }

      const runtimeName = normalizeSkillName(skill.name || skill.id);
      if (!runtimeName) {
        sendJson(res, 400, {
          ok: false,
          source,
          message: 'invalid runtime skill name',
          error: 'invalid runtime skill name'
        });
        return;
      }

      if (SKILLS_API_BASE) {
        try {
          const upstream = await fetch(`${SKILLS_API_BASE}/api/skills`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
              name: runtimeName,
              content: skillDraftToMarkdown(skill),
              confirmed: true
            })
          });
          const upstreamPayload = await upstream.json().catch(() => ({}));
          if (!upstream.ok || upstreamPayload?.ok === false) {
            sendJson(res, 502, {
              ok: false,
              source,
              message: 'runtime install failed on upstream gateway',
              error: upstreamPayload?.error || `HTTP ${upstream.status}`
            });
            return;
          }

          const installed = upstreamSkillToRuntimeRecord({
            name: runtimeName,
            title: skill.name || runtimeName,
            path: `/spiffs/skills/${runtimeName}.md`
          });
          installed.lastMessage = upstreamPayload?.message || 'proxied to runtime gateway';
          runtimeSkills.set(installed.runtimeName, installed);
          sendJson(res, 200, {
            ok: true,
            source,
            message: installed.lastMessage,
            skill: installed
          });
          return;
        } catch (error) {
          sendJson(res, 502, {
            ok: false,
            source,
            message: 'runtime install request failed',
            error: error instanceof Error ? error.message : 'runtime install request failed'
          });
          return;
        }
      }

      const installed = draftToRuntimeRecord(
        skill,
        'local_mock',
        'front-end runtime install simulated; ESP32 SPIFFS not yet connected'
      );
      runtimeSkills.set(installed.runtimeName, installed);
      sendJson(res, 200, {
        ok: true,
        source,
        message: installed.lastMessage,
        skill: installed
      });
    });
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/preferences') {
    let body = '';
    req.on('data', (chunk) => {
      body += chunk;
    });
    req.on('end', () => {
      const payload = safeJsonParse(body);
      sendJson(res, 200, payload && typeof payload === 'object' ? payload : {});
    });
    return;
  }

  sendJson(res, 404, { error: 'not found' });
});

const wsServer = new WebSocketServer({ noServer: true });
wsServer.on('connection', (socket) => {
  createChatSession(socket);
});

server.on('upgrade', (req, socket, head) => {
  const url = new URL(req.url || '/', `http://${req.headers.host || '127.0.0.1'}`);
  if (url.pathname !== CHAT_GATEWAY_PATH) {
    socket.destroy();
    return;
  }

  wsServer.handleUpgrade(req, socket, head, (client) => {
    wsServer.emit('connection', client, req);
  });
});

server.listen(PORT, '0.0.0.0', () => {
  console.log(`[dashboard] http://127.0.0.1:${PORT}`);
  console.log(`[dashboard] mqtt=${MQTT_URL} prefix=${TOPIC_PREFIX}`);
  console.log(`[dashboard] chat-gateway=${CHAT_GATEWAY_PATH} request=${CHAT_REQUEST_TOPIC} reply=${CHAT_REPLY_TOPIC}`);
});
