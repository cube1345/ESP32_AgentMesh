import {
  Alert,
  App as AntdApp,
  Badge,
  Button,
  Card,
  Col,
  ConfigProvider,
  Divider,
  Empty,
  Form,
  Input,
  Layout,
  List,
  Modal,
  Progress,
  Row,
  Segmented,
  Select,
  Slider,
  Space,
  Statistic,
  Switch,
  Table,
  Tag,
  Timeline,
  Typography,
  message
} from 'antd';
import {
  ApiOutlined,
  AuditOutlined,
  BgColorsOutlined,
  BranchesOutlined,
  ControlOutlined,
  DashboardOutlined,
  DeploymentUnitOutlined,
  NodeIndexOutlined,
  SafetyCertificateOutlined,
  ThunderboltOutlined
} from '@ant-design/icons';
import { useEffect, useMemo, useState, type ReactNode } from 'react';
import { mockDashboardPayload } from './data/mock';
import { fetchDashboard, savePreferences, saveSkills } from './services/dashboard';
import type { AgentNode, Capability, DashboardPayload, EnvironmentMetric, SkillDraft, TimelineEvent, UserPreferenceProfile } from './types';

const { Header, Content, Sider } = Layout;
const { Title, Paragraph, Text } = Typography;

type ViewMode = '总览' | '协作链路' | 'Skills Studio' | '用户偏好';
type TimelineFilter = '全部' | 'telemetry' | 'state' | 'policy' | 'reply' | 'error';
type Role = 'admin' | 'operator' | 'viewer';

const DRAFT_SKILLS_STORAGE_KEY = 'espagent.console.skillDrafts';
const PREFS_STORAGE_KEY = 'espagent.console.preferences';
const AUTH_STORAGE_KEY = 'espagent.console.auth';

interface UserSession {
  username: string;
  role: Role;
}

interface ChatMessage {
  id: string;
  role: 'user' | 'assistant' | 'system';
  text: string;
  time: string;
}

const demoAccounts: Array<UserSession & { password: string; description: string }> = [
  { username: 'admin', password: 'admin123', role: 'admin', description: '全功能演示账号' },
  { username: 'operator', password: 'operator123', role: 'operator', description: '调度与监控账号' },
  { username: 'viewer', password: 'viewer123', role: 'viewer', description: '只读展示账号' }
];

const viewItems: { label: ViewMode; icon: ReactNode }[] = [
  { label: '总览', icon: <DashboardOutlined /> },
  { label: '协作链路', icon: <BranchesOutlined /> },
  { label: 'Skills Studio', icon: <ControlOutlined /> },
  { label: '用户偏好', icon: <SafetyCertificateOutlined /> }
];

const fixedNodeOrder: AgentNode['role'][] = ['coordinator_agent', 'sensor_agent', 'control_agent', 'guardian_agent'];

const nodeChannelHints: Record<AgentNode['role'], { sends: string[]; receives: string[]; subagent: boolean }> = {
  coordinator_agent: {
    sends: ['dispatch', 'timeline', 'mesh_command', 'voice_request'],
    receives: ['feishu/websocket input', 'tool_result', 'mesh reply', 'stt_result'],
    subagent: true
  },
  sensor_agent: {
    sends: ['telemetry', 'environment', 'threshold event'],
    receives: ['agent_task', 'role command', 'policy gated request'],
    subagent: false
  },
  control_agent: {
    sends: ['control_state', 'tool_result', 'actuator result'],
    receives: ['mesh_command', 'policy_decision', 'voice_tts_status'],
    subagent: false
  },
  guardian_agent: {
    sends: ['policy_decision', 'audit', 'stateboard'],
    receives: ['policy_check', 'timeline observe', 'node state/telemetry'],
    subagent: false
  },
  display_terminal: {
    sends: ['ui event', 'voice front-end event'],
    receives: ['timeline', 'dashboard sync', 'tts request'],
    subagent: false
  }
};

function readStorage<T>(key: string, fallback: T): T {
  try {
    const raw = localStorage.getItem(key);
    return raw ? (JSON.parse(raw) as T) : fallback;
  } catch {
    return fallback;
  }
}

function statusColor(status: AgentNode['status'] | TimelineEvent['status']): string {
  switch (status) {
    case 'online':
    case 'ok':
      return 'green';
    case 'degraded':
    case 'queued':
      return 'gold';
    default:
      return 'red';
  }
}

function metricPercent(metric: EnvironmentMetric): number {
  if (metric.label === '温度') return Math.min((metric.value / 40) * 100, 100);
  if (metric.label === '湿度') return Math.min(metric.value, 100);
  if (metric.label === 'eCO2') return Math.min((metric.value / 1200) * 100, 100);
  if (metric.label === 'TVOC') return Math.min((metric.value / 220) * 100, 100);
  if (metric.label === '光照') return Math.min((metric.value / 300) * 100, 100);
  return metric.value > 0 ? 100 : 0;
}

function maturityTag(maturity: Capability['maturity']): string {
  if (maturity === '已验证') return 'success';
  if (maturity === '进行中') return 'processing';
  return 'default';
}

function stageMatchesNode(value: string, node: AgentNode): boolean {
  return value.includes(node.id) || value.includes(node.role);
}

const capabilityColumns = [
  { title: '能力', dataIndex: 'name', key: 'name', render: (value: string) => <Text strong>{value}</Text> },
  { title: '类别', dataIndex: 'category', key: 'category', render: (value: string) => <Tag color="blue">{value}</Tag> },
  { title: '角色', dataIndex: 'role', key: 'role' },
  { title: '状态', dataIndex: 'maturity', key: 'maturity', render: (value: Capability['maturity']) => <Tag color={maturityTag(value)}>{value}</Tag> },
  { title: '说明', dataIndex: 'summary', key: 'summary' }
];

function App() {
  const [payload, setPayload] = useState<DashboardPayload>(mockDashboardPayload);
  const [draftSkills, setDraftSkills] = useState<SkillDraft[]>(() => readStorage(DRAFT_SKILLS_STORAGE_KEY, mockDashboardPayload.skills));
  const [preferences, setPreferences] = useState<UserPreferenceProfile>(() => readStorage(PREFS_STORAGE_KEY, mockDashboardPayload.preferences));
  const [session, setSession] = useState<UserSession | null>(() => readStorage<UserSession | null>(AUTH_STORAGE_KEY, null));
  const [viewMode, setViewMode] = useState<ViewMode>('总览');
  const [timelineFilter, setTimelineFilter] = useState<TimelineFilter>('全部');
  const [activeSkillId, setActiveSkillId] = useState<string>(mockDashboardPayload.skills[0]?.id ?? '');
  const [savingSkills, setSavingSkills] = useState(false);
  const [savingPrefs, setSavingPrefs] = useState(false);
  const [chatOpen, setChatOpen] = useState(false);
  const [wsUrl, setWsUrl] = useState('ws://127.0.0.1:18789/');
  const [chatId, setChatId] = useState('web_console_01');
  const [chatInput, setChatInput] = useState('');
  const [loginUsername, setLoginUsername] = useState('admin');
  const [loginPassword, setLoginPassword] = useState('admin123');
  const [wsConnected, setWsConnected] = useState(false);
  const [wsInstance, setWsInstance] = useState<WebSocket | null>(null);
  const [chatMessages, setChatMessages] = useState<ChatMessage[]>([
    { id: 'system-welcome', role: 'system' as const, text: '这里模拟飞书式消息入口，消息经 WebSocket 发到 ESP32 网关。', time: new Date().toLocaleTimeString('zh-CN', { hour12: false }) }
  ]);
  const [messageApi, contextHolder] = message.useMessage();

  useEffect(() => {
    let cancelled = false;
    const syncDashboard = async () => {
      const next = await fetchDashboard();
      if (!cancelled && next.nodes.length > 0) setPayload(next);
    };
    syncDashboard();
    const timer = window.setInterval(syncDashboard, 3000);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, []);

  useEffect(() => {
    localStorage.setItem(DRAFT_SKILLS_STORAGE_KEY, JSON.stringify(draftSkills));
  }, [draftSkills]);

  useEffect(() => {
    localStorage.setItem(PREFS_STORAGE_KEY, JSON.stringify(preferences));
  }, [preferences]);

  useEffect(() => {
    if (session) localStorage.setItem(AUTH_STORAGE_KEY, JSON.stringify(session));
    else localStorage.removeItem(AUTH_STORAGE_KEY);
  }, [session]);

  useEffect(() => {
    if (!draftSkills.find((item) => item.id === activeSkillId)) {
      setActiveSkillId(draftSkills[0]?.id ?? '');
    }
  }, [activeSkillId, draftSkills]);

  useEffect(() => () => wsInstance?.close(), [wsInstance]);

  const activeSkill = useMemo(() => draftSkills.find((item) => item.id === activeSkillId) ?? null, [activeSkillId, draftSkills]);
  const canEditSkills = session?.role === 'admin';
  const canUseChat = session?.role !== 'viewer';
  const filteredTimeline = useMemo(() => payload.timeline.filter((event) => {
    if (timelineFilter === '全部') return true;
    if (timelineFilter === 'telemetry') return event.stage.includes('telemetry');
    if (timelineFilter === 'state') return event.stage.includes('state');
    if (timelineFilter === 'policy') return event.stage.includes('policy') || event.stage.includes('guardian');
    if (timelineFilter === 'reply') return event.stage.includes('reply') || event.stage.includes('outbound');
    return event.stage.includes('error') || event.status === 'warn';
  }), [payload.timeline, timelineFilter]);

  const fixedNodePanels = useMemo(() => fixedNodeOrder.map((role) => {
    const node = payload.nodes.find((item) => item.role === role) ?? { id: role, role, transport: ['MQTT Mesh'], status: 'offline', location: 'not detected', responsibilities: [], surfaces: [] };
    const outgoing = payload.timeline.filter((event) => stageMatchesNode(event.source, node));
    const incoming = payload.timeline.filter((event) => stageMatchesNode(event.target, node));
    const subagentEvents = payload.timeline.filter((event) => (stageMatchesNode(event.source, node) || stageMatchesNode(event.target, node)) && (event.stage.includes('subagent') || event.payload.toLowerCase().includes('subagent')));
    const hints = nodeChannelHints[role];
    return {
      node,
      sentSummary: outgoing.length ? outgoing.slice(0, 3).map((event) => `${event.stage}: ${event.payload}`) : hints.sends.map((item) => `固定职责: ${item}`),
      recvSummary: incoming.length ? incoming.slice(0, 3).map((event) => `${event.stage}: ${event.payload}`) : hints.receives.map((item) => `固定职责: ${item}`),
      subagentLabel: hints.subagent ? subagentEvents[0]?.payload ?? '可用，当前未运行' : '该节点不开放 subagent'
    };
  }), [payload.nodes, payload.timeline]);

  const nodeStats = useMemo(() => ({
    online: payload.nodes.filter((node) => node.status === 'online').length,
    degraded: payload.nodes.filter((node) => node.status === 'degraded').length,
    capabilityCount: payload.capabilities.length,
    skillCount: draftSkills.length
  }), [draftSkills.length, payload.capabilities.length, payload.nodes]);

  const chatRelatedTimeline = useMemo(() => payload.timeline.slice(0, 8), [payload.timeline]);

  function appendChatMessage(role: 'user' | 'assistant' | 'system', text: string) {
    setChatMessages((current) => [...current, { id: `${role}-${Date.now()}-${Math.random().toString(16).slice(2, 8)}`, role, text, time: new Date().toLocaleTimeString('zh-CN', { hour12: false }) }]);
  }

  function patchSkill(patch: Partial<SkillDraft>) {
    setDraftSkills((current) => current.map((item) => item.id === activeSkillId ? { ...item, ...patch } : item));
  }

  function handleLogin() {
    const matched = demoAccounts.find((account) => account.username === loginUsername.trim() && account.password === loginPassword);
    if (!matched) return void messageApi.error('用户名或密码错误');
    setSession({ username: matched.username, role: matched.role });
    messageApi.success(`已登录为 ${matched.username}`);
  }

  function handleLogout() {
    wsInstance?.close();
    setSession(null);
    setChatOpen(false);
  }

  function addSkill() {
    if (!canEditSkills) return;
    const draft: SkillDraft = { id: `skill-${Date.now()}`, name: '新建 Skill', scope: 'coordinator_agent', trigger: '用户自定义触发条件', policy: '默认走 Guardian 审核', prompt: '描述这个 skill 的触发、约束、输出和回执格式。', enabled: false };
    setDraftSkills((current) => [draft, ...current]);
    setActiveSkillId(draft.id);
  }

  async function handleSaveSkills() {
    if (!canEditSkills) return;
    setSavingSkills(true);
    try {
      const next = await saveSkills(draftSkills);
      setDraftSkills(next);
      messageApi.success('前端草稿已保存，本次不会自动写入 ESP32');
    } finally {
      setSavingSkills(false);
    }
  }

  async function handleSavePreferences() {
    setSavingPrefs(true);
    try {
      const next = await savePreferences(preferences);
      setPreferences(next);
      messageApi.success('用户偏好已保存');
    } finally {
      setSavingPrefs(false);
    }
  }

  function connectWs() {
    if (!canUseChat) return void messageApi.warning('viewer 账号只读，不开放 AI 通信');
    wsInstance?.close();
    try {
      const socket = new WebSocket(wsUrl);
      socket.onopen = () => { setWsConnected(true); appendChatMessage('system', `WebSocket 已连接：${wsUrl}`); };
      socket.onmessage = (event) => {
        try {
          const data = JSON.parse(String(event.data)) as { type?: string; content?: string };
          appendChatMessage(data.type === 'response' && data.content ? 'assistant' : 'system', data.content || String(event.data));
        } catch {
          appendChatMessage('system', String(event.data));
        }
      };
      socket.onerror = () => appendChatMessage('system', 'WebSocket 连接或通信出错');
      socket.onclose = () => { setWsConnected(false); setWsInstance(null); appendChatMessage('system', 'WebSocket 已断开'); };
      setWsInstance(socket);
    } catch (error) {
      appendChatMessage('system', `无法创建 WebSocket：${String(error)}`);
    }
  }

  function sendChatMessage() {
    if (!canUseChat) return void messageApi.warning('viewer 账号只读，不开放 AI 通信');
    if (!chatInput.trim()) return;
    if (!wsInstance || wsInstance.readyState !== WebSocket.OPEN) return void messageApi.warning('请先连接 ESP32 的 WebSocket 网关');
    wsInstance.send(JSON.stringify({ type: 'message', content: chatInput.trim(), chat_id: chatId.trim() || 'web_console_01' }));
    appendChatMessage('user', chatInput.trim());
    setChatInput('');
  }

  if (!session) {
    return (
      <ConfigProvider theme={{ token: { colorPrimary: '#1677ff', colorBgLayout: '#f5f7fb', colorBgContainer: '#ffffff', borderRadius: 8 } }}>
        <AntdApp>
          {contextHolder}
          <div className="flex min-h-screen items-center justify-center bg-slate-50 px-4 py-10">
            <div className="grid w-full max-w-5xl gap-6 lg:grid-cols-[1.2fr_420px]">
              <Card bordered={false} className="glass-panel rounded-lg">
                <Title level={2}>ESPAgent Console</Title>
                <Paragraph className="!text-slate-500">
                  面向四节点协同调度、环境展示、AI 通信与技能草稿管理的统一前端。
                </Paragraph>
                <div className="mt-6 grid gap-4 md:grid-cols-2">
                  <div className="rounded-lg border border-slate-200 bg-slate-50 p-4 text-sm text-slate-600">
                    <div className="mb-2 font-semibold text-slate-800">当前能力</div>
                    <div>真实 MQTT telemetry / state / timeline 聚合</div>
                    <div>四节点固定通信面板</div>
                    <div>AI 通信面板（WebSocket to agent_loop）</div>
                    <div>Skills 草稿与 runtime skill 分离展示</div>
                  </div>
                  <div className="rounded-lg border border-slate-200 bg-slate-50 p-4 text-sm text-slate-600">
                    <div className="mb-2 font-semibold text-slate-800">演示账号</div>
                    {demoAccounts.map((account) => (
                      <div key={account.username} className="mb-2">
                        <div>{account.username} / {account.password}</div>
                        <div className="text-xs text-slate-500">{account.description}</div>
                      </div>
                    ))}
                  </div>
                </div>
              </Card>
              <Card bordered={false} className="glass-panel rounded-lg" title="登录系统">
                <Form layout="vertical" onFinish={handleLogin}>
                  <Form.Item label="用户名">
                    <Input value={loginUsername} onChange={(e) => setLoginUsername(e.target.value)} />
                  </Form.Item>
                  <Form.Item label="密码">
                    <Input.Password value={loginPassword} onChange={(e) => setLoginPassword(e.target.value)} />
                  </Form.Item>
                  <Button type="primary" htmlType="submit" block>
                    登录
                  </Button>
                </Form>
                <Divider />
                <Text type="secondary">
                  当前是前端本地登录守卫。真实鉴权、Token 和 Guardian 身份联动仍需后端链路补齐。
                </Text>
              </Card>
            </div>
          </div>
        </AntdApp>
      </ConfigProvider>
    );
  }

  return (
    <ConfigProvider theme={{ token: { colorPrimary: '#1677ff', colorBgLayout: '#f5f7fb', colorBgContainer: '#ffffff', borderRadius: 8 } }}>
      <AntdApp>
        {contextHolder}
        <Layout className="page-shell min-h-screen">
          <Sider breakpoint="lg" collapsedWidth="0" width={360} className="border-r border-slate-200">
            <div className="flex h-full flex-col px-5 py-5">
              <div className="mb-6">
                <div className="mb-3 flex items-center gap-3"><div className="flex h-11 w-11 items-center justify-center rounded-lg bg-cyan-500/12 text-cyan-300"><DeploymentUnitOutlined className="text-xl" /></div><div><div className="text-base font-semibold text-slate-900">ESPAgent Console</div><div className="text-xs text-slate-500">LingShu Agent Mesh Frontend</div></div></div>
                <Paragraph className="!mb-0 !text-slate-500">面向四个 ESP32-S3 节点、ESP32-P4 / Android 终端的统一调度与展示界面。</Paragraph>
              </div>
              <Segmented block className="mb-5" value={viewMode} onChange={(value) => setViewMode(value as ViewMode)} options={viewItems.map((item) => ({ label: <span className="flex items-center gap-2">{item.icon}{item.label}</span>, value: item.label }))} />
              <Card bordered={false} className="glass-panel rounded-lg"><div className="mb-3 flex items-center justify-between"><Text>节点健康</Text><Badge status="processing" text="live data" /></div><Space direction="vertical" size={12} className="w-full">{payload.nodes.map((node) => <div key={node.id} className="rounded-lg border border-slate-200 px-3 py-2"><div className="mb-1 flex items-center justify-between"><Text>{node.role}</Text><Tag color={statusColor(node.status)}>{node.status}</Tag></div><div className="text-xs text-slate-500">{node.id}</div><div className="mt-2 text-xs text-slate-400">{node.location}</div></div>)}</Space></Card>
              <Card bordered={false} className="glass-panel mt-4 rounded-lg"><div className="mb-2 flex items-center gap-2 text-slate-900"><ApiOutlined />通信面</div><div className="space-y-2 text-xs text-slate-500"><div>Feishu / WebSocket / Serial CLI</div><div>MQTT Mesh / ESP-NOW / RMT / I2S</div><div>ESP32-P4 / Android / Web Console</div></div></Card>
            </div>
          </Sider>
          <Layout>
            <Header className="border-b border-slate-200 px-6 !bg-transparent">
              <div className="flex h-full items-center justify-between gap-4">
                <div><Title level={3} className="!mb-0">多 Agent 协同边缘可视化控制台</Title><Text className="!text-slate-500">展示调度链路、能力目录、环境状态、技能草稿与终端联动。</Text></div>
                <Space size="middle" wrap><Tag color={payload.mqtt?.connected ? 'green' : 'red'}>MQTT {payload.mqtt?.connected ? 'connected' : 'offline'}</Tag><Tag color="blue">{payload.mqtt?.topicPrefix || 'mock-prefix'}</Tag><Tag color="purple">{session.username} / {session.role}</Tag><Button onClick={handleLogout}>退出</Button><Button type="primary" onClick={() => setChatOpen(true)}>AI 通信</Button></Space>
              </div>
            </Header>
            <Content className="px-6 py-6">
              <Row gutter={[16, 16]}>
                <Col xs={24} md={12} xl={6}><Card bordered={false} className="metric-shell rounded-lg"><Statistic title="在线节点" value={nodeStats.online} prefix={<NodeIndexOutlined />} /></Card></Col>
                <Col xs={24} md={12} xl={6}><Card bordered={false} className="metric-shell rounded-lg"><Statistic title="降级节点" value={nodeStats.degraded} prefix={<AuditOutlined />} /></Card></Col>
                <Col xs={24} md={12} xl={6}><Card bordered={false} className="metric-shell rounded-lg"><Statistic title="能力项" value={nodeStats.capabilityCount} prefix={<ThunderboltOutlined />} /></Card></Col>
                <Col xs={24} md={12} xl={6}><Card bordered={false} className="metric-shell rounded-lg"><Statistic title="Skill 草稿" value={nodeStats.skillCount} prefix={<BgColorsOutlined />} /></Card></Col>
              </Row>

              {viewMode === '总览' && <Row gutter={[16, 16]} className="mt-4">
                <Col xs={24}><Card title="四节点拓扑与主要链路" className="glass-panel rounded-lg"><div className="grid gap-4 xl:grid-cols-[1fr_340px]"><div className="grid gap-4 md:grid-cols-2">{fixedNodePanels.map(({ node }) => <div key={node.role} className="rounded-lg border border-slate-200 bg-slate-50 p-4"><div className="mb-2 flex items-center justify-between"><div className="text-sm font-semibold text-slate-900">{node.role}</div><Tag color={statusColor(node.status)}>{node.status}</Tag></div><div className="text-xs text-slate-500">{node.id}</div><div className="mt-3 flex flex-wrap gap-2">{node.transport.map((item) => <Tag key={item}>{item}</Tag>)}</div></div>)}</div><div className="rounded-lg border border-slate-200 bg-slate-50 p-4"><div className="mb-3 text-sm font-semibold text-slate-900">主要通信边</div><div className="space-y-3">{[{ id: '1', title: '用户入口', from: 'Feishu / Web / CLI', to: 'coordinator_agent', detail: '自然语言指令进入 Coordinator' }, { id: '2', title: '策略裁决', from: 'coordinator_agent', to: 'guardian_agent', detail: 'policy_check / decision' }, { id: '3', title: '环境采集', from: 'sensor_agent', to: 'coordinator_agent', detail: 'telemetry / threshold event' }, { id: '4', title: '控制执行', from: 'coordinator_agent', to: 'control_agent', detail: 'mesh_command / actuator result' }].map((edge) => <div key={edge.id} className="rounded-lg border border-slate-200 bg-white p-3"><div className="mb-1 text-sm font-medium text-slate-800">{edge.title}</div><div className="text-xs text-slate-500">{edge.from} → {edge.to}</div><div className="mt-1 text-sm text-slate-600">{edge.detail}</div></div>)}</div></div></div></Card></Col>
                <Col xs={24}><Card title="四节点固定通信面板" className="glass-panel rounded-lg"><div className="grid gap-4 xl:grid-cols-2 2xl:grid-cols-4">{fixedNodePanels.map(({ node, sentSummary, recvSummary, subagentLabel }) => <div key={node.role} className="rounded-lg border border-slate-200 p-4"><div className="mb-3 flex items-center justify-between"><div><div className="text-base font-semibold text-slate-900">{node.role}</div><div className="text-xs text-slate-500">{node.id}</div></div><Tag color={statusColor(node.status)}>{node.status}</Tag></div><div className="mb-4 rounded-lg bg-slate-50 p-3"><div className="mb-1 text-xs text-slate-500">发送数据</div>{sentSummary.map((item) => <div key={item} className="text-sm text-slate-700">{item}</div>)}</div><div className="mb-4 rounded-lg bg-slate-50 p-3"><div className="mb-1 text-xs text-slate-500">接收数据</div>{recvSummary.map((item) => <div key={item} className="text-sm text-slate-700">{item}</div>)}</div><div className="rounded-lg bg-slate-50 p-3"><div className="mb-2 flex items-center justify-between"><div className="text-xs text-slate-500">Subagent</div><Tag color={nodeChannelHints[node.role].subagent ? 'blue' : 'default'}>{nodeChannelHints[node.role].subagent ? 'supported' : 'not supported'}</Tag></div><div className="text-sm text-slate-700">{subagentLabel}</div></div><div className="mt-3 rounded-lg bg-slate-50 p-3"><div className="mb-1 text-xs text-slate-500">本地任务模式</div><div className="text-sm text-slate-700">{node.role === 'coordinator_agent' ? '可本地发起 subagent，也可向远端角色下发 agent_task。' : '默认不开放 subagent，但可被 coordinator 以 agent_task 委托本地 AI 回合。'}</div></div></div>)}</div></Card></Col>
                <Col xs={24}><Card title="真实数据接入状态" className="glass-panel rounded-lg"><div className="grid gap-4 md:grid-cols-4">{[{ label: 'MQTT Broker', value: payload.mqtt?.url || 'mock mode' }, { label: 'Topic Prefix', value: payload.mqtt?.topicPrefix || '-' }, { label: 'Last Event', value: payload.mqtt?.lastEventAt || '暂无' }, { label: 'Guardian StateBoard', value: payload.guardian?.updatedAt || '暂无' }].map((item) => <div key={item.label} className="rounded-lg border border-slate-200 p-4"><div className="text-xs text-slate-500">{item.label}</div><div className="mt-2 text-sm font-medium text-slate-900">{item.value}</div></div>)}</div></Card></Col>
                <Col xs={24} xl={15}><Card title="项目能力总览" className="glass-panel rounded-lg" extra={<Tag color="blue">React + TypeScript + Antd + Tailwind</Tag>}><Table rowKey="name" dataSource={payload.capabilities} columns={capabilityColumns} pagination={false} size="middle" /></Card></Col>
                <Col xs={24} xl={9}><Card title="环境面板" className="glass-panel rounded-lg"><Space direction="vertical" size={16} className="w-full">{payload.environment.map((metric) => <div key={metric.label} className="rounded-lg border border-slate-200 px-4 py-3"><div className="mb-2 flex items-center justify-between"><Text>{metric.label}</Text><Tag color={metric.status === 'good' ? 'green' : metric.status === 'attention' ? 'gold' : 'red'}>{metric.status}</Tag></div><div className="mb-3 text-xl font-semibold text-slate-900">{metric.value} {metric.unit}</div><Progress percent={Math.round(metricPercent(metric))} showInfo={false} strokeColor={metric.status === 'good' ? '#22c55e' : metric.status === 'attention' ? '#f59e0b' : '#ef4444'} /><div className="mt-2 text-xs text-slate-500">趋势 {metric.trend > 0 ? '+' : ''}{metric.trend}</div></div>)}</Space></Card></Col>
                <Col xs={24} xl={12}><Card title="多节点角色分工" className="glass-panel rounded-lg"><List itemLayout="vertical" dataSource={payload.nodes} renderItem={(node) => <List.Item key={node.id}><div className="flex flex-wrap items-start justify-between gap-3"><div><Space size={8}><Text strong>{node.role}</Text><Tag color={statusColor(node.status)}>{node.status}</Tag></Space><div className="mt-1 text-sm text-slate-500">{node.id}</div><div className="mt-2 flex flex-wrap gap-2">{node.transport.map((item) => <Tag key={item}>{item}</Tag>)}</div></div><div className="text-right text-sm text-slate-500">{node.location}</div></div><div className="mt-3 grid gap-2 md:grid-cols-2"><div><div className="mb-1 text-xs text-slate-500">职责</div><div className="flex flex-wrap gap-2">{node.responsibilities.map((item) => <Tag color="geekblue" key={item}>{item}</Tag>)}</div></div><div><div className="mb-1 text-xs text-slate-500">终端面</div><div className="flex flex-wrap gap-2">{node.surfaces.map((item) => <Tag color="purple" key={item}>{item}</Tag>)}</div></div></div></List.Item>} /></Card></Col>
                <Col xs={24} xl={12}><Card title="前端已落地能力" className="glass-panel rounded-lg"><Timeline items={[{ color: 'green', children: '真实环境数据、真实节点状态、真实 timeline 已接入。' }, { color: 'blue', children: '前端草稿 skill、本地偏好、实时接入状态卡片已经可用。' }, { color: 'gold', children: '前端不会自动把 skill 安装到 ESP32 SPIFFS。' }, { color: 'purple', children: `当前登录用户：${session.username} / ${session.role}` }]} /><Divider /><Alert type="info" showIcon message="前端优先使用真实 /api/dashboard" description="只有 dashboard 接口失效时才会回退到本地 mock。登录系统当前是前端本地守卫，不属于板端真实权限链路。" /></Card></Col>
              </Row>}

              {viewMode === '协作链路' && <Row gutter={[16, 16]} className="mt-4">
                <Col xs={24} xl={10}><Card title="通信流程图" className="glass-panel rounded-lg"><Space direction="vertical" size={14} className="w-full">{payload.flows.map((flow, index) => <div key={flow.id} className="rounded-lg border border-slate-200 p-4"><div className="mb-2 flex items-center justify-between"><Space><div className="flex h-8 w-8 items-center justify-center rounded-lg bg-cyan-500/12 text-cyan-300">{index + 1}</div><Text strong>{flow.step}</Text></Space><Tag color="cyan">{flow.transport}</Tag></div><div className="text-sm text-slate-700">{flow.producer} → {flow.consumer}</div><div className="mt-2 text-xs text-slate-500">{flow.topic}</div><div className="mt-2 text-sm text-slate-600">{flow.detail}</div></div>)}</Space></Card></Col>
                <Col xs={24} xl={14}><Card title="Timeline" className="glass-panel rounded-lg" extra={<Segmented value={timelineFilter} onChange={(value) => setTimelineFilter(value as TimelineFilter)} options={['全部', 'telemetry', 'state', 'policy', 'reply', 'error']} />}>{filteredTimeline.length ? <Timeline items={filteredTimeline.map((event) => ({ color: statusColor(event.status), children: <div><div className="flex flex-wrap items-center gap-2"><Text strong>{event.stage}</Text><Tag color={statusColor(event.status)}>{event.status}</Tag></div><div className="text-xs text-slate-500">{event.time} · {event.source} → {event.target}</div><div className="mt-1 text-sm text-slate-700">{event.payload}</div></div> }))} /> : <Empty description="当前筛选条件下没有事件" />}</Card></Col>
              </Row>}

              {viewMode === 'Skills Studio' && <Row gutter={[16, 16]} className="mt-4">
                <Col xs={24} xl={8}><Card title="Skill 草稿列表" className="glass-panel rounded-lg" extra={<Button type="primary" onClick={addSkill} disabled={!canEditSkills}>新增</Button>}><List dataSource={draftSkills} renderItem={(item) => <List.Item key={item.id} className={`cursor-pointer rounded-lg px-3 ${item.id === activeSkillId ? 'bg-slate-50' : ''}`} onClick={() => setActiveSkillId(item.id)}><div className="w-full"><div className="flex items-center justify-between gap-3"><Text strong>{item.name}</Text><Tag color={item.enabled ? 'green' : 'default'}>{item.enabled ? 'enabled' : 'disabled'}</Tag></div><div className="mt-1 text-xs text-slate-500">{item.scope}</div></div></List.Item>} /></Card></Col>
                <Col xs={24} xl={16}><Card title="Skill 编辑器" className="glass-panel rounded-lg" extra={<Space><Tag color={canEditSkills ? 'green' : 'gold'}>{canEditSkills ? 'admin 可写' : '当前只读'}</Tag><Button type="primary" loading={savingSkills} onClick={handleSaveSkills} disabled={!canEditSkills}>保存草稿</Button></Space>}>{activeSkill ? <Form layout="vertical"><Form.Item label="名称"><Input value={activeSkill.name} onChange={(e) => patchSkill({ name: e.target.value })} disabled={!canEditSkills} /></Form.Item><Row gutter={16}><Col xs={24} md={12}><Form.Item label="作用域"><Input value={activeSkill.scope} onChange={(e) => patchSkill({ scope: e.target.value })} disabled={!canEditSkills} /></Form.Item></Col><Col xs={24} md={12}><Form.Item label="启用"><Switch checked={activeSkill.enabled} onChange={(checked) => patchSkill({ enabled: checked })} disabled={!canEditSkills} /></Form.Item></Col></Row><Form.Item label="触发条件"><Input value={activeSkill.trigger} onChange={(e) => patchSkill({ trigger: e.target.value })} disabled={!canEditSkills} /></Form.Item><Form.Item label="策略要求"><Input value={activeSkill.policy} onChange={(e) => patchSkill({ policy: e.target.value })} disabled={!canEditSkills} /></Form.Item><Form.Item label="Prompt"><Input.TextArea rows={8} value={activeSkill.prompt} onChange={(e) => patchSkill({ prompt: e.target.value })} disabled={!canEditSkills} /></Form.Item><Alert type="info" showIcon message="这里编辑的是前端草稿层" description="当前保存只会写到前端聚合服务或浏览器本地，不会自动进入 ESP32 的 /spiffs/skills。" /></Form> : <Empty description="没有可编辑的 skill" />}</Card></Col>
              </Row>}

              {viewMode === '用户偏好' && <Row gutter={[16, 16]} className="mt-4">
                <Col xs={24} xl={14}><Card title="偏好配置" className="glass-panel rounded-lg" extra={<Button type="primary" loading={savingPrefs} onClick={handleSavePreferences}>保存偏好</Button>}><Form layout="vertical"><Form.Item label="首选通道"><Select value={preferences.preferredChannel} onChange={(value) => setPreferences((current) => ({ ...current, preferredChannel: value }))} options={[{ value: 'Web Console' }, { value: 'ESP32-P4' }, { value: 'Android' }, { value: 'Feishu' }]} /></Form.Item><Form.Item label="语音输出"><Switch checked={preferences.voiceOutput} onChange={(checked) => setPreferences((current) => ({ ...current, voiceOutput: checked }))} /></Form.Item><Form.Item label="隐私模式"><Select value={preferences.privacyMode} onChange={(value) => setPreferences((current) => ({ ...current, privacyMode: value }))} options={[{ value: 'metadata_only', label: 'metadata_only' }, { value: 'balanced', label: 'balanced' }, { value: 'full_context', label: 'full_context' }]} /></Form.Item><Form.Item label="自动化激进程度"><Slider value={preferences.automationAggressiveness} onChange={(value) => setPreferences((current) => ({ ...current, automationAggressiveness: value }))} /></Form.Item><Form.Item label="摘要风格"><Select value={preferences.summaryStyle} onChange={(value) => setPreferences((current) => ({ ...current, summaryStyle: value }))} options={[{ value: 'concise' }, { value: 'standard' }, { value: 'detailed' }]} /></Form.Item><Form.Item label="语言"><Input value={preferences.preferredLanguage} onChange={(e) => setPreferences((current) => ({ ...current, preferredLanguage: e.target.value }))} /></Form.Item></Form></Card></Col>
                <Col xs={24} xl={10}><Card title="偏好生效说明" className="glass-panel rounded-lg"><Space direction="vertical" size={14} className="w-full"><Alert type="success" showIcon message="前端偏好已可持久化" description="会写入 localStorage，并通过 /api/preferences 回显保存结果。" /><div className="rounded-lg border border-slate-200 p-4 text-sm text-slate-600"><div>当前用户：{session.username}</div><div>Role：{session.role}</div><div>Channel：{preferences.preferredChannel}</div><div>Privacy：{preferences.privacyMode}</div><div>Auto level：{preferences.automationAggressiveness}%</div></div><Alert type="warning" showIcon message="板端尚未消费这些偏好" description="如果要让 ESP32 根据用户偏好改变调度策略，还需要扩展板端消息协议或聚合服务。" /></Space></Card></Col>
              </Row>}
            </Content>
          </Layout>

          <Modal open={chatOpen} onCancel={() => setChatOpen(false)} footer={null} width={920} title="AI 通信面板">
            <div className="grid gap-4 lg:grid-cols-[280px_minmax(0,1fr)]">
              <div className="space-y-4">
                <Card size="small" title="连接参数" className="glass-panel rounded-lg"><Form layout="vertical"><Form.Item label="当前用户"><Input value={`${session.username} / ${session.role}`} disabled /></Form.Item><Form.Item label="WebSocket URL"><Input value={wsUrl} onChange={(e) => setWsUrl(e.target.value)} placeholder="ws://<ESP32_IP>:18789/" /></Form.Item><Form.Item label="Chat ID"><Input value={chatId} onChange={(e) => setChatId(e.target.value)} placeholder="web_console_01" /></Form.Item><Space><Button type="primary" onClick={connectWs} disabled={!canUseChat}>连接</Button><Button onClick={() => wsInstance?.close()}>断开</Button></Space></Form></Card>
                <Card size="small" title="协议说明" className="glass-panel rounded-lg"><div className="space-y-2 text-sm text-slate-500"><div>发送格式：</div><pre className="overflow-x-auto rounded bg-slate-50 p-3 text-xs text-slate-700">{`{"type":"message","content":"你好","chat_id":"web_console_01"}`}</pre><div>返回格式：</div><pre className="overflow-x-auto rounded bg-slate-50 p-3 text-xs text-slate-700">{`{"type":"response","content":"你好，我在。","chat_id":"web_console_01"}`}</pre><Tag color={wsConnected ? 'green' : 'red'}>{wsConnected ? '已连接' : '未连接'}</Tag><Alert type={canUseChat ? 'info' : 'warning'} showIcon message={canUseChat ? '当前账号允许通信' : 'viewer 账号只读'} description={canUseChat ? '当前仍按现有 ESP32 WebSocket 协议发送，不附带登录身份。' : '如需按用户身份下发到板端，后续需要扩展协议。'} /></div></Card>
              </div>
              <Card size="small" title="会话窗口" className="glass-panel rounded-lg" extra={<Text type="secondary">模拟飞书式消息入口</Text>}><div className="mb-4 h-[420px] overflow-y-auto rounded border border-slate-200 bg-slate-50 p-4"><div className="space-y-3">{chatMessages.map((item) => <div key={item.id} className={`max-w-[85%] rounded-lg px-3 py-2 text-sm ${item.role === 'user' ? 'ml-auto bg-blue-600 text-white' : item.role === 'assistant' ? 'bg-white text-slate-800 shadow-sm' : 'bg-amber-50 text-amber-900'}`}><div>{item.text}</div><div className={`mt-1 text-[11px] ${item.role === 'user' ? 'text-blue-100' : 'text-slate-400'}`}>{item.time}</div></div>)}</div></div><Space.Compact className="w-full"><Input value={chatInput} onChange={(e) => setChatInput(e.target.value)} onPressEnter={sendChatMessage} placeholder="输入一条要发给 ESP32 Agent 的消息" disabled={!canUseChat} /><Button type="primary" onClick={sendChatMessage} disabled={!canUseChat}>发送</Button></Space.Compact><Divider /><div><div className="mb-3 text-sm font-semibold text-slate-900">相关 timeline</div><Timeline items={chatRelatedTimeline.map((event) => ({ color: statusColor(event.status), children: <div><div className="text-sm font-medium text-slate-800">{event.stage}</div><div className="text-xs text-slate-500">{event.source} → {event.target} · {event.time}</div><div className="mt-1 text-sm text-slate-600">{event.payload}</div></div> }))} /></div></Card>
            </div>
          </Modal>
        </Layout>
      </AntdApp>
    </ConfigProvider>
  );
}

export default App;
