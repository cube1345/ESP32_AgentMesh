import http from 'node:http';
import { createHash, createHmac } from 'node:crypto';
import fs from 'node:fs/promises';
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { StringDecoder } from 'node:string_decoder';
import mqtt from 'mqtt';
import { WebSocketServer, WebSocket } from 'ws';
import { createHostCoordinatorAgent, sha256, signMeshCommand } from './host_coordinator_agent.mjs';
import { createHostAutomation } from './host_automation.mjs';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const REPO_ROOT = path.resolve(__dirname, '../..');
const PORT = Number(process.env.ESPAGENT_DASHBOARD_PORT || 4175);
const MQTT_HOST = process.env.ESPAGENT_MQTT_HOST || 'broker.emqx.io';
const MQTT_PORT = Number(process.env.ESPAGENT_MQTT_PORT || 1883);
const MQTT_PROTOCOL = process.env.ESPAGENT_MQTT_PROTOCOL || 'mqtt';
const TOPIC_PREFIX = (process.env.ESPAGENT_TOPIC_PREFIX || 'espagent/cube1345').replace(/\/+$/, '');
const MQTT_URL = `${MQTT_PROTOCOL}://${MQTT_HOST}:${MQTT_PORT}`;
const CHAT_REQUEST_TOPIC = `${TOPIC_PREFIX}/web/chat/request`;
const CHAT_REPLY_TOPIC = `${TOPIC_PREFIX}/web/chat/reply`;
const FEISHU_BRIDGE_ENABLED = process.env.ESPAGENT_FEISHU_BRIDGE_ENABLED === '1';
const FEISHU_BRIDGE_STATUSES = new Set(
  (process.env.ESPAGENT_FEISHU_BRIDGE_STATUSES || 'skipped_low_memory')
    .split(',')
    .map((item) => item.trim())
    .filter(Boolean)
);
const FEISHU_BRIDGE_DEDUP_MS = Number(process.env.ESPAGENT_FEISHU_BRIDGE_DEDUP_MS || 10 * 60 * 1000);
const FEISHU_BRIDGE_MAX_TEXT_BYTES = Number(process.env.ESPAGENT_FEISHU_BRIDGE_MAX_TEXT_BYTES || 700);
const FEISHU_BRIDGE_SKIP_QUEUED_ACK = process.env.ESPAGENT_FEISHU_BRIDGE_SKIP_QUEUED_ACK !== '0';
const FEISHU_BRIDGE_SECRET_FILE = process.env.ESPAGENT_FEISHU_BRIDGE_SECRET_FILE ||
  path.join(REPO_ROOT, 'main', 'espagent_secrets.h');
const FEISHU_TOKEN_URL = 'https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal';
const FEISHU_SEND_MSG_URL = 'https://open.feishu.cn/open-apis/im/v1/messages';
const SKILLS_API_BASE = (process.env.ESPAGENT_SKILLS_API_BASE || '').replace(/\/+$/, '');
const SKILLS_SERIAL_ENABLED = process.env.ESPAGENT_SKILLS_SERIAL_ENABLED === '1';
const SKILLS_SERIAL_PORT = process.env.ESPAGENT_SKILLS_SERIAL_PORT || '/dev/ttyUSB0';
const SKILLS_SERIAL_TIMEOUT_MS = Number(process.env.ESPAGENT_SKILLS_SERIAL_TIMEOUT_MS || 20000);
const SKILLS_SERIAL_READ_TIMEOUT_MS = Number(process.env.ESPAGENT_SKILLS_SERIAL_READ_TIMEOUT_MS || 30000);
const SKILLS_SERIAL_WRITE_TIMEOUT_MS = Number(process.env.ESPAGENT_SKILLS_SERIAL_WRITE_TIMEOUT_MS || 60000);
const SKILLS_SERIAL_LIST_CACHE_MS = Number(process.env.ESPAGENT_SKILLS_SERIAL_LIST_CACHE_MS || 30000);
const SKILLS_SERIAL_MAX_CONTENT_BYTES = Number(process.env.ESPAGENT_SKILLS_SERIAL_MAX_CONTENT_BYTES || 4096);
const SKILLS_MQTT_ENABLED = process.env.ESPAGENT_SKILLS_MQTT_ENABLED !== '0';
const SKILLS_MQTT_TIMEOUT_MS = Number(process.env.ESPAGENT_SKILLS_MQTT_TIMEOUT_MS || 15000);
const SKILLS_MQTT_MAX_CONTENT_BYTES = Number(process.env.ESPAGENT_SKILLS_MQTT_MAX_CONTENT_BYTES || 1100);
const MESH_AUTH_KEY = process.env.ESPAGENT_MESH_AUTH_KEY || '';
const SKILLS_MQTT_REQUEST_TOPIC = `${TOPIC_PREFIX}/runtime/skills/request`;
const SKILLS_MQTT_REPLY_TOPIC = `${TOPIC_PREFIX}/runtime/skills/reply`;
const DEVICE_SERIAL_MAX_CONTENT_BYTES = Number(process.env.ESPAGENT_DEVICE_SERIAL_MAX_CONTENT_BYTES || 4096);
const DEVICE_SERIAL_LIST_CACHE_MS = Number(process.env.ESPAGENT_DEVICE_SERIAL_LIST_CACHE_MS || 30000);
const SERIAL_CMD_PATH = process.env.ESPAGENT_SERIAL_CMD_PATH ||
  path.join(REPO_ROOT, 'tools', 'serial_cmd.py');
const LOCAL_SPIFFS_SKILLS_DIR = path.join(REPO_ROOT, 'spiffs_data', 'skills');
const CHAT_GATEWAY_PATH = '/ws';
const HOST_AGENT_ENABLED = process.env.ESPAGENT_HOST_AGENT_ENABLED === '1';
const HOST_AUTOMATION_ENABLED = process.env.ESPAGENT_HOST_AUTOMATION_ENABLED === '1';
const HOST_AGENT_POLICY_TIMEOUT_MS = Number(process.env.ESPAGENT_HOST_AGENT_POLICY_TIMEOUT_MS || 15000);
const runtimeSkills = new Map();
let runtimeSkillsCacheAt = 0;
const pendingSkillRequests = new Map();
let serialCommandTail = Promise.resolve();
const runtimeDeviceManifests = new Map();
let runtimeDeviceManifestsCacheAt = 0;
const chatSessions = new Map();
const wsSessions = new Set();
const feishuBridgeSeen = new Map();
const hostAgentPending = new Map();
let feishuBridgeCredentialsPromise = null;
let feishuBridgeToken = null;
const MAX_TIMELINE_EVENTS = 120;
const requestedEnvironmentHistoryPoints = Number(process.env.ESPAGENT_ENV_HISTORY_POINTS || 360);
const MAX_ENVIRONMENT_HISTORY_POINTS = Number.isFinite(requestedEnvironmentHistoryPoints)
  ? Math.min(1440, Math.max(60, Math.trunc(requestedEnvironmentHistoryPoints)))
  : 360;

function isHighValueTimelineEntry(entry) {
  const text = `${entry?.stage || ''} ${entry?.source || ''} ${entry?.target || ''} ${entry?.payload || ''}`.toLowerCase();
  return text.includes('sandbox') ||
    text.includes('guardian') ||
    text.includes('policy') ||
    text.includes('denied') ||
    text.includes('blocked') ||
    entry?.status === 'warn';
}

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

function stableNameSuffix(value) {
  const text = String(value || 'skill');
  let hash = 5381;
  for (let index = 0; index < text.length; index += 1) {
    hash = ((hash << 5) + hash) ^ text.charCodeAt(index);
  }
  return (hash >>> 0).toString(36).slice(0, 8);
}

function contentSha256(content) {
  return createHash('sha256').update(String(content || ''), 'utf8').digest('hex');
}

function attachContentHash(record) {
  if (record && typeof record.content === 'string') {
    record.contentHash = contentSha256(record.content);
  }
  return record;
}

function runtimeSkillName(skill) {
  if (typeof skill === 'string') {
    return normalizeSkillName(skill) || `skill_${stableNameSuffix(skill)}`;
  }
  const fromRuntimeName = normalizeSkillName(skill?.runtimeName);
  if (fromRuntimeName) return fromRuntimeName;
  const fromName = normalizeSkillName(skill?.name);
  if (fromName) return fromName;
  const fromId = normalizeSkillName(skill?.id);
  if (fromId) return fromId;
  return `skill_${stableNameSuffix(JSON.stringify(skill || {}))}`;
}

function normalizeDeviceName(name) {
  return String(name || '')
    .trim()
    .replace(/[^A-Za-z0-9_-]+/g, '_')
    .replace(/^_+|_+$/g, '')
    .slice(0, 47);
}

function runtimeDeviceName(device) {
  if (typeof device === 'string') {
    return normalizeDeviceName(device) || `device_${stableNameSuffix(device)}`;
  }
  const fromManifest = normalizeDeviceName(device?.manifestName);
  if (fromManifest) return fromManifest;
  const fromName = normalizeDeviceName(device?.name);
  if (fromName) return fromName;
  const fromId = normalizeDeviceName(device?.id);
  if (fromId) return fromId;
  return `device_${stableNameSuffix(JSON.stringify(device || {}))}`;
}

function normalizeRuntimeSkillContent(content, title) {
  const text = String(content || '').trimEnd();
  const firstNonSpace = text.trimStart();
  if (firstNonSpace.startsWith('# ')) {
    return `${text}\n`;
  }
  return `# ${title || 'Runtime Skill'}\n\n${text}\n`;
}

function draftToRuntimeRecord(skill, source = 'serial', message = 'runtime skill installed') {
  const runtimeName = runtimeSkillName(skill);
  const content = typeof skill?.content === 'string'
    ? skill.content
    : skillDraftToMarkdown(skill);
  return attachContentHash({
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
    content,
    lastMessage: message
  });
}

function spiffsSkillPath(name) {
  return `/spiffs/skills/${runtimeSkillName(name)}.md`;
}

function localSpiffsSkillPath(name) {
  return path.join(LOCAL_SPIFFS_SKILLS_DIR, `${runtimeSkillName(name)}.md`);
}

function spiffsDevicePath(name) {
  return `/spiffs/devices/${runtimeDeviceName(name)}.json`;
}

function spiffsDeviceSignaturePath(name) {
  return `${spiffsDevicePath(name)}.sha256`;
}

function normalizeDeviceManifestContent(content, expectedName) {
  const text = String(content || '').trim();
  if (!text) {
    throw new Error('device manifest content is empty');
  }
  const parsed = JSON.parse(text);
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
    throw new Error('device manifest must be a JSON object');
  }
  const manifestName = normalizeDeviceName(parsed.name);
  if (!manifestName) {
    throw new Error('device manifest name is required');
  }
  if (expectedName && manifestName !== expectedName) {
    throw new Error(`manifest name=${manifestName} does not match selected file=${expectedName}`);
  }
  return `${JSON.stringify(parsed, null, 2)}\n`;
}

function upstreamSkillToRuntimeRecord(item) {
  const runtimeName = normalizeSkillName(item?.name || item?.title || 'unknown');
  return attachContentHash({
    id: runtimeName,
    runtimeName,
    title: item?.title || item?.name || runtimeName,
    path: item?.path || `/spiffs/skills/${runtimeName}.md`,
    source: 'proxy',
    installedAt: nowIso(),
    cacheState: 'unknown',
    status: 'installed',
    scope: item?.scope || 'runtime',
    enabled: true,
    content: typeof item?.content === 'string' ? item.content : '',
    lastMessage: typeof item?.size_bytes === 'number'
      ? `upstream listed, ${item.size_bytes} bytes`
      : 'upstream listed'
  });
}

function serialSkillToRuntimeRecord(item) {
  const runtimeName = normalizeSkillName(item?.name || item?.title || 'unknown');
  return attachContentHash({
    id: runtimeName,
    runtimeName,
    title: item?.title || item?.name || runtimeName,
    path: item?.path || spiffsSkillPath(runtimeName),
    source: 'serial',
    installedAt: item?.installedAt || nowIso(),
    cacheState: 'unknown',
    status: 'installed',
    scope: item?.scope || 'runtime',
    enabled: true,
    content: typeof item?.content === 'string' ? item.content : '',
    lastMessage: typeof item?.size_bytes === 'number'
      ? `listed from ESP32 SPIFFS, ${item.size_bytes} bytes`
      : 'listed from ESP32 SPIFFS'
  });
}

function mqttSkillToRuntimeRecord(item) {
  const record = upstreamSkillToRuntimeRecord(item);
  return {
    ...record,
    source: 'mqtt',
    lastMessage: typeof item?.size_bytes === 'number'
      ? `listed wirelessly from ESP32 SPIFFS, ${item.size_bytes} bytes`
      : 'listed wirelessly from ESP32 SPIFFS'
  };
}

function listRuntimeSkills() {
  return Array.from(runtimeSkills.values()).sort((a, b) => {
    return String(b.installedAt).localeCompare(String(a.installedAt));
  });
}

function deviceRecordFromManifest(name, content, source = 'local_mock', message = 'listed') {
  const parsed = safeJsonParse(content) || {};
  const manifestName = runtimeDeviceName(parsed?.name || name);
  return attachContentHash({
    id: manifestName,
    manifestName,
    title: parsed?.name || manifestName,
    protocol: parsed?.protocol || 'unknown',
    role: parsed?.role || 'unknown',
    risk: parsed?.risk || 'unknown',
    path: spiffsDevicePath(manifestName),
    signaturePath: spiffsDeviceSignaturePath(manifestName),
    source,
    installedAt: nowIso(),
    status: 'installed',
    content,
    lastMessage: message
  });
}

function listRuntimeDeviceManifests() {
  return Array.from(runtimeDeviceManifests.values()).sort((a, b) => {
    return String(a.manifestName).localeCompare(String(b.manifestName));
  });
}

function invalidateRuntimeDeviceManifestCache() {
  runtimeDeviceManifestsCacheAt = 0;
}

function invalidateRuntimeSkillCache() {
  runtimeSkillsCacheAt = 0;
}

function executeSerialCommand(command, timeoutMs) {
  return new Promise((resolve, reject) => {
    const args = [
      SERIAL_CMD_PATH,
      SKILLS_SERIAL_PORT,
      command,
      '--timeout',
      String(Math.max(1, Math.ceil(timeoutMs / 1000))),
      '--settle',
      '0.2'
    ];
    const child = spawn('python3', args, {
      cwd: REPO_ROOT,
      stdio: ['ignore', 'pipe', 'pipe']
    });
    let stdout = '';
    let stderr = '';
    const stdoutDecoder = new StringDecoder('utf8');
    const stderrDecoder = new StringDecoder('utf8');
    const timer = setTimeout(() => {
      child.kill('SIGTERM');
      reject(new Error(`serial command timed out after ${timeoutMs}ms`));
    }, timeoutMs + 3000);

    child.stdout.on('data', (chunk) => {
      stdout += stdoutDecoder.write(chunk);
    });
    child.stderr.on('data', (chunk) => {
      stderr += stderrDecoder.write(chunk);
    });
    child.on('error', (error) => {
      clearTimeout(timer);
      reject(error);
    });
    child.on('close', (code) => {
      clearTimeout(timer);
      stdout += stdoutDecoder.end();
      stderr += stderrDecoder.end();
      if (code === 0) {
        resolve(stdout);
        return;
      }
      reject(new Error((stderr || stdout || `serial command exited ${code}`).trim()));
    });
  });
}

function runSerialCommand(command, timeoutMs = SKILLS_SERIAL_TIMEOUT_MS) {
  // One physical UART cannot serve overlapping list/read/write processes.
  const operation = serialCommandTail.then(
    () => executeSerialCommand(command, timeoutMs),
    () => executeSerialCommand(command, timeoutMs)
  );
  serialCommandTail = operation.catch(() => undefined);
  return operation;
}

function extractSerialPayload(output) {
  const marker = '===== serial output end =====';
  const index = output.indexOf(marker);
  return (index >= 0 ? output.slice(index + marker.length) : output).trim();
}

function parseSerialSkillList(output) {
  const payload = extractSerialPayload(output);
  const records = [];
  const seen = new Set();
  const fileRegex = /\/spiffs\/skills\/([A-Za-z0-9_-]+)\.md(?:\s+\((\d+)\s+bytes\))?/g;
  let match;
  while ((match = fileRegex.exec(payload)) !== null) {
    const runtimeName = normalizeSkillName(match[1]);
    if (!runtimeName || seen.has(runtimeName)) continue;
    seen.add(runtimeName);
    records.push(serialSkillToRuntimeRecord({
      name: runtimeName,
      title: runtimeName,
      path: spiffsSkillPath(runtimeName),
      size_bytes: match[2] ? Number(match[2]) : undefined
    }));
  }

  if (records.length === 0) {
    for (const line of payload.split(/\r?\n/)) {
      const item = line.match(/-\s+\*\*(.+?)\*\*.*read_file\s+(\/spiffs\/skills\/([A-Za-z0-9_-]+)\.md)/);
      if (!item) continue;
      const runtimeName = normalizeSkillName(item[3] || item[1]);
      if (!runtimeName || seen.has(runtimeName)) continue;
      seen.add(runtimeName);
      records.push(serialSkillToRuntimeRecord({
        name: runtimeName,
        title: item[1].trim(),
        path: item[2]
      }));
    }
  }
  return records;
}

function parseSerialDeviceManifestList(output) {
  const payload = extractSerialPayload(output);
  const records = [];
  const seen = new Set();
  const fileRegex = /\/spiffs\/devices\/([A-Za-z0-9_-]+)\.json(?=\s|$|\()/g;
  let match;
  while ((match = fileRegex.exec(payload)) !== null) {
    const manifestName = normalizeDeviceName(match[1]);
    if (!manifestName || seen.has(manifestName)) continue;
    seen.add(manifestName);
    records.push(deviceRecordFromManifest(manifestName, '{}\n', 'serial', 'listed from ESP32 SPIFFS'));
  }
  return records;
}

function extractSerialToolOutput(output) {
  const payload = extractSerialPayload(output);
  const lines = payload.split(/\r?\n/);
  const statusIndex = lines.findIndex((line) => /^tool_exec status:\s*/.test(line.trim()));
  if (statusIndex < 0) {
    throw new Error(payload || 'missing tool_exec status');
  }
  const statusLine = lines[statusIndex];
  if (!/tool_exec status:\s*ESP_OK/.test(statusLine)) {
    throw new Error(payload || 'ESP32 tool_exec failed');
  }
  const bodyLines = lines.slice(statusIndex + 1);
  const promptIndex = bodyLines.findIndex((line) => /^\s*ESPAgent>/.test(line));
  if (promptIndex >= 0) {
    bodyLines.splice(promptIndex);
  }
  return bodyLines.join('\n').trimEnd();
}

async function fetchUpstreamSkillContent(record) {
  const response = await fetch(`${SKILLS_API_BASE}/api/skills?name=${encodeURIComponent(record.runtimeName)}`);
  const payload = await response.json().catch(() => ({}));
  if (!response.ok || payload?.ok === false) {
    throw new Error(payload?.error || `HTTP ${response.status}`);
  }

  const detail = payload?.skill && typeof payload.skill === 'object'
    ? payload.skill
    : payload;
  return upstreamSkillToRuntimeRecord({
    ...record,
    ...detail,
    name: detail?.name || record.runtimeName,
    title: detail?.title || record.title,
    path: detail?.path || record.path,
    content: typeof detail?.content === 'string' ? detail.content : record.content
  });
}

async function fetchSerialSkillContent(record) {
  const output = await runSerialCommand(`tool_exec read_file ${JSON.stringify({
    path: record.path || spiffsSkillPath(record.runtimeName)
  })}`, SKILLS_SERIAL_READ_TIMEOUT_MS);
  const rawContent = extractSerialToolOutput(output);
  let content = rawContent ? `${rawContent.trimEnd()}\n` : rawContent;
  if (content.includes('�')) {
    try {
      content = await fs.readFile(localSpiffsSkillPath(record.runtimeName), 'utf8');
    } catch {
      // Keep the device response when the skill only exists on the board.
    }
  }
  return serialSkillToRuntimeRecord({
    ...record,
    name: record.runtimeName,
    content,
    title: content.trimStart().startsWith('# ')
      ? content.trimStart().split(/\r?\n/, 1)[0].replace(/^#\s+/, '').trim()
      : record.title
  });
}

function isMissingRuntimeSkillError(error) {
  const message = error instanceof Error ? error.message : String(error || '');
  return /not found|HTTP 404|file not found|ESP_ERR_NOT_FOUND/i.test(message);
}

async function fetchUpstreamSkillContentOrNull(runtimeName) {
  try {
    return await fetchUpstreamSkillContent({
      runtimeName,
      title: runtimeName,
      path: spiffsSkillPath(runtimeName)
    });
  } catch (error) {
    if (isMissingRuntimeSkillError(error)) {
      return null;
    }
    throw error;
  }
}

async function fetchSerialSkillContentOrNull(runtimeName) {
  try {
    return await fetchSerialSkillContent({
      runtimeName,
      title: runtimeName,
      path: spiffsSkillPath(runtimeName)
    });
  } catch (error) {
    if (isMissingRuntimeSkillError(error)) {
      return null;
    }
    throw error;
  }
}

function buildUnchangedSkillMessage(runtimeName, hash) {
  return `unchanged: ${runtimeName} already matches SPIFFS sha256=${hash.slice(0, 12)}; skipped write`;
}

async function upsertUpstreamRuntimeSkillIfChanged(runtimeName, content) {
  // Cloud-side change detection avoids rewriting SPIFFS when the edited skill is byte-identical.
  const desiredHash = contentSha256(content);
  const existing = await fetchUpstreamSkillContentOrNull(runtimeName);
  if (existing?.content && existing.contentHash === desiredHash) {
    return {
      skipped: true,
      message: buildUnchangedSkillMessage(runtimeName, desiredHash)
    };
  }

  const upstream = await fetch(`${SKILLS_API_BASE}/api/skills`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      name: runtimeName,
      content,
      confirmed: true
    })
  });
  const payload = await upstream.json().catch(() => ({}));
  if (!upstream.ok || payload?.ok === false) {
    throw new Error(payload?.error || `HTTP ${upstream.status}`);
  }
  return {
    skipped: false,
    message: payload?.message || 'runtime skill saved through upstream gateway'
  };
}

async function upsertSerialRuntimeSkillContentIfChanged(runtimeName, content) {
  const desiredHash = contentSha256(content);
  const existing = await fetchSerialSkillContentOrNull(runtimeName);
  if (existing?.content && existing.contentHash === desiredHash) {
    return buildUnchangedSkillMessage(runtimeName, desiredHash);
  }
  return upsertSerialRuntimeSkillContent(runtimeName, content);
}

function runtimeSkillRequestSignature(request) {
  if (!MESH_AUTH_KEY) return '';
  const fields = [
    request.request_id,
    request.operation,
    request.name,
    request.sha256,
    request.ts_ms
  ];
  if (request.operation === 'list' || request.operation === 'get') {
    fields.push(request.offset || 0);
  }
  const canonical = fields.join('\n');
  return createHmac('sha256', MESH_AUTH_KEY).update(canonical, 'utf8').digest('hex');
}

function runMqttRuntimeSkillRequest(operation, {
  runtimeName = '',
  content = '',
  sha256 = '',
  confirmed = false,
  offset = 0
} = {}) {
  if (!SKILLS_MQTT_ENABLED) {
    return Promise.reject(new Error('MQTT runtime skill transport disabled'));
  }
  if (!client.connected) {
    return Promise.reject(new Error('MQTT is not connected'));
  }

  const suffix = `${Date.now().toString(36)}-${Math.random().toString(16).slice(2, 8)}`;
  const requestId = `skill-${suffix}`;
  const request = {
    request_id: requestId,
    operation,
    name: runtimeName,
    content,
    sha256,
    confirmed,
    offset,
    ts_ms: Date.now(),
  };
  const signature = runtimeSkillRequestSignature(request);
  if (signature) request.signature = signature;

  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pendingSkillRequests.delete(requestId);
      reject(new Error(
        `MQTT runtime skill request timed out after ${SKILLS_MQTT_TIMEOUT_MS}ms ` +
        `(operation=${operation}, offset=${offset})`
      ));
    }, SKILLS_MQTT_TIMEOUT_MS);
    pendingSkillRequests.set(requestId, {
      resolve,
      reject,
      timer
    });
    client.publish(SKILLS_MQTT_REQUEST_TOPIC, JSON.stringify(request), { qos: 0 }, (error) => {
      if (!error) return;
      clearTimeout(timer);
      pendingSkillRequests.delete(requestId);
      reject(error);
    });
  });
}

async function upsertMqttRuntimeSkillContentIfChanged(runtimeName, content) {
  const contentBytes = Buffer.byteLength(content, 'utf8');
  if (contentBytes > SKILLS_MQTT_MAX_CONTENT_BYTES) {
    throw new Error(
      `runtime skill content is ${contentBytes} bytes; MQTT limit is ${SKILLS_MQTT_MAX_CONTENT_BYTES} bytes`
    );
  }
  const desiredHash = contentSha256(content);
  const cached = runtimeSkills.get(runtimeName);
  if (cached?.contentHash === desiredHash) {
    return buildUnchangedSkillMessage(runtimeName, desiredHash);
  }

  const result = await runMqttRuntimeSkillRequest('upsert', {
    runtimeName,
    content,
    sha256: desiredHash,
    confirmed: true
  });
  const message = String(result?.message || '');
  if (!result?.ok || !message.startsWith('OK:')) {
    throw new Error(message || 'MQTT runtime skill install failed');
  }
  return message;
}

async function fetchMqttRuntimeSkillData(operation, runtimeName = '') {
  let offset = 0;
  const chunks = [];
  for (let page = 0; page < 64; page += 1) {
    const result = await runMqttRuntimeSkillRequest(operation, {
      runtimeName,
      offset
    });
    if (!result?.ok || typeof result.data !== 'string') {
      throw new Error(result?.message || `MQTT runtime skill ${operation} failed`);
    }
    if (Number(result.offset) !== offset) {
      throw new Error(`MQTT runtime skill ${operation} offset mismatch`);
    }
    chunks.push(result.data);
    if (result.final === true) {
      return chunks.join('');
    }
    const nextOffset = Number(result.next_offset);
    if (!Number.isInteger(nextOffset) || nextOffset <= offset) {
      throw new Error(`MQTT runtime skill ${operation} returned invalid next_offset`);
    }
    offset = nextOffset;
  }
  throw new Error(`MQTT runtime skill ${operation} exceeded 64 pages`);
}

function parseMqttRuntimeSkillData(data, operation) {
  const payload = safeJsonParse(data);
  if (!payload || payload.ok === false) {
    throw new Error(payload?.error || `invalid MQTT runtime skill ${operation} response`);
  }
  return payload;
}

async function fetchMqttRuntimeSkills() {
  const data = await fetchMqttRuntimeSkillData('list');
  const payload = parseMqttRuntimeSkillData(data, 'list');
  if (!Array.isArray(payload.skills)) {
    throw new Error('MQTT runtime skill list response has no skills array');
  }

  const previous = new Map(runtimeSkills);
  runtimeSkills.clear();
  for (const item of payload.skills) {
    let record = mqttSkillToRuntimeRecord(item);
    const cached = previous.get(record.runtimeName);
    if (!record.content && cached?.content) {
      record = attachContentHash({
        ...record,
        content: cached.content,
        title: cached.title || record.title
      });
    }
    runtimeSkills.set(record.runtimeName, record);
  }
  runtimeSkillsCacheAt = Date.now();
  return listRuntimeSkills();
}

async function fetchMqttRuntimeSkillContent(runtimeName) {
  const data = await fetchMqttRuntimeSkillData('get', runtimeName);
  const payload = parseMqttRuntimeSkillData(data, 'get');
  if (!payload.skill || typeof payload.skill !== 'object') {
    throw new Error('MQTT runtime skill get response has no skill object');
  }
  return mqttSkillToRuntimeRecord(payload.skill);
}

async function deleteMqttRuntimeSkill(runtimeName) {
  const result = await runMqttRuntimeSkillRequest('delete', {
    runtimeName,
    confirmed: true
  });
  if (!result?.ok) {
    throw new Error(result?.message || 'MQTT runtime skill delete failed');
  }
  runtimeSkills.delete(runtimeName);
  invalidateRuntimeSkillCache();
  return result.message || `OK: skill ${runtimeName} deleted`;
}

async function fetchUpstreamRuntimeSkills() {
  const response = await fetch(`${SKILLS_API_BASE}/api/skills`);
  const payload = await response.json().catch(() => ({}));
  if (!response.ok || payload?.ok === false || !Array.isArray(payload?.skills)) {
    throw new Error(payload?.error || `HTTP ${response.status}`);
  }

  const previous = new Map(runtimeSkills);
  runtimeSkills.clear();
  for (const item of payload.skills) {
    let record = upstreamSkillToRuntimeRecord(item);
    const cached = previous.get(record.runtimeName);
    if (!record.content && cached?.content) {
      record = attachContentHash({ ...record, content: cached.content, title: cached.title || record.title });
    }
    runtimeSkills.set(record.runtimeName, record);
  }
  runtimeSkillsCacheAt = Date.now();
  return listRuntimeSkills();
}

async function fetchSerialRuntimeSkills() {
  if (!SKILLS_SERIAL_ENABLED) {
    return listRuntimeSkills();
  }
  if (runtimeSkillsCacheAt && Date.now() - runtimeSkillsCacheAt < SKILLS_SERIAL_LIST_CACHE_MS) {
    return listRuntimeSkills();
  }
  const output = await runSerialCommand('tool_exec list_dir {"prefix":"/spiffs/skills/"}');
  const skills = parseSerialSkillList(output);
  const previous = new Map(runtimeSkills);
  runtimeSkills.clear();
  for (const item of skills) {
    let record = item;
    const cached = previous.get(record.runtimeName);
    if (cached?.content) {
      record = attachContentHash({ ...record, content: cached.content, title: cached.title || record.title });
    }
    runtimeSkills.set(record.runtimeName, record);
  }
  runtimeSkillsCacheAt = Date.now();
  return listRuntimeSkills();
}

async function upsertSerialRuntimeSkillContent(runtimeName, content) {
  if (!SKILLS_SERIAL_ENABLED) {
    throw new Error('serial runtime skill gateway disabled');
  }
  const contentBytes = Buffer.byteLength(content, 'utf8');
  if (contentBytes > SKILLS_SERIAL_MAX_CONTENT_BYTES) {
    throw new Error(
      `runtime skill content is ${contentBytes} bytes; serial gateway limit is ${SKILLS_SERIAL_MAX_CONTENT_BYTES} bytes`
    );
  }
  const command = `tool_exec write_file ${JSON.stringify({
    path: spiffsSkillPath(runtimeName),
    content,
    confirmed: true
  })}`;
  const output = await runSerialCommand(command, SKILLS_SERIAL_WRITE_TIMEOUT_MS);
  const payload = extractSerialPayload(output);
  if (!/tool_exec status:\s*ESP_OK|OK:/m.test(payload)) {
    throw new Error(payload || 'ESP32 write_file did not report success');
  }
  invalidateRuntimeSkillCache();
  return payload.split(/\r?\n/).find((line) => line.includes('OK:')) ||
    `saved to ${spiffsSkillPath(runtimeName)}`;
}

async function fetchSerialDeviceManifestContent(record) {
  const output = await runSerialCommand(`tool_exec read_file ${JSON.stringify({
    path: record.path || spiffsDevicePath(record.manifestName)
  })}`);
  const content = extractSerialToolOutput(output);
  return deviceRecordFromManifest(record.manifestName, content, 'serial', 'loaded from ESP32 SPIFFS');
}

async function fetchSerialDeviceManifestContentOrNull(manifestName) {
  try {
    return await fetchSerialDeviceManifestContent({
      manifestName,
      path: spiffsDevicePath(manifestName)
    });
  } catch (error) {
    if (isMissingRuntimeSkillError(error)) {
      return null;
    }
    throw error;
  }
}

async function fetchSerialDeviceManifests() {
  if (!SKILLS_SERIAL_ENABLED) {
    return listRuntimeDeviceManifests();
  }
  if (runtimeDeviceManifestsCacheAt &&
      Date.now() - runtimeDeviceManifestsCacheAt < DEVICE_SERIAL_LIST_CACHE_MS) {
    return listRuntimeDeviceManifests();
  }
  const output = await runSerialCommand('tool_exec list_dir {"prefix":"/spiffs/devices/"}');
  const devices = parseSerialDeviceManifestList(output);
  runtimeDeviceManifests.clear();
  for (const item of devices) {
    let record = item;
    try {
      record = await fetchSerialDeviceManifestContent(item);
    } catch (error) {
      record.lastMessage = error instanceof Error ? error.message : 'content unavailable';
    }
    runtimeDeviceManifests.set(record.manifestName, record);
  }
  runtimeDeviceManifestsCacheAt = Date.now();
  return listRuntimeDeviceManifests();
}

async function fetchLocalDeviceManifests() {
  const deviceDir = path.join(REPO_ROOT, 'spiffs_data', 'devices');
  const entries = await fs.readdir(deviceDir, { withFileTypes: true });
  runtimeDeviceManifests.clear();
  for (const entry of entries) {
    if (!entry.isFile() || !entry.name.endsWith('.json')) continue;
    const manifestName = entry.name.replace(/\.json$/, '');
    const content = await fs.readFile(path.join(deviceDir, entry.name), 'utf8');
    runtimeDeviceManifests.set(
      manifestName,
      deviceRecordFromManifest(manifestName, content, 'local_mock', 'loaded from repository spiffs_data')
    );
  }
  runtimeDeviceManifestsCacheAt = Date.now();
  return listRuntimeDeviceManifests();
}

function buildUnchangedDeviceMessage(manifestName, hash) {
  return `unchanged: ${manifestName} already matches SPIFFS sha256=${hash.slice(0, 12)}; skipped write`;
}

async function upsertSerialDeviceManifestContent(manifestName, content) {
  if (!SKILLS_SERIAL_ENABLED) {
    throw new Error('serial device manifest gateway disabled');
  }
  const contentBytes = Buffer.byteLength(content, 'utf8');
  if (contentBytes > DEVICE_SERIAL_MAX_CONTENT_BYTES) {
    throw new Error(
      `device manifest content is ${contentBytes} bytes; serial gateway limit is ${DEVICE_SERIAL_MAX_CONTENT_BYTES} bytes`
    );
  }
  const hash = contentSha256(content);
  const writeManifest = `tool_exec write_file ${JSON.stringify({
    path: spiffsDevicePath(manifestName),
    content,
    confirmed: true
  })}`;
  const manifestOutput = await runSerialCommand(writeManifest);
  const manifestPayload = extractSerialPayload(manifestOutput);
  if (!/tool_exec status:\s*ESP_OK|OK:/m.test(manifestPayload)) {
    throw new Error(manifestPayload || 'ESP32 write_file did not report success for manifest');
  }

  const writeSignature = `tool_exec write_file ${JSON.stringify({
    path: spiffsDeviceSignaturePath(manifestName),
    content: `${hash}\n`,
    confirmed: true
  })}`;
  const signatureOutput = await runSerialCommand(writeSignature);
  const signaturePayload = extractSerialPayload(signatureOutput);
  if (!/tool_exec status:\s*ESP_OK|OK:/m.test(signaturePayload)) {
    throw new Error(signaturePayload || 'ESP32 write_file did not report success for manifest signature');
  }
  invalidateRuntimeDeviceManifestCache();
  return `saved ${spiffsDevicePath(manifestName)} and sha256=${hash.slice(0, 12)}`;
}

async function upsertSerialDeviceManifestContentIfChanged(manifestName, content) {
  const desiredHash = contentSha256(content);
  const existing = await fetchSerialDeviceManifestContentOrNull(manifestName);
  if (existing?.content && existing.contentHash === desiredHash) {
    return buildUnchangedDeviceMessage(manifestName, desiredHash);
  }
  return upsertSerialDeviceManifestContent(manifestName, content);
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
    feishuBridge: {
      enabled: FEISHU_BRIDGE_ENABLED,
      statuses: Array.from(FEISHU_BRIDGE_STATUSES),
      sent: 0,
      skipped: 0,
      failed: 0,
      lastEventAt: null,
      lastError: null
    },
    nodes: new Map(),
    telemetry: new Map(),
    environmentHistory: [],
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
  while (store.timeline.length > MAX_TIMELINE_EVENTS) {
    const removableIndex = store.timeline
      .map((item, index) => ({ item, index }))
      .reverse()
      .find(({ item }) => item.stage === 'node_state' && !isHighValueTimelineEntry(item))?.index;
    if (typeof removableIndex === 'number') {
      store.timeline.splice(removableIndex, 1);
    } else {
      store.timeline.pop();
    }
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
    environmentHistory: store.environmentHistory,
    skills: defaultSkills,
    preferences,
    flows,
    mqtt: store.mqtt,
    chatGateway: store.chatGateway,
    hostAgent: {
      enabled: hostAgent.enabled,
      role: 'coordinator_agent',
      maxActions: hostAgent.maxActions,
      maxRounds: hostAgent.maxRounds,
      automation: hostAutomation.snapshot()
    },
    feishuBridge: store.feishuBridge,
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

      if (HOST_AGENT_ENABLED) {
        void hostAgent.run({ chatId, channel: 'websocket', content: payload.content, approvalRequired: true })
          .then((result) => {
            if (session.downstream.readyState === WebSocket.OPEN) {
              session.downstream.send(JSON.stringify({
                type: 'response',
                chat_id: chatId,
                content: result.content,
                trace_id: result.traceId,
                action_count: result.actionCount
              }));
            }
          })
          .catch((error) => {
            noteChatGatewayEvent(error instanceof Error ? error.message : String(error));
            if (session.downstream.readyState === WebSocket.OPEN) {
              session.downstream.send(JSON.stringify({
                type: 'system',
                chat_id: chatId,
                content: `上位机 Coordinator 处理失败：${error instanceof Error ? error.message : String(error)}`
              }));
            }
          });
        return;
      }

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

  const updatedAt = nowIso();
  store.telemetry.set(nodeId, {
    nodeId,
    role,
    payload,
    updatedAt
  });

  if (role === 'sensor_agent') {
    const numberOrNull = (value) => {
      if (value === null || value === undefined || value === '') return null;
      const parsed = Number(value);
      return Number.isFinite(parsed) ? parsed : null;
    };
    store.environmentHistory.push({
      timestamp: updatedAt,
      nodeId,
      temperatureC: numberOrNull(payload.temp),
      humidityPercent: numberOrNull(payload.humidity),
      eco2Ppm: numberOrNull(payload.co2),
      tvocPpb: numberOrNull(payload.tvoc),
      lightLux: numberOrNull(payload.light_lux),
      presence: typeof payload.present === 'boolean'
        ? (payload.present ? 1 : 0)
        : numberOrNull(payload.present)
    });
    if (store.environmentHistory.length > MAX_ENVIRONMENT_HISTORY_POINTS) {
      store.environmentHistory.splice(
        0,
        store.environmentHistory.length - MAX_ENVIRONMENT_HISTORY_POINTS
      );
    }
    hostAutomation.onTelemetry({ nodeId, role, payload, updatedAt });
  }

  pushTimeline({
    time: new Date().toLocaleTimeString('zh-CN', { hour12: false }),
    stage: 'telemetry',
    source: nodeId,
    target: 'dashboard',
    payload: `temp=${payload.temp ?? '-'} humidity=${payload.humidity ?? '-'} co2=${payload.co2 ?? '-'} lux=${payload.light_lux ?? '-'}`,
    status: 'ok',
    eventId: `telemetry-${nodeId}-${Date.now()}`,
    nodeId,
    role,
    phase: 'sensor',
    tsMs: Date.now()
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
    status: payload.state === 'online' ? 'ok' : 'warn',
    eventId: `state-${nodeId}-${Date.now()}`,
    nodeId,
    role,
    phase: 'state',
    tsMs: Date.now()
  });
}

function pruneFeishuBridgeSeen(now = Date.now()) {
  for (const [key, seenAt] of feishuBridgeSeen.entries()) {
    if (now - seenAt > FEISHU_BRIDGE_DEDUP_MS) {
      feishuBridgeSeen.delete(key);
    }
  }
}

function truncateUtf8(text, maxBytes) {
  const input = String(text || '');
  if (Buffer.byteLength(input, 'utf8') <= maxBytes) {
    return input;
  }

  let output = '';
  for (const char of input) {
    const next = `${output}${char}`;
    if (Buffer.byteLength(`${next}...`, 'utf8') > maxBytes) {
      break;
    }
    output = next;
  }
  return `${output}...`;
}

function feishuBridgeTarget(chatId) {
  if (typeof chatId !== 'string' || !chatId.trim()) {
    return null;
  }
  const id = chatId.trim();
  if (id.startsWith('ou_')) {
    return { receiveId: id, receiveIdType: 'open_id' };
  }
  if (id.startsWith('oc_')) {
    return { receiveId: id, receiveIdType: 'chat_id' };
  }
  return null;
}

function parseCStringMacro(text, name) {
  const pattern = new RegExp(`#define\\s+${name}\\s+"([^"]*)"`);
  return text.match(pattern)?.[1] || '';
}

async function readFeishuBridgeCredentials() {
  const fromEnv = {
    appId: process.env.ESPAGENT_FEISHU_BRIDGE_APP_ID || '',
    appSecret: process.env.ESPAGENT_FEISHU_BRIDGE_APP_SECRET || ''
  };
  if (fromEnv.appId && fromEnv.appSecret) {
    return fromEnv;
  }

  const text = await fs.readFile(FEISHU_BRIDGE_SECRET_FILE, 'utf8');
  return {
    appId: parseCStringMacro(text, 'ESPAGENT_SECRET_FEISHU_APP_ID'),
    appSecret: parseCStringMacro(text, 'ESPAGENT_SECRET_FEISHU_APP_SECRET')
  };
}

async function getFeishuBridgeCredentials() {
  if (!feishuBridgeCredentialsPromise) {
    feishuBridgeCredentialsPromise = readFeishuBridgeCredentials();
  }
  const credentials = await feishuBridgeCredentialsPromise;
  if (!credentials.appId || !credentials.appSecret) {
    throw new Error('missing Feishu bridge app credentials');
  }
  return credentials;
}

async function getFeishuTenantToken() {
  const now = Date.now();
  if (feishuBridgeToken?.token && feishuBridgeToken.expiresAt > now + 5 * 60 * 1000) {
    return feishuBridgeToken.token;
  }

  const credentials = await getFeishuBridgeCredentials();
  const response = await fetch(FEISHU_TOKEN_URL, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json; charset=utf-8' },
    body: JSON.stringify({
      app_id: credentials.appId,
      app_secret: credentials.appSecret
    })
  });
  const payload = await response.json().catch(() => ({}));
  if (!response.ok || payload?.code !== 0 || !payload?.tenant_access_token) {
    throw new Error(`Feishu token request failed: code=${payload?.code ?? response.status} msg=${payload?.msg || response.statusText}`);
  }

  feishuBridgeToken = {
    token: payload.tenant_access_token,
    expiresAt: now + Number(payload.expire || 7200) * 1000
  };
  return feishuBridgeToken.token;
}

async function sendFeishuBridgeMessage(target, text, key) {
  const token = await getFeishuTenantToken();
  const url = new URL(FEISHU_SEND_MSG_URL);
  url.searchParams.set('receive_id_type', target.receiveIdType);
  url.searchParams.set('uuid', `espagent-bridge-${key}`);

  const response = await fetch(url, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${token}`,
      'Content-Type': 'application/json; charset=utf-8'
    },
    body: JSON.stringify({
      receive_id: target.receiveId,
      msg_type: 'text',
      content: JSON.stringify({ text })
    })
  });
  const payload = await response.json().catch(() => ({}));
  if (!response.ok || payload?.code !== 0) {
    throw new Error(`Feishu bridge send failed: code=${payload?.code ?? response.status} msg=${payload?.msg || response.statusText}`);
  }
}

function shouldSkipQueuedFeishuAck(text) {
  if (!FEISHU_BRIDGE_SKIP_QUEUED_ACK) {
    return false;
  }
  return text.includes('已通过 MQTT Mesh 下发命令') ||
    text.includes('result will be injected when OutputMessage arrives');
}

function noteFeishuBridgeEvent(error = null) {
  store.feishuBridge.lastEventAt = nowIso();
  store.feishuBridge.lastError = error;
}

function maybeBridgeFeishuOutbound(payload, meta = {}) {
  if (!FEISHU_BRIDGE_ENABLED || payload?.event !== 'feishu_outbound') {
    return;
  }

  if (meta.retained) {
    store.feishuBridge.skipped += 1;
    return;
  }

  const status = String(payload.status || '');
  if (!FEISHU_BRIDGE_STATUSES.has(status)) {
    return;
  }

  const rawText = String(payload.text || payload.text_preview || '').trim();
  if (!rawText || shouldSkipQueuedFeishuAck(rawText)) {
    store.feishuBridge.skipped += 1;
    return;
  }

  const target = feishuBridgeTarget(payload.chat_id);
  if (!target) {
    noteFeishuBridgeEvent(`unsupported chat_id=${payload.chat_id || '(missing)'}`);
    store.feishuBridge.failed += 1;
    return;
  }

  pruneFeishuBridgeSeen();
  const keyMaterial = [
    payload.chat_id || '',
    status,
    payload.ts_ms || '',
    rawText
  ].join('\n');
  const key = createHash('sha256').update(keyMaterial).digest('hex').slice(0, 24);
  if (feishuBridgeSeen.has(key)) {
    store.feishuBridge.skipped += 1;
    return;
  }
  feishuBridgeSeen.set(key, Date.now());

  const text = truncateUtf8(rawText, FEISHU_BRIDGE_MAX_TEXT_BYTES);
  sendFeishuBridgeMessage(target, text, key)
    .then(() => {
      store.feishuBridge.sent += 1;
      noteFeishuBridgeEvent(null);
    })
    .catch((error) => {
      store.feishuBridge.failed += 1;
      noteFeishuBridgeEvent(error instanceof Error ? error.message : String(error));
    });
}

function handleTimeline(payload, meta = {}) {
  maybeBridgeFeishuOutbound(payload, meta);

  const stage = payload.event || payload.type || payload.phase || 'timeline';
  pushTimeline({
    time: new Date(Number(payload.ts_ms || Date.now())).toLocaleTimeString('zh-CN', { hour12: false }),
    stage,
    source: payload.node_id || payload.sender || payload.source || 'mesh',
    target: payload.recipient || payload.target_role || payload.target_node || 'dashboard',
    payload: payload.summary || payload.text || payload.detail || JSON.stringify(payload).slice(0, 160),
    status: payload.status === 'ok' || payload.status === 'queued' ? payload.status : payload.status === 'error' ? 'warn' : 'ok',
    eventId: typeof payload.event_id === 'string' ? payload.event_id : undefined,
    nodeId: typeof payload.node_id === 'string' ? payload.node_id : undefined,
    role: typeof payload.role === 'string'
      ? payload.role
      : typeof payload.source_role === 'string'
        ? payload.source_role
        : undefined,
    phase: typeof payload.phase === 'string' ? payload.phase : undefined,
    traceId: typeof payload.trace_id === 'string' ? payload.trace_id : undefined,
    taskId: typeof payload.task_id === 'string' ? payload.task_id : undefined,
    parentTaskId: typeof payload.parent_task_id === 'string' ? payload.parent_task_id : undefined,
    commandId: typeof payload.command_id === 'string' ? payload.command_id : undefined,
    action: typeof payload.action === 'string' ? payload.action : undefined,
    targetRole: typeof payload.target_role === 'string' ? payload.target_role : undefined,
    targetNode: typeof payload.target_node === 'string' ? payload.target_node : undefined,
    tsMs: Number.isFinite(Number(payload.ts_ms)) ? Number(payload.ts_ms) : undefined
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

function hostPendingKey(kind, id) {
  return `${kind}:${id}`;
}

function makeHostId(prefix) {
  return `${prefix}-${Date.now().toString(36)}-${Math.random().toString(16).slice(2, 10)}`;
}

function waitForHostEvent(kind, id, timeoutMs = HOST_AGENT_POLICY_TIMEOUT_MS) {
  const key = hostPendingKey(kind, id);
  return new Promise((resolve) => {
    const timer = setTimeout(() => {
      hostAgentPending.delete(key);
      resolve(null);
    }, timeoutMs);
    hostAgentPending.set(key, { resolve, timer });
  });
}

function publishMqtt(topic, payload) {
  return new Promise((resolve, reject) => {
    if (!store.mqtt.connected) {
      reject(new Error('MQTT gateway is offline'));
      return;
    }
    client.publish(topic, JSON.stringify(payload), { qos: 1 }, (error) => {
      if (error) reject(error);
      else resolve();
    });
  });
}

async function publishHostMeshCommand(request) {
  const targetRole = String(request.target_role || '');
  const targetNode = String(request.target_node || '');
  const action = String(request.action || '');
  if (!['sensor_agent', 'control_agent', 'guardian_agent'].includes(targetRole)) {
    return { ok: false, error: 'target_role must be sensor_agent, control_agent, or guardian_agent' };
  }
  if (!action) return { ok: false, error: 'action is required' };

  const commandId = makeHostId('cmd');
  const traceId = String(request.traceId || makeHostId('trace'));
  const taskId = String(request.taskId || `task-${commandId}`);
  const parentTaskId = String(request.parentTaskId || '');
  const sourceChannel = String(request.sourceChannel || 'web');
  const sourceChatId = String(request.sourceChatId || 'host_console_01');
  const ttlMs = Math.max(1000, Math.min(30000, Number(request.ttl_ms || 30000)));
  const safetyLevel = Math.max(0, Math.min(2, Number(request.safety_level ?? 1)));
  const args = request.args && typeof request.args === 'object' && !Array.isArray(request.args)
    ? request.args
    : {};
  const argsJson = JSON.stringify(args);
  const timestamp = Date.now();
  const nonce = `nonce-${Math.random().toString(16).slice(2, 10)}`;
  const deadline = timestamp + ttlMs;
  const argsHash = sha256(argsJson);
  const common = {
    command_id: commandId,
    trace_id: traceId,
    task_id: taskId,
    parent_task_id: parentTaskId,
    source_channel: sourceChannel,
    source_chat_id: sourceChatId,
    deadline_ms: deadline,
    retry_count: 0,
    task_status: 'policy_pending',
    source_role: 'coordinator_agent',
    target_role: targetRole,
    target_node: targetNode,
    action,
    args_json: argsJson,
    nonce,
    args_sha256: argsHash,
    source_type: 'coordinator',
    source_id: 'coordinator_agent',
    safety_level: safetyLevel,
    ttl_ms: ttlMs,
    ts_ms: timestamp
  };

  try {
    const decisionWait = waitForHostEvent('policy', commandId);
    await publishMqtt(`${TOPIC_PREFIX}/security/policy_check`, {
      schema: 'espagent.policy_check.v1',
      event: 'policy_check',
      ...common,
      reply_channel: sourceChannel,
      reply_chat_id: sourceChatId
    });
    const decision = await decisionWait;
    if (!decision || decision.decision !== 'allow' || decision.allowed !== true) {
      return { ok: false, command_id: commandId, trace_id: traceId, status: 'blocked', reason: decision?.reason || 'Guardian policy decision timed out' };
    }

    const command = {
      schema: 'espagent.mesh_command.v1',
      ...common,
      task_status: 'queued',
      require_ack: true,
      args,
      signature: ''
    };
    command.signature = signMeshCommand({
      ...command,
      args_json: argsJson
    }, MESH_AUTH_KEY);
    const outputWait = waitForHostEvent('output', commandId, ttlMs);
    const topic = targetNode
      ? `${TOPIC_PREFIX}/nodes/${targetNode}/command`
      : `${TOPIC_PREFIX}/roles/${targetRole}/command`;
    await publishMqtt(topic, command);
    const output = await outputWait;
    if (!output) {
      return { ok: true, command_id: commandId, trace_id: traceId, status: 'queued', topic, warning: 'execution result timed out' };
    }
    return { ok: output.status === 'ok' || output.status === 'succeeded', command_id: commandId, trace_id: traceId, status: output.status || 'completed', result: output.result || output.text || output };
  } catch (error) {
    return { ok: false, command_id: commandId, trace_id: traceId, error: error instanceof Error ? error.message : String(error) };
  }
}

const hostAgent = createHostCoordinatorAgent({
  enabled: HOST_AGENT_ENABLED,
  publishMeshCommand: publishHostMeshCommand,
  getDashboardState: () => toDashboardPayload(),
  publishTimeline: handleTimeline,
  onApprovalRequired: (proposal) => hostAutomation.requestApproval(proposal)
});

const hostAutomation = createHostAutomation({
  enabled: HOST_AGENT_ENABLED && HOST_AUTOMATION_ENABLED,
  runAgent: (request) => hostAgent.run(request),
  getState: () => toDashboardPayload(),
  publishCommand: publishHostMeshCommand,
  publishTimeline: handleTimeline
});
hostAutomation.start();

client.on('connect', () => {
  store.mqtt.connected = true;
  const topics = [
    `${TOPIC_PREFIX}/nodes/+/telemetry`,
    `${TOPIC_PREFIX}/nodes/+/state`,
    `${TOPIC_PREFIX}/nodes/+/events`,
    SKILLS_MQTT_REPLY_TOPIC,
    `${TOPIC_PREFIX}/agent/timeline`,
    `${TOPIC_PREFIX}/security/decision`,
    `${TOPIC_PREFIX}/guardian/stateboard`,
    CHAT_REPLY_TOPIC
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

client.on('message', (topic, buffer, packet) => {
  const text = buffer.toString();
  const payload = safeJsonParse(text);
  const meta = { retained: Boolean(packet?.retain) };
  store.mqtt.lastEventAt = nowIso();

  if (!payload) {
    return;
  }

  if (topic === `${TOPIC_PREFIX}/security/decision` && payload.command_id) {
    const pending = hostAgentPending.get(hostPendingKey('policy', payload.command_id));
    if (pending) {
      clearTimeout(pending.timer);
      hostAgentPending.delete(hostPendingKey('policy', payload.command_id));
      pending.resolve(payload);
    }
    handleTimeline(payload, meta);
    return;
  }

  if (topic.endsWith('/events') && payload.command_id &&
      (payload.schema === 'espagent.output.v1' || payload.type === 'output' || payload.event === 'mesh_command_result')) {
    const pending = hostAgentPending.get(hostPendingKey('output', payload.command_id));
    if (pending) {
      clearTimeout(pending.timer);
      hostAgentPending.delete(hostPendingKey('output', payload.command_id));
      pending.resolve(payload);
    }
    handleTimeline(payload, meta);
    return;
  }

  if (topic === SKILLS_MQTT_REPLY_TOPIC) {
    const pending = pendingSkillRequests.get(payload.request_id);
    if (pending) {
      clearTimeout(pending.timer);
      pendingSkillRequests.delete(payload.request_id);
      pending.resolve(payload);
    }
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
    handleTimeline(payload, meta);
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
  if (topic.endsWith('/stateboard')) {
    handleGuardian(payload);
  }
});

function sendJson(res, statusCode, payload) {
  res.writeHead(statusCode, {
    'Content-Type': 'application/json; charset=utf-8',
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET,POST,DELETE,OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type'
  });
  res.end(JSON.stringify(payload));
}

const server = http.createServer(async (req, res) => {
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

  if (req.method === 'GET' && url.pathname === '/api/host/automation') {
    sendJson(res, 200, hostAutomation.snapshot());
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/host/automation/evaluate') {
    const result = await hostAutomation.evaluate('operator_request');
    sendJson(res, result?.ok === false ? 502 : 200, result || { ok: false, error: 'automation disabled or already running' });
    return;
  }

  const approvalMatch = url.pathname.match(/^\/api\/host\/proposals\/([^/]+)\/(approve|reject)$/);
  if (req.method === 'POST' && approvalMatch) {
    const result = await hostAutomation.approve(decodeURIComponent(approvalMatch[1]), approvalMatch[2] === 'approve');
    sendJson(res, result.ok === false ? 404 : 200, result);
    return;
  }

  if (req.method === 'GET' && url.pathname === '/api/devices/runtime') {
    if (SKILLS_SERIAL_ENABLED) {
      fetchSerialDeviceManifests()
        .then((devices) => {
          sendJson(res, 200, {
            devices,
            source: 'serial'
          });
        })
        .catch((error) => {
          fetchLocalDeviceManifests()
            .then((devices) => {
              sendJson(res, 200, {
                devices,
                source: 'local_mock',
                error: error instanceof Error ? error.message : 'serial device manifest list failed'
              });
            })
            .catch((localError) => {
              sendJson(res, 200, {
                devices: listRuntimeDeviceManifests(),
                source: 'local_mock',
                error: localError instanceof Error ? localError.message : 'local device manifest list failed'
              });
            });
        });
      return;
    }

    fetchLocalDeviceManifests()
      .then((devices) => {
        sendJson(res, 200, {
          devices,
          source: 'local_mock'
        });
      })
      .catch((error) => {
        sendJson(res, 200, {
          devices: listRuntimeDeviceManifests(),
          source: 'local_mock',
          error: error instanceof Error ? error.message : 'local device manifest list failed'
        });
      });
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/devices/runtime') {
    let body = '';
    req.on('data', (chunk) => {
      body += chunk;
    });
    req.on('end', async () => {
      const payload = safeJsonParse(body);
      const device = payload?.device;
      const confirmed = Boolean(payload?.confirmed);
      const source = SKILLS_SERIAL_ENABLED ? 'serial' : 'local_mock';

      if (!device || typeof device !== 'object') {
        sendJson(res, 400, {
          ok: false,
          source,
          message: 'missing device manifest payload',
          error: 'missing device manifest payload'
        });
        return;
      }
      if (!confirmed) {
        sendJson(res, 400, {
          ok: false,
          source,
          message: 'device manifest update requires confirmed=true',
          error: 'device manifest update requires confirmed=true'
        });
        return;
      }

      const manifestName = runtimeDeviceName(device);
      let content = '';
      try {
        content = normalizeDeviceManifestContent(device.content, manifestName);
      } catch (error) {
        sendJson(res, 400, {
          ok: false,
          source,
          message: 'invalid device manifest content',
          error: error instanceof Error ? error.message : 'invalid device manifest content'
        });
        return;
      }

      if (SKILLS_SERIAL_ENABLED) {
        try {
          const message = await upsertSerialDeviceManifestContentIfChanged(manifestName, content);
          const skipped = message.startsWith('unchanged:');
          const saved = deviceRecordFromManifest(manifestName, content, 'serial', message);
          runtimeDeviceManifests.set(saved.manifestName, saved);
          runtimeDeviceManifestsCacheAt = Date.now();
          sendJson(res, 200, {
            ok: true,
            source,
            message,
            skipped,
            device: saved
          });
          return;
        } catch (error) {
          sendJson(res, 502, {
            ok: false,
            source,
            message: 'device manifest update failed on ESP32 serial gateway',
            error: error instanceof Error ? error.message : 'device manifest update failed on ESP32 serial gateway'
          });
          return;
        }
      }

      const localExisting = runtimeDeviceManifests.get(manifestName);
      const localSkipped = localExisting?.contentHash === contentSha256(content);
      const saved = deviceRecordFromManifest(
        manifestName,
        content,
        'local_mock',
        localSkipped
          ? buildUnchangedDeviceMessage(manifestName, contentSha256(content))
          : 'front-end device manifest update simulated; ESP32 SPIFFS not yet connected'
      );
      runtimeDeviceManifests.set(saved.manifestName, saved);
      runtimeDeviceManifestsCacheAt = Date.now();
      sendJson(res, 200, {
        ok: true,
        source,
        message: saved.lastMessage,
        skipped: localSkipped,
        device: saved
      });
    });
    return;
  }

  if (req.method === 'GET' && url.pathname === '/api/skills/runtime') {
    const requestedName = url.searchParams.get('name');
    if (requestedName) {
      const runtimeName = normalizeSkillName(requestedName);
      if (!runtimeName) {
        sendJson(res, 400, { error: 'invalid runtime skill name' });
        return;
      }

      try {
        let skill;
        let source;
        if (SKILLS_API_BASE) {
          skill = await fetchUpstreamSkillContent({
            runtimeName,
            title: runtimeName,
            path: spiffsSkillPath(runtimeName)
          });
          source = 'proxy';
        } else if (SKILLS_MQTT_ENABLED && client.connected) {
          try {
            skill = await fetchMqttRuntimeSkillContent(runtimeName);
            source = 'mqtt';
          } catch (mqttError) {
            if (!SKILLS_SERIAL_ENABLED) {
              throw mqttError;
            }
            skill = await fetchSerialSkillContent({
              runtimeName,
              title: runtimeName,
              path: spiffsSkillPath(runtimeName)
            });
            source = 'serial';
          }
        } else if (SKILLS_SERIAL_ENABLED) {
          skill = await fetchSerialSkillContent({
            runtimeName,
            title: runtimeName,
            path: spiffsSkillPath(runtimeName)
          });
          source = 'serial';
        } else {
          skill = runtimeSkills.get(runtimeName);
          source = skill?.source || 'local_mock';
        }
        if (!skill) {
          sendJson(res, 404, { error: 'runtime skill not found' });
          return;
        }
        runtimeSkills.set(runtimeName, skill);
        sendJson(res, 200, { skill, source });
      } catch (error) {
        sendJson(res, isMissingRuntimeSkillError(error) ? 404 : 502, {
          error: error instanceof Error ? error.message : 'runtime skill read failed'
        });
      }
      return;
    }

    if (SKILLS_API_BASE) {
      fetchUpstreamRuntimeSkills()
        .then((skills) => {
          sendJson(res, 200, {
            skills,
            source: 'proxy'
          });
        })
        .catch((error) => {
          sendJson(res, 200, {
            skills: listRuntimeSkills(),
            source: 'proxy',
            error: error instanceof Error ? error.message : 'upstream runtime list failed'
          });
      });
      return;
    }

    if (SKILLS_MQTT_ENABLED && client.connected) {
      fetchMqttRuntimeSkills()
        .then((skills) => {
          sendJson(res, 200, {
            skills,
            source: 'mqtt'
          });
        })
        .catch((error) => {
          if (SKILLS_SERIAL_ENABLED) {
            fetchSerialRuntimeSkills()
              .then((skills) => sendJson(res, 200, {
                skills,
                source: 'serial',
                error: error instanceof Error ? error.message : 'MQTT runtime list failed'
              }))
              .catch((serialError) => sendJson(res, 200, {
                skills: listRuntimeSkills(),
                source: 'mqtt',
                error: [error, serialError]
                  .map((item) => item instanceof Error ? item.message : String(item))
                  .join('; ')
              }));
            return;
          }
          sendJson(res, 200, {
            skills: listRuntimeSkills(),
            source: 'mqtt',
            error: error instanceof Error ? error.message : 'MQTT runtime list failed'
          });
        });
      return;
    }

    if (SKILLS_SERIAL_ENABLED) {
      fetchSerialRuntimeSkills()
        .then((skills) => {
          sendJson(res, 200, {
            skills,
            source: 'serial'
          });
        })
        .catch((error) => {
          sendJson(res, 200, {
            skills: listRuntimeSkills(),
            source: 'serial',
            error: error instanceof Error ? error.message : 'serial runtime list failed'
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

  if (req.method === 'DELETE' && url.pathname === '/api/skills/runtime') {
    const runtimeName = normalizeSkillName(url.searchParams.get('name'));
    const confirmed = ['1', 'true'].includes(url.searchParams.get('confirmed'));
    if (!runtimeName) {
      sendJson(res, 400, { ok: false, message: 'invalid runtime skill name' });
      return;
    }
    if (!confirmed) {
      sendJson(res, 400, { ok: false, message: 'runtime skill delete requires confirmed=true' });
      return;
    }

    try {
      let message;
      let source;
      if (SKILLS_API_BASE) {
        const response = await fetch(
          `${SKILLS_API_BASE}/api/skills?name=${encodeURIComponent(runtimeName)}&confirmed=true`,
          { method: 'DELETE' }
        );
        const payload = await response.json().catch(() => ({}));
        if (!response.ok || payload?.ok === false) {
          throw new Error(payload?.error || payload?.message || `HTTP ${response.status}`);
        }
        message = payload?.message || `OK: skill ${runtimeName} deleted`;
        source = 'proxy';
      } else if (SKILLS_MQTT_ENABLED && client.connected) {
        message = await deleteMqttRuntimeSkill(runtimeName);
        source = 'mqtt';
      } else {
        throw new Error('MQTT is not connected; wireless skill delete unavailable');
      }
      runtimeSkills.delete(runtimeName);
      invalidateRuntimeSkillCache();
      sendJson(res, 200, { ok: true, source, message, runtimeName });
    } catch (error) {
      sendJson(res, 502, {
        ok: false,
        source: SKILLS_API_BASE ? 'proxy' : 'mqtt',
        message: 'runtime skill delete failed',
        error: error instanceof Error ? error.message : 'runtime skill delete failed'
      });
    }
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/skills/runtime') {
    let body = '';
    req.on('data', (chunk) => {
      body += chunk;
    });
    req.on('end', async () => {
      const payload = safeJsonParse(body);
      const skill = payload?.skill;
      const confirmed = Boolean(payload?.confirmed);
      const source = SKILLS_API_BASE
        ? 'proxy'
        : SKILLS_MQTT_ENABLED && client.connected
          ? 'mqtt'
          : SKILLS_SERIAL_ENABLED ? 'serial' : 'local_mock';

      if (!skill || typeof skill !== 'object') {
        sendJson(res, 400, {
          ok: false,
          source,
          message: 'missing runtime skill payload',
          error: 'missing runtime skill payload'
        });
        return;
      }
      if (!confirmed) {
        sendJson(res, 400, {
          ok: false,
          source,
          message: 'runtime skill update requires confirmed=true',
          error: 'runtime skill update requires confirmed=true'
        });
        return;
      }

      const runtimeName = runtimeSkillName(skill);
      const content = normalizeRuntimeSkillContent(skill.content, skill.title || runtimeName);
      if (!content.trim()) {
        sendJson(res, 400, {
          ok: false,
          source,
          message: 'missing runtime skill content',
          error: 'missing runtime skill content'
        });
        return;
      }

      if (SKILLS_API_BASE) {
        try {
          const result = await upsertUpstreamRuntimeSkillIfChanged(runtimeName, content);

          const saved = upstreamSkillToRuntimeRecord({
            ...skill,
            name: runtimeName,
            content,
            title: skill.title || runtimeName,
            path: skill.path || `/spiffs/skills/${runtimeName}.md`
          });
          saved.lastMessage = result.message;
          saved.cacheState = result.skipped ? 'unknown' : 'invalidated';
          runtimeSkills.set(saved.runtimeName, saved);
          runtimeSkillsCacheAt = Date.now();
          sendJson(res, 200, {
            ok: true,
            source,
            message: saved.lastMessage,
            skipped: result.skipped,
            skill: saved
          });
          return;
        } catch (error) {
          sendJson(res, 502, {
            ok: false,
            source,
            message: 'runtime skill update request failed',
            error: error instanceof Error ? error.message : 'runtime skill update request failed'
          });
          return;
        }
      }

      let mqttError = '';
      if (SKILLS_MQTT_ENABLED && client.connected) {
        try {
          const message = await upsertMqttRuntimeSkillContentIfChanged(runtimeName, content);
          const skipped = message.startsWith('unchanged:');
          const saved = draftToRuntimeRecord({
            ...skill,
            id: skill.id || runtimeName,
            name: skill.title || runtimeName,
            content,
            enabled: skill.enabled !== false,
            scope: skill.scope || 'runtime'
          }, 'mqtt', message);
          runtimeSkills.set(saved.runtimeName, saved);
          runtimeSkillsCacheAt = Date.now();
          sendJson(res, 200, {
            ok: true,
            source: 'mqtt',
            message,
            skipped,
            skill: saved
          });
          return;
        } catch (error) {
          mqttError = error instanceof Error ? error.message : 'MQTT runtime skill update failed';
          if (!SKILLS_SERIAL_ENABLED) {
            sendJson(res, 502, {
              ok: false,
              source: 'mqtt',
              message: 'runtime skill update failed on MQTT Mesh',
              error: mqttError
            });
            return;
          }
        }
      }

      if (SKILLS_SERIAL_ENABLED) {
        try {
          const message = await upsertSerialRuntimeSkillContentIfChanged(runtimeName, content);
          const skipped = message.startsWith('unchanged:');
          const saved = serialSkillToRuntimeRecord({
            ...skill,
            name: runtimeName,
            content,
            title: skill.title || runtimeName,
            path: skill.path || spiffsSkillPath(runtimeName)
          });
          saved.lastMessage = message;
          runtimeSkills.set(saved.runtimeName, saved);
          runtimeSkillsCacheAt = Date.now();
          sendJson(res, 200, {
            ok: true,
            source: 'serial',
            message,
            skipped,
            skill: saved
          });
          return;
        } catch (error) {
          sendJson(res, 502, {
            ok: false,
            source: 'serial',
            message: 'runtime skill update failed on ESP32 serial gateway',
            error: [mqttError, error instanceof Error ? error.message : 'runtime skill update failed on ESP32 serial gateway']
              .filter(Boolean)
              .join('; ')
          });
          return;
        }
      }

      const localExisting = runtimeSkills.get(runtimeName);
      const localSkipped = localExisting?.contentHash === contentSha256(content);
      const saved = draftToRuntimeRecord({
        ...skill,
        id: skill.id || runtimeName,
        name: skill.title || runtimeName,
        content,
        enabled: skill.enabled !== false,
        scope: skill.scope || 'runtime'
      }, 'local_mock', localSkipped
        ? buildUnchangedSkillMessage(runtimeName, contentSha256(content))
        : 'front-end runtime skill update simulated; ESP32 SPIFFS not yet connected');
      runtimeSkills.set(saved.runtimeName, saved);
      runtimeSkillsCacheAt = Date.now();
      sendJson(res, 200, {
        ok: true,
        source,
        message: saved.lastMessage,
        skipped: localSkipped,
        skill: saved
      });
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
      const source = SKILLS_API_BASE
        ? 'proxy'
        : SKILLS_MQTT_ENABLED && client.connected
          ? 'mqtt'
          : SKILLS_SERIAL_ENABLED ? 'serial' : 'local_mock';

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

      const runtimeName = runtimeSkillName(skill);
      const content = skillDraftToMarkdown(skill);

      if (SKILLS_API_BASE) {
        try {
          const result = await upsertUpstreamRuntimeSkillIfChanged(runtimeName, content);

          const installed = upstreamSkillToRuntimeRecord({
            name: runtimeName,
            title: skill.name || runtimeName,
            path: `/spiffs/skills/${runtimeName}.md`,
            content
          });
          installed.lastMessage = result.message;
          installed.cacheState = result.skipped ? 'unknown' : 'invalidated';
          runtimeSkills.set(installed.runtimeName, installed);
          runtimeSkillsCacheAt = Date.now();
          sendJson(res, 200, {
            ok: true,
            source,
            message: installed.lastMessage,
            skipped: result.skipped,
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

      let mqttError = '';
      if (SKILLS_MQTT_ENABLED && client.connected) {
        try {
          const message = await upsertMqttRuntimeSkillContentIfChanged(runtimeName, content);
          const skipped = message.startsWith('unchanged:');
          const installed = draftToRuntimeRecord(skill, 'mqtt', message);
          runtimeSkills.set(installed.runtimeName, installed);
          runtimeSkillsCacheAt = Date.now();
          sendJson(res, 200, {
            ok: true,
            source: 'mqtt',
            message,
            skipped,
            skill: installed
          });
          return;
        } catch (error) {
          mqttError = error instanceof Error ? error.message : 'MQTT runtime skill install failed';
          if (!SKILLS_SERIAL_ENABLED) {
            sendJson(res, 502, {
              ok: false,
              source: 'mqtt',
              message: 'runtime install failed on MQTT Mesh',
              error: mqttError
            });
            return;
          }
        }
      }

      if (SKILLS_SERIAL_ENABLED) {
        try {
          const message = await upsertSerialRuntimeSkillContentIfChanged(runtimeName, content);
          const skipped = message.startsWith('unchanged:');
          const installed = draftToRuntimeRecord(skill, 'serial', message);
          runtimeSkills.set(installed.runtimeName, installed);
          runtimeSkillsCacheAt = Date.now();
          sendJson(res, 200, {
            ok: true,
            source: 'serial',
            message,
            skipped,
            skill: installed
          });
          return;
        } catch (error) {
          sendJson(res, 502, {
            ok: false,
            source: 'serial',
            message: 'runtime install failed on ESP32 serial gateway',
            error: [mqttError, error instanceof Error ? error.message : 'runtime install failed on ESP32 serial gateway']
              .filter(Boolean)
              .join('; ')
          });
          return;
        }
      }

      const localExisting = runtimeSkills.get(runtimeName);
      const localSkipped = localExisting?.contentHash === contentSha256(content);
      const installed = draftToRuntimeRecord(
        skill,
        'local_mock',
        localSkipped
          ? buildUnchangedSkillMessage(runtimeName, contentSha256(content))
          : 'front-end runtime install simulated; ESP32 SPIFFS not yet connected'
      );
      runtimeSkills.set(installed.runtimeName, installed);
      runtimeSkillsCacheAt = Date.now();
      sendJson(res, 200, {
        ok: true,
        source,
        message: installed.lastMessage,
        skipped: localSkipped,
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
  console.log(`[dashboard] feishu-bridge enabled=${FEISHU_BRIDGE_ENABLED} statuses=${Array.from(FEISHU_BRIDGE_STATUSES).join(',')}`);
});
