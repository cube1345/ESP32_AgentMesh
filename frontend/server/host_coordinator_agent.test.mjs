import assert from 'node:assert/strict';
import {
  compactMeshContextId,
  createHostCoordinatorAgent,
  normalizeHostMeshAction,
  validateHostMeshRequest
} from './host_coordinator_agent.mjs';

const originalFetch = globalThis.fetch;
const published = [];
let calls = 0;

globalThis.fetch = async () => {
  calls += 1;
  const payload = calls === 1
    ? {
      choices: [{ message: { content: '', tool_calls: [
        { id: 'call-1', function: { name: 'get_dashboard_state', arguments: '{}' } },
        { id: 'call-2', function: { name: 'mesh_send_command', arguments: '{"target_role":"sensor_agent","action":"read_environment"}' } }
      ] } }]
    }
    : {
      choices: [{ message: { content: '完成', tool_calls: [
        { id: 'call-3', function: { name: 'mesh_send_command', arguments: '{"target_role":"control_agent","action":"set_status_led","args":{"color":"blue"}}' } },
        { id: 'call-4', function: { name: 'mesh_send_command', arguments: '{"target_role":"control_agent","action":"set_status_light","args":{"color":"off"}}' } }
      ] } }]
    };
  return { ok: true, json: async () => payload };
};

try {
  const agent = createHostCoordinatorAgent({
    enabled: true,
    apiUrl: 'http://test.invalid/v1/chat/completions',
    apiKey: 'test-key',
    model: 'test-model',
    maxActions: 3,
    maxRounds: 4,
    publishMeshCommand: async (request) => {
      published.push(request);
      return { ok: true, status: 'succeeded', action: request.action };
    },
    getDashboardState: async () => ({ nodes: [] }),
    publishTimeline: () => {}
  });

  const result = await agent.run({ chatId: 'test-chat', content: '先读取环境，再处理灯光' });
  assert.equal(result.ok, true);
  assert.equal(result.actionCount, 3);
  assert.equal(published.length, 2);
  assert.equal(published[0].action, 'read_environment');
  assert.equal(published[1].action, 'set_status_light');
  assert.equal(calls, 2);

  assert.equal(normalizeHostMeshAction('set_status_led'), 'set_status_light');
  assert.deepEqual(validateHostMeshRequest({
    target_role: 'sensor_agent',
    action: 'set_status_light'
  }), {
    ok: false,
    action: 'set_status_light',
    error: 'action=set_status_light is not allowed for target_role=sensor_agent'
  });

  let invalidPublishCount = 0;
  globalThis.fetch = async () => ({
    ok: true,
    json: async () => ({
      choices: [{ message: { content: '', tool_calls: [{
        id: 'invalid-call',
        function: {
          name: 'mesh_send_command',
          arguments: '{"target_role":"sensor_agent","action":"set_status_light"}'
        }
      }] } }]
    })
  });
  const invalidAgent = createHostCoordinatorAgent({
    enabled: true,
    apiUrl: 'http://test.invalid/v1/chat/completions',
    apiKey: 'test-key',
    model: 'test-model',
    maxActions: 1,
    publishMeshCommand: async () => {
      invalidPublishCount += 1;
      return { ok: true };
    },
    getDashboardState: async () => ({}),
    publishTimeline: () => {}
  });
  const invalidResult = await invalidAgent.run({ chatId: 'invalid-chat', content: 'invalid action' });
  assert.equal(invalidResult.ok, true);
  assert.equal(invalidPublishCount, 0);
  assert.match(invalidResult.content, /not allowed/);

  const longContext = '长会话/'.repeat(40);
  const compactContext = compactMeshContextId(longContext, 'chat', 16);
  assert.equal(compactContext, compactMeshContextId(longContext, 'chat', 16));
  assert.ok(Buffer.byteLength(compactContext, 'utf8') <= 16);
  assert.match(compactContext, /^chat-[a-f0-9]+$/);

  const decision = {
    schema: 'espagent.policy_decision.v1',
    event: 'policy_decision',
    decision_id: `decision-${'c'.repeat(16)}-9999999999999`,
    guardian_node: 'esp32s3-guardian-01',
    guardian_role: 'guardian_agent',
    command_id: 'c'.repeat(16),
    trace_id: 't'.repeat(16),
    task_id: 'k'.repeat(16),
    parent_task_id: 'p'.repeat(16),
    source_channel: 'c'.repeat(12),
    source_chat_id: 's'.repeat(16),
    deadline_ms: 9999999999999,
    retry_count: 0,
    task_status: 'queued',
    action: 'read_environment',
    target_role: 'sensor_agent',
    target_node: '',
    decision: 'allow',
    allowed: true,
    reason: 'allowed low-risk sensor read',
    reason_code: 'sensor_read_allowed',
    safety_level: 1,
    risk_score: 25,
    privacy_mode: 'metadata_only',
    policy_version: 'guardian-policy.v2',
    args_sha256: 'a'.repeat(64),
    nonce: 'nonce-12345678',
    rate_limited: false,
    lockout_until_ms: 0,
    source_type: 'coordinator',
    source_id: 'coordinator_agent',
    ts_ms: 9999999999999
  };
  assert.ok(
    Buffer.byteLength(JSON.stringify(decision), 'utf8') < 1024,
    `Guardian policy decision must fit MQTT buffer, got ${Buffer.byteLength(JSON.stringify(decision), 'utf8')} bytes`
  );
  console.log('host coordinator action budget: PASS');
} finally {
  globalThis.fetch = originalFetch;
}
