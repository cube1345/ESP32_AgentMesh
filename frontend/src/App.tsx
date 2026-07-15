import {
  ApiOutlined,
  AuditOutlined,
  BranchesOutlined,
  ControlOutlined,
  DashboardOutlined,
  DeploymentUnitOutlined,
  LogoutOutlined,
  MessageOutlined,
  NodeIndexOutlined,
  SafetyCertificateOutlined,
  ThunderboltOutlined,
  UserOutlined
} from '@ant-design/icons';
import { App as AntdApp, Button, ConfigProvider, Form, Input, Tag, message } from 'antd';
import { useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import ChatWorkspace, { type ChatMessage } from './components/ChatWorkspace';
import {
  CollaborationView,
  OverviewView,
  PreferencesView,
  SandboxView,
  SkillsView,
  type NodePanel,
  type TimelineFilter
} from './components/ConsoleViews';
import { mockDashboardPayload } from './data/mock';
import { fetchDashboard, fetchRuntimeSkills, installRuntimeSkill, savePreferences, saveSkills, updateRuntimeSkill } from './services/dashboard';
import type { AgentNode, DashboardPayload, RuntimeSkillRecord, SkillDraft, TimelineEvent, UserPreferenceProfile } from './types';

type ViewMode = '总览' | '协作链路' | 'Sandbox' | 'Skills Studio' | '用户偏好';
type Role = 'admin' | 'operator' | 'viewer';

const DRAFT_SKILLS_STORAGE_KEY = 'espagent.console.skillDrafts';
const PREFS_STORAGE_KEY = 'espagent.console.preferences';
const AUTH_STORAGE_KEY = 'espagent.console.auth';
const SANDBOX_DENY_TEST_COMMAND = `python3 tools/serial_cmd.py /dev/ttyUSB0 'tool_exec write_file {"path":"/spiffs/skills/bench-unsafe.md","content":"x"}' --timeout 20`;

interface UserSession {
  username: string;
  role: Role;
}

const demoAccounts: Array<UserSession & { password: string; description: string }> = [
  { username: 'admin', password: 'admin123', role: 'admin', description: '完整配置权限' },
  { username: 'operator', password: 'operator123', role: 'operator', description: '调度与监控权限' },
  { username: 'viewer', password: 'viewer123', role: 'viewer', description: '只读展示权限' }
];

const viewItems: Array<{ label: ViewMode; caption: string; icon: ReactNode }> = [
  { label: '总览', caption: '节点与环境', icon: <DashboardOutlined /> },
  { label: '协作链路', caption: '消息与事件', icon: <BranchesOutlined /> },
  { label: 'Sandbox', caption: '策略与审计', icon: <AuditOutlined /> },
  { label: 'Skills Studio', caption: '能力编辑', icon: <ControlOutlined /> },
  { label: '用户偏好', caption: '交互配置', icon: <SafetyCertificateOutlined /> }
];

const viewMeta: Record<ViewMode, { eyebrow: string; title: string; description: string }> = {
  总览: { eyebrow: 'System Overview', title: 'Agent Mesh 运行态', description: '节点、环境和最新任务的一屏状态。' },
  协作链路: { eyebrow: 'Collaboration', title: '消息与执行链路', description: '追踪命令、策略、遥测和结果。' },
  Sandbox: { eyebrow: 'Safety Boundary', title: 'Sandbox 与审计', description: '检查工具约束和风险事件。' },
  'Skills Studio': { eyebrow: 'Runtime Extension', title: 'Skills Studio', description: '编辑草稿并同步到设备 Runtime。' },
  用户偏好: { eyebrow: 'User Profile', title: '交互偏好', description: '管理通道、隐私和自动化策略。' }
};

const fixedNodeOrder: AgentNode['role'][] = ['coordinator_agent', 'sensor_agent', 'control_agent', 'guardian_agent'];

const nodeChannelHints: Record<AgentNode['role'], { sends: string[]; receives: string[]; subagent: boolean }> = {
  coordinator_agent: { sends: ['dispatch', 'timeline', 'mesh_command'], receives: ['chat input', 'tool result', 'mesh reply'], subagent: true },
  sensor_agent: { sends: ['telemetry', 'environment', 'threshold event'], receives: ['agent task', 'role command', 'policy request'], subagent: false },
  control_agent: { sends: ['control state', 'tool result', 'actuator result'], receives: ['mesh command', 'policy decision'], subagent: false },
  guardian_agent: { sends: ['policy decision', 'audit', 'stateboard'], receives: ['policy check', 'timeline', 'node state'], subagent: false },
  display_terminal: { sends: ['ui event'], receives: ['timeline', 'dashboard sync'], subagent: false }
};

function readStorage<T>(key: string, fallback: T): T {
  try {
    const raw = localStorage.getItem(key);
    return raw ? JSON.parse(raw) as T : fallback;
  } catch {
    return fallback;
  }
}

function stageMatchesNode(value: string, node: AgentNode): boolean {
  return value.includes(node.id) || value.includes(node.role);
}

function isSandboxEvent(event: TimelineEvent): boolean {
  return `${event.stage} ${event.source} ${event.target} ${event.payload}`.toLowerCase().match(/sandbox|沙箱|denied|blocked by sandbox/) !== null;
}

function defaultGatewayWsUrl(): string {
  if (typeof window === 'undefined') return 'ws://127.0.0.1:4173/ws';
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${protocol}//${window.location.host}/ws`;
}

function LoginScreen(props: {
  username: string;
  password: string;
  setUsername: (value: string) => void;
  setPassword: (value: string) => void;
  login: () => void;
}) {
  return (
    <div className="login-screen">
      <section className="login-brand">
        <div className="brand-lockup"><span><DeploymentUnitOutlined /></span><div><strong>ESPAgent</strong><small>Agent Mesh Console</small></div></div>
        <div className="login-copy"><span>四节点协同运行台</span><h1>感知、决策与执行，保持在同一条可信链路。</h1></div>
        <div className="login-topology" aria-label="Coordinator、Sensor、Control、Guardian 四节点拓扑">
          <div className="login-node node-coordinator">Coordinator</div>
          <div className="login-node node-sensor">Sensor</div>
          <div className="login-node node-control">Control</div>
          <div className="login-node node-guardian">Guardian</div>
        </div>
      </section>
      <section className="login-panel">
        <div className="login-form-wrap">
          <div className="login-heading"><span>Console Access</span><h2>进入控制台</h2><p>选择演示身份或使用已有账号。</p></div>
          <Form layout="vertical" onFinish={props.login} requiredMark={false}>
            <Form.Item label="用户名"><Input size="large" prefix={<UserOutlined />} value={props.username} onChange={(event) => props.setUsername(event.target.value)} autoComplete="username" /></Form.Item>
            <Form.Item label="密码"><Input.Password size="large" value={props.password} onChange={(event) => props.setPassword(event.target.value)} autoComplete="current-password" /></Form.Item>
            <Button type="primary" size="large" htmlType="submit" block>登录控制台</Button>
          </Form>
          <div className="demo-account-list">
            {demoAccounts.map((account) => <button key={account.username} onClick={() => { props.setUsername(account.username); props.setPassword(account.password); }}><span>{account.username}</span><small>{account.description}</small></button>)}
          </div>
        </div>
      </section>
    </div>
  );
}

function App() {
  const [payload, setPayload] = useState<DashboardPayload>(mockDashboardPayload);
  const [draftSkills, setDraftSkills] = useState<SkillDraft[]>(() => readStorage(DRAFT_SKILLS_STORAGE_KEY, mockDashboardPayload.skills));
  const [runtimeSkills, setRuntimeSkills] = useState<RuntimeSkillRecord[]>([]);
  const [preferences, setPreferences] = useState<UserPreferenceProfile>(() => readStorage(PREFS_STORAGE_KEY, mockDashboardPayload.preferences));
  const [session, setSession] = useState<UserSession | null>(() => readStorage<UserSession | null>(AUTH_STORAGE_KEY, null));
  const [viewMode, setViewMode] = useState<ViewMode>('总览');
  const [timelineFilter, setTimelineFilter] = useState<TimelineFilter>('全部');
  const [activeSkillId, setActiveSkillId] = useState(mockDashboardPayload.skills[0]?.id || '');
  const [skillEditorMode, setSkillEditorMode] = useState<'draft' | 'runtime'>('draft');
  const [activeRuntimeSkillId, setActiveRuntimeSkillId] = useState('');
  const [savingSkills, setSavingSkills] = useState(false);
  const [installingSkillId, setInstallingSkillId] = useState<string | null>(null);
  const [savingRuntimeSkillId, setSavingRuntimeSkillId] = useState<string | null>(null);
  const [runtimeSkillSource, setRuntimeSkillSource] = useState<'local_mock' | 'proxy' | 'serial'>('local_mock');
  const [runtimeSkillError, setRuntimeSkillError] = useState('');
  const [savingPrefs, setSavingPrefs] = useState(false);
  const [chatOpen, setChatOpen] = useState(false);
  const [wsUrl, setWsUrl] = useState(defaultGatewayWsUrl);
  const [chatId, setChatId] = useState('web_console_01');
  const [chatInput, setChatInput] = useState('');
  const [loginUsername, setLoginUsername] = useState('admin');
  const [loginPassword, setLoginPassword] = useState('admin123');
  const [wsConnected, setWsConnected] = useState(false);
  const [wsInstance, setWsInstance] = useState<WebSocket | null>(null);
  const [chatMessages, setChatMessages] = useState<ChatMessage[]>([
    { id: 'system-welcome', role: 'system', tone: 'info', text: '连接网关后即可向 ESP32 Agent 发送消息。', time: new Date().toLocaleTimeString('zh-CN', { hour12: false }) }
  ]);
  const [messageApi, contextHolder] = message.useMessage();
  const chatScrollRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    let cancelled = false;
    const sync = async () => { const next = await fetchDashboard(); if (!cancelled && next.nodes.length) setPayload(next); };
    void sync();
    const timer = window.setInterval(sync, 3000);
    return () => { cancelled = true; window.clearInterval(timer); };
  }, []);

  useEffect(() => {
    if (viewMode !== 'Skills Studio') return;
    let cancelled = false;
    void fetchRuntimeSkills().then((next) => { if (!cancelled) { setRuntimeSkills(next.skills); setRuntimeSkillSource(next.source); setRuntimeSkillError(next.error || ''); } });
    return () => { cancelled = true; };
  }, [viewMode]);

  useEffect(() => { localStorage.setItem(DRAFT_SKILLS_STORAGE_KEY, JSON.stringify(draftSkills)); }, [draftSkills]);
  useEffect(() => { localStorage.setItem(PREFS_STORAGE_KEY, JSON.stringify(preferences)); }, [preferences]);
  useEffect(() => { if (session) localStorage.setItem(AUTH_STORAGE_KEY, JSON.stringify(session)); else localStorage.removeItem(AUTH_STORAGE_KEY); }, [session]);
  useEffect(() => { if (!draftSkills.some((item) => item.id === activeSkillId)) setActiveSkillId(draftSkills[0]?.id || ''); }, [activeSkillId, draftSkills]);
  useEffect(() => {
    if (!runtimeSkills.length) {
      if (activeRuntimeSkillId) setActiveRuntimeSkillId('');
      return;
    }
    if (!runtimeSkills.some((item) => item.runtimeName === activeRuntimeSkillId)) {
      setActiveRuntimeSkillId(runtimeSkills[0].runtimeName);
    }
  }, [activeRuntimeSkillId, runtimeSkills]);
  useEffect(() => () => wsInstance?.close(), [wsInstance]);
  useEffect(() => { if (chatScrollRef.current) chatScrollRef.current.scrollTop = chatScrollRef.current.scrollHeight; }, [chatMessages]);

  const activeSkill = useMemo(() => draftSkills.find((item) => item.id === activeSkillId) || null, [activeSkillId, draftSkills]);
  const activeRuntimeSkill = useMemo(() => runtimeSkills.find((item) => item.runtimeName === activeRuntimeSkillId) || null, [activeRuntimeSkillId, runtimeSkills]);
  const canEditSkills = session?.role === 'admin';
  const canUseChat = session?.role !== 'viewer';
  const isLive = Boolean(payload.mqtt);
  const sandboxEvents = useMemo(() => payload.timeline.filter(isSandboxEvent), [payload.timeline]);
  const filteredTimeline = useMemo(() => payload.timeline.filter((event) => {
    if (timelineFilter === '全部') return true;
    if (timelineFilter === 'telemetry') return event.stage.includes('telemetry');
    if (timelineFilter === 'state') return event.stage.includes('state');
    if (timelineFilter === 'policy') return event.stage.includes('policy') || event.stage.includes('guardian');
    if (timelineFilter === 'sandbox') return isSandboxEvent(event);
    if (timelineFilter === 'reply') return event.stage.includes('reply') || event.stage.includes('outbound');
    return event.stage.includes('error') || event.status === 'warn';
  }), [payload.timeline, timelineFilter]);

  const fixedNodes = useMemo<NodePanel[]>(() => fixedNodeOrder.map((role) => {
    const node = payload.nodes.find((item) => item.role === role) || { id: role, role, transport: ['MQTT Mesh'], status: 'offline' as const, location: 'not detected', responsibilities: [], surfaces: [] };
    const outgoing = payload.timeline.filter((event) => stageMatchesNode(event.source, node));
    const incoming = payload.timeline.filter((event) => stageMatchesNode(event.target, node));
    const hints = nodeChannelHints[role];
    return {
      node,
      sentSummary: outgoing.length ? outgoing.slice(0, 3).map((event) => `${event.stage}: ${event.payload}`) : hints.sends,
      recvSummary: incoming.length ? incoming.slice(0, 3).map((event) => `${event.stage}: ${event.payload}`) : hints.receives,
      subagentLabel: hints.subagent ? '可用，当前未运行' : '该节点不开放 Subagent'
    };
  }), [payload.nodes, payload.timeline]);

  const nodeStats = useMemo(() => ({
    online: fixedNodes.filter((item) => item.node.status === 'online').length,
    degraded: fixedNodes.filter((item) => item.node.status === 'degraded').length,
    capabilities: payload.capabilities.length,
    skills: draftSkills.length
  }), [draftSkills.length, fixedNodes, payload.capabilities.length]);

  function appendChatMessage(role: ChatMessage['role'], text: string, tone?: ChatMessage['tone']) {
    setChatMessages((current) => [...current, { id: `${role}-${Date.now()}-${Math.random().toString(16).slice(2)}`, role, tone, text, time: new Date().toLocaleTimeString('zh-CN', { hour12: false }) }]);
  }

  function handleLogin() {
    const matched = demoAccounts.find((account) => account.username === loginUsername.trim() && account.password === loginPassword);
    if (!matched) return void messageApi.error('用户名或密码错误');
    setSession({ username: matched.username, role: matched.role });
    messageApi.success(`已登录为 ${matched.username}`);
  }

  function addSkill() {
    if (!canEditSkills) return;
    const draft: SkillDraft = { id: `skill-${Date.now()}`, name: '新建 Skill', scope: 'coordinator_agent', trigger: '用户自定义触发条件', policy: '默认经过 Guardian 审核', prompt: '描述触发、约束、输出和回执格式。', enabled: false };
    setDraftSkills((current) => [draft, ...current]);
    setActiveSkillId(draft.id);
    setSkillEditorMode('draft');
  }

  async function handleSaveSkills() {
    if (!canEditSkills) return;
    setSavingSkills(true);
    try { setDraftSkills(await saveSkills(draftSkills)); messageApi.success('Skill 草稿已保存'); } finally { setSavingSkills(false); }
  }

  async function handleSavePreferences() {
    setSavingPrefs(true);
    try { setPreferences(await savePreferences(preferences)); messageApi.success('偏好已保存'); } finally { setSavingPrefs(false); }
  }

  async function refreshRuntimeSkills() {
    const next = await fetchRuntimeSkills(); setRuntimeSkills(next.skills); setRuntimeSkillSource(next.source); setRuntimeSkillError(next.error || '');
  }

  function patchRuntimeSkill(patch: Partial<RuntimeSkillRecord>) {
    if (!activeRuntimeSkillId) return;
    setRuntimeSkills((current) => current.map((item) => (
      item.runtimeName === activeRuntimeSkillId ? { ...item, ...patch } : item
    )));
  }

  async function handleSaveRuntimeSkill() {
    if (!activeRuntimeSkill || !canEditSkills) return;
    setSavingRuntimeSkillId(activeRuntimeSkill.runtimeName);
    try {
      const result = await updateRuntimeSkill(activeRuntimeSkill, true);
      if (result.ok) {
        if (result.skill) {
          setRuntimeSkills((current) => current.map((item) => (
            item.runtimeName === result.skill?.runtimeName ? result.skill : item
          )));
          setActiveRuntimeSkillId(result.skill.runtimeName);
        }
        messageApi.success(result.message || 'Runtime Skill 已保存');
        await refreshRuntimeSkills();
      } else {
        messageApi.error(result.error || result.message || '保存失败');
      }
    } finally {
      setSavingRuntimeSkillId(null);
    }
  }

  async function handleInstallSkill() {
    if (!activeSkill || !canEditSkills) return;
    setInstallingSkillId(activeSkill.id);
    try {
      const result = await installRuntimeSkill(activeSkill, true);
      if (result.ok) {
        if (result.skill) {
          setRuntimeSkills((current) => [result.skill as RuntimeSkillRecord, ...current.filter((item) => item.runtimeName !== result.skill?.runtimeName)]);
          setActiveRuntimeSkillId(result.skill.runtimeName);
          setSkillEditorMode('runtime');
        }
        messageApi.success(result.message || 'Runtime Skill 已安装');
        await refreshRuntimeSkills();
      }
      else messageApi.error(result.error || result.message || '安装失败');
    } finally { setInstallingSkillId(null); }
  }

  function connectWs() {
    if (!canUseChat) return void messageApi.warning('只读账号不开放 Agent 通信');
    wsInstance?.close();
    try {
      const socket = new WebSocket(wsUrl);
      socket.onopen = () => { setWsConnected(true); appendChatMessage('system', 'WebSocket 已连接', 'success'); };
      socket.onmessage = (event) => {
        try {
          const data = JSON.parse(String(event.data)) as { type?: string; content?: string };
          appendChatMessage(data.type === 'response' ? 'assistant' : 'system', data.content || String(event.data));
        } catch { appendChatMessage('system', String(event.data)); }
      };
      socket.onerror = () => appendChatMessage('system', 'WebSocket 连接或通信出错', 'error');
      socket.onclose = () => { setWsConnected(false); setWsInstance(null); appendChatMessage('system', 'WebSocket 已断开', 'warning'); };
      setWsInstance(socket);
    } catch (error) { appendChatMessage('system', `无法建立连接：${String(error)}`, 'error'); }
  }

  function sendChatMessage() {
    if (!chatInput.trim() || !wsInstance || wsInstance.readyState !== WebSocket.OPEN) return;
    wsInstance.send(JSON.stringify({ type: 'message', content: chatInput.trim(), chat_id: chatId.trim() || 'web_console_01' }));
    appendChatMessage('user', chatInput.trim());
    setChatInput('');
  }

  const theme = { token: { colorPrimary: '#2f6fed', colorBgLayout: '#f2f4f3', colorBgContainer: '#ffffff', colorText: '#18211d', borderRadius: 6, fontFamily: 'Inter, "Segoe UI", sans-serif' } };
  if (!session) return <ConfigProvider theme={theme}><AntdApp>{contextHolder}<LoginScreen username={loginUsername} password={loginPassword} setUsername={setLoginUsername} setPassword={setLoginPassword} login={handleLogin} /></AntdApp></ConfigProvider>;

  const currentMeta = viewMeta[viewMode];
  const statItems = [
    { label: '在线节点', value: nodeStats.online, icon: <NodeIndexOutlined />, tone: 'green' },
    { label: '降级节点', value: nodeStats.degraded, icon: <AuditOutlined />, tone: 'amber' },
    { label: '能力项', value: nodeStats.capabilities, icon: <ThunderboltOutlined />, tone: 'blue' },
    { label: 'Skill 草稿', value: nodeStats.skills, icon: <ControlOutlined />, tone: 'coral' }
  ];

  return (
    <ConfigProvider theme={theme}>
      <AntdApp>
        {contextHolder}
        <div className="console-shell">
          <aside className="console-sidebar">
            <div className="console-brand"><span><DeploymentUnitOutlined /></span><div><strong>ESPAgent</strong><small>Agent Mesh Console</small></div></div>
            <nav className="console-nav" aria-label="主导航">
              {viewItems.map((item) => <button key={item.label} className={viewMode === item.label ? 'is-active' : ''} onClick={() => setViewMode(item.label)}><span>{item.icon}</span><div><strong>{item.label}</strong><small>{item.caption}</small></div></button>)}
            </nav>
            <div className="sidebar-status">
              <div><span className={`status-light ${payload.mqtt?.connected ? 'is-online' : ''}`} /><span>MQTT</span><strong>{payload.mqtt?.connected ? '在线' : '离线'}</strong></div>
              <div><span className={`status-light ${payload.chatGateway?.connectedSessions ? 'is-online' : 'is-idle'}`} /><span>通信网关</span><strong>{payload.chatGateway?.connectedSessions ? '已连接' : '空闲'}</strong></div>
            </div>
          </aside>

          <main className="console-main">
            <header className="workspace-header">
              <div><span>{currentMeta.eyebrow}</span><h1>{currentMeta.title}</h1><p>{currentMeta.description}</p></div>
              <div className="header-actions">
                <Tag color={isLive ? 'success' : 'warning'}>{isLive ? '实时数据' : '演示数据'}</Tag>
                <span className="user-chip"><UserOutlined /> {session.username}<small>{session.role}</small></span>
                <Button icon={<LogoutOutlined />} onClick={() => { wsInstance?.close(); setSession(null); setChatOpen(false); }} aria-label="退出登录" />
                <Button type="primary" icon={<MessageOutlined />} onClick={() => setChatOpen(true)}>Agent 通信</Button>
              </div>
            </header>

            <section className="summary-strip" aria-label="运行摘要">
              {statItems.map((item) => <div key={item.label} className={`summary-item tone-${item.tone}`}><span>{item.icon}</span><div><small>{item.label}</small><strong>{item.value}</strong></div></div>)}
              <div className="summary-source"><ApiOutlined /><div><small>Topic Prefix</small><strong>{payload.mqtt?.topicPrefix || 'local mock'}</strong></div></div>
            </section>

            <div className="workspace-content">
              {viewMode === '总览' && <OverviewView payload={payload} nodes={fixedNodes} isLive={isLive} />}
              {viewMode === '协作链路' && <CollaborationView payload={payload} filteredTimeline={filteredTimeline} filter={timelineFilter} setFilter={setTimelineFilter} />}
              {viewMode === 'Sandbox' && <SandboxView events={sandboxEvents} warnCount={sandboxEvents.filter((event) => event.status === 'warn').length} latest={sandboxEvents[0] || null} command={SANDBOX_DENY_TEST_COMMAND} />}
              {viewMode === 'Skills Studio' && (
                <SkillsView
                  drafts={draftSkills}
                  active={activeSkill}
                  activeId={activeSkillId}
                  editorMode={skillEditorMode}
                  setEditorMode={setSkillEditorMode}
                  setActiveId={setActiveSkillId}
                  patchSkill={(patch) => setDraftSkills((current) => current.map((item) => item.id === activeSkillId ? { ...item, ...patch } : item))}
                  canEdit={canEditSkills}
                  addSkill={addSkill}
                  saveSkills={handleSaveSkills}
                  saving={savingSkills}
                  install={handleInstallSkill}
                  installing={installingSkillId === activeSkillId}
                  runtimeSkills={runtimeSkills}
                  activeRuntime={activeRuntimeSkill}
                  activeRuntimeId={activeRuntimeSkillId}
                  setActiveRuntimeId={setActiveRuntimeSkillId}
                  patchRuntimeSkill={patchRuntimeSkill}
                  saveRuntimeSkill={handleSaveRuntimeSkill}
                  savingRuntime={savingRuntimeSkillId === activeRuntimeSkillId}
                  runtimeSource={runtimeSkillSource}
                  runtimeError={runtimeSkillError}
                  refresh={refreshRuntimeSkills}
                />
              )}
              {viewMode === '用户偏好' && <PreferencesView preferences={preferences} setPreferences={setPreferences} save={handleSavePreferences} saving={savingPrefs} username={session.username} role={session.role} />}
            </div>
          </main>
        </div>

        <ChatWorkspace open={chatOpen} onClose={() => setChatOpen(false)} username={session.username} role={session.role} canUseChat={canUseChat} wsUrl={wsUrl} setWsUrl={setWsUrl} chatId={chatId} setChatId={setChatId} connected={wsConnected} connect={connectWs} disconnect={() => wsInstance?.close()} chatInput={chatInput} setChatInput={setChatInput} send={sendChatMessage} messages={chatMessages} timeline={payload.timeline} chatGateway={payload.chatGateway} scrollRef={chatScrollRef} />
      </AntdApp>
    </ConfigProvider>
  );
}

export default App;
