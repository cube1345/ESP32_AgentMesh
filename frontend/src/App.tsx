import {
  App as AntdApp,
  Badge,
  Button,
  Card,
  Col,
  ConfigProvider,
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
  ArrowDownOutlined,
  BgColorsOutlined,
  BranchesOutlined,
  ControlOutlined,
  DashboardOutlined,
  DeploymentUnitOutlined,
  MessageOutlined,
  NodeIndexOutlined,
  SendOutlined,
  SafetyCertificateOutlined,
  ThunderboltOutlined
} from '@ant-design/icons';
import { useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import { mockDashboardPayload } from './data/mock';
import { fetchDashboard, fetchRuntimeSkills, installRuntimeSkill, savePreferences, saveSkills } from './services/dashboard';
import type {
  AgentNode,
  Capability,
  DashboardPayload,
  EnvironmentMetric,
  RuntimeSkillRecord,
  SkillDraft,
  TimelineEvent,
  UserPreferenceProfile
} from './types';

const { Header, Content, Sider } = Layout;
const { Title, Text } = Typography;

type ViewMode = '总览' | '协作链路' | 'Sandbox' | 'Skills Studio' | '用户偏好';
type TimelineFilter = '全部' | 'telemetry' | 'state' | 'policy' | 'sandbox' | 'reply' | 'error';
type Role = 'admin' | 'operator' | 'viewer';

const DRAFT_SKILLS_STORAGE_KEY = 'espagent.console.skillDrafts';
const PREFS_STORAGE_KEY = 'espagent.console.preferences';
const AUTH_STORAGE_KEY = 'espagent.console.auth';
const SANDBOX_DENY_TEST_COMMAND = `python3 tools/serial_cmd.py /dev/ttyUSB0 'tool_exec write_file {"path":"/spiffs/skills/bench-unsafe.md","content":"x"}' --timeout 20`;

interface UserSession {
  username: string;
  role: Role;
}

interface ChatMessage {
  id: string;
  role: 'user' | 'assistant' | 'system';
  tone?: 'info' | 'success' | 'warning' | 'error';
  text: string;
  time: string;
}

interface BrowserSpeechRecognitionResultItem {
  transcript: string;
}

interface BrowserSpeechRecognitionResult {
  isFinal: boolean;
  0: BrowserSpeechRecognitionResultItem;
  length: number;
}

interface BrowserSpeechRecognitionEvent extends Event {
  resultIndex: number;
  results: ArrayLike<BrowserSpeechRecognitionResult>;
}

interface BrowserSpeechRecognitionErrorEvent extends Event {
  error: string;
  message?: string;
}

interface BrowserSpeechRecognition extends EventTarget {
  lang: string;
  continuous: boolean;
  interimResults: boolean;
  maxAlternatives: number;
  onstart: ((this: BrowserSpeechRecognition, ev: Event) => void) | null;
  onresult: ((this: BrowserSpeechRecognition, ev: BrowserSpeechRecognitionEvent) => void) | null;
  onerror: ((this: BrowserSpeechRecognition, ev: BrowserSpeechRecognitionErrorEvent) => void) | null;
  onend: ((this: BrowserSpeechRecognition, ev: Event) => void) | null;
  start(): void;
  stop(): void;
  abort(): void;
}

interface BrowserSpeechRecognitionCtor {
  new (): BrowserSpeechRecognition;
}

declare global {
  interface Window {
    SpeechRecognition?: BrowserSpeechRecognitionCtor;
    webkitSpeechRecognition?: BrowserSpeechRecognitionCtor;
  }
}

const demoAccounts: Array<UserSession & { password: string; description: string }> = [
  { username: 'admin', password: 'admin123', role: 'admin', description: '全功能演示账号' },
  { username: 'operator', password: 'operator123', role: 'operator', description: '调度与监控账号' },
  { username: 'viewer', password: 'viewer123', role: 'viewer', description: '只读展示账号' }
];

const viewItems: { label: ViewMode; icon: ReactNode }[] = [
  { label: '总览', icon: <DashboardOutlined /> },
  { label: '协作链路', icon: <BranchesOutlined /> },
  { label: 'Sandbox', icon: <AuditOutlined /> },
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

function displayNodeName(role: AgentNode['role']): string {
  if (role === 'coordinator_agent') return '调度节点';
  if (role === 'sensor_agent') return '传感节点';
  if (role === 'control_agent') return '控制节点';
  if (role === 'guardian_agent') return '安全节点';
  return '展示终端';
}

function chatToneFromText(text: string): ChatMessage['tone'] {
  if (/ERROR|失败|异常|不可用|断开|invalid|failed/i.test(text)) return 'error';
  if (/等待队列|重试|稍后|未配置|warning/i.test(text)) return 'warning';
  if (/已连接|send_ok|成功|installed|linked/i.test(text)) return 'success';
  return 'info';
}

function isAssistantFailureText(text: string): boolean {
  return /模型服务这次调用失败了|LLM transport failed|ESP_ERR_|HTTP_CONNECT/i.test(text);
}

function microphonePermissionHint(error: string): string {
  if (error === 'not-allowed' || error === 'service-not-allowed') {
    return '浏览器拒绝了麦克风权限。请确认地址栏左侧麦克风权限已允许；如果当前通过局域网 IP 的 http 页面访问，请改用 localhost 或 HTTPS。';
  }
  if (error === 'audio-capture') {
    return '没有检测到可用麦克风，或麦克风正被其他应用占用。';
  }
  if (error === 'no-speech') {
    return '没有检测到语音，请靠近麦克风后重试。';
  }
  if (error === 'network') {
    return '浏览器语音识别服务网络不可用，请稍后重试。';
  }
  return `语音识别失败：${error}`;
}

function isLocalhost(hostname: string): boolean {
  return hostname === 'localhost' || hostname === '127.0.0.1' || hostname === '[::1]' || hostname === '::1';
}

function isSandboxTimelineEvent(event: TimelineEvent): boolean {
  const text = `${event.stage} ${event.source} ${event.target} ${event.payload}`.toLowerCase();
  return text.includes('sandbox') || text.includes('沙箱') || text.includes('denied') || text.includes('blocked by sandbox');
}

function browserTtsSupported(): boolean {
  return typeof window !== 'undefined' &&
    typeof window.speechSynthesis !== 'undefined' &&
    typeof SpeechSynthesisUtterance !== 'undefined';
}

const capabilityColumns = [
  { title: '能力', dataIndex: 'name', key: 'name', render: (value: string) => <Text strong>{value}</Text> },
  { title: '类别', dataIndex: 'category', key: 'category', render: (value: string) => <Tag color="blue">{value}</Tag> },
  { title: '角色', dataIndex: 'role', key: 'role' },
  { title: '状态', dataIndex: 'maturity', key: 'maturity', render: (value: Capability['maturity']) => <Tag color={maturityTag(value)}>{value}</Tag> },
  { title: '说明', dataIndex: 'summary', key: 'summary' }
];

function defaultGatewayWsUrl(): string {
  if (typeof window === 'undefined') {
    return 'ws://127.0.0.1:4173/ws';
  }
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${protocol}//${window.location.host}/ws`;
}

function App() {
  const [payload, setPayload] = useState<DashboardPayload>(mockDashboardPayload);
  const [draftSkills, setDraftSkills] = useState<SkillDraft[]>(() => readStorage(DRAFT_SKILLS_STORAGE_KEY, mockDashboardPayload.skills));
  const [runtimeSkills, setRuntimeSkills] = useState<RuntimeSkillRecord[]>([]);
  const [preferences, setPreferences] = useState<UserPreferenceProfile>(() => readStorage(PREFS_STORAGE_KEY, mockDashboardPayload.preferences));
  const [session, setSession] = useState<UserSession | null>(() => readStorage<UserSession | null>(AUTH_STORAGE_KEY, null));
  const [viewMode, setViewMode] = useState<ViewMode>('总览');
  const [timelineFilter, setTimelineFilter] = useState<TimelineFilter>('全部');
  const [activeSkillId, setActiveSkillId] = useState<string>(mockDashboardPayload.skills[0]?.id ?? '');
  const [savingSkills, setSavingSkills] = useState(false);
  const [installingSkillId, setInstallingSkillId] = useState<string | null>(null);
  const [runtimeSkillSource, setRuntimeSkillSource] = useState<'local_mock' | 'proxy' | 'serial'>('local_mock');
  const [savingPrefs, setSavingPrefs] = useState(false);
  const [chatOpen, setChatOpen] = useState(false);
  const [wsUrl, setWsUrl] = useState(defaultGatewayWsUrl);
  const [chatId, setChatId] = useState('web_console_01');
  const [chatInput, setChatInput] = useState('');
  const [loginUsername, setLoginUsername] = useState('admin');
  const [loginPassword, setLoginPassword] = useState('admin123');
  const [wsConnected, setWsConnected] = useState(false);
  const [wsInstance, setWsInstance] = useState<WebSocket | null>(null);
  const [sttListening, setSttListening] = useState(false);
  const [sttInterimText, setSttInterimText] = useState('');
  const [sttStatusText, setSttStatusText] = useState('未启动');
  const [chatMessages, setChatMessages] = useState<ChatMessage[]>([
    { id: 'system-welcome', role: 'system' as const, tone: 'info', text: '这里模拟飞书式消息入口，消息经本地网关转发到 ESP32。', time: new Date().toLocaleTimeString('zh-CN', { hour12: false }) }
  ]);
  const [messageApi, contextHolder] = message.useMessage();
  const chatScrollRef = useRef<HTMLDivElement | null>(null);
  const speechRecognitionRef = useRef<BrowserSpeechRecognition | null>(null);
  const pendingSttRequestRef = useRef<{ requestId: string; replyChannel: string; replyChatId: string } | null>(null);
  const activeTtsUtteranceRef = useRef<SpeechSynthesisUtterance | null>(null);

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
    let cancelled = false;
    const syncRuntimeSkills = async () => {
      const next = await fetchRuntimeSkills();
      if (!cancelled) {
        setRuntimeSkills(next.skills);
        setRuntimeSkillSource(next.source);
      }
    };
    syncRuntimeSkills();
    const timer = window.setInterval(syncRuntimeSkills, 5000);
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

  useEffect(() => {
    return () => {
      if (typeof window !== 'undefined' && window.speechSynthesis) {
        window.speechSynthesis.cancel();
      }
    };
  }, []);

  useEffect(() => {
    const viewport = chatScrollRef.current;
    if (!viewport) return;
    viewport.scrollTop = viewport.scrollHeight;
  }, [chatMessages]);

  const activeSkill = useMemo(() => draftSkills.find((item) => item.id === activeSkillId) ?? null, [activeSkillId, draftSkills]);
  const canEditSkills = session?.role === 'admin';
  const canUseChat = session?.role !== 'viewer';
  const filteredTimeline = useMemo(() => payload.timeline.filter((event) => {
    if (timelineFilter === '全部') return true;
    if (timelineFilter === 'telemetry') return event.stage.includes('telemetry');
    if (timelineFilter === 'state') return event.stage.includes('state');
    if (timelineFilter === 'policy') return event.stage.includes('policy') || event.stage.includes('guardian');
    if (timelineFilter === 'sandbox') return isSandboxTimelineEvent(event);
    if (timelineFilter === 'reply') return event.stage.includes('reply') || event.stage.includes('outbound');
    return event.stage.includes('error') || event.status === 'warn';
  }), [payload.timeline, timelineFilter]);

  const sandboxEvents = useMemo(() => payload.timeline.filter(isSandboxTimelineEvent), [payload.timeline]);
  const latestSandboxEvent = sandboxEvents[0] ?? null;
  const sandboxWarnCount = sandboxEvents.filter((event) => event.status === 'warn').length;

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
  const sttSupported = typeof window !== 'undefined' && Boolean(window.SpeechRecognition || window.webkitSpeechRecognition);
  const sttSecureContext = typeof window !== 'undefined' && (window.isSecureContext || isLocalhost(window.location.hostname));

  function appendChatMessage(role: 'user' | 'assistant' | 'system', text: string, tone?: ChatMessage['tone']) {
    setChatMessages((current) => [...current, { id: `${role}-${Date.now()}-${Math.random().toString(16).slice(2, 8)}`, role, tone, text, time: new Date().toLocaleTimeString('zh-CN', { hour12: false }) }]);
  }

  function stopSpeechRecognition() {
    speechRecognitionRef.current?.stop();
  }

  function speakBrowserTts(text: string) {
    const clean = text.trim();
    if (!clean || !preferences.voiceOutput) {
      return false;
    }
    if (!browserTtsSupported()) {
      return false;
    }
    if (typeof window === 'undefined' || !window.speechSynthesis) {
      return false;
    }

    const utterance = new SpeechSynthesisUtterance(clean);
    utterance.lang = 'zh-CN';
    utterance.rate = 1;
    utterance.pitch = 1;
    activeTtsUtteranceRef.current = utterance;
    window.speechSynthesis.cancel();
    window.speechSynthesis.speak(utterance);
    return true;
  }

  function sendTranscriptToGateway(transcript: string, request?: { requestId: string; replyChannel: string; replyChatId: string } | null) {
    if (!wsInstance || wsInstance.readyState !== WebSocket.OPEN) {
      appendChatMessage('system', '语音转写完成，但当前 WebSocket 未连接，无法发送。', 'error');
      return;
    }

    const clean = transcript.trim();
    if (!clean) {
      return;
    }

    if (request) {
      wsInstance.send(JSON.stringify({
        type: 'stt_result',
        request_id: request.requestId,
        transcript: clean,
        reply_channel: request.replyChannel,
        reply_chat_id: request.replyChatId,
        provider: 'browser_webspeech',
        status: 'ok'
      }));
      appendChatMessage('system', `已完成语音转写并回传：${clean}`, 'success');
      return;
    }

    wsInstance.send(JSON.stringify({ type: 'message', content: clean, chat_id: chatId.trim() || 'web_console_01' }));
    appendChatMessage('user', clean);
  }

  function startSpeechRecognition(request?: { requestId: string; replyChannel: string; replyChatId: string } | null) {
    const SpeechRecognitionCtor = window.SpeechRecognition || window.webkitSpeechRecognition;
    if (!SpeechRecognitionCtor) {
      setSttStatusText('当前浏览器不支持 SpeechRecognition');
      appendChatMessage('system', '当前浏览器不支持 SpeechRecognition，无法启动 STT。', 'error');
      return;
    }
    if (!sttSecureContext) {
      const hint = '浏览器语音输入需要安全来源。请用 localhost 打开本机前端，或给当前站点配置 HTTPS 后再点语音输入。';
      setSttStatusText('麦克风不可用：当前页面不是安全来源');
      appendChatMessage('system', hint, 'warning');
      return;
    }
    if (!wsInstance || wsInstance.readyState !== WebSocket.OPEN) {
      appendChatMessage('system', '请先连接本地通信网关，再启动 STT。', 'warning');
      return;
    }
    if (sttListening) {
      appendChatMessage('system', 'STT 已在运行。', 'warning');
      return;
    }

    pendingSttRequestRef.current = request ?? null;
    setSttInterimText('');
    setSttListening(true);
    setSttStatusText(request ? '响应远端 STT 请求中...' : '正在采集语音...');

    const recognition = new SpeechRecognitionCtor();
    speechRecognitionRef.current = recognition;
    recognition.lang = preferences.preferredLanguage || 'zh-CN';
    recognition.continuous = false;
    recognition.interimResults = true;
    recognition.maxAlternatives = 1;

    recognition.onstart = () => {
      setSttStatusText(request ? '浏览器 STT 已启动，等待说话...' : '浏览器 STT 已启动');
    };

    recognition.onresult = (event) => {
      let interim = '';
      let finalText = '';

      for (let i = event.resultIndex; i < event.results.length; i += 1) {
        const result = event.results[i];
        const text = result[0]?.transcript || '';
        if (result.isFinal) finalText += text;
        else interim += text;
      }

      setSttInterimText(interim);
      if (finalText.trim()) {
        setSttStatusText('语音识别完成');
        sendTranscriptToGateway(finalText, pendingSttRequestRef.current);
        pendingSttRequestRef.current = null;
        recognition.stop();
      }
    };

    recognition.onerror = (event) => {
      const hint = microphonePermissionHint(event.error);
      setSttStatusText(hint);
      appendChatMessage('system', hint, event.error === 'not-allowed' || event.error === 'service-not-allowed' ? 'warning' : 'error');
      pendingSttRequestRef.current = null;
      setSttListening(false);
      speechRecognitionRef.current = null;
    };

    recognition.onend = () => {
      setSttListening(false);
      speechRecognitionRef.current = null;
      if (!pendingSttRequestRef.current) {
        setSttStatusText((current) => current === '语音识别完成' ? current : '已停止');
      }
    };

    recognition.start();
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

  async function refreshRuntimeSkills() {
    const next = await fetchRuntimeSkills();
    setRuntimeSkills(next.skills);
    setRuntimeSkillSource(next.source);
  }

  async function handleInstallActiveSkill() {
    if (!canEditSkills || !activeSkill) return;
    setInstallingSkillId(activeSkill.id);
    try {
      const result = await installRuntimeSkill(activeSkill, true);
      if (result.ok) {
        messageApi.success(result.message || 'runtime skill install ok');
        await refreshRuntimeSkills();
      } else {
        messageApi.error(result.error || result.message || 'runtime skill install failed');
      }
    } finally {
      setInstallingSkillId(null);
    }
  }

  function connectWs() {
    if (!canUseChat) return void messageApi.warning('viewer 账号只读，不开放 AI 通信');
    wsInstance?.close();
    try {
      const socket = new WebSocket(wsUrl);
      socket.onopen = () => { setWsConnected(true); appendChatMessage('system', `WebSocket 已连接：${wsUrl}`, 'success'); };
      socket.onmessage = (event) => {
        try {
          const data = JSON.parse(String(event.data)) as { type?: string; content?: string; request_id?: string; reply_channel?: string; reply_chat_id?: string; hint_text?: string; text?: string; detail?: string };
          const content = data.content || String(event.data);
          if (data.type === 'stt_request') {
            appendChatMessage('system', data.hint_text ? `收到 STT 请求：${data.hint_text}` : '收到 STT 请求，开始录音转写。', 'info');
            startSpeechRecognition({
              requestId: data.request_id || `stt-${Date.now()}`,
              replyChannel: data.reply_channel || 'web',
              replyChatId: data.reply_chat_id || (chatId.trim() || 'web_console_01')
            });
            return;
          }
          if (data.type === 'tts_fallback') {
            const text = typeof data.text === 'string' ? data.text.trim() : '';
            const spoken = text ? speakBrowserTts(text) : false;
            appendChatMessage(
              'system',
              spoken
                ? `板端 TTS 失败，已回退到浏览器播报。${data.detail ? `原因：${data.detail}` : ''}`
                : `板端 TTS 失败。${data.detail ? `原因：${data.detail}` : ''}${text ? ' 浏览器 TTS 当前不可用。' : ''}`,
              spoken ? 'warning' : 'error'
            );
            return;
          }
          if (data.type === 'response' && data.content) {
            if (isAssistantFailureText(content)) {
              appendChatMessage('system', content, 'error');
            } else {
              appendChatMessage('assistant', content, 'info');
            }
            return;
          }
          appendChatMessage('system', content, chatToneFromText(content));
        } catch {
          appendChatMessage('system', String(event.data), chatToneFromText(String(event.data)));
        }
      };
      socket.onerror = () => appendChatMessage('system', 'WebSocket 连接或通信出错', 'error');
      socket.onclose = () => { setWsConnected(false); setWsInstance(null); appendChatMessage('system', 'WebSocket 已断开', 'warning'); };
      setWsInstance(socket);
    } catch (error) {
      appendChatMessage('system', `无法创建 WebSocket：${String(error)}`, 'error');
    }
  }

  function sendChatMessage() {
    if (!canUseChat) return void messageApi.warning('viewer 账号只读，不开放 AI 通信');
    if (!chatInput.trim()) return;
    if (!wsInstance || wsInstance.readyState !== WebSocket.OPEN) return void messageApi.warning('请先连接本地通信网关');
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
                <div className="mt-6 grid gap-4 md:grid-cols-2">
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
              <Card bordered={false} className="glass-panel rounded-lg" title="登录">
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
          <Sider breakpoint="xl" collapsedWidth="0" width={360} className="border-r border-slate-200 !bg-white">
            <div className="flex h-full flex-col overflow-hidden px-5 py-5">
              <div className="mb-6">
                <div className="mb-3 flex items-center gap-3"><div className="flex h-11 w-11 items-center justify-center rounded-lg bg-cyan-500/12 text-cyan-300"><DeploymentUnitOutlined className="text-xl" /></div><div><div className="text-base font-semibold text-slate-900">ESPAgent Console</div><div className="text-xs text-slate-500">LingShu Agent Mesh Frontend</div></div></div>
              </div>
              <Segmented block className="mb-5" value={viewMode} onChange={(value) => setViewMode(value as ViewMode)} options={viewItems.map((item) => ({ label: <span className="flex items-center gap-2">{item.icon}{item.label}</span>, value: item.label }))} />
              <Card bordered={false} className="glass-panel rounded-lg">
                <div className="mb-3 flex items-center justify-between"><Text>节点健康</Text><Badge status="processing" text="live data" /></div>
                <div className="panel-scroll grid max-h-[520px] gap-3 overflow-y-auto pr-1">
                  {fixedNodePanels.map(({ node, sentSummary, recvSummary, subagentLabel }) => (
                    <div key={node.role} className="rounded-lg border border-slate-200 bg-slate-50 px-3 py-3">
                      <div className="mb-2 flex items-center justify-between gap-3">
                        <div>
                          <div className="text-sm font-semibold text-slate-900">{displayNodeName(node.role)}</div>
                          <div className="text-xs text-slate-500">{node.role}</div>
                        </div>
                        <Tag color={statusColor(node.status)}>{node.status}</Tag>
                      </div>
                      <div className="mb-2 text-xs text-slate-500">{node.id}</div>
                      <div className="grid gap-2 text-xs text-slate-600">
                        <div className="rounded border border-slate-200 bg-white px-2 py-2">发出：{sentSummary[0]}</div>
                        <div className="rounded border border-slate-200 bg-white px-2 py-2">接收：{recvSummary[0]}</div>
                        <div className="rounded border border-slate-200 bg-white px-2 py-2">Subagent：{subagentLabel}</div>
                      </div>
                    </div>
                  ))}
                </div>
              </Card>
              <Card bordered={false} className="glass-panel mt-4 rounded-lg">
                <div className="mb-3 flex items-center gap-2 text-slate-900"><ApiOutlined />链路状态</div>
                <div className="grid gap-2 text-xs">
                  <div className="flex items-center justify-between rounded border border-slate-200 bg-slate-50 px-3 py-2">
                    <span className="text-slate-500">MQTT</span>
                    <Tag color={payload.mqtt?.connected ? 'green' : 'red'}>{payload.mqtt?.connected ? 'online' : 'offline'}</Tag>
                  </div>
                  <div className="flex items-center justify-between rounded border border-slate-200 bg-slate-50 px-3 py-2">
                    <span className="text-slate-500">Chat Gateway</span>
                    <Tag color={payload.chatGateway?.enabled ? (payload.chatGateway.connectedSessions > 0 ? 'green' : 'gold') : 'red'}>
                      {payload.chatGateway?.enabled ? (payload.chatGateway.connectedSessions > 0 ? 'linked' : 'idle') : 'disabled'}
                    </Tag>
                  </div>
                </div>
              </Card>
            </div>
          </Sider>
          <Layout>
            <Header className="border-b border-slate-200 px-6 !bg-transparent">
              <div className="flex h-full items-center justify-between gap-4">
                <div><Title level={3} className="!mb-0">多 Agent 控制台</Title></div>
                <Space size="middle" wrap><Tag color={payload.mqtt?.connected ? 'green' : 'red'}>MQTT {payload.mqtt?.connected ? 'connected' : 'offline'}</Tag><Tag color={payload.chatGateway?.enabled ? (payload.chatGateway.connectedSessions > 0 ? 'green' : 'gold') : 'red'}>Chat Gateway {payload.chatGateway?.enabled ? (payload.chatGateway.connectedSessions > 0 ? 'linked' : 'idle') : 'disabled'}</Tag><Tag color="blue">{payload.mqtt?.topicPrefix || 'mock-prefix'}</Tag><Tag color="purple">{session.username} / {session.role}</Tag><Button onClick={handleLogout}>退出</Button><Button type="primary" onClick={() => setChatOpen(true)}>AI 通信</Button></Space>
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
                <Col xs={24} xl={15}><Card title="四节点拓扑与主要链路" className="glass-panel rounded-lg"><div className="grid gap-4 xl:grid-cols-[1fr_280px]"><div className="panel-scroll grid max-h-[420px] gap-4 overflow-y-auto pr-1 md:grid-cols-2">{fixedNodePanels.map(({ node }) => <div key={node.role} className="rounded-lg border border-slate-200 bg-slate-50 p-4"><div className="mb-2 flex items-center justify-between"><div><div className="text-sm font-semibold text-slate-900">{displayNodeName(node.role)}</div><div className="text-xs text-slate-500">{node.role}</div></div><Tag color={statusColor(node.status)}>{node.status}</Tag></div><div className="text-xs text-slate-500">{node.id}</div><div className="mt-3 flex flex-wrap gap-2">{node.transport.map((item) => <Tag key={item}>{item}</Tag>)}</div></div>)}</div><div className="rounded-lg border border-slate-200 bg-slate-50 p-4"><div className="mb-3 text-sm font-semibold text-slate-900">主要通信边</div><div className="panel-scroll max-h-[420px] space-y-3 overflow-y-auto pr-1">{[{ id: '1', title: '用户入口', from: 'Feishu / Web / CLI', to: 'coordinator_agent', detail: '自然语言指令进入 Coordinator' }, { id: '2', title: '策略裁决', from: 'coordinator_agent', to: 'guardian_agent', detail: 'policy_check / decision' }, { id: '3', title: '环境采集', from: 'sensor_agent', to: 'coordinator_agent', detail: 'telemetry / threshold event' }, { id: '4', title: '控制执行', from: 'coordinator_agent', to: 'control_agent', detail: 'mesh_command / actuator result' }].map((edge) => <div key={edge.id} className="rounded-lg border border-slate-200 bg-white p-3"><div className="mb-1 text-sm font-medium text-slate-800">{edge.title}</div><div className="text-xs text-slate-500">{edge.from} → {edge.to}</div><div className="mt-1 text-sm text-slate-600">{edge.detail}</div></div>)}</div></div></div></Card></Col>
                <Col xs={24} xl={9}><Card title="调度摘要" className="glass-panel rounded-lg"><div className="grid gap-3">{[{ label: '上游 Chat Gateway', value: payload.chatGateway?.upstreamUrl || '未配置', tone: payload.chatGateway?.enabled ? 'blue' : 'red' }, { label: '最近通信事件', value: payload.chatGateway?.lastEventAt || '暂无' }, { label: '最近错误', value: payload.chatGateway?.lastError || '无' }, { label: '主题前缀', value: payload.mqtt?.topicPrefix || '-' }].map((item) => <div key={item.label} className="rounded-lg border border-slate-200 bg-slate-50 p-4"><div className="text-xs text-slate-500">{item.label}</div><div className="mt-2 break-all text-sm font-medium text-slate-900">{item.value}</div></div>)}</div></Card></Col>
                <Col xs={24}><Card title="四节点固定通信面板" className="glass-panel rounded-lg"><div className="grid gap-4 xl:grid-cols-2 2xl:grid-cols-4">{fixedNodePanels.map(({ node, sentSummary, recvSummary, subagentLabel }) => <div key={node.role} className="rounded-lg border border-slate-200 p-4"><div className="mb-3 flex items-center justify-between"><div><div className="text-base font-semibold text-slate-900">{node.role}</div><div className="text-xs text-slate-500">{node.id}</div></div><Tag color={statusColor(node.status)}>{node.status}</Tag></div><div className="grid gap-3"><div className="panel-scroll rounded-lg bg-slate-50 p-3"><div className="mb-1 text-xs text-slate-500">发送</div><div className="max-h-[88px] space-y-1 overflow-y-auto pr-1">{sentSummary.map((item) => <div key={item} className="text-sm text-slate-700">{item}</div>)}</div></div><div className="panel-scroll rounded-lg bg-slate-50 p-3"><div className="mb-1 text-xs text-slate-500">接收</div><div className="max-h-[88px] space-y-1 overflow-y-auto pr-1">{recvSummary.map((item) => <div key={item} className="text-sm text-slate-700">{item}</div>)}</div></div><div className="rounded-lg bg-slate-50 p-3"><div className="mb-2 flex items-center justify-between"><div className="text-xs text-slate-500">Subagent</div><Tag color={nodeChannelHints[node.role].subagent ? 'blue' : 'default'}>{nodeChannelHints[node.role].subagent ? 'supported' : 'not supported'}</Tag></div><div className="max-h-[72px] overflow-y-auto pr-1 text-sm text-slate-700">{subagentLabel}</div></div></div></div>)}</div></Card></Col>
                <Col xs={24} xl={15} className="flex"><Card title="项目能力总览" className="glass-panel h-full w-full rounded-lg" extra={<Tag color="blue">React + TypeScript + Antd + Tailwind</Tag>}><div className="panel-scroll h-[460px] overflow-auto"><Table rowKey="name" dataSource={payload.capabilities} columns={capabilityColumns} pagination={false} size="middle" scroll={{ x: 720 }} /></div></Card></Col>
                <Col xs={24} xl={9} className="flex"><Card title="环境面板" className="glass-panel h-full w-full rounded-lg"><div className="panel-scroll h-[460px] overflow-y-auto pr-1"><Space direction="vertical" size={16} className="w-full">{payload.environment.map((metric) => <div key={metric.label} className="rounded-lg border border-slate-200 px-4 py-3"><div className="mb-2 flex items-center justify-between"><Text>{metric.label}</Text><Tag color={metric.status === 'good' ? 'green' : metric.status === 'attention' ? 'gold' : 'red'}>{metric.status}</Tag></div><div className="mb-3 text-xl font-semibold text-slate-900">{metric.value} {metric.unit}</div><Progress percent={Math.round(metricPercent(metric))} showInfo={false} strokeColor={metric.status === 'good' ? '#22c55e' : metric.status === 'attention' ? '#f59e0b' : '#ef4444'} /><div className="mt-2 text-xs text-slate-500">趋势 {metric.trend > 0 ? '+' : ''}{metric.trend}</div></div>)}</Space></div></Card></Col>
              </Row>}

              {viewMode === '协作链路' && <Row gutter={[16, 16]} className="mt-4">
                <Col xs={24} xl={10}><Card title="通信流程图" className="glass-panel rounded-lg"><div className="panel-scroll max-h-[620px] overflow-y-auto pr-1"><Space direction="vertical" size={14} className="w-full">{payload.flows.map((flow, index) => <div key={flow.id} className="rounded-lg border border-slate-200 p-4"><div className="mb-2 flex items-center justify-between"><Space><div className="flex h-8 w-8 items-center justify-center rounded-lg bg-cyan-500/12 text-cyan-300">{index + 1}</div><Text strong>{flow.step}</Text></Space><Tag color="cyan">{flow.transport}</Tag></div><div className="text-sm text-slate-700">{flow.producer} → {flow.consumer}</div><div className="mt-2 text-xs text-slate-500">{flow.topic}</div><div className="mt-2 text-sm text-slate-600">{flow.detail}</div></div>)}</Space></div></Card></Col>
                <Col xs={24} xl={14}><Card title="Timeline" className="glass-panel rounded-lg" extra={<Segmented value={timelineFilter} onChange={(value) => setTimelineFilter(value as TimelineFilter)} options={['全部', 'telemetry', 'state', 'policy', 'sandbox', 'reply', 'error']} />}>{filteredTimeline.length ? <div className="panel-scroll max-h-[620px] overflow-y-auto pr-1"><Timeline items={filteredTimeline.map((event) => ({ color: statusColor(event.status), children: <div><div className="flex flex-wrap items-center gap-2"><Text strong>{event.stage}</Text><Tag color={statusColor(event.status)}>{event.status}</Tag></div><div className="text-xs text-slate-500">{event.time} · {event.source} → {event.target}</div><div className="mt-1 text-sm text-slate-700">{event.payload}</div></div> }))} /></div> : <Empty description="当前筛选条件下没有事件" />}</Card></Col>
              </Row>}

              {viewMode === 'Sandbox' && <Row gutter={[16, 16]} className="mt-4">
                <Col xs={24} md={8}><Card bordered={false} className="metric-shell rounded-lg"><Statistic title="Sandbox 拦截" value={sandboxEvents.length} prefix={<AuditOutlined />} /></Card></Col>
                <Col xs={24} md={8}><Card bordered={false} className="metric-shell rounded-lg"><Statistic title="风险事件" value={sandboxWarnCount} prefix={<SafetyCertificateOutlined />} /></Card></Col>
                <Col xs={24} md={8}><Card bordered={false} className="metric-shell rounded-lg"><Statistic title="最近工具" value={latestSandboxEvent?.target || '-'} prefix={<ControlOutlined />} /></Card></Col>
                <Col xs={24} xl={10}><Card title="Sandbox 策略面板" className="glass-panel rounded-lg"><div className="panel-scroll max-h-[620px] overflow-y-auto pr-1"><Space direction="vertical" size={14} className="w-full">
                  {[
                    { title: 'Skill 文件写入', status: 'confirmed required', detail: '/spiffs/skills 下的 write_file/edit_file 必须显式 confirmed=true。' },
                    { title: '配置与密钥保护', status: 'blocked', detail: 'config、secrets 等敏感路径禁止通过工具直接写入。' },
                    { title: 'Mesh 控制边界', status: 'ttl limited', detail: 'mesh_send_command 的 ttl_ms 必须不超过 30000，并且 action 必须在白名单内。' },
                    { title: '高控制风险', status: 'confirmed required', detail: 'automation_create_rule 等高控制动作需要确认流，避免误触发长期规则。' },
                    { title: '音频执行限幅', status: 'bounded', detail: 'max98357_play_tone 限制时长和音量，防止长时间或过高音量输出。' }
                  ].map((item) => <div key={item.title} className="rounded-lg border border-slate-200 bg-slate-50 p-4"><div className="mb-2 flex items-center justify-between gap-3"><Text strong>{item.title}</Text><Tag color={item.status === 'blocked' ? 'red' : 'gold'}>{item.status}</Tag></div><div className="text-sm text-slate-600">{item.detail}</div></div>)}
                </Space></div></Card></Col>
                <Col xs={24} xl={14}><Card title="Sandbox 事件流" className="glass-panel rounded-lg" extra={<Tag color={latestSandboxEvent ? statusColor(latestSandboxEvent.status) : 'default'}>{latestSandboxEvent ? latestSandboxEvent.status : 'idle'}</Tag>}>{sandboxEvents.length ? <div className="panel-scroll max-h-[620px] overflow-y-auto pr-1"><Timeline items={sandboxEvents.map((event) => ({ color: statusColor(event.status), children: <div><div className="flex flex-wrap items-center gap-2"><Text strong>{event.stage}</Text><Tag color={statusColor(event.status)}>{event.status}</Tag><Tag>{event.target}</Tag></div><div className="text-xs text-slate-500">{event.time} · {event.source} → {event.target}</div><div className="mt-1 text-sm text-slate-700">{event.payload}</div></div> }))} /></div> : <Empty description="还没有收到 sandbox 拦截事件" />}</Card></Col>
                <Col xs={24}><Card title="验证命令" className="glass-panel rounded-lg"><div className="grid gap-3 xl:grid-cols-2">
                  <div className="rounded-lg border border-slate-200 bg-slate-50 p-4"><div className="mb-2 text-sm font-semibold text-slate-900">应被拦截</div><Typography.Paragraph copyable className="!mb-0 !text-sm">{SANDBOX_DENY_TEST_COMMAND}</Typography.Paragraph></div>
                  <div className="rounded-lg border border-slate-200 bg-slate-50 p-4"><div className="mb-2 text-sm font-semibold text-slate-900">前端现象</div><div className="text-sm text-slate-600">Timeline 出现 sandbox_denied / warn 事件；Sandbox 页面累计拦截次数，并展示被拦截工具和原因。</div></div>
                </div></Card></Col>
              </Row>}

              {viewMode === 'Skills Studio' && <Row gutter={[16, 16]} className="mt-4">
                <Col xs={24} xl={8}><Card title="Skill 草稿列表" className="glass-panel rounded-lg" extra={<Button type="primary" onClick={addSkill} disabled={!canEditSkills}>新增</Button>}><div className="panel-scroll max-h-[620px] overflow-y-auto pr-1"><List dataSource={draftSkills} renderItem={(item) => <List.Item key={item.id} className={`cursor-pointer rounded-lg px-3 ${item.id === activeSkillId ? 'bg-slate-50' : ''}`} onClick={() => setActiveSkillId(item.id)}><div className="w-full"><div className="flex items-center justify-between gap-3"><Text strong>{item.name}</Text><Tag color={item.enabled ? 'green' : 'default'}>{item.enabled ? 'enabled' : 'disabled'}</Tag></div><div className="mt-1 text-xs text-slate-500">{item.scope}</div></div></List.Item>} /></div></Card></Col>
                <Col xs={24} xl={16}><Card title="Skill 编辑器" className="glass-panel rounded-lg" extra={<Space><Tag color={canEditSkills ? 'green' : 'gold'}>{canEditSkills ? 'admin 可写' : '当前只读'}</Tag><Button type="primary" loading={savingSkills} onClick={handleSaveSkills} disabled={!canEditSkills}>保存草稿</Button><Button loading={installingSkillId === activeSkillId} onClick={handleInstallActiveSkill} disabled={!canEditSkills || !activeSkill}>安装到 Runtime</Button></Space>}>{activeSkill ? <div className="panel-scroll max-h-[620px] overflow-y-auto pr-1"><Form layout="vertical"><Form.Item label="名称"><Input value={activeSkill.name} onChange={(e) => patchSkill({ name: e.target.value })} disabled={!canEditSkills} /></Form.Item><Row gutter={16}><Col xs={24} md={12}><Form.Item label="作用域"><Input value={activeSkill.scope} onChange={(e) => patchSkill({ scope: e.target.value })} disabled={!canEditSkills} /></Form.Item></Col><Col xs={24} md={12}><Form.Item label="启用"><Switch checked={activeSkill.enabled} onChange={(checked) => patchSkill({ enabled: checked })} disabled={!canEditSkills} /></Form.Item></Col></Row><Form.Item label="触发条件"><Input value={activeSkill.trigger} onChange={(e) => patchSkill({ trigger: e.target.value })} disabled={!canEditSkills} /></Form.Item><Form.Item label="策略要求"><Input value={activeSkill.policy} onChange={(e) => patchSkill({ policy: e.target.value })} disabled={!canEditSkills} /></Form.Item><Form.Item label="Skill 内容"><Input.TextArea rows={10} value={activeSkill.prompt} onChange={(e) => patchSkill({ prompt: e.target.value })} disabled={!canEditSkills} /></Form.Item></Form></div> : <Empty description="没有可编辑的 skill" />}</Card></Col>
                <Col xs={24}><Card title="Runtime Skill" className="glass-panel rounded-lg" extra={<Space><Tag color={runtimeSkillSource === 'proxy' || runtimeSkillSource === 'serial' ? 'green' : 'gold'}>{runtimeSkillSource === 'proxy' ? 'runtime linked' : runtimeSkillSource === 'serial' ? 'serial linked' : 'local mock'}</Tag><Button onClick={refreshRuntimeSkills}>刷新</Button></Space>}><div className="mb-4 grid gap-4 md:grid-cols-2"><div className="rounded-lg border border-slate-200 bg-slate-50 p-4 text-sm text-slate-600">source: {runtimeSkillSource}</div><div className="rounded-lg border border-slate-200 bg-slate-50 p-4 text-sm text-slate-600">{activeSkill ? `选中：${activeSkill.name}` : '未选中草稿'}</div></div><div className="panel-scroll max-h-[360px] overflow-y-auto pr-1"><List locale={{ emptyText: '当前还没有安装过 runtime skill' }} dataSource={runtimeSkills} renderItem={(item) => <List.Item key={item.runtimeName}><div className="w-full"><div className="flex flex-wrap items-center justify-between gap-3"><div><Space size={8}><Text strong>{item.title}</Text><Tag color={item.status === 'installed' ? 'green' : item.status === 'pending' ? 'gold' : 'red'}>{item.status}</Tag><Tag>{item.source}</Tag></Space><div className="mt-1 text-xs text-slate-500">{item.path}</div></div><div className="text-right text-xs text-slate-500"><div>{item.installedAt}</div><div>cache: {item.cacheState}</div></div></div><div className="mt-2 grid gap-2 md:grid-cols-3 text-sm text-slate-600"><div>scope: {item.scope}</div><div>enabled: {item.enabled ? 'true' : 'false'}</div><div>runtimeName: {item.runtimeName}</div></div>{item.lastMessage ? <div className="mt-2 text-sm text-slate-600">{item.lastMessage}</div> : null}</div></List.Item>} /></div></Card></Col>
              </Row>}

              {viewMode === '用户偏好' && <Row gutter={[16, 16]} className="mt-4">
                <Col xs={24} xl={14}><Card title="偏好配置" className="glass-panel rounded-lg" extra={<Button type="primary" loading={savingPrefs} onClick={handleSavePreferences}>保存偏好</Button>}><div className="panel-scroll max-h-[560px] overflow-y-auto pr-1"><Form layout="vertical"><Form.Item label="首选通道"><Select value={preferences.preferredChannel} onChange={(value) => setPreferences((current) => ({ ...current, preferredChannel: value }))} options={[{ value: 'Web Console' }, { value: 'ESP32-P4' }, { value: 'Android' }, { value: 'Feishu' }]} /></Form.Item><Form.Item label="语音输出"><Switch checked={preferences.voiceOutput} onChange={(checked) => setPreferences((current) => ({ ...current, voiceOutput: checked }))} /></Form.Item><Form.Item label="隐私模式"><Select value={preferences.privacyMode} onChange={(value) => setPreferences((current) => ({ ...current, privacyMode: value }))} options={[{ value: 'metadata_only', label: 'metadata_only' }, { value: 'balanced', label: 'balanced' }, { value: 'full_context', label: 'full_context' }]} /></Form.Item><Form.Item label="自动化激进程度"><Slider value={preferences.automationAggressiveness} onChange={(value) => setPreferences((current) => ({ ...current, automationAggressiveness: value }))} /></Form.Item><Form.Item label="摘要风格"><Select value={preferences.summaryStyle} onChange={(value) => setPreferences((current) => ({ ...current, summaryStyle: value }))} options={[{ value: 'concise' }, { value: 'standard' }, { value: 'detailed' }]} /></Form.Item><Form.Item label="语言"><Input value={preferences.preferredLanguage} onChange={(e) => setPreferences((current) => ({ ...current, preferredLanguage: e.target.value }))} /></Form.Item></Form></div></Card></Col>
                <Col xs={24} xl={10}><Card title="当前偏好" className="glass-panel rounded-lg"><div className="rounded-lg border border-slate-200 p-4 text-sm text-slate-600"><div>当前用户：{session.username}</div><div>Role：{session.role}</div><div>Channel：{preferences.preferredChannel}</div><div>Privacy：{preferences.privacyMode}</div><div>Auto level：{preferences.automationAggressiveness}%</div><div>Summary：{preferences.summaryStyle}</div></div></Card></Col>
              </Row>}
            </Content>
          </Layout>

          <Modal open={chatOpen} onCancel={() => setChatOpen(false)} footer={null} width={1024} title="AI 通信面板">
            <div className="grid gap-4 lg:grid-cols-[280px_minmax(0,1fr)]">
              <div className="space-y-4">
                <Card size="small" title="连接参数" className="glass-panel rounded-lg"><Form layout="vertical"><Form.Item label="当前用户"><Input value={`${session.username} / ${session.role}`} disabled /></Form.Item><Form.Item label="Gateway WebSocket URL"><Input value={wsUrl} onChange={(e) => setWsUrl(e.target.value)} placeholder="ws://127.0.0.1:4173/ws" /></Form.Item><Form.Item label="Chat ID"><Input value={chatId} onChange={(e) => setChatId(e.target.value)} placeholder="web_console_01" /></Form.Item><Space><Button type="primary" onClick={connectWs} disabled={!canUseChat}>连接</Button><Button onClick={() => wsInstance?.close()}>断开</Button></Space></Form></Card>
                <Card size="small" title="连接状态" className="glass-panel rounded-lg">
                  <div className="grid gap-3 text-sm">
                    <div className="flex items-center justify-between rounded border border-slate-200 bg-slate-50 px-3 py-2"><span className="text-slate-500">会话状态</span><Tag color={wsConnected ? 'green' : 'red'}>{wsConnected ? '已连接' : '未连接'}</Tag></div>
                    <div className="flex items-center justify-between rounded border border-slate-200 bg-slate-50 px-3 py-2"><span className="text-slate-500">浏览器 STT</span><Tag color={sttSupported && sttSecureContext ? 'green' : sttSupported ? 'gold' : 'red'}>{sttSupported ? (sttSecureContext ? 'supported' : 'needs secure origin') : 'unsupported'}</Tag></div>
                    <div className="rounded border border-slate-200 bg-slate-50 px-3 py-2"><div className="text-xs text-slate-500">最近网关事件</div><div className="mt-1 break-all text-sm text-slate-800">{payload.chatGateway?.lastError || payload.chatGateway?.lastEventAt || '暂无'}</div></div>
                    <div className="rounded border border-slate-200 bg-slate-50 px-3 py-2"><div className="text-xs text-slate-500">STT 状态</div><div className="mt-1 break-all text-sm text-slate-800">{sttStatusText}{sttInterimText ? ` / ${sttInterimText}` : ''}</div></div>
                    <div className="rounded border border-slate-200 bg-slate-50 px-3 py-2 text-xs text-slate-600">{canUseChat ? '当前账号允许通信' : 'viewer 账号只读'}</div>
                  </div>
                </Card>
              </div>
              <Card size="small" title="会话窗口" className="glass-panel rounded-lg">
                <div className="mb-4 grid gap-4 xl:grid-cols-[minmax(0,1fr)_280px]">
                  <div>
                    <div ref={chatScrollRef} className="h-[480px] overflow-y-auto rounded border border-slate-200 bg-slate-50 p-4">
                      <div className="space-y-3">
                        {chatMessages.map((item) => (
                          <div key={item.id} className={`max-w-[88%] rounded-lg px-3 py-2 text-sm ${item.role === 'user' ? 'ml-auto bg-blue-600 text-white' : item.role === 'assistant' ? 'bg-white text-slate-800 shadow-sm' : item.tone === 'error' ? 'bg-rose-50 text-rose-900' : item.tone === 'success' ? 'bg-emerald-50 text-emerald-900' : 'bg-amber-50 text-amber-900'}`}>
                            <div className="mb-1 flex items-center gap-2 text-[11px] opacity-80">
                              {item.role === 'user' ? <SendOutlined /> : item.role === 'assistant' ? <MessageOutlined /> : <ArrowDownOutlined />}
                              <span>{item.role === 'user' ? '用户' : item.role === 'assistant' ? 'Agent' : '系统'}</span>
                            </div>
                            <div>{item.text}</div>
                            <div className={`mt-1 text-[11px] ${item.role === 'user' ? 'text-blue-100' : 'text-slate-400'}`}>{item.time}</div>
                          </div>
                        ))}
                      </div>
                    </div>
                    <Space.Compact className="mt-4 w-full">
                      <Input value={chatInput} onChange={(e) => setChatInput(e.target.value)} onPressEnter={sendChatMessage} placeholder="输入一条要发给 ESP32 Agent 的消息" disabled={!canUseChat} />
                      <Button onClick={() => (sttListening ? stopSpeechRecognition() : startSpeechRecognition(null))} disabled={!canUseChat || !sttSupported}>
                        {sttListening ? '停止语音' : '语音输入'}
                      </Button>
                      <Button type="primary" onClick={sendChatMessage} disabled={!canUseChat}>发送</Button>
                    </Space.Compact>
                  </div>
                  <div className="space-y-4">
                    <div className="rounded-lg border border-slate-200 bg-slate-50 p-4">
                      <div className="mb-3 text-sm font-semibold text-slate-900">会话摘要</div>
                      <div className="space-y-2 text-sm text-slate-600">
                        <div>Chat ID：{chatId}</div>
                        <div>消息数：{chatMessages.length}</div>
                        <div>本地连接：{wsConnected ? 'open' : 'closed'}</div>
                        <div>上游网关：{payload.chatGateway?.enabled ? 'enabled' : 'disabled'}</div>
                      </div>
                    </div>
                    <div>
                      <div className="mb-3 text-sm font-semibold text-slate-900">相关 timeline</div>
                      <Timeline items={chatRelatedTimeline.map((event) => ({ color: statusColor(event.status), children: <div><div className="text-sm font-medium text-slate-800">{event.stage}</div><div className="text-xs text-slate-500">{event.source} → {event.target} · {event.time}</div><div className="mt-1 text-sm text-slate-600">{event.payload}</div></div> }))} />
                    </div>
                  </div>
                </div>
              </Card>
            </div>
          </Modal>
        </Layout>
      </AntdApp>
    </ConfigProvider>
  );
}

export default App;
