import { createHash, createHmac, randomUUID } from 'node:crypto';

const DEFAULT_MAX_ACTIONS = 8;
const DEFAULT_MAX_ROUNDS = 8;
const DEFAULT_MAX_HISTORY = 12;
const DEFAULT_TIMEOUT_MS = 45_000;

const meshTool = {
  type: 'function',
  function: {
    name: 'mesh_send_command',
    description: 'Send one structured command to sensor_agent, control_agent, or guardian_agent. Hardware actions are always checked by Guardian before execution.',
    parameters: {
      type: 'object',
      additionalProperties: false,
      properties: {
        target_role: { type: 'string', enum: ['sensor_agent', 'control_agent', 'guardian_agent'] },
        target_node: { type: 'string' },
        action: { type: 'string' },
        args: { type: 'object' },
        safety_level: { type: 'integer', minimum: 0, maximum: 2 },
        ttl_ms: { type: 'integer', minimum: 1000, maximum: 30000 }
      },
      required: ['target_role', 'action']
    }
  }
};

const dashboardTool = {
  type: 'function',
  function: {
    name: 'get_dashboard_state',
    description: 'Read the latest aggregated node, telemetry, guardian, and timeline state from the host console.',
    parameters: { type: 'object', additionalProperties: false, properties: {} }
  }
};

function asObject(value) {
  return value && typeof value === 'object' && !Array.isArray(value) ? value : {};
}

function compact(value, max = 1600) {
  const text = typeof value === 'string' ? value : JSON.stringify(value);
  return text.length > max ? `${text.slice(0, max)}...` : text;
}

function sha256(value) {
  return createHash('sha256').update(value, 'utf8').digest('hex');
}

function nowMs() {
  return Date.now();
}

function makeId(prefix) {
  return `${prefix}-${randomUUID().replaceAll('-', '').slice(0, 20)}`;
}

function parseOpenAiResponse(payload) {
  const message = payload?.choices?.[0]?.message || {};
  const calls = Array.isArray(message.tool_calls)
    ? message.tool_calls.map((call) => ({
      id: String(call.id || makeId('call')),
      name: String(call.function?.name || ''),
      arguments: call.function?.arguments || '{}'
    })).filter((call) => call.name)
    : [];
  return { text: typeof message.content === 'string' ? message.content : '', calls };
}

function parseAnthropicResponse(payload) {
  const calls = Array.isArray(payload?.content)
    ? payload.content.filter((item) => item?.type === 'tool_use' && item.name).map((item) => ({
      id: String(item.id || makeId('call')),
      name: String(item.name),
      arguments: JSON.stringify(asObject(item.input))
    }))
    : [];
  const text = Array.isArray(payload?.content)
    ? payload.content.filter((item) => item?.type === 'text').map((item) => item.text).join('')
    : '';
  return { text, calls };
}

function parseArguments(raw) {
  try {
    return asObject(typeof raw === 'string' ? JSON.parse(raw) : raw);
  } catch {
    return {};
  }
}

function buildSystemPrompt({ maxActions, maxRounds }) {
  return [
    'You are the host-side runtime of the single logical ESPAgent coordinator_agent.',
    'The host and the ESP32 coordinator are one logical role; do not tell the user they are separate agents.',
    'Use mesh_send_command for sensor, control, and guardian work. Never claim hardware execution without a tool result.',
    `You may execute at most ${maxActions} actions in this request and ${maxRounds} tool rounds.`,
    'Read-only sensor calls may be planned in sequence; control actions remain individually Guardian-gated.',
    'After the available action budget is exhausted, summarize the verified results and stop.'
  ].join('\n');
}

export function createHostCoordinatorAgent({
  enabled = false,
  provider = process.env.ESPAGENT_HOST_LLM_PROVIDER || 'openai',
  apiUrl = process.env.ESPAGENT_HOST_LLM_API_URL || '',
  apiKey = process.env.ESPAGENT_HOST_LLM_API_KEY || '',
  model = process.env.ESPAGENT_HOST_LLM_MODEL || '',
  maxActions = Number(process.env.ESPAGENT_HOST_AGENT_MAX_ACTIONS || DEFAULT_MAX_ACTIONS),
  maxRounds = Number(process.env.ESPAGENT_HOST_AGENT_MAX_ROUNDS || DEFAULT_MAX_ROUNDS),
  maxHistory = Number(process.env.ESPAGENT_HOST_AGENT_MAX_HISTORY || DEFAULT_MAX_HISTORY),
  timeoutMs = Number(process.env.ESPAGENT_HOST_AGENT_TIMEOUT_MS || DEFAULT_TIMEOUT_MS),
  publishMeshCommand,
  getDashboardState,
  publishTimeline,
  onApprovalRequired
}) {
  const histories = new Map();
  const queues = new Map();

  function enqueue(chatId, task) {
    const previous = queues.get(chatId) || Promise.resolve();
    const next = previous.catch(() => {}).then(task);
    const tracked = next.finally(() => {
      if (queues.get(chatId) === tracked) queues.delete(chatId);
    });
    // Keep the bookkeeping promise from becoming an unhandled rejection when
    // the caller handles the returned `next` promise itself.
    tracked.catch(() => {});
    queues.set(chatId, tracked);
    return next;
  }

  async function callLlm(messages, tools, signal) {
    if (!apiUrl || !apiKey || !model) {
      throw new Error('host coordinator LLM is not configured');
    }
    if (provider.toLowerCase() === 'anthropic') {
      const system = messages.find((item) => item.role === 'system')?.content || '';
      const bodyMessages = messages.filter((item) => item.role !== 'system').map((item) => {
        if (item.role === 'tool') {
          return { role: 'user', content: [{ type: 'tool_result', tool_use_id: item.tool_call_id, content: item.content }] };
        }
        return item;
      });
      const response = await fetch(apiUrl, {
        method: 'POST',
        signal,
        headers: { 'content-type': 'application/json', 'x-api-key': apiKey, 'anthropic-version': '2023-06-01' },
        body: JSON.stringify({ model, max_tokens: 2048, system, messages: bodyMessages, tools: tools.map((item) => ({ name: item.function.name, description: item.function.description, input_schema: item.function.parameters })) })
      });
      const payload = await response.json();
      if (!response.ok) throw new Error(`host LLM HTTP ${response.status}: ${compact(payload, 240)}`);
      return parseAnthropicResponse(payload);
    }

    const response = await fetch(apiUrl, {
      method: 'POST',
      signal,
      headers: { 'content-type': 'application/json', authorization: `Bearer ${apiKey}` },
      body: JSON.stringify({ model, messages, tools, tool_choice: 'auto' })
    });
    const payload = await response.json();
    if (!response.ok) throw new Error(`host LLM HTTP ${response.status}: ${compact(payload, 240)}`);
    return parseOpenAiResponse(payload);
  }

  async function run({ chatId, channel = 'web', content, approvalRequired = false }) {
    if (!enabled) throw new Error('host coordinator is disabled');
    const id = chatId || 'host_console_01';
    return enqueue(id, async () => {
      const traceId = makeId('trace');
      const history = histories.get(id) || [];
      const messages = [
        { role: 'system', content: buildSystemPrompt({ maxActions, maxRounds }) },
        ...history.slice(-maxHistory),
        { role: 'user', content: String(content || '') }
      ];
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), timeoutMs);
      let actionCount = 0;
      let round = 0;
      let finalText = '';
      try {
        publishTimeline?.({ event: 'host_agent_start', phase: 'reasoning', status: 'ok', role: 'coordinator_agent', trace_id: traceId, task_id: `task-${traceId}`, source: 'coordinator_agent', target: 'dashboard', payload: String(content || '') });
        while (round < maxRounds && actionCount < maxActions) {
          round += 1;
          const response = await callLlm(messages, [meshTool, dashboardTool], controller.signal);
          if (!response.calls.length) {
            finalText = response.text || '已完成处理，但模型没有返回可展示的结果。';
            break;
          }
          messages.push({ role: 'assistant', content: response.text || null, tool_calls: response.calls.map((call) => ({ id: call.id, type: 'function', function: { name: call.name, arguments: call.arguments } })) });
          for (const call of response.calls) {
            if (actionCount >= maxActions) break;
            actionCount += 1;
            const args = parseArguments(call.arguments);
            publishTimeline?.({ event: 'host_agent_action', phase: 'tool_use', status: 'queued', role: 'coordinator_agent', trace_id: traceId, task_id: `task-${traceId}`, command_id: call.id, source: 'coordinator_agent', target: args.target_role || 'dashboard', action: call.name, payload: compact(args) });
            let result;
            if (call.name === 'mesh_send_command') {
              if (approvalRequired && args.target_role === 'control_agent') {
                result = await onApprovalRequired?.({
                  proposalId: makeId('proposal'),
                  callId: call.id,
                  traceId,
                  taskId: `task-${traceId}`,
                  parentTaskId: id,
                  sourceChannel: channel,
                  sourceChatId: id,
                  ...args
                }) || { ok: false, status: 'awaiting_confirmation', error: 'approval callback unavailable' };
              } else {
                result = await publishMeshCommand({ ...args, traceId, taskId: `task-${traceId}`, parentTaskId: id, sourceChannel: channel, sourceChatId: id });
              }
            } else if (call.name === 'get_dashboard_state') {
              result = await getDashboardState();
            } else {
              result = { ok: false, error: `unsupported host tool: ${call.name}` };
            }
            const serialized = compact(result, 3000);
            messages.push({ role: 'tool', tool_call_id: call.id, content: serialized });
            publishTimeline?.({ event: 'host_agent_result', phase: 'tool_result', status: result?.ok === false ? 'warn' : 'ok', role: 'coordinator_agent', trace_id: traceId, task_id: `task-${traceId}`, command_id: call.id, source: 'coordinator_agent', target: args.target_role || 'dashboard', action: call.name, payload: serialized });
          }
        }
        if (!finalText) {
          const summary = messages.filter((item) => item.role === 'tool').at(-1)?.content || '动作预算已用尽';
          finalText = `已执行 ${actionCount} 个动作。${summary}`;
        }
        history.push({ role: 'user', content: String(content || '') }, { role: 'assistant', content: finalText });
        histories.set(id, history.slice(-maxHistory));
        publishTimeline?.({ event: 'host_agent_final', phase: 'final_reply', status: 'ok', role: 'coordinator_agent', trace_id: traceId, task_id: `task-${traceId}`, source: 'coordinator_agent', target: channel, payload: finalText });
        return { ok: true, traceId, actionCount, rounds: round, content: finalText };
      } finally {
        clearTimeout(timer);
      }
    });
  }

  return {
    enabled: Boolean(enabled),
    maxActions,
    maxRounds,
    run
  };
}

export function signMeshCommand(command, key) {
  if (!key) return '';
  const canonical = [
    command.command_id, command.trace_id, command.target_node || '', command.target_role || '',
    command.action, command.args_json || '{}', command.nonce, command.ttl_ms, command.safety_level,
    command.ts_ms, command.task_id, command.parent_task_id || '', command.source_channel || '',
    command.source_chat_id || '', command.deadline_ms, command.retry_count || 0
  ].join('\n');
  return createHmac('sha256', key).update(canonical, 'utf8').digest('hex');
}

export { sha256 };
