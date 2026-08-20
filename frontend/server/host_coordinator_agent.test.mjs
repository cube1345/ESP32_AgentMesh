import assert from 'node:assert/strict';
import { createHostCoordinatorAgent } from './host_coordinator_agent.mjs';

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
        { id: 'call-3', function: { name: 'mesh_send_command', arguments: '{"target_role":"control_agent","action":"set_status_light","args":{"color":"blue"}}' } },
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
  console.log('host coordinator action budget: PASS');
} finally {
  globalThis.fetch = originalFetch;
}
