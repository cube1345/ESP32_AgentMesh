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
  eventId?: string;
  nodeId?: string;
  role?: AgentNodeRole | string;
  phase?: string;
  traceId?: string;
  taskId?: string;
  parentTaskId?: string;
  commandId?: string;
  action?: string;
  targetRole?: string;
  targetNode?: string;
  tsMs?: number;
}

export interface EnvironmentMetric {
  label: string;
  value: number;
  unit: string;
  status: 'good' | 'attention' | 'critical';
  trend: number;
}

export interface EnvironmentHistoryPoint {
  timestamp: string;
  nodeId: string;
  temperatureC: number | null;
  humidityPercent: number | null;
  eco2Ppm: number | null;
  tvocPpb: number | null;
  lightLux: number | null;
  presence: number | null;
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

export type RuntimeAssetSource = 'local_mock' | 'proxy' | 'serial' | 'mqtt';

export interface RuntimeSkillRecord {
  id: string;
  runtimeName: string;
  title: string;
  path: string;
  source: RuntimeAssetSource;
  installedAt: string;
  cacheState: 'invalidated' | 'unknown';
  status: 'installed' | 'pending' | 'failed';
  scope: string;
  enabled: boolean;
  content?: string;
  contentHash?: string;
  lastMessage?: string;
}

export interface RuntimeSkillListResponse {
  skills: RuntimeSkillRecord[];
  source: RuntimeAssetSource;
  error?: string;
}

export interface RuntimeSkillInstallResponse {
  ok: boolean;
  skill?: RuntimeSkillRecord;
  source: RuntimeAssetSource;
  message: string;
  error?: string;
  skipped?: boolean;
}

export interface RuntimeSkillDetailResponse {
  skill: RuntimeSkillRecord;
  source: RuntimeAssetSource;
}

export interface RuntimeSkillReadResult {
  skill: RuntimeSkillRecord | null;
  source: RuntimeAssetSource;
  error?: string;
}

export interface RuntimeDeviceManifestRecord {
  id: string;
  manifestName: string;
  title: string;
  protocol: string;
  role: string;
  risk: string;
  path: string;
  signaturePath: string;
  source: 'local_mock' | 'proxy' | 'serial';
  installedAt: string;
  status: 'installed' | 'pending' | 'failed';
  content?: string;
  contentHash?: string;
  lastMessage?: string;
}

export interface RuntimeDeviceManifestListResponse {
  devices: RuntimeDeviceManifestRecord[];
  source: 'local_mock' | 'proxy' | 'serial';
  error?: string;
}

export interface RuntimeDeviceManifestUpdateResponse {
  ok: boolean;
  device?: RuntimeDeviceManifestRecord;
  source: 'local_mock' | 'proxy' | 'serial';
  message: string;
  error?: string;
  skipped?: boolean;
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
  environmentHistory: EnvironmentHistoryPoint[];
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
  hostAgent?: {
    enabled: boolean;
    role: 'coordinator_agent';
    maxActions: number;
    maxRounds: number;
    automation?: {
      enabled: boolean;
      running: boolean;
      intervalMs: number;
      pending: Array<Record<string, unknown>>;
      lastRunAt: string | null;
      lastError: string | null;
    };
  };
  guardian?: {
    updatedAt: string;
    payload: Record<string, unknown>;
  } | null;
}
