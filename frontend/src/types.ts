export type AgentNodeRole =
  | 'coordinator_agent'
  | 'sensor_agent'
  | 'control_agent'
  | 'guardian_agent'
  | 'display_terminal';

export interface AgentNode {
  id: string;
  role: AgentNodeRole;
  transport: string[];
  status: 'online' | 'degraded' | 'offline';
  location: string;
  responsibilities: string[];
  surfaces: string[];
}

export interface Capability {
  name: string;
  category: '感知' | '控制' | '协同' | '安全' | '扩展';
  role: AgentNodeRole;
  maturity: '已验证' | '进行中' | '规划中';
  summary: string;
}

export interface TimelineEvent {
  time: string;
  stage: string;
  source: string;
  target: string;
  payload: string;
  status: 'ok' | 'queued' | 'warn';
}

export interface EnvironmentMetric {
  label: string;
  value: number;
  unit: string;
  status: 'good' | 'attention' | 'critical';
  trend: number;
}

export interface SkillDraft {
  id: string;
  name: string;
  scope: string;
  trigger: string;
  policy: string;
  prompt: string;
  enabled: boolean;
}

export interface RuntimeSkillRecord {
  id: string;
  runtimeName: string;
  title: string;
  path: string;
  source: 'local_mock' | 'proxy' | 'serial';
  installedAt: string;
  cacheState: 'invalidated' | 'unknown';
  status: 'installed' | 'pending' | 'failed';
  scope: string;
  enabled: boolean;
  lastMessage?: string;
}

export interface RuntimeSkillListResponse {
  skills: RuntimeSkillRecord[];
  source: 'local_mock' | 'proxy' | 'serial';
}

export interface RuntimeSkillInstallResponse {
  ok: boolean;
  skill?: RuntimeSkillRecord;
  source: 'local_mock' | 'proxy' | 'serial';
  message: string;
  error?: string;
}

export interface UserPreferenceProfile {
  preferredChannel: string;
  privacyMode: 'metadata_only' | 'balanced' | 'full_context';
  automationAggressiveness: number;
  summaryStyle: 'concise' | 'standard' | 'detailed';
  preferredLanguage: string;
}

export interface MeshMessageFlow {
  id: string;
  step: string;
  transport: string;
  producer: string;
  consumer: string;
  topic: string;
  detail: string;
}

export interface DashboardPayload {
  nodes: AgentNode[];
  capabilities: Capability[];
  timeline: TimelineEvent[];
  environment: EnvironmentMetric[];
  skills: SkillDraft[];
  preferences: UserPreferenceProfile;
  flows: MeshMessageFlow[];
  mqtt?: {
    connected: boolean;
    url: string;
    topicPrefix: string;
    lastEventAt: string | null;
  };
  chatGateway?: {
    enabled: boolean;
    path: string;
    upstreamUrl: string | null;
    activeSessions: number;
    connectedSessions: number;
    lastEventAt: string | null;
    lastError: string | null;
  };
  guardian?: {
    updatedAt: string;
    payload: Record<string, unknown>;
  } | null;
}
