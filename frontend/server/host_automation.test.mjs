import assert from 'node:assert/strict';
import { createHostAutomation } from './host_automation.mjs';

const proposals = [];
const commands = [];
let runs = 0;
const automation = createHostAutomation({
  enabled: true,
  intervalMs: 1,
  runAgent: async ({ approvalRequired }) => {
    runs += 1;
    assert.equal(approvalRequired, true);
    const proposal = await automation.requestApproval({
      proposalId: 'proposal-test', traceId: 'trace-test', taskId: 'task-test', callId: 'call-test',
      target_role: 'control_agent', action: 'set_status_light', args: { color: 'blue' }
    });
    proposals.push(proposal);
    return { ok: true, content: '等待人工确认' };
  },
  getState: async () => ({ environment: [{ label: '温度', value: 30 }] }),
  publishCommand: async (request) => {
    commands.push(request);
    return { ok: true, status: 'succeeded' };
  }
});

try {
  await automation.evaluate('test');
  assert.equal(runs, 1);
  assert.equal(proposals[0].status, 'awaiting_confirmation');
  const approved = await automation.approve('proposal-test', true);
  assert.equal(approved.status, 'executed');
  assert.equal(commands.length, 1);
  assert.equal(commands[0].action, 'set_status_light');
  console.log('host automation approval loop: PASS');
} finally {
  automation.stop();
}
