import { randomUUID } from 'node:crypto';

const DEFAULT_INTERVAL_MS = 60_000;
const DEFAULT_MAX_PENDING = 32;

function id(prefix) {
  return `${prefix}-${randomUUID().replaceAll('-', '').slice(0, 20)}`;
}

function compact(value, max = 1800) {
  const text = typeof value === 'string' ? value : JSON.stringify(value);
  return text.length > max ? `${text.slice(0, max)}...` : text;
}

export function createHostAutomation({
  enabled = false,
  intervalMs = Number(process.env.ESPAGENT_HOST_AUTOMATION_INTERVAL_MS || DEFAULT_INTERVAL_MS),
  maxPending = Number(process.env.ESPAGENT_HOST_AUTOMATION_MAX_PENDING || DEFAULT_MAX_PENDING),
  runAgent,
  getState,
  publishCommand,
  publishTimeline
}) {
  const pending = new Map();
  const lastRunByNode = new Map();
  let timer = null;
  let running = false;
  let lastTelemetry = null;
  let lastRunAt = null;
  let lastError = null;

  function snapshot() {
    return {
      enabled: Boolean(enabled),
      running,
      intervalMs,
      pending: Array.from(pending.values()).map(({ proposalId, createdAt, ...item }) => ({ proposalId, createdAt, ...item })),
      lastRunAt,
      lastError
    };
  }

  async function evaluate(reason = 'telemetry') {
    if (!enabled || running || !runAgent) return null;
    running = true;
    lastRunAt = new Date().toISOString();
    try {
      const state = await getState();
      const content = [
        '持续环境监测触发了一次主协调器 ReAct 评估。',
        `触发原因：${reason}`,
        '请读取当前 dashboard 状态，判断是否需要采取行动。传感器读取可以直接执行；任何 control_agent 动作只能生成建议并等待人工确认。',
        `当前状态：${compact(state, 5000)}`
      ].join('\n');
      const result = await runAgent({
        chatId: 'host_automation',
        channel: 'automation',
        content,
        approvalRequired: true
      });
      publishTimeline?.({
        event: 'host_automation_cycle', phase: 'finalize', status: 'ok',
        role: 'coordinator_agent', source: 'coordinator_agent', target: 'dashboard',
        trace_id: result?.traceId, task_id: `automation-${result?.traceId || id('trace')}`,
        payload: result?.content || 'automation cycle completed'
      });
      return result;
    } catch (error) {
      lastError = error instanceof Error ? error.message : String(error);
      publishTimeline?.({ event: 'host_automation_error', phase: 'error', status: 'error', role: 'coordinator_agent', source: 'coordinator_agent', target: 'dashboard', payload: lastError });
      return { ok: false, error: lastError };
    } finally {
      running = false;
    }
  }

  async function requestApproval(proposal) {
    if (pending.size >= maxPending) {
      return { ok: false, status: 'blocked', error: 'host automation approval queue is full' };
    }
    const proposalId = proposal.proposalId || id('proposal');
    const record = {
      proposalId,
      createdAt: new Date().toISOString(),
      status: 'awaiting_confirmation',
      source: 'host_automation',
      ...proposal
    };
    pending.set(proposalId, record);
    publishTimeline?.({
      event: 'host_action_proposed', phase: 'approval', status: 'queued',
      role: 'coordinator_agent', source: 'coordinator_agent', target: proposal.target_role || 'control_agent',
      trace_id: proposal.traceId, task_id: proposal.taskId, command_id: proposal.callId,
      action: proposal.action, payload: compact(record)
    });
    return { ok: true, status: 'awaiting_confirmation', proposal_id: proposalId, proposal: record };
  }

  async function approve(proposalId, confirmed = true) {
    const record = pending.get(proposalId);
    if (!record) return { ok: false, error: 'proposal not found or expired' };
    pending.delete(proposalId);
    if (!confirmed) {
      publishTimeline?.({ event: 'host_action_rejected', phase: 'approval', status: 'blocked', role: 'coordinator_agent', source: 'operator', target: record.target_role, trace_id: record.traceId, task_id: record.taskId, command_id: record.callId, action: record.action, payload: 'operator rejected proposal' });
      return { ok: true, status: 'rejected', proposal_id: proposalId };
    }
    const result = await publishCommand(record);
    publishTimeline?.({ event: 'host_action_confirmed', phase: 'tool_result', status: result?.ok ? 'ok' : 'warn', role: 'coordinator_agent', source: 'operator', target: record.target_role, trace_id: record.traceId, task_id: record.taskId, command_id: record.callId, action: record.action, payload: compact(result) });
    return { ...result, proposal_id: proposalId, status: result?.ok ? 'executed' : (result?.status || 'failed') };
  }

  function onTelemetry(event) {
    if (!enabled || event?.role !== 'sensor_agent') return;
    lastTelemetry = event;
    const nodeId = event.nodeId || 'sensor_agent';
    const now = Date.now();
    if (now - (lastRunByNode.get(nodeId) || 0) < intervalMs) return;
    lastRunByNode.set(nodeId, now);
    void evaluate('telemetry_update');
  }

  function start() {
    if (!enabled || timer) return;
    timer = setInterval(() => void evaluate('periodic_monitor'), intervalMs);
    timer.unref?.();
  }

  function stop() {
    if (timer) clearInterval(timer);
    timer = null;
  }

  return { enabled: Boolean(enabled), start, stop, evaluate, onTelemetry, requestApproval, approve, snapshot, get lastTelemetry() { return lastTelemetry; } };
}
